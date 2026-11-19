#include "a21s.h"
#include "font.h"

#define LOGCAP 60000
#define LOGHEAD 6000
char logbuf[LOGCAP]; size_t loglen;
static int cx, cy;
static u32 fg = 0xffffffff, bg = 0xff000000;
#define SCALE 2
#define CW (8 * SCALE)
#define CH (16 * SCALE)
#define COLS (FB_W / CW)
#define ROWS (FB_H / CH)

u64 cnt(void) { u64 v; asm volatile("isb\n mrs %0, cntpct_el0" : "=r"(v)); return v; }
static u64 freq_hz(void) { u64 f; asm volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f ? f : 26000000ULL; }
void udelay(u32 us) { u64 t = cnt() + freq_hz() * us / 1000000ULL; while (cnt() < t) ; }
void mdelay(u32 ms) { while (ms--) udelay(1000); }
u64 ms_now(void) { return cnt() * 1000ULL / freq_hz(); }

static void px(int x, int y, u32 c) {
    volatile u32 *p = (volatile u32 *)(FB_BASE + (u64)y * FB_STRIDE + (u64)x * 4);
    *p = c;
    *(volatile u32 *)((u64)p + FB_HALF) = c;
}
void con_color(u32 f, u32 b) { fg = f; bg = b; }
void con_clear(void) {
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++) px(x, y, bg);
    cx = cy = 0;
}
void con_init(void) { con_clear(); }
void con_at(int c, int r) { cx = c; cy = r; }
static void glyph(int col, int row, char ch) {
    if (ch < 32 || ch > 126) ch = '?';
    const u8 *g = font8x16[ch - 32];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++) {
            u32 c = (g[y] & (0x80 >> x)) ? fg : bg;
            for (int sy = 0; sy < SCALE; sy++)
                for (int sx = 0; sx < SCALE; sx++)
                    px(col * CW + x * SCALE + sx, row * CH + y * SCALE + sy, c);
        }
}
void con_putc(char c) {
    if (loglen >= sizeof(logbuf) - 1) {        /* full: keep the start, drop the oldest 8 KiB after it */
        memmove(logbuf + LOGHEAD, logbuf + LOGHEAD + 8192, loglen - LOGHEAD - 8192);
        loglen -= 8192;
    }
    logbuf[loglen++] = c; logbuf[loglen] = 0;
    if (c == '\n') { cx = 0; cy++; }
    else { glyph(cx, cy, c); if (++cx >= COLS) { cx = 0; cy++; } }
    if (cy >= ROWS) { con_clear(); }
}
void kputs(const char *s) { while (*s) con_putc(*s++); }

static void putnum(u64 v, int base, int width, char pad, int neg) {
    char tmp[24]; int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { int d = v % base; tmp[n++] = d < 10 ? '0' + d : 'a' + d - 10; v /= base; }
    if (neg) tmp[n++] = '-';
    while (n < width) tmp[n++] = pad;
    while (n) con_putc(tmp[--n]);
}
void kprintf(const char *f, ...) {
    va_list ap; va_start(ap, f);
    for (; *f; f++) {
        if (*f != '%') { con_putc(*f); continue; }
        f++;
        char pad = ' '; int width = 0, lng = 0, left = 0;
        if (*f == '-') { left = 1; f++; }
        if (*f == '0') { pad = '0'; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l') { lng++; f++; }
        switch (*f) {
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; int l = strlen(s); if (!left) while (l++ < width) con_putc(' '); kputs(s); if (left) while (l++ < width) con_putc(' '); break; }
        case 'c': con_putc((char)va_arg(ap, int)); break;
        case 'd': { long long v = lng ? va_arg(ap, long long) : va_arg(ap, int); putnum(v < 0 ? -v : v, 10, width, pad, v < 0); break; }
        case 'u': putnum(lng ? va_arg(ap, u64) : va_arg(ap, u32), 10, width, pad, 0); break;
        case 'x': putnum(lng ? va_arg(ap, u64) : va_arg(ap, u32), 16, width, pad, 0); break;
        case 'p': kputs("0x"); putnum(va_arg(ap, u64), 16, 0, '0', 0); break;
        case '%': con_putc('%'); break;
        default: con_putc('?'); break;
        }
    }
    va_end(ap);
}

/* persist the console log in the 64 KiB just below the iOS map at the end of the boot partition (read back from Android/OrangeFox) */
static u64 log_dest_sector = (37748736UL - 2 * 65536) / 512;
void log_set_dest(u64 sector) { log_dest_sector = sector; }
void log_flush(void) {
    static u8 blk[LOGCAP + 512];
    struct part *b = gpt_find("boot");
    if (!b) return;
    u64 lba = b->first + log_dest_sector;
    size_t n = loglen > LOGCAP ? LOGCAP : loglen;
    memset(blk, 0, sizeof(blk));
    memcpy(blk, "A21SLOG1", 8); *(u32 *)(blk + 8) = (u32)n;
    memcpy(blk + 16, logbuf, n);
    u32 secs = (16 + n + 511) / 512;
    int rc = mmc_write(lba, secs, blk);
    if (rc) kprintf("log_flush rc=%d\n", rc);
}
