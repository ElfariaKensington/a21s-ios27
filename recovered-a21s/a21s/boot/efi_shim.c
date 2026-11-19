/* A tiny UEFI environment, just enough for core/xnuloader/loader.efi: system/boot/runtime services, a memory map built
 * from S-Boot's device tree, a GOP on S-Boot's framebuffer, the device tree as a configuration table, a PE loader and
 * an identity-mapped MMU (the loader expects UEFI's page tables to be on). No protocol the loader does not need on the
 * path to its kernel start is provided; the rest answer NOT_FOUND. */
#include "a21s.h"
#include "libfdt/libfdt.h"
#include "../../core/xnuloader/efi.h"

#define EFI_SUCCESS 0ULL
#define EFI_NOT_FOUND (EFI_ERROR_BIT | 14)
#define EFI_UNSUPPORTED (EFI_ERROR_BIT | 3)
#define EFI_OUT_OF_RESOURCES (EFI_ERROR_BIT | 9)
#define EFI_INVALID_PARAMETER (EFI_ERROR_BIT | 2)
enum { T_RESERVED = 0, T_LOADER_CODE = 1, T_LOADER_DATA = 2, T_CONV = 7 };

/* ---------------- memory map ---------------- */
struct mr { u64 s, e; u32 t; };
static struct mr mm[160]; static int nmm;
static u64 map_key = 1;

static void mm_overlay(u64 s, u64 e, u32 t) {
    if (s >= e) return;
    static struct mr tmp[160]; int n = 0;
    for (int i = 0; i < nmm; i++) {
        struct mr r = mm[i];
        if (r.e <= s || r.s >= e) { tmp[n++] = r; continue; }
        if (r.s < s) { tmp[n].s = r.s; tmp[n].e = s; tmp[n++].t = r.t; }
        if (r.e > e) { tmp[n].s = e; tmp[n].e = r.e; tmp[n++].t = r.t; }
    }
    tmp[n].s = s; tmp[n].e = e; tmp[n++].t = t;
    for (int i = 1; i < n; i++) { struct mr k = tmp[i]; int j = i - 1; while (j >= 0 && tmp[j].s > k.s) { tmp[j + 1] = tmp[j]; j--; } tmp[j + 1] = k; }
    nmm = 0;
    for (int i = 0; i < n; i++) {
        if (nmm && mm[nmm - 1].t == tmp[i].t && mm[nmm - 1].e == tmp[i].s) mm[nmm - 1].e = tmp[i].e;
        else if (nmm < 160) mm[nmm++] = tmp[i];
    }
    map_key++;
}

static u64 rd_cells(const fdt32_t *p, int cells) { u64 v = 0; for (int i = 0; i < cells; i++) v = (v << 32) | fdt32_to_cpu(p[i]); return v; }

static void mm_init(const void *fdt) {
    int ac = fdt_address_cells(fdt, 0), sc = fdt_size_cells(fdt, 0);
    if (ac < 1 || ac > 2 || sc < 1 || sc > 2) { ac = 2; sc = 2; }
    nmm = 0;
    int off;
    fdt_for_each_subnode(off, fdt, 0) {
        const char *nm = fdt_get_name(fdt, off, 0);
        if (!nm || strncmp(nm, "memory", 6)) continue;
        int l; const fdt32_t *reg = fdt_getprop(fdt, off, "reg", &l);
        if (!reg) continue;
        for (int k = 0; k + (ac + sc) * 4 <= l; k += (ac + sc) * 4) {
            const fdt32_t *p = reg + k / 4;
            u64 b = rd_cells(p, ac), z = rd_cells(p + ac, sc);
            mm_overlay(b, b + z, T_CONV);
        }
    }
    int rm = fdt_path_offset(fdt, "/reserved-memory");
    if (rm >= 0) fdt_for_each_subnode(off, fdt, rm) {
        int l; const fdt32_t *reg = fdt_getprop(fdt, off, "reg", &l);
        if (!reg) continue;
        for (int k = 0; k + (ac + sc) * 4 <= l; k += (ac + sc) * 4) {
            const fdt32_t *p = reg + k / 4;
            u64 b = rd_cells(p, ac), z = rd_cells(p + ac, sc);
            mm_overlay(b, b + z, T_RESERVED);
        }
    }
    for (int i = 0; i < fdt_num_mem_rsv(fdt); i++) { u64 a, z; if (!fdt_get_mem_rsv(fdt, i, &a, &z)) mm_overlay(a, a + z, T_RESERVED); }
    /* what this first stage and S-Boot's loads occupy */
    mm_overlay(0x80000000UL, 0x82E00000UL, T_RESERVED);   /* Android kernel as S-Boot loaded it (kept for the fall-back) */
    mm_overlay(0x84080000UL, 0x85600000UL, T_RESERVED);   /* ramdisk */
    mm_overlay(0x8A000000UL, 0x8A600000UL, T_RESERVED);   /* S-Boot's device tree */
    mm_overlay(0xA0000000UL, 0xA1000000UL, T_RESERVED);   /* scratch */
    mm_overlay(0xA1000000UL, 0xA1A00000UL, T_LOADER_CODE); /* loader.efi */
    mm_overlay(0xA8000000UL, 0xB0000000UL, T_RESERVED);   /* first stage: image, stack, device tree copy, heap */
    /* what the loader claims for itself or its payload: free for it to allocate (the DT reserves them for Samsung's debug) */
    mm_overlay(0xF0000000UL, 0xF4000000UL, T_CONV);       /* the payload's home */
    mm_overlay(0xFA000000UL, 0xFB400000UL, T_CONV);       /* the panel's buffer: the loader keeps XNU out of it */
    mm_overlay(0xFD000000UL, 0xFD200000UL, T_CONV);
    mm_overlay(0x91200000UL, 0x91400000UL, T_CONV);
    /* a 2 MiB page table granularity: shrink every free range inward, the edges become reserved */
    static struct mr c[160]; int n = 0;
    for (int i = 0; i < nmm; i++) c[n++] = mm[i];
    for (int i = 0; i < n; i++) if (c[i].t == T_CONV) {
        u64 s = (c[i].s + 0x1FFFFF) & ~0x1FFFFFULL, e = c[i].e & ~0x1FFFFFULL;
        if (s >= e) { mm_overlay(c[i].s, c[i].e, T_RESERVED); continue; }
        if (c[i].s < s) mm_overlay(c[i].s, s, T_RESERVED);
        if (c[i].e > e) mm_overlay(e, c[i].e, T_RESERVED);
    }
}

/* ---------------- boot services ---------------- */
static u8 *pool_next = (u8 *)0xAD000000UL;
static efi_status EFIAPI s_alloc_pages(int type, int mtype, u64 pages, u64 *addr) {
    u64 sz = pages * 4096;
    if (type == AllocateAddress) {
        u64 a = *addr;
        for (int i = 0; i < nmm; i++) if (mm[i].t == T_CONV && a >= mm[i].s && a + sz <= mm[i].e) { mm_overlay(a, a + sz, mtype); return EFI_SUCCESS; }
        return EFI_NOT_FOUND;
    }
    u64 limit = type == AllocateMaxAddress ? *addr + 1 : ~0ULL;
    for (int i = nmm - 1; i >= 0; i--) {
        if (mm[i].t != T_CONV) continue;
        u64 e = mm[i].e < limit ? mm[i].e : limit;
        if (e < mm[i].s + sz) continue;
        u64 a = (e - sz) & ~0xFFFULL;
        if (a < mm[i].s) continue;
        *addr = a; mm_overlay(a, a + sz, mtype); return EFI_SUCCESS;
    }
    return EFI_OUT_OF_RESOURCES;
}
static efi_status EFIAPI s_free_pages(u64 addr, u64 pages) { mm_overlay(addr, addr + pages * 4096, T_CONV); return EFI_SUCCESS; }
static efi_status EFIAPI s_get_memory_map(u64 *size, void *map, u64 *key, u64 *dsz, u32 *dver) {
    u64 need = (u64)nmm * sizeof(efi_memory_descriptor);
    *dsz = sizeof(efi_memory_descriptor); if (dver) *dver = 1;
    if (*size < need) { *size = need; return EFI_BUFFER_TOO_SMALL; }
    efi_memory_descriptor *d = map;
    for (int i = 0; i < nmm; i++) {
        d[i].type = mm[i].t; d[i].pad = 0; d[i].phys_start = mm[i].s; d[i].virt_start = 0;
        d[i].pages = (mm[i].e - mm[i].s) / 4096; d[i].attribute = mm[i].t == T_RESERVED ? 0 : 0xF;
    }
    *size = need; if (key) *key = map_key;
    return EFI_SUCCESS;
}
static efi_status EFIAPI s_alloc_pool(int type, u64 size, void **buf) {
    pool_next = (u8 *)(((u64)pool_next + 15) & ~15ULL);
    if ((u64)pool_next + size > 0xAE000000UL) return EFI_OUT_OF_RESOURCES;
    *buf = pool_next; pool_next += size; return EFI_SUCCESS;
}
static efi_status EFIAPI s_free_pool(void *b) { return EFI_SUCCESS; }
static efi_status EFIAPI s_signal(void *e) { return EFI_SUCCESS; }
static efi_status EFIAPI s_close_event(void *e) { return EFI_SUCCESS; }
static efi_status EFIAPI s_create_event_ex(u32 t, u64 tpl, void (EFIAPI *n)(void *, void *), const void *c, const efi_guid *g, void **ev) { static u64 dummy; *ev = &dummy; return EFI_SUCCESS; }
static efi_status EFIAPI s_stall(u64 us) { udelay((u32)us); return EFI_SUCCESS; }
static efi_status EFIAPI s_watchdog(u64 t, u64 c, u64 s, char16 *d) { return EFI_SUCCESS; }
static efi_status EFIAPI s_exit_bs(efi_handle i, u64 k) { return EFI_SUCCESS; }

static int guid_is(const efi_guid *a, u32 d1, u16 d2, u16 d3) { return a->a == d1 && a->b == d2 && a->c == d3; }
static efi_gop_mode_info gop_info = { 0, FB_W, FB_H, 1, { 0, 0, 0, 0 }, FB_STRIDE / 4 };
static efi_gop_mode gop_mode = { 1, 0, &gop_info, sizeof(efi_gop_mode_info), FB_BASE, 0x1400000 };
static efi_status EFIAPI gop_query(efi_gop *s, u32 m, u64 *sz, efi_gop_mode_info **i) { *sz = sizeof(gop_info); *i = &gop_info; return EFI_SUCCESS; }
static efi_status EFIAPI gop_set(efi_gop *s, u32 m) { return EFI_SUCCESS; }
static efi_gop gop = { gop_query, gop_set, 0, &gop_mode };

/* The system image as one GPT-less "partition" named SYSTEM (4 KiB blocks), read through the iOS map: the loader's block
 * service (ufs=2) offers it to the kernel as /dev/ufs_SYSTEM. Read only. */
static struct map_file *sysfile;
static u8 sys_handle;
static efi_block_media sys_media;
static efi_status EFIAPI sys_read(efi_block_io *self, u32 media_id, u64 lba, u64 size, void *buf) {
    if (!sysfile || (size & 4095) || (lba * 4096 + size) > ios_size(sysfile)) return EFI_INVALID_PARAMETER;
    return ios_read(sysfile, lba * 4096, size, buf) ? (EFI_ERROR_BIT | 7) : EFI_SUCCESS;
}
static efi_status EFIAPI sys_write(efi_block_io *self, u32 media_id, u64 lba, u64 size, void *buf) { return EFI_ERROR_BIT | 8; }
static efi_status EFIAPI sys_flush(efi_block_io *self) { return EFI_SUCCESS; }
static efi_block_io sys_bio = { 0x00020001, &sys_media, 0, sys_read, sys_write, sys_flush };
static efi_partition_info sys_pinfo;

/* "logdump": where the loader writes its log and the kernel's before it restarts. Here a 1 MiB area of the boot partition's
 * tail (sectors 71552..73470; the first stage's own 63 KiB log follows it, then the flag sector and the iOS map). A write
 * bigger than the area is cut to it instead of refused. */
#define LOG_FIRST 71552UL
#define LOG_SECS  1919UL
static u8 log_handle;
static efi_block_media log_media;
static efi_partition_info log_pinfo;
static efi_status EFIAPI log_read(efi_block_io *self, u32 media_id, u64 lba, u64 size, void *buf) {
    struct part *b = gpt_find("boot"); u64 n = size / 512;
    if (!b || lba + n > LOG_SECS) return EFI_INVALID_PARAMETER;
    return mmc_read(b->first + LOG_FIRST + lba, (u32)n, buf) ? (EFI_ERROR_BIT | 7) : EFI_SUCCESS;
}
static efi_status EFIAPI log_write(efi_block_io *self, u32 media_id, u64 lba, u64 size, void *buf) {
    struct part *b = gpt_find("boot"); u64 n = size / 512;
    if (!b || lba >= LOG_SECS) return EFI_INVALID_PARAMETER;
    if (lba + n > LOG_SECS) n = LOG_SECS - lba;
    return mmc_write(b->first + LOG_FIRST + lba, (u32)n, buf) ? (EFI_ERROR_BIT | 7) : EFI_SUCCESS;
}
static efi_block_io log_bio = { 0x00020001, &log_media, 0, log_read, log_write, sys_flush };

static efi_status find_protocol(efi_handle h, const efi_guid *g, void **iface) {
    if (guid_is(g, 0x9042a9de, 0x23dc, 0x4a38)) { *iface = &gop; return EFI_SUCCESS; }   /* GOP */
    if (h == (efi_handle)&log_handle) {
        if (guid_is(g, 0x964e5b21, 0x6459, 0x11d2)) { *iface = &log_bio; return EFI_SUCCESS; }
        if (guid_is(g, 0x8cf2f62c, 0xbc9b, 0x4821)) { *iface = &log_pinfo; return EFI_SUCCESS; }
    }
    if (h == (efi_handle)&sys_handle && sysfile) {
        if (guid_is(g, 0x964e5b21, 0x6459, 0x11d2)) { *iface = &sys_bio; return EFI_SUCCESS; }      /* Block I/O */
        if (guid_is(g, 0x8cf2f62c, 0xbc9b, 0x4821)) { *iface = &sys_pinfo; return EFI_SUCCESS; }    /* Partition Info */
    }
    return EFI_NOT_FOUND;
}
static efi_status EFIAPI s_handle_protocol(efi_handle h, const efi_guid *g, void **i) { return find_protocol(h, g, i); }
static efi_status EFIAPI s_locate_protocol(const efi_guid *g, void *reg, void **i) { return find_protocol(0, g, i); }
static efi_status EFIAPI s_locate_handle_buffer(int t, const efi_guid *g, void *k, u64 *count, efi_handle **buf) {
    *count = 0;
    if (guid_is(g, 0x964e5b21, 0x6459, 0x11d2) || guid_is(g, 0x8cf2f62c, 0xbc9b, 0x4821)) {
        efi_handle *a; if (s_alloc_pool(2, 2 * sizeof(*a), (void **)&a)) return EFI_OUT_OF_RESOURCES;
        int n = 0; if (sysfile) a[n++] = (efi_handle)&sys_handle;
        a[n++] = (efi_handle)&log_handle; *buf = a; *count = n; return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
}

/* ---------------- console / runtime ---------------- */
static efi_status EFIAPI con_out_string(efi_text_output *s, const char16 *str) {
    for (; *str; str++) { char c = *str < 128 ? (char)*str : '?'; if (c != '\r') con_putc(c); }
    return EFI_SUCCESS;
}
static efi_text_output con_out = { 0, con_out_string };
static efi_status EFIAPI r_get_time(struct efi_time *t, void *cap) { memset(t, 0, sizeof(*t)); t->year = 2026; t->month = 11; t->day = 10; t->timezone = 0x7FF; return EFI_SUCCESS; }
static void psci_reset(void) {
    *(volatile u32 *)0x11863a00UL = 2;      /* the stock kernel's exynos_restart_v1: PMU SYSTEM_CONFIGURATION, SWRESET_SYSTEM */
    asm volatile("dsb sy\n isb");
    mdelay(1000); register u64 x0 asm("x0") = 0x84000009; asm volatile("smc #0" : "+r"(x0) :: "x1", "x2", "x3", "memory"); for (;;) asm volatile("wfe"); }
static void EFIAPI r_reset(int type, efi_status st, u64 sz, void *d) { log_flush(); mdelay(200); psci_reset(); }
static efi_runtime_services rt = { { 0 }, r_get_time, 0, 0, 0, 0, 0, 0, 0, 0, 0, r_reset };

static efi_boot_services bs;
static efi_system_table st;
static struct { efi_guid guid; void *table; } cfg[1];

/* ---------------- MMU ---------------- */
static u64 l1[64] __attribute__((aligned(4096)));
static u64 l2_dev[512] __attribute__((aligned(4096)));    /* 0x00000000-0x3fffffff */
static u64 l2_80[512] __attribute__((aligned(4096)));     /* 0x80000000 */
static u64 l2_c0[512] __attribute__((aligned(4096)));     /* 0xc0000000 */
static u64 l2_hi[512] __attribute__((aligned(4096)));     /* 0x880000000 */
#define PTE_BLK 0x1ULL
#define PTE_AF (1ULL << 10)
#define PTE_SH (3ULL << 8)
#define ATTR_DEV (0ULL << 2)
#define ATTR_WB (1ULL << 2)
#define ATTR_NC (2ULL << 2)
static void map2(u64 *tbl, u64 base, u64 va, u64 len, u64 attr) {
    for (u64 a = va; a < va + len; a += 0x200000) tbl[((a - base) >> 21) & 511] = a | PTE_BLK | PTE_AF | PTE_SH | attr;
}
static void mmu_on(void) {
    memset(l1, 0, sizeof(l1)); memset(l2_dev, 0, 4096); memset(l2_80, 0, 4096); memset(l2_c0, 0, 4096); memset(l2_hi, 0, 4096);
    map2(l2_dev, 0, 0x10000000UL, 0x10000000UL, ATTR_DEV);                    /* the SoC's registers */
    for (int i = 0; i < nmm; i++) {
        if (mm[i].t == T_RESERVED) continue;                                   /* reserved memory stays unmapped */
        for (u64 a = mm[i].s & ~0x1FFFFFULL; a < mm[i].e; a += 0x200000) {
            u64 *tbl = 0, base = 0;
            if (a >= 0x80000000UL && a < 0xC0000000UL) { tbl = l2_80; base = 0x80000000UL; }
            else if (a >= 0xC0000000UL && a < 0x100000000UL) { tbl = l2_c0; base = 0xC0000000UL; }
            else if (a >= 0x880000000UL && a < 0x8C0000000UL) { tbl = l2_hi; base = 0x880000000UL; }
            if (tbl) tbl[((a - base) >> 21) & 511] = a | PTE_BLK | PTE_AF | PTE_SH | ATTR_WB;
        }
    }
    /* anything the first stage itself or the loader's image/scratch needs is mapped, reserved or not */
    map2(l2_80, 0x80000000UL, 0x80000000UL, 0x2E00000UL, ATTR_WB);
    map2(l2_80, 0x80000000UL, 0x84000000UL, 0x1600000UL, ATTR_WB);
    map2(l2_80, 0x80000000UL, 0x8A000000UL, 0x600000UL, ATTR_WB);
    map2(l2_80, 0x80000000UL, 0xA0000000UL, 0x10000000UL, ATTR_WB);
    map2(l2_c0, 0xC0000000UL, 0xFA000000UL, 0x1400000UL, ATTR_NC);            /* the panel's buffer: uncached */
    l1[0] = (u64)l2_dev | 3; l1[2] = (u64)l2_80 | 3; l1[3] = (u64)l2_c0 | 3; l1[34] = (u64)l2_hi | 3;
    u64 mair = 0x00UL | (0xffUL << 8) | (0x44UL << 16);
    u64 tcr = 28 | (1UL << 8) | (1UL << 10) | (3UL << 12) | (0UL << 14) | (1UL << 32);   /* T0SZ 36-bit, WB WA inner/outer, inner shareable, 4K, IPS 36-bit */
    u64 sctlr;
    asm volatile("dsb sy\n msr mair_el1, %0\n msr tcr_el1, %1\n msr ttbr0_el1, %2\n isb\n tlbi vmalle1\n dsb sy\n isb\n"
                 :: "r"(mair), "r"(tcr), "r"((u64)l1) : "memory");
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1 << 0) | (1 << 2) | (1 << 12);       /* M, C, I */
    sctlr &= ~((1UL << 1) | (1UL << 19));           /* no alignment check, no WXN */
    asm volatile("ic iallu\n dsb sy\n isb\n msr sctlr_el1, %0\n isb\n" :: "r"(sctlr) : "memory");
}

extern char vectors[];
void exc_handler(u64 esr, u64 elr, u64 far, u64 lr) {
    kprintf("\nEXCEPTION esr=%lx elr=%lx far=%lx lr=%lx\n", esr, elr, far, lr);
    log_flush();
    mdelay(8000);
    psci_reset();
}

/* ---------------- PE loader ---------------- */
struct pe_sec { char name[8]; u32 vsize, vaddr, rawsize, rawptr, r1, r2; u16 n1, n2; u32 chars; };
static int pe_load(const u8 *img, u64 *entry, u64 *base_out) {
    if (img[0] != 'M' || img[1] != 'Z') return -1;
    u32 pe = *(const u32 *)(img + 0x3c);
    if (memcmp(img + pe, "PE\0\0", 4)) return -2;
    const u8 *coff = img + pe + 4;
    u16 nsec = *(const u16 *)(coff + 2), optsz = *(const u16 *)(coff + 16);
    const u8 *opt = coff + 20;
    if (*(const u16 *)opt != 0x20b) return -3;
    u32 ep = *(const u32 *)(opt + 16);
    u64 base = *(const u64 *)(opt + 24);
    u32 isz = *(const u32 *)(opt + 56), hsz = *(const u32 *)(opt + 60);
    memset((void *)base, 0, isz);
    memcpy((void *)base, img, hsz);
    const struct pe_sec *s = (const struct pe_sec *)(opt + optsz);
    for (int i = 0; i < nsec; i++) memcpy((void *)(base + s[i].vaddr), img + s[i].rawptr, s[i].rawsize < s[i].vsize || !s[i].vsize ? s[i].rawsize : s[i].vsize);
    *entry = base + ep; *base_out = base;
    return 0;
}

int efi_boot(void *dtb, const u8 *pe_image, struct map_file *system) {
    u64 entry, base;
    sysfile = system;
    log_media.media_id = 2; log_media.present = 1; log_media.logical = 1; log_media.block_size = 512; log_media.last_block = LOG_SECS - 1;
    log_pinfo.revision = 0x1000; log_pinfo.type = 2; log_pinfo.end_lba = LOG_SECS - 1;
    { static const char n[] = "logdump"; for (int i = 0; i < 8; i++) log_pinfo.name[i] = n[i]; }
    if (system) {
        sys_media.media_id = 1; sys_media.present = 1; sys_media.logical = 1; sys_media.read_only = 1;
        sys_media.block_size = 4096; sys_media.last_block = ios_size(system) / 4096 - 1;
        sys_pinfo.revision = 0x1000; sys_pinfo.type = 2;
        static const char n[] = "SYSTEM"; for (int i = 0; i < 7; i++) sys_pinfo.name[i] = n[i];
        sys_pinfo.start_lba = 0; sys_pinfo.end_lba = sys_media.last_block;
    }
    int rc = pe_load(pe_image, &entry, &base);
    if (rc) { kprintf("efi: PE load rc=%d\n", rc); return rc; }
    kprintf("efi: loader.efi at %p entry %p\n", (void *)base, (void *)entry);
    mm_init(dtb);
    for (int i = 0; i < nmm; i++) kprintf("  %09lx-%09lx t%u\n", mm[i].s, mm[i].e, mm[i].t);
    memset(&bs, 0, sizeof(bs)); memset(&st, 0, sizeof(st));
    bs.allocate_pages = s_alloc_pages; bs.free_pages = s_free_pages; bs.get_memory_map = s_get_memory_map;
    bs.allocate_pool = s_alloc_pool; bs.free_pool = s_free_pool; bs.signal_event = s_signal; bs.close_event = s_close_event;
    bs.handle_protocol = s_handle_protocol; bs.exit_boot_services = s_exit_bs; bs.stall = s_stall; bs.set_watchdog_timer = s_watchdog;
    bs.locate_handle_buffer = s_locate_handle_buffer; bs.locate_protocol = s_locate_protocol; bs.create_event_ex = s_create_event_ex;
    bs.hdr.signature = 0x56524553544f4f42ULL;
    static const efi_guid dtb_guid = { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };
    cfg[0].guid = dtb_guid; cfg[0].table = dtb;
    static char16 vendor[] = { 'A', '2', '1', 's', 0 };
    st.hdr.signature = 0x5453595320494249ULL; st.vendor = vendor; st.revision = 0x2000a;
    st.con_out = &con_out; st.con_out_handle = 0; st.rt = &rt; st.bs = &bs; st.nr_tables = 1; st.tables = (void *)cfg;
    asm volatile("msr vbar_el1, %0\n isb\n" :: "r"((u64)vectors));
    log_flush();
    mmu_on();
    kprintf("efi: MMU on, calling efi_main\n");
    efi_status (*main_fn)(efi_handle, efi_system_table *) = (void *)entry;
    efi_status r = main_fn((efi_handle)1, &st);
    kprintf("efi: efi_main returned %lx\n", r);
    log_flush();
    return (int)r;
}
