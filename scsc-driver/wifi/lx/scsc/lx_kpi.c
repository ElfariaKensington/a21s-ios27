/*
 * LinuxKPI additions and SCSC WLBT shims on top of wifi/lx/src/lx_base.c:
 *   - Delays, timers, string helpers, sscanf, scnprintf, IEEE 802.3 ether_crc()
 *   - SCSC firmware path normalization -> flat basenames via lx_os_firmware()
 *   - Netdev allocation, channel/frequency helpers, regulatory sync, procfs/debugfs/kobject stubs
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/linker.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/sx.h>
#include <sys/malloc.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/kthread.h>
#include <sys/time.h>

#include <linux/kernel.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/kobject.h>
#include <linux/kthread.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>

#include <lx_os.h>
#include "lx.h"
#include "lx_scsc.h"
#include <linux/soc/samsung/exynos-soc.h>

int linuxkpi_debug;
int linuxkpi_debug_80211;
linker_file_t __this_linker_file;
struct workqueue_struct *system_wq;
struct workqueue_struct *system_power_efficient_wq;
struct kobject *wifi_kobj_ref;
struct exynos_chipid_info exynos_soc_info;

/* ---- time & scheduling -------------------------------------------------------------------------- */

unsigned long long
lx_local_clock(void)
{
	return (lx_os_nanoseconds());
}

void
DELAY(int usec)
{
	unsigned long long until;

	if (usec <= 0)
		return;
	until = lx_os_nanoseconds() + (unsigned long long)usec * 1000ULL;
	while (lx_os_nanoseconds() < until) {
		__asm__ volatile("yield");
	}
}

int
pause_sbt(const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags)
{
	static char nothing;
	unsigned long long ns = ((unsigned long long)sbt * 1000000000ULL) >> 32;

	(void)wmesg;
	(void)pr;
	(void)flags;
	lx_os_lock();
	lx_os_sleep(&nothing, ns == 0 ? 1ULL : ns);
	lx_os_unlock();
	return (0);
}

void
add_timer(struct timer_list *timer)
{
	mod_timer(timer, timer->expires);
}

bool
linux_waitqueue_active(wait_queue_head_t *wqh)
{
	(void)wqh;
	return (true);
}

void
sx_sysinit(const void *arg)
{
	const struct sx_args *args = arg;

	sx_init_flags(args->sa_sx, args->sa_desc, args->sa_flags);
}

void
sx_destroy(struct sx *sx)
{
	(void)sx;
}

int
timer_shutdown_sync(struct timer_list *timer)
{
	return (timer_delete_sync(timer));
}

void
nanouptime(struct timespec *ts)
{
	unsigned long long ns = lx_os_nanoseconds();

	ts->tv_sec = (time_t)(ns / 1000000000ULL);
	ts->tv_nsec = (long)(ns % 1000000000ULL);
}

void
sched_relinquish(struct thread *td)
{
	static char nothing;

	(void)td;
	lx_os_lock();
	lx_os_sleep(&nothing, 1000ULL);
	lx_os_unlock();
}

void
panic(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	lx_os_panic(line);
}

/* ---- strings, formatting, sscanf, CRC32 --------------------------------------------------------- */

size_t
strnlen(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n] != 0)
		n++;
	return (n);
}

char *
strchrnul(const char *s, int c)
{
	while (*s != 0 && *s != (char)c)
		s++;
	return (__DECONST(char *, s));
}

char *
strnstr(const char *s, const char *find, size_t max)
{
	size_t length = strlen(find);

	if (length == 0)
		return (__DECONST(char *, s));
	for (; max >= length && *s != 0; s++, max--) {
		if (strncmp(s, find, length) == 0)
			return (__DECONST(char *, s));
	}
	return (NULL);
}

char *
strstr(const char *s, const char *find)
{
	return (strnstr(s, find, strlen(s)));
}

char *
strsep(char **string, const char *delimiters)
{
	char *start = *string, *s;

	if (start == NULL)
		return (NULL);
	for (s = start; *s != 0; s++) {
		if (strchr(delimiters, *s) != NULL) {
			*s = 0;
			*string = s + 1;
			return (start);
		}
	}
	*string = NULL;
	return (start);
}

u_long
strtoul(const char *s, char **end, int base)
{
	u_long value = 0;

	while (*s == ' ' || *s == '\t')
		s++;
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		base = 16;
	} else if (base == 0) {
		base = s[0] == '0' ? 8 : 10;
	}
	for (;; s++) {
		int digit = *s >= '0' && *s <= '9' ? *s - '0' :
		    *s >= 'a' && *s <= 'z' ? *s - 'a' + 10 :
		    *s >= 'A' && *s <= 'Z' ? *s - 'A' + 10 : 99;

		if (digit >= base)
			break;
		value = value * (u_long)base + (u_long)digit;
	}
	if (end != NULL)
		*end = __DECONST(char *, s);
	return (value);
}

long
strtol(const char *s, char **end, int base)
{
	int neg = 0;

	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == '-') {
		neg = 1;
		s++;
	} else if (*s == '+') {
		s++;
	}
	u_long u = strtoul(s, end, base);
	return (neg ? -(long)u : (long)u);
}

int
lx_scnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (size == 0)
		return (0);
	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	if (n < 0)
		return (0);
	return ((size_t)n >= size ? (int)(size - 1) : n);
}

static int
hex_val(char c)
{
	if (c >= '0' && c <= '9')
		return (c - '0');
	if (c >= 'a' && c <= 'f')
		return (c - 'a' + 10);
	if (c >= 'A' && c <= 'F')
		return (c - 'A' + 10);
	return (-1);
}

/* Minimal self-contained sscanf supporting SCSC's MAC ("%02X:%02X:...") and "%17s"/"%d"/"%u"/"%x" */
int
lx_sscanf(const char *str, const char *fmt, ...)
{
	va_list ap;
	int matched = 0;

	if (str == NULL || fmt == NULL)
		return (0);

	va_start(ap, fmt);
	while (*fmt != '\0') {
		if (*fmt == ' ' || *fmt == '\t') {
			while (*fmt == ' ' || *fmt == '\t')
				fmt++;
			while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')
				str++;
			continue;
		}
		if (*fmt != '%') {
			if (*str != *fmt)
				break;
			str++;
			fmt++;
			continue;
		}
		fmt++; /* skip '%' */
		if (*fmt == '%') {
			if (*str != '%')
				break;
			str++;
			fmt++;
			continue;
		}

		/* Optional width (e.g. 02, 17) */
		unsigned width = 0;
		while (*fmt >= '0' && *fmt <= '9') {
			width = width * 10U + (unsigned)(*fmt - '0');
			fmt++;
		}
		/* Optional length modifier */
		int is_half = 0, is_byte = 0;
		if (*fmt == 'h') {
			fmt++;
			if (*fmt == 'h') {
				is_byte = 1;
				fmt++;
			} else {
				is_half = 1;
			}
		} else if (*fmt == 'l') {
			fmt++;
		}

		char conv = *fmt++;
		if (conv == 's') {
			char *out = va_arg(ap, char *);
			unsigned copied = 0;

			while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')
				str++;
			if (*str == '\0')
				break;
			while (*str != '\0' && *str != ' ' && *str != '\t' && *str != '\n' && *str != '\r' &&
			    (width == 0 || copied < width)) {
				out[copied++] = *str++;
			}
			out[copied] = '\0';
			matched++;
		} else if (conv == 'x' || conv == 'X' || conv == 'u' || conv == 'd') {
			int base = (conv == 'x' || conv == 'X') ? 16 : 10;
			int neg = 0;
			unsigned long val = 0;
			unsigned digits = 0;

			while (*str == ' ' || *str == '\t')
				str++;
			if (conv == 'd' && *str == '-') {
				neg = 1;
				str++;
			}
			if (base == 16 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X') && (width == 0 || width > 2)) {
				str += 2;
			}
			while (*str != '\0' && (width == 0 || digits < width)) {
				int d = hex_val(*str);
				if (d < 0 || d >= base)
					break;
				val = val * (unsigned long)base + (unsigned long)d;
				str++;
				digits++;
			}
			if (digits == 0)
				break;
			if (neg)
				val = (unsigned long)(-(long)val);
			if (is_byte)
				*va_arg(ap, uint8_t *) = (uint8_t)val;
			else if (is_half)
				*va_arg(ap, uint16_t *) = (uint16_t)val;
			else
				*va_arg(ap, uint32_t *) = (uint32_t)val;
			matched++;
		} else {
			break;
		}
	}
	va_end(ap);
	return (matched);
}

static uint32_t
bitrev32_val(uint32_t x)
{
	x = ((x >> 1) & 0x55555555U) | ((x & 0x55555555U) << 1);
	x = ((x >> 2) & 0x33333333U) | ((x & 0x33333333U) << 2);
	x = ((x >> 4) & 0x0f0f0f0fU) | ((x & 0x0f0f0f0fU) << 4);
	x = ((x >> 8) & 0x00ff00ffU) | ((x & 0x00ff00ffU) << 8);
	return ((x >> 16) | (x << 16));
}

/* Linux ether_crc(length, data): bitrev32(crc32_le(~0, data, length)) */
uint32_t
lx_ether_crc(int length, const unsigned char *data)
{
	uint32_t crc = 0xffffffffU;

	for (int i = 0; i < length; i++) {
		crc ^= (uint32_t)data[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
		}
	}
	return (bitrev32_val(crc));
}

static char *
vkasprintf(const char *format, va_list list)
{
	char *text;
	va_list again;
	int length;

	va_copy(again, list);
	length = vsnprintf(NULL, 0, format, again);
	va_end(again);
	text = kmalloc((size_t)length + 1, GFP_KERNEL);
	if (text != NULL)
		vsnprintf(text, (size_t)length + 1, format, list);
	return (text);
}

char *
kasprintf(gfp_t gfp, const char *format, ...)
{
	va_list list;
	char *text;

	(void)gfp;
	va_start(list, format);
	text = vkasprintf(format, list);
	va_end(list);
	return (text);
}

char *
lkpi_devm_kasprintf(struct device *dev, gfp_t gfp, const char *format, ...)
{
	va_list list;
	char *text;

	(void)dev;
	(void)gfp;
	va_start(list, format);
	text = vkasprintf(format, list);
	va_end(list);
	return (text);
}

/* ---- logging & hex dump ------------------------------------------------------------------------- */

int
device_printf(device_t dev, const char *format, ...)
{
	char line[400];
	va_list list;

	(void)dev;
	strlcpy(line, "scsc: ", sizeof(line));
	va_start(list, format);
	lx_vformat(line + 6, sizeof(line) - 6, format, list);
	va_end(list);
	lx_os_print(line);
	return (0);
}

int
__lkpi_hexdump_printf(void *argument, const char *format, ...)
{
	char line[400];
	va_list list;

	(void)argument;
	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("%s", line);
	return (0);
}

void
lkpi_hex_dump(int (*print)(void *, const char *, ...), void *argument, const char *level, const char *prefix,
    const int prefix_type, const int rowsize, const int groupsize, const void *buffer, size_t length,
    const bool ascii, const bool trailing)
{
	const uint8_t *b = buffer;

	(void)level;
	(void)prefix_type;
	(void)rowsize;
	(void)groupsize;
	(void)ascii;
	(void)trailing;
	for (size_t at = 0; at < length; at += 16) {
		char line[16 * 3 + 1];
		size_t n = length - at < 16 ? length - at : 16;

		for (size_t i = 0; i < n; i++) {
			snprintf(line + 3 * i, 4, " %02x", b[at + i]);
		}
		line[3 * n] = 0;
		print(argument, "%s%04zx:%s\n", prefix != NULL ? prefix : "", at, line);
	}
}

/* ---- SCSC firmware path -> flat basename mapping ------------------------------------------------ */

int
lx_scsc_request_firmware(const struct linuxkpi_firmware **out, const char *name, struct device *dev)
{
	const char *base;
	char mapped[64];
	int err;

	if (out != NULL)
		*out = NULL;
	if (name == NULL || out == NULL)
		return (-EINVAL);
	if (!lx_scsc_reached(2))
		return (-ENOENT);

	base = strrchr(name, '/');
	base = base != NULL ? base + 1 : name;
	if (base[0] == '.')
		base++; /* ".mac.info" -> "mac.info" */

	/* Map hierarchical conf names directly to flat vendor-wifi names if ever asked */
	if (strcmp(base, "wlan.hcf") == 0)
		strlcpy(mapped, "mx140_wlan.hcf", sizeof(mapped));
	else if (strcmp(base, "wlan_sw.hcf") == 0)
		strlcpy(mapped, "mx140_wlan_sw.hcf", sizeof(mapped));
	else if (strcmp(base, "common.hcf") == 0)
		strlcpy(mapped, "mx140_common.hcf", sizeof(mapped));
	else if (strcmp(base, "common_sw.hcf") == 0)
		strlcpy(mapped, "mx140_common_sw.hcf", sizeof(mapped));
	else
		strlcpy(mapped, base, sizeof(mapped));

	err = linuxkpi_firmware_request_nowarn(out, mapped, dev);
	if (err == 0 && *out != NULL) {
		lx_tell("scsc: firmware %s -> %s (%zu bytes)\n", name, mapped, (*out)->size);
	}
	return (err);
}

int
linuxkpi_request_firmware(const struct linuxkpi_firmware **out, const char *name, struct device *dev)
{
	return (lx_scsc_request_firmware(out, name, dev));
}

/* ---- net_device allocation & FreeBSD malloc shim ------------------------------------------------ */

struct net_device *
linuxkpi_alloc_netdev(size_t priv_length, const char *name, uint32_t flags, void (*setup)(struct net_device *))
{
	struct net_device *ndev = kzalloc(sizeof(*ndev) + priv_length, GFP_KERNEL);

	(void)flags;
	if (ndev == NULL)
		return (NULL);
	strlcpy(ndev->name, name, sizeof(ndev->name));
	if (setup != NULL)
		setup(ndev);
	return (ndev);
}

void
linuxkpi_free_netdev(struct net_device *ndev)
{
	kfree(ndev);
}

int cold;
struct malloc_type M_KMALLOC[1];

#undef malloc
void *
malloc(size_t size, struct malloc_type *type, int flags)
{
	(void)type;
	return (lkpi_kmalloc(size, (flags & M_ZERO) ? GFP_KERNEL | __GFP_ZERO : GFP_KERNEL));
}

int
linux_copyin(const void *uaddr, void *kaddr, size_t length)
{
	(void)uaddr;
	(void)kaddr;
	(void)length;
	return (EFAULT);
}

int
linux_copyout(const void *kaddr, void *uaddr, size_t length)
{
	(void)kaddr;
	(void)uaddr;
	(void)length;
	return (EFAULT);
}

/* ---- debugfs / seq_file / notifier no-ops ------------------------------------------------------- */

struct dentry *
debugfs_create_file(const char *name, umode_t mode, struct dentry *parent, void *data, const struct file_operations *fops)
{
	(void)name;
	(void)mode;
	(void)parent;
	(void)data;
	(void)fops;
	return (NULL);
}

void
seq_puts(struct seq_file *m, const char *text)
{
	(void)m;
	(void)text;
}

void
lkpi_seq_printf(struct seq_file *m, const char *format, ...)
{
	(void)m;
	(void)format;
}

int
blocking_notifier_chain_register(struct blocking_notifier_head *nh, struct notifier_block *n)
{
	(void)nh;
	(void)n;
	return (0);
}

int
blocking_notifier_chain_unregister(struct blocking_notifier_head *nh, struct notifier_block *n)
{
	(void)nh;
	(void)n;
	return (0);
}

int
blocking_notifier_call_chain(struct blocking_notifier_head *nh, unsigned long val, void *v)
{
	(void)nh;
	(void)val;
	(void)v;
	return (NOTIFY_DONE);
}

int
atomic_notifier_chain_register(struct atomic_notifier_head *nh, struct notifier_block *n)
{
	(void)nh;
	(void)n;
	return (0);
}

int
atomic_notifier_chain_unregister(struct atomic_notifier_head *nh, struct notifier_block *n)
{
	(void)nh;
	(void)n;
	return (0);
}

int
atomic_notifier_call_chain(struct atomic_notifier_head *nh, unsigned long val, void *v)
{
	(void)nh;
	(void)val;
	(void)v;
	return (NOTIFY_DONE);
}

int
register_inetaddr_notifier(struct notifier_block *nb)
{
	(void)nb;
	return (0);
}

int
unregister_inetaddr_notifier(struct notifier_block *nb)
{
	(void)nb;
	return (0);
}

/* ---- cfg80211 channel & regulatory helpers ------------------------------------------------------ */

uint32_t
linuxkpi_ieee80211_channel_to_frequency(uint32_t channel, enum nl80211_band band)
{
	switch (band) {
	case NL80211_BAND_2GHZ:
		return (channel == 14 ? 2484 : channel >= 1 && channel <= 13 ? 2407 + 5 * channel : 0);
	case NL80211_BAND_5GHZ:
		return (channel >= 182 && channel <= 196 ? 4000 + 5 * channel : 5000 + 5 * channel);
	case NL80211_BAND_6GHZ:
		return (channel == 2 ? 5935 : 5950 + 5 * channel);
	default:
		return (0);
	}
}

uint32_t
linuxkpi_ieee80211_frequency_to_channel(uint32_t frequency, uint32_t flags)
{
	(void)flags;
	if (frequency == 2484)
		return (14);
	if (frequency >= 2412 && frequency < 2484)
		return ((frequency - 2407) / 5);
	if (frequency >= 4910 && frequency <= 4980)
		return ((frequency - 4000) / 5);
	if (frequency == 5935)
		return (2);
	if (frequency > 5950 && frequency <= 7115)
		return ((frequency - 5950) / 5);
	if (frequency >= 5000 && frequency <= 5925)
		return ((frequency - 5000) / 5);
	return (0);
}

struct linuxkpi_ieee80211_channel *
linuxkpi_ieee80211_get_channel(struct wiphy *wiphy, uint32_t frequency)
{
	if (wiphy == NULL)
		return (NULL);
	for (int b = 0; b < NUM_NL80211_BANDS; b++) {
		struct ieee80211_supported_band *band = wiphy->bands[b];

		for (int c = 0; band != NULL && c < band->n_channels; c++) {
			if (band->channels[c].center_freq == frequency)
				return (&band->channels[c]);
		}
	}
	return (NULL);
}

int
linuxkpi_regulatory_set_wiphy_regd_sync(struct wiphy *wiphy, struct linuxkpi_ieee80211_regdomain *regd)
{
	if (wiphy == NULL || regd == NULL)
		return (-EINVAL);
	wiphy->regd = regd;
	return (0);
}

void
linuxkpi_wiphy_apply_custom_regulatory(struct wiphy *wiphy, const struct linuxkpi_ieee80211_regdomain *regd)
{
	if (wiphy == NULL || regd == NULL)
		return;
	for (int b = 0; b < NUM_NL80211_BANDS; b++) {
		struct ieee80211_supported_band *band = wiphy->bands[b];

		for (int ch = 0; band != NULL && ch < band->n_channels; ch++) {
			struct linuxkpi_ieee80211_channel *c = &band->channels[ch];
			uint32_t khz = c->center_freq * 1000U;
			bool matched = false;

			for (uint32_t r = 0; r < regd->n_reg_rules; r++) {
				const struct ieee80211_reg_rule *rule = &regd->reg_rules[r];
				if (khz >= (uint32_t)rule->freq_range.start_freq_khz &&
				    khz <= (uint32_t)rule->freq_range.end_freq_khz) {
					matched = true;
					if (rule->flags & NL80211_RRF_NO_IR)
						c->flags |= IEEE80211_CHAN_NO_IR;
					if (rule->flags & NL80211_RRF_DFS)
						c->flags |= IEEE80211_CHAN_RADAR;
					break;
				}
			}
			if (!matched && regd->n_reg_rules > 0)
				c->flags |= IEEE80211_CHAN_DISABLED;
		}
	}
}

/* ---- locks, kthreads, workqueue flush, skb & FreeBSD runtime shims ------------------------------- */

void
getnanouptime(struct timespec *ts)
{
	nanouptime(ts);
}

#undef timeval
int
eventratecheck(struct timeval *lasttime, int *curpps, int maxpps)
{
	(void)lasttime;
	(void)curpps;
	(void)maxpps;
	return (1);
}

int
linuxkpi_net_ratelimit(void)
{
	return (1);
}

int
sx_try_xlock_(struct sx *sx, const char *file, int line)
{
	uintptr_t self = (uintptr_t)lx_os_thread_self();
	int got = 0;

	(void)file;
	(void)line;
	lx_os_lock();
	if (sx->sx_lock == 0) {
		sx->sx_lock = self;
		got = 1;
	}
	lx_os_unlock();
	return (got);
}

void
mtx_sysinit(const void *arg)
{
	const struct mtx_args *ma = arg;

	if (ma != NULL && ma->ma_mtx != NULL)
		_mtx_init(&((struct mtx *)ma->ma_mtx)->mtx_lock, ma->ma_desc, NULL, ma->ma_opts);
}

void
_rw_init_flags(volatile uintptr_t *c, const char *name, int opts)
{
	(void)name;
	(void)opts;
	*c = 0;
}

void
__rw_rlock(volatile uintptr_t *c, const char *file, int line)
{
	(void)file;
	(void)line;
	lx_os_lock();
	while (*c != 0 && (*c & 1U) == 0) {
		lx_os_sleep((const void *)c, 5000000ULL);
	}
	*c = (((*c >> 1) + 1U) << 1) | 1U;
	lx_os_unlock();
}

void
_rw_runlock_cookie(volatile uintptr_t *c, const char *file, int line)
{
	uintptr_t readers;

	(void)file;
	(void)line;
	lx_os_lock();
	readers = (*c >> 1);
	if (readers > 1U) {
		*c = ((readers - 1U) << 1) | 1U;
	} else {
		*c = 0;
		lx_os_wakeup((const void *)c);
	}
	lx_os_unlock();
}

void
_rw_wlock_cookie(volatile uintptr_t *c, const char *file, int line)
{
	uintptr_t self = (uintptr_t)lx_os_thread_self();

	(void)file;
	(void)line;
	lx_os_lock();
	while (*c != 0) {
		lx_os_sleep((const void *)c, 5000000ULL);
	}
	*c = self;
	lx_os_unlock();
}

void
_rw_wunlock_cookie(volatile uintptr_t *c, const char *file, int line)
{
	(void)file;
	(void)line;
	lx_os_lock();
	*c = 0;
	lx_os_wakeup((const void *)c);
	lx_os_unlock();
}

bool
linux_rcu_read_lock_held(unsigned type)
{
	(void)type;
	return (true);
}

/* Kernel threads spawned via kthread_run() (mxmgmt_thread, mxlog_thread) */
struct lx_kthread_slot {
	bool used;
	volatile bool should_stop;
	volatile bool exited;
	int exit_code;
	void *os_thread;
	linux_task_fn_t *fn;
	void *arg;
	char name[32];
	struct thread td;
	struct task_struct task;
};

static struct lx_kthread_slot kthreads[8];

static void
lx_kthread_entry(void *ctx)
{
	struct lx_kthread_slot *kt = ctx;

	kt->os_thread = lx_os_thread_self();
	kt->exit_code = kt->fn(kt->arg);
	lx_os_lock();
	kt->exited = true;
	lx_os_wakeup(&kt->exited);
	lx_os_unlock();
}

void
linux_kthread_fn(void *unused)
{
	(void)unused;
}

int
kthread_add(void (*func)(void *), void *arg, struct proc *p, struct thread **newtdp,
    int flags, int pages, const char *fmt, ...)
{
	struct lx_kthread_slot *kt = NULL;
	va_list ap;

	(void)func;
	(void)arg;
	(void)p;
	(void)flags;
	(void)pages;
	lx_os_lock();
	for (unsigned i = 0; i < nitems(kthreads); i++) {
		if (!kthreads[i].used || kthreads[i].exited) {
			kt = &kthreads[i];
			bzero(kt, sizeof(*kt));
			kt->used = true;
			break;
		}
	}
	lx_os_unlock();
	if (kt == NULL)
		return (ENOMEM);

	va_start(ap, fmt);
	vsnprintf(kt->name, sizeof(kt->name), fmt, ap);
	va_end(ap);
	kt->task.task_thread = &kt->td;
	kt->td.td_lkpi_task = &kt->task;
	if (newtdp != NULL)
		*newtdp = &kt->td;
	return (0);
}

struct task_struct *
linux_kthread_setup_and_run(struct thread *td, linux_task_fn_t *fn, void *arg)
{
	struct lx_kthread_slot *kt = __containerof(td, struct lx_kthread_slot, td);

	kt->fn = fn;
	kt->arg = arg;
	if (lx_os_thread(kt->name, lx_kthread_entry, kt) != 0) {
		kt->used = false;
		return (ERR_PTR(-ENOMEM));
	}
	return (&kt->task);
}

bool
linux_kthread_should_stop(void)
{
	void *self = lx_os_thread_self();

	for (unsigned i = 0; i < nitems(kthreads); i++) {
		if (kthreads[i].used && kthreads[i].os_thread == self)
			return (kthreads[i].should_stop);
	}
	return (false);
}

int
linux_kthread_stop(struct task_struct *task)
{
	struct lx_kthread_slot *kt;

	if (task == NULL || IS_ERR(task))
		return (-EINVAL);
	kt = __containerof(task, struct lx_kthread_slot, task);
	lx_os_lock();
	kt->should_stop = true;
	while (!kt->exited) {
		lx_os_sleep(&kt->exited, 5000000ULL);
	}
	lx_os_unlock();
	return (kt->exit_code);
}

long
linux_schedule_timeout(long timeout)
{
	if (timeout == MAX_SCHEDULE_TIMEOUT) {
		for (unsigned i = 0; i < 200 && !linux_kthread_should_stop(); i++)
			lx_pause_ms(5);
		return (0);
	}
	if (timeout > 0)
		lx_pause_ms((unsigned)timeout);
	return (0);
}

void
linux_free_current(struct task_struct *ts)
{
	(void)ts;
}

struct lx_wq_barrier {
	struct work_struct work;
	struct completion done;
};

static void
lx_wq_barrier_fn(struct work_struct *work)
{
	struct lx_wq_barrier *b = __containerof(work, struct lx_wq_barrier, work);

	complete(&b->done);
}

void
lx_flush_workqueue(struct workqueue_struct *wq)
{
	struct lx_wq_barrier b;

	if (wq == NULL)
		return;
	bzero(&b, sizeof(b));
	INIT_WORK(&b.work, lx_wq_barrier_fn);
	init_completion(&b.done);
	if (linux_queue_work_on(0, wq, &b.work))
		wait_for_completion_timeout(&b.done, msecs_to_jiffies(2000));
}

void
taskqueue_drain_all(struct taskqueue *tq)
{
	(void)tq;
}

bool
linux_work_pending(struct work_struct *work)
{
	return (work != NULL && atomic_read(&work->state) != 0);
}

bool
linux_work_busy(struct work_struct *work)
{
	return (linux_work_pending(work));
}

vm_offset_t
linux_alloc_kmem(gfp_t flags, unsigned int order)
{
	size_t sz = (size_t)PAGE_SIZE << order;
	void *p = lkpi_kmalloc(sz, flags);

	if (p != NULL && (flags & __GFP_ZERO))
		bzero(p, sz);
	return ((vm_offset_t)(uintptr_t)p);
}

void
linux_free_kmem(vm_offset_t addr, unsigned int order)
{
	(void)order;
	if (addr != 0)
		lkpi_kfree((const void *)(uintptr_t)addr);
}

struct sk_buff *
linuxkpi_alloc_skb(size_t size, gfp_t gfp)
{
	struct sk_buff *skb = linuxkpi_dev_alloc_skb(size, gfp);

	if (skb != NULL) {
		skb->data = skb->head;
		skb_reset_tail_pointer(skb);
	}
	return (skb);
}

struct sk_buff *
linuxkpi_skb_copy(const struct sk_buff *skb, gfp_t gfp)
{
	size_t headroom, tailroom;
	struct sk_buff *n;

	if (skb == NULL)
		return (NULL);
	headroom = (size_t)(skb->data - skb->head);
	tailroom = (size_t)(skb->end - skb_tail_pointer(skb));
	n = linuxkpi_alloc_skb(headroom + skb->len + tailroom, gfp);
	if (n == NULL)
		return (NULL);
	skb_reserve(n, headroom);
	memcpy(skb_put(n, skb->len), skb->data, skb->len);
	n->protocol = skb->protocol;
	n->priority = skb->priority;
	n->queue_mapping = skb->queue_mapping;
	n->pkt_type = skb->pkt_type;
	n->dev = skb->dev;
	n->mac_header = skb->mac_header;
	n->l3hdroff = skb->l3hdroff;
	n->l4hdroff = skb->l4hdroff;
	memcpy(n->cb, skb->cb, sizeof(n->cb));
	return (n);
}

struct kobject *
kobject_create(void)
{
	return (kzalloc(sizeof(struct kobject), GFP_KERNEL));
}

int
kobject_add(struct kobject *kobj, struct kobject *parent, const char *fmt, ...)
{
	va_list ap;

	if (kobj == NULL)
		return (-EINVAL);
	kobj->parent = parent;
	va_start(ap, fmt);
	kobj->name = vkasprintf(fmt, ap);
	va_end(ap);
	return (0);
}

void
linux_kobject_release(struct kref *kref)
{
	(void)kref;
}

struct sysctl_oid *
sysctl_add_oid(struct sysctl_ctx_list *clist, struct sysctl_oid_list *parent, int nbr,
    const char *name, int kind, void *arg1, intmax_t arg2, int (*handler)(SYSCTL_HANDLER_ARGS),
    const char *fmt, const char *descr, const char *label)
{
	(void)clist;
	(void)parent;
	(void)nbr;
	(void)name;
	(void)kind;
	(void)arg1;
	(void)arg2;
	(void)handler;
	(void)fmt;
	(void)descr;
	(void)label;
	return (NULL);
}

int
sysctl_handle_string(SYSCTL_HANDLER_ARGS)
{
	(void)oidp;
	(void)arg1;
	(void)arg2;
	(void)req;
	return (0);
}

int
sysctl_remove_name(struct sysctl_oid *parent, const char *name, int del, int recurse)
{
	(void)parent;
	(void)name;
	(void)del;
	(void)recurse;
	return (0);
}

int
sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, 0x7fffffff, fmt, ap);
	va_end(ap);
	return (n);
}

char *
strncpy(char *dst, const char *src, size_t n)
{
	size_t i = 0;

	for (; i < n && src[i] != '\0'; i++)
		dst[i] = src[i];
	for (; i < n; i++)
		dst[i] = '\0';
	return (dst);
}

int
strcasecmp(const char *s1, const char *s2)
{
	for (;; s1++, s2++) {
		int c1 = (unsigned char)*s1;
		int c2 = (unsigned char)*s2;

		if (c1 >= 'A' && c1 <= 'Z')
			c1 += 'a' - 'A';
		if (c2 >= 'A' && c2 <= 'Z')
			c2 += 'a' - 'A';
		if (c1 != c2 || c1 == 0)
			return (c1 - c2);
	}
}
