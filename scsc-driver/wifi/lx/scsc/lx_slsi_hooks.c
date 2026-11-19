/*
 * Thin SLSI structure inspection helper compiled with wireless/scsc headers.
 */
#include "dev.h"
#include "mgt.h"
#include "scsc_wifi_fcq.h"
#include "lx_scsc.h"

_Static_assert(offsetof(struct net_device, drv_priv) == ALIGN(sizeof(struct net_device), NETDEV_ALIGN),
    "slsi_ndev_vif_2_net_device offset must match netdev_priv()");

bool
lx_slsi_sta_port_is_open(struct net_device *dev)
{
	struct netdev_vif *ndev_vif;
	struct slsi_peer *peer;

	if (dev == NULL)
		return (false);
	ndev_vif = netdev_priv(dev);
	if (ndev_vif == NULL || !ndev_vif->activated)
		return (false);
	peer = ndev_vif->peer_sta_record[SLSI_STA_PEER_QUEUESET];
	if (peer == NULL || !peer->valid)
		return (false);
	return (peer->authorized &&
	    peer->data_qs.controlled_port_state == SCSC_WIFI_FCQ_8021x_STATE_OPEN &&
	    ndev_vif->sta.m4_host_tag == 0);
}
