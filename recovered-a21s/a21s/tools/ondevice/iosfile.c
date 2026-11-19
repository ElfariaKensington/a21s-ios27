/* iosfile: create/prepare a pinned, pre-allocated file on f2fs and print its physical extents.
 *   iosfile alloc PATH BYTES    create PATH (must not exist), pin it (never moved by GC), preallocate BYTES
 *   iosfile extents PATH        print "logical_bytes physical_bytes length_bytes" per extent (physical is
 *                               relative to the start of the block device the filesystem lives on)
 * Static aarch64 binary for recovery/Android (built with zig cc, see a21s/tools/build-ondevice.sh). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define F2FS_IOCTL_MAGIC 0xf5
#define F2FS_IOC_SET_PIN_FILE _IOW(F2FS_IOCTL_MAGIC, 13, uint32_t)
#define F2FS_IOC_GET_PIN_FILE _IOR(F2FS_IOCTL_MAGIC, 14, uint32_t)

static int alloc_file(const char *p, uint64_t n) {
    int fd = open(p, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { perror("create"); return 1; }
    uint32_t pin = 1;
    if (ioctl(fd, F2FS_IOC_SET_PIN_FILE, &pin) < 0) { perror("F2FS_IOC_SET_PIN_FILE"); return 2; }
    if (fallocate(fd, 0, 0, n) < 0) { perror("fallocate"); return 3; }
    fsync(fd);
    uint32_t chk = 0; ioctl(fd, F2FS_IOC_GET_PIN_FILE, &chk);
    fprintf(stderr, "allocated %llu bytes, pin count %u\n", (unsigned long long)n, chk);
    close(fd);
    return 0;
}

static int extents(const char *p) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    fsync(fd);
    enum { N = 512 };
    size_t sz = sizeof(struct fiemap) + N * sizeof(struct fiemap_extent);
    struct fiemap *fm = calloc(1, sz);
    uint64_t start = 0;
    for (;;) {
        memset(fm, 0, sz);
        fm->fm_start = start; fm->fm_length = ~0ULL; fm->fm_flags = FIEMAP_FLAG_SYNC; fm->fm_extent_count = N;
        if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) { perror("FIEMAP"); return 2; }
        if (!fm->fm_mapped_extents) break;
        int last = 0;
        for (unsigned i = 0; i < fm->fm_mapped_extents; i++) {
            struct fiemap_extent *e = &fm->fm_extents[i];
            if (e->fe_flags & (FIEMAP_EXTENT_UNWRITTEN)) { /* preallocated but unwritten still has a physical address */ }
            if (e->fe_flags & (FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_NOT_ALIGNED | FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_ENCODED)) {
                fprintf(stderr, "extent %u unusable, flags %x\n", i, e->fe_flags); return 3;
            }
            printf("%llu %llu %llu\n", (unsigned long long)e->fe_logical, (unsigned long long)e->fe_physical, (unsigned long long)e->fe_length);
            start = e->fe_logical + e->fe_length;
            if (e->fe_flags & FIEMAP_EXTENT_LAST) last = 1;
        }
        if (last) break;
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "alloc")) return alloc_file(argv[2], strtoull(argv[3], 0, 0));
    if (argc == 3 && !strcmp(argv[1], "extents")) return extents(argv[2]);
    fprintf(stderr, "usage: iosfile alloc PATH BYTES | iosfile extents PATH\n");
    return 64;
}
