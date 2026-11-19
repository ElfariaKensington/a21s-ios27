# Samsung Galaxy A21s (SM-A217F / Exynos 850) — iOS 27 (24A446) Port

Working iOS 27 (`24A446`, XNU `12377.121.6`) port for the Samsung Galaxy A21s (`S5E3830` / Exynos 850, Mali-G52 MP1, Samsung Maxwell S612 `scsc_wlan`).

## Quick Install

Requirements: rooted Galaxy A21s (`SM-A217F`) in Android (Magisk/KernelSU `su`) or OrangeFox recovery with ADB connected.

```bash
# Check current dual-boot status
./install.sh --status

# Full install (Stage 1 + payload + system image with Wi-Fi + boot into iOS 27)
./install.sh --stage1 out/a21s-stage1-boot.img \
             --payload out/payload-ios27.bin \
             --system out/root-slim.img \
             --wifi "YourSSID" "YourPassword" --boot

# Fast kernel-only update (preserves /data/ios/system and warmed Metal shader cache)
./install.sh --payload out/payload-ios27.bin --boot

# Clone upstream scion-kernel, apply A21s + SCSC Wi-Fi patches, build, flash, and boot
./install.sh --setup-sources --build-kernel --boot
```

## Hardware Status
- **Boot & Storage**: Dual-boot Stage 1 (`recovered-a21s/a21s/boot/`) + UEFI Loader v2 (`a21s_ios27_loader.c`) + 64-bit IDMAC eMMC (`/dev/ufsroot0` over pinned `/data/ios/system` extents) + clean `XNU27LOG` panic/reboot log flushing.
- **Graphics**: iOS 27 Liquid Glass Metal compositor (`sbgl=0 sbmtl=1`, `sgu_depth=256`, `DEXR=2`, `FLUSH=2`, `backdrop-scale=0.25`) running up to **60 FPS** (`60 swaps/s`) on Mali-G52 MP1 with transparency enabled.
- **Wi-Fi (`119c0000.scsc_wifibt` / Maxwell S612 `mx140` + `slsi_wlan`)**: Full end-to-end bring-up (`SMC_CMD_CONN_IF TZPC`, BAAW, KA patch, `BOOT_CFG_ACK`, HIP4, GICv2 SPI 54 / INTID 86), 2.4/5 GHz scan, WPA2-PSK 4-way handshake (16-byte CCMP and 32-byte TKIP GTK), `en0` DHCP + internet (verified 46.1 MB RX, 0 dropped packets).
- **Audio & Input**: Exynos 850 ABOX + Silicon Mitus SMA1303 amplifier + touchscreen (`a21s_input=1 startouch=92`).

## Known Remaining Issues
- On-screen keyboard (`InputUI`): touches on the keyboard area currently route to the host app window instead of `UIRemoteKeyboardWindow`, causing the keyboard to dismiss on tap.
- App Library Search: triggers an uncaught `NSException` (`Foundation + 0xc339c`) in `SpringBoard`.

## Repository Layout
- `a21s_ios27_loader.c`, `apply_a21s_xnu.py`, `mkadt_a21s.py`, `boot_ios27.sh`, `rebuild_and_boot.sh`, `dump_boot_logs.py` — A21s loader, ADT builder, XNU patcher, boot/log scripts.
- `scsc-driver/` — Samsung Exynos 850 SCSC WLBT (`mx140` + `slsi_wlan`) XNU glue (`wifi/lx/scsc/`), FreeBSD LinuxKPI header fixes (`nl80211.h`, `cfg80211.h`), and `mk-scsc-wlan.sh`.
- `patches/` — Clean diffs against `Scion-Project/scion-kernel` (`scion-kernel-a21s.patch`), XNU `12377.121.6` (`xnu-12377-a21s.patch`), and Exynos 850 SCSC WLAN (`scsc-wlan-src.patch`).
- `recovered-a21s/a21s/` — Stage 1 dual-boot loader and `a21s-install.py`.
