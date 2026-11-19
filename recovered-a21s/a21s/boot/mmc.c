/* DesignWare MSHC (Exynos flavour) raw reader for the eMMC, PIO, used exactly as S-Boot left it:
 * no clock/timing/bus-width changes, no card re-initialisation. Reads only (writes are not implemented). */
#include "a21s.h"

#define CTRL 0x00
#define CLKENA 0x10
#define CLKDIV 0x08
#define CTYPE 0x18
#define BLKSIZ 0x1c
#define BYTCNT 0x20
#define INTMASK 0x24
#define CMDARG 0x28
#define CMD 0x2c
#define RESP0 0x30
#define RINTSTS 0x44
#define STATUS 0x48
#define FIFOTH 0x4c
#define HCON 0x70
#define UHS 0x74
#define BMOD 0x80
#define IDSTS 0x8c
#define DATA 0x200

#define CMD_START (1u << 31)
#define CMD_HOLD (1u << 29)
#define CMD_WAITPRV (1u << 13)
#define CMD_AUTOSTOP (1u << 12)
#define CMD_DATA (1u << 9)
#define CMD_CRC (1u << 8)
#define CMD_RLONG (1u << 7)
#define CMD_RESP (1u << 6)

#define INT_RE (1u << 1)
#define INT_CD (1u << 2)
#define INT_DTO (1u << 3)
#define INT_RXDR (1u << 5)
#define INT_RCRC (1u << 6)
#define INT_DCRC (1u << 7)
#define INT_RTO (1u << 8)
#define INT_DRTO (1u << 9)
#define INT_HTO (1u << 10)
#define INT_HLE (1u << 12)
#define INT_SBE (1u << 13)
#define INT_ACD (1u << 14)
#define INT_EBE (1u << 15)
#define INT_ERR (INT_RE | INT_RCRC | INT_DCRC | INT_RTO | INT_DRTO | INT_HTO | INT_HLE | INT_SBE | INT_EBE)

static int rca = 1;
static int wide64;

static inline u32 mr(u32 o) { return r32(MMC_BASE + o); }
static inline void mw(u32 o, u32 v) { w32(MMC_BASE + o, v); }

static int wait_idle(u32 ms) {
    u64 t = ms_now() + ms;
    while (mr(STATUS) & ((1u << 9) | (1u << 10)))   /* data busy | mc busy */
        if (ms_now() > t) return -1;
    return 0;
}
static int send_cmd(u32 cmd, u32 arg, u32 *resp) {
    u64 t = ms_now() + 500;
    mw(RINTSTS, 0xffffffff);
    mw(CMDARG, arg);
    mw(CMD, cmd | CMD_START | CMD_HOLD);
    while (mr(CMD) & CMD_START) if (ms_now() > t) return -2;       /* hardware locked error / not taken */
    t = ms_now() + 500;
    u32 s;
    while (!((s = mr(RINTSTS)) & INT_CD)) { if (ms_now() > t) return -3; }
    if (s & (INT_RTO)) { mw(RINTSTS, 0xffffffff); return -4; }
    if (s & (INT_RCRC | INT_RE)) { mw(RINTSTS, 0xffffffff); return -5; }
    if (resp) resp[0] = mr(RESP0);
    mw(RINTSTS, INT_CD);
    return 0;
}

void mmc_dump(void) {
    kprintf("MMC ctrl=%08x pwr=%x clkdiv=%x clkena=%x ctype=%x\n", mr(CTRL), mr(0x04), mr(CLKDIV), mr(CLKENA), mr(CTYPE));
    kprintf("    fifoth=%08x hcon=%08x uhs=%08x bmod=%x status=%08x\n", mr(FIFOTH), mr(HCON), mr(UHS), mr(BMOD), mr(STATUS));
    kprintf("    verid=%08x clksel=%08x\n", mr(0x6c), mr(0x9c));
}

int mmc_init(void) {
    wide64 = (((mr(HCON) >> 7) & 7) == 2) ? 1 : 0;
    /* not using the internal DMA: PIO through the FIFO */
    mw(CTRL, mr(CTRL) & ~(1u << 25));
    mw(BMOD, mr(BMOD) & ~(1u << 7));
    if (wait_idle(200)) { kprintf("mmc: busy before init, status=%08x\n", mr(STATUS)); }
    mw(CTRL, mr(CTRL) | (1u << 1));                          /* FIFO reset */
    for (int i = 0; i < 100000 && (mr(CTRL) & (1u << 1)); i++) ;
    mw(RINTSTS, 0xffffffff);
    /* is the card talking to us?  CMD13 SEND_STATUS with the usual RCAs */
    for (int r = 1; r <= 2; r++) {
        u32 resp = 0;
        int rc = send_cmd(13 | CMD_RESP | CMD_CRC | CMD_WAITPRV, r << 16, &resp);
        kprintf("mmc: CMD13 rca=%d -> %d resp=%08x\n", r, rc, resp);
        if (rc == 0) { rca = r; int st = (resp >> 9) & 15; if (st != 4) { u32 r7; int rc7 = send_cmd(7 | CMD_RESP | CMD_CRC | CMD_WAITPRV, r << 16, &r7); kprintf("mmc: CMD7 select (state %d) -> %d\n", st, rc7); } return 0; }
    }
    return -1;
}

static int read_chunk(u64 lba, u32 n, u8 *buf) {
    if (wait_idle(500)) return -10;
    mw(CTRL, mr(CTRL) | (1u << 1));
    for (int i = 0; i < 100000 && (mr(CTRL) & (1u << 1)); i++) ;
    mw(BLKSIZ, 512);
    mw(BYTCNT, n * 512);
    mw(RINTSTS, 0xffffffff);
    u32 cmd = 18 | CMD_RESP | CMD_CRC | CMD_DATA | CMD_WAITPRV | CMD_AUTOSTOP;
    u64 t = ms_now() + 1000;
    mw(CMDARG, (u32)lba);          /* eMMC >2GB: block addressing; user area < 4 G sectors */
    mw(CMD, cmd | CMD_START | CMD_HOLD);
    while (mr(CMD) & CMD_START) if (ms_now() > t) return -11;
    u32 left = n * 512;
    u32 *p = (u32 *)buf;
    u64 deadline = ms_now() + 3000 + n;
    while (left) {
        u32 s = mr(RINTSTS);
        if (s & INT_ERR) { kprintf("mmc: rintsts=%08x lba=%lx\n", s, lba); mw(RINTSTS, 0xffffffff); return -12; }
        u32 fc = (mr(STATUS) >> 17) & 0x1fff;      /* words in the FIFO */
        if (!fc) { if (ms_now() > deadline) { kprintf("mmc: FIFO stall, rintsts=%08x left=%u\n", s, left); return -13; } continue; }
        deadline = ms_now() + 3000;
        if (wide64) {
            /* FIFO_COUNT counts 64-bit entries */
            volatile u64 *d = (volatile u64 *)(MMC_BASE + DATA);
            while (fc-- && left >= 8) { u64 v = *d; p[0] = (u32)v; p[1] = (u32)(v >> 32); p += 2; left -= 8; }
        } else {
            volatile u32 *d = (volatile u32 *)(MMC_BASE + DATA);
            while (fc-- && left >= 4) { *p++ = *d; left -= 4; }
        }
    }
    u64 t2 = ms_now() + 1000;
    while (!(mr(RINTSTS) & INT_DTO)) { if (ms_now() > t2) break; }
    u64 t3 = ms_now() + 500;
    while (!(mr(RINTSTS) & INT_ACD) && ms_now() < t3) ;
    u32 s = mr(RINTSTS);
    mw(RINTSTS, 0xffffffff);
    if (s & INT_ERR) return -14;
    return 0;
}

int mmc_read(u64 lba, u32 n, void *buf) {
    u8 *b = buf;
    while (n) {
        u32 c = n > 2048 ? 2048 : n;
        int rc = read_chunk(lba, c, b);
        if (rc) return rc;
        lba += c; n -= c; b += (u64)c * 512;
    }
    return 0;
}

static int write_chunk(u64 lba, u32 n, const u8 *buf) {
    if (wait_idle(500)) return -10;
    mw(CTRL, mr(CTRL) | (1u << 1));
    for (int i = 0; i < 100000 && (mr(CTRL) & (1u << 1)); i++) ;
    mw(BLKSIZ, 512);
    mw(BYTCNT, n * 512);
    mw(RINTSTS, 0xffffffff);
    u32 cmd = 25 | CMD_RESP | CMD_CRC | CMD_DATA | (1u << 10) | CMD_WAITPRV | CMD_AUTOSTOP;
    u64 t = ms_now() + 1000;
    mw(CMDARG, (u32)lba);
    mw(CMD, cmd | CMD_START | CMD_HOLD);
    while (mr(CMD) & CMD_START) if (ms_now() > t) return -11;
    u32 left = n * 512;
    const u32 *p = (const u32 *)buf;
    u64 deadline = ms_now() + 3000;
    u32 fifo_words = 64;                        /* fifo-depth from the device tree; the count register tells how much is free */
    while (left) {
        u32 s = mr(RINTSTS);
        if (s & INT_ERR) { kprintf("mmc: wr rintsts=%08x\n", s); mw(RINTSTS, 0xffffffff); return -12; }
        u32 fc = (mr(STATUS) >> 17) & 0x1fff;
        u32 room = fifo_words > fc ? fifo_words - fc : 0;
        if (!room) { if (ms_now() > deadline) { kprintf("mmc: wr FIFO stall\n"); return -13; } continue; }
        deadline = ms_now() + 3000;
        if (wide64) {
            volatile u64 *d = (volatile u64 *)(MMC_BASE + DATA);
            while (room-- && left >= 8) { *d = (u64)p[0] | ((u64)p[1] << 32); p += 2; left -= 8; }
        } else {
            volatile u32 *d = (volatile u32 *)(MMC_BASE + DATA);
            while (room-- && left >= 4) { *d = *p++; left -= 4; }
        }
    }
    u64 t2 = ms_now() + 3000;
    while (!(mr(RINTSTS) & INT_DTO)) { if (ms_now() > t2) { kprintf("mmc: wr no DTO\n"); return -14; } }
    u64 t3 = ms_now() + 500;
    while (!(mr(RINTSTS) & INT_ACD) && ms_now() < t3) ;
    mw(RINTSTS, 0xffffffff);
    /* wait for the card to finish programming: CMD13 until READY_FOR_DATA (bit 8) and state transfer */
    for (int i = 0; i < 2000; i++) {
        u32 r = 0;
        if (send_cmd(13 | CMD_RESP | CMD_CRC | CMD_WAITPRV, rca << 16, &r) == 0 && (r & (1u << 8)) && ((r >> 9) & 15) == 4) return 0;
        mdelay(1);
    }
    return -15;
}
int mmc_write(u64 lba, u32 n, const void *buf) {
    const u8 *b = buf;
    while (n) {
        u32 c = n > 128 ? 128 : n;
        int rc = write_chunk(lba, c, b);
        if (rc) return rc;
        lba += c; n -= c; b += (u64)c * 512;
    }
    return 0;
}

/* ---- GPT ---- */
static struct part parts[128]; static int nparts;
int gpt_count(void) { return nparts; }
struct part *gpt_get(int i) { return &parts[i]; }
struct part *gpt_find(const char *n) { for (int i = 0; i < nparts; i++) if (!strcmp(parts[i].name, n)) return &parts[i]; return 0; }
int gpt_load(void) {
    static u8 hdr[512], ents[128 * 128];
    int rc = mmc_read(1, 1, hdr);
    if (rc) { kprintf("gpt: read hdr rc=%d\n", rc); return rc; }
    if (memcmp(hdr, "EFI PART", 8)) { kprintf("gpt: bad signature %08x\n", *(u32 *)hdr); return -20; }
    u64 elba = *(u64 *)(hdr + 72); u32 ne = *(u32 *)(hdr + 80), es = *(u32 *)(hdr + 84);
    if (es != 128 || ne > 128) { kprintf("gpt: odd table %u x %u\n", ne, es); return -21; }
    rc = mmc_read(elba, (ne * es + 511) / 512, ents);
    if (rc) { kprintf("gpt: read entries rc=%d\n", rc); return rc; }
    nparts = 0;
    for (u32 i = 0; i < ne; i++) {
        u8 *e = ents + i * es;
        int used = 0; for (int k = 0; k < 16; k++) used |= e[k];
        if (!used) continue;
        struct part *p = &parts[nparts++];
        p->first = *(u64 *)(e + 32); p->last = *(u64 *)(e + 40);
        int j = 0; for (; j < 36; j++) { u16 c = *(u16 *)(e + 56 + 2 * j); if (!c) break; p->name[j] = c < 128 ? (char)c : '?'; }
        p->name[j] = 0;
    }
    return nparts;
}

/* ---- volume keys (gpa0.7 up, gpa1.0 down; active low) ---- */
int key_volup(void) { return !(r32(GPIO_ALIVE + 0x04) & (1u << 7)); }
int key_voldown(void) { return !(r32(GPIO_ALIVE + 0x24) & 1u); }
