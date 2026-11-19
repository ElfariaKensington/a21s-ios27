/*
 * Shared internal declarations for Samsung Exynos 850 (S5E3830, SM-A217F)
 * SCSC WLBT (Maxwell S612 mx140 + slsi_wlan) on Scion's wifi/lx layer.
 */
#ifndef _LX_SCSC_H_
#define _LX_SCSC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct net_device;
struct wiphy;

/* lx_scsc.c: bring-up ladder scsc_wlan=1..8 */
bool	 lx_scsc_may(unsigned step, const char *what);
bool	 lx_scsc_reached(unsigned step);
unsigned lx_scsc_steps(void);
void	 lx_scsc_set_mac_override(const uint8_t addr[6]);

/* lx_mif_3830.c: standalone Exynos 850 WLBT MIF */
int	 lx_mif_3830_map_hw(void);
int	 lx_mif_3830_probe_mx(void);
void	 lx_mif_3830_remove_mx(void);
int	 lx_mif_3830_poll_start(void);
void	 lx_mif_3830_poll_stop(void);
int	 lx_mif_3830_irq_start(void);
void	 lx_mif_3830_irq_stop(void);
void	 lx_mif_3830_quiesce(void);
void	 lx_mif_3830_say_state(void);
unsigned long long lx_mif_3830_irqs_seen(void);

/* lx_cfg.c: cfg80211 + net_device + hakios_sta_ops station layer */
int	 lx_cfg_open_wlan(void);
void	 lx_cfg_begin(void);
void	 lx_cfg_stop(void);
int	 lx_cfg_transmit(const unsigned char *frame, unsigned long length);
void	 lx_cfg_apply_mac(const uint8_t addr[6]);
const uint8_t *lx_cfg_mac(void);
void	 lx_cfg_say_state(void);

/* SCSC driver entry points exposed by scsc-lx.patch */
int	 lx_scsc_mx_module_init(void);
void	 lx_scsc_mx_module_exit(void);
int	 lx_slsi_wlan_driver_init(void);
void	 lx_slsi_wlan_driver_exit(void);
void	 lx_slsi_set_mac_override_str(const char *mac_str);
bool	 lx_slsi_sta_port_is_open(struct net_device *dev);

#endif
