/*
 * Standalone Exynos 850 (S5E3830, SM-A217F) WLBT Memory Interface (struct scsc_mif_abs)
 * for Scion's wifi/lx layer.
 *
 * Replaces platform_mif_3830.c + platform_mif_module.c without Linux syscon, regmap,
 * devicetree, or IOMMU dependencies:
 *   - Direct MMIO to the 7 Exynos 850 WLBT blocks (MBOX_AP2WLBT, PMU_ALIVE, I3C_APM_PMIC,
 *     DBUS_BAAW, PBUS_BAAW, WLBT_REMAP, BOOT_CFG) + CHIPID (0x10000000) + GICD (0x12a01000)
 *   - 12 MiB WLBT shared DRAM (0xe9400000 when wifi_dma_at=0xe9400000, or contiguous
 *     VM_WIMG_WCOMB pool from lx_base.c) mapped into WLBT 0x80000000..0x80c00000 via DBUS_BAAW
 *   - SMC_CMD_CONN_IF (0x82000710) TZPC non-secure setup + 57-word Maxwell142 ka_patch[]
 *   - Synchronous CFG_REQ (SPI 230 / INTID 262) handling via GICD_ISPENDR level sense
 *   - 1 ms MBOX polling thread (scsc_wlan=4 or scsc_poll=1) AND GIC SPI 54 (INTID 86) line
 *     interrupt handoff via lx_os_irq_line_attach() (scsc_wlan>=5)
 */
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <scsc/scsc_logring.h>
#include "scsc_mif_abs.h"
#include "mif_reg_S5E3830.h"

#include <lx_os.h>
#include "lx.h"
#include "lx_scsc.h"

/* Exynos 850 (SM-A217F) physical memory map from fdt.dts */
#define S3830_CHIPID_PA		0x10000000ULL
#define S3830_CHIPID_SZ		0x00001000UL
#define S3830_CHIPID_PRO_ID	0x0000U
#define S3830_CHIPID_REV	0x0010U

#define S3830_PMU_ALIVE_PA	0x11860000ULL
#define S3830_PMU_ALIVE_SZ	0x00010000UL

#define S3830_MBOX_WLBT_PA	0x119c0000ULL
#define S3830_MBOX_WLBT_SZ	0x00001000UL

#define S3830_I3C_APM_PMIC_PA	0x11a50000ULL
#define S3830_I3C_APM_PMIC_SZ	0x00002000UL

#define S3830_GICD_PA		0x12a01000ULL
#define S3830_GICD_SZ		0x00001000UL

#define S3830_DBUS_BAAW_PA	0x14c20000ULL
#define S3830_DBUS_BAAW_SZ	0x00001000UL

#define S3830_PBUS_BAAW_PA	0x14c30000ULL
#define S3830_PBUS_BAAW_SZ	0x00001000UL

#define S3830_WLBT_REMAP_PA	0x14c50000ULL
#define S3830_WLBT_REMAP_SZ	0x00001000UL

#define S3830_BOOT_CFG_PA	0x14c60000ULL
#define S3830_BOOT_CFG_SZ	0x00002000UL

/* WLBT shared DRAM: 12 MiB (0x00c00000), default reserved gap at 0xe9400000 */
#define S3830_WLBT_DRAM_PA	0xe9400000ULL
#define S3830_WLBT_DRAM_SZ	0x00c00000UL

/* GICv2 SPIs (INTID = 32 + SPI) */
#define S3830_SPI_ALIVE		38U
#define S3830_SPI_MBOX		54U
#define S3830_SPI_CFG_REQ	230U
#define S3830_SPI_WDOG		231U

#define S3830_INTID_MBOX	(32U + S3830_SPI_MBOX)		/* 86 */
#define S3830_INTID_CFG_REQ	(32U + S3830_SPI_CFG_REQ)	/* 262 */
#define S3830_INTID_WDOG	(32U + S3830_SPI_WDOG)		/* 263 */

/* GIC-400 Distributor offsets */
#define GICD_ISPENDR(n)		(0x0200U + 4U * (n))
#define GICD_ICPENDR(n)		(0x0280U + 4U * (n))
#define GICD_ICFGR(n)		(0x0c00U + 4U * (n))

/* Maxwell142 PMU+PROC combined boot ROM patch (57 words from platform_mif_3830.c) */
static const uint32_t s3830_ka_patch[] = {
	0x80750002, 0x11a4c218, 0x755a118a, 0x8075e080,
	0x30b3e5e7, 0xa230f8e0, 0xf5077411, 0xfdb2b5b1,
	0x7400b153, 0xb5acf507, 0xb475fdad, 0x20b3e501,
	0xb475fbe0, 0x75a3d200, 0xd2801880, 0x7907a075,
	0xe6b07837, 0x07b40754, 0xd90b8002, 0xf5c404f6,
	0x00af7580, 0x80750380, 0xf7532290, 0xeff753f7,
	0x53dff753, 0x0479fece, 0xce53fed9, 0xd90c79fd,
	0xfbce53fe, 0x53159275, 0xce53fd91, 0x02f943f7,
	0x22fef953, 0xd8fed9f9, 0x9e7522fb, 0xcfc17501,
	0x75a4c275, 0xc4754ac3, 0xa4c57547, 0x7561c675,
	0xd27542c7, 0x80d37503, 0x7500c975, 0xcb75d0ca,
	0x00cc7500, 0x75009a75, 0x9c75c09b, 0x009d7500,
	0x82740278, 0x43848012, 0x057802c6, 0x8012d074,
	0x20c34384, 0x75d08075, 0x93750291, 0x029e7501,
	0x00000022,
};

struct lx_mif_3830 {
	struct scsc_mif_abs abs;
	struct device dev;
	spinlock_t lock;

	volatile uint8_t *mbox;
	volatile uint8_t *pmu;
	volatile uint8_t *i3c_pmic;
	volatile uint8_t *dbus_baaw;
	volatile uint8_t *pbus_baaw;
	volatile uint8_t *wlbt_remap;
	volatile uint8_t *boot_cfg;
	volatile uint8_t *chipid;
	volatile uint8_t *gicd;

	uint32_t soc_product;
	uint32_t soc_rev;

	void *mem_virt;
	dma_addr_t mem_phys;
	size_t mem_size;
	bool mem_mapped;
	bool cold_init_done;
	bool in_reset;
	bool wdog_latched;

	void (*r4_handler)(int irq, void *data);
	void *r4_data;
	void (*reset_req_handler)(int irq, void *data);
	void *reset_req_data;
	int (*suspend_handler)(struct scsc_mif_abs *abs, void *data);
	void (*resume_handler)(struct scsc_mif_abs *abs, void *data);
	void *suspend_data;
	bool (*recovery_disabled)(void);

	struct scsc_mif_abs_driver *mx_drv;
	bool mx_probed;

	volatile bool poll_run;
	volatile bool poll_alive;
	bool line_attached;
	unsigned poll_ms;
	unsigned long long mbox_dispatches;
	unsigned long long cfg_req_count;
};

static struct lx_mif_3830 mif3830;

static inline uint32_t
mmio_r32(volatile uint8_t *base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);

	__asm__ volatile("dmb oshld" ::: "memory");
	return (v);
}

static inline void
mmio_w32(volatile uint8_t *base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
}

static inline void
mmio_rmw32(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t val)
{
	uint32_t cur = mmio_r32(base, off);

	mmio_w32(base, off, (cur & ~mask) | (val & mask));
}

static uint64_t
exynos_smc_conn_tzpc(void)
{
	register uint64_t x0 __asm__("x0") = (uint64_t)SMC_CMD_CONN_IF;
	register uint64_t x1 __asm__("x1") = (1ULL << 32) | (uint64_t)EXYNOS_SET_CONN_TZPC;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;

	__asm__ volatile("smc #0"
	    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
	    :
	    : "memory");
	return (x0);
}

static bool
gicd_intid_pending(uint32_t intid)
{
	if (mif3830.gicd == NULL)
		return (false);
	return ((mmio_r32(mif3830.gicd, GICD_ISPENDR(intid / 32U)) & (1U << (intid % 32U))) != 0);
}

static void
gicd_prep_level_spi(uint32_t intid)
{
	uint32_t bit;

	if (mif3830.gicd == NULL)
		return;
	/* Ensure level-sensitive (bit [2*(intid%16)+1] = 0) and clear any software pending latch */
	bit = 1U << (2U * (intid % 16U) + 1U);
	mmio_rmw32(mif3830.gicd, GICD_ICFGR(intid / 16U), bit, 0);
	mmio_w32(mif3830.gicd, GICD_ICPENDR(intid / 32U), 1U << (intid % 32U));
}

static void
wlbt_pmu_dump(const char *tag)
{
	if (mif3830.pmu == NULL)
		return;
	lx_tell("scsc: %s: CTRL_S=%#x CFG=%#x STATUS=%#x STATES=%#x CTRL_NS=%#x IN=%#x OUT=%#x DEBUG=%#x\n",
	    tag,
	    mmio_r32(mif3830.pmu, WLBT_CTRL_S),
	    mmio_r32(mif3830.pmu, WLBT_CONFIGURATION),
	    mmio_r32(mif3830.pmu, WLBT_STATUS),
	    mmio_r32(mif3830.pmu, WLBT_STATES),
	    mmio_r32(mif3830.pmu, WLBT_CTRL_NS),
	    mmio_r32(mif3830.pmu, WLBT_IN),
	    mmio_r32(mif3830.pmu, WLBT_OUT),
	    mmio_r32(mif3830.pmu, WLBT_DEBUG));
}

/* ---- CFG_REQ handler (replaces platform_cfg_req_isr) ------------------------------------------- */

static int
lx_mif_handle_cfg_req(struct lx_mif_3830 *m)
{
	uint64_t smc_ret;
	uint32_t id, updated_id, ka_off;

	m->cfg_req_count++;

	/* 1. Set WLBT TZPC to non-secure mode via EL3 monitor */
	smc_ret = exynos_smc_conn_tzpc();
	if (smc_ret != 0) {
		lx_tell("scsc: 3: SMC_CMD_CONN_IF returned %#llx (continuing)\n", (unsigned long long)smc_ret);
	} else {
		lx_tell("scsc: 3: SMC_CMD_CONN_IF TZPC non-secure OK\n");
	}

	/* 2. WLBT_REMAP (+0x400) -> 0x80000000 >> 12 */
	mmio_w32(m->wlbt_remap, 0x400, WLBT_DBUS_BAAW_0_START >> 12);

	/* 3. CHIP_VERSION_ID (+0x410) -> stamp AP SoC revision [23:16] */
	id = mmio_r32(m->wlbt_remap, CHIP_VERSION_ID_OFFSET);
	updated_id = (id & ~(CHIP_VERSION_ID_IP_MAJOR | CHIP_VERSION_ID_IP_MINOR)) |
	    ((m->soc_rev << CHIP_VERSION_ID_IP_MINOR_SHIFT) & (CHIP_VERSION_ID_IP_MAJOR | CHIP_VERSION_ID_IP_MINOR));
	mmio_w32(m->wlbt_remap, CHIP_VERSION_ID_OFFSET, updated_id);
	lx_tell("scsc: 3: CHIP_VERSION_ID %#x -> %#x (soc rev %#x)\n", id, updated_id, m->soc_rev);

	/* 4. DBUS_BAAW: map WLBT 0x80000000..0x80c00000 (12 MiB) -> AP shared DRAM */
	mmio_w32(m->dbus_baaw, 0x00, WLBT_DBUS_BAAW_0_START >> 12);
	mmio_w32(m->dbus_baaw, 0x04, WLBT_DBUS_BAAW_0_END >> 12);
	mmio_w32(m->dbus_baaw, 0x08, (uint32_t)(m->mem_phys >> 12));
	mmio_w32(m->dbus_baaw, 0x0c, WLBT_BAAW_ACCESS_CTRL);

	/* 5. PBUS_BAAW: CP2WLBT + GNSS/APM/AP/ABOX/CHUB mailboxes */
	mmio_w32(m->pbus_baaw, 0x00, WLBT_CBUS_BAAW_0_START >> 12);
	mmio_w32(m->pbus_baaw, 0x04, WLBT_CBUS_BAAW_0_END >> 12);
	mmio_w32(m->pbus_baaw, 0x08, WLBT_PBUS_MBOX_CP2WLBT_BASE >> 12);
	mmio_w32(m->pbus_baaw, 0x0c, WLBT_BAAW_ACCESS_CTRL);

	mmio_w32(m->pbus_baaw, 0x10, WLBT_CBUS_BAAW_1_START >> 12);
	mmio_w32(m->pbus_baaw, 0x14, WLBT_CBUS_BAAW_1_END >> 12);
	mmio_w32(m->pbus_baaw, 0x18, WLBT_PBUS_MBOX_GNSS2WLBT_BASE >> 12);
	mmio_w32(m->pbus_baaw, 0x1c, WLBT_BAAW_ACCESS_CTRL);

	/* 6. PMU KA RAM patch (57 words) + BOOT_CFG_ACK */
	mmio_w32(m->boot_cfg, 0x00, 0x1);
	ka_off = 0x1000;
	for (size_t i = 0; i < ARRAY_SIZE(s3830_ka_patch); i++, ka_off += 4) {
		mmio_w32(m->boot_cfg, ka_off, s3830_ka_patch[i]);
	}
	mmio_w32(m->boot_cfg, 0x00, 0x0);
	__asm__ volatile("dsb sy" ::: "memory");
	mmio_w32(m->boot_cfg, 0x04, 0x1);
	__asm__ volatile("dsb sy" ::: "memory");

	lx_tell("scsc: 3: BAAW (dram %#llx) + KA patch (%zu words) + BOOT_CFG_ACK complete\n",
	    (unsigned long long)m->mem_phys, ARRAY_SIZE(s3830_ka_patch));
	return (0);
}

/* ---- MBOX drain helper (called by polling thread or GIC SPI 54 thread) ------------------------- */

static unsigned
lx_mif_drain_mbox(struct lx_mif_3830 *m)
{
	unsigned loops = 0;

	if (m->mbox == NULL)
		return (0);

	while (loops < 64 && (mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTMSR0)) >> 16) != 0) {
		void (*fn)(int, void *) = m->r4_handler;
		void *arg = m->r4_data;

		if (fn == NULL)
			break;
		fn((int)S3830_INTID_MBOX, arg);
		m->mbox_dispatches++;
		loops++;
	}

	if (!m->in_reset && !m->wdog_latched && gicd_intid_pending(S3830_INTID_WDOG)) {
		m->wdog_latched = true;
		lx_tell("scsc: WLBT WDOG (INTID %u) asserted! INTSR0=%#x INTMR0=%#x\n",
		    S3830_INTID_WDOG,
		    mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTSR0)),
		    mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTMR0)));
		wlbt_pmu_dump("wdog");
		if (m->reset_req_handler != NULL) {
			m->reset_req_handler((int)S3830_INTID_WDOG, m->reset_req_data);
		}
	}
	return (loops);
}

static void
lx_mif_poll_thread(void *arg)
{
	struct lx_mif_3830 *m = arg;
	static char sleep_chan;

	m->poll_alive = true;
	while (m->poll_run) {
		if (lx_mif_drain_mbox(m) == 0) {
			lx_os_lock();
			if (m->poll_run) {
				lx_os_sleep(&sleep_chan, (unsigned long long)m->poll_ms * 1000000ULL);
			}
			lx_os_unlock();
		}
	}
	m->poll_alive = false;
	lx_os_lock();
	lx_os_wakeup(&m->poll_alive);
	lx_os_unlock();
}

static void
lx_mif_line_isr(void *arg)
{
	struct lx_mif_3830 *m = arg;

	lx_mif_drain_mbox(m);
	lx_os_irq_line_enable();
}

/* ---- struct scsc_mif_abs methods --------------------------------------------------------------- */

static void
mif_destroy(struct scsc_mif_abs *interface)
{
	(void)interface;
}

static char *
mif_get_uid(struct scsc_mif_abs *interface)
{
	(void)interface;
	return ("0");
}

static int
mif_pmu_reset_assert(struct lx_mif_3830 *m)
{
	unsigned waited;

	m->in_reset = true;
	mmio_rmw32(m->pmu, WLBT_CTRL_NS, WLBT_ACTIVE_EN, 0);
	mmio_rmw32(m->pmu, WLBT_CONFIGURATION, LOCAL_PWR_CFG, 0);

	for (waited = 0; waited < 500; waited++) {
		if ((mmio_r32(m->pmu, WLBT_STATUS) & WLBT_STATUS_BIT0) == 0) {
			lx_tell("scsc: WLBT powered down (WLBT_STATUS=%#x)\n", mmio_r32(m->pmu, WLBT_STATUS));
			return (0);
		}
		lx_pause_ms(1);
	}
	lx_tell("scsc: timeout waiting for WLBT power-down\n");
	wlbt_pmu_dump("reset-assert-timeout");
	return (-ETIME);
}

static int
mif_pmu_reset_release(struct lx_mif_3830 *m)
{
	unsigned waited;
	uint32_t val = 0;
	bool cfg_seen = false;

	if (!lx_scsc_may(3, "WLBT PMU reset release, SMC TZPC, BAAW, KA patch and BOOT_CFG_ACK")) {
		return (-ECANCELED);
	}

	gicd_prep_level_spi(S3830_INTID_CFG_REQ);
	gicd_prep_level_spi(S3830_INTID_WDOG);
	m->wdog_latched = false;

	if (!m->cold_init_done) {
		mmio_rmw32(m->pmu, WLBT_CTRL_S, WLBT_START, WLBT_START);
		mmio_rmw32(m->pmu, WLBT_OPTION, WLBT_OPTION_DATA, WLBT_OPTION_DATA);
		udelay(3);
		mmio_rmw32(m->pmu, TOP_OUT, PWRRGTON_CP, PWRRGTON_CP);

		for (waited = 0; waited < 500; waited++) {
			val = mmio_r32(m->i3c_pmic, VGPIO_TX_MONITOR);
			if (val & VGPIO_TX_MON_BIT29)
				break;
			lx_pause_ms(1);
		}
		if (!(val & VGPIO_TX_MON_BIT29)) {
			lx_tell("scsc: 3: VGPIO_TX_MONITOR bit29 did not set within 500 ms (%#x)\n", val);
		}
		udelay(1000);

		mmio_rmw32(m->pmu, WLBT_CONFIGURATION, LOCAL_PWR_CFG, LOCAL_PWR_CFG);
		for (waited = 0; waited < 500; waited++) {
			val = mmio_r32(m->pmu, WLBT_STATUS);
			if (val & WLBT_STATUS_BIT0)
				break;
			lx_pause_ms(1);
		}
		if (!(val & WLBT_STATUS_BIT0)) {
			lx_tell("scsc: 3: WLBT_STATUS[0] timeout (%#x)\n", val);
			wlbt_pmu_dump("pwr-up-timeout");
			return (-ETIMEDOUT);
		}

		mmio_rmw32(m->pmu, WLBT_CTRL_NS, WLBT_ACTIVE_CLR, 0);
		mmio_rmw32(m->pmu, WLBT_CTRL_NS, WLBT_ACTIVE_EN, WLBT_ACTIVE_EN);
		m->cold_init_done = true;
	} else {
		mmio_rmw32(m->pmu, WLBT_OPTION, WLBT_OPTION_DATA, WLBT_OPTION_DATA);
		udelay(3);
		mmio_rmw32(m->pmu, TOP_OUT, PWRRGTON_CP, PWRRGTON_CP);

		for (waited = 0; waited < 500; waited++) {
			val = mmio_r32(m->i3c_pmic, VGPIO_TX_MONITOR);
			if (val & VGPIO_TX_MON_BIT29)
				break;
			lx_pause_ms(1);
		}
		udelay(1000);

		mmio_rmw32(m->pmu, WLBT_CONFIGURATION, LOCAL_PWR_CFG, LOCAL_PWR_CFG);
		for (waited = 0; waited < 500; waited++) {
			val = mmio_r32(m->pmu, WLBT_STATUS);
			if (val & WLBT_STATUS_BIT0)
				break;
			lx_pause_ms(1);
		}
		if (!(val & WLBT_STATUS_BIT0)) {
			wlbt_pmu_dump("warm-pwr-timeout");
			return (-ETIMEDOUT);
		}

		mmio_rmw32(m->pmu, WLBT_CTRL_NS, WLBT_ACTIVE_CLR, 0);
		mmio_rmw32(m->pmu, WLBT_CTRL_NS, WLBT_ACTIVE_EN, WLBT_ACTIVE_EN);
	}

	/* Poll GICD_ISPENDR for CFG_REQ (SPI 230 / INTID 262) up to 1000 ms */
	for (waited = 0; waited < 200; waited++) {
		if (gicd_intid_pending(S3830_INTID_CFG_REQ)) {
			cfg_seen = true;
			break;
		}
		udelay(50);
	}
	for (waited = 0; !cfg_seen && waited < 1000; waited++) {
		if (gicd_intid_pending(S3830_INTID_CFG_REQ)) {
			cfg_seen = true;
			break;
		}
		lx_pause_ms(1);
	}
	if (!cfg_seen) {
		if (lx_os_option("scsc_force_cfg") != 0) {
			lx_tell("scsc: 3: CFG_REQ pending bit not seen; proceeding via scsc_force_cfg=1\n");
		} else {
			lx_tell("scsc: 3: timeout waiting for WLBT CFG_REQ (INTID %u)\n", S3830_INTID_CFG_REQ);
			wlbt_pmu_dump("cfg-req-timeout");
			return (-ETIMEDOUT);
		}
	}

	lx_mif_handle_cfg_req(m);
	m->in_reset = false;
	wlbt_pmu_dump("boot-cfg-done");

	if (!lx_scsc_may(4, "Maxwell MM_START_IND, HIP4 setup and WLAN service start")) {
		mif_pmu_reset_assert(m);
		return (-ECANCELED);
	}

	/* Ensure MBOX polling is active before mxman_start waits on MM_START_IND */
	lx_mif_3830_poll_start();
	return (0);
}

static int
mif_reset(struct scsc_mif_abs *interface, bool reset)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	if (!reset)
		return (mif_pmu_reset_release(m));
	return (mif_pmu_reset_assert(m));
}

static void *
mif_map(struct scsc_mif_abs *interface, size_t *allocated)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	if (allocated != NULL)
		*allocated = 0;

	if (m->mem_virt == NULL) {
		m->mem_size = S3830_WLBT_DRAM_SZ;
		m->mem_virt = dma_alloc_coherent(&m->dev, m->mem_size, &m->mem_phys, GFP_KERNEL);
		if (m->mem_virt == NULL) {
			lx_tell("scsc: failed to allocate 12 MiB WLBT shared DRAM\n");
			return (NULL);
		}
		lx_tell("scsc: 2: WLBT 12 MiB shared DRAM at phys %#llx%s\n",
		    (unsigned long long)m->mem_phys,
		    m->mem_phys == S3830_WLBT_DRAM_PA ? " (wifibt_if 0xe9400000)" :
		    m->mem_phys >= 0x100000000ULL ? " (WARNING: >4 GiB; pass wifi_dma_at=0xe9400000)" : "");
	} else {
		memset(m->mem_virt, 0, m->mem_size);
	}

	for (unsigned i = 0; i < NUM_MBOX_PLAT; i++) {
		mmio_w32(m->mbox, MAILBOX_WLBT_REG(ISSR(i)), 0);
	}
	mmio_w32(m->mbox, MAILBOX_WLBT_REG(INTMR0), 0xffff0000U);
	mmio_w32(m->mbox, MAILBOX_WLBT_REG(INTMR1), 0x0000ffffU);
	mmio_w32(m->mbox, MAILBOX_WLBT_REG(INTCR0), 0xffff0000U);
	mmio_w32(m->mbox, MAILBOX_WLBT_REG(INTCR1), 0x0000ffffU);

	m->mem_mapped = true;
	if (allocated != NULL)
		*allocated = m->mem_size;
	return (m->mem_virt);
}

static void
mif_unmap(struct scsc_mif_abs *interface, void *mem)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	(void)mem;
	if (m->mbox != NULL) {
		mmio_w32(m->mbox, MAILBOX_WLBT_REG(INTMR0), 0xffff0000U);
		mmio_w32(m->mbox, MAILBOX_WLBT_REG(INTMR1), 0x0000ffffU);
		mmio_w32(m->mbox, MAILBOX_WLBT_REG(INTCR0), 0xffff0000U);
		mmio_w32(m->mbox, MAILBOX_WLBT_REG(INTCR1), 0x0000ffffU);
	}
	m->mem_mapped = false;
}

static u32 *
mif_get_mbox_ptr(struct scsc_mif_abs *interface, u32 mbox_index)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	return ((u32 *)(uintptr_t)(m->mbox + MAILBOX_WLBT_REG(ISSR(mbox_index))));
}

static u32
mif_irq_bit_mask_status_get(struct scsc_mif_abs *interface)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	return (mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTMR0)) >> 16);
}

static u32
mif_irq_get(struct scsc_mif_abs *interface)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	return (mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTMSR0)) >> 16);
}

static void
mif_irq_bit_set(struct scsc_mif_abs *interface, int bit_num, enum scsc_mif_abs_target target)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	(void)target;
	if (bit_num < 0 || bit_num >= 16)
		return;
	mmio_w32(m->mbox, MAILBOX_WLBT_REG(INTGR1), 1U << bit_num);
}

static void
mif_irq_bit_clear(struct scsc_mif_abs *interface, int bit_num)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	if (bit_num < 0 || bit_num >= 16)
		return;
	mmio_w32(m->mbox, MAILBOX_WLBT_REG(INTCR0), (1U << bit_num) << 16);
}

static void
mif_irq_bit_mask(struct scsc_mif_abs *interface, int bit_num)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);
	unsigned long flags;

	if (bit_num < 0 || bit_num >= 16)
		return;
	spin_lock_irqsave(&m->lock, flags);
	mmio_w32(m->mbox, MAILBOX_WLBT_REG(INTMR0),
	    mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTMR0)) | ((1U << bit_num) << 16));
	spin_unlock_irqrestore(&m->lock, flags);
}

static void
mif_irq_bit_unmask(struct scsc_mif_abs *interface, int bit_num)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);
	unsigned long flags;

	if (bit_num < 0 || bit_num >= 16)
		return;
	spin_lock_irqsave(&m->lock, flags);
	mmio_w32(m->mbox, MAILBOX_WLBT_REG(INTMR0),
	    mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTMR0)) & ~((1U << bit_num) << 16));
	spin_unlock_irqrestore(&m->lock, flags);
}

static void
mif_irq_reg_handler(struct scsc_mif_abs *interface, void (*handler)(int irq, void *data), void *dev)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);
	unsigned long flags;

	spin_lock_irqsave(&m->lock, flags);
	m->r4_handler = handler;
	m->r4_data = dev;
	spin_unlock_irqrestore(&m->lock, flags);
}

static void
mif_irq_unreg_handler(struct scsc_mif_abs *interface)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);
	unsigned long flags;

	spin_lock_irqsave(&m->lock, flags);
	m->r4_handler = NULL;
	m->r4_data = NULL;
	spin_unlock_irqrestore(&m->lock, flags);
}

static void
mif_irq_clear(void)
{
}

static void
mif_irq_reg_reset_req(struct scsc_mif_abs *interface, void (*handler)(int irq, void *data), void *dev)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	m->reset_req_handler = handler;
	m->reset_req_data = dev;
}

static void
mif_irq_unreg_reset_req(struct scsc_mif_abs *interface)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	m->reset_req_handler = NULL;
	m->reset_req_data = NULL;
}

static void
mif_suspend_reg(struct scsc_mif_abs *interface,
    int (*suspend)(struct scsc_mif_abs *abs, void *data),
    void (*resume)(struct scsc_mif_abs *abs, void *data),
    void *data)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	m->suspend_handler = suspend;
	m->resume_handler = resume;
	m->suspend_data = data;
}

static void
mif_suspend_unreg(struct scsc_mif_abs *interface)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	m->suspend_handler = NULL;
	m->resume_handler = NULL;
	m->suspend_data = NULL;
}

static void *
mif_get_mifram_ptr(struct scsc_mif_abs *interface, scsc_mifram_ref ref)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	if (!m->mem_mapped || m->mem_virt == NULL || ref < 0 || (size_t)ref >= m->mem_size)
		return (NULL);
	return ((uint8_t *)m->mem_virt + (size_t)ref);
}

static int
mif_get_mifram_ref(struct scsc_mif_abs *interface, void *ptr, scsc_mifram_ref *ref)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);
	uintptr_t base, p;

	if (!m->mem_mapped || m->mem_virt == NULL || ptr == NULL)
		return (-ENOMEM);
	base = (uintptr_t)m->mem_virt;
	p = (uintptr_t)ptr;
	if (p < base || p >= base + m->mem_size)
		return (-ENOMEM);
	*ref = (scsc_mifram_ref)(p - base);
	return (0);
}

static uintptr_t
mif_get_mifram_pfn(struct scsc_mif_abs *interface)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	return ((uintptr_t)(m->mem_phys >> PAGE_SHIFT));
}

static void *
mif_get_mifram_phy_ptr(struct scsc_mif_abs *interface, scsc_mifram_ref ref)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	if (m->mem_phys == 0 || ref < 0 || (size_t)ref >= m->mem_size)
		return (NULL);
	return ((void *)(uintptr_t)(m->mem_phys + (size_t)ref));
}

static struct device *
mif_get_mif_device(struct scsc_mif_abs *interface)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	return (&m->dev);
}

static void
mif_dump_registers(struct scsc_mif_abs *interface)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	if (m->mbox == NULL)
		return;
	lx_tell("scsc: mbox: INTGR0=%#x INTMR0=%#x INTSR0=%#x INTMSR0=%#x INTGR1=%#x INTMR1=%#x INTSR1=%#x\n",
	    mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTGR0)),
	    mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTMR0)),
	    mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTSR0)),
	    mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTMSR0)),
	    mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTGR1)),
	    mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTMR1)),
	    mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTSR1)));
	wlbt_pmu_dump("mif-dump");
}

static void
mif_cleanup(struct scsc_mif_abs *interface)
{
	(void)interface;
}

static void
mif_restart(struct scsc_mif_abs *interface)
{
	(void)interface;
}

static bool
mif_reset_failure(struct scsc_mif_abs *interface)
{
	(void)interface;
	return (false);
}

static int
mif_read_register(struct scsc_mif_abs *interface, u64 id, u32 *val)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	if (id == SCSC_REG_READ_WLBT_STAT && m->pmu != NULL) {
		*val = mmio_r32(m->pmu, WLBT_STAT);
		return (0);
	}
	return (-EIO);
}

static void
mif_recovery_disabled_reg(struct scsc_mif_abs *interface, bool (*handler)(void))
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	m->recovery_disabled = handler;
}

static void
mif_recovery_disabled_unreg(struct scsc_mif_abs *interface)
{
	struct lx_mif_3830 *m = container_of(interface, struct lx_mif_3830, abs);

	m->recovery_disabled = NULL;
}

/* ---- SCSC MIF driver registration hooks (called by scsc_mx_module.c) --------------------------- */

void
scsc_mif_abs_register(struct scsc_mif_abs_driver *driver)
{
	mif3830.mx_drv = driver;
}

void
scsc_mif_abs_unregister(struct scsc_mif_abs_driver *driver)
{
	if (mif3830.mx_drv == driver)
		mif3830.mx_drv = NULL;
}

void
scsc_mif_mmap_register(struct scsc_mif_mmap_driver *mmap_driver)
{
	(void)mmap_driver;
}

void
scsc_mif_mmap_unregister(struct scsc_mif_mmap_driver *mmap_driver)
{
	(void)mmap_driver;
}

/* ---- Public control entry points for lx_scsc.c ------------------------------------------------ */

int
lx_mif_3830_map_hw(void)
{
	struct lx_mif_3830 *m = &mif3830;
	unsigned pm;

	if (m->mbox != NULL)
		return (0);

	spin_lock_init(&m->lock);
	m->in_reset = true;
	pm = lx_os_option("scsc_poll_ms");
	m->poll_ms = pm >= 1 && pm <= 100 ? pm : 1;

	m->mbox = lx_os_map_registers(S3830_MBOX_WLBT_PA, S3830_MBOX_WLBT_SZ);
	m->pmu = lx_os_map_registers(S3830_PMU_ALIVE_PA, S3830_PMU_ALIVE_SZ);
	m->i3c_pmic = lx_os_map_registers(S3830_I3C_APM_PMIC_PA, S3830_I3C_APM_PMIC_SZ);
	m->dbus_baaw = lx_os_map_registers(S3830_DBUS_BAAW_PA, S3830_DBUS_BAAW_SZ);
	m->pbus_baaw = lx_os_map_registers(S3830_PBUS_BAAW_PA, S3830_PBUS_BAAW_SZ);
	m->wlbt_remap = lx_os_map_registers(S3830_WLBT_REMAP_PA, S3830_WLBT_REMAP_SZ);
	m->boot_cfg = lx_os_map_registers(S3830_BOOT_CFG_PA, S3830_BOOT_CFG_SZ);
	m->chipid = lx_os_map_registers(S3830_CHIPID_PA, S3830_CHIPID_SZ);
	m->gicd = lx_os_map_registers(S3830_GICD_PA, S3830_GICD_SZ);

	if (!m->mbox || !m->pmu || !m->i3c_pmic || !m->dbus_baaw ||
	    !m->pbus_baaw || !m->wlbt_remap || !m->boot_cfg || !m->chipid || !m->gicd) {
		lx_tell("scsc: 1: failed to map Exynos 850 WLBT MMIO blocks\n");
		return (ENXIO);
	}

	m->soc_product = mmio_r32(m->chipid, S3830_CHIPID_PRO_ID) & 0xfffff000U;
	m->soc_rev = (mmio_r32(m->chipid, S3830_CHIPID_REV) >> 16) & 0xffU;

	m->abs.destroy = mif_destroy;
	m->abs.get_uid = mif_get_uid;
	m->abs.reset = mif_reset;
	m->abs.map = mif_map;
	m->abs.unmap = mif_unmap;
	m->abs.get_mbox_ptr = mif_get_mbox_ptr;
	m->abs.irq_bit_mask_status_get = mif_irq_bit_mask_status_get;
	m->abs.irq_get = mif_irq_get;
	m->abs.irq_bit_clear = mif_irq_bit_clear;
	m->abs.irq_bit_mask = mif_irq_bit_mask;
	m->abs.irq_bit_unmask = mif_irq_bit_unmask;
	m->abs.irq_bit_set = mif_irq_bit_set;
	m->abs.irq_reg_handler = mif_irq_reg_handler;
	m->abs.irq_unreg_handler = mif_irq_unreg_handler;
	m->abs.irq_clear = mif_irq_clear;
	m->abs.irq_reg_reset_request_handler = mif_irq_reg_reset_req;
	m->abs.irq_unreg_reset_request_handler = mif_irq_unreg_reset_req;
	m->abs.suspend_reg_handler = mif_suspend_reg;
	m->abs.suspend_unreg_handler = mif_suspend_unreg;
	m->abs.get_mifram_ptr = mif_get_mifram_ptr;
	m->abs.get_mifram_ref = mif_get_mifram_ref;
	m->abs.get_mifram_pfn = mif_get_mifram_pfn;
	m->abs.get_mifram_phy_ptr = mif_get_mifram_phy_ptr;
	m->abs.get_mif_device = mif_get_mif_device;
	m->abs.mif_dump_registers = mif_dump_registers;
	m->abs.mif_cleanup = mif_cleanup;
	m->abs.mif_restart = mif_restart;
	m->abs.mif_reset_failure = mif_reset_failure;
	m->abs.mif_read_register = mif_read_register;
	m->abs.recovery_disabled_reg = mif_recovery_disabled_reg;
	m->abs.recovery_disabled_unreg = mif_recovery_disabled_unreg;

	lx_tell("scsc: 1: Exynos 850 WLBT MMIO mapped: product=%#x rev=%#x MBOX_VER=%#x WLBT_STATUS=%#x WLBT_STATES=%#x\n",
	    m->soc_product, m->soc_rev,
	    mmio_r32(m->mbox, MAILBOX_WLBT_REG(IS_VERSION)),
	    mmio_r32(m->pmu, WLBT_STATUS),
	    mmio_r32(m->pmu, WLBT_STATES));
	return (0);
}

int
lx_mif_3830_probe_mx(void)
{
	struct lx_mif_3830 *m = &mif3830;

	if (m->mx_drv == NULL || m->mx_drv->probe == NULL)
		return (ENXIO);
	if (!m->mx_probed) {
		m->mx_drv->probe(m->mx_drv, &m->abs);
		m->mx_probed = true;
	}
	return (0);
}

void
lx_mif_3830_remove_mx(void)
{
	struct lx_mif_3830 *m = &mif3830;

	if (m->mx_probed && m->mx_drv != NULL && m->mx_drv->remove != NULL) {
		m->mx_drv->remove(&m->abs);
		m->mx_probed = false;
	}
}

int
lx_mif_3830_poll_start(void)
{
	struct lx_mif_3830 *m = &mif3830;
	int err;

	if (m->poll_alive || m->line_attached)
		return (0);
	m->poll_run = true;
	err = lx_os_thread("scsc-poll", lx_mif_poll_thread, m);
	if (err != 0) {
		m->poll_run = false;
		lx_tell("scsc: failed to start scsc-poll thread: %d\n", err);
		return (err);
	}
	lx_tell("scsc: 4: MBOX polling thread active (every %u ms)\n", m->poll_ms);
	return (0);
}

void
lx_mif_3830_poll_stop(void)
{
	struct lx_mif_3830 *m = &mif3830;

	if (!m->poll_run && !m->poll_alive)
		return;
	lx_os_lock();
	m->poll_run = false;
	while (m->poll_alive) {
		lx_os_sleep(&m->poll_alive, 5000000ULL);
	}
	lx_os_unlock();
}

int
lx_mif_3830_irq_start(void)
{
	struct lx_mif_3830 *m = &mif3830;
	int err;

	if (lx_os_option("scsc_poll") != 0) {
		lx_tell("scsc: 5: scsc_poll=1 set; keeping %u ms MBOX polling thread\n", m->poll_ms);
		return (0);
	}
	if (m->line_attached)
		return (0);

	err = lx_os_irq_line_attach(lx_mif_line_isr, m);
	if (err != 0) {
		lx_tell("scsc: 5: kernel line IRQ (SPI %u / INTID %u) unavailable (%d); keeping %u ms MBOX polling thread\n",
		    S3830_SPI_MBOX, S3830_INTID_MBOX, err, m->poll_ms);
		return (0);
	}
	lx_mif_3830_poll_stop();
	m->line_attached = true;
	/* Drain any pending MBOX bits latched during handoff, then unmask GIC SPI */
	lx_mif_drain_mbox(m);
	lx_os_irq_line_enable();
	lx_tell("scsc: 5: switched MBOX from polling to GIC SPI %u (INTID %u)\n", S3830_SPI_MBOX, S3830_INTID_MBOX);
	return (0);
}

void
lx_mif_3830_irq_stop(void)
{
	struct lx_mif_3830 *m = &mif3830;

	if (m->line_attached) {
		lx_os_irq_line_detach();
		m->line_attached = false;
	}
}

void
lx_mif_3830_quiesce(void)
{
	struct lx_mif_3830 *m = &mif3830;

	lx_mif_3830_irq_stop();
	lx_mif_3830_poll_stop();
	if (m->pmu != NULL && !m->in_reset) {
		mif_pmu_reset_assert(m);
	}
}

unsigned long long
lx_mif_3830_irqs_seen(void)
{
	return (mif3830.line_attached ? lx_os_irq_line_count() : mif3830.mbox_dispatches);
}

void
lx_mif_3830_say_state(void)
{
	struct lx_mif_3830 *m = &mif3830;

	if (m->mbox == NULL)
		return;
	lx_tell("  mif: dram=%#llx (%s) mode=%s mbox_dispatches=%llu line_irqs=%llu cfg_req=%llu INTSR0=%#x INTMR0=%#x WLBT_STATUS=%#x\n",
	    (unsigned long long)m->mem_phys,
	    m->in_reset ? "in-reset" : "running",
	    m->line_attached ? "gic-spi54" : m->poll_alive ? "poll" : "idle",
	    m->mbox_dispatches,
	    m->line_attached ? lx_os_irq_line_count() : 0ULL,
	    m->cfg_req_count,
	    mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTSR0)),
	    mmio_r32(m->mbox, MAILBOX_WLBT_REG(INTMR0)),
	    mmio_r32(m->pmu, WLBT_STATUS));
}
