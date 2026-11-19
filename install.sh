#!/usr/bin/env bash
# Quick installer for iOS 27 (24A446) on Samsung Galaxy A21s (SM-A217F / Exynos 850)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
export PATH="$ROOT/bin:/opt/homebrew/bin:$PATH"

INSTALLER="$ROOT/recovered-a21s/a21s/tools/a21s-install.py"
DEFAULT_PAYLOAD="$ROOT/out/payload-ios27.bin"
DEFAULT_SYSTEM="$ROOT/out/root-slim.img"
DEFAULT_SYS_SIZE="11282677760"
DEFAULT_CMDLINE="scsc_wlan=8 abox=8 abox_rmem=1 sma1303=3 sweetsample=-"

STAGE1_IMG=""
PAYLOAD=""
SYSTEM_IMG=""
SYSTEM_SIZE=""
WIFI1_SSID=""
WIFI1_PASS=""
WIFI2_SSID=""
WIFI2_PASS=""
DO_SETUP_SRC=0
DO_BUILD_KERNEL=0
DO_BOOT=0
DO_STATUS=0

usage() {
  cat <<'EOF'
Samsung Galaxy A21s (SM-A217F) — iOS 27 Quick Installer

Usage:
  ./install.sh [options]

Options:
  --stage1 IMG            Flash dual-boot Stage 1 boot image (preserves iOS extent map)
  --payload FILE          Flash iOS payload (default: out/payload-ios27.bin)
  --system FILE           Flash full HFSX system image (e.g. out/root-slim.img)
  --system-size BYTES     Keep existing /data/ios/system on device (default: 11282677760)
  --wifi SSID PASS        Inject primary WPA2-PSK network (+ device MAC) into --system image
  --wifi2 SSID PASS       Inject fallback WPA2-PSK network into --system image
  --setup-sources         Clone Scion-Project/scion-kernel into work/ and apply A21s patches
  --build-kernel          Build scsc_wlan.o + XNU + loader.efi -> out/payload-ios27.bin
  --boot                  Set one-shot iOS 27 boot flag and reboot device into iOS 27
  --status                Show current A21s dual-boot extent map on device
  -h, --help              Show this help message

Examples:
  # 1. Check current dual-boot map on a rooted A21s (Android Magisk/KernelSU or OrangeFox):
  ./install.sh --status

  # 2. First-time full install (Stage 1 + payload + system image with Wi-Fi + boot):
  ./install.sh --stage1 out/a21s-stage1-boot.img \
               --payload out/payload-ios27.bin \
               --system out/root-slim.img \
               --wifi "MyWiFi" "MyPassword" --boot

  # 3. Fast kernel-only update (preserves /data/ios/system and warmed Metal cache):
  ./install.sh --payload out/payload-ios27.bin --boot

  # 4. Prepare source tree from upstream + A21s patches, then build kernel:
  ./install.sh --setup-sources --build-kernel --boot
EOF
}

die() { echo "error: $*" >&2; exit 1; }
need_cmd() { command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"; }

check_adb_root() {
  need_cmd adb
  need_cmd python3
  adb get-state >/dev/null 2>&1 || die "no ADB device detected (connect A21s in Android or OrangeFox)"
  local uid
  uid="$(adb shell 'id -u' 2>/dev/null | tr -d '\r\n')"
  if [ "$uid" != "0" ]; then
    adb shell 'su -c id' 2>/dev/null | grep -q 'uid=0' || die "root required (grant Magisk/KernelSU su to adb shell or boot OrangeFox)"
  fi
}

setup_sources() {
  need_cmd git
  local base_rev
  base_rev="$(tr -d '[:space:]' < "$ROOT/patches/scion-kernel.base")"
  if [ ! -d "$ROOT/work/.git" ]; then
    echo "[1/3] Cloning Scion-Project/scion-kernel into work/..."
    git clone https://github.com/Scion-Project/scion-kernel.git "$ROOT/work"
    git -C "$ROOT/work" checkout "$base_rev"
  fi
  echo "[2/3] Applying patches/scion-kernel-a21s.patch..."
  git -C "$ROOT/work" apply --check "$ROOT/patches/scion-kernel-a21s.patch" 2>/dev/null \
    && git -C "$ROOT/work" apply "$ROOT/patches/scion-kernel-a21s.patch" \
    || echo "  (patch already applied or work tree already modified; skipping apply)"
  echo "[3/3] Copying Exynos 850 SCSC Wi-Fi driver glue into work/..."
  mkdir -p "$ROOT/work/wifi/lx" "$ROOT/work/tools"
  cp -R "$ROOT/scsc-driver/wifi/lx/scsc" "$ROOT/work/wifi/lx/"
  cp -f "$ROOT/scsc-driver/tools/mk-scsc-wlan.sh" "$ROOT/work/tools/"
  if [ -d "$ROOT/work/xnu-deps/build/scsc/freebsd/sys/compat/linuxkpi/common/include" ]; then
    cp -f "$ROOT/scsc-driver/freebsd-headers/linux/nl80211.h" \
      "$ROOT/work/xnu-deps/build/scsc/freebsd/sys/compat/linuxkpi/common/include/linux/"
    cp -f "$ROOT/scsc-driver/freebsd-headers/net/cfg80211.h" \
      "$ROOT/work/xnu-deps/build/scsc/freebsd/sys/compat/linuxkpi/common/include/net/"
  fi
  echo "Source tree ready in $ROOT/work"
}

build_kernel() {
  [ -d "$ROOT/work" ] || die "work/ not found — run with --setup-sources first"
  [ -f "$ROOT/out/a21s.adt" ] || python3 "$ROOT/mkadt_a21s.py"
  export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode_27.2.app/Contents/Developer}"
  export KDKROOT="${KDKROOT:-$ROOT/fake-kdk/KDK.kdk}"
  export TOOLS_BIN="$ROOT/bin"
  mkdir -p "$ROOT/out"

  echo "[1/4] Building UEFI loader (loader.efi)..."
  xcrun clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar -mgeneral-regs-only -O1 \
    -c "$ROOT/a21s_ios27_loader.c" -o "$ROOT/out/loader.obj"
  lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 \
    /out:"$ROOT/out/loader.efi" "$ROOT/out/loader.obj"

  echo "[2/4] Building Exynos 850 SCSC Wi-Fi object (scsc_wlan.o)..."
  (cd "$ROOT/work" && bash tools/mk-scsc-wlan.sh)

  echo "[3/4] Building XNU 12377.121.6 (STARLTE/A21s, SCSC_WIFI=1)..."
  local kpath
  (cd "$ROOT/work" && SCSC_WIFI=1 OS_PROFILE=ios-27 tools/ios27/build-xnu-ios.sh -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 8)")
  kpath="$(cd "$ROOT/work" && SCSC_WIFI=1 OS_PROFILE=ios-27 tools/ios27/build-xnu-ios.sh --kernel-path)"
  cp -f "$kpath" "$ROOT/work/hakios-kernel"
  cp -f "$kpath" "$ROOT/out/kernel.stripped"
  xcrun strip -x "$ROOT/out/kernel.stripped"

  echo "[4/4] Packing XNUP payload -> $DEFAULT_PAYLOAD..."
  python3 "$ROOT/work/tools/ios27/herolte/mkpayload.py" \
    "$ROOT/out/kernel.stripped" "$ROOT/out/a21s.adt" "$ROOT/out/a21s-ios27.sweetxnu" --cmdline ""
  rm -f "$ROOT/out/kernel.stripped"

  python3 - "$ROOT/out/loader.efi" "$ROOT/out/a21s-ios27.sweetxnu" "$DEFAULT_CMDLINE" "$DEFAULT_PAYLOAD" <<'PY'
import struct, sys
loader = open(sys.argv[1], "rb").read()
sweet = open(sys.argv[2], "rb").read()
cmd = sys.argv[3].encode() + b"\0"
def ent(name, data):
    pad = (-len(data)) & 15
    return struct.pack("<16sII", name.encode(), len(data), 0) + data + b"\0" * pad
body = ent("loader.efi", loader) + ent("sweetxnu", sweet) + ent("cmdline", cmd)
out = struct.pack("<II", 0x50554E58, len(body)) + body
assert out[0x20:0x22] == b"MZ"
open(sys.argv[4], "wb").write(out)
print(f"Packed {sys.argv[4]} ({len(out)} bytes)")
PY
  PAYLOAD="$DEFAULT_PAYLOAD"
}

inject_wifi_into_image() {
  local img="$1"
  [ "$(uname -s)" = "Darwin" ] || die "--wifi image injection requires macOS (hdiutil + HFSX)"
  [ -f "$img" ] || die "system image not found: $img"
  local mnt dev mac
  mnt="$(mktemp -d /tmp/a21s-wifi-mnt.XXXXXX)"
  dev="$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$img" | awk 'NR==1{print $1}')"
  trap 'umount "$mnt" 2>/dev/null || true; hdiutil detach "$dev" 2>/dev/null || true; rmdir "$mnt" 2>/dev/null || true' RETURN
  mount -t hfs -o rw,noowners "$dev" "$mnt"
  mkdir -p "$mnt/private/var/hakios/wifi"
  if [ -n "$WIFI1_SSID" ]; then
    printf '%s\n%s\n' "$WIFI1_SSID" "$WIFI1_PASS" > "$mnt/private/var/hakios/wifi/network"
    echo "Wrote primary Wi-Fi credentials ($WIFI1_SSID) into $img"
  fi
  if [ -n "$WIFI2_SSID" ]; then
    printf '%s\n%s\n' "$WIFI2_SSID" "$WIFI2_PASS" > "$mnt/private/var/hakios/wifi/network2"
    echo "Wrote fallback Wi-Fi credentials ($WIFI2_SSID) into $img"
  fi
  if adb get-state >/dev/null 2>&1; then
    mac="$(adb shell 'su -c "cat /efs/wifi/.mac.info 2>/dev/null"' 2>/dev/null | tr -d '\r\n' || true)"
    if [[ "$mac" =~ ^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$ ]]; then
      printf '%s\n' "$mac" > "$mnt/private/var/hakios/wifi/mac.info"
      echo "Pulled device Wi-Fi MAC into $img"
    fi
  fi
  umount "$mnt"
  hdiutil detach "$dev" >/dev/null
  rmdir "$mnt"
  trap - RETURN
  if [ -f "$ROOT/fix_hfsx_owners.py" ] && [ -f "$ROOT/hfsx-extra-rules.txt" ]; then
    python3 "$ROOT/fix_hfsx_owners.py" "$img" "$ROOT/hfsx-extra-rules.txt" \
      ${ROOT}/out/hfsx-compressed.list 2>/dev/null || python3 "$ROOT/fix_hfsx_owners.py" "$img" "$ROOT/hfsx-extra-rules.txt"
  fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    --stage1)        STAGE1_IMG="$2"; shift 2 ;;
    --payload)       PAYLOAD="$2"; shift 2 ;;
    --system)        SYSTEM_IMG="$2"; shift 2 ;;
    --system-size)   SYSTEM_SIZE="$2"; shift 2 ;;
    --wifi)          WIFI1_SSID="$2"; WIFI1_PASS="$3"; shift 3 ;;
    --wifi2)         WIFI2_SSID="$2"; WIFI2_PASS="$3"; shift 3 ;;
    --setup-sources) DO_SETUP_SRC=1; shift ;;
    --build-kernel)  DO_BUILD_KERNEL=1; shift ;;
    --boot)          DO_BOOT=1; shift ;;
    --status)        DO_STATUS=1; shift ;;
    -h|--help)       usage; exit 0 ;;
    *)               die "unknown option: $1 (try --help)" ;;
  esac
done

[ "$DO_SETUP_SRC" = "1" ] && setup_sources
[ "$DO_BUILD_KERNEL" = "1" ] && build_kernel

# If no explicit action was given, default to flashing out/payload-ios27.bin
if [ "$DO_STATUS" = "0" ] && [ -z "$STAGE1_IMG" ] && [ -z "$PAYLOAD" ] && [ -z "$SYSTEM_IMG" ] && [ "$DO_BOOT" = "0" ] && [ "$DO_SETUP_SRC" = "0" ] && [ "$DO_BUILD_KERNEL" = "0" ]; then
  [ -f "$DEFAULT_PAYLOAD" ] || die "no payload found at $DEFAULT_PAYLOAD (pass --payload FILE or --build-kernel)"
  PAYLOAD="$DEFAULT_PAYLOAD"
fi

if [ -n "$WIFI1_SSID" ] || [ -n "$WIFI2_SSID" ]; then
  local_sys="${SYSTEM_IMG:-$DEFAULT_SYSTEM}"
  inject_wifi_into_image "$local_sys"
  SYSTEM_IMG="$local_sys"
fi

if [ "$DO_STATUS" = "1" ]; then
  check_adb_root
  python3 "$INSTALLER" status
  exit 0
fi

if [ -n "$STAGE1_IMG" ] || [ -n "$PAYLOAD" ] || [ -n "$SYSTEM_IMG" ] || [ "$DO_BOOT" = "1" ]; then
  check_adb_root
fi

if [ -n "$STAGE1_IMG" ]; then
  [ -f "$STAGE1_IMG" ] || die "stage1 image not found: $STAGE1_IMG"
  echo "Flashing Stage 1 dual-boot image: $STAGE1_IMG"
  python3 "$INSTALLER" stage1 "$STAGE1_IMG"
fi

if [ -n "$PAYLOAD" ] || [ -n "$SYSTEM_IMG" ]; then
  args=(ios)
  [ -n "$PAYLOAD" ] && { [ -f "$PAYLOAD" ] || die "payload not found: $PAYLOAD"; args+=(--payload "$PAYLOAD"); }
  if [ -n "$SYSTEM_IMG" ]; then
    [ -f "$SYSTEM_IMG" ] || die "system image not found: $SYSTEM_IMG"
    args+=(--system "$SYSTEM_IMG")
  else
    if [ -z "$SYSTEM_SIZE" ]; then
      SYSTEM_SIZE="$(adb shell 'su -c "stat -c %s /data/ios/system 2>/dev/null"' 2>/dev/null | tr -d '\r\n' || true)"
      SYSTEM_SIZE="${SYSTEM_SIZE:-$DEFAULT_SYS_SIZE}"
    fi
    args+=(--system-size "$SYSTEM_SIZE")
  fi
  echo "Installing iOS files to /data/ios and updating extent map..."
  python3 "$INSTALLER" "${args[@]}"
fi

if [ "$DO_BOOT" = "1" ]; then
  bash "$ROOT/boot_ios27.sh"
fi
