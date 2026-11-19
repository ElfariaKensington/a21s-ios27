/* Chain-load an Android boot image (header v0-v2) from an eMMC partition the way S-Boot would:
 * kernel at DRAM base + text_offset, ramdisk beside it, device tree patched with the ramdisk range. */
#include "a21s.h"
#include "libfdt/libfdt.h"

static u64 align_up(u64 v, u64 a) { return (v + a - 1) / a * a; }

struct abih {            /* boot image header (v0..v2 common prefix) */
    u8 magic[8]; u32 ksize, kaddr, rsize, raddr, ssize, saddr, taddr, pagesize, hdrver, osver;
    u8 name[16]; u8 cmdline[512]; u32 id[8]; u8 extra[1024];
    u32 rdtbo_size; u64 rdtbo_off; u32 hdr_size; u32 dtb_size; u64 dtb_addr;
} __attribute__((packed));

static void fdt_edit_bootargs(void *fdt, int chosen, const char *const *drops, const char *add) {
    int len; const char *old = fdt_getprop(fdt, chosen, "bootargs", &len);
    static char nb[4096]; int n = 0; nb[0] = 0;
    if (old) {
        const char *p = old;
        while (*p) {
            while (*p == ' ') p++;
            const char *e = p; while (*e && *e != ' ') e++;
            int tl = e - p;
            int skip = 0;
            for (int k = 0; drops && drops[k]; k++) if (!strncmp(p, drops[k], strlen(drops[k]))) skip = 1;
            if (!skip && tl && n + tl + 2 < (int)sizeof(nb)) { memcpy(nb + n, p, tl); n += tl; nb[n++] = ' '; }
            p = e;
        }
    }
    for (const char *a = add; a && *a && n + 2 < (int)sizeof(nb); a++) nb[n++] = *a;
    nb[n] = 0;
    fdt_setprop_string(fdt, chosen, "bootargs", nb);
}

/* append a tiny uncompressed newc cpio carrying our boot log; the kernel unpacks concatenated archives in order */
static u32 cpio_hex(char *d, u32 v) { static const char h[] = "0123456789ABCDEF"; for (int i = 7; i >= 0; i--) { d[i] = h[v & 15]; v >>= 4; } return 8; }
static u64 append_cpio(u8 *dst, const char *name, const void *data, u32 dlen) {
    u8 *p = dst;
    u32 nl = strlen(name) + 1;
    memcpy(p, "070701", 6); p += 6;
    u32 f[13] = { 1, 0100644, 0, 0, 1, 0, dlen, 0, 0, 0, 0, nl, 0 };
    for (int i = 0; i < 13; i++) { cpio_hex((char *)p, f[i]); p += 8; }
    memcpy(p, name, nl); p += nl;
    while ((u64)(p - dst) & 3) *p++ = 0;
    memcpy(p, data, dlen); p += dlen;
    while ((u64)(p - dst) & 3) *p++ = 0;
    /* trailer */
    const char *tn = "TRAILER!!!"; u32 tl = strlen(tn) + 1;
    u8 *t = p;
    memcpy(p, "070701", 6); p += 6;
    u32 g[13] = { 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, tl, 0 };
    for (int i = 0; i < 13; i++) { cpio_hex((char *)p, g[i]); p += 8; }
    memcpy(p, tn, tl); p += tl;
    while ((u64)(p - t) & 3) *p++ = 0;
    while ((u64)(p - dst) & 511) *p++ = 0;
    return p - dst;
}

/* Android path: S-Boot already loaded kernel+ramdisk; add our log to the ramdisk (concatenated cpio) and fix initrd-end in place */
void append_log_to_loaded_ramdisk(void *dtb) {
    if (fdt_check_header(dtb)) return;
    int chosen = fdt_path_offset(dtb, "/chosen");
    if (chosen < 0) return;
    int l1; const void *ps_ = fdt_getprop(dtb, chosen, "linux,initrd-start", &l1);
    int l2; const void *pe_ = fdt_getprop(dtb, chosen, "linux,initrd-end", &l2);
    if (!ps_ || !pe_ || l1 != l2 || (l1 != 4 && l1 != 8)) return;
    u64 end = l2 == 8 ? fdt64_to_cpu(*(const fdt64_t *)pe_) : fdt32_to_cpu(*(const fdt32_t *)pe_);
    end = (end + 3) & ~3ULL;
    static char lg[40000]; u32 ll = loglen > sizeof(lg) ? sizeof(lg) : loglen; memcpy(lg, logbuf, ll);
    u64 extra = append_cpio((u8 *)end, "a21s-boot.log", lg, ll);
    end += extra;
    if (l2 == 8) fdt_setprop_inplace_u64(dtb, chosen, "linux,initrd-end", end);
    else fdt_setprop_inplace_u32(dtb, chosen, "linux,initrd-end", (u32)end);
}

void jump_kernel(u64 entry, u64 dtb) {
    asm volatile("dsb sy\n ic iallu\n dsb sy\n isb\n"
                 "mov x0, %1\n mov x1, xzr\n mov x2, xzr\n mov x3, xzr\n br %0\n" :: "r"(entry), "r"(dtb) : "x0", "x1", "x2", "x3");
    for (;;) ;
}

int boot_android_image(struct part *p, int recovery_mode, void *dtb) {
    static u8 hbuf[4096];
    int rc = mmc_read(p->first, 8, hbuf);
    if (rc) { kprintf("boot: header read rc=%d\n", rc); return rc; }
    struct abih *h = (struct abih *)hbuf;
    if (memcmp(h->magic, "ANDROID!", 8)) { kprintf("boot: %s has no Android image (%08x)\n", p->name, *(u32 *)hbuf); return -30; }
    u64 ps = h->pagesize;
    u64 koff = ps, roff = koff + align_up(h->ksize, ps), soff = roff + align_up(h->rsize, ps);
    kprintf("boot: %s v%u kernel %u ramdisk %u page %u\n", p->name, h->hdrver, h->ksize, h->rsize, h->pagesize);
    if ((u64)h->ksize > 0x3c00000) { kprintf("boot: kernel too large\n"); return -31; }
    static char cmdline[1600]; memcpy(cmdline, h->cmdline, 512); cmdline[512] = 0;
    kprintf("boot: loading kernel...\n");
    rc = mmc_read(p->first + koff / 512, (h->ksize + 511) / 512, (void *)KERNEL_ADDR);
    if (rc) { kprintf("boot: kernel read rc=%d\n", rc); return rc; }
    kprintf("boot: loading ramdisk...\n");
    rc = mmc_read(p->first + roff / 512, (h->rsize + 511) / 512, (void *)RAMDISK_ADDR);
    if (rc) { kprintf("boot: ramdisk read rc=%d\n", rc); return rc; }
    (void)soff;
    u64 rend = RAMDISK_ADDR + h->rsize;
    rend = align_up(rend, 4);
    /* log -> /a21s-boot.log in the ramdisk */
    static char lg[40000]; u32 ll = loglen > sizeof(lg) ? sizeof(lg) : loglen; memcpy(lg, logbuf, ll);
    u64 extra = append_cpio((u8 *)rend, "a21s-boot.log", lg, ll);
    rend += extra;

    int er = fdt_open_into(dtb, (void *)DTB_WORK, 0x400000);
    if (er) { kprintf("boot: fdt_open_into %d\n", er); return er; }
    void *fdt = (void *)DTB_WORK;
    int chosen = fdt_path_offset(fdt, "/chosen");
    if (chosen < 0) { kprintf("boot: no /chosen\n"); return chosen; }
    int l1, l2;
    const void *ps_ = fdt_getprop(fdt, chosen, "linux,initrd-start", &l1); (void)ps_;
    (void)fdt_getprop(fdt, chosen, "linux,initrd-end", &l2);
    if (l1 == 8) { fdt_setprop_u64(fdt, chosen, "linux,initrd-start", RAMDISK_ADDR); fdt_setprop_u64(fdt, chosen, "linux,initrd-end", rend); }
    else { fdt_setprop_u32(fdt, chosen, "linux,initrd-start", RAMDISK_ADDR); fdt_setprop_u32(fdt, chosen, "linux,initrd-end", rend); }
    if (recovery_mode)
    { static const char *const drops[] = { "androidboot.selinux=", "bootmode=", "root=", 0 };
      static char add[700]; int n = 0; const char *pre = "bootmode=2 root=/dev/ram0 "; while (*pre) add[n++] = *pre++;
      for (const char *c = cmdline; *c && n < 680; c++) add[n++] = *c; add[n] = 0;
      fdt_edit_bootargs(fdt, chosen, drops, add); }
    fdt_pack(fdt);
    kprintf("boot: jumping, dtb %p (%u bytes) initrd %p-%p\n", fdt, fdt_totalsize(fdt), (void *)RAMDISK_ADDR, (void *)rend);
    log_flush();
    mdelay(300);
    jump_kernel(KERNEL_ADDR, (u64)fdt);
}
