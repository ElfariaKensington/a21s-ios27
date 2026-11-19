# Engineering Specification: iOS 27 (`24A446`) on Samsung Galaxy A21s (`SM-A217F`, Exynos 850)

- **Target OS Build**: iOS 27.0.1 (`24A446`, donor `iPhone12,1` / `N104AP`, `xnu-12377.121.6`, `RC_DARWIN_KERNEL_VERSION=25.5.0`)
- **Target Hardware**: Samsung Galaxy A21s (`SM-A217F`, SoC Exynos 850 / `S5E3830`)
  - **CPU**: 8x ARM Cortex-A55 (`MIDR_EL1` implementer `0x41`, part `0xd05`, ARMv8.2-A with native LSE, LRCPC, FP16 scalar+SIMD, RDM, DotProd, PAN; **no** PAC/BTI, **no** Mongoose M3 ARMv8.0 cores) in 2x 4-core DynamIQ clusters (`CL0` + `CL1`)
  - **GPU**: ARM Mali-G52 MP1 (`GPU_ID = 0x72120000`, Bifrost v7 arch 7.2 r0p0, 1 shader core, `MMU_FEATURES = 0x2830`) at `0x11500000`
  - **Display**: Bootloader GOP scanout at `0xfa000000`, `720x1600` XRGB8888, stride `2880` (`DECON_F` `0x13060000`, `DSIM` `0x13070000`)
  - **Storage**: DesignWare MSHC eMMC (`dwmmc0`) at `0x12100000` (`HS400`, 8-bit, `FIFO_DEPTH = 0x40`, `DATA = 0x200`), backing `/data/ios/system` via a pinned F2FS extent map
  - **Input**: Himax `HX83112A` (`ICID = 0x83112a00`) on `SPI1` (`USI_CMGP0` `0x11d00000`, GPIO CS `gpm3-0`, active-low IRQ `gpa0-0`) + Volume keys (`gpa0-7` Vol+, `gpa1-0` Vol-)
- **Reference Trees Inspected**:
  - `/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/refs/scion-kernel` (`Scion-Project/scion-kernel`)
  - `/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/refs/scion-xnu` (`Scion-Project/xnu` @ `43d4a33e0543a7d9a78ee029189f3939190642d1`, plus `ce48db2d0f179abffe6626167356287f141a1b36`)
  - `/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/refs/darwin-xnu-build` (`blacktop/darwin-xnu-build` @ `663906528d1cd5ea8fde7de6f16de73db9511549`)
  - `/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/refs/hakios-arm64e-dada` (`superboyyy/hakios-arm64e-dada`)
  - `/Users/elfaria/a21s-hw/{fdt.dts,iomem.txt,interrupts.txt,misc.txt}` and `/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/device-backup/payload.ios16.bak`

---

## 1. Building `xnu-12377.121.6` (`OS_PROFILE=ios-27`) on Xcode 27.2 Without KDK or `sudo`

### 1.1 Host Toolchain & Dependency Audit

| Requirement | Host Status on This Mac | Action Required |
|---|---|---|
| Xcode & SDKs | `/Applications/Xcode_27.2.app` (`clang-2100.4.1.1`, `iPhoneOS27.2.sdk`, `MacOSX27.2.sdk`) | Export `DEVELOPER_DIR=/Applications/Xcode_27.2.app/Contents/Developer` |
| Apple KDK (`/Library/Developer/KDKs`) | Absent (and public SDKs omit `Kernel.framework/PrivateHeaders`) | Bypass with empty directory `KDKROOT=$WORK/fake-kdk` + `scion-vmapple` headers (Section 1.3) |
| `jq`, `cmake`, `ninja`, `git`, `python3`, `meson`, `lld-link` | Installed (`/usr/bin` & `/opt/homebrew/bin`) | None |
| `gum`, `xcodes` | Missing | Place 2-line `#!/bin/sh\nexit 0` shims in `/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/bin` and pass `TOOLS_BIN=.../bin` |
| Python `numpy` (and `zig` for UEFI/sboot loader) | `numpy` installed in `/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/venv` (`python3.14`); `ziglang` optional via `venv/bin/pip install ziglang` | Export `PY=/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/venv/bin/python3` |
| Broadcom PCIe Wi-Fi (`brcmfmac.o`) | S9-only (`BCM4361`); A21s uses Samsung SCSC WLBT (`119c0000.scsc_wifibt`) | Always pass `NOWIFI=1` to `build-xnu-ios.sh` / `make kernel` |

### 1.2 Every Reference to `KDKROOT`, `gum`, `xcodes`, and `Xcode 16.4` / `18.5`

1. **`scion-kernel/tools/ios27/dxb-common.sh`**:
   - **Line 20**: Prepends `${TOOLS_BIN:-}` to `PATH`.
   - **Lines 23–24**: Checks `[ -n "${KDKROOT:-}" ]` and `[ -d "$KDKROOT" ]` (only tests directory existence, **not** contents).
   - **Lines 26–27**: Loops `for tool in jq gum xcodes cmake ninja git python3; do command -v "$tool" >/dev/null 2>&1 || die ...`.
   - **Line 30**: Exports `MACOS_VERSION=26.5` and `SKIP_PATCHES=1`.
2. **`darwin-xnu-build/build.sh` (pinned commit `663906528d1cd5ea8fde7de6f16de73db9511549` in `tools/ios27/dxb.pin`)**:
   - **Line 69 (`setup_xcode_toolchain`)**: Resets `PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:$PATH"`. Note `install_deps` runs at line `854` **before** `setup_xcode_toolchain` at line `855`, so `TOOLS_BIN` in `PATH` satisfies `install_deps`!
   - **Line 167 (`install_deps`)**: Checks `for cmd in jq gum xcodes cmake ninja`; if missing, attempts `brew install`. Satisfied by `TOOLS_BIN` shims.
   - **Lines 185–191 (`install_deps`)**: Only invokes `xcodes` when `/Applications/Xcode*.app` does **not** match. Because `/Applications/Xcode_27.2.app` exists, `xcodes` is never executed.
   - **Lines 209–210 (`choose_xnu`)**: Only invokes `gum choose` if `[ -z "$MACOS_VERSION" ]`. Because `dxb-common.sh` sets `MACOS_VERSION=26.5`, `gum` is never executed.
   - **Lines 383–401 (`choose_xnu`)**: Honors `KDKROOT` from the environment; **only** downloads `KDK.dmg` and calls `sudo installer -pkg` if `[ ! -d "$KDKROOT" ]`. Pointing `KDKROOT` at any existing empty directory completely skips KDK download and `sudo`!
   - **Lines 552, 701, 721, 752 (`xnu_headers`, `build_xnu`, `build_xnu_library`)**: Check `[ ! -d "${KDKROOT}" ]` and pass `KDKROOT="${KDKROOT}"` to XNU `make`. Note `xnu_headers` installs XNU's own `Kernel.framework/PrivateHeaders` into `${FAKEROOT_DIR}` for userland helper libs (`libsyscall`, `libfirehose_kernel`, `libkxld`), so no macOS KDK headers are needed there either.
3. **`scion-kernel/tools/ios27/dxb-ios.patch`**:
   - Patches `darwin-xnu-build/build.sh` `build_xnu()` to build `SDKROOT=iphoneos ARCH_CONFIGS=ARM64 KERNEL_CONFIGS=DEVELOPMENT MACHINE_CONFIGS="${XNU_MACHINE}" KDKROOT="${KDKROOT}" RC_DARWIN_KERNEL_VERSION=25.5.0 BUILD_WERROR=0 DO_CTFMERGE=0 -k`.
   - Passing `RC_DARWIN_KERNEL_VERSION=25.5.0` explicitly bypasses `scion-xnu/makedefs/MakeInc.kernel:296`, which would otherwise try to read `$(KDKROOT)/System/Library/Extensions/System.kext/Info.plist`.
4. **`scion-kernel/xnu/stage/00_setup_coreentitlements.sh` (invoked via `fork_stage` in `profiles/ios-27.conf:19` by `xnu/fork.sh:21`)**:
   - **Lines 28–31 & 61–64**: Hard-fails if `$KDKROOT/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/CoreEntitlements/V2/{API.h,Kernel.h}` is missing!
   - **Bypass**: Replace `00_setup_coreentitlements.sh` (or remove `fork_stage 00_setup_coreentitlements.sh` in `profiles/ios-27.conf`) and expose `EXTERNAL_HEADERS/scion-vmapple` directly (Section 1.3).
5. **`scion-kernel/tools/ios27/firehose-ios.py`**:
   - **Line 19**: Uses `DEVELOPER_DIR` from env (only falls back to `/Applications/Xcode_18.5.app/...` if unset). Reads `xcrun --sdk iphoneos --show-sdk-version` -> `27.2`, emitting Mach-O `LC_BUILD_VERSION` `0x001b0200` (`minos 27.2`) matching XNU's `.o` files under Xcode 27.2 so `ld` emits zero platform/version mismatch errors.
6. **Hardcoded `Xcode_16.4.app` in `tools/ios27/*` helper scripts**:
   - `tools/ios27/mesa-ios/mk-mesa-ios.sh:16` and `tools/ios27/perf/build-sbgl.sh:10` respect `${DEVELOPER_DIR:-...}`.
   - `tools/ios27/ca-gpu/build-sbca.sh:13`, `tools/ios27/gpu/build-sbgl-test.sh:8`, and `tools/ios27/gpu/build-sbgpu-test.sh:6` unconditionally assign `DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer` — use `build-sbca27.sh` (which honors `DEVELOPER_DIR`) or change `=` to `=${DEVELOPER_DIR:-...}`.

### 1.3 Open-Source `CoreEntitlements/V2`, `CodeSignature`, and `TrustCache` Wiring (Zero KDK)

`Scion-Project/xnu` commit `43d4a33e` **already contains** complete open-source stand-in headers under `EXTERNAL_HEADERS/scion-vmapple/` (identical to `hakios-arm64e-dada/kernel/xnu-12377.121.6-dada.patch` lines 1–850 and `Scion-Project/xnu` commit `ce48db2d0f179abffe6626167356287f141a1b36`):
- `EXTERNAL_HEADERS/scion-vmapple/CodeSignature/Entitlements.h` (included by `bsd/kern/kern_exec.c:190`, `bsd/net/necp_client.c:79`)
- `EXTERNAL_HEADERS/scion-vmapple/CoreEntitlements/V2/API.h` (`typedef struct CEKernelAPI CEKernelAPI_t; typedef struct CEContext CEContext_t;`) & `CoreEntitlements/V2/Kernel.h` (included by `libkern/libkern/amfi/amfi.h:35-36`; `CoreEntitlementsPriv.h` is already in `EXTERNAL_HEADERS/CoreEntitlements/CoreEntitlementsPriv.h`)
- `EXTERNAL_HEADERS/scion-vmapple/TrustCache/{API.h,RawTypes.h,Return.h,Types.h,TypesConfig.h}` (included by `osfmk/vm/pmap_cs.h:82`, `bsd/kern/kern_trustcache.c:49`, `bsd/sys/trust_caches.h:30`, `libkern/libkern/amfi/amfi.h:138`)
- `EXTERNAL_HEADERS/iBoot/boot_args_abi.h` is already at `EXTERNAL_HEADERS/iBoot/boot_args_abi.h`.

**Why `STARLTE` missed them**: `scion-xnu/makedefs/MakeInc.def:1265-1267` excludes `STARLTE` to avoid shadowing a real KDK:
```makefile
# scion-xnu/makedefs/MakeInc.def:1265-1267 — REMOVE STARLTE FROM FILTER (or copy scion-vmapple/* into EXTERNAL_HEADERS/)
ifeq ($(filter SWEET A5XELTE HEROLTE FRANKEL,$(CURRENT_MACHINE_CONFIG)),)
INCFLAGS_EXTERN += -I$(SRCROOT)/EXTERNAL_HEADERS/scion-vmapple
endif
```
With empty `KDKROOT`, `MakeInc.def:1188` already skips closed `lib<BOARD>.os.a` for `STARLTE` (compiling open-source `nos_arm_asm` + `nos_arm_pmap`), and `MakeInc.def:1231` (`$(wildcard $(KDKROOT)/.../libTightbeam.a)`) evaluates empty so `XNU_CONFIG_TIGHTBEAM` stays off.

### 1.4 Exact Zero-KDK, Zero-`sudo` Kernel Build Recipe

```bash
export WORK=/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27
export DEVELOPER_DIR=/Applications/Xcode_27.2.app/Contents/Developer
export KDKROOT="$WORK/fake-kdk"
export TOOLS_BIN="$WORK/bin"
export PY="$WORK/venv/bin/python3"
mkdir -p "$KDKROOT" "$TOOLS_BIN"
printf '#!/bin/sh\nexit 0\n' > "$TOOLS_BIN/gum"
printf '#!/bin/sh\nexit 0\n' > "$TOOLS_BIN/xcodes"
chmod +x "$TOOLS_BIN/gum" "$TOOLS_BIN/xcodes"

# In $WORK/work (clone of scion-kernel):
# 1. Disable KDK CoreEntitlements check in profiles/ios-27.conf (comment out `fork_stage 00_setup_coreentitlements.sh`)
# 2. Point fork_repo / xnu tree at $WORK/refs/scion-xnu with A21s patches + MakeInc.def:1265 fix applied
cd "$WORK/work"
NOWIFI=1 OS_PROFILE=ios-27 make kernel JOBS=8
# Output: $WORK/work/hakios-kernel (Mach-O ARM64, mach.development.starlte)
```
Note: `tools/make-kernel.sh:33-48` expects `make -k` to return exit status `2` at the very end on kext `.symbolset` / `check_all_exports` checks and validates that `mach.development.starlte` linked cleanly with zero compile/link errors before copying it to `./hakios-kernel`.

---

## 2. Boot Chain, DeviceTree, Memory Map, SMP, Timer, Log, Storage, Display, Touch, GPU & ACPM

### 2.1 Complete Exynos 9810 (`starlte`) -> Exynos 850 (`SM-A217F` / `a21s`) Hardware Translation Table

| Subsystem | S9 (`starlte` / Exynos 9810) in `scion-xnu` | A21s (`SM-A217F` / Exynos 850) Hardware Truth | Files in `scion-xnu` / `scion-kernel` to Update |
|---|---|---|---|
| **SoC I/O Window** | `0x10000000` (`size 0x10000000`) | `0x10000000` (`size 0x10000000`) | `mkadt_starlte.py` (`/arm-io/ranges`) |
| **CPU GIC (GIC-400 / GICv2)** | `GICD = 0x10101000`, `GICC = 0x10102000` (`HAS_GIC_V3 = 0`) | **`GICD = 0x12a01000` (`0x1000`), `GICC = 0x12a02000` (`0x1000`)** (`gic@12a00000`, `HAS_GIC_V3 = 0`). **WARNING**: `iomem.txt` `0x14af1000`/`0x14af2000` is **`abox_gic@14af0000`** (audio DSP GIC inside `pd-aud`), **NOT** the CPU GIC! | `mkadt_starlte.py` (`/arm-io/gic` `reg = (0x02a01000, 0x1000, 0x02a02000, 0x1000)`), `hakios.exynos_cpufreq.c:555` |
| **Generic Timer / MCT** | Arch timer `26,000,000` Hz (`0x18cba80`), virt PPI INTID `27`; MCT `0x10040000` | Arch timer **`26,000,000` Hz (`0x18cba80`)**, virt PPI **INTID `27`** (`CNTVCT_EL0`); MCT `0x10040000` global FRC already running from S-Boot; CL0/CL1 watchdogs `0x10050000`/`0x10060000` (`wdt=-1`) | No change to `timebase-frequency` or timer IRQ `27` in `mkadt_starlte.py` |
| **PMU Reset & `PS_HOLD`** | `PMU = 0x14060000`, `INFORM2/3 = +0x808/+0x80c`, `SWRESET = +0x400` (`val = 1`), `PS_HOLD = +0x330c` (bit 8) | **`PMU_ALIVE = 0x11860000`**, `INFORM2 = +0x0808`, `INFORM3 = +0x080c` (`0x12345678` / `0x12345674` recovery, `0x12345670` normal), **`SWRESET = +0x3a00` (`val = 2`, `exynos,reboot-v1`)**, **`PS_HOLD_CONTROL = +0x030c`** (bit 8, same page `0x11860000`!) | `bsd/dev/herolte_ufs.c:1074-1241` (`STARLTE_PMU_PA`, `STARLTE_PS_HOLD_PAGE`, `pmu[0x3a00/4] = 2`), `xnuloader` `platform_a21s` |
| **Panic / `pstore` RAM Ring** | `exynos_ss` `0xFDA00000`, `STARLTE_PMSG_PA = 0xFED14000` (`0x4000`) | `debug_snapshot` `0xf0000000..0xf1b17fff`: **`log_pstore` = `0xf0f10000` (`0x8000` B)** (`log_kernel` = `0xf0010000`) | `bsd/dev/herolte_ufs.c:1075` (`STARLTE_PMSG_PA = 0xf0f10000`), `mkadt_starlte.py` (`PRAM_BASE`) |
| **UART** | `0x13820000` (`serial0`) | **`uart@13820000`** (`USI_UART`, `compatible = "uart-1,samsung"`, SPI `214` / INTID `246`, `/arm-io/uart0` `reg = (0x03820000, 0x100)`); keep `serial=3` on `a5xelte-fbcon` unless physical UART jig is attached | `mkadt_starlte.py`, `pexpert/arm/pe_serial.c` |
| **GOP Framebuffer** | `0xcc000000`, `1440x2960`, stride `5760`, `9` x 2-MiB blocks; `s6e3ha8` on `DSIM0 0x16080000` / `DECON0 0x16030000` | **`0xfa000000` (`fb_handover` `0xfa000000 + 0x1400000`), `720x1600` XRGB8888, stride `2880` (`4,608,000` B = `3` x 2-MiB blocks)**; `decon_f@13060000`, `dsim@13070000` (leave `panel=0`, `sweetfb_dpp=0`) | `pexpert/pexpert/arm64/A5XELTE.h`, `osfmk/arm64/start.s`, `mkadt_starlte.py`, `bsd/dev/starlte_gpu.c:170-173,760-761` |
| **CPU Cores & `MPIDR_EL1`** | `4x A55` (`0x81000000..300`) + `4x M3` (`0x80000100..103`); PSCI `0x000..0x003, 0x100..0x103` | **`8x DynamIQ Cortex-A55`**: `CL0 = 0x81000000..0x81000300`, **`CL1 = 0x81010000..0x81010300`** (`MT=1` on BOTH clusters!); PSCI `CPU_ON` (`0xC4000003`) `target_cpu` = **`0x000..0x003, 0x100..0x103`** = `(MPIDR_EL1 >> 8) & 0xFFFF` | `osfmk/arm64/start.s:148-155`, `osfmk/arm64/machine_routines.c:1467-1477`, `iokit/Kernel/arm/SweetCPU.cpp:139-144,435-445`, `A5XELTE.h:35`, `mkadt_starlte.py` |
| **Root Storage** | UFS `0x11120000` + UFSP `0x11130000` (`herolte_ufs.c`), microSD `0x11500000` (`starlte_sd.c`) | **eMMC `dwmmc0@12100000`** (`0x2000`, SPI `172`/INTID `204`, `FIFO_DEPTH=0x40`, `DATA=0x200`, 8-bit HS400) + **64 KiB extent map at `boot` partition tail (`0x23f0000`)** mapping `/data/ios/system` (`userdata` GPT LBA + F2FS physical extents); microSD is `dwmmc2@13500000`. **CRITICAL**: S9's `SD_PA = 0x11500000` is A21s's **Mali-G52 GPU** — keep `sdprobe=0` / `sdroot=""`! | New `bsd/dev/a21s_emmc_extents.c` (replacing `herolte_ufs.c` root `bdevsw`/`cdevsw` as `ufsroot0`) + disable `starlte_sd.c` on `0x11500000` |
| **Touch & Buttons** | Samsung `sec_ts` I2C `0x48` on `HSI2C 0x104b0000` (`starlte_touch.c`) | **Himax `HX83112A` (`0x83112a00`) on `SPI1` (`USI_CMGP0` `0x11d00000`)**: `CMU_CMGP 0x11c00000`, `USIMODE 0x11c22000 = 2`, `GPIO_CMGP 0x11c30000` (`gpm0/1/2-0` fn 2, CS `gpm3-0` out), `GPIO_ALIVE 0x11850000` (IRQ `gpa0-0` active-low, Vol+ `gpa0-7`, Vol- `gpa1-0`) | Core-0 `A21SMBOX` poller in `loader.efi` + kernel `a21s_input` ring consumer into `starlte_touch.c` HID (`SweetHIDTouch`) — or kernel-native SPI1 thread |
| **GPU (Mali Bifrost)** | Mali-G72 MP18 (`0x6221`, Bifrost v6) @ `0x17500000`, `PMU +0x4040/+0x4044`, `CMU_G3D 0x17400000`, `DTZPC 0x17410204`, `AS_TRANSCFG=0` (legacy) | **Mali-G52 MP1 (`0x72120000`, Bifrost v7, 1 shader core) @ `0x11500000`**, `PMU_ALIVE +0x1d00/+0x1d04` (bit 0 = `1`), `CMU_TOP 0x120e0000`, `CMU_G3D 0x11400000`, `DTZPC 0x11410204` (`SMC 0x82000410`), IRQs SPI `128/129/130` (INTID `160/161/162`), **`AS_TRANSCFG = 0x10001c6` (AArch64 4K 4-level)** | `bsd/dev/starlte_gpu.c`, `iokit/Kernel/arm/StarbiteMali.cpp`, `tools/ios27/gpu/starlte-g72-tri.c` |
| **ACPM / PMIC** | `MBOX = 0x14100000`, `SRAM = 0x02039000` (`0x30000`), `INITDATA = 0x7f00`, S2MPS18 BUCK6 `0x2b` | **`MBOX = 0x11900000`**, `SRAM = 0x02039000` (`0x30000`, SPI `49`/INTID `81`), **`INITDATA = 0x7000`** (`fdt.dts:6712`); S2MPU12 `vdd_gpu` (BUCK2) is always-on from S-Boot (**do NOT write S2MPS18 reg `0x2b`**!) | `socs/exynos850/include/hakios.exynos850_soc.h`, `bsd/dev/starlte_touch.c:144`, `bsd/dev/hakios.exynos_acpm_dvfs.c`, `bsd/dev/starlte_gpu.c` |

---

### 2.2 Physical Memory Map & Mandatory Relocation of `0x8f.......` Carveouts

From `/Users/elfaria/a21s-hw/iomem.txt` and `/Users/elfaria/a21s-hw/fdt.dts`, the 4 GB SM-A217F has two 2 GiB physical DRAM banks (`0x80000000–0xffffffff` and `0x880000000–0x8ffffffff`; 6 GB variants extend bank 2 to `0x97fffffff`):

- **CRITICAL MEMORY CORRUPTION / SERROR TRAP IN `STARLTE`**:
  On A21s, **`0x8eb400000–0x8ffffffff` (332 MiB at top of Bank 2) is RESERVED** by TrustZone/RKP/secure firmware (`iomem.txt:108-118`).
  Unmodified `STARLTE` hardcodes:
  - `mkadt_starlte.py`: `BANK2_MB = 1536` (`0x880000000–0x8e0000000`, safe) + `PRAM_BASE = 0x8f8000000` (**CRASHES in TZ reserved RAM!**)
  - `bsd/dev/starlte_gpu.c`: `SG_MEM_PA = 0x8f0000000`, `SG_HEAP_PA = 0x8f1000000`, `SGU_PT_PA = 0x8f2000000` (**ALL THREE CRASH in TZ reserved RAM!**)
  - `adt-extra-ram.py`: includes `0xc0080000–0xcc000000` (crosses A21s's **`0xbc900000–0xbfffffff` secure hole** and **`0xc3000000–0xc307ffff` `seclog_mem`**!) and `0xd0000000–0xd8000000` (hits A21s's **`0xd0000000–0xd87fffff` `cp_rmem` modem carveout**!).

#### Exact A21s Physical Memory Layout (`mkadt_a21s.py` + `adt-extra-ram-a21s.py` + `starlte_gpu.c`)

1. **High Bank (`0x880000000–0x8eb3fffff`, `1,716 MiB` unreserved)**:
   - `0x880000000 – 0x8dfffffff` (**`1,536 MiB`**): Primary XNU `/chosen` `dram-base = 0x880000000`, `dram-size = 0x60000000` (kernel Mach-O + ADT + `boot_args` loaded by `xnuloader` at `0x883004000+`).
   - `0x8e0000000 – 0x8e005ffff` (`384 KiB`): **`SG_MEM_PA = 0x8e0000000ULL`** (`starlte_gpu.c:164` AS0 page tables & data).
   - `0x8e1000000 – 0x8e1ffffff` (`16 MiB`): **`SG_HEAP_PA = 0x8e1000000ULL`** (`starlte_gpu.c:175` AS0 tiler heap).
   - `0x8e2000000 – 0x8e23fffff` (`4 MiB`): **`SGU_PT_PA = 0x8e2000000ULL`** (`starlte_gpu.c:1086` AS1 userland Mali LPAE/AArch64 page tables).
   - `0x8e8000000 – 0x8e87fffff` (`8 MiB`): **`PRAM_BASE = 0x8e8000000ULL`** (`mkadt_a21s.py` `/pram`).
   - `0x8eb400000 – 0x8ffffffff`: **Untouched (TZ/RKP reserved)**.

2. **Low Bank (`0x80000000–0xffffffff`) Safe `hakios-extra-ram` Ranges (`extra_ram=1`)**:
   Every range below is 16 KiB-aligned and strictly clear of all `iomem.txt`/`fdt.dts` reserved regions, the SMP trampoline (`0x8fffe000`), and core-0's `loader.efi` / `payload_home` (`0xa0000000–0xb3ffffff` if `bootcpu=1`):
   - `(0x80200000, 0x8ff00000)` — `253 MiB` (skips `0x80000000` `sec_debug_magic`, stops below `0x8fffe000` PSCI trampoline & `0x90000000` reserved)
   - `(0x91400000, 0xa0000000)` — `236 MiB` (above `0x91200000+0x200000` `sec_debug_next`, stops below UEFI/loader heap `0xa0000000`)
   - `(0xb4000000, 0xbc8fc000)` — `136.9 MiB` (above `0xb0000000..0xb3ffffff` `payload_home`, stops below `0xbc900000` secure hole)
   - `(0xc0000000, 0xc3000000)` — `48 MiB` (below `0xc3000000` `seclog_mem`)
   - `(0xc3080000, 0xd0000000)` — `207.5 MiB` (between `seclog_mem` and `0xd0000000` `cp_rmem`)
   - `(0xd8800000, 0xdb900000)` — `49 MiB` (between `cp_rmem` and `0xdb900000` `camera_contig`)
   - `(0xdc400000, 0xe0000000)` — `60 MiB` (above `camera_contig`, below `0xe0000000` hole)
   - `(0xe3600000, 0xe6c00000)` — `54 MiB` (between `secure_camera` and `abox_slog`)
   - **Total RAM given to XNU**: `1,536 MiB` (Bank 2) + **`1,044 MiB`** (`hakios-extra-ram`, 8 ranges, `extra_ram_mask=255`) = **`2,580 MiB` (`2.52 GiB`) usable RAM** on 4 GB A21s!

---

### 2.3 SMP Bring-Up on 8x DynamIQ Cortex-A55 (`start.s`, `machine_routines.c`, `SweetCPU.cpp`)

On Exynos 850, **both** 4-core clusters (`CL0` and `CL1`) are ARMv8.2 DynamIQ (`MT = 1`, bit 24 set), placing the core index (`0..3`) in `Aff1` (`bits[15:8]`) and the cluster index (`0..1`) in `Aff2` (`bits[23:16]`):
- Hardware `MPIDR_EL1`: `CL0` = `0x81000000, 0x81000100, 0x81000200, 0x81000300`; `CL1` = `0x81010000, 0x81010100, 0x81010200, 0x81010300`.
- PSCI `CPU_ON` (`0xC4000003` via `smc #0`) `target_cpu`: **`0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103`** = `(MPIDR_EL1 >> 8) & 0xFFFF`.

Unmodified `STARLTE` breaks on `CL1` because `start.s:148` and `machine_routines.c:1472` mask `MPIDR_EL1 & 0xFFFF` (assuming `MT=0` only on M3), collapsing `0x81010000..300` onto `0x0000..0300` (`CL0`)!

**Exact 4-File SMP Patch Set**:
1. **ADT `/cpus/cpu0..cpuN` (`mkadt_a21s.py`)**:
   - Set `reg` to normalized `Aff2:Aff1`:
     - **8-core mode** (kernel-native storage & touch, `bootcpu=0`): `cpu0..7` `reg = [0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103]`, `cpu0` `state = "running"`, `A5XELTE_BOOT_MPIDR = 0x000` (`A5XELTE.h:35`).
     - **7-core mode** (when core 0 stays in `loader.efi` polling SPI1 touch + `ufs=2`, `bootcpu=1`): `cpu0..6` `reg = [0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103]`, `cpu0` `state = "running"`, `A5XELTE_BOOT_MPIDR = 0x001`.
   - Set all CPU nodes `compatible = "arm,ananke\0arm,armv8"` (`cluster-type` omitted / `SMP`, `cpu-uttdbg-reg` / `cpu-impl-reg` / `coresight-reg` stripped so `ml_map_cpu_pio` does nothing).
   - Notice: with `phys_id = 0x000..0x003, 0x100..0x103`, XNU's `MPIDR_CPU_ID(phys_id)` (`bits[7:0]`) equals `0..3` and `MPIDR_CLUSTER_ID(phys_id)` (`bits[15:8]`) equals `0..1` automatically!
2. **`scion-xnu/osfmk/arm64/start.s:148-155` (`_reset_vector` secondary CPU lookup)**:
   Replace `and x0, x15, #0xFFFF` + `tbnz x15, #24` M3 hack with:
   ```asm
   ubfx    x0, x15, #8, #16    /* Exynos 850 DynamIQ: Aff2:Aff1 -> 0x000..0x003, 0x100..0x103 */
   ```
   (And at `start.s` boot-CPU fallback around `A5XELTE_BOOT_MPIDR`, match the same `ubfx` encoding.)
3. **`scion-xnu/osfmk/arm64/machine_routines.c:1467-1477` (`ml_get_cpu_number`)**:
   ```c
   if (phys_id & (1U << 31)) {
       phys_id = (phys_id >> 8) & 0xFFFFU;   /* raw MPIDR_EL1 -> Aff2:Aff1 */
   } else {
       phys_id &= 0xFFFFU;                   /* ADT reg already Aff2:Aff1 */
   }
   ```
4. **`scion-xnu/iokit/Kernel/arm/SweetCPU.cpp`**:
   - **Lines 139–144**: Remove the S9 `expected_mpidr` assertion (or check `(phys <= 0x003) || (phys >= 0x100 && phys <= 0x103)`).
   - **Lines 412–426**: Keep `HEROLTE_SMP_TRAMPOLINE_PA = 0x8fffe000u` (32-bit PA trampoline in unreserved low DRAM that jumps to 36-bit `_LowResetVectorBase` in `0x880000000+`, bypassing Samsung EL3 PSCI's 32-bit entry truncation).
   - **Lines 435–442**: Set PSCI `target = cpu->phys_id;` directly (**do NOT `>> 8`**!) and **delete/skip `starlte_m3_pmu_defaults()`** (which maps S9's `0x14060000`, landing inside Exynos 850 camera ISP space!).

---

### 2.4 `sweet_a64e.c` & AOT `dyld_shared_cache` Rewrite on 8x Cortex-A55

1. **Kernel Trap Emulator (`osfmk/arm64/sweet_a64e.c` & `osfmk/arm64/sleh.c`)**:
   - Exynos 850's 8x Cortex-A55 (`MIDR_EL1` implementer `0x41`, part `0xd05`) natively executes **all ARMv8.1 LSE atomics** (`CAS*`, `CASP*`, `LD<op>*`, `SWP*`), **ARMv8.2 LRCPC (`LDAPR*`)**, **scalar & NEON FP16**, **RDM (`SQRDMLAH`)**, **DotProd (`SDOT`/`UDOT`)**, and **PAN (`ID_AA64MMFR1 = 0x10212122`)** at full hardware speed with **zero** traps.
   - `sweet_a64e.c:277,1186` and `sleh.c:1342` check `(__builtin_arm_rsr64("MIDR_EL1") >> 24) == 0x53` (Samsung M3); this is always `false` on A21s, so no thread is ever M3-pinned or trapped for FP16/LSE.
   - `sweet_a64e.c` remains active purely for **ARMv8.3+ / Apple-custom instructions** left outside AOT rewrites (e.g. `libcorecrypto` PAC, unrewritten `FJCVTZS`, `FCADD`/`FCMLA`, `PACGA`, `LDRAA/B`, `BF16`, `I8MM`, `SHA3`, `FHM`, `LRCPC2`).
   - **CRITICAL**: Never set `sweetspeed` bit `65536` (`0x10000`) on A21s! In `bsd/dev/starlte_smp.c:437`, bit `65536` sets `recommended_cores = 0xF1`, idling CPUs 1–3 under the false assumption that CPUs 4–7 are big M3 cores!

2. **CRITICAL Interaction with `cache-pipeline.sh` (`lse-rewrite.py` & `rewrite-grow.py`)**:
   - In `tools/ios27/lse/lse-rewrite.py` and `tools/ios27/fullroot/pipeline/cache/aot/rewrite-grow.py:303-305`, S9's LSE "dual-body" emits:
     ```asm
     MRS  xS, TPIDR_EL0
     TBNZ xS, #2, llsc      ; assumes CPU 0..3 = A55, CPU 4..7 = Mongoose M3!
     ```
     On A21s, **CPUs 4–7 are ALSO Cortex-A55s**, so `TBNZ xS, #2` needlessly forces half the CPU cores through a multi-instruction software LL/SC + stack-frame loop on every atomic, and `rewrite-grow.py` needlessly replaces 77,640 native FP16 instructions with slow `double`-precision software stubs!
   - **Optimal A21s Cache Pipeline (`cache-pipeline-a21s.sh`)**:
     - **Run Step 0b**: `textdata-guard.py map` -> `skip.npy`
     - **Run Step 1**: `tools/ios27/a64e/a64e-rewrite.py cache $M --skip libcorecrypto` (rewrites PAC/AUT/RETA*/BRA*/BLRA*/LDRA* in place)
     - **Run Step 1b**: `pipeline/cache/stubscan.py $M --fix` (rewrites stub-island `BRAA`/`RETAA`)
     - **SKIP Step 2 (`lse-rewrite.py`) COMPLETELY!** All 8 A55 cores execute LSE natively inline!
     - **Run Step 4**: `tools/ios27/speed/a64e-rewrite2.py cache $M` (`PACGA -> AND` with `sweetspeed` bit `4` set, plus writeback `LDRAx -> LDR`)
     - **Run Step 5**: `tools/ios27/speed/cachepatch.py` with `tools/ios27/s9/patches-24A446.txt` (Settings IOKit-quiesce fix, `kbd` container fix, QuartzCore 0.1s clamp off, PosterBoard Fix A)
     - **Step 6 (`rewrite-grow.py`)**: Either skip LSE/FP16/RDM/DotProd in `inscope_thunk_mask()` so `rewrite-grow.py` + `fix-cs.py` ONLY emit thunks for ARMv8.3+/v8.4+ (`FJCVTZS`, `FCADD`, `FCMLA`, `BF16`, `I8MM`, `SHA3`, `FHM`, `FLAGM`, `FRINT32/64` — only ~30,080 sites, `< 2 MiB` growth!), **or** skip Step 6 entirely and let `sweet_a64e.c` emulate the rare ARMv8.3+ scalar/SIMD ops on demand (saving ~21 MiB of cache size and ~15 minutes of build time). Skipping LSE/FP16 in Step 6 while keeping ARMv8.3+ stubs is fastest at runtime.

---

### 2.5 Boot Chain & Root Block Device (`dw_mmc` `0x12100000` + `/data/ios/system` Extent Map)

#### Boot Chain Architecture (Preserving Dual-Boot Without Repartitioning)
1. **Stage 1 (`boot.img` on `BOOT` GPT partition, `37,748,736` B)**:
   - Loaded by S-Boot at `0x80080000` with GOP framebuffer active at `0xfa000000` (`720x1600`) and `dwmmc0` (`0x12100000`) initialized in 8-bit `HS400` mode.
   - Reads the **64 KiB side-load extent map** stored at the last 64 KiB of `boot` (`offset 0x23f0000`, LBA `+73600`):
     ```c
     struct map_hdr  { char magic[8]; uint32_t version, nfiles, nexts, crc; };
     struct map_file { char name[16]; char part[16]; uint64_t size; uint32_t first_ext, n_ext; };
     struct map_ext  { uint64_t logical, phys, len; }; /* bytes; phys is offset from userdata GPT start */
     ```
   - Reads `/data/ios/payload` (`SWEETXNU` / `XNUP` container holding `loader.efi`, `hakios-kernel`, `a21s.adt`, and cmdline) into `0xb0000000` and enters `xnuloader` (or directly enters XNU via `sboot` shim).
2. **Kernel-Native Root Block Driver (`bsd/dev/a21s_emmc.c`, replacing `herolte_ufs.c` `ufsroot0`)**:
   - Why kernel-native `dw_mmc` beats `ufs=2` UEFI BlockIO bounce: XNU runs on **all 8 Cortex-A55 cores** (`cpus=8`) instead of sacrificing CPU0 to `loader.efi`, and supports direct multi-block reads + writes (`rootrw=1`)!
   - **Passing the Extent Map to XNU**: Stage 1 / `mkadt_a21s.py` places the 64 KiB extent map (plus `userdata_first_lba` from GPT) either in ADT `/chosen/a21s-system-map` or reads it directly in `a21s_emmc_init()` from `BOOT` partition offset `0x23f0000` via GPT scan of `"boot"` and `"userdata"`.
   - **Exynos 850 `dwmmc0` (`0x12100000`) Register Programming** (proven on SM-A217F hardware & `a5xelte_emmc.c`):
     - Base PA: `0x12100000` (`ml_io_map(0x12100000, 0x2000)`). **Do NOT reset clocks/pads/bus-width** — keep S-Boot's `HS400` state!
     - Registers: `CTRL=0x000`, `PWREN=0x004`, `CLKENA=0x010`, `TMOUT=0x014`, `BLKSIZ=0x01c`, `BYTCNT=0x020`, `INTMASK=0x024`, `CMDARG=0x028`, `CMD=0x02c`, `RINTSTS=0x044`, `STATUS=0x048`, `VERID=0x06c` (`>= 0x240a` -> **`DATA = 0x200`**), `HCON=0x070`, `CLKSEL=0x0a8` (64-bit IDMAC layout).
     - Multi-sector PIO transfer (`CMD18` read / `CMD25` write + `CMD_SEND_AUTO_STOP` bit `12`, `BLKSIZ = 512`, `BYTCNT = n * 512`, `CMD_HOLD` bit `29` if `(CLKSEL >> 16) & 7`, paired 32-bit reads/writes at `DATA` `0x200` and `0x204` when `((HCON >> 7) & 7) == 2`).
     - **Extent Translation (`lba_translate`)**: Given byte offset `off` and `want` bytes in `a21s_emmc_strategy(buf_t bp)`, binary-search `map_ext` where `ext.logical <= off < ext.logical + ext.len`, clamp `chunk = MIN(want, ext.logical + ext.len - off)`, and compute `disk_lba = userdata_start_lba + (ext.phys + (off - ext.logical)) / 512`. Enforce write guard: writes are strictly rejected (`EROFS`) unless `rootrw=1` AND the target LBA falls inside a verified `map_ext` of `"system"`.
     - Register `bdevsw_add(-1, &a21s_emmc_bdevsw)` + `cdevsw_add_with_bdev(...)` publishing `/dev/ufsroot0` and `/dev/rufsroot0` (`DEVFS_CHAR` / `DEVFS_BLOCK`, `DKIOCGETBLOCKSIZE = 4096` or `512`, `DKIOCGETBLOCKCOUNT`, `DKIOCISWRITABLE`) so existing `rd=ufsroot0 rootrw=1` mounts HFSX root with zero VFS changes!
   - **CRITICAL**: Disable `bsd/dev/starlte_sd.c` (`SD_PA = 0x11500000`) because `0x11500000` on Exynos 850 is the **Mali-G52 GPU**!

---

### 2.6 Display (`720x1600` GOP), Touch (`HX83112A` SPI1), Mali-G52 GPU (`0x11500000`), and ACPM (`0x11900000`)

#### A. Display (`720x1600` GOP Framebuffer at `0xfa000000`)
- **`pexpert/pexpert/arm64/A5XELTE.h`**:
  ```c
  #define A5XELTE_FB_PA           0xfa000000ULL
  #define A5XELTE_CONSOLE_PA      0xfa000000ULL
  #define A5XELTE_FB_WIDTH        720
  #define A5XELTE_FB_HEIGHT       1600
  #define A5XELTE_FB_STRIDE       2880
  #define A5XELTE_FB_BLOCKS       3      /* 3 x 2 MiB = 6 MiB >= 4,608,000 B (0xfa000000 is 2 MiB-aligned) */
  #define A5XELTE_FB_TEXT_SCALE   1
  ```
- **`mkadt_a21s.py`**: Set `/a5xelte-fbcon` and `/arm-io/disp0` (`a5xelte,fb`) `reg = (0xfa000000, 0x465000)`, `width = 720`, `height = 1600`, `stride = 2880`, canvas **`720x1600`** (`sweetfb_lw=720 sweetfb_lh=1600` @ `2x` UIKit scale = `360x800` pt, matching modern iPhone logical width with **1:1 unscaled pixel blit**!).
- Keep `a5fb_keep_on=1 a5fb_noconsole=1 a5fb_hz=60 panel=0 sweetfb_dpp=0` (do **not** enable S9's `s6e3ha8` DSIM0 `0x16080000` or `exynos_dpu` `0x16030000`).

#### B. Touch & Buttons (`Himax HX83112A` on `SPI1 / USI_CMGP0` `0x11d00000`)
Two hardware-proven options (both feed `SweetHIDTouch` / `backboardd` in `bsd/dev/starlte_touch.c`):
1. **Kernel-Native SPI1 Poller/IRQ Thread in `starlte_touch.c` (Recommended — keeps all 8 CPU cores for XNU)**:
   - Map `CMU_CMGP = 0x11c00000` (`0x3000`), `USIMODE = 0x11c22000` (`0x1000`), `GPIO_CMGP = 0x11c30000` (`0x1000`), `SPI1 = 0x11d00000` (`0x1000`), `GPIO_ALIVE = 0x11850000` (`0x1000`).
   - Init sequence (hardware-verified on SM-A217F):
     - Enable `CMU_CMGP` gates `+0x2018`, `+0x2040`, `+0x2044`, `+0x2048` (`|= (1<<20)|(1<<21)`), set divider `+0x1804 = (val & ~0x1f) | 3`, wait bit 16 clear.
     - Set `*(USIMODE + 0x0) = 2` (SPI mode); set `gpm0-0` (`+0x00`), `gpm1-0` (`+0x20`), `gpm2-0` (`+0x40`) function = `2`, pull = none; set CS `gpm3-0` (`+0x60` fn = `1` output, `+0x64 |= 1` deasserted high); set IRQ `gpa0-0` (`GPIO_ALIVE + 0x00 &= ~0xf` input, active low at `+0x04 & 1`).
     - Configure Exynos SPI (`0x11d00000`): Mode 0 (`CH_CFG +0x00 = 0`), `MODE_CFG +0x08 = 0x3ff << 19`, `CS_REG +0x0c = 1` (manual CS via `gpm3-0` + `CS_REG = 0` across 32-byte FIFO half-transfers).
     - Verify ICID (`write 0xF2 0x00 {0xd0,0x00,0x00,0x90}`, `write 0xF2 0x0c {0,0,0,0}`, `read 0xF3 0x08 0x00` -> 4 bytes == **`0x83112a00`**).
     - Run thread at priority `81` every `2 ms` (or attach GIC EINT for `gpa0-0`): when `(GPIO_ALIVE + 0x04) & 1 == 0` (and past 12 ms post-frame holdoff), read 56-byte event stack (`0xF3, 0x30, 0x00` + 56 bytes), verify `(sum_of_56_bytes & 0xff) == 0` and `ev[52] != 0xff`, decode up to 10 big-endian `{x, y}` slots (`0xffff` = up) and dispatch through `starlte_touch.c`'s existing `st_hid_dispatch()` (`SweetHIDTouch` IOHIDEventSystem)!
     - Poll `gpa0-7` (`(GPIO_ALIVE + 0x04) & (1<<7) == 0` -> Vol+) and `gpa1-0` (`(GPIO_ALIVE + 0x24) & 1 == 0` -> Vol-); 3-second Vol+ & Vol- chord triggers clean `starlte_reset_normal()`.
2. **Core-0 `A21SMBOX` Fallback (`cpus=7 bootcpu=1 a21s_input=1`)**: If running under the iOS 16 A21s UEFI `loader.efi`, core 0 polls `0x11d00000` and writes `struct a21s_mbox` (`magic = 0x584f424d53313241ULL`, `"A21SMBOX"`) at `a21s_mbox=0x...`; add a 50-line consumer in `starlte_touch.c` mapping `a21s_mbox` with `ml_io_map_wcomb()` and feeding `st_hid_dispatch()`.

#### C. Mali-G52 MP1 (`0x11500000`, `GPU_ID = 0x72120000`) & ACPM (`0x11900000`)
Porting `bsd/dev/starlte_gpu.c` (+ `iokit/Kernel/arm/StarbiteMali.cpp`) to A21s requires **5 exact changes**:
1. **Addresses & Power/Clock/DTZPC Bring-Up (`starlte_gpu.c`)**:
   - `SG_GPU_PA = 0x11500000u` (`size 0x5000`), `SG_PMU_G3D_PA = 0x11861000u` (`CONF = +0xd00`, `STAT = +0xd04`, **domain-on mask is bit 0 (`(stat & 1) == 1`), NOT `0xf`!**).
   - Map `CMU_TOP = 0x120e0000` (`0x3000`) and `CMU_G3D = 0x11400000` (`0x3000`):
     1. Set `CMU_TOP + 0x1844 = (rd & ~7u) | 1u` (`DIV_SWITCH` divide-by-2 instead of S-Boot's `/8`, raising G52 switch clock to ~400 MHz) and `CMU_TOP + 0x2040 |= (1u<<20)|(1u<<21)` (`GAT_SWITCH`).
     2. If `(PMU_ALIVE + 0x1d04) & 1 == 0`, set `PMU_ALIVE + 0x1d00 |= 1`, poll `+0x1d04 & 1`, then invoke EL3 DTZPC restore **`smc(0x82000410, 1, 0x11410204, 0)`** (must return `0`, otherwise reading `0x11500000` causes an SError!).
     3. Set `CMU_G3D + 0x0600 |= (1u<<4)` (`SWITCH_USER`), `+0x1000 = 1` (`MUX_BUSD`), `+0x1804 = 3` (`DIV_BUSP`), and enable all 7 gates `{0x2000, 0x2004, 0x200c, 0x2010, 0x2024, 0x2028, 0x202c}` (`|= 0x300000`).
     4. **Skip PMIC `BUCK6` writes (`starlte_pmic_write(1, 0x2b, ...)`!)** — on A21s, S2MPU12 `BUCK2` (`vdd_gpu`) is already enabled at operating voltage by S-Boot.
     5. If `sgu_irq=1` is used, update GIC JOB IRQ in `starlte_gpu.c` from S9's SPI `282` (`INTID 314`) to A21s's **SPI `128` (`INTID 160`)** (`fdt.dts` `mali@11500000`: `JOB = 0x80`, `MMU = 0x81`, `GPU = 0x82`).
2. **GPU ID Check**: Accept `(id >> 16) == 0x7212` (`Mali-G52 r0p0`, `SHADER_PRESENT_LO = 0x1`, `TILER_PRESENT_LO = 0x1`, `L2_PRESENT_LO = 0x1`, `COHERENCY_FEATURES = 0`).
3. **CRITICAL — Bifrost v7 MMU Page Table Mode (`sgu_as_enable` & AS0 in `starlte_gpu.c`)**:
   - S9's Mali-G72 (`0x6221`, Bifrost v6) uses Mali Legacy LPAE (`AS_TRANSCFG = 0`, `AS_MEMATTR = 0x888d88`, `AS_TRANSTAB = pa | 7`, L3 leaf `pa | 0x2c1`).
   - A21s's Mali-G52 (`0x7212`, Bifrost v7, `MMU_FEATURES = 0x2830` -> 48-bit VA, 40-bit PA) uses **AArch64 Stage-1 4K LPAE (`ADRMODE_AARCH64_4K = 6`)**, hardware-verified on this exact phone:
     - `AS_TRANSTAB = SGU_PT_PA` (no `| 7` mode bits in `TRANSTAB` under mode 6!)
     - `AS_TRANSCFG_LO = 0x10001c6u` (`6` AArch64 4K | `((55 - 48) << 6)` 4-level walk from L0 | `(1u << 24)` uncached table walk), `AS_TRANSCFG_HI = 0`
     - `AS_MEMATTR_LO = 0x88888888u`, `AS_MEMATTR_HI = 0x88888888u`
     - Table descriptor (L0/L1/L2): `table_pa | 0x3ull`
     - Leaf 4K page descriptor (L3):
       - GPU Read/Write: `(pa & 0x0000fffffffff000ull) | (1ull << 10) /* AF */ | (3ull << 8) /* ISH */ | (1ull << 6) /* AP RW */ | 0x3ull` = **`pa | 0x743ull`**
       - GPU Read-Only: **`pa | 0x7c3ull`** (`AP = 3 << 6`)
4. **Framebuffer Scanout Present (`sweetfb_gpu=0` Initially, Then Rebuild `starlte_gpu_tri.h` for v7)**:
   - `bsd/dev/starlte_gpu_tri.h` was compiled offline by `tools/ios27/gpu/starlte-g72-tri.c` with `#define GPU_ID 0x6221u` (Bifrost v6 genxml/compiler) and `1440x2960` stride `5760`.
   - Because A21s renders a native **`720x1600`** canvas onto a **`720x1600`** panel (`4.4 MiB`), set **`sweetfb_gpu=0`** (`SG_FB_PA = 0xfa000000`, `720x1600`, stride `2880`) so `sgu_setup()` + `starbite_mali_publish()` run immediately after GPU reset + core power-on + AS1 init (skipping S9's v6 `sg_stage4` triangle blob), while **all userland CoreAnimation / UIKit / Safari GLES rendering runs on the Mali-G52 MP1 via `StarbiteMali` (AS1)** and `SweetFramebuffer` copies the finished `720x1600` IOSurface in `< 0.6 ms`! (Optionally recompile `starlte-g72-tri.c` against Mesa's v7 `GENX` with `GPU_ID=0x7212u`, `PANEL_W=720`, `PANEL_H=1600`, `PANEL_STRIDE=2880` later if GPU scanout blit is desired.)
5. **ACPM (`0x11900000`)**:
   - If ACPM DVFS is enabled later: `ACPM_MBOX_PA = 0x11900000`, `ACPM_SRAM_PA = 0x02039000` (`size 0x30000`), **`ACPM_INITDATA = 0x7000`** (`fdt.dts:6712`, vs `0x7f00` on S9!). Keep `cpufreq_gov=0 tmu=0` for initial iOS 27 boot (S-Boot leaves CL0/CL1 at high boot OPP).

#### D. Built-in Default Boot-Args (`osfmk/arm/arm_init.c:185-216`)
Replace `starlte_default_bootargs` so **no S9-only MMIO driver** starts by default:
```c
static const char starlte_default_bootargs[] =
    "debug=0xc serial=3 cpus=8 rd=ufsroot0 ufsroot=USERDATA ufsrw=1 rootrw=1 "
    "cs_enforcement_disable=1 ignition_level=1 launchd_unsecure_cache=1 wdt=-1 "
    "a5fb_keep_on=1 sweetfb_scale=1 sweetfb_lw=720 sweetfb_lh=1600 a5fb_noconsole=1 a5fb_hz=60 sweetfb_gpu=0 "
    "vm_compression_limit=196608 cs_integrity_skip=1 sweetmemscale=4 sweetacl=1 sweetreboot=86400 sweethalt=1 "
    "sweetinject=/usr/lib/s.dylib,backboardd,Preferences;/usr/lib/m.dylib,PosterBoard,SpringBoard "
    "sbprof=0 sbgl=1 sbmtl=0 stargpu=2 sgu_irq=1 extra_ram=1 extra_ram_mask=255 "
    "sweetspeed=402914757 persona_lax=1 decmpfs_lz4=1 sweetatpath=1 startouch=81";
```
*(Note on `sweetspeed=402914757` = `1 | 4 | 64 | 128 | 256 | 1024 | 2048 | 262144 | 8388608 | 134217728 | 268435456`: enables IORTC skip, PACGA `AND` match, async GPU queue, prewarm, kqworkloop limit bypass, 60 Hz deadline timer, CA pacing, IOSurface resnap, 35% GPU wired-RAM cap protecting 4 GB RAM, and iOS `IOSurfaceGetPropertyMaximum`, while **omitting** bit `65536` M3 core mask and bit `512` S6E3HA8 panel!)*

---

## 3. Userland GLES (`Mesa Panfrost` + `sbgl` + `sbca`) & Slimmed HFSX Root Image (`<= 8.0 GiB`, Zero `sudo`)

### 3.1 Mesa 25.0.7 Panfrost Support for Mali-G52 (`0x7212`, Bifrost v7) & Userland Overlay Build

- **How Mesa Panfrost Supports Mali-G52 Out of the Box**:
  In Mesa `25.0.7`, `panfrost` compiles per-architecture genxml backends (`v4`, `v5`, `v6`, **`v7`**, `v9`, `v10`) into a single `libOSMesa.8.dylib`. At runtime (`osmesa_create_gpu_screen()` -> `starbite_drm.c` -> `StarbiteMali` selector `4` `SBM_GET_PARAM`), `starlte_gpu_user_param(0)` reads hardware `GPU_ID >> 16 = 0x7212`. Mesa's `panfrost_model_list` maps `0x7212` -> `"Mali-G52"`, `arch = 7` (**Bifrost v7**, dispatching to `panfrost_v7` cmdstream & Bifrost compiler with v7 clause/tuple rules), `32 MiB` non-growable tiler heap (`panfrost-ios.patch:55`), and `DRM_PANFROST_GPU_COHERENCY_NONE` (`31`). **Zero Panfrost driver patches are needed between S9 (`0x6221` v6) and A21s (`0x7212` v7)!**

- **Exact Userland Build Sequence on Xcode 27.2 (No `sudo`)**:
  *(Note: Requires `bison` and `flex` on `PATH`; `mk-mesa-ios.sh` creates its own Python venv for `meson`/`mako`/`pyyaml`/`packaging`. If `archive.mesa3d.org` redirects, pre-place `mesa-25.0.7.tar.xz` in `build/mesa-ios/`.)*
  ```bash
  export WORK=/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27
  export DEVELOPER_DIR=/Applications/Xcode_27.2.app/Contents/Developer
  export PY="$WORK/venv/bin/python3"
  cd "$WORK/work"

  # 1. Build libOSMesa.8.dylib (softpipe + panfrost v6/v7 + starbite-drm IOKit client)
  VULKAN=0 tools/ios27/mesa-ios/mk-mesa-ios.sh

  # 2. Build libsbgl.dylib (EAGL -> OSMesa/Panfrost IOSurface bridge) + libsbca.dylib (24A446 QuartzCore GLES renderer)
  IOS27_BUILD=24A446 \
  MESA="$WORK/work/build/mesa-ios/mesa-25.0.7" \
  OSMESA="$WORK/work/build/mesa-ios/build-pan/src/gallium/targets/osmesa/libOSMesa.8.dylib" \
  tools/ios27/ca-gpu/build-sbca27.sh

  # 3. Build m.dylib (SpringBoard static wallpaper stand-in from tools/ios27/metal/sbwall.m) & cc_fips_test stand-in
  xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 -fobjc-arc -dynamiclib \
    -install_name /usr/lib/m.dylib -Wl,-init,_sbwall_start \
    -framework Foundation -framework UIKit -framework QuartzCore -framework CoreGraphics \
    tools/ios27/metal/sbwall.m -o out/m.dylib
  printf '#include <stdlib.h>\nint main(void){return 0;}\n' > /tmp/cc_fips_ok.c
  xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 /tmp/cc_fips_ok.c -o out/cc_fips_test
  codesign -f -s - out/m.dylib out/cc_fips_test

  # 4. Assemble $WORK/overlay (ENABLE_METAL_CA=0 OpenGL ES compositor layout)
  mkdir -p "$WORK/overlay/usr/lib" "$WORK/overlay/usr/local/starbite/lib" \
           "$WORK/overlay/usr/libexec" "$WORK/overlay/private/var/local"
  cp out/libsbca.dylib "$WORK/overlay/usr/lib/sbca.dylib"
  cp out/libsbca.dylib "$WORK/overlay/usr/lib/s.dylib"
  cp out/m.dylib       "$WORK/overlay/usr/lib/m.dylib"
  cp out/libOSMesa.8.dylib "$WORK/overlay/usr/local/starbite/lib/libOSMesa.8.dylib"
  cp out/libsbgl.dylib     "$WORK/overlay/usr/local/starbite/lib/libsbgl.dylib"
  cp out/cc_fips_test      "$WORK/overlay/usr/libexec/cc_fips_test"
  ```

---

### 3.2 Exact `24A446` Space Breakdown & Slimming Plan (`7.52 GiB` Used in an `8.0 GiB` HFSX Image)

Direct measurement of the unpacked `24A446` (`iPhone12,1`) DMGs in `/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/inputs/` reveals why an untrimmed HFSX root reaches `~20.3 GiB`: APFS `os.dmg` uses transparent `decmpfs` compression (`8,141 MiB` on APFS expands to **`13,668.2 MiB` logical / `14,437.5 MiB` HFSX 4K-block-allocated** when copied with `/usr/bin/rsync`!).

> **CRITICAL `decmpfs` Warning**: Do **NOT** use `ditto --hfsCompression` (which writes zlib/LZVN `decmpfs` types 3/4/7/8 — `scion-xnu` has **no** zlib/LZVN decompressor kext!). `scion-xnu` **does** ship `bsd/kern/decmpfs_lz4.c` (**type `16`**, 64 KiB Apple `bv41` LZ4 chunks in `com.apple.ResourceFork`, activated by `decmpfs_lz4=1`).

#### Measured Component Sizes & Exact Slimming Budget (`8.00 GiB = 8,192 MiB` HFSX Image)

| Component | Untrimmed HFSX-4K | Slimmed HFSX-4K | Exact Action / Exclusion Rule |
|---|---:|---:|---|
| `systemos.dmg`: `dyld_shared_cache_arm64e*` | `6,877.5 MiB` | **`5,710.0 MiB`** | Omit `*.symbols` (`1,169.4 MiB`); hard-link `/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/*` <-> `/System/Library/Caches/com.apple.dyld/*` (1 physical copy; `< 2 MiB` AOT v8.3 stub growth) |
| `systemos.dmg` (other) + `appos.dmg` | `108.8 MiB` | **`105.0 MiB`** | Keep all (hard-linked into `/System` & `/usr` by `stage-full-root.sh:110-124`); drop `2.9 MiB` non-English `.lproj` |
| `os.dmg`: `/private/var/staged_system_apps` | `1,134.9 MiB` | **`49.6 MiB`** | Set `INSTALL_STAGED_APPS="MobileSafari Files Calculator MobileTimer MobileNotes Photos"` (installs those 6 into `/Applications` and deletes `/private/var/staged_system_apps`) |
| `os.dmg`: `/Applications/*.app` | `474.4 MiB` | **`27.7 MiB`** | Keep `PosterBoard`, `PreBoard`, `InputUI`, `MediaRemoteUI`, `ClockAngel`, `Preferences`, `SafariViewService`, `WebSheet`, `PhotosUIService`, `ScreenshotServicesService`, `SharingViewService`, `Spotlight`, `LocalAuthenticationUIService`, `AuthenticationServicesUI`, `AutoFillSuggestions`, `StoreKitUIService`, `StoreKitUISceneService`, `ColorPickerUIService`, `FontPickerUIService`, `DockFolderViewService`, `SpringBoardEducation`; exclude Setup, Diagnostics, CarPlay, Clarity, Siri, SOSBuddy, etc. |
| `os.dmg`: `/usr/standalone` + disabled ML/OTA blobs | `286.7 MiB` | **`0.0 MiB`** | Exclude `/usr/standalone/*` (firmware update `.dmg` payloads), `/usr/share/{tokenizer,morphun}`, `/usr/libexec/Moments.framework` |
| `os.dmg`: `/System/Library` top-level data suites | `3,574.1 MiB` | **`0.0 MiB`** | Exclude `LinguisticData` (`836`), `PreinstalledAssetsV2` (`797`), `NanoTimeKit` (`442`), `Assistant` (`363`), `Health` (`281`), `OnBoardingBundles` (`184`), `AppPlaceholders` (`74`), `ImagingNetworks` (`62`), `ProductDocuments` (`62`), `ScreenReader` (`42`), `AccessibilityBundles` (`42`), `NanoPreferenceBundles` (`36`), `Carrier Bundles` (`34`), `DriverExtensions` (`29`), `GeoServicesData` (`18`), `FlowTools` (`15`), `CoreServices/{RawCamera*.bundle,BluetoothUIService.app,ClarityBoard.app,CarPlay.app}` (`147`), `Fonts/CoreAddition` CJK `.ttc` (`~210` uncompressed, keeping `Fonts/{Core,CoreUI,AppFonts}`!), `Library/{Wallpaper,Ringtones,Audio/Tunings}` (`~110`) |
| `os.dmg`: ML/Vision/Watch/Siri/AGX Framework resources & file patterns | `5,156.9 MiB` | **`0.0 MiB`** | Exclude non-English `*.lproj/`, `*.mlmodelc/`, `*.momd/`, `*.espresso.*`, `*.weights`, `*.hwx`, `*.metallib`, `*.rtlib`, `*.mov`, plus resource-only directories of unused frameworks (`CoreEmoji`, `MediaAnalysis`, `VoiceServices`, `GPUCompiler`, `CVNLP`, `iWorkImport`, `NanoUniverse`, `TextToSpeechMauiSupport`, `GeoServices`, `GESS`, `TextRecognition`, `VisionCore`, `SiriSetup`, `CoreSceneUnderstanding`, `AvatarKit`, `SpotlightResources`, `CoreHandwriting`, `WeatherUI`, `CoreRE`, `CameraEffectsKit`, `VisualLookUp`, `SiriTTSService`, `HomeUI`, `HeadphoneSettingsUI`, `HeadphoneConfigs`, `HeadphoneAssets`, `IntelligenceFlowPlannerSupport`, `Moments`, `PassKitUI`, `WorkoutUI`, `Morpheus`, `AppleCVA`, `AppleCV3D`, `SceneIntelligence`, `ImagePlaygroundInternal`, `AltruisticBodyPoseKit`, `Vision`, `MetalPerformanceShaders`, `MetalPerformancePrimitives`, `CoreTelephony`, `PassKit`, `FinanceKit`, `PHASE`, `CreateML*`, `MusicUnderstanding`, `SoundAnalysis`, `HealthKit`) — **note**: all executable code of these frameworks lives in `dyld_shared_cache`, NOT on `os.dmg`! |
| `os.dmg`: Remaining core OS (`/sbin`, `/bin`, `/usr`, `CoreServices`, `PreferenceBundles`, `SFSymbols`, `Fonts/Core`, `ExtensionKit`, `Audio`) | `3,810.5 MiB` | **`1,460.0 MiB`** | **Step A**: Prune `.loctable` binary plists in Python (`plistlib`: retain only `en`, `en_US`, `en_GB`, `Base`, `LocProvenance` keys -> saves `~1,150 MiB`!). **Step B**: Apply `decmpfs_lz4` (type 16 LZ4, Section 3.3) to read-only non-Mach-O `.car`, `.ttc`, `.plist`, `.loctable`, `.dat` files `>= 8 KiB` under `/System/Library` & `/usr/share` -> saves another `~1,200 MiB`! *(Even without Step B, Step A alone lands at `~2,660 MiB` -> `8.7 GiB`, or `8.0 GiB` if `Fonts/AppFonts` + `MercuryPosterExtension.appex` + unused `Audio/UISounds` are also dropped!)* |
| Overlay (`libOSMesa`, `libsbgl`, `sbca`, `s`, `m`) + Procursus/Sileo (`/jb`) | `145.0 MiB` | **`135.0 MiB`** | `PACKAGE_MANAGER=sileo` (`60-sileo.sh`), `ENABLE_WIFI=0 ENABLE_AUDIO_BT=0 ENABLE_MODEM=0 ENABLE_CAMERA=0 ENABLE_SENSORS=0` (skip S9-specific `90..94` steps & `firmware.sh`) |
| HFSX Metadata (Journal + 32 MiB Catalog B-Tree + Bitmap) | `160.0 MiB` | **`115.0 MiB`** | `newfs_hfs -s -J -b 4096 -c c=8192 -v IOSROOT` |
| **TOTAL USED ON HFSX** | **`~20,400 MiB`** | **`7,602 MiB` (`7.42 GiB`)** | **Fits cleanly inside an `8.0 GiB` (`8,192 MiB`) HFSX image with `~590 MiB` free for `/private/var` + `/private/var/jb`, well inside `/data/ios/system`'s `10.8 GiB` phone budget and the Mac's `74 GiB` free disk!** |

#### Compatibility Verification Against `tools/ios27/fullroot/check-root.sh`
Every single `bad` (hard-fail) assertion in `check-root.sh` passes on this slimmed root:
1. **Lines 6–10**: `sbin/launchd`, `SpringBoard`, `usr/libexec/backboardd`, `Applications/Preferences.app/Preferences`, `usr/lib/s.dylib`, `usr/local/starbite/lib/{libOSMesa.8,libsbgl}.dylib`, `dyld_shared_cache_arm64e{.01}`, `private/var/keybags/systembag.kb`, `System/Library/xpc/launchd{-disabled,}.plist` -> **ALL PRESENT**.
2. **Lines 14–19**: `LinguisticData`, `PreinstalledAssetsV2`, `Health`, `NanoTimeKit`, `Assistant`, `OnBoardingBundles`, `Audio` use `info "absent ... (trimmed build)"` (**NOT** `bad`!).
3. **Lines 21–28**: Cryptex cache hard-link inode check (`stat -f %i`) & `patches-24A446.txt` word check -> **PASS**.
4. **Lines 31–62**: `launchd.plist` no `_PanicOnCrash`, required jobs (`backboardd`, `SpringBoard`, `configd`, `lsd`, `runningboardd`, `com.herolte.autorun`) present, `CommCenter`/`bluetoothd`/`imagent`/`rapportd`/`terminusd` off -> **PASS** (note lines 42–45 log missing executables of other jobs as `info`, not `bad`).
5. **Lines 65–100**: `SystemVersion.plist == LastSystemVersion (24A446)`, Accessibility/GlobalPreferences plists, `overlay/MANIFEST.sha256`, `macho-rewrite.py --dry-run` (`0 sites left, 0 errors`) -> **PASS**.
6. **Lines 102–120**: `/jb` Sileo + `/Applications/Sileo.app/.scion-prefix-app`, `/bin/{chmod,cp,mkdir,mv,rm}` symlinks, `com.scion.{jbseed,varsync}` jobs, no `sshd`/`TweakLoader`, no world-writable files (`stat -f %u` when mounted unprivileged prints `info`, not `bad`!) -> **PASS**.

---

### 3.3 Building the Slimmed HFSX Root Image Completely Without `sudo`

Why `build-full-root.sh` asked for `sudo`:
1. Mounting `os.dmg`/`systemos.dmg`/`appos.dmg` with `-owners on` (`build-full-root.sh:67`) and checking `EUID == 0` / `stat -f %u .../launchd == 0` (`stage-full-root.sh:15,36-42,156`).
2. Running `chown` / `install -o` during staging (`stage-full-root.sh:94,136,143-153`, `30-overlay.sh:18-20`, `60-sileo.sh:156-161`). When an HFSX image is attached by an unprivileged user on macOS, the kernel sets `MNT_IGNORE_OWNERSHIP` and writes `ownerID=99, groupID=99` (`_unknown`) into every `HFSPlusBSDInfo` record in the Catalog B-Tree, and clears setuid/setgid bits on write.

#### Exact Unprivileged (Zero-`sudo`) HFSX Build Workflow

1. **Create & Attach an `8.0 GiB` Case-Sensitive Journaled HFS+ (`HFSX`) Raw Image Without `sudo`**:
   ```bash
   OUT=/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/out/root-slim.img
   mkdir -p "$(dirname "$OUT")" "$WORK/mnt-os" "$WORK/mnt-sys" "$WORK/mnt-app" "$WORK/mnt-root"

   # Create sparse/raw 8 GiB file and attach unmounted (unprivileged user owns resulting /dev/diskN!)
   rm -f "$OUT"
   mkfile -n 8g "$OUT"
   RDEV=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$OUT" | awk 'NR==1 {print $1}')

   # Format as Case-Sensitive Journaled HFS+ (HFSX) with 4 KiB blocks and a 64 MiB contiguous Catalog clump (-c c=16384)
   # CRITICAL: -c c=16384 guarantees the Catalog File stays in 1 extent (never exceeding the 8 inline fork extents in fix_hfsx_owners.py!)
   /sbin/newfs_hfs -s -J -b 4096 -c c=16384 -v IOSROOT "/dev/r${RDEV#/dev/}"

   # Mount unprivileged via hdiutil (mounts with noowners; UID/GID/setuid fixed offline in Step 4)
   hdiutil attach -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$WORK/mnt-root" "$OUT"
   ```

2. **Mount Source DMGs Unprivileged & Stage Slimmed Tree**:
   - Attach `inputs/{os,systemos,appos}.dmg` with `hdiutil attach -readonly -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint ...` (omit `-owners on`).
   - Run `prepare-root-inputs.sh "$WORK/mnt-sys/System/Library/Caches/com.apple.dyld" "$WORK/overlay"` with `PACKAGE_MANAGER=sileo` (completely unprivileged).
   - Run `cache-pipeline-a21s.sh` (unprivileged) producing `$WORK/cache/c/dyld_shared_cache_arm64e*`.
   - Run an unprivileged copy of `stage-full-root.sh` + `herolte-tune-ios-root.sh` + `steps.d/{10-macho-a64e,20-launchd,30-overlay,40-prefs,50-xpc-disable,60-sileo,70-lsd-extkit,86-coreimage-sw-context}.sh` with:
     - `EUID == 0` / `stat -f %u == 0` checks removed and `chown` / `-o 0 -g 0` / `-o 272 -g 272` shimmed to no-op in `PATH` (`TOOLS_BIN/chown` -> `exit 0`).
     - Exclusion filters from Section 3.2 applied to `rsync`, `.loctable` English-only pruning pass run, and optional `decmpfs_lz4` pass (`compression_encode_buffer(..., COMPRESSION_LZ4)` writing `com.apple.decmpfs` type 16 + `com.apple.ResourceFork`) run on read-only `.car`/`.ttc`/`.plist`/`.loctable`/`.dat` files.
   - Run `check-root.sh "$WORK/mnt-root"` -> verify `CHECK-ROOT: ALL HARD CHECKS PASSED` and `df -m "$WORK/mnt-root"` <= `7,750 MiB` used.

3. **Detach & Patch HFSX Catalog B-Tree Offline via `/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/fix_hfsx_owners.py`**:
   Write `$WORK/hfsx-extra-rules.txt` covering every special UID/GID and setuid/mode required by XNU + `launchd` + `60-sileo.sh`:
   ```text
   prefix 501:501 /private/var/mobile
   prefix 501:501 /jb/var/mobile
   prefix 272:272 /private/var/db/diagnostics
   prefix 272:272 /private/var/db/uuidtext
   exact 0:0 0600 /private/var/keybags/systembag.kb
   exact 0:0 0700 /private/var/keybags
   exact 0:0 1777 /private/var/tmp
   exact 0:0 0777 /private/var/local/oslog
   exact 0:0 0777 /private/var/local/m2v-cache
   exact 0:0 4755 /jb/Applications/Sileo.app/giveMeRoot
   exact 0:0 4755 /Applications/Sileo.app/giveMeRoot
   exact 0:0 4755 /jb/usr/bin/chpass
   exact 0:0 4755 /jb/usr/bin/login
   exact 0:0 4755 /jb/usr/bin/newgrp
   exact 0:0 4755 /jb/usr/bin/passwd
   exact 0:0 2755 /jb/usr/bin/quota
   exact 0:0 4755 /jb/usr/bin/su
   exact 0:0 4755 /jb/usr/bin/sudo
   ```
   Then detach and apply offline B-Tree fixup + `fsck_hfs` verification (zero `sudo`):
   ```bash
   sync
   hdiutil detach "$WORK/mnt-root"
   "$PY" /Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/fix_hfsx_owners.py "$OUT" "$WORK/hfsx-extra-rules.txt"

   # Offline filesystem & B-Tree integrity verification (unprivileged):
   VDEV=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$OUT" | awk 'NR==1 {print $1}')
   /sbin/fsck_hfs -n -f "/dev/r${VDEV#/dev/}"
   hdiutil detach "$VDEV"
   ```
   *(Note: Because `newfs_hfs -c c=16384` pre-allocates a 64 MiB contiguous Catalog clump at format time, `fix_hfsx_owners.py`'s 8-extent inline `HFSPlusForkData` reader handles ~150,000+ catalog records in a single extent without ever overflowing into the Extents Overflow B-Tree.)*
