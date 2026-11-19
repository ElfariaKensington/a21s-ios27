#!/bin/bash
# Unprivileged (zero-sudo) iOS 27.0.1 (24A446) slimmed 8.0 GiB HFSX root builder for Samsung Galaxy A21s (SM-A217F).
set -euo pipefail

BASE=/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27
FR=$BASE/work/tools/ios27/fullroot
PIPE=$FR/pipeline
PRIV=$FR/private
IN=$BASE/inputs
CACHE_SRC=$FR/work/cache/c
OUT=${1:-$BASE/out/root-slim.img}
SIZE_GB=${SIZE_GB:-10}
BUILD_VERSION=24A446

export DEVELOPER_DIR=/Applications/Xcode_27.2.app/Contents/Developer
export PY="$BASE/venv/bin/python"
export IOS27_PIPELINE="$PIPE"
export IOS27_PRIVATE="$PRIV"
export ENABLE_METAL_CA=0
export ENABLE_SILEO=1
export ENABLE_SENSORS=0
export ENABLE_WIFI=0
export ENABLE_AUDIO_BT=0
export ENABLE_MODEM=0
export ENABLE_CAMERA=0

# Create no-root shim bin for chown & install (-o/-g stripper)
SHIMBIN=$BASE/norootsbin
mkdir -p "$SHIMBIN" "$BASE/out"
cat > "$SHIMBIN/chown" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$SHIMBIN/install" <<'EOF'
#!/bin/sh
args=""
while [ $# -gt 0 ]; do
    case "$1" in
        -o|-g) shift 2 ;;
        *) args="$args \"$(printf '%s' "$1" | sed 's/"/\\"/g')\""; shift ;;
    esac
done
eval "exec /usr/bin/install $args"
EOF
chmod +x "$SHIMBIN/chown" "$SHIMBIN/install"
export PYTHONPATH="$BASE/norootpy"
export PATH="$SHIMBIN:$BASE/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"

[ -f "$FR/work/cache/DONE" ] || { echo "ERROR: cache pipeline not complete ($FR/work/cache/DONE missing)" >&2; exit 1; }
[ -f "$CACHE_SRC/dyld_shared_cache_arm64e" ] || { echo "ERROR: missing $CACHE_SRC/dyld_shared_cache_arm64e" >&2; exit 1; }

W=$BASE/mnt-work
OSMNT=$W/os
SYSMNT=$W/sys
APPMNT=$W/app
ROOTMNT=$W/root
mkdir -p "$OSMNT" "$SYSMNT" "$APPMNT" "$ROOTMNT"

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
    detach_if "$APPMNT"
}
trap cleanup EXIT

for m in "$ROOTMNT" "$OSMNT" "$SYSMNT" "$APPMNT"; do detach_if "$m"; done

echo "[1/8] Attaching 24A446 source DMGs unprivileged..."
hdiutil attach -readonly -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$OSMNT" "$IN/os.dmg" >/dev/null
hdiutil attach -readonly -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$SYSMNT" "$IN/systemos.dmg" >/dev/null
hdiutil attach -readonly -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$APPMNT" "$IN/appos.dmg" >/dev/null

echo "[2/8] Creating and formatting ${SIZE_GB}.0 GiB HFSX image ($OUT) with 64 MiB contiguous Catalog clump..."
rm -f "$OUT"
mkfile -n "${SIZE_GB}g" "$OUT"
RDEV=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$OUT" | awk 'NR==1 {print $1}')
/sbin/newfs_hfs -s -J -b 4096 -c c=16384 -v IOSROOT "/dev/r${RDEV#/dev/}"
detach_if "$RDEV"
RDEV=""
hdiutil attach -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$ROOTMNT" "$OUT" >/dev/null

echo "[3/8] Staging slimmed OS, SystemOS, AppOS, and Cortex-A55 rewritten dyld shared cache..."
KEEP_SYS_APPS=(
    PosterBoard PreBoard InputUI MediaRemoteUI ClockAngel PaperBoard Preferences
    SafariViewService WebSheet PhotosUIService ScreenshotServicesService SharingViewService
    Spotlight LocalAuthenticationUIService AuthenticationServicesUI AutoFillSuggestions
    StoreKitUIService StoreKitUISceneService ColorPickerUIService FontPickerUIService
    DockFolderViewService SpringBoardEducation TrustMe AccountAuthenticationDialog
    PassbookUIService DataActivation CTNotifyUIService
)
APP_FILTERS=('--include=/Applications/')
for a in "${KEEP_SYS_APPS[@]}"; do
    if [ -d "$OSMNT/Applications/$a.app" ]; then
        APP_FILTERS+=("--include=/Applications/$a.app/***")
    fi
done
APP_FILTERS+=('--exclude=/Applications/***')

OMIT_FILTERS=(
    '--exclude=/.file'
    '--include=en.lproj/***'
    '--include=Base.lproj/***'
    '--include=English.lproj/***'
    '--include=en_*.lproj/***'
    '--exclude=*.lproj/'
    '--exclude=*.mlmodelc/'
    '--exclude=*.espresso.*'
    '--exclude=*.weights'
    '--exclude=*.hwx'
    '--exclude=*.metallib'
    '--exclude=*.rtlib'
    '--exclude=*.mov'
    '--exclude=/private/var/staged_system_apps/***'
    '--exclude=/usr/standalone/***'
    '--exclude=/usr/share/tokenizer/***'
    '--exclude=/usr/share/morphun/***'
    '--exclude=/usr/libexec/Moments.framework/***'
    '--exclude=/Library/Wallpaper/***'
    '--exclude=/Library/Ringtones/***'
    '--exclude=/Library/Audio/Tunings/***'
    '--exclude=/System/Library/Audio/Tunings/***'
    '--exclude=/System/Library/LinguisticData/***'
    '--exclude=/System/Library/PreinstalledAssetsV2/***'
    '--exclude=/System/Library/NanoTimeKit/***'
    '--exclude=/System/Library/Assistant/***'
    '--exclude=/System/Library/Health/***'
    '--exclude=/System/Library/OnBoardingBundles/***'
    '--exclude=/System/Library/AppPlaceholders/***'
    '--exclude=/System/Library/ImagingNetworks/***'
    '--exclude=/System/Library/ProductDocuments/***'
    '--exclude=/System/Library/ScreenReader/***'
    '--exclude=/System/Library/AccessibilityBundles/***'
    '--exclude=/System/Library/NanoPreferenceBundles/***'
    '--exclude=/System/Library/Carrier Bundles/***'
    '--exclude=/System/Library/DriverExtensions/***'
    '--exclude=/System/Library/GeoServicesData/***'
    '--exclude=/System/Library/FlowTools/***'
    '--exclude=/System/Library/Fonts/CoreAddition/***'
    '--exclude=/System/Library/CoreServices/RawCamera.bundle/***'
    '--exclude=/System/Library/CoreServices/RawCameraBundles/***'
    '--exclude=/System/Library/CoreServices/BluetoothUIService.app/***'
    '--exclude=/System/Library/CoreServices/ClarityBoard.app/***'
    '--exclude=/System/Library/CoreServices/CarPlay.app/***'
)

/usr/bin/rsync -aH --extended-attributes "${OMIT_FILTERS[@]}" "${APP_FILTERS[@]}" "$OSMNT/" "$ROOTMNT/"

# Strip asset payloads inside unused ML/Watch/Vision/Siri frameworks while preserving Info.plist for NSBundle
UNUSED_FWS=(
    CoreEmoji MediaAnalysis VoiceServices GPUCompiler CVNLP iWorkImport NanoUniverse
    TextToSpeechMauiSupport GESS TextRecognition VisionCore SiriSetup CoreSceneUnderstanding
    AvatarKit SpotlightResources CoreHandwriting WeatherUI CoreRE CameraEffectsKit
    VisualLookUp SiriTTSService HomeUI HeadphoneSettingsUI HeadphoneConfigs HeadphoneAssets
    IntelligenceFlowPlannerSupport Moments PassKitUI WorkoutUI Morpheus AppleCVA AppleCV3D
    SceneIntelligence ImagePlaygroundInternal AltruisticBodyPoseKit Vision
    MetalPerformanceShaders MetalPerformancePrimitives PHASE CreateML CreateMLComponents
    MusicUnderstanding SoundAnalysis HealthKit TranslationDaemon
)
for fw in "${UNUSED_FWS[@]}"; do
    for sub in Frameworks PrivateFrameworks; do
        fwdir="$ROOTMNT/System/Library/$sub/$fw.framework"
        if [ -d "$fwdir" ]; then
            find "$fwdir" -mindepth 1 -maxdepth 1 ! -name 'Info.plist' -exec rm -rf {} +
        fi
    done
done

mkdir -p "$ROOTMNT/private/preboot/Cryptexes/OS" "$ROOTMNT/private/preboot/Cryptexes/App"
/usr/bin/rsync -aH --extended-attributes \
    --exclude="*.symbols" \
    --exclude="/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e*" \
    --include='en.lproj/***' --include='Base.lproj/***' --include='English.lproj/***' --include='en_*.lproj/***' --exclude='*.lproj/' \
    "$SYSMNT/" "$ROOTMNT/private/preboot/Cryptexes/OS/"

CDST="$ROOTMNT/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld"
mkdir -p "$CDST" "$ROOTMNT/System/Library/Caches/com.apple.dyld"

/usr/bin/rsync -aH --extended-attributes \
    --include='en.lproj/***' --include='Base.lproj/***' --include='English.lproj/***' --include='en_*.lproj/***' --exclude='*.lproj/' \
    "$APPMNT/" "$ROOTMNT/private/preboot/Cryptexes/App/"

# Hard-link Cryptex dylibs/frameworks into /System and /usr for ignition_level=1
for cryptex in OS App; do
    source="$ROOTMNT/private/preboot/Cryptexes/$cryptex"
    while IFS= read -r -d '' file; do
        relative=${file#"$source"/}
        [[ $relative == System/Library/Caches/com.apple.dyld/* ]] && continue
        target="$ROOTMNT/$relative"
        [[ -e $target || -L $target ]] && continue
        mkdir -p "${target%/*}"
        if [[ -L $file ]]; then
            ln -s "$(readlink "$file")" "$target"
        else
            ln "$file" "$target"
        fi
    done < <(find "$source" ! -type d -print0)
done

# Install selected staged apps into /Applications
for appname in MobileSafari Files Calculator MobileTimer MobileNotes Photos; do
    bundle="$OSMNT/private/var/staged_system_apps/$appname.app"
    if [ -d "$bundle" ]; then
        /usr/bin/rsync -aH --extended-attributes \
            --include='en.lproj/***' --include='Base.lproj/***' --include='English.lproj/***' --include='en_*.lproj/***' --exclude='*.lproj/' \
            --exclude='*.mlmodelc/' --exclude='*.espresso.*' --exclude='*.metallib' \
            "$bundle" "$ROOTMNT/Applications/"
        echo "Installed staged app: $appname.app"
    fi
done

# Create /private/var/db & systembag.kb
db="$ROOTMNT/private/var/db"
mkdir -p "$db/diagnostics" "$db/uuidtext" "$db/diagnostics/timesync" "$ROOTMNT/private/var/keybags" "$ROOTMNT/Library/Wallpaper/Collections"
chmod 0751 "$db/diagnostics"
chmod 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for dir in diagnostics uuidtext diagnostics/timesync; do
    : > "$db/$dir/.migrated"
    chmod 0644 "$db/$dir/.migrated"
done
chmod 0700 "$ROOTMNT/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
    '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
    '<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' \
    > "$ROOTMNT/private/var/keybags/systembag.kb"
chmod 0600 "$ROOTMNT/private/var/keybags/systembag.kb"

df -h "$ROOTMNT"

echo "[4/8] Running herolte-tune-ios-root (unprivileged) + non-cache steps.d..."
TUNE_UNPRIV="$BASE/out/herolte-tune-unpriv.sh"
sed 's/\[\[ $# != 2 || ${EUID} != 0 \]\]/[[ $# != 2 ]]/' "$PRIV/herolte-tune-ios-root.sh" > "$TUNE_UNPRIV"
bash "$TUNE_UNPRIV" "$ROOTMNT" "$BUILD_VERSION"

for step in \
    10-macho-a64e.sh \
    20-launchd.sh \
    30-overlay.sh \
    40-prefs.sh \
    50-xpc-disable.sh \
    60-sileo.sh \
    70-lsd-extkit.sh; do
    if [ -f "$FR/steps.d/$step" ]; then
        echo "--- Running step $step ---"
        DISABLE_HOGS=1 OS_MNT="$OSMNT" bash "$FR/steps.d/$step" "$ROOTMNT"
    fi
done

echo "[5/8] Pruning .loctable locales and applying decmpfs_lz4 (type-16) asset compression BEFORE staging 5.7 GiB dyld cache..."
COMP_LIST="$BASE/out/hfsx-compressed.list"
USED_MB=$(df -m "$ROOTMNT" | awk 'NR==2{print $3}')
CACHE_MB=$(du -sm "$CACHE_SRC" | awk '{print $1}')
echo "used now ${USED_MB} MiB + cache ${CACHE_MB} MiB = $((USED_MB + CACHE_MB)) MiB (budget $((SIZE_GB * 1024 - 700)) MiB)"
if [ $((USED_MB + CACHE_MB)) -gt $((SIZE_GB * 1024 - 700)) ]; then
    "$PY" "$BASE/compress_hfsx_lz4.py" "$ROOTMNT" "$COMP_LIST"
else
    "$PY" "$BASE/compress_hfsx_lz4.py" "$ROOTMNT" "$COMP_LIST" prune-only
fi
df -h "$ROOTMNT"

echo "[6/8] Installing Cortex-A55 rewritten dyld shared cache + running cache steps & check-root.sh..."
for f in "$CACHE_SRC"/dyld_shared_cache_arm64e*; do
    cp -p "$f" "$CDST/"
    ln "$CDST/${f##*/}" "$ROOTMNT/System/Library/Caches/com.apple.dyld/${f##*/}"
done
echo "Installed and hard-linked $(ls "$CDST" | wc -l | tr -d ' ') rewritten dyld cache files"

for step in 85-textdata-restore.sh 86-coreimage-sw-context.sh; do
    if [ -f "$FR/steps.d/$step" ]; then
        echo "--- Running step $step ---"
        OS_MNT="$OSMNT" bash "$FR/steps.d/$step" "$ROOTMNT"
    fi
done

bash "$FR/check-root.sh" "$ROOTMNT" | tee "$BASE/out/check-root.log"
grep -q "CHECK-ROOT: ALL HARD CHECKS PASSED" "$BASE/out/check-root.log"
sync
df -h "$ROOTMNT"
df -m "$ROOTMNT"

echo "[7/8] Detaching HFSX volume and applying offline Catalog B-Tree UID/GID/setuid/UF_COMPRESSED fixup..."
detach_if "$OSMNT"
detach_if "$SYSMNT"
detach_if "$APPMNT"
sync
hdiutil detach "$ROOTMNT"
ROOTMNT=""

"$PY" "$BASE/fix_hfsx_owners.py" "$OUT" "$BASE/hfsx-extra-rules.txt" "$COMP_LIST"

echo "[8/8] Verifying HFSX Catalog & B-Tree integrity with fsck_hfs -n -f..."
VDEV=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$OUT" | awk 'NR==1 {print $1}')
/sbin/fsck_hfs -n -f "/dev/r${VDEV#/dev/}"
hdiutil detach "$VDEV"

# herolog=1: the kernel's console ring (4 MiB + 4 KiB header) goes to the END of the root device, beyond the HFSX
# volume. Pad 8 MiB so it never touches the volume or its alternate header.
dd if=/dev/zero of="$OUT" bs=1048576 count=0 seek=$((SIZE_GB * 1024 + 8)) 2>/dev/null
ls -lh "$OUT"
echo "A21S-ROOT-SLIM-READY: $OUT"
