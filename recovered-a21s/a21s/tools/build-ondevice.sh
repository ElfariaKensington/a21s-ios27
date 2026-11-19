#!/bin/sh
# Cross-build the static aarch64 helpers that run on the phone (needs zig: brew install zig)
set -e
cd "$(dirname "$0")/ondevice"
zig cc -target aarch64-linux-musl -static -O2 -o iosfile iosfile.c
