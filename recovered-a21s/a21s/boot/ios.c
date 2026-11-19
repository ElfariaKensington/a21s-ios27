/* iOS side-load map. The iOS payload and system image live as pinned, pre-allocated files inside Android's own
 * f2fs /data (/data/ios/*), so no partition has to be shrunk. The installer records where their blocks are in a
 * 64 KiB map at the very end of the boot partition; this reads the map and the files by raw extents. */
#include "a21s.h"

#define MAP_BYTES 65536
#define BOOT_PART_BYTES 37748736UL

struct map_hdr  { char magic[8]; u32 version, nfiles, nexts, crc; u32 pad[2]; };
struct map_file { char name[16]; char part[16]; u64 size; u32 first_ext, n_ext; };
struct map_ext  { u64 logical, phys, len; };   /* bytes; phys is relative to the partition `part` */

static u8 mapbuf[MAP_BYTES];
static struct map_hdr *mh = (struct map_hdr *)mapbuf;
static struct map_file *mf;
static struct map_ext *me;
static int map_ok;

u32 crc32(const u8 *p, size_t n) {
    u32 c = 0xffffffff;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320 & -(c & 1)); }
    return ~c;
}

int ios_map_load(void) {
    struct part *b = gpt_find("boot");
    if (!b) { kprintf("ios: no 'boot' partition\n"); return -1; }
    u64 lba = b->first + (BOOT_PART_BYTES - MAP_BYTES) / 512;
    int rc = mmc_read(lba, MAP_BYTES / 512, mapbuf);
    if (rc) { kprintf("ios: map read rc=%d\n", rc); return rc; }
    if (memcmp(mh->magic, "A21SIOS1", 8)) { kprintf("ios: no map (iOS not installed)\n"); return -2; }
    u32 want = mh->crc; mh->crc = 0;
    u32 got = crc32(mapbuf, MAP_BYTES);
    if (got != want) { kprintf("ios: map crc %08x != %08x\n", got, want); return -3; }
    mf = (struct map_file *)(mapbuf + sizeof(*mh));
    me = (struct map_ext *)(mapbuf + sizeof(*mh) + 8 * sizeof(struct map_file));
    map_ok = 1;
    return 0;
}

struct map_file *ios_file(const char *name) {
    if (!map_ok) return 0;
    for (u32 i = 0; i < mh->nfiles; i++) if (!strcmp(mf[i].name, name)) return &mf[i];
    return 0;
}

/* read [off, off+len) of a mapped file into dst; off and len multiples of 512 */
int ios_read(struct map_file *f, u64 off, u64 len, void *dst) {
    struct part *p = gpt_find(f->part);
    if (!p) { kprintf("ios: partition %s missing\n", f->part); return -1; }
    u8 *d = dst;
    while (len) {
        struct map_ext *e = 0;
        for (u32 i = 0; i < f->n_ext; i++) { struct map_ext *x = &me[f->first_ext + i]; if (off >= x->logical && off < x->logical + x->len) { e = x; break; } }
        if (!e) { kprintf("ios: offset %lx not mapped\n", off); return -2; }
        u64 within = off - e->logical, n = e->len - within;
        if (n > len) n = len;
        int rc = mmc_read(p->first + (e->phys + within) / 512, n / 512, d);
        if (rc) return rc;
        off += n; len -= n; d += n;
    }
    return 0;
}

int ios_info(void) {
    if (ios_map_load()) return -1;
    for (u32 i = 0; i < mh->nfiles; i++)
        kprintf("ios: %-8s %lu bytes, %u extents on %s\n", mf[i].name, mf[i].size, mf[i].n_ext, mf[i].part);
    return 0;
}

u64 ios_size(struct map_file *f) { return f->size; }
