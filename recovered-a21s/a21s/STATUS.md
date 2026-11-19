# Status (verified on the phone unless it says otherwise)

## Works
* **First stage starts.** S-Boot accepts the modified `boot.img` (unlocked, orange state); the stage runs at EL1, MMU off, relocates itself, draws a text console on S-Boot's 720x1600 XRGB framebuffer at 0xfa000000 (colour order checked with a swatch).
* **eMMC.** The DesignWare MSHC at 0x12100000 is used as S-Boot left it (HS400, 8-bit): PIO reads (CMD18 + auto-stop) and writes (CMD25), GPT parsing (37 partitions). Writes are used only for the first stage's own log area.
* **Android boots through the first stage**, with the stock kernel at its stock place (`uname -r` 4.19.325-g4a2052095396, `sys.boot_completed=1`). Default entry, 8 s menu, Vol Up = next, Vol Down = boot.
* **OrangeFox chain-load.** The first stage reads kernel + ramdisk of the `recovery` partition, rewrites `/chosen` (initrd range, `bootmode=2 root=/dev/ram0`, header cmdline), packs the tree (must be < 2 MiB; the first attempt left it at 4 MiB and hung), and jumps: OrangeFox came up (`uname -r` 4.19.325-gb7b212f09bb7).
* **iOS side-load storage.** Pinned, pre-allocated file in Android's f2fs `/data`, physical extents in a map at the end of `boot`; the first stage read the test payload through the map (`A21S-IOS...` header) and persisted its log to eMMC.
* Failed boot attempts fall back to Android after 10 s instead of halting.
* **iOS 16.5 boots to SpringBoard's home screen** (photo of the phone: FaceTime, Calendar, Photos, Mail, ... dock, 720x1600 panel). Chain: first stage -> EFI shim (`boot/efi_shim.c`: system/boot/runtime services, memory map from S-Boot's DT, GOP on the panel buffer, DT as config table, PE loader, identity MMU) -> `loader.efi` (`platform_a21s`) -> XNU 8796 on core 1 (PSCI CPU_ON; the EL3 monitor drops the context id, so the loader's secondary stub loads its start data from a symbol) while core 0 serves the kernel's reads of the system image (`ufs=2` block service on the shim's BlockIO "SYSTEM", backed by the pinned file via the map). Root: the 4 MiB ALONE image (`build.sh ALONE=1 RC=rc.launchd`), then `system_apfs=ufs_SYSTEM`.
* **Dual boot control.** Default entry is Android. One-shot override from Android/OrangeFox: `tools/boot-once.sh ios|recovery|android` then `adb reboot` (flag sector in `boot`, cleared by the first stage before use: no boot loops). The 8 s menu (Vol Up next, Vol Down boot) also has an iOS entry. iOS returns to Android by itself after `loader_return_after=600` s (PMU reset via +0x3a00 = 2, from the stock kernel's `exynos_restart_v1`, with `recovery=0` so S-Boot starts BOOT), or by holding Power + Vol Down.
* **Logs.** The first stage's own log: boot sectors 73472.., the loader's and the kernel's (written by the loader before its reset, via a `logdump` BlockIO): sectors 71552..73470 (`dd if=/dev/block/by-name/boot bs=512 skip=71552 count=1919 | strings`).

## Not done
* **iOS has no input.** The screen is drawn and then frozen to the eye: there is no touch driver (the A21s' controller is a Himax HX83112A on SPI1, `spi@11D00000`, chip select 1, firmware `tsp_himax/hx83112a_a21s.bin`; a Synaptics TD4150 is also in the DT), no volume/power key driver for it (volume keys are GPA0.7 / GPA1.0, active low; power is on the PMIC). Needed: the Exynos 850 SPI/USI clock and pin setup, the Himax protocol (and probably its firmware load), HID events into the kernel's input layer.
* No GPU (Mali-G52), no Wi-Fi (Samsung SCSC), no audio/modem/camera. Graphics are the framebuffer path.
* `platform_a21s` values still unverified: PMU watchdog bit, parked cores (cores 1-7 all start by PSCI; only core 1 is used, `cpus=1`).
* The kernel's log is only captured when the kernel returns or panics (after `loader_return_after`), not when the phone is force-rebooted.

## Backups (taken before anything was written)
`~/a21s-backup/`: every small partition (boot, recovery, dtb, dtbo, vbmeta*, efs, sec_efs, persistent, param, up_param, misc, uh, uhcfg, ldfw, tz*, keystorage, radio, ...), `mmcblk0boot0/1`, the GPT head, and `/data/media/0` (3.8 GB). `super` and `userdata` were not imaged; nothing in them was modified.
