# Build the A21s stage-1 and wrap it in a boot.img (needs: llvm clang + lld, python3)
CC      = /Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/recovered-a21s/_stage/shims/clang
LD      = /Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/recovered-a21s/_stage/shims/ld.lld
OBJCOPY = /Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/recovered-a21s/_stage/shims/llvm-objcopy
DEFAULT_ENTRY ?= 1
CFLAGS  = --target=aarch64-none-elf -ffreestanding -fno-builtin -fno-stack-protector -mgeneral-regs-only \
          -mstrict-align -O2 -Wall -Wno-unused-function -Wno-pointer-sign -I. -Ilibfdt -DDEFAULT_ENTRY=$(DEFAULT_ENTRY) -fno-pic \
          -nostdinc -isystem $(shell $(CC) --target=aarch64-none-elf -print-resource-dir)/include
SRCS    = main.c console.c mmc.c bootimg.c ios.c touch.c efi_shim.c util.c libfdt/fdt.c libfdt/fdt_ro.c libfdt/fdt_rw.c libfdt/fdt_wip.c libfdt/fdt_strerror.c libfdt/fdt_addresses.c
OBJS    = $(SRCS:.c=.o) start.o vectors.o
all: stage1.bin
%.o: %.c a21s.h font.h
	$(CC) $(CFLAGS) -c $< -o $@
start.o: start.S
	$(CC) --target=aarch64-none-elf -c $< -o $@
vectors.o: vectors.S
	$(CC) --target=aarch64-none-elf -c $< -o $@
stage1.elf: $(OBJS) link.ld
	$(LD) -T link.ld -o $@ $(OBJS)
stage1.bin: stage1.elf
	$(OBJCOPY) -O binary $< $@
clean:
	rm -f $(OBJS) stage1.elf stage1.bin
