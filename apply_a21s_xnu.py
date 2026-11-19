#!/usr/bin/env python3
"""
Apply the complete Samsung Galaxy A21s (SM-A217F, Exynos 850 / S5E3830) port
to Scion-Project's xnu-12377.121.6 (STARLTE) source tree.
"""
import subprocess
import sys
from pathlib import Path

XNU = Path(sys.argv[1] if len(sys.argv) > 1 else
           "/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/work/build/dxb-12377/xnu")


def patch_file(rel: str, replacements: list[tuple[str, str]]) -> None:
    p = XNU / rel
    text = p.read_text()
    for idx, (old, new) in enumerate(replacements):
        if old not in text:
            raise RuntimeError(f"{rel}: replacement #{idx + 1} target not found:\n{old[:240]}")
        cnt = text.count(old)
        if cnt != 1 and "ml_io_map(STARLTE_PMU_PA, PAGE_SIZE)" not in old:
            raise RuntimeError(f"{rel}: replacement #{idx + 1} matched {cnt} times (expected 1)")
        text = text.replace(old, new)
    p.write_text(text)
    print(f"[ok] patched {rel} ({len(replacements)} hunk(s))")


def main() -> None:
    subprocess.run(["git", "-C", str(XNU), "checkout", "--", "."], check=True)

    # 1. makedefs/MakeInc.def: disable Clang 21 -Weverything diagnostic flood (-w) + expose scion-vmapple headers
    patch_file("makedefs/MakeInc.def", [
        (
            "WARNFLAGS_STD := \\\n\t-Weverything \\",
            "WARNFLAGS_STD := \\\n\t-w \\"
        ),
        (
            "ifeq ($(filter SWEET A5XELTE HEROLTE STARLTE FRANKEL,$(CURRENT_MACHINE_CONFIG)),)",
            "ifeq ($(filter SWEET A5XELTE HEROLTE FRANKEL,$(CURRENT_MACHINE_CONFIG)),)"
        ),
    ])

    # 2. pexpert/pexpert/arm64/A5XELTE.h: A21s 720x1600 GOP @ 0xfa000000, boot MPIDR 0x001, marker 0x8ffff100
    patch_file("pexpert/pexpert/arm64/A5XELTE.h", [
        (
            """#if defined(ARM64_BOARD_CONFIG_STARLTE)
/* Galaxy S9 SM-G960F (Exynos 9810): reuses the HEROLTE port. S-Boot leaves
 * DECON_F (0x16030000) scanning out this buffer (uniLoader board-starlte.c,
 * s3cfb.bootloaderfb in the stock cmdline); the shim/starlte probe drew on it. */
#define HEROLTE                       1
#define STARLTE                       1
/* Boot CPU: Cortex-A55 "ananke", MPIDR 0x81000000 (Aff1|Aff0 = 0). Only CPU0
 * is described until the other cores' MPIDRs are measured. */
#define SWEET_SMP_MAX_CPUS            8
#define A5XELTE_FB_PA                 0xcc000000
#define A5XELTE_FB_WIDTH              1440
#define A5XELTE_FB_HEIGHT             2960
#define A5XELTE_FB_STRIDE             5760
#define A5XELTE_FB_BLOCKS             9     /* 2 MiB VA=PA bootstrap blocks (16.3 MiB) */
#define A5XELTE_FB_TEXT_SCALE         3
#define A5XELTE_FB_DOT_PITCH          458   /* iPhone X userland density */
/* The boot console runs between the Project Starbite header and the
 * card slot the loader draws (tools/brand/overlay.mjs HEADER_H,
 * BAND_TOP), so both stay on screen through the kernel boot. */
#define A5XELTE_CONSOLE_TOP           640
#define A5XELTE_CONSOLE_HEIGHT        1880
/* Log page below the XNU window 0x92000000; the vendor logs end at 0x9179c000. */
#define A5XELTE_MARKER_PA             0x91fff100
#define A5XELTE_BOOT_MPIDR            0x0""",
            """#if defined(ARM64_BOARD_CONFIG_STARLTE)
/* Galaxy A21s SM-A217F (Exynos 850 / S5E3830): 8x DynamIQ Cortex-A55, Mali-G52 MP1,
 * DECON_F GOP scanout at 0xfa000000 (720x1600 XRGB8888, stride 2880). */
#define HEROLTE                       1
#define STARLTE                       1
#define SWEET_SMP_MAX_CPUS            8
#define A5XELTE_FB_PA                 0xfa000000
#define A5XELTE_FB_WIDTH              720
#define A5XELTE_FB_HEIGHT             1600
#define A5XELTE_FB_STRIDE             2880
#define A5XELTE_FB_BLOCKS             3     /* 3 x 2 MiB = 6 MiB >= 4.4 MiB */
#define A5XELTE_FB_TEXT_SCALE         1
#define A5XELTE_FB_DOT_PITCH          326   /* @2x iPhone 11 class density */
#define A5XELTE_CONSOLE_TOP           0
#define A5XELTE_CONSOLE_HEIGHT        1600
#define A5XELTE_MARKER_PA             0x8ffff100
#define A5XELTE_BOOT_MPIDR            0x001"""
        ),
    ])

    # 3. osfmk/arm64/start.s: Exynos 850 DynamIQ MPIDR Aff2:Aff1 -> 0x000..0x003, 0x100..0x103
    patch_file("osfmk/arm64/start.s", [
        (
            """#if HAS_CLUSTER || defined(SWEET)
\tand\t\tx0, x15, #0xFFFF\t\t\t\t\t// Full Aff1:Aff0 (SWEET: core in Aff1)
#if defined(STARLTE)
\t/* The Mongoose M3 cores have no MT bit: MPIDR 0x80000100..0x80000103,
\t * the same Aff1:Aff0 as A55 core 1 (0x81000100). Their ADT reg carries
\t * bit 16 (0x10100..0x10103, mkadt_starlte.py --big). */
\ttbnz\tx15, #24, 1f
\torr\t\tx0, x0, #0x10000
1:
#endif""",
            """#if HAS_CLUSTER || defined(SWEET)
#if defined(STARLTE)
\t/* Exynos 850: both 4-core Cortex-A55 clusters are DynamIQ (MT=1):
\t * Aff2 = cluster (0..1), Aff1 = core (0..3) -> 0x000..0x003, 0x100..0x103. */
\tubfx\tx0, x15, #8, #16
#else
\tand\t\tx0, x15, #0xFFFF\t\t\t\t\t// Full Aff1:Aff0 (SWEET: core in Aff1)
#endif"""
        ),
    ])

    # 4. osfmk/arm64/machine_routines.c: ml_get_cpu_number DynamIQ Aff2:Aff1 normalization
    patch_file("osfmk/arm64/machine_routines.c", [
        (
            """#if defined(STARLTE)
\t/* Callers pass an ADT reg (bit 31 clear) or a raw MPIDR (bit 31 RES1).
\t * The Mongoose M3s' MPIDR has no MT bit and the same Aff1:Aff0 as A55
\t * core 1; their reg carries bit 16 instead, as start.s does. */
\tif (phys_id & (1U << 31)) {
\t\tphys_id = (phys_id & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK)) |
\t\t    ((phys_id & (1U << 24)) ? 0 : 0x10000);
\t} else {
\t\tphys_id &= 0x10000 | MPIDR_AFF1_MASK | MPIDR_AFF0_MASK;
\t}""",
            """#if defined(STARLTE)
\t/* Exynos 850: raw MPIDR_EL1 (bit 31 set) has Aff2:Aff1 in bits [23:8];
\t * ADT reg already stores normalized Aff2:Aff1 (0x000..0x003, 0x100..0x103). */
\tif (phys_id & (1U << 31)) {
\t\tphys_id = (phys_id >> 8) & 0xFFFFU;
\t} else {
\t\tphys_id &= 0xFFFFU;
\t}"""
        ),
    ])

    # 5. iokit/Kernel/arm/SweetCPU.cpp: Exynos 850 7/8-core PSCI CPU_ON without S9 0x14060000 M3 PMU pokes
    patch_file("iokit/Kernel/arm/SweetCPU.cpp", [
        (
            """\tstatic vm_offset_t pmu;
\tif (pmu == 0) {
\t\tpmu = ml_io_map(0x14060000u, 0x3000);
\t}""",
            """\treturn; /* Exynos 850 has 8x Cortex-A55 cores, no Mongoose M3 PMU sequence */
\tstatic vm_offset_t pmu;
\tif (pmu == 0) {
\t\tpmu = 0;
\t}"""
        ),
        (
            """#if defined(STARLTE)
\t/* Logical 0..3 = Cortex-A55 cores; MPIDR_EL1 Aff1 = core (0x81000n00),
\t * measured with starsmpprobe=1 (mkadt_starlte.py). Logical 4..7 = the
\t * Mongoose M3 cores, MPIDR 0x100+n without MT, reg 0x10100+n (start.s
\t * adds bit 16 for cores without the MT bit). */
\tuint32_t expected_mpidr = cpu->cpu_id < 4 ? cpu->cpu_id << 8 :
\t    cpu->cpu_id < 8 ? 0x10100u + cpu->cpu_id - 4 : ~0u;
\tif (cpu->phys_id != expected_mpidr || topology->num_cpus > SWEET_SMP_MAX_CPUS) {
\t\tpanic(CPU_PORT_NAME " CPU: unsupported topology cpu=%u mpidr=0x%x count=%u",
\t\t    cpu->cpu_id, cpu->phys_id, topology->num_cpus);
\t}""",
            """#if defined(STARLTE)
\tbool valid_mpidr = (cpu->phys_id <= 0x003u) || (cpu->phys_id >= 0x100u && cpu->phys_id <= 0x103u);
\tif (!valid_mpidr || topology->num_cpus > SWEET_SMP_MAX_CPUS) {
\t\tpanic(CPU_PORT_NAME " CPU: unsupported topology cpu=%u mpidr=0x%x count=%u",
\t\t    cpu->cpu_id, cpu->phys_id, topology->num_cpus);
\t}"""
        ),
        (
            """#if defined(HEROLTE)
\tcpuName = OSSymbol::withCStringNoCopy(cpu->cpu_id < 4 ? "Cortex-A53" : "Mongoose");""",
            """#if defined(STARLTE)
\tcpuName = OSSymbol::withCStringNoCopy("Cortex-A55");
#elif defined(HEROLTE)
\tcpuName = OSSymbol::withCStringNoCopy(cpu->cpu_id < 4 ? "Cortex-A53" : "Mongoose");"""
        ),
        (
            """#if defined(A5XELTE)
\tif (topology->boot_cpu->cpu_id != 0 ||
\t    topology->boot_cpu->phys_id != A5XELTE_BOOT_MPIDR) {
\t\tpanic(CPU_PORT_NAME " CPU: expected boot MPIDR 0x%x and logical CPU 0",
\t\t    A5XELTE_BOOT_MPIDR);""",
            """#if defined(A5XELTE)
\tif (topology->boot_cpu->cpu_id != 0) {
\t\tpanic(CPU_PORT_NAME " CPU: expected logical CPU 0 for boot MPIDR 0x%x",
\t\t    topology->boot_cpu->phys_id);"""
        ),
        (
            """#if defined(A5XELTE)
\tif (start_paddr != 0 || arg_paddr != 0 || cpu->phys_id == A5XELTE_BOOT_MPIDR ||""",
            """#if defined(A5XELTE)
\tif (start_paddr != 0 || arg_paddr != 0 || cpu->phys_id == ml_get_topology_info()->boot_cpu->phys_id ||"""
        ),
        (
            """#if defined(STARLTE)
\t/* The S9's PSCI 0.1 firmware numbers the A55 cores 1..3 (the vendor DTB
\t * reg), while the cores read MPIDR Aff1 = n (starsmpprobe=1). The M3
\t * cores are PSCI 0x100..0x103 and need the vendor PMU defaults first. */
\tuint64_t target = cpu->phys_id >> 8;
\tif (cpu->phys_id & 0x10000) {
\t\ttarget = cpu->phys_id & 0xffff;
\t\tstarlte_m3_pmu_defaults();
\t}""",
            """#if defined(STARLTE)
\t/* Exynos 850 PSCI 1.0 CPU_ON target_cpu = normalized Aff2:Aff1 (0x000..0x003, 0x100..0x103). */
\tuint64_t target = cpu->phys_id;"""
        ),
    ])

    # 6. osfmk/arm/arm_init.c: A21s safe built-in boot-args (no S9 MMIO drivers!)
    patch_file("osfmk/arm/arm_init.c", [
        (
            """static const char starlte_default_bootargs[] =
    "debug=0xc serial=3 cpus=8 rd=ufsroot0 ufsroot=USERDATA ufsrw=1 rootrw=1 "
    "cs_enforcement_disable=1 ignition_level=1 launchd_unsecure_cache=1 wdt=-1 "
    "a5fb_keep_on=1 sweetfb_scale=1 sweetfb_lw=828 sweetfb_lh=1792 a5fb_noconsole=1 a5fb_hz=60 sweetfb_gpu=1 "
    "vm_compression_limit=196608 cs_integrity_skip=1 sweetmemscale=4 sweetacl=1 sweetreboot=86400 usb_gadget=5 "
    "sweetinject=/usr/lib/s.dylib,backboardd,Preferences;/usr/lib/m.dylib,PosterBoard,SpringBoard;/usr/lib/b.dylib,backboardd "
    "sbprof=0 sbmtl=1 extra_ram=1 extra_ram_mask=127 cpufreq_gov=1 cpufreq_gov_big_min=1794000 "
    "sweetm3unpin=2 sweetspeed=100351 "
    /*
     * The S9's own hardware, on by default since 17 November 2026 so that one image has all of it to test together
     * (each was merged off; a run turns one off again by naming it on the loader's line: cp=0, abox=0, ...).
     * Appended last on purpose: when the loader's line leaves no room, these are the ones left out.
     * The temperatures read, the GPU's clock and the cores slowed by them, and a run ended at 110 C (tmu=3: until now
     * this tree read none by default and nothing stopped a hot run).
     * Sensors and rotation; restart and power off from iOS; the battery's gauge and the PMIC's clock, both read only;
     * Sileo's installs (persona_lax); Wi-Fi; the speakers, the codec, the microphone and Bluetooth; the modem, with its
     * NV data read from EFS READ ONLY into RAM (cp_nv=2: nothing is written to the phone) and its data context as
     * pdp_ip0 (cp_if=1: no data without cp_apn=, which only a run gives).
     */
    "tmu=3 shub=6 shub_hid=3 sweethalt=1 sweetfg=1 rtc_pmic=1 persona_lax=1 wifi_pcie=6 brcmfmac=8 "
    "abox=8 max98512=3 cs47l93=6 abox_mic=1 abox_rmem=1 bt=7 "
    "cp=7 cp_nv=2 cp_delay=40 cp_rfs=1 cp_if=1 "
    /*
     * On by default since 18 November 2026, at the owner's word ("enable all features"; faults are for follow-ups):
     * the cameras (cam_s9=7, front and rear in one boot with cam_switch=1; without the root's camera step nothing
     * opens the device), the GPU's job interrupt (sgu_irq=1: attached and measured on a phone, 17 November), files
     * compressed as decmpfs type 16 read (decmpfs_lz4=1: the app switcher's snapshots; not yet run on a phone), and
     * dyld's @rpath for the AMFI stand-in's processes (sweetatpath=1: Books' and Podcasts' widgets).
     * Not here on purpose: sweetfb_dpp (the display's scaler: after its probe boot), sweetc2 (a core powered down has
     * never come back on a phone), gov_policy=3 and ocp_guard=4 (above 1794 MHz), sgu_depth (measured harmful).
     */
    "cam_s9=7 cam_switch=1 sgu_irq=1 decmpfs_lz4=1 sweetatpath=1";""",
            """static const char starlte_default_bootargs[] =
    "rd=ufsroot0 ufsroot=system ufsrw=1 rootrw=1 herolog=1 decmpfs_lz4=1 cpus=7 a21s_input=1 startouch=92 "
    "cs_enforcement_disable=1 ignition_level=1 launchd_unsecure_cache=1 cs_integrity_skip=1 persona_lax=1 sweetatpath=1 sweetacl=1 wdt=-1 "
    "debug=0xc serial=3 a5fb_keep_on=1 sweetfb_native=1 sweetfb_lw=720 sweetfb_lh=1600 a5fb_noconsole=1 "
    "sweetinject=/usr/lib/s.dylib,backboardd,Preferences;/usr/lib/m.dylib,PosterBoard,SpringBoard "
    "sgu_irq=1 extra_ram=1 extra_ram_mask=255 sweetspeed=402914757 vm_compression_limit=196608 sweetmemscale=4 sweethalt=1";"""
        ),
    ])

    # 7. bsd/dev/a5xelte_keys.c: point A5_ALIVE_PA at Exynos 850 GPIO_ALIVE (0x11850000) & default off (a5keys=0)
    patch_file("bsd/dev/a5xelte_keys.c", [
        (
            "#if defined(ARM64_BOARD_CONFIG_STARLTE)\n#define A5_ALIVE_PA 0x14050000u",
            "#if defined(ARM64_BOARD_CONFIG_STARLTE)\n#define A5_ALIVE_PA 0x11850000u"
        ),
        (
            "int enabled = 1;",
            "int enabled = 0;"
        ),
    ])

    # 8. bsd/dev/starlte_sd.c: disable unconditionally (0x11500000 is A21s Mali-G52 GPU!)
    patch_file("bsd/dev/starlte_sd.c", [
        (
            "int\nstarlte_sd_rootdev(dev_t *dev)\n{",
            "int\nstarlte_sd_rootdev(dev_t *dev)\n{\n\t(void)dev;\n\treturn ENODEV; /* 0x11500000 is Mali-G52 on Exynos 850 */"
        ),
        (
            "void\nstarlte_sd_probe(void)\n{",
            "void\nstarlte_sd_probe(void)\n{\n\treturn; /* 0x11500000 is Mali-G52 on Exynos 850 */"
        ),
        (
            "int\nstarlte_sd_dump_log(const void *ring, uint32_t size, uint32_t bufx)\n{",
            "int\nstarlte_sd_dump_log(const void *ring, uint32_t size, uint32_t bufx)\n{\n\t(void)ring; (void)size; (void)bufx;\n\treturn ENODEV;"
        ),
    ])

    # 9. bsd/dev/herolte_ufs.c: Exynos 850 dwmmc0 (0x12100000) + A21SIOS1 extent map + PMU 0x11860000 + pstore 0xf0f10000
    a21s_emmc_block = r"""
/* ---- Galaxy A21s (SM-A217F, Exynos 850) dwmmc0 (0x12100000) + A21SIOS1 extent map ---- */
#define A21S_MMC_PA             0x12100000UL
#define A21S_MMC_MAP            0x2000UL
#define A21S_BOOT_PART_BYTES    37748736UL
#define A21S_MAP_BYTES          65536UL
#define A21S_LOGDUMP_FIRST      71552UL
#define A21S_LOGDUMP_SECS       1919UL

#define MMC_CTRL        0x00
#define MMC_BLKSIZ      0x1c
#define MMC_BYTCNT      0x20
#define MMC_CMDARG      0x28
#define MMC_CMD         0x2c
#define MMC_RESP0       0x30
#define MMC_RINTSTS     0x44
#define MMC_STATUS      0x48
#define MMC_HCON        0x70
#define MMC_BMOD        0x80
#define MMC_DATA        0x200

#define MMC_CMD_START    (1u << 31)
#define MMC_CMD_HOLD     (1u << 29)
#define MMC_CMD_WAITPRV  (1u << 13)
#define MMC_CMD_AUTOSTOP (1u << 12)
#define MMC_CMD_WR       (1u << 10)
#define MMC_CMD_DATA     (1u << 9)
#define MMC_CMD_CRC      (1u << 8)
#define MMC_CMD_RESP     (1u << 6)

#define MMC_INT_RE       (1u << 1)
#define MMC_INT_CD       (1u << 2)
#define MMC_INT_DTO      (1u << 3)
#define MMC_INT_RCRC     (1u << 6)
#define MMC_INT_DCRC     (1u << 7)
#define MMC_INT_RTO      (1u << 8)
#define MMC_INT_DRTO     (1u << 9)
#define MMC_INT_HTO      (1u << 10)
#define MMC_INT_HLE      (1u << 12)
#define MMC_INT_SBE      (1u << 13)
#define MMC_INT_ACD      (1u << 14)
#define MMC_INT_EBE      (1u << 15)
#define MMC_INT_ERR      (MMC_INT_RE | MMC_INT_RCRC | MMC_INT_DCRC | MMC_INT_RTO | MMC_INT_DRTO | MMC_INT_HTO | MMC_INT_HLE | MMC_INT_SBE | MMC_INT_EBE)

struct a21s_map_hdr  { char magic[8]; uint32_t version, nfiles, nexts, crc; uint32_t pad[2]; };
struct a21s_map_file { char name[16]; char part[16]; uint64_t size; uint32_t first_ext, n_ext; };
struct a21s_map_ext  { uint64_t logical, phys, len; };

static int a21s_mmc_rca = 1;
static int a21s_mmc_wide64;
static uint64_t a21s_boot_first, a21s_boot_last;
static uint64_t a21s_userdata_first, a21s_userdata_last;
static uint8_t a21s_mapbuf[A21S_MAP_BYTES] __attribute__((aligned(8)));
static struct a21s_map_ext *a21s_sys_exts;
static uint32_t a21s_sys_nexts;
static uint64_t a21s_sys_bytes;

static inline uint32_t a21s_mr(uint32_t o) {
	uint32_t v = *(volatile uint32_t *)(su.regs + o);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}
static inline void a21s_mw(uint32_t o, uint32_t v) {
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(su.regs + o) = v;
}
static uint64_t a21s_ms_now(void) {
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000ULL;
}
static int a21s_mmc_wait_idle(uint32_t ms) {
	uint64_t t = a21s_ms_now() + ms;
	while (a21s_mr(MMC_STATUS) & ((1u << 9) | (1u << 10))) {
		if (a21s_ms_now() > t) return -1;
	}
	return 0;
}
static int a21s_mmc_send_cmd(uint32_t cmd, uint32_t arg, uint32_t *resp) {
	uint64_t t = a21s_ms_now() + 500;
	a21s_mw(MMC_RINTSTS, 0xffffffffu);
	a21s_mw(MMC_CMDARG, arg);
	a21s_mw(MMC_CMD, cmd | MMC_CMD_START | MMC_CMD_HOLD);
	while (a21s_mr(MMC_CMD) & MMC_CMD_START) {
		if (a21s_ms_now() > t) return -2;
	}
	t = a21s_ms_now() + 500;
	uint32_t s;
	while (!((s = a21s_mr(MMC_RINTSTS)) & MMC_INT_CD)) {
		if (a21s_ms_now() > t) return -3;
	}
	if (s & MMC_INT_RTO) { a21s_mw(MMC_RINTSTS, 0xffffffffu); return -4; }
	if (s & (MMC_INT_RCRC | MMC_INT_RE)) { a21s_mw(MMC_RINTSTS, 0xffffffffu); return -5; }
	if (resp) *resp = a21s_mr(MMC_RESP0);
	a21s_mw(MMC_RINTSTS, MMC_INT_CD);
	return 0;
}
static int a21s_mmc_init_hw(void) {
	a21s_mmc_wide64 = (((a21s_mr(MMC_HCON) >> 7) & 7u) == 2u) ? 1 : 0;
	a21s_mw(MMC_CTRL, a21s_mr(MMC_CTRL) & ~(1u << 25));
	a21s_mw(MMC_BMOD, a21s_mr(MMC_BMOD) & ~(1u << 7));
	(void)a21s_mmc_wait_idle(200);
	a21s_mw(MMC_CTRL, a21s_mr(MMC_CTRL) | (1u << 1));
	for (int i = 0; i < 100000 && (a21s_mr(MMC_CTRL) & (1u << 1)); i++) {}
	a21s_mw(MMC_RINTSTS, 0xffffffffu);
	for (int r = 1; r <= 2; r++) {
		uint32_t resp = 0;
		int rc = a21s_mmc_send_cmd(13u | MMC_CMD_RESP | MMC_CMD_CRC | MMC_CMD_WAITPRV, (uint32_t)r << 16, &resp);
		printf("A21S EMMC: CMD13 rca=%d -> %d resp=%#x wide64=%d\n", r, rc, resp, a21s_mmc_wide64);
		if (rc == 0) {
			a21s_mmc_rca = r;
			int st = (resp >> 9) & 15;
			if (st != 4) {
				uint32_t r7 = 0;
				(void)a21s_mmc_send_cmd(7u | MMC_CMD_RESP | MMC_CMD_CRC | MMC_CMD_WAITPRV, (uint32_t)r << 16, &r7);
			}
			return 0;
		}
	}
	return EIO;
}
static int a21s_mmc_read_chunk(uint64_t lba, uint32_t n, uint8_t *buf) {
	if (a21s_mmc_wait_idle(500)) return EIO;
	a21s_mw(MMC_CTRL, a21s_mr(MMC_CTRL) | (1u << 1));
	for (int i = 0; i < 100000 && (a21s_mr(MMC_CTRL) & (1u << 1)); i++) {}
	a21s_mw(MMC_BLKSIZ, 512);
	a21s_mw(MMC_BYTCNT, n * 512);
	a21s_mw(MMC_RINTSTS, 0xffffffffu);
	uint32_t cmd = (n == 1)
	    ? (17u | MMC_CMD_RESP | MMC_CMD_CRC | MMC_CMD_DATA | MMC_CMD_WAITPRV)
	    : (18u | MMC_CMD_RESP | MMC_CMD_CRC | MMC_CMD_DATA | MMC_CMD_WAITPRV | MMC_CMD_AUTOSTOP);
	uint64_t t = a21s_ms_now() + 1000;
	a21s_mw(MMC_CMDARG, (uint32_t)lba);
	a21s_mw(MMC_CMD, cmd | MMC_CMD_START | MMC_CMD_HOLD);
	while (a21s_mr(MMC_CMD) & MMC_CMD_START) {
		if (a21s_ms_now() > t) return EIO;
	}
	uint32_t left = n * 512;
	uint32_t *p = (uint32_t *)(void *)buf;
	uint64_t deadline = a21s_ms_now() + 3000 + n;
	while (left) {
		uint32_t s = a21s_mr(MMC_RINTSTS);
		if (s & MMC_INT_ERR) {
			printf("A21S EMMC: rd err rintsts=%#x lba=%#llx n=%u\n", s, lba, n);
			a21s_mw(MMC_RINTSTS, 0xffffffffu);
			return EIO;
		}
		uint32_t fc = (a21s_mr(MMC_STATUS) >> 17) & 0x1fffu;
		if (!fc) {
			if (a21s_ms_now() > deadline) {
				printf("A21S EMMC: rd FIFO stall rintsts=%#x left=%u\n", s, left);
				return ETIMEDOUT;
			}
			continue;
		}
		deadline = a21s_ms_now() + 3000;
		if (a21s_mmc_wide64) {
			volatile uint64_t *d = (volatile uint64_t *)(su.regs + MMC_DATA);
			while (fc-- && left >= 8) {
				uint64_t v = *d;
				p[0] = (uint32_t)v;
				p[1] = (uint32_t)(v >> 32);
				p += 2;
				left -= 8;
			}
		} else {
			volatile uint32_t *d = (volatile uint32_t *)(su.regs + MMC_DATA);
			while (fc-- && left >= 4) {
				*p++ = *d;
				left -= 4;
			}
		}
	}
	uint64_t t2 = a21s_ms_now() + 1000;
	while (!(a21s_mr(MMC_RINTSTS) & MMC_INT_DTO) && a21s_ms_now() < t2) {}
	if (n > 1) {
		uint64_t t3 = a21s_ms_now() + 500;
		while (!(a21s_mr(MMC_RINTSTS) & MMC_INT_ACD) && a21s_ms_now() < t3) {}
	}
	uint32_t s = a21s_mr(MMC_RINTSTS);
	a21s_mw(MMC_RINTSTS, 0xffffffffu);
	su.ncmds++;
	su.last_lba = lba;
	return (s & MMC_INT_ERR) ? EIO : 0;
}
static int a21s_mmc_read_sectors(uint64_t lba, uint32_t n, void *buf) {
	uint8_t *b = (uint8_t *)buf;
	while (n) {
		uint32_t c = n > 2048u ? 2048u : n;
		int err = 0;
		for (int tries = 0; tries < 3; tries++) {
			err = a21s_mmc_read_chunk(lba, c, b);
			if (!err) break;
			delay(1000);
		}
		if (err) return err;
		lba += c;
		n -= c;
		b += (size_t)c * 512u;
	}
	return 0;
}
static int a21s_mmc_write_chunk(uint64_t lba, uint32_t n, const uint8_t *buf) {
	if (a21s_mmc_wait_idle(500)) return EIO;
	a21s_mw(MMC_CTRL, a21s_mr(MMC_CTRL) | (1u << 1));
	for (int i = 0; i < 100000 && (a21s_mr(MMC_CTRL) & (1u << 1)); i++) {}
	a21s_mw(MMC_BLKSIZ, 512);
	a21s_mw(MMC_BYTCNT, n * 512);
	a21s_mw(MMC_RINTSTS, 0xffffffffu);
	uint32_t cmd = (n == 1)
	    ? (24u | MMC_CMD_RESP | MMC_CMD_CRC | MMC_CMD_DATA | MMC_CMD_WR | MMC_CMD_WAITPRV)
	    : (25u | MMC_CMD_RESP | MMC_CMD_CRC | MMC_CMD_DATA | MMC_CMD_WR | MMC_CMD_WAITPRV | MMC_CMD_AUTOSTOP);
	uint64_t t = a21s_ms_now() + 1000;
	a21s_mw(MMC_CMDARG, (uint32_t)lba);
	a21s_mw(MMC_CMD, cmd | MMC_CMD_START | MMC_CMD_HOLD);
	while (a21s_mr(MMC_CMD) & MMC_CMD_START) {
		if (a21s_ms_now() > t) return EIO;
	}
	uint32_t left = n * 512;
	const uint32_t *p = (const uint32_t *)(const void *)buf;
	uint64_t deadline = a21s_ms_now() + 3000;
	const uint32_t fifo_words = 64;
	while (left) {
		uint32_t s = a21s_mr(MMC_RINTSTS);
		if (s & MMC_INT_ERR) {
			printf("A21S EMMC: wr err rintsts=%#x lba=%#llx n=%u\n", s, lba, n);
			a21s_mw(MMC_RINTSTS, 0xffffffffu);
			return EIO;
		}
		uint32_t fc = (a21s_mr(MMC_STATUS) >> 17) & 0x1fffu;
		uint32_t room = fifo_words > fc ? fifo_words - fc : 0;
		if (!room) {
			if (a21s_ms_now() > deadline) {
				printf("A21S EMMC: wr FIFO stall lba=%#llx left=%u\n", lba, left);
				return ETIMEDOUT;
			}
			continue;
		}
		deadline = a21s_ms_now() + 3000;
		if (a21s_mmc_wide64) {
			volatile uint64_t *d = (volatile uint64_t *)(su.regs + MMC_DATA);
			while (room-- && left >= 8) {
				*d = (uint64_t)p[0] | ((uint64_t)p[1] << 32);
				p += 2;
				left -= 8;
			}
		} else {
			volatile uint32_t *d = (volatile uint32_t *)(su.regs + MMC_DATA);
			while (room-- && left >= 4) {
				*d = *p++;
				left -= 4;
			}
		}
	}
	uint64_t t2 = a21s_ms_now() + 3000;
	while (!(a21s_mr(MMC_RINTSTS) & MMC_INT_DTO)) {
		if (a21s_ms_now() > t2) return ETIMEDOUT;
	}
	if (n > 1) {
		uint64_t t3 = a21s_ms_now() + 500;
		while (!(a21s_mr(MMC_RINTSTS) & MMC_INT_ACD) && a21s_ms_now() < t3) {}
	}
	a21s_mw(MMC_RINTSTS, 0xffffffffu);
	for (int i = 0; i < 2000; i++) {
		uint32_t r = 0;
		if (a21s_mmc_send_cmd(13u | MMC_CMD_RESP | MMC_CMD_CRC | MMC_CMD_WAITPRV, (uint32_t)a21s_mmc_rca << 16, &r) == 0 &&
		    (r & (1u << 8)) && ((r >> 9) & 15u) == 4u) {
			su.ncmds++;
			su.last_lba = lba;
			return 0;
		}
		delay(500);
	}
	return ETIMEDOUT;
}
static int a21s_mmc_write_sectors(uint64_t lba, uint32_t n, const void *buf) {
	const uint8_t *b = (const uint8_t *)buf;
	while (n) {
		uint32_t c = n > 128u ? 128u : n;
		int err = a21s_mmc_write_chunk(lba, c, b);
		if (err) return err;
		lba += c;
		n -= c;
		b += (size_t)c * 512u;
	}
	return 0;
}
static uint32_t a21s_crc32(const uint8_t *p, size_t n) {
	uint32_t c = 0xffffffffu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(c & 1u));
		}
	}
	return ~c;
}
static int a21s_load_extents(void) {
	uint8_t *b = (uint8_t *)(su.dma + DMA_BOUNCE);
	if (a21s_mmc_read_sectors(1, 1, b) != 0 || memcmp(b, "EFI PART", 8) != 0) {
		printf("A21S EMMC: bad GPT header\n");
		return ENXIO;
	}
	uint64_t elba = 0;
	uint32_t nent = 0, esz = 0;
	memcpy(&elba, b + 72, 8);
	memcpy(&nent, b + 80, 4);
	memcpy(&esz, b + 84, 4);
	if (esz != 128 || nent == 0 || nent > 128) {
		printf("A21S EMMC: bad GPT table %u x %u\n", nent, esz);
		return ENXIO;
	}
	if (a21s_mmc_read_sectors(elba, (nent * esz + 511u) / 512u, b) != 0) {
		return EIO;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = b + i * esz;
		uint64_t first = 0, last = 0;
		memcpy(&first, e + 32, 8);
		memcpy(&last, e + 40, 8);
		if (!first && !last) continue;
		char nm[37];
		unsigned c = 0;
		for (; c < 36; c++) {
			uint16_t ch = (uint16_t)(e[56 + 2 * c] | (e[57 + 2 * c] << 8));
			if (!ch) break;
			nm[c] = ch < 128 ? (char)ch : '?';
		}
		nm[c] = 0;
		if (strcmp(nm, "boot") == 0) { a21s_boot_first = first; a21s_boot_last = last; }
		else if (strcmp(nm, "userdata") == 0) { a21s_userdata_first = first; a21s_userdata_last = last; }
	}
	if (!a21s_boot_first || !a21s_userdata_first) {
		printf("A21S EMMC: missing boot (%#llx) or userdata (%#llx) GPT\n", a21s_boot_first, a21s_userdata_first);
		return ENOENT;
	}
	uint64_t map_lba = a21s_boot_first + (A21S_BOOT_PART_BYTES - A21S_MAP_BYTES) / 512u;
	if (a21s_mmc_read_sectors(map_lba, A21S_MAP_BYTES / 512u, a21s_mapbuf) != 0) {
		printf("A21S EMMC: failed to read extent map at LBA %#llx\n", map_lba);
		return EIO;
	}
	struct a21s_map_hdr *mh = (struct a21s_map_hdr *)(void *)a21s_mapbuf;
	if (memcmp(mh->magic, "A21SIOS1", 8) != 0) {
		printf("A21S EMMC: invalid A21SIOS1 magic\n");
		return ENOENT;
	}
	uint32_t want_crc = mh->crc;
	mh->crc = 0;
	uint32_t got_crc = a21s_crc32(a21s_mapbuf, A21S_MAP_BYTES);
	mh->crc = want_crc;
	if (got_crc != want_crc) {
		printf("A21S EMMC: extent map CRC mismatch %#x != %#x\n", got_crc, want_crc);
		return EIO;
	}
	struct a21s_map_file *mf = (struct a21s_map_file *)(void *)(a21s_mapbuf + sizeof(*mh));
	struct a21s_map_ext *me = (struct a21s_map_ext *)(void *)(a21s_mapbuf + sizeof(*mh) + 8 * sizeof(*mf));
	for (uint32_t i = 0; i < mh->nfiles && i < 8; i++) {
		if (strcmp(mf[i].name, "system") == 0) {
			if (strcmp(mf[i].part, "userdata") != 0 || mf[i].n_ext == 0 ||
			    mf[i].first_ext + mf[i].n_ext > mh->nexts) {
				return EINVAL;
			}
			a21s_sys_exts = &me[mf[i].first_ext];
			a21s_sys_nexts = mf[i].n_ext;
			a21s_sys_bytes = mf[i].size;
			/* Verify every extent lies strictly within userdata GPT bounds and is 512B aligned. */
			uint64_t part_bytes = (a21s_userdata_last - a21s_userdata_first + 1ULL) * 512ULL;
			for (uint32_t k = 0; k < a21s_sys_nexts; k++) {
				if ((a21s_sys_exts[k].logical & 511ULL) || (a21s_sys_exts[k].phys & 511ULL) ||
				    (a21s_sys_exts[k].len & 511ULL) || a21s_sys_exts[k].len == 0 ||
				    a21s_sys_exts[k].phys > part_bytes ||
				    a21s_sys_exts[k].len > part_bytes - a21s_sys_exts[k].phys) {
					printf("A21S EMMC: bad extent %u!\n", k);
					return EINVAL;
				}
			}
			printf("A21S EMMC: mapped /data/ios/system: %llu MiB across %u extents on userdata (LBA %#llx)\n",
			    a21s_sys_bytes >> 20, a21s_sys_nexts, a21s_userdata_first);
			return 0;
		}
	}
	printf("A21S EMMC: 'system' entry not found in A21SIOS1 map\n");
	return ENOENT;
}
static int a21s_sys_io(uint64_t off, uint64_t len, uint8_t *buf, bool write) {
	while (len > 0) {
		const struct a21s_map_ext *e = NULL;
		/* Binary/linear search extent covering logical byte offset `off`. */
		uint32_t lo = 0, hi = a21s_sys_nexts;
		while (lo < hi) {
			uint32_t mid = lo + (hi - lo) / 2;
			if (off < a21s_sys_exts[mid].logical) {
				hi = mid;
			} else if (off >= a21s_sys_exts[mid].logical + a21s_sys_exts[mid].len) {
				lo = mid + 1;
			} else {
				e = &a21s_sys_exts[mid];
				break;
			}
		}
		if (!e) {
			for (uint32_t i = 0; i < a21s_sys_nexts; i++) {
				if (off >= a21s_sys_exts[i].logical && off < a21s_sys_exts[i].logical + a21s_sys_exts[i].len) {
					e = &a21s_sys_exts[i];
					break;
				}
			}
		}
		if (!e) {
			printf("A21S EMMC: logical offset %#llx unmapped!\n", off);
			return EIO;
		}
		uint64_t within = off - e->logical;
		uint64_t chunk = MIN(len, e->len - within);
		uint64_t disk_lba = a21s_userdata_first + (e->phys + within) / 512ULL;
		uint32_t secs = (uint32_t)(chunk / 512ULL);
		if (disk_lba < a21s_userdata_first || disk_lba + secs - 1ULL > a21s_userdata_last) {
			printf("A21S EMMC: refusing out-of-userdata LBA %#llx+%u\n", disk_lba, secs);
			return EPERM;
		}
		int err = write ? a21s_mmc_write_sectors(disk_lba, secs, buf)
		                : a21s_mmc_read_sectors(disk_lba, secs, buf);
		if (err) return err;
		off += chunk;
		len -= chunk;
		buf += chunk;
	}
	return 0;
}
"""
    patch_file("bsd/dev/herolte_ufs.c", [
        (
            """/* READ(10) of count device blocks at lba into the bounce buffer. */
static int
ufs_read_blocks(uint64_t lba, uint32_t count)
{
\tif (lba > UINT32_MAX || count > BOUNCE_SIZE / UFS_BLOCK) {
\t\treturn EINVAL;
\t}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
\tif (lba + count - 1 > UINT32_MAX) {
\t\treturn EINVAL;
\t}
\tint error = 0;
\tuint32_t per = su.prdt_stride ? STARLTE_MULTI : 1u;
\tfor (uint32_t i = 0; i < count && !error; i += per) {
\t\tuint64_t b = lba + i;
\t\tuint32_t n = MIN(per, count - i);
\t\tuint8_t cdb[10] = {
\t\t\t0x28, 0,
\t\t\t(uint8_t)(b >> 24), (uint8_t)(b >> 16), (uint8_t)(b >> 8), (uint8_t)b,
\t\t\t0, (uint8_t)(n >> 8), (uint8_t)n, 0
\t\t};
\t\tsu.bounce_off = i * UFS_BLOCK;
\t\terror = ufs_scsi(cdb, sizeof(cdb), n * UFS_BLOCK, false);
\t}
\tsu.bounce_off = 0;
\treturn error;
#else
\tuint8_t cdb[10] = {
\t\t0x28, 0,
\t\t(uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
\t\t0, (uint8_t)(count >> 8), (uint8_t)count, 0
\t};
\treturn ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, false);
#endif
}

/*
 * WRITE(10) of count device blocks from the bounce buffer to lba. The range
 * must lie inside the partition: this is the only place data goes out.
 */
static int
ufs_write_blocks(uint64_t lba, uint32_t count)
{
\tif (!su.writable || !su.nblocks || count == 0 || count > BOUNCE_SIZE / UFS_BLOCK ||
\t    lba < su.first_block || lba - su.first_block >= su.nblocks ||
\t    count > su.nblocks - (lba - su.first_block) || lba + count - 1 > UINT32_MAX) {
\t\tprintf("HEROLTE UFS: refusing write of %u blocks at 0x%llx\\n", count, lba);
\t\treturn EPERM;
\t}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
\tint error = 0;
\tuint32_t per = su.prdt_stride ? STARLTE_MULTI : 1u;
\tfor (uint32_t i = 0; i < count && !error; i += per) {
\t\tuint64_t b = lba + i;
\t\tuint32_t n = MIN(per, count - i);
\t\tuint8_t cdb[10] = {
\t\t\t0x2a, 0,
\t\t\t(uint8_t)(b >> 24), (uint8_t)(b >> 16), (uint8_t)(b >> 8), (uint8_t)b,
\t\t\t0, (uint8_t)(n >> 8), (uint8_t)n, 0
\t\t};
\t\tsu.bounce_off = i * UFS_BLOCK;
\t\terror = ufs_scsi(cdb, sizeof(cdb), n * UFS_BLOCK, true);
\t}
\tsu.bounce_off = 0;
\treturn error;
#else
\tuint8_t cdb[10] = {
\t\t0x2a, 0,
\t\t(uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
\t\t0, (uint8_t)(count >> 8), (uint8_t)count, 0
\t};
\treturn ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, true);
#endif
}

static int
ufs_sync_cache(void)
{
\tstatic const uint8_t cdb[10] = { 0x35 };       /* SYNCHRONIZE CACHE(10), whole LUN */
\treturn ufs_scsi(cdb, sizeof(cdb), 0, false);
}""",
            a21s_emmc_block + """
static int
ufs_read_blocks(uint64_t lba, uint32_t count)
{
\tif (count == 0 || count > BOUNCE_SIZE / UFS_BLOCK || !su.nblocks ||
\t    lba < su.first_block || lba - su.first_block >= su.nblocks ||
\t    count > su.nblocks - (lba - su.first_block)) {
\t\treturn EINVAL;
\t}
\treturn a21s_sys_io((lba - su.first_block) * UFS_BLOCK, (uint64_t)count * UFS_BLOCK,
\t    (uint8_t *)(su.dma + DMA_BOUNCE), false);
}

static int
ufs_write_blocks(uint64_t lba, uint32_t count)
{
\tif (!su.writable || !su.nblocks || count == 0 || count > BOUNCE_SIZE / UFS_BLOCK ||
\t    lba < su.first_block || lba - su.first_block >= su.nblocks ||
\t    count > su.nblocks - (lba - su.first_block)) {
\t\tprintf("A21S EMMC: refusing write of %u blocks at 0x%llx\\n", count, lba);
\t\treturn EPERM;
\t}
\treturn a21s_sys_io((lba - su.first_block) * UFS_BLOCK, (uint64_t)count * UFS_BLOCK,
\t    (uint8_t *)(su.dma + DMA_BOUNCE), true);
}

static int
ufs_sync_cache(void)
{
\treturn 0;
}"""
        ),
        (
            """#define STARLTE_PMU_PA          0x14060000
#define STARLTE_PMSG_PA         0xFED14000
#define STARLTE_PMSG_DATA       0x3F00u         /* the loader's cap, clear of the UEFI latches */""",
            """#define STARLTE_PMU_PA          0x11860000
#define STARLTE_PMSG_PA         0xf0f14000
#define STARLTE_PMSG_DATA       0x3F00u         /* Exynos 850 log_pstore pmsg 0xf0f14000 (16 KiB) */"""
        ),
        (
            """\tpmu[0x808 / 4] = 0x12345678U;
\tpmu[0x80c / 4] = 0x12345674U;
\t__asm__ volatile ("dsb sy" ::: "memory");
\tpmu[0x400 / 4] = 1;""",
            """\tpmu[0x808 / 4] = 0x12345678U;
\t/* Reboot into stage1 default (Android + ADB) unless sweetrecovery=1 is explicitly set. */
\tuint32_t rec = 0;
\tPE_parse_boot_argn("sweetrecovery", &rec, sizeof(rec));
\tpmu[0x80c / 4] = rec ? 0x12345674U : 0x12345670U;
\t__asm__ volatile ("dsb sy" ::: "memory");
\tpmu[0x3a00 / 4] = 2;"""
        ),
        (
            """\t\th[0] = 0x43474244U;                     /* "DBGC" */
\t\th[1] = o % STARLTE_PMSG_DATA;
\t\th[2] = o;
\t\t__asm__ volatile ("dsb sy" ::: "memory");
\t}
\tstarlte_reset_to_recovery();
}

static void
starlte_reboot_setup(void)""",
            """\t\th[0] = 0x43474244U;                     /* "DBGC" */
\t\th[1] = o % STARLTE_PMSG_DATA;
\t\th[2] = o;
\t\t__asm__ volatile ("dsb sy" ::: "memory");
\t\t/* Also persist the buffer into boot partition logdump sectors (71552..73470) if eMMC is idle. */
\t\tif (su.regs != 0 && a21s_boot_first != 0 && lck_mtx_try_lock(&herolte_ufs_lock)) {
\t\t\tuint32_t bytes = (o + 12u + 511u) & ~511u;
\t\t\tuint32_t secs = MIN(bytes / 512u, (uint32_t)A21S_LOGDUMP_SECS);
\t\t\tif (secs > 0) {
\t\t\t\t(void)a21s_mmc_write_sectors(a21s_boot_first + A21S_LOGDUMP_FIRST, secs, (const void *)starlte_pmsg_va);
\t\t\t}
\t\t\tlck_mtx_unlock(&herolte_ufs_lock);
\t\t}
\t}
\tstarlte_reset_to_recovery();
}

static void
starlte_reboot_setup(void)"""
        ),
        (
            """\tstarlte_pmu_va = (volatile uint32_t *)ml_io_map(STARLTE_PMU_PA, PAGE_SIZE);
\tstarlte_pmsg_va = (volatile uint8_t *)ml_io_map(STARLTE_PMSG_PA, 0x4000);""",
            """\tstarlte_pmu_va = (volatile uint32_t *)ml_io_map(STARLTE_PMU_PA, 0x4000);
\tstarlte_pmsg_va = (volatile uint8_t *)ml_io_map(STARLTE_PMSG_PA, 0x4000);"""
        ),
        (
            """#define STARLTE_PS_HOLD_PAGE    0x14063000
#define STARLTE_PS_HOLD_OFF     0x30C""",
            """#define STARLTE_PS_HOLD_PAGE    0x11860000
#define STARLTE_PS_HOLD_OFF     0x30C"""
        ),
        (
            """\tpmu[0x808 / 4] = 0x12345678U;
\tpmu[0x80c / 4] = 0x12345670U;
\t__asm__ volatile ("dsb sy" ::: "memory");
\tpmu[0x400 / 4] = 1;""",
            """\tpmu[0x808 / 4] = 0x12345678U;
\tpmu[0x80c / 4] = 0x12345670U;
\t__asm__ volatile ("dsb sy" ::: "memory");
\tpmu[0x3a00 / 4] = 2;"""
        ),
        (
            "ml_io_map(STARLTE_PMU_PA, PAGE_SIZE)",
            "ml_io_map(STARLTE_PMU_PA, 0x4000)"
        ),
        (
            """\t\tprintf("HEROLTE UFS: hb %d: %llu cmds, last lba 0x%llx, busy 0x%x, dbr 0x%x\\n",
\t\t    i, su.ncmds, su.last_lba, su.busy, ufs_rd(REG_UTRLDBR));""",
            """\t\tprintf("A21S EMMC: hb %d: %llu cmds, last lba 0x%llx, status 0x%x\\n",
\t\t    i, su.ncmds, su.last_lba, a21s_mr(MMC_STATUS));"""
        ),
        (
            """static int
herolte_ufs_init(void)
{
\tDTEntry node;
\tuintptr_t const *reg = NULL;
\tunsigned int size = 0;
\tchar want[64] = "USERDATA";
\tint error;

\tif (SecureDTLookupEntry(NULL, "/herolte-ufs", &node) != kSuccess ||
\t    SecureDTGetProperty(node, "reg", (void const **)&reg, &size) != kSuccess ||
\t    size < 2 * sizeof(uintptr_t)) {
\t\tprintf("HEROLTE UFS: no /herolte-ufs node in the device tree\\n");
\t\treturn ENXIO;
\t}
\tPE_parse_boot_argn("ufsroot", want, sizeof(want));
\tuint32_t rw = 0;
\tPE_parse_boot_argn("ufsrw", &rw, sizeof(rw));
\tuint32_t cache = 0, wait = 0;
\tPE_parse_boot_argn("ufscache", &cache, sizeof(cache));
\tPE_parse_boot_argn("ufswait", &wait, sizeof(wait));
\tsu.cache = (cache == 1);
\tsu.wait = (wait == 1);

\tsu.regs = ml_io_map(reg[0], MAX(reg[1], (uintptr_t)HEROLTE_UFS_MAP));
\tif (kmem_alloc_contig(kernel_map, &su.dma, DMA_SIZE, PAGE_MASK, 0, 0,
\t    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
\t\tprintf("HEROLTE UFS: no contiguous DMA memory\\n");
\t\treturn ENOMEM;
\t}
\tppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)su.dma);
\tif (!pn) {
\t\treturn ENOMEM;
\t}
\tsu.dma_pa = ptoa_64(pn);
\tsu.lun = 0;
\tsu.secsize = DEV_BSIZE;

\tlck_mtx_lock(&herolte_ufs_lock);
\terror = ufs_take_over_host();
\tif (!error) {
\t\t(void)ufs_wake_device();
\t}
\tif (!error) {
\t\tstatic const uint8_t tur[6] = { 0x00 };
\t\t/* The first command after the bootloader may report a unit attention. */
\t\tfor (int i = 0; i < 3 && (error = ufs_scsi(tur, sizeof(tur), 0, false)); i++) {
\t\t}
\t}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
\tif (!error) {
\t\tstarlte_ufsp_open();
\t\tuint32_t multi = 0;     /* experimental: a failed test can stop the host */
\t\tPE_parse_boot_argn("ufsmulti", &multi, sizeof(multi));
\t\tif (multi) {
\t\t\tstarlte_ufs_pick_stride();
\t\t}
\t}
#endif
\tif (!error) {
\t\terror = ufs_find_partition(want);
\t}
\tlck_mtx_unlock(&herolte_ufs_lock);
\tif (error) {
\t\treturn error;
\t}

\tsu.bmajor = bdevsw_add(-1, &herolte_ufs_bdevsw);
\tif (su.bmajor < 0) {
\t\treturn ENXIO;
\t}
\tsu.cmajor = cdevsw_add_with_bdev(-1, &herolte_ufs_cdevsw, su.bmajor);
\tif (su.cmajor < 0) {
\t\treturn ENXIO;
\t}
\tsu.bdev = makedev(su.bmajor, 0);
\tdevfs_make_node(su.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "ufsroot0");
\tdevfs_make_node(makedev(su.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rufsroot0");
\t/* Only now, with the partition bounds known, can writes be allowed. */
\tsu.writable = (rw != 0);
\tsu.ready = true;
\tprintf("HEROLTE UFS: \\"%s\\" on LUN %u: first block 0x%llx, %llu MiB, %s, DMA at 0x%llx\\n",
\t    want, su.lun, su.first_block, su.nblocks * UFS_BLOCK >> 20,
\t    su.writable ? "read-write" : "read-only", su.dma_pa);
\therolte_log_start();
\treturn 0;
}""",
            """static int
herolte_ufs_init(void)
{
\tuint32_t rw = 0;
\tPE_parse_boot_argn("ufsrw", &rw, sizeof(rw));
\tsu.regs = ml_io_map(A21S_MMC_PA, A21S_MMC_MAP);
\tif (!su.regs) {
\t\tprintf("A21S EMMC: cannot map dwmmc0 at 0x%lx\\n", A21S_MMC_PA);
\t\treturn ENXIO;
\t}
\tif (kmem_alloc_contig(kernel_map, &su.dma, DMA_SIZE, PAGE_MASK, 0, 0,
\t    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
\t\tprintf("A21S EMMC: no contiguous bounce memory\\n");
\t\treturn ENOMEM;
\t}
\tppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)su.dma);
\tsu.dma_pa = pn ? ptoa_64(pn) : 0;
\tsu.secsize = DEV_BSIZE;

\tlck_mtx_lock(&herolte_ufs_lock);
\tint error = a21s_mmc_init_hw();
\tif (!error) {
\t\terror = a21s_load_extents();
\t}
\tlck_mtx_unlock(&herolte_ufs_lock);
\tif (error) {
\t\treturn error;
\t}
\tsu.first_block = 0;
\tsu.nblocks = a21s_sys_bytes / UFS_BLOCK;

\tsu.bmajor = bdevsw_add(-1, &herolte_ufs_bdevsw);
\tif (su.bmajor < 0) {
\t\treturn ENXIO;
\t}
\tsu.cmajor = cdevsw_add_with_bdev(-1, &herolte_ufs_cdevsw, su.bmajor);
\tif (su.cmajor < 0) {
\t\treturn ENXIO;
\t}
\tsu.bdev = makedev(su.bmajor, 0);
\tdevfs_make_node(su.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "ufsroot0");
\tdevfs_make_node(makedev(su.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rufsroot0");
\tsu.writable = (rw != 0);
\tsu.ready = true;
\tprintf("A21S EMMC: /dev/ufsroot0 ready: %llu blocks (%llu MiB), %s\\n",
\t    su.nblocks, (su.nblocks * UFS_BLOCK) >> 20, su.writable ? "read-write" : "read-only");
\therolte_log_start();
\treturn 0;
}"""
        ),
        (
            "void\nherolte_ufs_probe(void)\n{",
            "void\nherolte_ufs_probe(void)\n{\n\treturn;"
        ),
    ])

    # 10. bsd/dev/starlte_touch.c: Exynos 850 ACPM (0x11900000, SRAM +0x3000, INITDATA 0x7000) + A21SMBOX touch/button consumer
    a21s_touch_thread = r"""
/* ---- Galaxy A21s (SM-A217F): Core-0 A21SMBOX consumer + Exynos 850 CPU DVFS ---- */
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void starlte_return_to_recovery(void);
extern void sweet_hid_key(int key, int down);

#define A21S_MBOX_MAGIC  0x584f424d53313241ULL /* "A21SMBOX" */
#define A21S_MBOX_EVENTS 128

struct a21s_mbox_event {
	uint32_t type;      /* 1 down, 2 move, 3 up; 4 button down, 5 button up */
	uint32_t finger;    /* slot 0..9, or button: 1 lock, 2 vol+, 3 vol- */
	uint32_t x, y;      /* 0..719, 0..1599 panel pixels */
};
struct a21s_mbox_ring {
	uint64_t magic;
	uint32_t version, head;
	uint32_t chip, events_size;
	uint64_t irqs, frames, bad;
	struct a21s_mbox_event ev[A21S_MBOX_EVENTS];
};

static void
a21s_touch_thread(__unused void *arg, __unused wait_result_t wr)
{
	/* Raise both Cortex-A55 clusters via ACPM DVFS (default starcpu=2002000 kHz). */
	st_msleep(1500);
	if (acpm_ready()) {
		st_cpu_dvfs();
	}
	uint64_t pa = 0;
	if (!PE_parse_boot_argn("a21s_mbox", &pa, sizeof(pa)) || pa == 0) {
		printf("A21S INPUT: no a21s_mbox= address on cmdline; touch ring inactive\n");
		return;
	}
	vm_offset_t page_off = (vm_offset_t)(pa & (PAGE_SIZE - 1));
	vm_size_t map_sz = (page_off + sizeof(struct a21s_mbox_ring) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	vm_offset_t base = ml_io_map_wcomb((vm_offset_t)(pa & ~(PAGE_SIZE - 1)), map_sz);
	if (!base) {
		printf("A21S INPUT: ml_io_map_wcomb(%#llx) failed\n", pa);
		return;
	}
	volatile struct a21s_mbox_ring *mb = (volatile struct a21s_mbox_ring *)(base + page_off);
	if (mb->magic != A21S_MBOX_MAGIC || mb->events_size != sizeof(struct a21s_mbox_event)) {
		printf("A21S INPUT: bad magic %#llx at %#llx\n", mb->magic, pa);
		return;
	}
	printf("A21S INPUT: connected to Core-0 A21SMBOX at %#llx (chip=%u, irqs=%llu)\n",
	    pa, mb->chip, mb->irqs);

	uint32_t tail = 0, last_chip = 0;
	uint64_t chord_at = 0;
	uint32_t down_mask = 0;
	for (;;) {
		uint32_t head = mb->head;
		__asm__ volatile ("dmb ld" ::: "memory");
		if (head - tail > A21S_MBOX_EVENTS) {
			tail = head - A21S_MBOX_EVENTS;
		}
		while (tail != head) {
			struct a21s_mbox_event e = mb->ev[tail % A21S_MBOX_EVENTS];
			tail++;
			if (e.type >= 1 && e.type <= 3 && e.finger < ST_FINGERS) {
				int down = (e.type != 3);
				if (down) {
					down_mask |= (1u << e.finger);
				} else {
					down_mask &= ~(1u << e.finger);
				}
				sweet_hid_touch((int)e.finger, down, (int)e.x, (int)e.y);
			} else if (e.type == 4 || e.type == 5) {
				int down = (e.type == 4);
				if (e.finger == 1) {
					chord_at = down ? mach_absolute_time() : 0;
					sweet_hid_key(0, down); /* Power / Lock */
				} else if (e.finger == 2) {
					sweet_hid_key(1, down); /* Vol+ */
				} else if (e.finger == 3) {
					sweet_hid_key(2, down); /* Vol- */
				}
			}
		}
		if (chord_at != 0) {
			uint64_t ns = 0;
			absolutetime_to_nanoseconds(mach_absolute_time() - chord_at, &ns);
			if (ns >= 3000ULL * NSEC_PER_MSEC) {
				printf("A21S INPUT: Vol+ & Vol- held 3 s -> flushing log and rebooting to Android\n");
				starlte_return_to_recovery();
				chord_at = 0;
			}
		}
		if (mb->chip != last_chip) {
			last_chip = mb->chip;
			printf("A21S INPUT: HX83112A touch controller %s (irqs=%llu frames=%llu)\n",
			    last_chip ? "online" : "offline", mb->irqs, mb->frames);
		}
		st_msleep(4);
	}
}
"""
    patch_file("bsd/dev/starlte_touch.c", [
        (
            """#define ACPM_MBOX_PA            0x14100000u
#define ACPM_SRAM_PA            0x02039000u
#define ACPM_SRAM_SIZE          0x30000u
#define ACPM_INITDATA           0x7f00u
#define ACPM_PMIC_CH            2u""",
            """#define ACPM_MBOX_PA            0x11900000u
#define ACPM_SRAM_PA            0x02039000u
#define ACPM_SRAM_FIRST         0x3000u
#define ACPM_SRAM_SIZE          0x30000u
#define ACPM_INITDATA           0x7000u
#define ACPM_PMIC_CH            2u"""
        ),
        (
            """\tacpm_mbox = ml_io_map(ACPM_MBOX_PA, 0x1000);
\tacpm_sram = ml_io_map(ACPM_SRAM_PA, ACPM_SRAM_SIZE);""",
            """\tacpm_mbox = ml_io_map(ACPM_MBOX_PA, 0x1000);
\tvm_offset_t sram_hi = ml_io_map(ACPM_SRAM_PA + ACPM_SRAM_FIRST, ACPM_SRAM_SIZE - ACPM_SRAM_FIRST);
\tacpm_sram = sram_hi ? (sram_hi - ACPM_SRAM_FIRST) : 0;
\tif (!acpm_mbox || !sram_hi) {
\t\treturn false;
\t}"""
        ),
        (
            """\tuint32_t little = 1690000, big = 1794000;
\tif (PE_parse_boot_argn("starcpu", arg, sizeof(arg))) {
\t\tif (arg[0] == '0' && arg[1] == 0) {
\t\t\tprintf("STARLTE DVFS: starcpu=0: CPU clocks left as S-Boot set them\\n");
\t\t\treturn;
\t\t}
\t\tchar *end = arg;
\t\tlittle = (uint32_t)strtoul(arg, &end, 10);
\t\tbig = *end == ',' ? (uint32_t)strtoul(end + 1, NULL, 10) : big;
\t}
\tif (little > 1794000 || big > 1794000) {
\t\tprintf("STARLTE DVFS: %u/%u kHz above 1794000: not without thermal control\\n", little, big);
\t\treturn;
\t}
\t/* channel index 0 as Linux, then the descriptor whose id is 5 as the DT says */
\tuint32_t descs[2] = { acpm_chans, acpm_find(5) };
\tfor (unsigned d = 0; d < 2; d++) {
\t\tuint32_t c = descs[d];
\t\tif (c == 0) {
\t\t\tcontinue;
\t\t}
\t\tuint32_t id = acpm_rd(c);
\t\tuint32_t set0[4] = { 2, little, 0, 0 }, set1[4] = { 3, big, 0, 0 };""",
            """\tuint32_t little = 2002000, big = 2002000;
\tif (PE_parse_boot_argn("starcpu", arg, sizeof(arg))) {
\t\tif (arg[0] == '0' && arg[1] == 0) {
\t\t\tprintf("A21S DVFS: starcpu=0: CPU clocks left as S-Boot set them\\n");
\t\t\treturn;
\t\t}
\t\tchar *end = arg;
\t\tlittle = (uint32_t)strtoul(arg, &end, 10);
\t\tbig = *end == ',' ? (uint32_t)strtoul(end + 1, NULL, 10) : little;
\t}
\tif (little > 2210000 || big > 2210000) {
\t\tlittle = big = 2002000;
\t}
\tuint64_t ns_now = 0;
\tabsolutetime_to_nanoseconds(mach_absolute_time(), &ns_now);
\tuint32_t stamp = (uint32_t)(ns_now / 1000000ULL);
\tuint32_t descs[2] = { acpm_find(0), acpm_chans };
\tfor (unsigned d = 0; d < 2; d++) {
\t\tuint32_t c = descs[d];
\t\tif (c == 0) {
\t\t\tcontinue;
\t\t}
\t\tuint32_t id = acpm_rd(c);
\t\tuint32_t set0[4] = { 2, little, 0, stamp }, set1[4] = { 3, big, 0, stamp };"""
        ),
        (
            """\tuint32_t cmd[4] = { domain, khz, 0, 0 };
\tbool ok = acpm_send_on(acpm_chans, acpm_rd(acpm_chans), cmd);""",
            """\tuint64_t ns_now = 0;
\tabsolutetime_to_nanoseconds(mach_absolute_time(), &ns_now);
\tuint32_t ch0 = acpm_find(0);
\tif (!ch0) ch0 = acpm_chans;
\tuint32_t cmd[4] = { domain, khz, 0, (uint32_t)(ns_now / 1000000ULL) };
\tbool ok = acpm_send_on(ch0, acpm_rd(ch0), cmd);"""
        ),
        (
            """\tst_con_job.x0 = 36 + v[0] * 1368 / 1000;
\tst_con_job.y0 = v[1] * 2959 / 1000;
\tst_con_job.x1 = 36 + v[2] * 1368 / 1000;
\tst_con_job.y1 = v[3] * 2959 / 1000;""",
            """\tst_con_job.x0 = v[0] * 719 / 1000;
\tst_con_job.y0 = v[1] * 1599 / 1000;
\tst_con_job.x1 = v[2] * 719 / 1000;
\tst_con_job.y1 = v[3] * 1599 / 1000;"""
        ),
        (
            """\tst_alive = ml_io_map(ST_ALIVE_PA, 0x1000);
\tprintf("STARLTE TOUCH: INT pin %s at probe\\n", st_int_low() ? "low" : "high");
\tuint32_t pri = on, ms = 0;      /* startouch=N, N > 1: the thread's priority (short: the command line is full) */
\tif (PE_parse_boot_argn("startouch_ms", &ms, sizeof(ms)) && ms >= 2 && ms <= 20) {
\t\tst_poll_ms = ms;
\t}
\tkern_return_t kr;
\tif (pri >= 2 && pri <= 95) {
\t\tkr = kernel_thread_start_priority((thread_continue_t)st_thread, NULL, (integer_t)pri, &thread);
\t\tprintf("STARLTE TOUCH: thread at priority %u, every %u ms\\n", pri, st_poll_ms);
\t} else {
\t\tkr = kernel_thread_start(st_thread, NULL, &thread);
\t}""",
            """\tuint32_t pri = on;
\tkern_return_t kr;
\tif (pri >= 2 && pri <= 95) {
\t\tkr = kernel_thread_start_priority((thread_continue_t)a21s_touch_thread, NULL, (integer_t)pri, &thread);
\t\tprintf("A21S INPUT: thread started at priority %u\\n", pri);
\t} else {
\t\tkr = kernel_thread_start(a21s_touch_thread, NULL, &thread);
\t}"""
        ),
        (
            "void\nstarlte_touch_probe(void)",
            a21s_touch_thread + "\nvoid\nstarlte_touch_probe(void)"
        ),
    ])

    # 11. bsd/dev/starlte_gpu.c: Exynos 850 Mali-G52 MP1 (0x7212 @ 0x11500000), Bifrost v7 AArch64 4K MMU, safe bank-2 PA
    a21s_gpu_helpers = r"""
/* ---- Exynos 850 Mali-G52 MP1 (0x11500000, GPU_ID 0x72120000) power & clock bring-up ---- */
#define A21_TOP_PA              0x120e0000u
#define A21_TOP_DIV_SWITCH      0x1844u
#define A21_TOP_GAT_SWITCH      0x2040u
#define A21_G3D_SWITCH_USER     0x0600u
#define A21_G3D_PLL_CON0        0x0100u
#define A21_G3D_PLL_CON3        0x010cu
#define A21_G3D_MUX_BUSD        0x1000u
#define A21_G3D_DIV_BUSP        0x1804u
#define A21_DTZPC_G3D           0x11410204u
static vm_offset_t a21_cmu_top, a21_cmu_g3d;

static uint64_t
a21_smc(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fid;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile ("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static bool
a21_g52_domain_and_clocks(vm_offset_t pmu)
{
	static const uint32_t gates[] = { 0x2000, 0x2004, 0x200c, 0x2010, 0x2024, 0x2028, 0x202c };
	if (!a21_cmu_top) {
		a21_cmu_top = ml_io_map(A21_TOP_PA, 0x3000);
	}
	if (!a21_cmu_g3d) {
		a21_cmu_g3d = ml_io_map(SG_CMU_PA, 0x3000);
	}
	if (!sg_gpu) {
		sg_gpu = ml_io_map(SG_GPU_PA, 0x5000);
	}
	if (!a21_cmu_top || !a21_cmu_g3d || !sg_gpu) {
		printf("A21S GPU: MMIO map failed\n");
		return false;
	}
	/* Enable G3D switch clock in CMU_TOP and set divide-by-2 (~400 MHz). */
	*(volatile uint32_t *)(a21_cmu_top + A21_TOP_GAT_SWITCH) |= (1u << 20) | (1u << 21);
	*(volatile uint32_t *)(a21_cmu_top + A21_TOP_DIV_SWITCH) =
	    (*(volatile uint32_t *)(a21_cmu_top + A21_TOP_DIV_SWITCH) & ~7u) | 1u;
	for (unsigned i = 0; i < 1000 && (*(volatile uint32_t *)(a21_cmu_top + A21_TOP_DIV_SWITCH) & (1u << 16)); i++) {
		delay(1);
	}
	if (!(sg_rd(pmu, SG_PMU_G3D_STAT) & 1u)) {
		*(volatile uint32_t *)(pmu + SG_PMU_G3D_CONF) |= 1u;
		__asm__ volatile ("dsb sy" ::: "memory");
		for (unsigned us = 0; us < 50000 && !(sg_rd(pmu, SG_PMU_G3D_STAT) & 1u); us += 10) {
			delay(10);
		}
		if (!(sg_rd(pmu, SG_PMU_G3D_STAT) & 1u)) {
			printf("A21S GPU: pd-g3d did not power on (stat=%#x)\n", sg_rd(pmu, SG_PMU_G3D_STAT));
			return false;
		}
		uint64_t rc = a21_smc(0x82000410ULL, 1ULL, A21_DTZPC_G3D, 0ULL);
		printf("A21S GPU: pd-g3d powered on, DTZPC SMC returned %#llx\n", rc);
		if ((uint32_t)rc != 0) {
			return false;
		}
	}
	*(volatile uint32_t *)(a21_cmu_g3d + A21_G3D_SWITCH_USER) |= (1u << 4);
	for (unsigned i = 0; i < 1000 && (*(volatile uint32_t *)(a21_cmu_g3d + A21_G3D_SWITCH_USER) & (1u << 7)); i++) {
		delay(1);
	}
	*(volatile uint32_t *)(a21_cmu_g3d + A21_G3D_MUX_BUSD) = 1u;
	for (unsigned i = 0; i < 1000 && (*(volatile uint32_t *)(a21_cmu_g3d + A21_G3D_MUX_BUSD) & (1u << 16)); i++) {
		delay(1);
	}
	*(volatile uint32_t *)(a21_cmu_g3d + A21_G3D_DIV_BUSP) = 3u;
	for (unsigned i = 0; i < 1000 && (*(volatile uint32_t *)(a21_cmu_g3d + A21_G3D_DIV_BUSP) & (1u << 16)); i++) {
		delay(1);
	}
	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
		*(volatile uint32_t *)(a21_cmu_g3d + gates[i]) |= (1u << 20) | (1u << 21);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	return true;
}
"""
    patch_file("bsd/dev/starlte_gpu.c", [
        (
            """#define SG_PMU_G3D_PA   0x14064000u     /* page with CONFIGURATION +0x40, STATUS +0x44 */
#define SG_PMU_G3D_CONF 0x40u
#define SG_PMU_G3D_STAT 0x44u
#define SG_CMU_PA       0x17400000u
#define SG_GPU_PA       0x17500000u""",
            """#define SG_PMU_G3D_PA   0x11861000u     /* Exynos 850 PMU pd-g3d: +0xd00 CONF, +0xd04 STAT */
#define SG_PMU_G3D_CONF 0xd00u
#define SG_PMU_G3D_STAT 0xd04u
#define SG_CMU_PA       0x11400000u
#define SG_GPU_PA       0x11500000u"""
        ),
        (
            """static void
sg_probe(vm_offset_t pmu, const char *when)
{
\tuint32_t conf = sg_rd(pmu, SG_PMU_G3D_CONF), stat = sg_rd(pmu, SG_PMU_G3D_STAT);
\tprintf("STARLTE GPU: %s: PMU G3D conf=%#x status=%#x\\n", when, conf, stat);
\tif ((stat & 0xf) != 0xf) {
\t\tprintf("STARLTE GPU: %s: G3D domain off; CMU and Mali not read\\n", when);
\t\treturn;
\t}
\tstatic vm_offset_t cmu;
\tif (!cmu) {
\t\tcmu = ml_io_map(SG_CMU_PA, 0x2000);
\t}
\tif (!sg_gpu) {
\t\tsg_gpu = ml_io_map(SG_GPU_PA, 0x5000);
\t}
\tvm_offset_t gpu = sg_gpu;
\tif (!cmu || !gpu) {
\t\tprintf("STARLTE GPU: MMIO mapping failed\\n");
\t\treturn;
\t}
\tuint32_t con0 = sg_rd(cmu, 0x140);
\tuint64_t m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;
\tuint64_t khz = (con0 & 0x10) && p ? 26000ull * m / (p << s) : 26000;
\tprintf("STARLTE GPU: %s: PLL_G3D CON0=%#x CON2=%#x (%llu kHz%s), USER mux=%#x BUSD mux=%#x div=%#x busp=%#x\\n",
\t    when, con0, sg_rd(cmu, 0x148), khz, (con0 & (1u << 29)) ? ", locked" : ", not stable",
\t    sg_rd(cmu, 0x100), sg_rd(cmu, 0x1000), sg_rd(cmu, 0x1800), sg_rd(cmu, 0x1804));
\tif ((sg_rd(pmu, SG_PMU_G3D_STAT) & 0xf) != 0xf) {
\t\tprintf("STARLTE GPU: %s: domain went off; Mali not read\\n", when);
\t\treturn;
\t}""",
            a21s_gpu_helpers + """
static void
sg_probe(vm_offset_t pmu, const char *when)
{
\tuint32_t conf = sg_rd(pmu, SG_PMU_G3D_CONF), stat = sg_rd(pmu, SG_PMU_G3D_STAT);
\tprintf("A21S GPU: %s: PMU G3D conf=%#x status=%#x\\n", when, conf, stat);
\tif (!a21_g52_domain_and_clocks(pmu)) {
\t\treturn;
\t}
\tvm_offset_t gpu = sg_gpu;"""
        ),
        (
            """#define SG_MEM_PA       0x8f0000000ull
#define SG_MEM_BYTES    0x60000u
#define SG_VA           0x02000000ull   /* the data pages, from +SG_DATA_OFF */
#define SG_DATA_OFF     0x10000u
#define SG_DATA_PAGES   48u
#define SG_PANEL_L3_OFF 0x4000u
#define SG_FB_PA        0xcc000000ull
#define SG_FB_W         1440u
#define SG_FB_H         2960u
#define SG_FB_STRIDE    (SG_FB_W * 4u)
#define SG_PANEL_VA     0x04000000ull
#define SG_HEAP_PA      0x8f1000000ull""",
            """#define SG_MEM_PA       0x8e0000000ull
#define SG_MEM_BYTES    0x60000u
#define SG_VA           0x02000000ull   /* the data pages, from +SG_DATA_OFF */
#define SG_DATA_OFF     0x10000u
#define SG_DATA_PAGES   48u
#define SG_PANEL_L3_OFF 0x4000u
#define SG_FB_PA        0xfa000000ull
#define SG_FB_W         720u
#define SG_FB_H         1600u
#define SG_FB_STRIDE    (SG_FB_W * 4u)
#define SG_PANEL_VA     0x04000000ull
#define SG_HEAP_PA      0x8e1000000ull"""
        ),
        (
            "l3[i] = (SG_MEM_PA + SG_DATA_OFF + i * 0x1000) | 0x2c1;",
            "l3[i] = (SG_MEM_PA + SG_DATA_OFF + i * 0x1000) | 0x743ull;"
        ),
        (
            "pl3[i] = (SG_FB_PA + (uint64_t)(t * 512 + i) * 0x1000) | 0x2c1;",
            "pl3[i] = (SG_FB_PA + (uint64_t)(t * 512 + i) * 0x1000) | 0x743ull;"
        ),
        (
            "hl3[i] = (SG_HEAP_PA + (uint64_t)(t * 512 + i) * 0x1000) | 0x2c1;",
            "hl3[i] = (SG_HEAP_PA + (uint64_t)(t * 512 + i) * 0x1000) | 0x743ull;"
        ),
        (
            """static bool
sg_mmu_setup(void)
{
\tuint64_t transtab = SG_MEM_PA | 0x7;            /* ADRMODE_TABLE | READ_INNER */
\tprintf("STARLTE GPU: AS0 before: TRANSCFG=%#x STATUS=%#x\\n",
\t    sg_rd(sg_gpu, AS0 + 0x30), sg_rd(sg_gpu, AS0 + 0x28));
\tsg_wr(sg_gpu, AS0 + 0x00, (uint32_t)transtab);
\tsg_wr(sg_gpu, AS0 + 0x04, (uint32_t)(transtab >> 32));
\tsg_wr(sg_gpu, AS0 + 0x08, 0x888d88);            /* MEMATTR */
\tsg_wr(sg_gpu, AS0 + 0x0c, 0);
\tsg_wr(sg_gpu, AS0 + 0x30, 0);                   /* TRANSCFG: legacy (LPAE) */
\tsg_wr(sg_gpu, AS0 + 0x34, 0);""",
            """static bool
sg_mmu_setup(void)
{
\t/* Mali-G52 (Bifrost v7, MMU_FEATURES 0x2830): AArch64 4K 4-level page tables (mode 6). */
\tuint64_t transtab = SG_MEM_PA;
\tprintf("A21S GPU: AS0 before: TRANSCFG=%#x STATUS=%#x\\n",
\t    sg_rd(sg_gpu, AS0 + 0x30), sg_rd(sg_gpu, AS0 + 0x28));
\tsg_wr(sg_gpu, AS0 + 0x00, (uint32_t)transtab);
\tsg_wr(sg_gpu, AS0 + 0x04, (uint32_t)(transtab >> 32));
\tsg_wr(sg_gpu, AS0 + 0x08, 0x88888888u);         /* MEMATTR_LO */
\tsg_wr(sg_gpu, AS0 + 0x0c, 0x88888888u);         /* MEMATTR_HI */
\tsg_wr(sg_gpu, AS0 + 0x30, 0x10001c6u);          /* TRANSCFG_LO: AArch64 4K, 48-bit 4-level, uncached walk */
\tsg_wr(sg_gpu, AS0 + 0x34, 0);"""
        ),
        (
            "#define SGU_PT_PA       0x8f2000000ull",
            "#define SGU_PT_PA       0x8e2000000ull"
        ),
        (
            """static bool
sgu_as_enable(unsigned as, uint64_t table_pa)
{
\tuint64_t transtab = table_pa | 0x7;
\tif (!sgu_as_wait(as)) {
\t\treturn false;
\t}
\tsg_wr(sg_gpu, ASN(as) + 0x00, (uint32_t)transtab);
\tsg_wr(sg_gpu, ASN(as) + 0x04, (uint32_t)(transtab >> 32));
\tsg_wr(sg_gpu, ASN(as) + 0x08, 0x888d88);
\tsg_wr(sg_gpu, ASN(as) + 0x0c, 0);
\tsg_wr(sg_gpu, ASN(as) + 0x30, 0);
\tsg_wr(sg_gpu, ASN(as) + 0x34, 0);
\tsg_wr(sg_gpu, ASN(as) + 0x18, 1);               /* UPDATE */
\treturn sgu_as_wait(as);
}""",
            """static bool
sgu_as_enable(unsigned as, uint64_t table_pa)
{
\tuint64_t transtab = table_pa;
\tif (!sgu_as_wait(as)) {
\t\treturn false;
\t}
\tsg_wr(sg_gpu, ASN(as) + 0x00, (uint32_t)transtab);
\tsg_wr(sg_gpu, ASN(as) + 0x04, (uint32_t)(transtab >> 32));
\tsg_wr(sg_gpu, ASN(as) + 0x08, 0x88888888u);
\tsg_wr(sg_gpu, ASN(as) + 0x0c, 0x88888888u);
\tsg_wr(sg_gpu, ASN(as) + 0x30, 0x10001c6u);
\tsg_wr(sg_gpu, ASN(as) + 0x34, 0);
\tsg_wr(sg_gpu, ASN(as) + 0x18, 1);               /* UPDATE */
\treturn sgu_as_wait(as);
}"""
        ),
        (
            "l3[(v >> 12) & 0x1ff] = (pages[i] & ~0xfffull) | (gpu_writes ? 0x2c1 : 0x241);",
            "l3[(v >> 12) & 0x1ff] = (pages[i] & ~0xfffull) | (gpu_writes ? 0x743ull : 0x7c3ull);"
        ),
        (
            """#define SGU_INTID_JOB           314             /* SPI 282 */
#define SGU_INTID_MMU           313             /* SPI 281 */
#define SGU_INTID_GPU           315             /* SPI 283 */""",
            """#define SGU_INTID_JOB           160             /* Exynos 850 SPI 128 */
#define SGU_INTID_MMU           161             /* Exynos 850 SPI 129 */
#define SGU_INTID_GPU           162             /* Exynos 850 SPI 130 */"""
        ),
        (
            """static void
sg_jobs(vm_offset_t pmu)
{
\tif ((sg_rd(pmu, SG_PMU_G3D_STAT) & 0xf) != 0xf || !sg_gpu ||
\t    (sg_rd(sg_gpu, GPU_ID) >> 16) != 0x6221) {
\t\tprintf("STARLTE GPU: stage 2: domain off or not a G72; no jobs\\n");
\t\treturn;
\t}
\tvm_offset_t cmu = ml_io_map(SG_CMU_PA, 0x1000);
\tif (!cmu || !(sg_rd(cmu, 0x140) & (1u << 29))) {
\t\tprintf("STARLTE GPU: stage 2: PLL_G3D not locked; no jobs\\n");
\t\treturn;
\t}
\tuint8_t was = 0, now = 0;
\tif (!starlte_pmic_read(1, SG_BUCK6, &was)) {
\t\tprintf("STARLTE GPU: stage 2: BUCK6 unreadable; no jobs\\n");
\t\treturn;
\t}
\t/* 0x20 = 500 mV .. 0x70 = 1000 mV; anything else is not vdd_g3d as known. */
\tif (was < 0x20 || was > 0x70) {
\t\tprintf("STARLTE GPU: stage 2: BUCK6 vsel %#x unexpected; no jobs\\n", was);
\t\treturn;
\t}
\tbool volt = starlte_pmic_write(1, SG_BUCK6, SG_BUCK6_950MV);
\tdelay(1000);                                    /* ramp: well under 1 ms */
\t(void)starlte_pmic_read(1, SG_BUCK6, &now);
\tprintf("STARLTE GPU: vdd_g3d BUCK6 %#x (%u uV) -> %#x (%u uV) %s\\n", was,
\t    300000u + was * 6250u, now, 300000u + now * 6250u,
\t    volt && now == SG_BUCK6_950MV ? "ok" : "FAILED");
\tif (!volt || now != SG_BUCK6_950MV) {
\t\tgoto restore;
\t}
\tsg_mem = ml_io_map((vm_offset_t)SG_MEM_PA, SG_MEM_BYTES);
\tif (!sg_mem) {
\t\tprintf("STARLTE GPU: GPU memory mapping failed\\n");
\t\tgoto restore;
\t}
\tuint32_t shaders = sg_rd(sg_gpu, SHADER_PRESENT_LO);
\tif (sg_gpu_up(shaders)) {
\t\tbool a = sg_write_value(0x000, 0x100, 3, 0, 0, shaders, "WRITE_VALUE zero");
\t\tbool b = sg_write_value(0x200, 0x300, 7, 0x4554494252415453ull,
\t\t    0x4554494252415453ull, shaders, "WRITE_VALUE STARBITE");
\t\tprintf("STARLTE GPU: stage 2 %s\\n", a && b ? "PASSED" : "did not pass");
\t\tif (a && b) {
\t\t\t/* Stages 3 and 4 draw on the panel and read it back: hold
\t\t\t * SweetFramebuffer's CPU copies meanwhile (with 8 cores iOS
\t\t\t * is already presenting by now; one landed in the triangle
\t\t\t * check), and let a copy in flight finish first. */
\t\t\tuint64_t hold;
\t\t\tclock_interval_to_deadline(3, NSEC_PER_SEC, &hold);
\t\t\tsweet_fb_hold_until = hold;
\t\t\tsg_msleep(100);
\t\t\tsg_stage3(shaders);
\t\t\t/* Fresh reset, tables and AS0 for the draw, then again for
\t\t\t * the present (stage 5), which keeps the GPU powered. */
\t\t\tbool drawn = sg_gpu_up(shaders) && sg_stage4(shaders);
\t\t\tsweet_fb_hold_until = 0;
\t\t\tif (drawn && sg_gpu_up(shaders)) {
\t\t\t\tsg_clock_up();
\t\t\t\tsgp_shaders = shaders;
\t\t\t\t__atomic_thread_fence(__ATOMIC_RELEASE);
\t\t\t\tsgp_ready = true;
\t\t\t\tsgu_ready = sgu_setup();
\t\t\t\tprintf("STARLTE GPU: stage 6: GPU for apps %s (AS1, tables at %#llx)\\n",
\t\t\t\t    sgu_ready ? "ready" : "NOT ready", SGU_PT_PA);
\t\t\t\tif (sgu_ready) {
\t\t\t\t\tstarbite_mali_publish();
\t\t\t\t}
\t\t\t\tprintf("STARLTE GPU: stage 5: GPU present ready (%u x %u canvas), cores on, "
\t\t\t\t    "vdd_g3d stays at 950 mV\\n", SG_PRES_CANVAS_W, SG_PRES_CANVAS_H);
\t\t\t\treturn;
\t\t\t}
\t\t}
\t}
\tsg_cores_off();
restore:
\tif (starlte_pmic_write(1, SG_BUCK6, was) && starlte_pmic_read(1, SG_BUCK6, &now)) {
\t\tprintf("STARLTE GPU: vdd_g3d BUCK6 back to %#x (%u uV)\\n", now, 300000u + now * 6250u);
\t}
}""",
            """static void
sg_jobs(vm_offset_t pmu)
{
\tif (!a21_g52_domain_and_clocks(pmu)) {
\t\treturn;
\t}
\tuint32_t prod = sg_rd(sg_gpu, GPU_ID) >> 16;
\tif (prod != 0x7212u && (prod & 0xf000u) != 0x7000u) {
\t\tprintf("A21S GPU: unexpected GPU_ID product %#x; no jobs\\n", prod);
\t\treturn;
\t}
\tsg_mem = ml_io_map((vm_offset_t)SG_MEM_PA, SG_MEM_BYTES);
\tif (!sg_mem) {
\t\tprintf("A21S GPU: GPU memory mapping failed\\n");
\t\treturn;
\t}
\tuint32_t shaders = sg_rd(sg_gpu, SHADER_PRESENT_LO);
\tif (sg_gpu_up(shaders)) {
\t\tbool a = sg_write_value(0x000, 0x100, 3, 0, 0, shaders, "WRITE_VALUE zero");
\t\tbool b = sg_write_value(0x200, 0x300, 7, 0x4554494252415453ull,
\t\t    0x4554494252415453ull, shaders, "WRITE_VALUE STARBITE");
\t\tprintf("A21S GPU: Mali-G52 MP1 stage 2 %s\\n", a && b ? "PASSED" : "did not pass");
\t\tif (a && b && sg_gpu_up(shaders)) {
\t\t\tsg_clock_up();
\t\t\tsgp_shaders = shaders;
\t\t\t__atomic_thread_fence(__ATOMIC_RELEASE);
\t\t\tsgp_ready = false; /* 1:1 720x1600 scanout uses fast CPU present; apps use Mali-G52 AS1 */
\t\t\tsgu_ready = sgu_setup();
\t\t\tprintf("A21S GPU: stage 6: Mali-G52 MP1 for apps %s (AS1 AArch64-4K, tables at %#llx)\\n",
\t\t\t    sgu_ready ? "READY" : "NOT ready", SGU_PT_PA);
\t\t\tif (sgu_ready) {
\t\t\t\tstarbite_mali_publish();
\t\t\t}
\t\t\treturn;
\t\t}
\t}
\tsg_cores_off();
}"""
        ),
        (
            """static uint32_t
sg_clock_khz(void)
{
\tstatic vm_offset_t cmu;
\tif (!cmu) {
\t\tcmu = ml_io_map(SG_CMU_PA, 0x2000);
\t}
\tuint32_t con0 = cmu ? sg_rd(cmu, 0x140) : 0;
\tuint64_t m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;
\tif (!(con0 & (1u << 29)) || !p) {
\t\treturn 0;                       /* not locked */
\t}
\treturn (con0 & 0x10) ? (uint32_t)(26000ull * m / (p << s)) : 26000;
}

static void
sg_clock_up(void)
{
\tstatic const uint32_t levels[] = { 572000, 546000, 455000, 338000, 299000, 260000 };
\tuint32_t want = 572000;
\tPE_parse_boot_argn("stargpu", &want, sizeof(want));
\tbool listed = false;
\tfor (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
\t\tlisted |= levels[i] == want;
\t}
\tif (want == 0 || !listed) {
\t\tprintf("STARLTE GPU: clock left at %u kHz (stargpu=%u)\\n", sg_clock_khz(), want);
\t\treturn;
\t}
\tuint32_t before = sg_clock_khz();
\tbool sent = starlte_acpm_dvfs(4, want);
\tdelay(1000);
\tuint8_t volt = 0;
\tbool v = starlte_pmic_write(1, SG_BUCK6, SG_BUCK6_950MV) && starlte_pmic_read(1, SG_BUCK6, &volt);
\tuint32_t after = sg_clock_khz();
\tprintf("STARLTE GPU: clock %u -> %u kHz (asked %u, %s), vdd_g3d %s %u uV\\n", before, after, want,
\t    sent ? "sent" : "no reply", v ? "at" : "NOT SET,", 300000u + volt * 6250u);
\tif (sent && after == 0) {
\t\t(void)starlte_acpm_dvfs(4, 260000);
\t\tprintf("STARLTE GPU: PLL not locked at %u kHz: back to 260000 (%u kHz)\\n", want, sg_clock_khz());
\t\treturn;
\t}
\tif (sent) {
\t\tsg_boot_khz = want;             /* what the heat may take down and gives back (starlte_gpu_thermal) */
\t}
}""",
            """static uint32_t
sg_clock_khz(void)
{
\tif (!a21_cmu_g3d) {
\t\treturn 400000;
\t}
\tuint32_t sel = sg_rd(a21_cmu_g3d, A21_G3D_PLL_CON0);
\tuint32_t con3 = sg_rd(a21_cmu_g3d, A21_G3D_PLL_CON3);
\tif (!(sel & 0x10u) || (sg_rd(a21_cmu_g3d, A21_G3D_MUX_BUSD) & 1u)) {
\t\treturn 400000;
\t}
\tuint64_t m = (con3 >> 16) & 0x3ffu, p = (con3 >> 8) & 0x3fu, s = con3 & 7u;
\treturn p ? (uint32_t)(26000ull * m / (p << s)) : 400000;
}

static void
sg_clock_up(void)
{
\tuint32_t want = 845000;
\tPE_parse_boot_argn("mali_a21s_khz", &want, sizeof(want));
\tif (want == 0 || want > 1001000 || !a21_cmu_g3d) {
\t\tprintf("A21S GPU: keeping ~400 MHz switch clock\\n");
\t\treturn;
\t}
\tbool sent = starlte_acpm_dvfs(4, want);
\tdelay(2000);
\tuint32_t con3 = sg_rd(a21_cmu_g3d, A21_G3D_PLL_CON3);
\tuint64_t m = (con3 >> 16) & 0x3ffu, p = (con3 >> 8) & 0x3fu, s = con3 & 7u;
\tuint32_t khz = p ? (uint32_t)(26000ull * m / (p << s)) : 0;
\tif (sent && ((con3 >> 31) & 1u) && ((con3 >> 29) & 1u) && khz >= 200000) {
\t\t*(volatile uint32_t *)(a21_cmu_g3d + A21_G3D_PLL_CON0) |= 0x10u;
\t\tfor (unsigned i = 0; i < 1000 && (*(volatile uint32_t *)(a21_cmu_g3d + A21_G3D_PLL_CON0) & 0x80u); i++) delay(1);
\t\t*(volatile uint32_t *)(a21_cmu_g3d + A21_G3D_MUX_BUSD) &= ~1u;
\t\tfor (unsigned i = 0; i < 1000 && (*(volatile uint32_t *)(a21_cmu_g3d + A21_G3D_MUX_BUSD) & (1u << 16)); i++) delay(1);
\t\tsg_boot_khz = khz;
\t\tprintf("A21S GPU: switched to PLL_G3D at %u kHz (asked %u kHz)\\n", khz, want);
\t} else {
\t\tprintf("A21S GPU: PLL_G3D con3=%#x (%u kHz); staying on 400 MHz switch clock\\n", con3, khz);
\t}
}"""
        ),
        (
            """\t/* Once early, once after the UI is up (in case something changed it). */
\tsg_msleep(5000);
\tsg_probe(pmu, "5s");
\tif (mode >= 2) {
\t\t/* After the touch thread's ACPM traffic (2 s) and before the UI. */
\t\tsg_msleep(3000);
\t\tsg_jobs(pmu);
\t}""",
            """\t/* Bring up Mali-G52 promptly (2.5 s) so backboardd sees StarbiteMali immediately. */
\tsg_msleep(2000);
\tsg_probe(pmu, "2s");
\tif (mode >= 2) {
\t\tsg_msleep(500);
\t\tsg_jobs(pmu);
\t}"""
        ),
    ])

    print("ALL A21S XNU PATCHES APPLIED CLEANLY.")


if __name__ == "__main__":
    main()
