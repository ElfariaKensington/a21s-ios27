#!/usr/bin/env python3
"""Build the Galaxy A21s (SM-A217F, Exynos 850 / S5E3830) ADT for iOS 27 (xnu-12377.121.6 STARLTE)."""
import argparse
import os
import struct
import sys
from pathlib import Path

BASE = Path(__file__).resolve().parent
sys.path.insert(0, str(BASE / 'work/tools/ios27/herolte'))
import adt    # noqa: E402
import mkadt  # noqa: E402

DRAM_BASE = 0x880000000
DRAM_SIZE = 0x60000000          # 1536 MiB (0x880000000..0x8e0000000; Mali-G52 uses 0x8e0000000..0x8e3000000)
PRAM_BASE = 0x8e8000000
PRAM_SIZE = 0x100000
PANIC_LOG_SIZE = 0x80000

SOC_BASE = 0x10000000
GICD_PA = 0x12a01000
GICC_PA = 0x12a02000
DWMMC0_PA = 0x12100000

FB_BASE = 0xfa000000
FB_WIDTH = 720
FB_HEIGHT = 1600
TIMEBASE = 26000000

# 7 Cortex-A55 cores for XNU (Core 0 / MPIDR 0x000 stays in loader.efi polling SPI1 touch + GPIO keys)
CPU_MPIDRS = [0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103]

# Verified 16 KiB-aligned unreserved low-bank RAM ranges from A21s /proc/iomem + stage1 keep-outs (1044 MiB extra)
EXTRA_RAM_RANGES = [
    (0x80200000, 0x8ff00000),   # 253 MiB (leaves 0x8fffe000 SMP trampoline, 0x8ffff100 marker, 0x91200000 sec_debug)
    (0x91400000, 0xa0000000),   # 236 MiB (leaves 0xa0000000..0xb4000000 for resident loader.efi + stage1)
    (0xb4000000, 0xbc8fc000),   # 136.98 MiB
    (0xc0000000, 0xc3000000),   # 48 MiB
    (0xc3080000, 0xd0000000),   # 207.5 MiB
    (0xd8800000, 0xdb900000),   # 49 MiB
    (0xdc400000, 0xe0000000),   # 60 MiB
    (0xe3600000, 0xe6c00000),   # 54 MiB
]


def set_prop(node, name, value):
    for i, (key, flags, _) in enumerate(node.props):
        if key == name:
            node.props[i] = (key, flags, value)
            return
    node.props.append((name, 0, value))


def child(node, name):
    for candidate in node.children:
        if candidate.prop('name') == mkadt.cstr(name):
            return candidate
    raise ValueError(f'missing donor ADT node {name}')


def build_a21s_adt(donor_adt: Path, include_extra_ram: bool = True):
    mkadt.D22AP_ADT = str(donor_adt)
    root = mkadt.build(seed=os.urandom(256), qemu=True)

    # N104AP / iPhone12,1 identity (iOS 27.0.1 24A446 donor) at native 720x1600 @2x (360x800 pt)
    old_prod = child(root, 'product')
    phandle = old_prod.prop('AAPL,phandle')
    root.children = [n for n in root.children if n.prop('name') != mkadt.cstr('product')]
    props = mkadt.d22ap_product(donor_adt)
    product = mkadt.node('product', ([('AAPL,phandle', phandle)] if phandle else []) + props)
    root.children.append(product)
    set_prop(product, 'artwork-device-subtype', mkadt.u32(1792))
    set_prop(product, 'artwork-scale-factor', mkadt.u32(2))

    set_prop(root, 'model', mkadt.cstr('iPhone12,1'))
    set_prop(root, 'target-type', mkadt.cstr('N104'))
    set_prop(root, 'target-sub-type', mkadt.cstr('N104AP'))
    set_prop(
        root,
        'compatible',
        mkadt.cstr('N104AP')
        + mkadt.cstr('iPhone12,1')
        + mkadt.cstr('samsung,a5xelte')
        + mkadt.cstr('samsung,herolte')
        + mkadt.cstr('samsung,starlte')
        + mkadt.cstr('samsung,exynos850')
        + mkadt.cstr('AppleARM'),
    )

    chosen = child(root, 'chosen')
    set_prop(chosen, 'display-scale', mkadt.u32(2))
    set_prop(chosen, 'chip-id', mkadt.u32(0x8030))
    set_prop(chosen, 'dram-base', mkadt.u64(DRAM_BASE))
    set_prop(chosen, 'dram-size', mkadt.u64(DRAM_SIZE))
    set_prop(chosen, 'firmware-version', mkadt.cstr('a21s-ios27'))
    set_prop(chosen, 'system-firmware-version', mkadt.cstr('a21s-ios27'))
    set_prop(chosen, 'embedded-panic-log-size', mkadt.u32(PANIC_LOG_SIZE))
    for name, last in (('wifi0', 1), ('bluetooth0', 2), ('ethernet0', 3)):
        set_prop(chosen, f'mac-address-{name}', bytes((0x02, 0x41, 0x32, 0x31, 0x53, last)))

    if include_extra_ram:
        for s, e in EXTRA_RAM_RANGES:
            assert s % 0x4000 == 0 and e % 0x4000 == 0 and e > s and e <= DRAM_BASE, hex(s)
        extra_blob = b''.join(struct.pack('<QQ', s, e - s) for s, e in EXTRA_RAM_RANGES)
        set_prop(chosen, 'hakios-extra-ram', extra_blob)

    # /cpus: cpu0 is physical CPU 1 (0x001, boot CPU), cpu1..cpu6 are 0x002, 0x003, 0x100..0x103
    cpus = child(root, 'cpus')
    cpu0 = child(cpus, 'cpu0')
    set_prop(cpu0, 'compatible', mkadt.cstr('arm,cortex-a55') + mkadt.cstr('arm,cortex-a53') + mkadt.cstr('ARM,v8'))
    set_prop(cpu0, 'reg', mkadt.u32(CPU_MPIDRS[0]))
    set_prop(cpu0, 'cpu-id', mkadt.u32(0))
    set_prop(cpu0, 'cluster-id', mkadt.u32(0))
    set_prop(cpu0, 'cluster-type', mkadt.u32(0))
    set_prop(cpu0, 'l2-cache-id', mkadt.u32(0))
    set_prop(cpu0, 'state', mkadt.cstr('running'))
    set_prop(cpu0, 'timebase-frequency', mkadt.u32(TIMEBASE))
    set_prop(cpu0, 'clock-frequency', mkadt.u32(2002000000))
    cpus.children = [cpu0]

    for logical in range(1, len(CPU_MPIDRS)):
        mpidr = CPU_MPIDRS[logical]
        cl = 1 if mpidr >= 0x100 else 0
        cpus.children.append(
            mkadt.node(
                f'cpu{logical}',
                [
                    ('device_type', mkadt.cstr('cpu')),
                    ('compatible', mkadt.cstr('arm,cortex-a55') + mkadt.cstr('arm,cortex-a53') + mkadt.cstr('ARM,v8')),
                    ('reg', mkadt.u32(mpidr)),
                    ('cpu-id', mkadt.u32(logical)),
                    ('cluster-id', mkadt.u32(cl)),
                    ('cluster-type', mkadt.u32(0)),
                    ('die-cluster-id', mkadt.u32(cl)),
                    ('cluster-core-id', mkadt.u32(mpidr & 0xff)),
                    ('l2-cache-id', mkadt.u32(cl)),
                    ('l3-cache-id', mkadt.u32(0)),
                    ('state', mkadt.cstr('stopped')),
                    ('timebase-frequency', mkadt.u32(TIMEBASE)),
                    ('clock-frequency', mkadt.u32(2002000000)),
                    ('bus-frequency', mkadt.u32(100000000)),
                    ('memory-frequency', mkadt.u32(1866000000)),
                    ('peripheral-frequency', mkadt.u32(TIMEBASE)),
                    ('fixed-frequency', mkadt.u32(TIMEBASE)),
                ],
            )
        )

    arm_io = child(root, 'arm-io')
    set_prop(arm_io, 'device_type', mkadt.cstr('starlte-io'))
    set_prop(arm_io, 'compatible', mkadt.cstr('samsung,exynos850-io') + mkadt.cstr('samsung,exynos9810-io') + mkadt.cstr('arm-io'))
    set_prop(arm_io, 'ranges', mkadt.u64(0, SOC_BASE, 0xf0000000))
    arm_io.children = [n for n in arm_io.children if n.prop('name') == mkadt.cstr('wdt')]
    arm_io.children.append(
        mkadt.node(
            'gic',
            [
                ('compatible', mkadt.cstr('arm,cortex-a15-gic')),
                ('interrupt-controller', mkadt.cstr('master')),
                ('virtual-timer-intid', mkadt.u32(27)),
                ('reg', mkadt.u64(GICD_PA - SOC_BASE, 0x1000, GICC_PA - SOC_BASE, 0x1000)),
            ],
        )
    )
    arm_io.children.append(
        mkadt.node(
            'disp0',
            [
                ('device_type', mkadt.cstr('display-subsystem')),
                (
                    'compatible',
                    mkadt.cstr('samsung,a5xelte-display')
                    + mkadt.cstr('samsung,herolte-display')
                    + mkadt.cstr('samsung,starlte-display'),
                ),
                ('canvas-width', mkadt.u32(FB_WIDTH)),
                ('canvas-height', mkadt.u32(FB_HEIGHT)),
            ],
        )
    )

    root.children = [n for n in root.children if n.prop('name') not in (mkadt.cstr('sweet-fbcon'), mkadt.cstr('sweet-ufs'))]
    root.children.append(mkadt.node('herolte-ufs', [('reg', mkadt.u64(DWMMC0_PA, 0x2000))]))
    root.children.append(mkadt.node('pram', [('reg', mkadt.u64(PRAM_BASE, PRAM_SIZE))]))
    root.children.append(
        mkadt.node(
            'a5xelte-fbcon',
            [
                ('reg', mkadt.u64(FB_BASE, FB_WIDTH * FB_HEIGHT * 4)),
                ('width', mkadt.u32(FB_WIDTH)),
                ('height', mkadt.u32(FB_HEIGHT)),
                ('stride', mkadt.u32(FB_WIDTH * 4)),
            ],
        )
    )
    return root


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--donor', type=Path, default=BASE / 'inputs/n104ap.adt')
    ap.add_argument('--no-extra-ram', action='store_true')
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    root = build_a21s_adt(args.donor, include_extra_ram=not args.no_extra_ram)
    blob = adt.serialize(root)
    args.output.write_bytes(blob)
    extra = 0 if args.no_extra_ram else sum(e - s for s, e in EXTRA_RAM_RANGES)
    print(
        f'{args.output}: {len(blob)} bytes '
        f'(window {DRAM_BASE:#x}+{DRAM_SIZE >> 20} MiB, extra {extra >> 20} MiB, total {(DRAM_SIZE + extra) >> 20} MiB)'
    )


if __name__ == '__main__':
    main()
