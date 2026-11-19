/* Himax HX83112A touch controller, on the A21s' SPI1 (USI_CMGP0 at 0x11D00000, chip select a GPIO: gpm3-0), polled.
 * Everything the stock kernel's clock/pinctrl/USI/SPI drivers do before the first transfer is done here by hand, because
 * S-Boot leaves the block unused: CMU_CMGP gates (0x11C00000), the USI mode word (0x11C22000), the pins (GPM at 0x11C30000),
 * the controller. The event stack (register 0x30, 56 bytes: 10 x {x, y} big endian, 10 widths, pad, number of points, pad,
 * checksum byte) is the one the mainline hx83112b driver reads. Sources: that driver (GPL, read for the layout), the A21s
 * device tree, Linux's clk-exynos850.c offsets, this tree's hakios.exynos_spi.c for the controller's sequence. */
#include "a21s.h"

#define CMU       0x11C00000UL
#define USI_MODE  0x11C22000UL
#define SPI       0x11D00000UL
#define GPM       0x11C30000UL
#define GPA       0x11850000UL

#define CH_CFG 0x00
#define MODE_CFG 0x08
#define SLAVE_SEL 0x0C
#define INT_EN 0x10
#define STATUS 0x14
#define TX_DATA 0x18
#define RX_DATA 0x1C
#define PACKET_CNT 0x20
#define PENDING_CLR 0x24
#define SWAP_CFG 0x28
#define FB_CLK 0x2C
#define USI_CON 0xC4

static u32 touch_hdr = 3;
static u32 spi_mode = 0;          /* 0..3: CPOL/CPHA */
static int cs_pin_inited;

static void gpio_func(u64 bank, int pin, u32 fn) { w32(bank, (r32(bank) & ~(0xfu << (pin * 4))) | (fn << (pin * 4))); }
static void cs(int on) { u32 d = r32(GPM + 0x60 + 4); w32(GPM + 0x60 + 4, on ? (d & ~1u) : (d | 1u)); }

static void spi_flush(void) {
    u32 ch = r32(SPI + CH_CFG) & ~3u;
    w32(SPI + PACKET_CNT, 0);
    w32(SPI + CH_CFG, ch);
    w32(SPI + CH_CFG, ch | (1u << 5));
    for (int i = 0; i < 1000 && ((r32(SPI + STATUS) >> 6) & 0x7f); i++) ;
    for (int i = 0; i < 200 && ((r32(SPI + STATUS) >> 15) & 0x7f); i++) (void)r32(SPI + RX_DATA);
    w32(SPI + CH_CFG, ch);
    w32(SPI + PENDING_CLR, 0x1e); w32(SPI + PENDING_CLR, 0);
}

static void spi_setup(u32 mode) {
    spi_mode = mode;
    w32(SPI + USI_CON, 0);
    w32(SPI + SLAVE_SEL, 1);
    w32(SPI + INT_EN, 0); w32(SPI + MODE_CFG, 0); w32(SPI + PACKET_CNT, 0);
    w32(SPI + PENDING_CLR, 0x1e); w32(SPI + PENDING_CLR, 0);
    w32(SPI + SWAP_CFG, 0);
    w32(SPI + CH_CFG, ((mode & 2) ? (1u << 3) : 0) | ((mode & 1) ? (1u << 2) : 0));
    w32(SPI + MODE_CFG, 0x3ffu << 19);
    w32(SPI + FB_CLK, 0);
    spi_flush();
}

int touch_hw_init(u32 div, u32 mode) {
    static const u32 gates[] = { 0x2018, 0x2040, 0x2044, 0x2048 };
    for (int i = 0; i < 4; i++) w32(CMU + gates[i], r32(CMU + gates[i]) | (1u << 20) | (1u << 21));
    w32(CMU + 0x1804, (r32(CMU + 0x1804) & ~0x1fu) | div);
    for (int i = 0; i < 10000 && (r32(CMU + 0x1804) & (1u << 16)); i++) ;
    w32(USI_MODE, 2);
    gpio_func(GPM + 0x00, 0, 2); gpio_func(GPM + 0x20, 0, 2); gpio_func(GPM + 0x40, 0, 2);
    for (int b = 0; b < 3; b++) w32(GPM + b * 0x20 + 8, r32(GPM + b * 0x20 + 8) & ~3u);
    if (!cs_pin_inited) { w32(GPM + 0x60 + 4, r32(GPM + 0x60 + 4) | 1u); gpio_func(GPM + 0x60, 0, 1); w32(GPM + 0x60 + 8, r32(GPM + 0x60 + 8) & ~3u); cs_pin_inited = 1; }
    gpio_func(GPA, 0, 0);                       /* the touch interrupt, gpa0-0: an input, active low */
    spi_setup(mode);
    return 0;
}

int touch_irq_low(void) { return !(r32(GPA + 4) & 1); }

/* chip select held low across the whole transfer, a FIFO's half at a time */
static int xfer(const u8 *tx, u8 *rx, u32 len) {
    u32 done = 0;
    cs(1);
    w32(SPI + SLAVE_SEL, 0);                  /* the controller's own nSS: it does not clock while this is inactive (1) */
    udelay(2);
    while (done < len) {
        u32 n = len - done > 32 ? 32 : len - done, got = 0;
        spi_flush();
        for (u32 i = 0; i < n; i++) w32(SPI + TX_DATA, tx ? tx[done + i] : 0);
        w32(SPI + CH_CFG, r32(SPI + CH_CFG) | 3u);
        u64 t = ms_now() + 20;
        while (got < n) {
            if ((r32(SPI + STATUS) >> 15) & 0x7f) { u8 b = (u8)r32(SPI + RX_DATA); if (rx) rx[done + got] = b; got++; }
            else if (ms_now() > t) break;
        }
        w32(SPI + CH_CFG, r32(SPI + CH_CFG) & ~3u);
        if (got != n) { w32(SPI + SLAVE_SEL, 1); cs(0); return -1; }
        done += n;
    }
    udelay(2);
    w32(SPI + SLAVE_SEL, 1);
    cs(0);
    return 0;
}

/* Himax SPI: write = F2 reg data..., read = F3 reg 00 then the data (3 header bytes in the answer) */
static int hx_write(u8 reg, const u8 *d, u32 n) { u8 b[64]; b[0] = 0xF2; b[1] = reg; for (u32 i = 0; i < n; i++) b[2 + i] = d[i]; return xfer(b, 0, 2 + n); }
static int hx_read(u8 reg, u8 *d, u32 n, u32 hdr) {
    u8 tx[128] = { 0xF3, reg, 0 }, rx[128];
    if (n + hdr > sizeof(rx)) return -2;
    for (u32 i = 3; i < n + hdr; i++) tx[i] = 0;
    if (xfer(tx, rx, n + hdr)) return -1;
    for (u32 i = 0; i < n; i++) d[i] = rx[hdr + i];
    return 0;
}

int touch_read_icid(u32 hdr, u32 *id) {
    u8 a[4] = { 0xd0, 0x00, 0x00, 0x90 }, z[4] = { 0, 0, 0, 0 }, v[4];
    if (hx_write(0x00, a, 4) || hx_write(0x0c, z, 4) || hx_read(0x08, v, 4, hdr)) return -1;
    *id = v[0] | v[1] << 8 | v[2] << 16 | (u32)v[3] << 24;
    return 0;
}

/* the 56-byte event stack; returns the sum check (0 when valid) and the raw bytes */
int touch_read_events(u8 *ev, u32 hdr) { if (hx_read(0x30, ev, 56, hdr)) return -1; u32 s = 0; for (int i = 0; i < 56; i++) s += ev[i]; return (int)(s & 0xff); }

/* does the controller clock a byte out at all? (what the transfers' timeouts do not say) */
static void spi_diag(void) {
    kprintf("CMU regs before: mux %x div %x gat gpio %x sysreg %x ip %x p %x\n", r32(CMU + 0x1004), r32(CMU + 0x1804), r32(CMU + 0x2018), r32(CMU + 0x2040), r32(CMU + 0x2044), r32(CMU + 0x2048));
    log_flush();
    touch_hw_init(1, 0);
    kprintf("pins CON gpm0 %08x gpm1 %08x gpm2 %08x gpm3 %08x DAT3 %x usi %x\n", r32(GPM), r32(GPM + 0x20), r32(GPM + 0x40), r32(GPM + 0x60), r32(GPM + 0x64), r32(USI_MODE));
    log_flush();
    for (u32 div = 0; div < 8; div += 3)
        for (u32 md = 0; md < 4; md += 3) {
            touch_hw_init(div, md);
            w32(SPI + CH_CFG, r32(SPI + CH_CFG) & ~3u);
            spi_flush();
            cs(1); w32(SPI + SLAVE_SEL, 0);
            w32(SPI + TX_DATA, 0xA5); w32(SPI + TX_DATA, 0x5A);
            u32 st0 = r32(SPI + STATUS);
            w32(SPI + CH_CFG, r32(SPI + CH_CFG) | 3u);
            u32 st1 = r32(SPI + STATUS);
            udelay(200);
            u32 st2 = r32(SPI + STATUS);
            mdelay(2);
            u32 st3 = r32(SPI + STATUS);
            w32(SPI + CH_CFG, r32(SPI + CH_CFG) & ~3u);
            w32(SPI + SLAVE_SEL, 1); cs(0);
            kprintf("div%u md%u: st %08x %08x %08x %08x div %x usicon %x\n", div, md, st0, st1, st2, st3, r32(CMU + 0x1804), r32(SPI + USI_CON));
            log_flush();
        }
}

/* bench: what the first stage shows when the one-shot flag asks for it (touch the screen while it runs) */
void touch_test(void) {
    log_set_dest(71552);                 /* the logdump area: the next boot's own log does not overwrite it */
    kprintf("touch test start\n"); log_flush();
    kprintf("\ntouch test: Himax over SPI1\n");
    touch_hw_init(1, 0);
    spi_diag();
    log_flush();
    touch_hw_init(3, 0); w32(SPI + FB_CLK, 0);       /* div 3, mode 0, no feedback delay: the ICID reads 0x83112a, HX83112A */
    { u32 id = 0; int rc = touch_read_icid(3, &id); kprintf("icid rc=%d %08x (0x83112a00 expected)\n", rc, id); }
    log_flush();
    kprintf("now touch the screen (40 s)\n");
    u64 end = ms_now() + 40000; int lines = 0, polls = 0, seen = 0, okc = 0;
    while (ms_now() < end) {
        u64 t = ms_now() + 30; int got = 0;
        polls++;
        while (ms_now() < t) if (touch_irq_low()) { got = 1; break; }
        if (!got) continue;
        seen++;
        u8 raw[59]; u8 tx[59] = { 0xF3, 0x30, 0x00 };
        int rc = xfer(tx, raw, 59);
        u32 sm = 0; for (int i = 0; i < 56; i++) sm += raw[3 + i];
        if ((sm & 0xff) == 0) okc++;
        if (lines < 60 && (raw[3 + 52] != 0xff)) {
            lines++;
            kprintf("rc%d ck%02x n=%02x:", rc, sm & 0xff, raw[3 + 52]);
            for (int i = 0; i < 12; i++) kprintf(" %02x", raw[3 + i]);
            kprintf("\n");
        }
        for (u64 t2 = ms_now() + 10; ms_now() < t2 && touch_irq_low(); ) ;
    }
    kprintf("polls %d, int seen %d, ck ok %d\n", polls, seen, okc);
    log_flush();
    log_set_dest((37748736UL - 2 * 65536) / 512);
}

/* a look at the controller from the iOS path: does it still answer here, before the kernel takes over? */
void touch_probe(void) {
    u32 id = 0;
    touch_hw_init(3, 0);
    int rc = touch_read_icid(3, &id);
    kprintf("touch probe: icid rc=%d %08x (0x83112a00 expected)\n", rc, id);
    if (id == 0x83112a00) return;
    /* what the bench did before it read the ICID: a dummy transfer at several clock settings */
    for (u32 div = 0; div < 8; div += 3)
        for (u32 md = 0; md < 4; md += 3) {
            touch_hw_init(div, md);
            u8 d[2] = { 0xA5, 0x5A };
            xfer(d, 0, 2);
        }
    touch_hw_init(3, 0); w32(SPI + FB_CLK, 0);
    id = 0; rc = touch_read_icid(3, &id);
    kprintf("touch probe after wake: icid rc=%d %08x\n", rc, id);
}
