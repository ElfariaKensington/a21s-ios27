#include "a21s.h"

static u64 dtb_ptr;
#ifndef DEFAULT_ENTRY
#define DEFAULT_ENTRY 1           /* 0 Android, 1 Recovery, 2 iOS: debug builds default to recovery */
#endif
#define COUNTDOWN_MS 8000
#define FLAG_SECTOR ((37748736UL - 129 * 512) / 512)

#define HDR ((volatile u32 *)0xA8000000UL)
static int embedded(void) { return HDR[9] == 0x44424d45; }
/* Android kernel is already where S-Boot put it; only its first instruction was redirected to us */
static void boot_android_embedded(u64 dtb) {
    log_flush();
    kprintf("Android: restoring kernel entry %08x, jumping\n", HDR[8]);
    append_log_to_loaded_ramdisk((void *)dtb);
    mdelay(200);
    *(volatile u32 *)KERNEL_ADDR = HDR[8];
    jump_kernel(KERNEL_ADDR, dtb);
}

int boot_ios(void *dtb) {
    (void)dtb;
    if (ios_info()) { kprintf("iOS is not installed.\n"); return -1; }
    struct map_file *pl = ios_file("payload");
    if (!pl) { kprintf("iOS: no payload file in the map\n"); return -1; }
    static u8 first[4096];
    int rc = ios_read(pl, 0, 4096, first);
    if (rc) { kprintf("iOS: payload read rc=%d\n", rc); return rc; }
    kprintf("iOS payload head: %c%c%c%c size %u\n", first[0], first[1], first[2], first[3], *(u32 *)(first + 4));
    u32 total = *(u32 *)(first + 4) + 8;
    if (first[0] != 'X' || total > 0x4000000) { kprintf("iOS: bad payload head\n"); return -1; }
    kprintf("iOS: loading payload (%u bytes) to F0000000...\n", total);
    u32 rd = (total + 511) & ~511u;
    rc = ios_read(pl, 0, rd, (void *)0xF0000000UL);
    if (rc) { kprintf("iOS: payload load rc=%d\n", rc); return rc; }
    kprintf("iOS: payload loaded, crc32 first MiB %08x\n", crc32((void *)0xF0000000UL, 1 << 20));
    touch_probe();
    log_flush();
    rc = efi_boot(dtb, (const u8 *)0xF0000020UL, ios_file("system"));
    kprintf("iOS: efi_boot returned %d\n", rc);
    return -1;
}


/* bench (one-shot flag 3): what the processor and the eMMC do here, for the log area; the first stage runs with its caches off */
static void bench(void) {
    log_set_dest(71552);
    kprintf("bench start\n"); log_flush();
    struct part *p = gpt_find("userdata");
    u8 *buf = (u8 *)0xF0000000UL;
    u64 t0 = ms_now(), n = 200000000ULL;
    asm volatile("1: subs %0, %0, #1\n b.ne 1b" : "+r"(n) : : "cc");
    u64 t1 = ms_now();
    kprintf("cpu: 200M loop iterations in %lu ms\n", t1 - t0); log_flush();
    if (!p) { kprintf("no userdata\n"); log_flush(); return; }
    t0 = ms_now();
    for (int i = 0; i < 64; i++) if (mmc_read(p->first + 1000000 + (u64)i * 2048, 2048, buf)) { kprintf("seq read failed at %d\n", i); break; }
    t1 = ms_now();
    kprintf("eMMC sequential: 64 MB in %lu ms\n", t1 - t0); log_flush();
    u64 x = 88172645463325252ULL;
    t0 = ms_now();
    for (int i = 0; i < 400; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; mmc_read(p->first + ((x % 40000000ULL) & ~7ULL), 8, buf); }
    t1 = ms_now();
    kprintf("eMMC random 4 KB: 400 reads in %lu ms\n", t1 - t0); log_flush();
    t0 = ms_now();
    for (int i = 0; i < 200; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; mmc_read(p->first + ((x % 40000000ULL) & ~127ULL), 128, buf); }
    t1 = ms_now();
    kprintf("eMMC random 64 KB: 200 reads in %lu ms\n", t1 - t0); log_flush();
    t0 = ms_now();
    for (int i = 0; i < 200; i++) { u64 *d = (u64 *)(buf + 0x100000), *sr = (u64 *)buf; for (int k = 0; k < 131072; k++) d[k] = sr[k]; }
    t1 = ms_now();
    kprintf("memcpy 1 MB x 200 in %lu ms\n", t1 - t0); log_flush();
    log_set_dest((37748736UL - 2 * 65536) / 512);
}

static void show_gpt(void) {
    kprintf("GPT: %d partitions\n", gpt_count());
    for (int i = 0; i < gpt_count(); i++) {
        struct part *p = gpt_get(i);
        u64 mb = (p->last - p->first + 1) / 2048;
        kprintf("%-13s %6lu MiB", p->name, mb);
        kputs((i & 1) ? "\n" : "  ");
    }
    kputs("\n");
}

void stage1_main(u64 dtb, u64 el) {
    dtb_ptr = dtb;
    con_init();
    con_color(0xff40ff80, 0xff000000);
    kprintf("A21s dual-boot stage 1\n");
    con_color(0xffffffff, 0xff000000);
    { u64 sc; if (el == 2) asm volatile("mrs %0, sctlr_el2" : "=r"(sc)); else asm volatile("mrs %0, sctlr_el1" : "=r"(sc)); kprintf("EL%lu sctlr=%lx dtb=%p\nfreq=", el, sc, (void *)dtb); }
    { u64 f; asm volatile("mrs %0, cntfrq_el0" : "=r"(f)); kprintf("%lu\n", f); }
    kprintf("fdt magic %08x size %u\n", *(u32 *)dtb, __builtin_bswap32(*(u32 *)(dtb + 4)));
    mmc_dump();
    if (mmc_init()) kprintf("mmc: card did not answer CMD13\n");
    int n = gpt_load();
    if (n > 0) show_gpt(); else kprintf("gpt: failed (%d)\n", n);

    /* colour swatch so the channel order is visible on a photo: R G B */
    static const u32 sw[3] = { 0xffff0000, 0xff00ff00, 0xff0000ff };
    for (int k = 0; k < 3; k++)
        for (int y = 0; y < 24; y++)
            for (int x = 0; x < 60; x++) {
                volatile u32 *p = (volatile u32 *)(FB_BASE + (u64)(FB_H - 40 + y) * FB_STRIDE + (u64)(20 + k * 70 + x) * 4);
                *p = sw[k]; *(volatile u32 *)((u64)p + FB_HALF) = sw[k];
            }

    size_t menu_log_mark = loglen;
    static const char *names[3] = { "Android", "Recovery (OrangeFox)", "iOS" };
    static const char *parts_for[3] = { "boot_aosp", "recovery", "boot" };
    int sel = DEFAULT_ENTRY;
    {   /* one-shot override written from Android/recovery (tools/boot-once.sh): cleared before it is used, so a hang or reset loop cannot repeat */
        struct part *b = gpt_find("boot");
        static u8 fl[512];
        if (b && !mmc_read(b->first + FLAG_SECTOR, 1, fl) && !memcmp(fl, "A21SBOOT", 8) && fl[8] < 4) {
            sel = fl[8] < 3 ? fl[8] : 0;
            int test = fl[8] == 3;
            memset(fl, 0, 512);
            mmc_write(b->first + FLAG_SECTOR, 1, fl);
            kprintf("one-shot boot flag: %d\n", sel);
            if (test) { bench(); menu_log_mark = loglen; }
        }
    }
    kprintf("\nVol+ = next   Vol- = boot   (auto in %d s)\n", COUNTDOWN_MS / 1000);
    u64 t0 = ms_now(); int lastsec = -1, lastup = 0, lastdn = 0;
    for (;;) {
        u64 el_ms = ms_now() - t0;
        int sec = (COUNTDOWN_MS - (int)el_ms) / 1000 + 1;
        int up = key_volup(), dn = key_voldown();
        if (up && !lastup) { sel = (sel + 1) % 3; t0 = ms_now() + 4000; sec = -1; }
        if (dn && !lastdn) break;
        lastup = up; lastdn = dn;
        if (el_ms >= COUNTDOWN_MS) break;
        if (sec != lastsec || up || dn) {
            lastsec = sec;
            int c = 0, r = 30;
            con_at(c, r);
            for (int i = 0; i < 3; i++) {
                struct part *p = gpt_find(parts_for[i]);
                if (i == 0 && embedded()) p = gpt_get(0);
                con_color(i == sel ? 0xff000000 : 0xffffffff, i == sel ? 0xffffffff : 0xff000000);
                kprintf(" %s %-22s %s ", i == sel ? ">" : " ", names[i], p ? "" : "(missing)");
                con_color(0xffffffff, 0xff000000);
                kputs("\n");
            }
            kprintf("\nkeys: up=%d dn=%d  gpa0=%02x gpa1=%02x  t=%d   \n", up, dn, r32(GPIO_ALIVE + 4) & 0xff, r32(GPIO_ALIVE + 0x24) & 0xff, sec < 0 ? 0 : sec);
            loglen = menu_log_mark;
        }
    }
    kprintf("\nBooting: %s\n", names[sel]);
    struct part *p = gpt_find(parts_for[sel]);
    if (sel == 0 && embedded()) boot_android_embedded(dtb);
    if (sel == 2) { boot_ios((void *)dtb); }
    else if (!p) kprintf("partition %s not found\n", parts_for[sel]);
    else boot_android_image(p, sel == 1, (void *)dtb);
    kprintf("\nBoot failed. Falling back to Android in 10 s.\n");
    log_flush();
    mdelay(10000);
    if (embedded()) boot_android_embedded(dtb);
    kprintf("No embedded Android; halting. Hold Vol Up + Power to reach recovery.\n");
    for (;;) asm volatile("wfe");
}
