# Galaxy A21s (SM-A217F, Exynos 850) beside Android

Port of this repository's S9+ work to the Samsung Galaxy A21s, with iOS meant to live next to the phone's own Android.
Everything here was measured on one phone (bootloader unlocked, OrangeFox recovery, a Magisk-rooted Android 16 ROM);
`hw/` holds its device tree, memory map and partition table.

**Read [STATUS.md](STATUS.md) first: the dual-boot loader works; iOS does not boot yet.**

## What is here

| Path | What |
| --- | --- |
| `boot/` | The first stage. S-Boot starts it as the `boot.img` kernel; draws on S-Boot's framebuffer, reads the eMMC, shows a Vol-key menu and starts Android, OrangeFox or (later) iOS. Freestanding C, built with clang + lld. |
| `tools/mkboot.py` | Wraps the first stage into a boot image, keeping the stock kernel (`--embed`). |
| `tools/boot-once.sh` | One-shot next-boot choice (`ios`, `recovery`, `android`) written from Android/OrangeFox into the `boot` partition. |
| `tools/a21s-install.py` | Installs/restores over adb (phone in OrangeFox or in rooted Android). |
| `tools/ondevice/iosfile.c` | Static helper that pre-allocates and pins a file on f2fs and prints its physical extents. |
| `hw/` | Device tree (`fdt.dts`), `iomem.txt`, partition table of the phone. |
| `../devices/a21s/device.conf` | The device record in this repository's format. |
| `../core/xnuloader/loader.c` | `platform_a21s` added (values from `hw/`, unverified ones marked). |

## How the dual boot works

```
S-Boot -> `boot` partition = stock Android kernel (Image) + stock ramdisk + stock DTB table
          its first instruction is redirected to the first stage, which is appended behind the kernel
          first stage (menu, 8 s):  Android (default)   put the instruction back, jump: exactly the stock hand-over
                                    Recovery            read OrangeFox from `recovery` over eMMC, patch the tree, jump
                                    iOS                 read /data/ios/payload via the extent map (loader not written)
```

* **Android** boots through the stock kernel S-Boot itself loaded, so Samsung's hypervisor sees what it expects.
* **iOS is stored as pinned files in Android's own `/data`** (`/data/ios/payload`, `/data/ios/system`), allocated with
  `F2FS_IOC_SET_PIN_FILE` + `fallocate`, so no partition is shrunk and no user data is touched. Their physical
  extents are written to a 64 KiB map at the end of the `boot` partition; the first stage reads files by raw extents.
  (Deleting `/data/ios` or factory-resetting removes iOS and nothing else.)
* The first stage logs to the framebuffer and also to eMMC (`boot` partition, 64 KiB below the map): read it back with
  `adb shell su -c 'dd if=/dev/block/by-name/boot bs=512 skip=73472 count=64' | strings`.

## Install / restore

Back up first (the commands used are in STATUS.md). Then, with the phone in OrangeFox or in rooted Android:

```sh
cd a21s/boot && make DEFAULT_ENTRY=0 && cd ..          # needs: brew install llvm lld
python3 tools/mkboot.py --embed --partition-size 37748736 STOCK_boot.img boot/stage1.bin build/boot.img   # build the stage with make DEFAULT_ENTRY=0 (Android default)
python3 tools/a21s-install.py stage1 build/boot.img     # writes `boot` (36 MiB minus the map), verifies
python3 tools/a21s-install.py ios --payload payload.bin # pinned file + map
python3 tools/a21s-install.py restore-stock STOCK_boot.img   # undo: the original boot image
```

If the screen hangs: hold Power + Vol Down ~10 s, then Vol Up + Power: S-Boot starts `recovery` directly,
whatever is in `boot`. Restore with `restore-stock`.
