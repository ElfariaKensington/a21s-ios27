/* iosdelta: fast 1 MiB chunk digest & delta writer for /data/ios/system on Android.
 *   iosdelta hashes PATH          write 16-byte digest (2x uint64_t) per 1 MiB chunk of PATH to stdout
 *   iosdelta apply PATH           read repeated [uint64_t offset, uint32_t len, uint32_t pad, bytes[len]] from stdin
 *                                 (terminated by len == 0), pwrite() in-place, fsync() once at end.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK (1024 * 1024)

static void hash1m(const uint8_t *p, size_t n, uint64_t out[2]) {
    const uint64_t *w = (const uint64_t *)p;
    size_t nw = n / 8;
    uint64_t h1 = 0x9e3779b97f4a7c15ULL ^ n;
    uint64_t h2 = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < nw; i++) {
        uint64_t v = w[i];
        h1 = (h1 ^ v) * 0xbf58476d1ce4e5b9ULL;
        h1 ^= h1 >> 31;
        h2 = (h2 + v + i) * 0x94d049bb133111ebULL;
        h2 ^= h2 >> 29;
    }
    out[0] = h1;
    out[1] = h2;
}

static int read_full(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, p + got, len - got);
        if (r == 0) return got == 0 ? 0 : -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 1;
}

static int write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t put = 0;
    while (put < len) {
        ssize_t r = write(fd, p + put, len - put);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        put += (size_t)r;
    }
    return 0;
}

static int cmd_hashes(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
#ifdef __linux__
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    uint8_t *buf = malloc(CHUNK);
    if (!buf) return 2;
    for (;;) {
        size_t got = 0;
        while (got < CHUNK) {
            ssize_t r = read(fd, buf + got, CHUNK - got);
            if (r == 0) break;
            if (r < 0) { if (errno == EINTR) continue; perror("read"); return 3; }
            got += (size_t)r;
        }
        if (got == 0) break;
        if (got < CHUNK) memset(buf + got, 0, CHUNK - got);
        uint64_t h[2];
        hash1m(buf, CHUNK, h);
        if (write_full(STDOUT_FILENO, h, sizeof(h)) < 0) return 4;
        if (got < CHUNK) break;
    }
    free(buf);
    close(fd);
    return 0;
}

static int cmd_apply(const char *path) {
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open rw"); return 1; }
    uint8_t *buf = malloc(CHUNK * 4);
    if (!buf) return 2;
    uint64_t total = 0, chunks = 0;
    for (;;) {
        struct { uint64_t off; uint32_t len; uint32_t pad; } hdr;
        int rr = read_full(STDIN_FILENO, &hdr, sizeof(hdr));
        if (rr == 0 || (rr == 1 && hdr.len == 0)) break;
        if (rr < 0 || hdr.len > CHUNK * 4) { fprintf(stderr, "bad hdr len=%u\n", hdr.len); return 3; }
        if (read_full(STDIN_FILENO, buf, hdr.len) != 1) { fprintf(stderr, "eof in data\n"); return 4; }
        size_t put = 0;
        while (put < hdr.len) {
            ssize_t w = pwrite(fd, buf + put, hdr.len - put, (off_t)(hdr.off + put));
            if (w < 0) { if (errno == EINTR) continue; perror("pwrite"); return 5; }
            put += (size_t)w;
        }
        total += hdr.len;
        chunks++;
    }
    if (fsync(fd) < 0) { perror("fsync"); return 6; }
    close(fd);
    free(buf);
    fprintf(stderr, "iosdelta: applied %llu chunks (%llu MiB) and fsynced\n",
        (unsigned long long)chunks, (unsigned long long)(total >> 20));
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "hashes")) return cmd_hashes(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "apply")) return cmd_apply(argv[2]);
    fprintf(stderr, "usage: iosdelta hashes PATH | iosdelta apply PATH\n");
    return 64;
}
