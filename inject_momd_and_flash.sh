#!/usr/bin/env bash
set -euo pipefail
B=/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27
export PATH="$B/bin:/opt/homebrew/bin:$PATH"
export TOOLS_BIN="$B/bin"
export DEVELOPER_DIR=/Applications/Xcode_27.2.app/Contents/Developer
export KDKROOT="$B/fake-kdk/KDK.kdk"
export NOWIFI=1
PY="$B/venv/bin/python"
OUT="$B/out/root-slim.img"

echo "=== [1/4] Updating XNU kernel (sweetspeed=402910661, no scripted swipes) ==="
cd "$B/work"
OS_PROFILE=ios-27 tools/ios27/build-xnu-ios.sh -j8
K=$(OS_PROFILE=ios-27 tools/ios27/build-xnu-ios.sh --kernel-path)
cp -f "$K" "$B/work/hakios-kernel"
cp -f "$K" "$B/out/kernel.stripped"
xcrun strip -x "$B/out/kernel.stripped"
python3 "$B/work/tools/ios27/herolte/mkpayload.py" "$B/out/kernel.stripped" "$B/out/a21s.adt" "$B/out/a21s-ios27.sweetxnu" --cmdline ""
rm -f "$B/out/kernel.stripped"
git -C "$B/work/build/dxb-12377/xnu" diff > "$B/out/a21s-xnu-12377.patch"

for spec in "sweetreboot=150:$B/out/payload-ios27-verify.bin" "sweetwdog=180:$B/out/payload-ios27.bin"; do
  cmd="${spec%%:*}"
  dst="${spec#*:}"
  python3 - "$B/out/loader.efi" "$B/out/a21s-ios27.sweetxnu" "$cmd" "$dst" <<'PYEOF'
import struct, sys
loader = open(sys.argv[1], "rb").read()
sweet = open(sys.argv[2], "rb").read()
cmd = sys.argv[3].encode() + b"\0"
def ent(name, data):
    pad = (-len(data)) & 15
    return struct.pack("<16sII", name.encode(), len(data), 0) + data + b"\0" * pad
body = ent("loader.efi", loader) + ent("sweetxnu", sweet) + ent("cmdline", cmd)
out = struct.pack("<II", 0x50554E58, len(body)) + body
open(sys.argv[4], "wb").write(out)
print("packed", sys.argv[4], len(out), "cmdline=", sys.argv[3])
PYEOF
done

echo "=== [2/4] Injecting all missing .momd CoreData models into out/root-slim.img ==="
python3 -c "import os; os.truncate('$OUT', 10 * 1024**3)"

W=$(mktemp -d /tmp/a21s-momd.XXXXXX)
mkdir -p "$W/os" "$W/sys" "$W/app" "$W/root"
cleanup() {
  for m in "$W/root" "$W/os" "$W/sys" "$W/app"; do
    hdiutil detach "$m" >/dev/null 2>&1 || hdiutil detach -force "$m" >/dev/null 2>&1 || true
  done
  rm -rf "$W"
}
trap cleanup EXIT

hdiutil attach -readonly -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$W/os" "$B/inputs/os.dmg" >/dev/null
hdiutil attach -readonly -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$W/sys" "$B/inputs/systemos.dmg" >/dev/null
hdiutil attach -readonly -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$W/app" "$B/inputs/appos.dmg" >/dev/null
hdiutil attach -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$W/root" "$OUT" >/dev/null

python3 - "$W/os" "$W/sys" "$W/app" "$W/root" <<'PYEOF'
import os, shutil, sys

os_m, sys_m, app_m, root_m = sys.argv[1:5]
copied = 0

def copy_momds_from(src_root, dst_prefix=""):
    global copied
    for dirpath, dirnames, _ in os.walk(src_root):
        rel = os.path.relpath(dirpath, src_root)
        if rel.startswith(("private/var/staged_system_apps", "System/Library/NanoTimeKit", "System/Library/Health")):
            dirnames[:] = []
            continue
        for d in list(dirnames):
            if d.endswith(".momd"):
                src_d = os.path.join(dirpath, d)
                rel_d = os.path.relpath(src_d, src_root)
                # If under Applications/X.app, only copy if Applications/X.app exists in root_m
                if rel_d.startswith("Applications/"):
                    app_bundle = "/".join(rel_d.split("/")[:2])
                    if not os.path.isdir(os.path.join(root_m, app_bundle)):
                        dirnames.remove(d)
                        continue
                dst_d = os.path.join(root_m, dst_prefix, rel_d) if dst_prefix else os.path.join(root_m, rel_d)
                if not os.path.exists(dst_d):
                    os.makedirs(os.path.dirname(dst_d), exist_ok=True)
                    shutil.copytree(src_d, dst_d, symlinks=True)
                    copied += 1
                # If copied into Cryptexes, also link into /System or /usr if missing
                if dst_prefix:
                    top_d = os.path.join(root_m, rel_d)
                    if not os.path.exists(top_d):
                        os.makedirs(os.path.dirname(top_d), exist_ok=True)
                        shutil.copytree(src_d, top_d, symlinks=True)
                dirnames.remove(d)

copy_momds_from(os_m, "")
copy_momds_from(sys_m, "private/preboot/Cryptexes/OS")
copy_momds_from(app_m, "private/preboot/Cryptexes/App")

# Also check installed staged apps in root_m/Applications
for appname in ("MobileSafari.app", "Files.app", "Calculator.app", "MobileTimer.app", "MobileNotes.app", "Photos.app"):
    st_src = os.path.join(os_m, "private/var/staged_system_apps", appname)
    st_dst = os.path.join(root_m, "Applications", appname)
    if os.path.isdir(st_src) and os.path.isdir(st_dst):
        for dirpath, dirnames, _ in os.walk(st_src):
            for d in list(dirnames):
                if d.endswith(".momd"):
                    rel_sub = os.path.relpath(os.path.join(dirpath, d), st_src)
                    target = os.path.join(st_dst, rel_sub)
                    if not os.path.exists(target):
                        os.makedirs(os.path.dirname(target), exist_ok=True)
                        shutil.copytree(os.path.join(dirpath, d), target, symlinks=True)
                        copied += 1
                    dirnames.remove(d)

print(f"Copied {copied} .momd CoreData model directories into root-slim.img")
assert os.path.isdir(os.path.join(root_m, "System/Library/PrivateFrameworks/AccountsDaemon.framework/accounts.momd"))
assert os.path.isdir(os.path.join(root_m, "System/Library/PrivateFrameworks/AppProtection.framework/AppProtection.momd"))
PYEOF

df -h "$W/root"
sync
hdiutil detach "$W/root" >/dev/null
hdiutil detach "$W/os" >/dev/null
hdiutil detach "$W/sys" >/dev/null
hdiutil detach "$W/app" >/dev/null

echo "=== [3/4] Running offline Catalog B-Tree fix_hfsx_owners.py + fsck_hfs + 8 MiB herolog pad ==="
"$PY" "$B/fix_hfsx_owners.py" "$OUT" "$B/hfsx-extra-rules.txt" "$B/out/hfsx-compressed.list"
VDEV=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$OUT" | awk 'NR==1 {print $1}')
/sbin/fsck_hfs -f -y "/dev/r${VDEV#/dev/}" || true
/sbin/fsck_hfs -n -f "/dev/r${VDEV#/dev/}"
hdiutil detach "$VDEV" >/dev/null
dd if=/dev/zero of="$OUT" bs=1048576 count=0 seek=$((10 * 1024 + 8)) 2>/dev/null
ls -lh "$OUT"

echo "=== [4/4] Flashing updated root-slim.img + payload-ios27-verify.bin (sweetreboot=90) and booting ==="
python3 "$B/recovered-a21s/a21s/tools/a21s-install.py" ios \
  --payload "$B/out/payload-ios27-verify.bin" --system "$OUT"
"$B/boot_ios27.sh"
echo "DISPATCHED_BOOT_RUN_6_AT=$(date +%H:%M:%S)"
