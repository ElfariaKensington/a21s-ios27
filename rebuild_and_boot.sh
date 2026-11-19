#!/usr/bin/env bash
# Fast 20s rebuild + pack + ADB payload push + one-shot iOS 27 boot
set -euo pipefail
B=/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27
CMDLINE="${1:-sweetreboot=75}"
OUT_PAYLOAD="${2:-$B/out/payload-ios27-verify.bin}"

export PATH="$B/bin:/opt/homebrew/bin:$PATH"
export TOOLS_BIN="$B/bin"
export DEVELOPER_DIR=/Applications/Xcode_27.2.app/Contents/Developer
export KDKROOT="$B/fake-kdk/KDK.kdk"
export NOWIFI=1

xcrun clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar -mgeneral-regs-only -O1 \
  -c "$B/a21s_ios27_loader.c" -o "$B/out/loader.obj"
/opt/homebrew/bin/lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 \
  /out:"$B/out/loader.efi" "$B/out/loader.obj"

cd "$B/work"
OS_PROFILE=ios-27 tools/ios27/build-xnu-ios.sh -j8
K=$(OS_PROFILE=ios-27 tools/ios27/build-xnu-ios.sh --kernel-path)
cp -f "$K" "$B/work/hakios-kernel"
cp -f "$K" "$B/out/kernel.stripped"
xcrun strip -x "$B/out/kernel.stripped"

python3 "$B/work/tools/ios27/herolte/mkpayload.py" "$B/out/kernel.stripped" "$B/out/a21s.adt" "$B/out/a21s-ios27.sweetxnu" --cmdline ""
rm -f "$B/out/kernel.stripped"

python3 - "$B/out/loader.efi" "$B/out/a21s-ios27.sweetxnu" "$CMDLINE" "$OUT_PAYLOAD" <<'PY'
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
print("wrote XNUP:", sys.argv[4], len(out), "bytes, cmdline=", sys.argv[3])
PY

python3 "$B/recovered-a21s/a21s/tools/a21s-install.py" ios \
  --payload "$OUT_PAYLOAD" --system-size 11282677760

python3 -c "import sys; sys.stdout.buffer.write(b'A21SBOOT\x02'+bytes(503))" > /tmp/a21s-flag.bin
adb push /tmp/a21s-flag.bin /data/local/tmp/a21s-flag.bin
adb shell 'su -c "dd if=/data/local/tmp/a21s-flag.bin of=/dev/block/by-name/boot bs=512 seek=73599 count=1 conv=notrunc && sync && echo aot_enable,0 > /sys/class/sec/tsp/cmd"'
adb reboot
echo "DISPATCHED_BOOT_AT=$(date +%H:%M:%S)"
