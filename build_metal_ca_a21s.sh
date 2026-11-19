#!/usr/bin/env bash
set -euo pipefail

BASE=/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27
SB=$BASE/work
W=$SB/build/metal27
export DEVELOPER_DIR=/Applications/Xcode_27.2.app/Contents/Developer
IOS_SDK=$(xcrun --sdk iphoneos --show-sdk-path)
export IOS_SDK
export PATH="/Users/elfaria/.rustup/toolchains/stable-aarch64-apple-darwin/bin:/Users/elfaria/.cargo/bin:/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/bison/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"

mkdir -p "$W"
if [ ! -e "$W/venv" ]; then
    ln -s "$SB/build/mesa-ios/venv" "$W/venv"
fi

echo "=== [1/3] Building libnative_metal_bridge.a (metal2vulkan + Starbite patches for iOS arm64) ==="
M2V_SRC=$SB/build/m2v/metal2vulkan
M2V_OUT=$SB/build/m2v-bridge
mkdir -p "$M2V_OUT" "$SB/build/m2v"
if [ ! -d "$M2V_SRC" ]; then
    git clone -q https://github.com/steelbrain/metal2vulkan "$M2V_SRC"
    git -C "$M2V_SRC" checkout -q 720ef1ce
    for p in "$SB"/tools/ios27/metal2/bridge/patches/*.patch; do
        echo "Applying $(basename "$p")..."
        git -C "$M2V_SRC" apply "$p"
    done
fi

/opt/homebrew/opt/llvm/bin/llvm-as -o "$M2V_OUT/runtime_examples.air" \
    "$M2V_SRC/crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll"
/opt/homebrew/opt/llvm/bin/llvm-as -o "$M2V_OUT/function_constant.air" \
    "$M2V_SRC/crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll"

cat > "$M2V_OUT/Cargo.toml" <<EOF
[package]
name = "native-metal-bridge"
version = "0.1.0"
edition = "2024"
rust-version = "1.98"
[workspace]
[lib]
crate-type = ["staticlib"]
path = "$SB/tools/ios27/metal2/bridge/NativeMetalBridge.rs"
[dependencies]
metal2vulkan = { path = "$M2V_SRC/crates/metal2vulkan", features = ["ash"] }
metal2vulkan-runtime = { path = "$M2V_SRC/crates/metal2vulkan-runtime" }
libc = "0.2"
sha2 = { version = "0.10", features = ["asm"] }
[profile.release]
panic = "unwind"
debug = 1
EOF

if [ ! -f "$M2V_OUT/ios/libnative_metal_bridge.a" ]; then
    NMB_FIXTURE_AIR="$M2V_OUT/runtime_examples.air" \
    NMB_FC_AIR="$M2V_OUT/function_constant.air" \
    IPHONEOS_DEPLOYMENT_TARGET=16.0 \
    MACOSX_DEPLOYMENT_TARGET=14.0 \
    CARGO_BUILD_JOBS=8 \
    cargo build --release --manifest-path "$M2V_OUT/Cargo.toml" --target aarch64-apple-ios \
        --target-dir "$M2V_OUT/target" > "$M2V_OUT/build-ios.log" 2>&1 || {
            tail -60 "$M2V_OUT/build-ios.log"
            exit 1
        }
    mkdir -p "$M2V_OUT/ios"
    cp "$M2V_OUT/target/aarch64-apple-ios/release/libnative_metal_bridge.a" "$M2V_OUT/ios/"
fi
ls -lh "$M2V_OUT/ios/libnative_metal_bridge.a"
echo "BRIDGE-OK"

echo "=== [2/3] Building Mesa 26.2.6 PanVK (Bifrost v6/v7 Mali-G52 + starbite-drm) ==="
mkdir -p "$W/host/lib/pkgconfig"
for pc in \
    /opt/homebrew/Cellar/libclc/*/share/pkgconfig/*.pc \
    /opt/homebrew/Cellar/spirv-headers/*/share/pkgconfig/*.pc \
    /opt/homebrew/Cellar/spirv-llvm-translator/*/lib/pkgconfig/*.pc \
    /opt/homebrew/Cellar/spirv-tools/*/lib/pkgconfig/*.pc; do
    [ -f "$pc" ] && ln -sf "$pc" "$W/host/lib/pkgconfig/$(basename "$pc")"
done

W="$W" SB="$SB" IOS_SDK="$IOS_SDK" DEVELOPER_DIR="$DEVELOPER_DIR" \
    bash "$SB/tools/ios27/metal/mk-panvk27.sh"

echo "=== [3/3] Packaging StarbiteMetal.bundle (all 7 frontend patches + TurnShaders.metallib) ==="
W="$W" SB="$SB" DEVELOPER_DIR="$DEVELOPER_DIR" \
    sh "$SB/tools/ios27/metal/mk-bundle27.sh"

echo "=== Verifying Bifrost v7 (Mali-G52) entrypoints in libvulkan_panfrost.dylib ==="
nm "$W/stage/StarbiteMetal.bundle/libvulkan_panfrost.dylib" | grep -E "panvk_v7|vkGetInstanceProcAddr" | head -15 || true
otool -L "$W/stage/StarbiteMetal.bundle/StarbiteMetal" "$W/stage/StarbiteMetal.bundle/libvulkan_panfrost.dylib"
echo "METAL-CA-BUNDLE-READY: $W/stage/StarbiteMetal.bundle"
