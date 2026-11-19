#ifndef A21S_H
#define A21S_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

/* Exynos 850 (S5E3830) / Galaxy A21s facts, from the phone's own device tree (a21s/hw/fdt.dts) */
#define FB_BASE      0xfa000000UL   /* S-Boot's framebuffer (s3cfb.bootloaderfb) */
#define FB_W         720
#define FB_H         1600
#define FB_STRIDE    2880
#define FB_HALF      (FB_STRIDE * FB_H)
#define MMC_BASE     0x12100000UL   /* dwmmc0: the eMMC */
#define GPIO_ALIVE   0x11850000UL   /* pinctrl@11850000: gpa0 +0x00, gpa1 +0x20 */
#define RAM_BASE     0x80000000UL
#define KERNEL_ADDR  0x80080000UL   /* where S-Boot puts a Linux kernel (DRAM base + text_offset) */
#define RAMDISK_ADDR 0x84080000UL
#define DTB_WORK     0xAA000000UL
#define WORK_BASE    0xAC000000UL   /* scratch/bump heap */

static inline u32 r32(u64 a) { return *(volatile u32 *)a; }
static inline void w32(u64 a, u32 v) { *(volatile u32 *)a = v; }
static inline u64 r64(u64 a) { return *(volatile u64 *)a; }

/* time */
u64 cnt(void);
void udelay(u32 us);
void mdelay(u32 ms);
u64 ms_now(void);

/* console */
void con_init(void);
void con_color(u32 fg, u32 bg);
void con_clear(void);
void con_putc(char c);
void touch_test(void);
void touch_probe(void);
void log_set_dest(u64 sector);
struct map_file;
int efi_boot(void *dtb, const u8 *pe_image, struct map_file *system);
void exc_handler(u64 esr, u64 elr, u64 far, u64 lr);
void kputs(const char *s);
void kprintf(const char *fmt, ...);
void con_at(int col, int row);
extern char logbuf[]; extern size_t loglen;

/* mmc / gpt */
int  mmc_init(void);
void mmc_dump(void);
int  mmc_read(u64 lba, u32 n, void *buf);
int  mmc_write(u64 lba, u32 n, const void *buf);
void log_flush(void);
struct part { char name[40]; u64 first, last; };
int  gpt_load(void);
int  gpt_count(void);
struct part *gpt_get(int i);
struct part *gpt_find(const char *name);

/* keys */
int key_volup(void);
int key_voldown(void);

/* boot */
int boot_android_image(struct part *p, int recovery_mode, void *dtb);
int boot_ios(void *dtb);
void jump_kernel(u64 entry, u64 dtb) __attribute__((noreturn));
void append_log_to_loaded_ramdisk(void *dtb);

/* iOS side-load map (files inside /data, see ios.c) */
struct map_file;
int  ios_map_load(void);
int  ios_info(void);
u32  crc32(const u8 *p, size_t n);
u64  ios_size(struct map_file *f);
struct map_file *ios_file(const char *name);
int  ios_read(struct map_file *f, u64 off, u64 len, void *dst);

/* misc */
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
#endif
