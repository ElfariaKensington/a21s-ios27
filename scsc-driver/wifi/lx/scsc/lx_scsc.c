/*
 * Top-level XNU <-> LinuxKPI entry points (lx_prepare, lx_start, lx_stop, lx_transmit)
 * for Samsung Exynos 850 (S5E3830, Galaxy A21s SM-A217F) SCSC WLBT (mx140 + slsi_wlan).
 *
 * Step-by-step bring-up ladder (boot-arg scsc_wlan=1..8):
 *   scsc_wlan=1  Map 7 Exynos 850 WLBT MMIO blocks + CHIPID + GICD; probe scsc_mx & slsi_wlan;
 *                log PMU/MBOX state; stop before requesting firmware.
 *   scsc_wlan=2  Allocate/map 12 MiB shared DRAM (0xe9400000 with wifi_dma_at=0xe9400000),
 *                load & CRC-check mx140.bin + mx140_common.hcf + slsi_reg_database.bin,
 *                init MIF RAM & MBOX ISSRs; stop before WLBT PMU reset release.
 *   scsc_wlan=3  Release WLBT PMU reset, run SMC_CMD_CONN_IF TZPC setup, program WLBT_REMAP,
 *                CHIP_VERSION_ID, DBUS_BAAW, PBUS_BAAW, 57-word KA patch & BOOT_CFG_ACK on
 *                CFG_REQ (INTID 262); stop before Maxwell MM_START_IND / HIP4.
 *   scsc_wlan=4  Start 1 ms MBOX polling thread, complete Maxwell R4 boot (MM_START_IND),
 *                load mx140_wlan.hcf, start HIP4 & WLAN service, read MIBs & regulatory DB.
 *   scsc_wlan=5  Switch MBOX from polling to GICv2 SPI 54 (INTID 86) via lx_os_irq_line_attach()
 *                (unless scsc_poll=1).
 *   scsc_wlan=6  Apply phone MAC address from lx_os_radio_address() (wifi_mac / fw/mac.info).
 *   scsc_wlan=7  Attach en0 (lx_os_net_attach) and offer hakios_sta_ops (lx_os_station) for scanning.
 *   scsc_wlan=8  Enable open and WPA2-PSK network joining (lx_wpa.c + slsi_add_key) and frame TX/RX.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/workqueue.h>

#include <lx_os.h>
#include "lx.h"
#include "lx_scsc.h"

extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_power_efficient_wq;

static unsigned steps;
static unsigned stopped_at;
static bool initialized;
static bool running;

unsigned
lx_scsc_steps(void)
{
	return (steps);
}

bool
lx_scsc_reached(unsigned step)
{
	return (steps >= step);
}

bool
lx_scsc_may(unsigned step, const char *what)
{
	if (steps >= step) {
		lx_tell("scsc: %u: %s\n", step, what);
		return (true);
	}
	if (stopped_at == 0) {
		stopped_at = step;
		lx_tell("scsc: stopping before step %u (%s); scsc_wlan=%u\n", step, what, steps);
	}
	return (false);
}

static void
scsc_say_state(void)
{
	lx_tell("scsc: scsc_wlan=%u running=%d\n", steps, running);
	lx_memory_say();
	lx_mif_3830_say_state();
	lx_cfg_say_state();
}

int
lx_prepare(void)
{
	unsigned opt = lx_os_option("scsc_wlan");

	if (opt == 0) {
		/* Allow fallback to wifi_step if caller uses generic option */
		opt = lx_os_option("wifi_step");
	}
	if (opt == 0)
		return (0);
	return (lx_base_init() != 0 || lx_pool_init() != 0 ? ENOMEM : 0);
}

int
lx_start(void)
{
	uint8_t mac[6];
	char mac_str[20];
	int err;

	steps = lx_os_option("scsc_wlan");
	if (steps == 0)
		steps = lx_os_option("wifi_step");
	if (steps == 0) {
		lx_tell("scsc: scsc_wlan=0 (pass scsc_wlan=1..8 to start Exynos 850 WLBT Wi-Fi)\n");
		return (0);
	}
	stopped_at = 0;
	lx_tell("scsc: starting Exynos 850 SCSC WLBT (mx140 + slsi_wlan), scsc_wlan=%u\n", steps);

	if ((err = lx_base_init()) != 0 || (err = lx_pool_init()) != 0) {
		lx_tell("scsc: lx_base_init / lx_pool_init failed: %d\n", err);
		return (err);
	}
	if (!initialized) {
		system_wq = alloc_workqueue("scsc-sys", 0, 1);
		system_power_efficient_wq = system_wq;
		if (system_wq == NULL)
			return (ENOMEM);
		initialized = true;
	}

	/* Step 1: Map Exynos 850 WLBT hardware & register scsc_mx + slsi_wlan */
	lx_scsc_may(1, "mapping Exynos 850 WLBT MMIO and registering scsc_mx + slsi_wlan");
	if ((err = lx_mif_3830_map_hw()) != 0) {
		lx_os_failure(true);
		scsc_say_state();
		lx_os_failure(false);
		return (err);
	}

	if (!running) {
		if ((err = lx_scsc_mx_module_init()) != 0)
			return (err < 0 ? -err : err);
		if ((err = lx_slsi_wlan_driver_init()) != 0)
			return (err < 0 ? -err : err);
		if ((err = lx_mif_3830_probe_mx()) != 0)
			return (err);
		running = true;
		lx_os_wifi_state(scsc_say_state);
	}

	if (!lx_scsc_may(2, "loading mx140.bin, HCF configs and regulatory DB into 12 MiB shared DRAM")) {
		return (0);
	}

	/* Step 6 MAC override is prepared ahead of wlan0 open so slsi_mac_address_init picks it up */
	if (steps >= 6 && lx_os_radio_address(mac)) {
		snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
		    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		lx_slsi_set_mac_override_str(mac_str);
		lx_cfg_apply_mac(mac);
		lx_tell("scsc: 6: radio MAC override set (%02x:%02x:%02x:..:..:..)\n", mac[0], mac[1], mac[2]);
	}

	/* Steps 2..4 run inside lx_cfg_open_wlan() -> slsi_net_open() -> slsi_start() -> mxman_open() */
	err = lx_cfg_open_wlan();
	if (err != 0) {
		lx_os_failure(true);
		scsc_say_state();
		lx_os_failure(false);
		return (err);
	}
	if (steps < 4) {
		/* Steps 2 and 3 completed their target check cleanly */
		return (0);
	}

	/* Step 5: Switch MBOX from 1 ms polling thread to GICv2 SPI 54 (unless scsc_poll=1) */
	if (!lx_scsc_may(5, "attaching WLBT MBOX GICv2 SPI 54 interrupt")) {
		return (0);
	}
	lx_mif_3830_irq_start();

	if (!lx_scsc_may(6, "finalizing station MAC address")) {
		return (0);
	}

	/* Step 7 & 8: Offer en0 + hakios_sta_ops (scanning at 7, WPA2-PSK joining at 8) */
	lx_cfg_begin();
	return (0);
}

void
lx_stop(void)
{
	if (!running)
		return;
	lx_tell("scsc: stopping SCSC WLBT driver\n");
	lx_os_wifi_state(NULL);
	lx_cfg_stop();
	lx_mif_3830_remove_mx();
	lx_slsi_wlan_driver_exit();
	lx_scsc_mx_module_exit();
	lx_mif_3830_quiesce();
	running = false;
}

int
lx_transmit(const unsigned char *frame, unsigned long length)
{
	return (lx_cfg_transmit(frame, length));
}
