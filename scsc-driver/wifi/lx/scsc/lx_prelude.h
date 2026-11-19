/*
 * Included via -include before every SCSC WLBT / slsi_wlan source file.
 * Bridges Linux 4.19/5.4 SCSC expectations onto FreeBSD 15.1 LinuxKPI + wifi/lx.
 */
#ifndef _LX_SCSC_PRELUDE_H_
#define _LX_SCSC_PRELUDE_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/module.h>

#include <linux/version.h>
#include <lx_more.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/bitmap.h>
#include <linux/timer.h>
#include <linux/rwlock.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/notifier.h>
#include <linux/moduleparam.h>
#include <linux/firmware.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <net/netlink.h>
#include <linux/nl80211.h>
#include <net/cfg80211.h>

#ifndef dev_printk_emit
#define dev_printk_emit(lvl, dev, fmt, ...)	printf(fmt, ##__VA_ARGS__)
#endif

#ifndef NETDEV_ALIGN
#define NETDEV_ALIGN				CACHE_LINE_SIZE
#endif

struct ieee80211_dscp_range {
	uint8_t low;
	uint8_t high;
};
struct ieee80211_dscp_exception {
	uint8_t dscp;
	uint8_t up;
};
struct cfg80211_qos_map {
	uint8_t num_des;
	struct ieee80211_dscp_exception des[21];
	struct ieee80211_dscp_range up[8];
};

struct net_device_stats {
	unsigned long rx_packets;
	unsigned long tx_packets;
	unsigned long rx_bytes;
	unsigned long tx_bytes;
	unsigned long rx_errors;
	unsigned long tx_errors;
	unsigned long rx_dropped;
	unsigned long tx_dropped;
	unsigned long multicast;
	unsigned long collisions;
	unsigned long rx_length_errors;
	unsigned long rx_over_errors;
	unsigned long rx_crc_errors;
	unsigned long rx_frame_errors;
	unsigned long rx_fifo_errors;
	unsigned long rx_missed_errors;
	unsigned long tx_aborted_errors;
	unsigned long tx_carrier_errors;
	unsigned long tx_fifo_errors;
	unsigned long tx_heartbeat_errors;
	unsigned long tx_window_errors;
	unsigned long rx_compressed;
	unsigned long tx_compressed;
};

static inline const char *
netdev_name(const struct net_device *dev)
{
	return ((dev != NULL && dev->name[0] != '\0') ? dev->name : "(unregistered net_device)");
}

#ifndef from_timer
#define from_timer(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof(*(var)), timer_fieldname)
#endif

#ifndef read_lock_bh
#define read_lock_bh(l)			read_lock(l)
#define read_unlock_bh(l)		read_unlock(l)
#define write_lock_bh(l)		write_lock(l)
#define write_unlock_bh(l)		write_unlock(l)
#endif

#ifndef in_irq
#define in_irq()			(0)
#endif

static inline uint64_t
ether_addr_to_u64(const uint8_t *addr)
{
	uint64_t u = 0;

	for (int i = 0; i < ETH_ALEN; i++)
		u = (u << 8) | addr[i];
	return (u);
}

static inline void
u64_to_ether_addr(uint64_t u, uint8_t *addr)
{
	for (int i = ETH_ALEN - 1; i >= 0; i--) {
		addr[i] = (uint8_t)(u & 0xff);
		u >>= 8;
	}
}

static inline bool
skb_mac_header_was_set(const struct sk_buff *skb)
{
	return (skb != NULL && skb->mac_header != (typeof(skb->mac_header))~0U);
}

static inline void
skb_reset_network_header(struct sk_buff *skb)
{
	if (skb != NULL && skb->head != NULL)
		skb->l3hdroff = (uint16_t)(skb->data - skb->head);
}

static inline struct sk_buff *
skb_copy_expand(const struct sk_buff *skb, int newheadroom, int newtailroom, gfp_t gfp)
{
	struct sk_buff *n;

	if (skb == NULL)
		return (NULL);
	n = alloc_skb((size_t)newheadroom + skb->len + (size_t)newtailroom, gfp);
	if (n == NULL)
		return (NULL);
	skb_reserve(n, newheadroom);
	memcpy(skb_put(n, skb->len), skb->data, skb->len);
	n->protocol = skb->protocol;
	n->priority = skb->priority;
	n->queue_mapping = skb->queue_mapping;
	n->dev = skb->dev;
	memcpy(n->cb, skb->cb, sizeof(n->cb));
	skb_reset_mac_header(n);
	return (n);
}

static inline int
dev_queue_xmit(struct sk_buff *skb)
{
	if (skb != NULL)
		kfree_skb(skb);
	return (0);
}

/* Standard Linux / IEEE 802.11 / nl80211 constants omitted or stubbed in FreeBSD LinuxKPI */
#ifndef ARPHRD_ETHER
#define ARPHRD_ETHER			1
#endif
#ifndef ARPHRD_IEEE80211
#define ARPHRD_IEEE80211		801
#endif
#undef ARPHRD_IEEE80211_RADIOTAP
#define ARPHRD_IEEE80211_RADIOTAP	803
#ifndef ETH_P_802_3
#define ETH_P_802_3			0x0001
#endif
#ifndef NEXTHDR_UDP
#define NEXTHDR_UDP			17
#endif

#undef WLAN_CAPABILITY_ESS
#undef WLAN_CAPABILITY_IBSS
#undef WLAN_CAPABILITY_PRIVACY
#undef WLAN_CAPABILITY_SHORT_PREAMBLE
#undef WLAN_CAPABILITY_QOS
#undef WLAN_CAPABILITY_SHORT_SLOT_TIME
#undef WLAN_CAPABILITY_APSD
#undef WLAN_CAPABILITY_RADIO_MEASURE
#define WLAN_CAPABILITY_ESS		(1 << 0)
#define WLAN_CAPABILITY_IBSS		(1 << 1)
#define WLAN_CAPABILITY_PRIVACY		(1 << 4)
#define WLAN_CAPABILITY_SHORT_PREAMBLE	(1 << 5)
#define WLAN_CAPABILITY_QOS		(1 << 9)
#define WLAN_CAPABILITY_SHORT_SLOT_TIME	(1 << 10)
#define WLAN_CAPABILITY_APSD		(1 << 11)
#define WLAN_CAPABILITY_RADIO_MEASURE	(1 << 12)

#ifndef WLAN_CATEGORY_PUBLIC
#define WLAN_CATEGORY_PUBLIC		4
#endif
#ifndef WLAN_CATEGORY_RADIO_MEASUREMENT
#define WLAN_CATEGORY_RADIO_MEASUREMENT	5
#endif
#ifndef WLAN_CATEGORY_WNM
#define WLAN_CATEGORY_WNM		10
#endif
#ifndef WLAN_CATEGORY_WMM
#define WLAN_CATEGORY_WMM		17
#endif

#define ieee80211_statuscode		ieee80211_status_code
#ifndef WLAN_STATUS_UNSPECIFIED_FAILURE
#define WLAN_STATUS_UNSPECIFIED_FAILURE	1
#endif

#ifndef WLAN_OUI_TYPE_MICROSOFT_WPA
#define WLAN_OUI_TYPE_MICROSOFT_WPA	1
#endif
#ifndef WLAN_OUI_TYPE_MICROSOFT_WMM
#define WLAN_OUI_TYPE_MICROSOFT_WMM	2
#endif
#ifndef WLAN_OUI_TYPE_MICROSOFT_WPS
#define WLAN_OUI_TYPE_MICROSOFT_WPS	4
#endif
#ifndef WLAN_OUI_TYPE_WFA_P2P
#define WLAN_OUI_TYPE_WFA_P2P		9
#endif

#ifndef WLAN_EID_RIC_DATA
#define WLAN_EID_RIC_DATA		57
#endif
#ifndef WLAN_EID_HT_OPERATION
#define WLAN_EID_HT_OPERATION		61
#endif
#ifndef WLAN_EID_RRM_ENABLED_CAPABILITIES
#define WLAN_EID_RRM_ENABLED_CAPABILITIES 70
#endif
#ifndef WLAN_EID_NON_TX_BSSID_CAP
#define WLAN_EID_NON_TX_BSSID_CAP	83
#endif
#ifndef WLAN_EID_EXT_CAPAB
#define WLAN_EID_EXT_CAPAB		127
#endif
#ifndef WLAN_EID_VHT_OPERATION
#define WLAN_EID_VHT_OPERATION		192
#endif

#ifndef IEEE80211_HT_PARAM_CHA_SEC_NONE
#define IEEE80211_HT_PARAM_CHA_SEC_NONE		0x00
#define IEEE80211_HT_PARAM_CHA_SEC_ABOVE	0x01
#define IEEE80211_HT_PARAM_CHA_SEC_BELOW	0x03
#define IEEE80211_HT_PARAM_CHAN_WIDTH_ANY	0x04
#endif

#ifndef IEEE80211_NUM_BANDS
#define IEEE80211_NUM_BANDS		NUM_NL80211_BANDS
#endif

#ifndef NL80211_AC_VO
#define NL80211_AC_VO			0
#define NL80211_AC_VI			1
#define NL80211_AC_BE			2
#define NL80211_AC_BK			3
#endif

#ifndef NL80211_RRF_NO_INDOOR
#define NL80211_RRF_NO_INDOOR		(1 << 2)
#endif
#ifndef NL80211_CONN_FAIL_MAX_CLIENTS
#define NL80211_CONN_FAIL_MAX_CLIENTS	0
#endif
#ifndef NL80211_EXTERNAL_AUTH_ABORT
#define NL80211_EXTERNAL_AUTH_ABORT	1
#endif
#ifndef NL80211_SAE_PWE_HASH_TO_ELEMENT
#define NL80211_SAE_PWE_HASH_TO_ELEMENT	2
#endif
enum nl80211_timeout_reason {
	NL80211_TIMEOUT_UNSPECIFIED = 0,
	NL80211_TIMEOUT_SCAN = 1,
	NL80211_TIMEOUT_AUTH = 2,
	NL80211_TIMEOUT_ASSOC = 3,
	NL80211_TIMEOUT_ROAM = 4,
};
#ifndef RATE_INFO_FLAGS_40_MHZ_WIDTH
#define RATE_INFO_FLAGS_40_MHZ_WIDTH	(1 << 2)
#endif
#ifndef WIPHY_FLAG_CONTROL_PORT_PROTOCOL
#define WIPHY_FLAG_CONTROL_PORT_PROTOCOL (1U << 24)
#endif
#ifndef NLMSG_DEFAULT_SIZE
#define NLMSG_DEFAULT_SIZE		4096
#endif
#ifndef NETIF_F_GRO
#define NETIF_F_GRO			0
#endif
#ifndef NET_RX_DROP
#define NET_RX_DROP			1
#endif

struct ieee80211_txq_params {
	int ac;
	uint16_t txop;
	uint16_t cwmin;
	uint16_t cwmax;
	uint8_t aifs;
};

struct cfg80211_update_ft_ies_params {
	uint16_t md;
	const uint8_t *ie;
	size_t ie_len;
};

struct ipv6hdr {
	uint8_t prio_ver[4];
	__be16 payload_len;
	uint8_t nexthdr;
	uint8_t hop_limit;
	struct in6_addr saddr;
	struct in6_addr daddr;
} __packed;

struct icmp6hdr {
	uint8_t icmp6_type;
	uint8_t icmp6_code;
	uint16_t icmp6_cksum;
	uint32_t icmp6_un;
} __packed;

static inline struct sk_buff *
cfg80211_vendor_event_alloc(struct wiphy *wiphy, struct wireless_dev *wdev,
    int approxlen, int event_idx, gfp_t gfp)
{
	(void)wiphy;
	(void)wdev;
	(void)approxlen;
	(void)event_idx;
	(void)gfp;
	return (NULL);
}

static inline void
cfg80211_vendor_event(struct sk_buff *skb, gfp_t gfp)
{
	(void)gfp;
	if (skb != NULL)
		kfree_skb(skb);
}

#undef __counted_by
#define __counted_by(member)

#ifndef LIST_HEAD_INIT
#define LIST_HEAD_INIT(name)		LINUX_LIST_HEAD_INIT(name)
#endif

#ifndef MODULE_IMPORT_NS
#define MODULE_IMPORT_NS(ns)
#endif

#ifndef FALLTHROUGH
#define FALLTHROUGH			__attribute__((__fallthrough__))
#endif

/* Linux scnprintf / vscnprintf */
#ifndef scnprintf
#define scnprintf(buf, size, fmt, ...)	lx_scnprintf((buf), (size), (fmt), ##__VA_ARGS__)
#endif
int	 lx_scnprintf(char *buf, size_t size, const char *fmt, ...);
int	 lx_sscanf(const char *str, const char *fmt, ...);
#define sscanf				lx_sscanf

/* Standard IEEE 802.3 MSB-first CRC32 used by fwimage.c */
uint32_t lx_ether_crc(int length, const unsigned char *data);
#undef ether_crc
#define ether_crc(len, data)		lx_ether_crc((int)(len), (const unsigned char *)(data))

/* Little-endian ARM64 bitmap copy used by mifintrbit.c */
#ifndef bitmap_copy_le
static inline void
bitmap_copy_le(unsigned long *dst, const unsigned long *src, unsigned int nbits)
{
	memcpy(dst, src, BITS_TO_LONGS(nbits) * sizeof(unsigned long));
}
#endif

/* Linux local_clock() -> nanoseconds since boot */
unsigned long long lx_local_clock(void);
#ifndef local_clock
#define local_clock()			lx_local_clock()
#endif

/* Linux time_in_range */
#ifndef time_in_range
#define time_in_range(a, b, c)		(time_after_eq((a), (b)) && time_before_eq((a), (c)))
#endif

/* Linux timeval helper for mxman.c logging */
#ifndef _STRUCT_TIMEVAL_DEFINED_LX
struct lx_timeval {
	long tv_sec;
	long tv_usec;
};
static inline struct lx_timeval
lx_ns_to_timeval(int64_t ns)
{
	struct lx_timeval tv;

	if (ns < 0)
		ns = 0;
	tv.tv_sec = (long)(ns / 1000000000LL);
	tv.tv_usec = (long)((ns % 1000000000LL) / 1000LL);
	return (tv);
}
#define timeval				lx_timeval
#define ns_to_timeval(ns)		lx_ns_to_timeval(ns)
#endif

/* Wakelock / wakeup_source no-ops (replace scsc/scsc_wakelock.h & linux/wakelock.h) */
#ifndef _LINUX_WAKELOCK_H
#define _LINUX_WAKELOCK_H
#ifndef WAKE_LOCK_SUSPEND
#define WAKE_LOCK_SUSPEND		1
#endif
struct wakeup_source {
	const char *name;
	int active;
};
struct wake_lock {
	struct wakeup_source ws_inline;
};
struct scsc_wake_lock {
	struct wakeup_source *ws;
	struct wakeup_source ws_storage;
};
static inline void
lx_scsc_wl_init(struct device *dev, struct wakeup_source **ws, const char *name)
{
	static struct wakeup_source dummy_ws;

	(void)dev;
	dummy_ws.name = name;
	if (ws != NULL)
		*ws = &dummy_ws;
}
#define wake_lock_init(dev, ws, name)	lx_scsc_wl_init((dev), (ws), (name))
#define wake_lock_destroy(wl)		((void)(wl))
#define wake_lock(wl)			((void)(wl))
#define wake_lock_timeout(wl, tmo)	((void)(wl), (void)(tmo))
#define wake_unlock(wl)			((void)(wl))
#define wake_lock_active(wl)		(0)
#endif

/* Notifier chains, kobject uevents, and module_param_cb stubs */
#ifndef BLOCKING_NOTIFIER_HEAD
struct blocking_notifier_head {
	int dummy;
};
struct atomic_notifier_head {
	int dummy;
};
#define BLOCKING_NOTIFIER_HEAD(name)	struct blocking_notifier_head name = { 0 }
#define ATOMIC_NOTIFIER_HEAD(name)	struct atomic_notifier_head name = { 0 }
int	 blocking_notifier_chain_register(struct blocking_notifier_head *nh, struct notifier_block *n);
int	 blocking_notifier_chain_unregister(struct blocking_notifier_head *nh, struct notifier_block *n);
int	 blocking_notifier_call_chain(struct blocking_notifier_head *nh, unsigned long val, void *v);
int	 atomic_notifier_chain_register(struct atomic_notifier_head *nh, struct notifier_block *n);
int	 atomic_notifier_chain_unregister(struct atomic_notifier_head *nh, struct notifier_block *n);
int	 atomic_notifier_call_chain(struct atomic_notifier_head *nh, unsigned long val, void *v);
#endif

#ifndef KOBJ_ADD
#define KOBJ_ADD	0x02
#endif
#ifndef KOBJ_REMOVE
#define KOBJ_REMOVE	0x03
#endif
static inline int
kobject_uevent(struct kobject *kobj, int action)
{
	(void)kobj;
	(void)action;
	return (0);
}

#ifndef module_param_cb
struct kernel_param {
	const char *name;
	void *arg;
};
struct kernel_param_ops {
	unsigned int flags;
	int (*set)(const char *val, const struct kernel_param *kp);
	int (*get)(char *buffer, const struct kernel_param *kp);
	void (*free)(void *arg);
};
#define module_param_cb(name, ops, arg, perm)	static const void *__lx_kparam_##name __unused = (ops)
#endif

/* VFS / usermodehelper stubs for mx140_file.c & mxman.c */
#ifndef KERNEL_DS
typedef int mm_segment_t;
#define KERNEL_DS			0
#define get_fs()			(0)
#define set_fs(x)			((void)(x))
#endif
struct kstat {
	uint64_t size;
};
static inline int
vfs_stat(const char *path, struct kstat *st)
{
	(void)path;
	if (st != NULL)
		st->size = 0;
	return (0);
}
#ifndef UMH_NO_WAIT
#define UMH_NO_WAIT			0
#define UMH_WAIT_EXEC			1
#define UMH_WAIT_PROC			2
#define UMH_KILLABLE			4
#endif
struct subprocess_info {
	char **argv;
	char **envp;
	int wait;
	void *data;
};
static inline char **
argv_split(gfp_t gfp, const char *str, int *argcp)
{
	(void)gfp;
	(void)str;
	if (argcp != NULL)
		*argcp = 0;
	return (NULL);
}
static inline void
argv_free(char **argv)
{
	(void)argv;
}
static inline struct subprocess_info *
call_usermodehelper_setup(const char *path, char **argv, char **envp, gfp_t gfp_mask,
    int (*init)(struct subprocess_info *info, struct cred *new_cred),
    void (*cleanup)(struct subprocess_info *info), void *data)
{
	(void)path;
	(void)argv;
	(void)envp;
	(void)gfp_mask;
	(void)init;
	(void)cleanup;
	(void)data;
	return (NULL);
}
static inline int
call_usermodehelper_exec(struct subprocess_info *sub_info, int wait)
{
	(void)sub_info;
	(void)wait;
	return (-ENOENT);
}
static inline int
call_usermodehelper(const char *path, char **argv, char **envp, int wait)
{
	(void)path;
	(void)argv;
	(void)envp;
	(void)wait;
	return (-ENOENT);
}
static inline long
usermodehelper_read_lock_wait(long timeout)
{
	(void)timeout;
	return (0);
}
static inline void
usermodehelper_read_unlock(void)
{
}

/* Firmware basename loader */
int	 lx_scsc_request_firmware(const struct linuxkpi_firmware **out, const char *name, struct device *dev);
#undef request_firmware
#define request_firmware(out, name, dev) lx_scsc_request_firmware((out), (name), (dev))

/* cfg80211 & net_device hooks routed into lx_cfg.c */
struct cfg80211_bss *lx_cfg80211_inform_bss_frame(struct wiphy *wiphy,
    struct linuxkpi_ieee80211_channel *channel, struct ieee80211_mgmt *mgmt,
    size_t len, int32_t signal_mbm, gfp_t gfp);
struct cfg80211_bss *lx_cfg80211_get_bss(struct wiphy *wiphy,
    struct linuxkpi_ieee80211_channel *channel, const uint8_t *bssid,
    const uint8_t *ssid, size_t ssid_len, int bss_type, int privacy);
void	 lx_cfg80211_scan_done(struct cfg80211_scan_request *request, struct cfg80211_scan_info *info);
void	 lx_cfg80211_connect_bss(struct net_device *ndev, const uint8_t *bssid,
    struct cfg80211_bss *bss, const uint8_t *req_ie, size_t req_ie_len,
    const uint8_t *resp_ie, size_t resp_ie_len, int status, gfp_t gfp, int timeout_reason);
void	 lx_cfg80211_disconnected(struct net_device *ndev, uint16_t reason, bool locally_generated);
void	 lx_cfg80211_roamed(struct net_device *ndev, struct cfg80211_roam_info *info);
void	 lx_wiphy_unregister(struct wiphy *wiphy);
int	 lx_register_netdev(struct net_device *ndev);
void	 lx_unregister_netdev(struct net_device *ndev);
int	 lx_netif_rx(struct sk_buff *skb);
__be16	 lx_eth_type_trans(struct sk_buff *skb, struct net_device *ndev);
void	 lx_netif_carrier(struct net_device *ndev, bool on);
bool	 lx_netif_carrier_ok(struct net_device *ndev);

#undef cfg80211_inform_bss_frame
#define cfg80211_inform_bss_frame(wiphy, chan, mgmt, len, sig, gfp) \
	lx_cfg80211_inform_bss_frame((wiphy), (chan), (mgmt), (len), (sig), (gfp))
#undef cfg80211_get_bss
#define cfg80211_get_bss(wiphy, chan, bssid, ssid, ssid_len, bss_type, privacy) \
	lx_cfg80211_get_bss((wiphy), (chan), (bssid), (ssid), (ssid_len), (int)(bss_type), (int)(privacy))
#undef cfg80211_put_bss
#define cfg80211_put_bss(wiphy, bss)			((void)(wiphy), (void)(bss))
#undef cfg80211_unlink_bss
#define cfg80211_unlink_bss(wiphy, bss)			((void)(wiphy), (void)(bss))
#undef cfg80211_ref_bss
#define cfg80211_ref_bss(wiphy, bss)			((void)(wiphy), (void)(bss))

#undef cfg80211_scan_done
#define cfg80211_scan_done(req, info)			lx_cfg80211_scan_done((req), (info))

#undef cfg80211_connect_bss
#define cfg80211_connect_bss(dev, bssid, bss, req_ie, req_len, resp_ie, resp_len, st, gfp, tmo) \
	lx_cfg80211_connect_bss((dev), (bssid), (bss), (req_ie), (req_len), (resp_ie), (resp_len), (st), (gfp), (int)(tmo))
#undef cfg80211_connect_result
#define cfg80211_connect_result(dev, bssid, req_ie, req_len, resp_ie, resp_len, st, gfp) \
	lx_cfg80211_connect_bss((dev), (bssid), NULL, (req_ie), (req_len), (resp_ie), (resp_len), (st), (gfp), 0)
#undef cfg80211_connect_timeout
#define cfg80211_connect_timeout(dev, bssid, req_ie, req_len, gfp, tmo) \
	lx_cfg80211_connect_bss((dev), (bssid), NULL, (req_ie), (req_len), NULL, 0, -1, (gfp), (int)(tmo))

#undef cfg80211_disconnected
#define cfg80211_disconnected(ndev, reason, ie, ie_len, local, gfp) \
	lx_cfg80211_disconnected((ndev), (reason), (local))
#undef cfg80211_roamed
#define cfg80211_roamed(ndev, info, gfp)			lx_cfg80211_roamed((ndev), (info))
#undef cfg80211_roamed_bss
#define cfg80211_roamed_bss(ndev, bss, req, req_len, resp, resp_len, gfp) \
	lx_cfg80211_roamed((ndev), NULL)

#undef cfg80211_remain_on_channel_expired
#define cfg80211_remain_on_channel_expired(wdev, cookie, chan, gfp) ((void)0)
#undef cfg80211_ready_on_channel
#define cfg80211_ready_on_channel(wdev, cookie, chan, dur, gfp) ((void)0)
#undef cfg80211_mgmt_tx_status
#define cfg80211_mgmt_tx_status(wdev, cookie, buf, len, ack, gfp) ((void)0)
#undef cfg80211_rx_mgmt
#define cfg80211_rx_mgmt(wdev, freq, sig, buf, len, flags) (false)
#undef cfg80211_michael_mic_failure
#define cfg80211_michael_mic_failure(dev, addr, type, id, tsc, gfp) ((void)0)
#undef cfg80211_ibss_joined
#define cfg80211_ibss_joined(dev, bssid, chan, gfp)	((void)0)
#undef cfg80211_new_sta
#define cfg80211_new_sta(dev, mac, sinfo, gfp)		((void)0)
#undef cfg80211_del_sta
#define cfg80211_del_sta(dev, mac, gfp)			((void)0)
#undef cfg80211_ch_switch_notify
#define cfg80211_ch_switch_notify(dev, chandef)		((void)0)
#undef cfg80211_pmksa_candidate_notify
#define cfg80211_pmksa_candidate_notify(dev, idx, bssid, preauth, gfp) ((void)0)
#undef cfg80211_sched_scan_results
#define cfg80211_sched_scan_results(wiphy, reqid)	((void)0)
#undef cfg80211_sched_scan_stopped
#define cfg80211_sched_scan_stopped(wiphy, reqid)	((void)0)
#undef cfg80211_external_auth_request
#define cfg80211_external_auth_request(dev, params, gfp) (-EOPNOTSUPP)
#undef cfg80211_conn_failed
#define cfg80211_conn_failed(dev, mac, reason, gfp)	((void)0)

#undef wiphy_unregister
#define wiphy_unregister(wiphy)				lx_wiphy_unregister((wiphy))
#undef register_netdev
#define register_netdev(ndev)				lx_register_netdev((ndev))
#undef register_netdevice
#define register_netdevice(ndev)			lx_register_netdev((ndev))
#undef cfg80211_register_netdevice
#define cfg80211_register_netdevice(ndev)		lx_register_netdev((ndev))
#undef unregister_netdev
#define unregister_netdev(ndev)				lx_unregister_netdev((ndev))
#undef unregister_netdevice
#define unregister_netdevice(ndev)			lx_unregister_netdev((ndev))
#undef unregister_netdevice_queue
#define unregister_netdevice_queue(ndev, head)		lx_unregister_netdev((ndev))
#undef unregister_netdevice_many
#define unregister_netdevice_many(head)			((void)(head))
#undef cfg80211_unregister_netdevice
#define cfg80211_unregister_netdevice(ndev)		lx_unregister_netdev((ndev))

#undef netif_rx
#define netif_rx(skb)					lx_netif_rx((skb))
#undef netif_rx_ni
#define netif_rx_ni(skb)				lx_netif_rx((skb))

static inline void
lx_eth_hw_addr_set(struct net_device *ndev, const u8 *addr)
{
	memcpy(ndev->dev_addr, addr, ETH_ALEN);
}
#undef eth_hw_addr_set
#define eth_hw_addr_set(ndev, addr)			lx_eth_hw_addr_set((ndev), (addr))
#undef eth_type_trans
#define eth_type_trans(skb, ndev)			lx_eth_type_trans((skb), (ndev))
#undef netif_carrier_on
#define netif_carrier_on(ndev)				lx_netif_carrier((ndev), true)
#undef netif_carrier_off
#define netif_carrier_off(ndev)				lx_netif_carrier((ndev), false)
#undef netif_carrier_ok
#define netif_carrier_ok(ndev)				lx_netif_carrier_ok((ndev))

/* Subqueue flow control no-ops (single TX thread in hakios.lx.c) */
void	 lx_flush_workqueue(struct workqueue_struct *wq);
#undef flush_workqueue
#define flush_workqueue(wq)				lx_flush_workqueue((wq))
#undef drain_workqueue
#define drain_workqueue(wq)				lx_flush_workqueue((wq))
#ifndef NET_NAME_PREDICTABLE
#define NET_NAME_PREDICTABLE				2
#endif
#ifndef alloc_netdev_mqs
#define alloc_netdev_mqs(sz, name, assign, setup, txq, rxq) \
	((void)(txq), (void)(rxq), alloc_netdev((sz), (name), (assign), (setup)))
#endif
static inline int
dev_alloc_name(struct net_device *dev, const char *name)
{
	char buf[IFNAMSIZ];

	if (dev == NULL || name == NULL)
		return (-EINVAL);
	if (strchr(name, '%') != NULL)
		snprintf(buf, sizeof(buf), name, 0);
	else
		strlcpy(buf, name, sizeof(buf));
	strlcpy(dev->name, buf, sizeof(dev->name));
	return (0);
}
#ifndef rtnl_is_locked
#define rtnl_is_locked()				(1)
#endif
#ifndef netif_tx_disable
#define netif_tx_disable(dev)				((void)(dev))
#endif
#undef netif_tx_start_all_queues
#define netif_tx_start_all_queues(dev)			((void)(dev))
#undef netif_tx_stop_all_queues
#define netif_tx_stop_all_queues(dev)			((void)(dev))
#undef netif_start_subqueue
#define netif_start_subqueue(dev, q)			((void)(dev), (void)(q))
#undef netif_stop_subqueue
#define netif_stop_subqueue(dev, q)			((void)(dev), (void)(q))
#undef netif_wake_subqueue
#define netif_wake_subqueue(dev, q)			((void)(dev), (void)(q))
#undef netif_subqueue_stopped
#define netif_subqueue_stopped(dev, q)			(false)
#undef netif_dormant_on
#define netif_dormant_on(dev)				((void)(dev))
#undef netif_dormant_off
#define netif_dormant_off(dev)				((void)(dev))

#endif
