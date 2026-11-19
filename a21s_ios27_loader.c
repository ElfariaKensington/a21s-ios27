/*
 * Galaxy A21s (SM-A217F, Exynos 850) UEFI stage-2 loader for iOS 27 SWEETXNU.
 *
 * Loaded by stage1 (a21s/boot/efi_shim.c) as a PE32+ image at ImageBase 0xA1000000
 * from /data/ios/payload ("XNUP" at 0xF0000000, loader.efi at 0xF0000020).
 */

#include <stdint.h>
#include <stddef.h>

#define EFIAPI
typedef uint64_t efi_status;
typedef void *efi_handle;
typedef uint16_t char16;

#define EFI_ERROR_BIT 0x8000000000000000ULL
#define EFI_ERROR(s)  (((s) & EFI_ERROR_BIT) != 0)

typedef struct {
    uint32_t a; uint16_t b; uint16_t c; uint8_t d[8];
} efi_guid;

struct efi_table_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

typedef struct efi_text_output {
    void *reset;
    efi_status (EFIAPI *output_string)(struct efi_text_output *self, const char16 *str);
} efi_text_output;

typedef struct {
    uint32_t media_id;
    uint8_t removable, present, logical, read_only, write_caching;
    uint8_t pad[3];
    uint32_t block_size;
    uint32_t io_align;
    uint64_t last_block;
} efi_block_media;

typedef struct efi_block_io {
    uint64_t revision;
    efi_block_media *media;
    void *reset;
    efi_status (EFIAPI *read_blocks)(struct efi_block_io *self, uint32_t media_id, uint64_t lba, uint64_t size, void *buf);
    efi_status (EFIAPI *write_blocks)(struct efi_block_io *self, uint32_t media_id, uint64_t lba, uint64_t size, void *buf);
    efi_status (EFIAPI *flush_blocks)(struct efi_block_io *self);
} efi_block_io;

typedef struct {
    struct efi_table_header hdr;
    void *raise_tpl, *restore_tpl;
    void *allocate_pages, *free_pages, *get_memory_map, *allocate_pool, *free_pool;
    void *create_event, *set_timer, *wait_for_event, *signal_event, *close_event, *check_event;
    void *install_protocol_interface, *reinstall_protocol_interface, *uninstall_protocol_interface;
    efi_status (EFIAPI *handle_protocol)(efi_handle h, const efi_guid *guid, void **iface);
    void *reserved, *register_protocol_notify, *locate_handle, *locate_device_path, *install_configuration_table;
    void *load_image, *start_image, *exit, *unload_image, *exit_boot_services, *get_next_monotonic_count;
    void *stall, *set_watchdog_timer;
    void *connect_controller, *disconnect_controller, *open_protocol, *close_protocol, *open_protocol_information;
    void *protocols_per_handle;
    efi_status (EFIAPI *locate_handle_buffer)(int type, const efi_guid *guid, void *key, uint64_t *count, efi_handle **buf);
    efi_status (EFIAPI *locate_protocol)(const efi_guid *guid, void *registration, void **iface);
} efi_boot_services;

typedef struct {
    struct efi_table_header hdr;
    char16 *firmware_vendor;
    uint32_t firmware_revision;
    uint32_t pad;
    efi_handle con_in_handle;
    void *con_in;
    efi_handle con_out_handle;
    efi_text_output *con_out;
    efi_handle stderr_handle;
    efi_text_output *std_err;
    void *rt;
    efi_boot_services *bs;
    uint64_t nr_tables;
    void *tables;
} efi_system_table;

#define PAYLOAD_BASE        0xF0000000ULL
#define PAYLOAD_MAX_SCAN    0x03F00000ULL
#define XNUP_MAGIC          0x50554E58U
#define BANK2_BASE          0x880000000ULL
#define BANK2_STAGE1_LIMIT  0x8C0000000ULL
#define GICC_CTLR_PA        0x12A02000ULL
#define PMU_SWRESET_PA      0x11863A00ULL
#define PMSG_BASE_PA        0xF0F14000ULL
#define PMSG_DATA_CAP       0x3F00U
#define A5XELTE_MARKER_PA   0x8FFFF100ULL
#define PAGE_16K            0x4000ULL
#define BOOT_LINE_LENGTH    608

struct xnup_header {
    uint32_t magic;
    uint32_t size;
};

struct xnup_entry {
    char name[16];
    uint32_t size;
    uint32_t reserved;
};

struct sweet_seg {
    uint64_t vmaddr, vmsize, file_off, file_size;
};

struct sweet_payload {
    char magic[8];
    uint32_t version, nsegs;
    uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
    uint64_t phys_base, mem_size;
    char cmdline[BOOT_LINE_LENGTH];
    struct sweet_seg segs[];
};

struct boot_video {
    uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
    uint16_t Revision, Version;
    uint64_t virtBase, physBase, memSize, topOfKernelData;
    struct boot_video Video;
    uint32_t machineType;
    uint64_t deviceTreeP;
    uint32_t deviceTreeLength;
    char CommandLine[BOOT_LINE_LENGTH];
    uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct sweet_payload, segs) == 0x2a8, "sweet_payload layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args size");

static efi_system_table *ST;
static efi_block_io *log_bio;
static char loader_log[8192] __attribute__((aligned(64)));
static size_t loader_log_len;
static uint8_t zero_4k[4096] __attribute__((aligned(64)));
static int pmsg_ready = 0;

static inline uint64_t read_cntvct(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline uint64_t timer_hz(void) {
    uint64_t hz;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    return hz ? hz : 26000000ULL;
}

static void clean_dcache(uint64_t start, uint64_t end) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ULL << ((ctr >> 16) & 0xf);
    for (uint64_t p = start & ~(line - 1); p < end; p += line) {
        __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void inval_dcache(uint64_t start, uint64_t end) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ULL << ((ctr >> 16) & 0xf);
    for (uint64_t p = start & ~(line - 1); p < end; p += line) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void mem_zero(void *dst, size_t n) {
    volatile uint8_t *p = (volatile uint8_t *)dst;
    while (n && ((uintptr_t)p & 7)) { *p++ = 0; n--; }
    volatile uint64_t *q = (volatile uint64_t *)p;
    while (n >= 8) { *q++ = 0; n -= 8; }
    p = (volatile uint8_t *)q;
    while (n--) { *p++ = 0; }
}

static void mem_copy(void *dst, const void *src, size_t n) {
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const volatile uint8_t *s = (const volatile uint8_t *)src;
    if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
        volatile uint64_t *dq = (volatile uint64_t *)d;
        const volatile uint64_t *sq = (const volatile uint64_t *)s;
        while (n >= 8) { *dq++ = *sq++; n -= 8; }
        d = (volatile uint8_t *)dq;
        s = (const volatile uint8_t *)sq;
    }
    while (n--) { *d++ = *s++; }
}

static int str_eq_n(const char *a, const char *b, size_t maxn) {
    for (size_t i = 0; i < maxn; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0) return 1;
    }
    return b[maxn] == 0;
}

static void logdump_init(void) {
    static const efi_guid blk_guid = { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
    uint64_t count = 0;
    efi_handle *handles = NULL;
    log_bio = NULL;
    if (!ST || !ST->bs || !ST->bs->locate_handle_buffer || !ST->bs->handle_protocol) return;
    if (EFI_ERROR(ST->bs->locate_handle_buffer(2, &blk_guid, NULL, &count, &handles))) return;
    for (uint64_t i = 0; i < count; i++) {
        efi_block_io *bio = NULL;
        if (!EFI_ERROR(ST->bs->handle_protocol(handles[i], &blk_guid, (void **)&bio)) &&
            bio && bio->media && bio->media->media_id == 2 && bio->media->block_size == 512) {
            log_bio = bio;
            break;
        }
    }
    if (log_bio) {
        /* Clear XNU's logdump header at LBA 16 (sector 71568) so stale previous runs cannot be mistaken for this boot */
        mem_zero(zero_4k, sizeof(zero_4k));
        log_bio->write_blocks(log_bio, 2, 16, sizeof(zero_4k), zero_4k);
    }
}

static void logdump_flush(void) {
    if (!log_bio) return;
    log_bio->write_blocks(log_bio, 2, 0, sizeof(loader_log), loader_log);
}

static void pmsg_puts(const char *s) {
    if (!pmsg_ready) return;
    volatile uint32_t *h = (volatile uint32_t *)(uintptr_t)PMSG_BASE_PA;
    volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)(PMSG_BASE_PA + 12);
    if (h[0] != 0x43474244U || h[1] >= PMSG_DATA_CAP || h[2] > PMSG_DATA_CAP) {
        h[0] = 0x43474244U;
        h[1] = 0;
        h[2] = 0;
    }
    for (; *s; s++) {
        uint32_t pos = h[1];
        d[pos] = (uint8_t)*s;
        h[1] = (pos + 1) % PMSG_DATA_CAP;
        if (h[2] < PMSG_DATA_CAP) h[2] = h[2] + 1;
    }
    clean_dcache(PMSG_BASE_PA, PMSG_BASE_PA + 0x4000);
}

static void con_puts(const char *s) {
    for (const char *p = s; *p && loader_log_len + 1 < sizeof(loader_log); p++) {
        loader_log[loader_log_len++] = *p;
        loader_log[loader_log_len] = 0;
    }
    pmsg_puts(s);
    if (!ST || !ST->con_out || !ST->con_out->output_string) return;
    char16 buf[128];
    size_t k = 0;
    for (; *s; s++) {
        if (*s == '\n') {
            buf[k++] = '\r';
            if (k >= 126) { buf[k] = 0; ST->con_out->output_string(ST->con_out, buf); k = 0; }
        }
        buf[k++] = (uint8_t)*s;
        if (k >= 126) { buf[k] = 0; ST->con_out->output_string(ST->con_out, buf); k = 0; }
    }
    if (k) { buf[k] = 0; ST->con_out->output_string(ST->con_out, buf); }
}

static void con_hex(const char *label, uint64_t val) {
    char b[24];
    static const char hex[] = "0123456789abcdef";
    con_puts(label);
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) b[2 + i] = hex[(val >> (60 - 4 * i)) & 0xf];
    b[18] = '\n'; b[19] = 0;
    con_puts(b);
}

static void swreset_reboot(void) {
    logdump_flush();
    *(volatile uint32_t *)(uintptr_t)PMU_SWRESET_PA = 2;
    __asm__ volatile("dsb sy; isb" ::: "memory");
    uint64_t wait = read_cntvct() + timer_hz();
    while (read_cntvct() < wait) {}
    register uint64_t x0 __asm__("x0") = 0x84000009ULL;
    __asm__ volatile("smc #0" : "+r"(x0) :: "x1", "x2", "x3", "memory");
    for (;;) __asm__ volatile("wfe");
}

/* ---------------- Core-0 Himax HX83112A SPI1 + GPIO_ALIVE key poller ---------------- */

#define A21S_CMU        0x11C00000ULL
#define A21S_USIMODE    0x11C22000ULL
#define A21S_GPM        0x11C30000ULL
#define A21S_SPI        0x11D00000ULL
#define A21S_GPA        0x11850000ULL
#define A21S_MBOX_MAGIC 0x584f424d53313241ULL
#define A21S_MBOX_EVENTS 128
#define A21S_FINGERS    10
#define A21S_EV_LEN     56
#define A21S_STALE_MS   150
#define AR(pa)          (*(volatile uint32_t *)(uintptr_t)(pa))

struct a21s_mbox_event {
    uint32_t type;
    uint32_t finger;
    uint32_t x, y;
};

struct a21s_mbox {
    uint64_t magic;
    uint32_t version, head;
    uint32_t chip, events_size;
    uint64_t irqs, frames, bad;
    struct a21s_mbox_event ev[A21S_MBOX_EVENTS];
} __attribute__((aligned(64)));

static struct a21s_mbox a21s_mbox;
static struct {
    uint8_t down;
    uint32_t x, y;
} a21s_fing[A21S_FINGERS];
static uint64_t a21s_probe_at, a21s_keys_at, a21s_last_frame, a21s_skip_until;
static int a21s_up_posted, a21s_dn_posted, a21s_chord;

static uint64_t a21s_ms(void) {
    return read_cntvct() / (timer_hz() / 1000ULL);
}

static void a21s_udelay(uint32_t us) {
    uint64_t end = read_cntvct() + (uint64_t)us * (timer_hz() / 1000000ULL);
    while (read_cntvct() < end) {}
}

static void a21s_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y) {
    struct a21s_mbox_event *e = &a21s_mbox.ev[a21s_mbox.head % A21S_MBOX_EVENTS];
    e->type = type;
    e->finger = finger;
    e->x = x;
    e->y = y;
    clean_dcache((uint64_t)(uintptr_t)e, (uint64_t)(uintptr_t)(e + 1));
    a21s_mbox.head++;
    clean_dcache((uint64_t)(uintptr_t)&a21s_mbox, (uint64_t)(uintptr_t)&a21s_mbox + 48);
}

static void a21s_gpio_func(uint32_t bank, int pin, uint32_t fn) {
    AR(A21S_GPM + bank) = (AR(A21S_GPM + bank) & ~(0xfu << (pin * 4))) | (fn << (pin * 4));
}

static void a21s_cs(int on) {
    uint32_t d = AR(A21S_GPM + 0x64);
    AR(A21S_GPM + 0x64) = on ? (d & ~1u) : (d | 1u);
    (void)AR(A21S_GPM + 0x64);
    __asm__ volatile("dsb sy" ::: "memory");
}

static void a21s_spi_flush(void) {
    uint32_t ch = AR(A21S_SPI + 0x00) & ~3u;
    AR(A21S_SPI + 0x20) = 0;
    AR(A21S_SPI + 0x00) = ch;
    AR(A21S_SPI + 0x00) = ch | (1u << 5);
    for (int i = 0; i < 1000 && ((AR(A21S_SPI + 0x14) >> 6) & 0x7f); i++) {}
    for (int i = 0; i < 200 && ((AR(A21S_SPI + 0x14) >> 15) & 0x7f); i++) {
        (void)AR(A21S_SPI + 0x1C);
    }
    AR(A21S_SPI + 0x00) = ch;
    AR(A21S_SPI + 0x24) = 0x1e;
    AR(A21S_SPI + 0x24) = 0;
}

static void a21s_hw_init(void) {
    static const uint32_t gates[] = { 0x2018, 0x2040, 0x2044, 0x2048 };
    for (int i = 0; i < 4; i++) {
        AR(A21S_CMU + gates[i]) |= (1u << 20) | (1u << 21);
    }
    AR(A21S_CMU + 0x1804) = (AR(A21S_CMU + 0x1804) & ~0x1fu) | 3;
    for (int i = 0; i < 10000 && (AR(A21S_CMU + 0x1804) & (1u << 16)); i++) {}
    AR(A21S_USIMODE) = 2;
    a21s_gpio_func(0x00, 0, 2);
    a21s_gpio_func(0x20, 0, 2);
    a21s_gpio_func(0x40, 0, 2);
    for (int b = 0; b < 3; b++) {
        AR(A21S_GPM + b * 0x20 + 8) &= ~3u;
    }
    AR(A21S_GPM + 0x64) |= 1u;
    a21s_gpio_func(0x60, 0, 1);
    AR(A21S_GPM + 0x68) &= ~3u;
    AR(A21S_GPA) &= ~0xfu;
    AR(A21S_SPI + 0xC4) = 0;
    AR(A21S_SPI + 0x0C) = 1;
    AR(A21S_SPI + 0x10) = 0;
    AR(A21S_SPI + 0x08) = 0;
    AR(A21S_SPI + 0x20) = 0;
    AR(A21S_SPI + 0x24) = 0x1e;
    AR(A21S_SPI + 0x24) = 0;
    AR(A21S_SPI + 0x28) = 0;
    AR(A21S_SPI + 0x00) = 0;
    AR(A21S_SPI + 0x08) = 0x3ffu << 19;
    AR(A21S_SPI + 0x2C) = 0;
    a21s_spi_flush();
}

static int a21s_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len) {
    uint32_t done = 0;
    a21s_cs(1);
    AR(A21S_SPI + 0x0C) = 0;
    (void)AR(A21S_SPI + 0x0C);
    a21s_udelay(5);
    while (done < len) {
        uint32_t n = len - done > 32 ? 32 : len - done, got = 0;
        uint64_t t;
        a21s_spi_flush();
        for (uint32_t i = 0; i < n; i++) {
            AR(A21S_SPI + 0x18) = tx ? tx[done + i] : 0;
        }
        AR(A21S_SPI + 0x00) |= 3u;
        t = a21s_ms() + 20;
        while (got < n) {
            if ((AR(A21S_SPI + 0x14) >> 15) & 0x7f) {
                uint8_t b = (uint8_t)AR(A21S_SPI + 0x1C);
                if (rx) rx[done + got] = b;
                got++;
            } else if (a21s_ms() > t) {
                break;
            }
        }
        AR(A21S_SPI + 0x00) &= ~3u;
        if (got != n) {
            AR(A21S_SPI + 0x0C) = 1;
            a21s_cs(0);
            return -1;
        }
        done += n;
    }
    a21s_udelay(2);
    AR(A21S_SPI + 0x0C) = 1;
    (void)AR(A21S_SPI + 0x0C);
    a21s_cs(0);
    a21s_udelay(50);
    return 0;
}

static int a21s_hx_write(uint8_t reg, const uint8_t *d, uint32_t n) {
    uint8_t b[16];
    b[0] = 0xF2;
    b[1] = reg;
    for (uint32_t i = 0; i < n && i < 12; i++) b[2 + i] = d[i];
    return a21s_xfer(b, NULL, 2 + n);
}

static int a21s_hx_read(uint8_t reg, uint8_t *d, uint32_t n) {
    uint8_t tx[64], rx[64];
    if (n + 3 > sizeof(rx)) return -1;
    tx[0] = 0xF3; tx[1] = reg; tx[2] = 0;
    for (uint32_t i = 3; i < n + 3; i++) tx[i] = 0;
    if (a21s_xfer(tx, rx, n + 3)) return -1;
    for (uint32_t i = 0; i < n; i++) d[i] = rx[3 + i];
    return 0;
}

static int a21s_hx_wake(void) {
    uint8_t c = 0x31, i4 = 0x10, r1 = 0, r2 = 0;
    (void)a21s_hx_read(0x13, &r1, 1);
    for (int n = 0; n < 10; n++) {
        if (!a21s_hx_write(0x13, &c, 1) && !a21s_hx_write(0x0d, &i4, 1) &&
            !a21s_hx_read(0x13, &r1, 1) && !a21s_hx_read(0x0d, &r2, 1) &&
            r1 == 0x31 && r2 == 0x10) {
            return 0;
        }
        a21s_udelay(1000);
    }
    return -1;
}

static int a21s_read_icid(uint32_t *id) {
    uint8_t a[4] = { 0xd0, 0x00, 0x00, 0x90 }, z[4] = { 0, 0, 0, 0 }, v[4];
    (void)a21s_hx_wake();
    if (a21s_hx_write(0x00, a, 4) || a21s_hx_write(0x0c, z, 4) || a21s_hx_read(0x08, v, 4)) {
        return -1;
    }
    *id = (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
    return 0;
}

static void a21s_input_init(void) {
    mem_zero(&a21s_mbox, sizeof(a21s_mbox));
    mem_zero(a21s_fing, sizeof(a21s_fing));
    a21s_probe_at = a21s_keys_at = a21s_last_frame = a21s_skip_until = 0;
    a21s_up_posted = a21s_dn_posted = a21s_chord = 0;
    a21s_mbox.version = 1;
    a21s_mbox.events_size = sizeof(struct a21s_mbox_event);
    a21s_mbox.magic = A21S_MBOX_MAGIC;
    clean_dcache((uint64_t)(uintptr_t)&a21s_mbox, (uint64_t)(uintptr_t)&a21s_mbox + sizeof(a21s_mbox));
}

static void a21s_release_all(void) {
    for (int i = 0; i < A21S_FINGERS; i++) {
        if (a21s_fing[i].down) {
            a21s_fing[i].down = 0;
            a21s_post(3, (uint32_t)i, a21s_fing[i].x, a21s_fing[i].y);
        }
    }
    a21s_last_frame = 0;
}

static void a21s_frame(const uint8_t *ev) {
    for (int i = 0; i < A21S_FINGERS; i++) {
        uint32_t x = ((uint32_t)ev[i * 4] << 8) | ev[i * 4 + 1];
        uint32_t y = ((uint32_t)ev[i * 4 + 2] << 8) | ev[i * 4 + 3];
        if (x != 0xffff && y != 0xffff) {
            if (!a21s_fing[i].down) {
                a21s_fing[i].down = 1;
                a21s_fing[i].x = x;
                a21s_fing[i].y = y;
                a21s_post(1, (uint32_t)i, x, y);
            } else if (a21s_fing[i].x != x || a21s_fing[i].y != y) {
                a21s_fing[i].x = x;
                a21s_fing[i].y = y;
                a21s_post(2, (uint32_t)i, x, y);
            }
        } else if (a21s_fing[i].down) {
            a21s_fing[i].down = 0;
            a21s_post(3, (uint32_t)i, a21s_fing[i].x, a21s_fing[i].y);
        }
    }
}

static void a21s_keys(void) {
    uint32_t gpa0 = AR(A21S_GPA + 4), gpa1 = AR(A21S_GPA + 0x24);
    int up = !(gpa0 & (1u << 7)), dn = !(gpa1 & 1u), both = up && dn;
    static int last_up, last_dn;
    static uint64_t both_since;
    if (up != last_up || dn != last_dn) {
        last_up = up;
        last_dn = dn;
        return;
    }
    if (both) {
        uint64_t now = a21s_ms();
        if (!both_since) {
            both_since = now;
        } else if (now - both_since >= 4500) {
            *(volatile uint32_t *)(uintptr_t)PMU_SWRESET_PA = 2U;
            __asm__ volatile("dsb sy; isb" ::: "memory");
        }
    } else {
        both_since = 0;
    }
    if (both != a21s_chord) {
        a21s_chord = both;
        a21s_post(both ? 4 : 5, 1, 0, 0);
    }
    if (up != a21s_up_posted && (!up || !dn)) {
        a21s_up_posted = up;
        a21s_post(up ? 4 : 5, 2, 0, 0);
    }
    if (dn != a21s_dn_posted && (!dn || !up)) {
        a21s_dn_posted = dn;
        a21s_post(dn ? 4 : 5, 3, 0, 0);
    }
}

static void a21s_input_poll(void) {
    uint64_t now = a21s_ms();
    if (now - a21s_keys_at >= 10) {
        a21s_keys_at = now;
        a21s_keys();
    }
    if (!a21s_mbox.chip) {
        if (now >= a21s_probe_at) {
            uint32_t id = 0;
            a21s_probe_at = now + 200;
            a21s_hw_init();
            if (!a21s_read_icid(&id) && id == 0x83112a00U) {
                a21s_mbox.chip = 1;
                clean_dcache((uint64_t)(uintptr_t)&a21s_mbox, (uint64_t)(uintptr_t)&a21s_mbox + 48);
            }
        }
        return;
    }
    if (AR(A21S_GPA + 4) & 1) {
        a21s_skip_until = 0;
        if (a21s_last_frame && now - a21s_last_frame > A21S_STALE_MS) {
            a21s_release_all();
        }
        return;
    }
    if (now < a21s_skip_until) return;
    a21s_skip_until = now + 3;
    {
        uint8_t ev[A21S_EV_LEN];
        uint32_t sum = 0, any = 0;
        int good = 0;
        a21s_mbox.irqs++;
        for (int attempt = 0; attempt < 3 && !good; attempt++) {
            if (attempt) a21s_udelay(300);
            sum = any = 0;
            if (a21s_hx_read(0x30, ev, A21S_EV_LEN)) continue;
            for (int i = 0; i < A21S_EV_LEN; i++) {
                sum += ev[i];
                any |= i < 52 ? ev[i] : 0;
            }
            if (ev[52] == 0xff && (sum & 0xff) == 0) {
                a21s_release_all();
                return;
            }
            good = (sum & 0xff) == 0 && any;
        }
        if (!good) {
            a21s_mbox.bad++;
            return;
        }
        a21s_mbox.frames++;
        a21s_frame(ev);
        a21s_last_frame = a21s_ms();
        clean_dcache((uint64_t)(uintptr_t)&a21s_mbox, (uint64_t)(uintptr_t)&a21s_mbox + 48);
    }
}

/* ---------------- CPU 1 (< 4 GiB) PSCI CPU_ON trampoline ---------------- */

struct cpu1_boot_params {
    uint64_t entry_pa;
    uint64_t ba_pa;
    volatile uint64_t stage;
    volatile uint64_t go;
} __attribute__((aligned(64)));

static struct cpu1_boot_params cpu1_params;
void a21s_cpu1_entry(void);

__asm__(
    ".text\n"
    ".balign 64\n"
    ".globl a21s_cpu1_entry\n"
    "a21s_cpu1_entry:\n"
    "  msr daifset, #0xf\n"
    "  mrs x1, CurrentEL\n"
    "  cmp x1, #0x8\n"
    "  b.ne 1f\n"
    "  mov x1, #(1 << 31)\n"
    "  msr hcr_el2, x1\n"
    "  msr vttbr_el2, xzr\n"
    "  msr cptr_el2, xzr\n"
    "  msr mdcr_el2, xzr\n"
    "  mov x1, #3\n"
    "  msr cnthctl_el2, x1\n"
    "  msr cntvoff_el2, xzr\n"
    "  movz x1, #0x0800\n"
    "  movk x1, #0x30d0, lsl #16\n"
    "  msr sctlr_el1, x1\n"
    "  isb\n"
    "  adr x1, 1f\n"
    "  msr elr_el2, x1\n"
    "  mov x1, #0x3c5\n"
    "  msr spsr_el2, x1\n"
    "  eret\n"
    "1:\n"
    "  mov x1, #(3 << 20)\n"
    "  msr cpacr_el1, x1\n"
    "  mrs x1, sctlr_el1\n"
    "  bic x1, x1, #0x1\n"
    "  bic x1, x1, #0x4\n"
    "  bic x1, x1, #0x1000\n"
    "  msr sctlr_el1, x1\n"
    "  isb\n"
    "  adrp x2, cpu1_params\n"
    "  add  x2, x2, :lo12:cpu1_params\n"
    "  mov  x3, #1\n"
    "  str  x3, [x2, #16]\n"
    "  dsb  sy\n"
    "2:\n"
    "  ldr  x3, [x2, #24]\n"
    "  cbnz x3, 3f\n"
    "  wfe\n"
    "  b    2b\n"
    "3:\n"
    "  ldp  x16, x0, [x2]\n"
    "  br   x16\n"
);

static int64_t psci_cpu_on(uint64_t target_mpidr, uint64_t entry_pa, uint64_t ctx) {
    register uint64_t x0 __asm__("x0") = 0xC4000003ULL;
    register uint64_t x1 __asm__("x1") = target_mpidr;
    register uint64_t x2 __asm__("x2") = entry_pa;
    register uint64_t x3 __asm__("x3") = ctx;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    return (int64_t)x0;
}

static size_t cmd_append(char *dst, size_t pos, const char *s) {
    if (!s || !*s) return pos;
    if (pos > 0 && dst[pos - 1] != ' ' && pos + 1 < BOOT_LINE_LENGTH) {
        dst[pos++] = ' ';
    }
    while (*s && pos + 1 < BOOT_LINE_LENGTH) {
        dst[pos++] = *s++;
    }
    dst[pos] = 0;
    return pos;
}

static size_t cmd_append_hex(char *dst, size_t pos, const char *prefix, uint64_t val) {
    static const char hex[] = "0123456789abcdef";
    pos = cmd_append(dst, pos, prefix);
    int started = 0;
    for (int sh = 60; sh >= 0 && pos + 1 < BOOT_LINE_LENGTH; sh -= 4) {
        unsigned d = (unsigned)((val >> sh) & 0xf);
        if (d || started || sh == 0) {
            dst[pos++] = hex[d];
            started = 1;
        }
    }
    dst[pos] = 0;
    return pos;
}

static uint32_t parse_cmd_u32(const char *line, const char *key) {
    size_t klen = 0;
    while (key[klen]) klen++;
    for (const char *p = line; *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        const char *t = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '=') p++;
        if ((size_t)(p - t) == klen && str_eq_n(t, key, klen) && *p == '=') {
            p++;
            uint32_t v = 0;
            while (*p >= '0' && *p <= '9') {
                v = v * 10u + (uint32_t)(*p - '0');
                p++;
            }
            return v;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return 0;
}

efi_status EFIAPI efi_main(efi_handle image, efi_system_table *st) {
    (void)image;
    ST = st;
    mem_zero(loader_log, sizeof(loader_log));
    loader_log_len = 0;

    /* Enable FP/SIMD on Core 0 just in case */
    {
        uint64_t cpacr = 3ULL << 20;
        __asm__ volatile("msr cpacr_el1, %0; isb" :: "r"(cpacr));
    }

    logdump_init();
    con_puts("=== A21S iOS 27 LOADER v2 (Exynos 850 / xnu-12377.121.6) ===\n");
    con_hex("log_bio=", (uint64_t)(uintptr_t)log_bio);
    logdump_flush();

    const struct xnup_header *xh = (const struct xnup_header *)(uintptr_t)PAYLOAD_BASE;
    if (xh->magic != XNUP_MAGIC) {
        con_hex("FATAL: bad XNUP magic=", xh->magic);
        swreset_reboot();
    }

    uint64_t walk_len = xh->size < PAYLOAD_MAX_SCAN ? xh->size : PAYLOAD_MAX_SCAN;
    const uint8_t *p = (const uint8_t *)(xh + 1);
    const uint8_t *pend = p + walk_len;
    const struct sweet_payload *sp = NULL;
    uint64_t sp_len = 0;
    const char *extra_cmdline = NULL;

    while (p + sizeof(struct xnup_entry) <= pend) {
        const struct xnup_entry *e = (const struct xnup_entry *)p;
        if (e->name[0] == 0) break;
        if ((uint64_t)e->size > (uint64_t)(pend - (p + sizeof(*e)))) {
            con_puts("FATAL: truncated XNUP entry\n");
            swreset_reboot();
        }
        const uint8_t *edata = p + sizeof(*e);
        if (str_eq_n(e->name, "sweetxnu", 16)) {
            sp = (const struct sweet_payload *)edata;
            sp_len = e->size;
        } else if (str_eq_n(e->name, "cmdline", 16)) {
            extra_cmdline = (const char *)edata;
        }
        p += sizeof(*e) + (((uint64_t)e->size + 15ULL) & ~15ULL);
    }

    if (!sp || sp_len < sizeof(*sp)) {
        con_puts("FATAL: missing sweetxnu entry in XNUP\n");
        swreset_reboot();
    }
    for (int i = 0; i < 8; i++) {
        if (sp->magic[i] != "SWEETXNU"[i]) {
            con_puts("FATAL: bad SWEETXNU magic\n");
            swreset_reboot();
        }
    }
    if (sp->version != 1 || sp->nsegs == 0 || sp->nsegs > 32) {
        con_hex("FATAL: bad SWEETXNU version/nsegs=", sp->nsegs);
        swreset_reboot();
    }

    uint64_t phys_base = sp->phys_base;
    uint64_t mem_size = sp->mem_size;
    uint64_t kernel_end_pa = phys_base + (sp->kernel_end - sp->virt_base);
    uint64_t adt_pa = (kernel_end_pa + PAGE_16K - 1) & ~(PAGE_16K - 1);
    uint64_t ba_pa = (adt_pa + sp->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
    uint64_t top_pa = ba_pa + PAGE_16K;

    if (phys_base != BANK2_BASE || top_pa > BANK2_STAGE1_LIMIT) {
        con_hex("FATAL: kernel layout outside stage1 Bank2 mapping top=", top_pa);
        swreset_reboot();
    }

    a21s_input_init();

    con_hex("Copying SWEETXNU phys_base=", phys_base);
    const uint8_t *sp_bytes = (const uint8_t *)sp;
    for (uint32_t i = 0; i < sp->nsegs; i++) {
        const struct sweet_seg *s = &sp->segs[i];
        if (s->vmaddr < sp->virt_base || s->file_size > s->vmsize ||
            s->file_off > sp_len || s->file_size > sp_len - s->file_off) {
            con_hex("FATAL: bad segment index=", i);
            swreset_reboot();
        }
        uint64_t dst = phys_base + (s->vmaddr - sp->virt_base);
        if (dst + s->vmsize > BANK2_STAGE1_LIMIT) {
            con_hex("FATAL: segment exceeds stage1 mapping dst=", dst);
            swreset_reboot();
        }
        mem_copy((void *)(uintptr_t)dst, sp_bytes + s->file_off, (size_t)s->file_size);
        mem_zero((void *)(uintptr_t)(dst + s->file_size), (size_t)(s->vmsize - s->file_size));
        clean_dcache(dst, dst + s->vmsize);
    }

    mem_copy((void *)(uintptr_t)adt_pa, sp_bytes + sp->adt_off, (size_t)sp->adt_size);
    mem_zero((void *)(uintptr_t)ba_pa, PAGE_16K);

    struct boot_args *ba = (struct boot_args *)(uintptr_t)ba_pa;
    ba->Revision = 2;
    ba->Version = 2;
    ba->virtBase = sp->virt_base;
    ba->physBase = phys_base;
    ba->memSize = mem_size;
    ba->topOfKernelData = top_pa;
    ba->deviceTreeP = sp->virt_base + (adt_pa - phys_base);
    ba->deviceTreeLength = (uint32_t)sp->adt_size;
    ba->memSizeActual = mem_size;

    size_t cpos = 0;
    ba->CommandLine[0] = 0;
    cpos = cmd_append_hex(ba->CommandLine, cpos, "a21s_mbox=0x", (uint64_t)(uintptr_t)&a21s_mbox);
    cpos = cmd_append(ba->CommandLine, cpos, sp->cmdline);
    if (extra_cmdline && extra_cmdline[0]) {
        cpos = cmd_append(ba->CommandLine, cpos, extra_cmdline);
    }

    clean_dcache(adt_pa, top_pa);
    __asm__ volatile("ic iallu; dsb sy; isb" ::: "memory");

    *(volatile uint32_t *)(uintptr_t)PMSG_BASE_PA = 0;
    clean_dcache(PMSG_BASE_PA, PMSG_BASE_PA + 64);
    pmsg_ready = 1;

    uint64_t entry_pa = phys_base + (sp->entry_va - sp->virt_base);
    cpu1_params.entry_pa = entry_pa;
    cpu1_params.ba_pa = ba_pa;
    cpu1_params.stage = 0;
    cpu1_params.go = 0;
    clean_dcache((uint64_t)(uintptr_t)&cpu1_params, (uint64_t)(uintptr_t)(&cpu1_params + 1));
    clean_dcache((uint64_t)(uintptr_t)&a21s_cpu1_entry, (uint64_t)(uintptr_t)&a21s_cpu1_entry + 256);
    __asm__ volatile("ic iallu; dsb sy; isb" ::: "memory");

    con_hex("entry_pa=", entry_pa);
    con_hex("boot_args_pa=", ba_pa);
    con_hex("a21s_mbox_pa=", (uint64_t)(uintptr_t)&a21s_mbox);
    con_puts("cmdline: ");
    con_puts(ba->CommandLine);
    con_puts("\nCalling PSCI_CPU_ON(0x001)...\n");
    logdump_flush();

    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)GICC_CTLR_PA = 0;
    __asm__ volatile("dsb sy; isb" ::: "memory");

    int64_t rc = psci_cpu_on(0x001ULL, (uint64_t)(uintptr_t)&a21s_cpu1_entry, 0);
    if (rc != 0) {
        con_hex("FATAL: PSCI_CPU_ON(0x001) failed rc=", (uint64_t)rc);
        swreset_reboot();
    }

    uint64_t deadline = read_cntvct() + 2ULL * timer_hz();
    while (read_cntvct() < deadline) {
        inval_dcache((uint64_t)(uintptr_t)&cpu1_params, (uint64_t)(uintptr_t)(&cpu1_params + 1));
        if (cpu1_params.stage != 0) break;
    }
    if (cpu1_params.stage == 0) {
        con_puts("FATAL: CPU 1 never reached a21s_cpu1_entry within 2s\n");
        swreset_reboot();
    }

    /* Flush final loader log to eMMC BEFORE releasing CPU 1 to XNU so dwmmc0 has zero contention */
    con_puts("CPU 1 awake at EL1 trampoline! Releasing CPU 1 to XNU _start now.\n");
    logdump_flush();
    log_bio = NULL;

    cpu1_params.go = 1;
    clean_dcache((uint64_t)(uintptr_t)&cpu1_params, (uint64_t)(uintptr_t)(&cpu1_params + 1));
    __asm__ volatile("dsb sy; sev" ::: "memory");

    uint32_t auto_sec = parse_cmd_u32(ba->CommandLine, "a21s_autoreboot");
    uint64_t auto_deadline = auto_sec ? (read_cntvct() + (uint64_t)auto_sec * timer_hz()) : 0;

    for (;;) {
        a21s_input_poll();
        if (auto_deadline && read_cntvct() >= auto_deadline) {
            *(volatile uint32_t *)(uintptr_t)PMU_SWRESET_PA = 2U;
            __asm__ volatile("dsb sy; isb" ::: "memory");
        }
    }
}
