#!/bin/sh
# usage: boot-once.sh android|recovery|ios|touchtest   (phone in Android with Magisk su, or in OrangeFox)
# Sets a one-shot flag in the `boot` partition: the next boot of the first stage uses that entry, then forgets it.
case "$1" in android) n=0;; recovery) n=1;; ios) n=2;; touchtest) n=3;; *) echo "usage: $0 android|recovery|ios|touchtest"; exit 1;; esac
f=$(mktemp); python3 -c "import sys; sys.stdout.buffer.write(b'A21SBOOT'+bytes([$n])+bytes(503))" > "$f"
adb push "$f" /data/local/tmp/a21s-flag.bin >/dev/null
adb shell 'if [ "$(id -u)" = 0 ]; then S=; else S="su -c"; fi; $S dd if=/data/local/tmp/a21s-flag.bin of=/dev/block/by-name/boot bs=512 seek=73599 count=1 conv=notrunc 2>&1 | tail -1; sync'
# iOS: Android's touch driver leaves the Himax in its double-tap-wake sleep (SMART_WAKEUP) across the reboot, and then it
# answers the loader only ~175 s into iOS. Switched off for this shutdown only (Android sets it again when it starts).
[ "$n" = 2 ] && adb shell 'if [ "$(id -u)" = 0 ]; then S=; else S="su -c"; fi; $S "echo aot_enable,0 > /sys/class/sec/tsp/cmd; cat /sys/class/sec/tsp/cmd_result"'
rm -f "$f"
