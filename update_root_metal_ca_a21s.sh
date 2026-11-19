#!/usr/bin/env bash
set -euo pipefail

BASE=/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27
FR=$BASE/work/tools/ios27/fullroot
PIPE=$FR/pipeline
PRIV=$FR/private
IN=$BASE/inputs
OUT=${1:-$BASE/out/root-slim.img}
SIZE_GB=${SIZE_GB:-10}
STAGE=$BASE/work/build/metal27/stage

export DEVELOPER_DIR=/Applications/Xcode_27.2.app/Contents/Developer
export KDKROOT="$BASE/fake-kdk/KDK.kdk"
export NOWIFI=1
export TOOLS_BIN="$BASE/bin"
export PY="$BASE/venv/bin/python"
export IOS27_PIPELINE="$PIPE"
export IOS27_PRIVATE="$PRIV"
export PYTHONPATH="$BASE/norootpy"
export PATH="$BASE/norootsbin:$BASE/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"

[ -f "$STAGE/StarbiteMetal.bundle/StarbiteMetal" ] || { echo "ERROR: missing StarbiteMetal.bundle" >&2; exit 1; }
[ -f "$STAGE/StarbiteMetal.bundle/libvulkan_panfrost.dylib" ] || { echo "ERROR: missing libvulkan_panfrost.dylib" >&2; exit 1; }

if [ "${SKIP_XNU:-0}" != "1" ]; then
echo "=== [0/8] Rebuilding XNU kernel (sbgl=0 sbmtl=1 sbshot=0 sweetspeed=601624005) & payloads ==="
(
    cd "$BASE/work"
    OS_PROFILE=ios-27 tools/ios27/build-xnu-ios.sh -j8
    K=$(OS_PROFILE=ios-27 tools/ios27/build-xnu-ios.sh --kernel-path)
    cp -f "$K" "$BASE/work/hakios-kernel"
    cp -f "$K" "$BASE/out/kernel.stripped"
    xcrun strip -x "$BASE/out/kernel.stripped"
    /usr/bin/python3 "$BASE/work/tools/ios27/herolte/mkpayload.py" "$BASE/out/kernel.stripped" "$BASE/out/a21s.adt" "$BASE/out/a21s-ios27.sweetxnu" --cmdline ""
    rm -f "$BASE/out/kernel.stripped"
    git -C "$BASE/work/build/dxb-12377/xnu" diff > "$BASE/out/a21s-xnu-12377.patch"
)

for spec in "sweetreboot=95 herolog=1:$BASE/out/payload-ios27-verify.bin" "sweetwdog=180:$BASE/out/payload-ios27.bin"; do
    cmd="${spec%%:*}"
    dst="${spec#*:}"
    /usr/bin/python3 - "$BASE/out/loader.efi" "$BASE/out/a21s-ios27.sweetxnu" "$cmd" "$dst" <<'PYEOF'
import struct, sys
loader = open(sys.argv[1], "rb").read()
sweet = open(sys.argv[2], "rb").read()
cmd = sys.argv[3].encode() + b"\0"
def ent(name, data):
    pad = (-len(data)) & 15
    return struct.pack("<16sII", name.encode(), len(data), 0) + data + b"\0" * pad
body = ent("loader.efi", loader) + ent("sweetxnu", sweet) + ent("cmdline", cmd)
out = struct.pack("<II", 0x50554E58, len(body)) + body
assert out[0x20:0x22] == b"MZ", out[0x20:0x24]
open(sys.argv[4], "wb").write(out)
print("packed", sys.argv[4], len(out), "cmdline=", sys.argv[3])
PYEOF
done
fi

W=$BASE/mnt-work
OSMNT=$W/os
SYSMNT=$W/sys
ROOTMNT=$W/root
mkdir -p "$OSMNT" "$SYSMNT" "$ROOTMNT"

detach_if() {
    if [ -n "${1:-}" ]; then
        hdiutil detach "$1" >/dev/null 2>&1 || hdiutil detach -force "$1" >/dev/null 2>&1 || true
    fi
}
cleanup() {
    detach_if "$ROOTMNT"
    [ -n "${RDEV:-}" ] && detach_if "$RDEV"
    detach_if "$OSMNT"
    detach_if "$SYSMNT"
    for m in "$BASE/mnt-ro/root" "$BASE/mnt-ro/os" "$BASE/mnt-ro/sys" "$BASE/mnt-ro/app"; do detach_if "$m"; done
}
trap cleanup EXIT

for m in "$ROOTMNT" "$OSMNT" "$SYSMNT" "$BASE/mnt-ro/root" "$BASE/mnt-ro/os" "$BASE/mnt-ro/sys" "$BASE/mnt-ro/app"; do
    detach_if "$m"
done

echo "=== [1/8] Truncating +8 MiB pad and attaching 24A446 DMGs (RO) + out/root-slim.img (RW) ==="
/usr/bin/python3 -c "import os; os.truncate('$OUT', $SIZE_GB * 1024**3)"
hdiutil attach -readonly -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$OSMNT" "$IN/os.dmg" >/dev/null
hdiutil attach -readonly -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$SYSMNT" "$IN/systemos.dmg" >/dev/null
hdiutil attach -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$ROOTMNT" "$OUT" >/dev/null
df -m "$ROOTMNT"

echo "=== [2/8] Staging StarbiteMetal.bundle + running 80-metal-ca.sh (Metal-CA Liquid Glass) ==="
MCA_DIR="$PRIV/metal-ca-files"
rm -rf "$MCA_DIR"
mkdir -p "$MCA_DIR"
cp -R "$STAGE/StarbiteMetal.bundle" "$MCA_DIR/"
(cd "$MCA_DIR" && find StarbiteMetal.bundle -type f | sort | while IFS= read -r f; do shasum -a 256 "$f"; done > MANIFEST.sha256)

mkdir -p "$ROOTMNT/usr/local/bin"
cp -f "$STAGE/m2vtest" "$ROOTMNT/usr/local/bin/m2vtest"
chmod 0755 "$ROOTMNT/usr/local/bin/m2vtest"

ENABLE_METAL_CA=1 METAL_CA_FILES="$MCA_DIR" METAL_CA_CLIENTS="backboardd" METAL_CA_FLUSH=2 \
    bash "$FR/steps.d/metal-ca/80-metal-ca.sh" "$ROOTMNT"

# Put SWEET_MTL2_CONSOLE=1 FIRST so HotMetal2 opens /dev/console before first sm2_log(), plus STARBITE_DRM_SEQ=1
cat > "$ROOTMNT/private/var/local/mtl2-env" <<'EOF'
SWEET_MTL2_CONSOLE=0
SWEET_MTL2_DEXR=1
SWEET_MTL2_FLUSH=2
STARBITE_DRM_SEQ=1
STARBITE_DRM_LEAKS=0
EOF
chmod 0644 "$ROOTMNT/private/var/local/mtl2-env"
echo "mtl2-env: $(tr '\n' ' ' < "$ROOTMNT/private/var/local/mtl2-env")"
echo "mtl2-clients: $(cat "$ROOTMNT/private/var/local/mtl2-clients")"

echo "=== [3/8] Restoring UI/Compositor *.metallib files from os.dmg & systemos.dmg ==="
SKIP_METALLIB_RE='(MetalPerformanceShaders|GESS\.framework|Morpheus\.framework|Espresso\.framework|CoreRE\.framework|AvatarKit\.framework|PortraitFilters\.cifilter|GPUToolsReplay\.framework|RawCamera|CMImaging\.framework|CorePhotogrammetry\.framework|DeepVideoProcessingCore\.framework)'
mcnt=0
while IFS= read -r -d '' mlib; do
    rel=${mlib#"$OSMNT"/}
    if printf '%s' "$rel" | grep -Eq "$SKIP_METALLIB_RE"; then
        continue
    fi
    dst="$ROOTMNT/$rel"
    mkdir -p "$(dirname "$dst")"
    cp -f "$mlib" "$dst"
    mcnt=$((mcnt + 1))
done < <(find "$OSMNT/System/Library" -name "*.metallib" -print0)

while IFS= read -r -d '' mlib; do
    rel=${mlib#"$SYSMNT"/}
    dst_sys="$ROOTMNT/$rel"
    if [ ! -e "$dst_sys" ]; then
        mkdir -p "$(dirname "$dst_sys")"
        cp -f "$mlib" "$dst_sys"
        mcnt=$((mcnt + 1))
    fi
done < <(find "$SYSMNT/System/Library" -name "*.metallib" -print0)
echo "Restored $mcnt UI/framework .metallib files"

echo "=== [4/8] Restoring missing Settings icon bundles (AppPlaceholders, iCloud+, GameCenter, SOSBuddy) & uncompressed icon catalogs ==="
/usr/bin/rsync -aH --extended-attributes \
    --include='en.lproj/***' --include='Base.lproj/***' --include='English.lproj/***' --include='en_*.lproj/***' --exclude='*.lproj/' \
    "$OSMNT/System/Library/AppPlaceholders/" "$ROOTMNT/System/Library/AppPlaceholders/"

EXTRA_ICON_APPS=(
    "iCloud+.app"
    "iCloud.app"
    "GameCenterUIService.app"
    "GameCenterWidgets.app"
    "GameTrampoline.app"
    "SOSBuddy.app"
    "Screen Time.app"
    "SafetyMonitorApp.app"
    "CarPlaySettings.app"
    "AppDeletionUIHost.app"
    "AppProtectionUIHost.app"
)
for app in "${EXTRA_ICON_APPS[@]}"; do
    if [ -d "$OSMNT/Applications/$app" ]; then
        /usr/bin/rsync -aH --extended-attributes \
            --include='en.lproj/***' --include='Base.lproj/***' --include='English.lproj/***' --include='en_*.lproj/***' --exclude='*.lproj/' \
            --exclude='*.mov' --exclude='*.mlmodelc/' --exclude='*.espresso.*' \
            "$OSMNT/Applications/$app" "$ROOTMNT/Applications/"
        echo "  Added /Applications/$app"
    fi
done

UNCOMP_PATHS=(
    "/System/Library/CoreServices/CoreGlyphs.bundle"
    "/System/Library/CoreServices/MobileCoreTypes.bundle"
    "/System/Library/CoreServices/IconsetResources.bundle"
    "/System/Library/PrivateFrameworks/IconFoundation.framework"
    "/Applications/Preferences.app/Assets.car"
)
for up in "${UNCOMP_PATHS[@]}"; do
    if [ -d "$OSMNT$up" ]; then
        rm -rf "$ROOTMNT$up"
        /usr/bin/rsync -aH --extended-attributes \
            --include='en.lproj/***' --include='Base.lproj/***' --include='English.lproj/***' --include='en_*.lproj/***' --exclude='*.lproj/' \
            "$OSMNT$up/" "$ROOTMNT$up/"
    elif [ -f "$OSMNT$up" ]; then
        rm -f "$ROOTMNT$up"
        cp -p "$OSMNT$up" "$ROOTMNT$up"
    fi
done

COMP_LIST="$BASE/out/hfsx-compressed.list"
if [ -f "$COMP_LIST" ]; then
    "$PY" - "$COMP_LIST" <<'PY'
import sys
p = sys.argv[1]
prefixes = (
    '/System/Library/CoreServices/CoreGlyphs.bundle/',
    '/System/Library/CoreServices/MobileCoreTypes.bundle/',
    '/System/Library/CoreServices/IconsetResources.bundle/',
    '/System/Library/PrivateFrameworks/IconFoundation.framework/',
    '/Applications/Preferences.app/Assets.car',
)
lines = [l for l in open(p) if not any(l.strip().startswith(px) for px in prefixes)]
open(p, 'w').writelines(lines)
print(f"Filtered {p}: {len(lines)} compressed entries remain")
PY
fi

echo "=== [5/8] Restoring /Library/Wallpaper (fixing PosterBoard/CollectionsPoster WallpaperKit crash) ==="
rm -rf "$ROOTMNT/Library/Wallpaper"
mkdir -p "$ROOTMNT/Library/Wallpaper"
/usr/bin/rsync -aH --extended-attributes \
    --include='en.lproj/***' --include='Base.lproj/***' --include='English.lproj/***' --include='en_*.lproj/***' --exclude='*.lproj/' \
    "$OSMNT/Library/Wallpaper/" "$ROOTMNT/Library/Wallpaper/"

heic_cnt=0
while IFS= read -r -d '' hf; do
    if /usr/bin/sips -s format png "$hf" --out "$hf.png" >/dev/null 2>&1; then
        mv -f "$hf.png" "$hf"
        heic_cnt=$((heic_cnt + 1))
    fi
done < <(find "$ROOTMNT/Library/Wallpaper" -type f \( -iname "*.heic" -o -iname "*.heif" \) -print0)
echo "Converted $heic_cnt wallpaper HEIC layers to PNG"

echo "=== [6/8] Running 10-macho-a64e.sh, 70-lsd-extkit.sh, 70-ui-stability.sh, wiping stale icon caches, and check-root.sh ==="
DISABLE_HOGS=1 OS_MNT="$OSMNT" bash "$FR/steps.d/10-macho-a64e.sh" "$ROOTMNT"
DISABLE_HOGS=1 OS_MNT="$OSMNT" bash "$FR/steps.d/70-lsd-extkit.sh" "$ROOTMNT"
if [ -e "$ROOTMNT/usr/share/zoneinfo.default/Europe/Istanbul" ] || [ -e "$ROOTMNT/usr/share/zoneinfo/Europe/Istanbul" ]; then
    SCION_TZ="Europe/Istanbul" bash "$BASE/work/tools/ios27/ui/70-ui-stability.sh" "$ROOTMNT"
else
    bash "$BASE/work/tools/ios27/ui/70-ui-stability.sh" "$ROOTMNT"
fi

rm -rf "$ROOTMNT/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.iconservices"/* \
       "$ROOTMNT/private/var/mobile/Library/Caches/com.apple.IconsCache"* 2>/dev/null || true

bash "$FR/check-root.sh" "$ROOTMNT" | tee "$BASE/out/check-root.log"
grep -q "CHECK-ROOT: ALL HARD CHECKS PASSED" "$BASE/out/check-root.log"
sync
df -h "$ROOTMNT"
df -m "$ROOTMNT"

echo "=== [7/8] Detaching, applying offline Catalog B-Tree UID/GID/setuid/UF_COMPRESSED fixup, fsck_hfs, and 8 MiB tail pad ==="
detach_if "$OSMNT"
detach_if "$SYSMNT"
sync
hdiutil detach "$ROOTMNT"
ROOTMNT=""

"$PY" "$BASE/fix_hfsx_owners.py" "$OUT" "$BASE/hfsx-extra-rules.txt" "$COMP_LIST"

VDEV=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$OUT" | awk 'NR==1 {print $1}')
/sbin/fsck_hfs -f -y "/dev/r${VDEV#/dev/}" || true
/sbin/fsck_hfs -n -f "/dev/r${VDEV#/dev/}"
hdiutil detach "$VDEV"

dd if=/dev/zero of="$OUT" bs=1048576 count=0 seek=$((SIZE_GB * 1024 + 8)) 2>/dev/null
ls -lh "$OUT"

echo "=== [8/8] Streaming updated root-slim.img + payload-ios27-verify.bin to Galaxy A21s and running 95s verification boot ==="
/usr/bin/python3 "$BASE/recovered-a21s/a21s/tools/a21s-install.py" ios \
    --payload "$BASE/out/payload-ios27-verify.bin" --system "$OUT"
"$BASE/boot_ios27.sh"
mkdir -p "$BASE/out/last-boot"
/usr/bin/python3 "$BASE/dump_boot_logs.py" --wait 260 --out "$BASE/out/last-boot/metal-ca-verify.log"
echo "=== METAL-CA VERIFICATION SUMMARY ==="
grep -E "STARBITE MTL|MTL2\[|SBDRM\[|native-metal:|STARLTE GPU QUEUE|A5XELTE FB|SWEET FB CAV|decmpfs|PosterBoard|CRASH" "$BASE/out/last-boot/metal-ca-verify.log" | head -80 || true
echo "METAL-CA-VERIFY-DONE"
