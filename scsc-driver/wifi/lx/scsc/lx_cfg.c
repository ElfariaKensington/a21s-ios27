/*
 * cfg80211, net_device ("wlan0" -> "en0"), and hakios_sta_ops station layer for
 * Samsung Exynos 850 slsi_wlan (Maxwell S612).
 *
 * Key slsi_wlan adaptations over brcmfmac's lx_cfg.c:
 *   1. Real struct cfg80211_bss + cfg80211_bss_ies cache (HEARD_MOST=64): slsi_rx_scan_pass_to_cfg80211()
 *      calls cfg80211_inform_bss_frame() and slsi_set_sta_bss_info() / slsi_rx_connect_ind() look up the
 *      cached BSS via cfg80211_get_bss() and inspect bss->channel, bss->bssid, bss->capability, bss->ies.
 *   2. cfg80211_connect_bss / cfg80211_connect_result / cfg80211_connect_timeout callbacks.
 *   3. TX path calls ndo_select_queue() + sets skb->protocol / mac_header / network_header so
 *      slsi_net_hw_xmit() routes EAPOL & DHCP to SLSI_NETIF_Q_PRIORITY (0), ARP to SLSI_NETIF_Q_ARP (1),
 *      and data frames to SLSI_NETIF_Q_PEER_START+.
 *   4. WPA2-PSK 4-way handshake via lx_wpa.c + slsi_add_key(), waiting for M4 air-ACK
 *      (lx_slsi_sta_port_is_open) before asserting lx_os_net_link(1) so the initial DHCP/ARP is never
 *      dropped by SCSC_WIFI_FCQ_8021x_STATE_BLOCKED.
 */
#include <linux/kernel.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/ieee80211.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>

#include <lx_os.h>
#include "lx.h"
#include "lx_scsc.h"
#include "lx_rsn.h"
#include "lx_wpa.h"

#ifndef WLAN_AKM_SUITE_PSK
#define WLAN_AKM_SUITE_PSK	0x000fac02
#endif

#define HEARD_MOST		64
#define EAPOL_MOST		8
#define EAPOL_BYTES		(14 + 600)

/* Direct slsi_wlan cfg80211 entry points (avoids Linux 4.19 vs 6.x struct cfg80211_ops link_id drift) */
int slsi_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request);
int slsi_connect(struct wiphy *wiphy, struct net_device *dev, struct cfg80211_connect_params *sme);
int slsi_disconnect(struct wiphy *wiphy, struct net_device *dev, u16 reason_code);
int slsi_add_key(struct wiphy *wiphy, struct net_device *dev, u8 key_index, bool pairwise,
    const u8 *mac_addr, struct key_params *params);
int slsi_get_station(struct wiphy *wiphy, struct net_device *dev, const u8 *mac, struct station_info *sinfo);

struct lx_eapol {
	uint16_t length;
	uint8_t bytes[EAPOL_BYTES];
};

/* Backing storage for each cached struct cfg80211_bss + flexible ies->data[] */
struct lx_bss_slot {
	struct cfg80211_bss bss;
	union {
		struct cfg80211_bss_ies ies;
		uint8_t raw[sizeof(struct cfg80211_bss_ies) + HAKIOS_STA_ELEMENTS_MOST];
	} u;
};

static struct {
	struct wiphy *wiphy;
	struct net_device *ndev;		/* "wlan0", station interface */
	bool offered, up, carrier;
	bool has_mac_override;
	uint8_t mac_override[ETH_ALEN];
	struct mutex send;

	/* Scan & BSS cache */
	volatile int scan_over;
	bool scan_aborted;
	unsigned heard_count;
	struct hakios_sta_bss *heard;
	struct lx_bss_slot *slots;
	unsigned long long *heard_at;

	/* Association & WPA2-PSK state */
	volatile int connected;			/* 0 waiting, 1 associated, -1 refused or lost */
	int status;
	volatile bool joining, joined, leaving;
	int last_error;
	struct hakios_sta_bss target;
	bool protected;
	unsigned group;
	uint8_t rsn[LX_RSN_OURS_LENGTH];
	struct lx_wpa wpa;
	volatile int handshake;
	const char *why;
	volatile int keys;

	struct lx_eapol *eapol;
	unsigned eapol_in, eapol_out;
	struct workqueue_struct *work;
	struct work_struct eapol_work;
	struct workqueue_struct *join_wq;
	struct work_struct autojoin_work;

	unsigned long long sent, received, eapol_count;
} c;

static struct hakios_sta_ops boundary;

static void
ensure_bss_tables(void)
{
	if (c.heard == NULL)
		c.heard = kzalloc(HEARD_MOST * sizeof(*c.heard), GFP_KERNEL);
	if (c.slots == NULL)
		c.slots = kzalloc(HEARD_MOST * sizeof(*c.slots), GFP_KERNEL);
	if (c.heard_at == NULL)
		c.heard_at = kzalloc(HEARD_MOST * sizeof(*c.heard_at), GFP_KERNEL);
}

/* ---- wiphy registration ------------------------------------------------------------------------- */

struct wiphy *
linuxkpi_wiphy_new(const struct cfg80211_ops *ops, size_t priv_length)
{
	struct wiphy *wiphy = kzalloc(sizeof(*wiphy) + priv_length, GFP_KERNEL);

	(void)ops;
	if (wiphy != NULL)
		mutex_init(&wiphy->mtx);
	return (wiphy);
}

void
linuxkpi_wiphy_free(struct wiphy *wiphy)
{
	if (wiphy == c.wiphy)
		c.wiphy = NULL;
	kfree(wiphy);
}

int
linuxkpi_80211_wiphy_register(struct wiphy *wiphy)
{
	c.wiphy = wiphy;
	for (int b = 0; b < NUM_NL80211_BANDS; b++) {
		if (wiphy->bands[b] != NULL) {
			lx_tell("scsc: cfg80211: band %d: %d channels, %d rates\n",
			    b, wiphy->bands[b]->n_channels, wiphy->bands[b]->n_bitrates);
		}
	}
	return (0);
}

void
lx_wiphy_unregister(struct wiphy *wiphy)
{
	if (wiphy == c.wiphy)
		c.wiphy = NULL;
}

/* ---- net_device ("wlan0") ----------------------------------------------------------------------- */

int
lx_register_netdev(struct net_device *ndev)
{
	ndev->reg_state = NETREG_REGISTERED;
	if (c.ndev == NULL && strncmp(ndev->name, "wlan", 4) == 0) {
		c.ndev = ndev;
		if (c.has_mac_override)
			memcpy(ndev->dev_addr, c.mac_override, ETH_ALEN);
		lx_tell("scsc: netdev %s registered\n", ndev->name);
	}
	return (0);
}

void
lx_unregister_netdev(struct net_device *ndev)
{
	ndev->reg_state = (enum net_device_reg_state)0;
	if (ndev == c.ndev) {
		c.ndev = NULL;
		c.up = false;
	}
}

void
lx_netif_carrier(struct net_device *ndev, bool on)
{
	if (ndev == c.ndev)
		c.carrier = on;
}

bool
lx_netif_carrier_ok(struct net_device *ndev)
{
	return (ndev == c.ndev && c.carrier);
}

void
lx_cfg_apply_mac(const uint8_t addr[6])
{
	memcpy(c.mac_override, addr, ETH_ALEN);
	c.has_mac_override = true;
	if (c.ndev != NULL)
		memcpy(c.ndev->dev_addr, addr, ETH_ALEN);
}

const uint8_t *
lx_cfg_mac(void)
{
	return (c.ndev != NULL ? c.ndev->dev_addr : c.mac_override);
}

/*
 * Leaves the 14-byte Ethernet header on skb->data for lx_netif_rx() while setting
 * skb->dev, mac_header, pkt_type, and returning ethertype in network byte order.
 */
__be16
lx_eth_type_trans(struct sk_buff *skb, struct net_device *ndev)
{
	skb->dev = ndev;
	skb_reset_mac_header(skb);
	if (skb->len < ETH_HLEN)
		return (0);
	skb->pkt_type = !(skb->data[0] & 1) ? PACKET_OTHERHOST :
	    is_broadcast_ether_addr(skb->data) ? PACKET_BROADCAST : PACKET_MULTICAST;
	return (*(__be16 *)(skb->data + 12));
}

int
lx_netif_rx(struct sk_buff *skb)
{
	if (skb == NULL)
		return (0);

	if (skb->len >= ETH_HLEN && skb->data[12] == 0x88 && skb->data[13] == 0x8e) {
		bool queued = false;

		lx_os_lock();
		if (c.eapol != NULL && c.work != NULL &&
		    c.eapol_in - c.eapol_out < EAPOL_MOST && skb->len <= EAPOL_BYTES) {
			struct lx_eapol *e = &c.eapol[c.eapol_in % EAPOL_MOST];

			memcpy(e->bytes, skb->data, skb->len);
			e->length = (uint16_t)skb->len;
			c.eapol_in++;
			queued = true;
		}
		lx_os_unlock();
		if (queued)
			queue_work(c.work, &c.eapol_work);
	} else if (c.joined && skb->len >= ETH_HLEN) {
		c.received++;
		lx_os_net_input(skb->data, skb->len);
	}
	dev_kfree_skb(skb);
	return (0); /* NET_RX_SUCCESS */
}

static int
frame_send(const uint8_t *frame, unsigned length)
{
	struct net_device *ndev = c.ndev;
	struct sk_buff *skb;
	unsigned head;

	if (ndev == NULL || !c.up || length < ETH_HLEN)
		return (ENETDOWN);

	head = (unsigned)ndev->needed_headroom + 96U;
	skb = dev_alloc_skb(head + length + (unsigned)ndev->needed_tailroom);
	if (skb == NULL)
		return (ENOBUFS);

	skb_reserve(skb, (int)head);
	memcpy(skb_put(skb, length), frame, length);
	skb->dev = ndev;
	skb->protocol = *(__be16 *)(skb->data + 12);
	skb_reset_mac_header(skb);
	skb->l3hdroff = (uint16_t)((skb->data - skb->head) + ETH_HLEN);

	mutex_lock(&c.send);
	if (ndev->netdev_ops->ndo_select_queue != NULL) {
		skb->queue_mapping = ndev->netdev_ops->ndo_select_queue(ndev, skb, NULL);
	}
	ndev->netdev_ops->ndo_start_xmit(skb, ndev);
	mutex_unlock(&c.send);
	c.sent++;
	return (0);
}

int
lx_cfg_transmit(const unsigned char *frame, unsigned long length)
{
	if (!c.joined)
		return (ENETDOWN);
	return (frame_send(frame, (unsigned)length));
}

/* ---- BSS cache (cfg80211_inform_bss_frame + cfg80211_get_bss) ----------------------------------- */

struct cfg80211_bss *
lx_cfg80211_inform_bss_frame(struct wiphy *wiphy, struct linuxkpi_ieee80211_channel *channel,
    struct ieee80211_mgmt *mgmt, size_t len, int32_t signal_mbm, gfp_t gfp)
{
	const size_t fixed_hdr = offsetof(struct ieee80211_mgmt, u.beacon.variable);
	const uint8_t *ie, *ssid_ie;
	size_t ie_len, kept = 0;
	uint16_t capability, interval;
	int idx = -1;

	(void)wiphy;
	(void)gfp;
	if (mgmt == NULL || len < fixed_hdr)
		return (NULL);

	ensure_bss_tables();
	if (c.heard == NULL || c.slots == NULL || c.heard_at == NULL)
		return (NULL);

	ie = mgmt->u.beacon.variable;
	ie_len = len - fixed_hdr;
	capability = le16_to_cpu(mgmt->u.beacon.capab_info);
	interval = le16_to_cpu(mgmt->u.beacon.beacon_int);
	ssid_ie = lx_rsn_element(ie, (unsigned)ie_len, 0);

	while (kept + 2 <= ie_len && kept + 2 + ie[kept + 1] <= ie_len &&
	    kept + 2 + ie[kept + 1] <= HAKIOS_STA_ELEMENTS_MOST) {
		kept += 2 + ie[kept + 1];
	}

	lx_os_lock();
	for (unsigned i = 0; i < c.heard_count; i++) {
		if (memcmp(c.heard[i].bssid, mgmt->bssid, ETH_ALEN) == 0) {
			/* Keep non-empty SSID if a probe/beacon previously supplied it and this beacon is hidden */
			idx = (int)i;
			break;
		}
	}
	if (idx < 0) {
		if (c.heard_count < HEARD_MOST) {
			idx = (int)c.heard_count++;
		} else {
			/* Replace weakest entry */
			idx = 0;
			for (unsigned i = 1; i < HEARD_MOST; i++) {
				if (c.heard[i].rssi < c.heard[idx].rssi)
					idx = (int)i;
			}
		}
		bzero(&c.heard[idx], sizeof(c.heard[idx]));
	}

	struct hakios_sta_bss *hb = &c.heard[idx];
	struct lx_bss_slot *slot = &c.slots[idx];

	memcpy(hb->bssid, mgmt->bssid, ETH_ALEN);
	if (ssid_ie != NULL && ssid_ie[1] > 0 && ssid_ie[1] <= sizeof(hb->ssid) && ssid_ie[2] != 0) {
		memcpy(hb->ssid, ssid_ie + 2, ssid_ie[1]);
		hb->ssid_length = ssid_ie[1];
	} else if (ssid_ie != NULL && ssid_ie[1] == 0 && hb->ssid_length == 0) {
		hb->ssid_length = 0;
	}
	hb->frequency = channel != NULL ? (uint16_t)channel->center_freq : hb->frequency;
	hb->rssi = (int16_t)(signal_mbm / 100);
	hb->beacon_interval = interval;
	hb->capability = capability;
	hb->elements_length = (uint16_t)kept;
	memcpy(hb->elements, ie, kept);
	c.heard_at[idx] = lx_os_nanoseconds();

	bzero(slot, sizeof(*slot));
	memcpy(slot->bss.bssid, mgmt->bssid, ETH_ALEN);
	slot->bss.channel = channel;
	slot->bss.capability = capability;
	slot->bss.beacon_interval = interval;
	slot->bss.signal = signal_mbm;
	slot->u.ies.len = kept;
	memcpy((void *)(uintptr_t)slot->u.ies.data, ie, kept);
	slot->bss.ies = &slot->u.ies;
	slot->bss.beacon_ies = &slot->u.ies;
	lx_os_unlock();

	return (&slot->bss);
}

struct cfg80211_bss *
lx_cfg80211_get_bss(struct wiphy *wiphy, struct linuxkpi_ieee80211_channel *channel,
    const uint8_t *bssid, const uint8_t *ssid, size_t ssid_len, int bss_type, int privacy)
{
	int best = -1;

	(void)wiphy;
	(void)bss_type;
	(void)privacy;
	if (c.heard == NULL || c.slots == NULL)
		return (NULL);

	lx_os_lock();
	for (unsigned i = 0; i < c.heard_count; i++) {
		const struct hakios_sta_bss *hb = &c.heard[i];

		if (channel != NULL && hb->frequency != (uint16_t)channel->center_freq)
			continue;
		if (bssid != NULL && !is_zero_ether_addr(bssid) && !is_broadcast_ether_addr(bssid) &&
		    memcmp(hb->bssid, bssid, ETH_ALEN) != 0)
			continue;
		if (ssid != NULL && ssid_len > 0 &&
		    (hb->ssid_length != ssid_len || memcmp(hb->ssid, ssid, ssid_len) != 0))
			continue;
		if (best < 0 || hb->rssi > c.heard[best].rssi)
			best = (int)i;
	}
	struct cfg80211_bss *found = best >= 0 ? &c.slots[best].bss : NULL;
	lx_os_unlock();
	return (found);
}

void
lx_cfg80211_scan_done(struct cfg80211_scan_request *request, struct cfg80211_scan_info *info)
{
	(void)request;
	lx_os_lock();
	c.scan_aborted = info != NULL && info->aborted;
	c.scan_over = 1;
	lx_os_wakeup(&c.scan_over);
	lx_os_unlock();
}

static int
station_scan(const struct hakios_sta_scan *what)
{
	static struct cfg80211_ssid ssid;
	struct cfg80211_scan_request *request;
	unsigned count = 0;
	int error;

	if (c.wiphy == NULL || c.ndev == NULL || !c.up)
		return (ENETDOWN);

	ensure_bss_tables();
	request = kzalloc(sizeof(*request) + 64 * sizeof(request->channels[0]), GFP_KERNEL);
	if (request == NULL)
		return (ENOMEM);

	if (what->frequency_count > 0) {
		unsigned n = what->frequency_count <= 64 ? what->frequency_count : 64;
		for (unsigned i = 0; i < n; i++) {
			struct linuxkpi_ieee80211_channel *ch = ieee80211_get_channel(c.wiphy, what->frequencies[i]);
			if (ch != NULL && !(ch->flags & IEEE80211_CHAN_DISABLED))
				request->channels[count++] = ch;
		}
	} else {
		/* Populate all enabled 2.4 GHz + 5 GHz channels (slsi_scan iterates request->n_channels) */
		for (int b = 0; b < NUM_NL80211_BANDS; b++) {
			struct ieee80211_supported_band *band = c.wiphy->bands[b];
			for (int i = 0; band != NULL && i < band->n_channels && count < 64; i++) {
				if (!(band->channels[i].flags & IEEE80211_CHAN_DISABLED))
					request->channels[count++] = &band->channels[i];
			}
		}
	}

	request->n_channels = (uint32_t)count;
	request->wiphy = c.wiphy;
	request->wdev = c.ndev->ieee80211_ptr;
	if (!what->passive) {
		bzero(&ssid, sizeof(ssid));
		ssid.ssid_len = what->ssid_length <= sizeof(ssid.ssid) ? what->ssid_length : 0;
		memcpy(ssid.ssid, what->ssid, (size_t)ssid.ssid_len);
		request->ssids = &ssid;
		request->n_ssids = 1;
	}

	lx_os_lock();
	c.scan_over = 0;
	c.heard_count = 0;
	lx_os_unlock();

	error = slsi_scan(c.wiphy, request);
	if (error != 0) {
		lx_os_lock();
		c.scan_over = 1;
		lx_os_unlock();
		kfree(request);
		return (error < 0 ? -error : error);
	}

	lx_os_lock();
	for (unsigned long long until = lx_os_nanoseconds() + 15000000000ULL;
	    !c.scan_over && lx_os_nanoseconds() < until;) {
		lx_os_sleep(&c.scan_over, 100000000ULL);
	}
	error = !c.scan_over ? ETIMEDOUT : c.scan_aborted ? EIO : 0;
	lx_os_unlock();
	if (error != ETIMEDOUT)
		kfree(request);
	return (error);
}

/* ---- interface up / down (slsi_net_open boots Maxwell + HIP4 + WLAN service!) ------------------- */

int
lx_cfg_open_wlan(void)
{
	int error;

	if (c.up)
		return (0);
	if (c.ndev == NULL || c.wiphy == NULL)
		return (ENXIO);

	mutex_init(&c.send);
	ensure_bss_tables();
	error = c.ndev->netdev_ops->ndo_open(c.ndev);
	if (error != 0) {
		/* Step 2 and Step 3 intentionally stop inside mif_reset with -ECANCELED */
		if (error == -ECANCELED && lx_scsc_steps() < 4)
			return (0);
		lx_tell("scsc: wlan0 open failed: %d\n", error);
		return (error < 0 ? -error : error);
	}
	c.ndev->flags |= IFF_UP;
	c.up = true;
	lx_tell("scsc: 4: %s up (MAC %02x:%02x:%02x:..:..:..)\n",
	    c.ndev->name, c.ndev->dev_addr[0], c.ndev->dev_addr[1], c.ndev->dev_addr[2]);
	return (0);
}

static void
station_leave(void)
{
	if (!c.joined && !c.joining)
		return;
	lx_os_lock();
	c.leaving = true;
	c.connected = 0;
	lx_os_unlock();

	if (c.wiphy != NULL && c.ndev != NULL)
		slsi_disconnect(c.wiphy, c.ndev, WLAN_REASON_DEAUTH_LEAVING);

	lx_os_lock();
	if (c.joined) {
		c.joined = false;
		lx_os_unlock();
		lx_os_net_link(0);
		lx_os_lock();
	}
	c.joining = false;
	c.leaving = false;
	lx_os_unlock();
	lx_tell("scsc: left network (%llu tx, %llu rx)\n", c.sent, c.received);
}

static void
station_down(void)
{
	station_leave();
	if (c.up && c.ndev != NULL) {
		c.ndev->netdev_ops->ndo_stop(c.ndev);
		c.ndev->flags &= ~IFF_UP;
		lx_tell("scsc: %s down\n", c.ndev->name);
	}
	c.up = false;
}

/* ---- association & WPA2-PSK 4-way handshake ----------------------------------------------------- */

void
lx_cfg80211_connect_bss(struct net_device *ndev, const uint8_t *bssid, struct cfg80211_bss *bss,
    const uint8_t *req_ie, size_t req_ie_len, const uint8_t *resp_ie, size_t resp_ie_len,
    int status, gfp_t gfp, int timeout_reason)
{
	(void)ndev;
	(void)bssid;
	(void)bss;
	(void)req_ie;
	(void)req_ie_len;
	(void)resp_ie;
	(void)resp_ie_len;
	(void)gfp;
	(void)timeout_reason;

	lx_os_lock();
	c.status = status;
	c.connected = status == 0 ? 1 : -1;
	lx_os_wakeup(&c.connected);
	lx_os_unlock();
}

void
lx_cfg80211_disconnected(struct net_device *ndev, uint16_t reason, bool locally_generated)
{
	bool lost;

	(void)ndev;
	lx_os_lock();
	lost = c.joined && !c.leaving;
	if (c.joined && lost)
		c.joined = false;
	c.connected = -1;
	c.last_error = lost ? ECONNRESET : c.last_error;
	lx_os_wakeup(&c.connected);
	lx_os_wakeup(&c.handshake);
	lx_os_unlock();

	lx_tell("scsc: disconnected (reason %u%s)%s\n",
	    reason, locally_generated ? ", local" : "", lost ? ": LOST" : "");
	if (lost) {
		lx_os_net_link(0);
		lx_os_station_lost(ECONNRESET);
	}
}

void
lx_cfg80211_roamed(struct net_device *ndev, struct cfg80211_roam_info *info)
{
	(void)ndev;
	(void)info;
	lx_tell("scsc: firmware roamed notification\n");
}

static int
keys_give(bool pairwise)
{
	struct key_params key;
	int error = 0;

	if (pairwise) {
		bzero(&key, sizeof(key));
		key.key = c.wpa.pairwise;
		key.key_len = 16;
		key.cipher = WLAN_CIPHER_SUITE_CCMP;
		error = slsi_add_key(c.wiphy, c.ndev, 0, true, c.target.bssid, &key);
	}
	if (error == 0) {
		bzero(&key, sizeof(key));
		key.key = c.wpa.group;
		key.key_len = (int)c.wpa.group_length;
		key.seq = c.wpa.group_counter;
		key.seq_len = 6;
		key.cipher = c.group == LX_RSN_TKIP ? WLAN_CIPHER_SUITE_TKIP : WLAN_CIPHER_SUITE_CCMP;
		error = slsi_add_key(c.wiphy, c.ndev, (uint8_t)c.wpa.group_index, false, NULL, &key);
	}
	lx_tell("scsc: WPA2 %s installed: %d\n", pairwise ? "PTK+GTK" : "GTK", error);
	return (error);
}

static void
handshake_message(const uint8_t *frame, unsigned length)
{
	uint8_t reply[14 + LX_WPA_REPLY_MOST];
	unsigned reply_length = 0;
	const char *why = "";
	int said, error = 0;

	c.eapol_count++;
	if (!c.protected || (!c.joining && !c.joined) || memcmp(frame + 6, c.target.bssid, ETH_ALEN) != 0)
		return;

	said = lx_wpa_message(&c.wpa, frame + 14, length - 14, reply + 14, &reply_length, &why);
	if (said == LX_WPA_KEYS || said == LX_WPA_GROUP) {
		/* Install PTK+GTK first, then transmit M4 via MLME_SEND_FRAME_REQ (unencrypted M4 path in slsi_tx_eapol) */
		error = keys_give(said == LX_WPA_KEYS);
	}
	if (reply_length != 0) {
		memcpy(reply, c.target.bssid, ETH_ALEN);
		memcpy(reply + 6, c.wpa.own, ETH_ALEN);
		reply[12] = 0x88;
		reply[13] = 0x8e;
		frame_send(reply, 14 + reply_length);
	}
	lx_tell("scsc: EAPOL (%u B): %s%s\n", length - 14,
	    said == LX_WPA_ANSWER ? "M1 -> sent M2" :
	    said == LX_WPA_KEYS ? "M3 verified -> keys installed + sent M4" :
	    said == LX_WPA_GROUP ? "group rekey -> GTK installed + replied" :
	    said == LX_WPA_BAD ? "BAD: " : "ignored", why);

	if (said != LX_WPA_NOTHING) {
		lx_os_lock();
		if (c.handshake != LX_WPA_KEYS) {
			c.handshake = said;
			c.why = why;
		}
		if (said == LX_WPA_KEYS)
			c.keys = error == 0 ? 1 : -1;
		lx_os_wakeup(&c.handshake);
		lx_os_unlock();
	}
}

static void
eapol_runs(struct work_struct *work)
{
	static struct lx_eapol e;

	(void)work;
	for (;;) {
		lx_os_lock();
		if (c.eapol_out == c.eapol_in) {
			lx_os_unlock();
			return;
		}
		e = c.eapol[c.eapol_out % EAPOL_MOST];
		c.eapol_out++;
		lx_os_unlock();
		handshake_message(e.bytes, e.length);
	}
}

static bool
target_find(const struct hakios_sta_join *what)
{
	static const uint8_t anyone[ETH_ALEN];
	int best = -1;

	lx_os_lock();
	for (unsigned i = 0; i < c.heard_count; i++) {
		const struct hakios_sta_bss *bss = &c.heard[i];

		if (bss->ssid_length == what->ssid_length &&
		    memcmp(bss->ssid, what->ssid, what->ssid_length) == 0 &&
		    (memcmp(what->bssid, anyone, ETH_ALEN) == 0 || memcmp(what->bssid, bss->bssid, ETH_ALEN) == 0) &&
		    (best < 0 || bss->rssi > c.heard[best].rssi)) {
			best = (int)i;
		}
	}
	if (best >= 0)
		c.target = c.heard[best];
	lx_os_unlock();
	return (best >= 0);
}

static int
wait_for(volatile int *what, bool (*done)(void), unsigned ms)
{
	unsigned long long until = lx_os_nanoseconds() + (unsigned long long)ms * 1000000ULL;

	lx_os_lock();
	while (!done() && lx_os_nanoseconds() < until) {
		lx_os_sleep((const void *)what, 50000000ULL);
	}
	lx_os_unlock();
	return (done() ? 0 : ETIMEDOUT);
}

static bool connect_over(void) { return (c.connected != 0); }
static bool handshake_over(void) { return (c.connected < 0 || (c.handshake == LX_WPA_KEYS && c.keys != 0)); }

static int
station_join(const struct hakios_sta_join *what)
{
	static uint32_t pairwise = WLAN_CIPHER_SUITE_CCMP, akm = WLAN_AKM_SUITE_PSK;
	struct cfg80211_connect_params sme;
	unsigned long long began;
	const char *refused;
	int error, kind;

	if (!lx_scsc_reached(8)) {
		lx_scsc_may(8, "joining a Wi-Fi network");
		return (EPERM);
	}
	if ((error = lx_cfg_open_wlan()) != 0)
		return (error);
	station_leave();

	if (what->ssid_length == 0 || what->ssid_length > 32)
		return (EINVAL);

	if (!target_find(what)) {
		struct hakios_sta_scan scan;

		bzero(&scan, sizeof(scan));
		memcpy(scan.ssid, what->ssid, what->ssid_length);
		scan.ssid_length = what->ssid_length;
		if ((error = station_scan(&scan)) != 0 && error != EIO)
			return (error);
		if (!target_find(what)) {
			lx_tell("scsc: 8: target SSID (%u B) not found in scan\n", what->ssid_length);
			return (ENOENT);
		}
	}

	kind = lx_rsn_kind(c.target.elements, c.target.elements_length, c.target.capability);
	c.protected = kind != LX_RSN_OPEN;
	refused = kind == LX_RSN_OLDER ? "legacy WEP/WPA1 not supported" : NULL;
	if (refused == NULL && c.protected)
		refused = lx_rsn_choose(c.target.elements, c.target.elements_length, c.rsn, &c.group);
	if (refused == NULL && c.protected && what->security != HAKIOS_STA_WPA2_PSK)
		refused = "network is WPA2-protected and no passphrase was supplied";
	if (refused != NULL) {
		lx_tell("scsc: 8: cannot join %u MHz BSS: %s\n", c.target.frequency, refused);
		return (EOPNOTSUPP);
	}

	lx_tell("scsc: 8: joining (%u B SSID) via %02x:%02x:%02x:..:..:.. on %u MHz (%d dBm, %s)\n",
	    what->ssid_length, c.target.bssid[0], c.target.bssid[1], c.target.bssid[2],
	    c.target.frequency, c.target.rssi, !c.protected ? "open" : "WPA2-PSK");

	if (c.protected) {
		bzero(&c.wpa, sizeof(c.wpa));
		if (what->passphrase_length != 0) {
			lx_wpa_pmk(what->passphrase, what->passphrase_length, what->ssid, what->ssid_length, c.wpa.pmk);
		} else {
			memcpy(c.wpa.pmk, what->pmk, sizeof(c.wpa.pmk));
		}
		memcpy(c.wpa.own, c.ndev->dev_addr, ETH_ALEN);
		memcpy(c.wpa.peer, c.target.bssid, ETH_ALEN);
		lx_os_random(c.wpa.own_nonce, sizeof(c.wpa.own_nonce));
		c.wpa.rsn = c.rsn;
		c.wpa.rsn_length = LX_RSN_OURS_LENGTH;
	}

	lx_os_lock();
	c.handshake = LX_WPA_NOTHING;
	c.why = "";
	c.keys = 0;
	c.connected = 0;
	c.eapol_out = c.eapol_in;
	c.joining = true;
	lx_os_unlock();

	bzero(&sme, sizeof(sme));
	sme.channel = ieee80211_get_channel(c.wiphy, c.target.frequency);
	sme.bssid = c.target.bssid;
	sme.ssid = c.target.ssid;
	sme.ssid_len = c.target.ssid_length;
	sme.auth_type = NL80211_AUTHTYPE_OPEN_SYSTEM;
	sme.privacy = c.protected;
	if (c.protected) {
		sme.ie = c.rsn;
		sme.ie_len = LX_RSN_OURS_LENGTH;
		sme.crypto.wpa_versions = NL80211_WPA_VERSION_2;
		sme.crypto.cipher_group = c.group == LX_RSN_TKIP ? WLAN_CIPHER_SUITE_TKIP : WLAN_CIPHER_SUITE_CCMP;
		sme.crypto.ciphers_pairwise[0] = pairwise;
		sme.crypto.n_ciphers_pairwise = 1;
		sme.crypto.akm_suites[0] = akm;
		sme.crypto.n_akm_suites = 1;
	}

	began = lx_os_nanoseconds();
	error = slsi_connect(c.wiphy, c.ndev, &sme);
	if (error != 0) {
		lx_tell("scsc: slsi_connect failed: %d\n", error);
		c.joining = false;
		return (error < 0 ? -error : error);
	}
	if (wait_for(&c.connected, connect_over, 15000) != 0 || c.connected < 0) {
		lx_tell("scsc: association failed (status %d) after %llu ms\n",
		    c.status, (lx_os_nanoseconds() - began) / 1000000ULL);
		station_leave();
		return (c.connected == 0 ? ETIMEDOUT : ECONNREFUSED);
	}
	if (c.protected) {
		if (wait_for(&c.handshake, handshake_over, 10000) != 0 || c.connected < 0 || c.keys != 1) {
			lx_tell("scsc: WPA2 handshake failed (%s; %llu eapol)\n", c.why, c.eapol_count);
			station_leave();
			return (c.keys < 0 ? EIO : ETIMEDOUT);
		}
		/* Wait up to 500 ms for M4 MLME_FRAME_TRANSMISSION_IND to open slsi FCQ 802.1X port */
		for (unsigned ms = 0; ms < 100 && !lx_slsi_sta_port_is_open(c.ndev); ms++) {
			lx_pause_ms(5);
		}
	}

	lx_os_lock();
	c.joining = false;
	c.joined = true;
	lx_os_unlock();
	lx_os_net_link(1);
	lx_tell("scsc: 8: NETWORK JOINED in %llu ms; en0 link UP\n",
	    (lx_os_nanoseconds() - began) / 1000000ULL);
	return (0);
}

/* ---- hakios_sta_ops callbacks ------------------------------------------------------------------- */

static int
boundary_power(bool on)
{
	if (on)
		return (lx_cfg_open_wlan());
	station_down();
	return (0);
}

static int
boundary_scan(const struct hakios_sta_scan *what)
{
	static unsigned said;
	unsigned long long began = lx_os_nanoseconds();
	int error;

	if ((error = lx_cfg_open_wlan()) != 0)
		return (error);
	error = station_scan(what);
	if (said < 24) {
		said++;
		lx_tell("scsc: scan (%u ch, %s): err=%d, %u APs in %llu ms\n",
		    what->frequency_count, what->passive ? "passive" : "active",
		    error, c.heard_count, (lx_os_nanoseconds() - began) / 1000000ULL);
	}
	return (error);
}

static void
boundary_heard(void (*each)(const struct hakios_sta_bss *, void *), void *context)
{
	static struct hakios_sta_bss bss;

	for (unsigned i = 0; i < c.heard_count; i++) {
		lx_os_lock();
		bss = c.heard[i];
		bss.age_ms = (uint32_t)((lx_os_nanoseconds() - c.heard_at[i]) / 1000000ULL);
		lx_os_unlock();
		each(&bss, context);
	}
}

static int
boundary_join(const struct hakios_sta_join *what)
{
	int error = station_join(what);

	c.last_error = error;
	return (error);
}

static int
boundary_leave(void)
{
	station_leave();
	c.last_error = 0;
	return (0);
}

static void
boundary_state(struct hakios_sta_state *state)
{
	bzero(state, sizeof(*state));
	if (!c.offered)
		return;
	lx_os_lock();
	state->phase = !c.up ? HAKIOS_STA_OFF :
	    c.joined ? HAKIOS_STA_JOINED :
	    c.joining ? HAKIOS_STA_JOINING :
	    c.scan_over == 0 ? HAKIOS_STA_SCANNING : HAKIOS_STA_IDLE;
	state->last_error = c.last_error;
	if (c.joined || c.joining)
		state->bss = c.target;
	lx_os_unlock();
}

static int
boundary_link_quality(struct hakios_sta_quality *quality)
{
	static struct station_info sinfo;
	int error;

	if (!c.joined || c.ndev == NULL || c.wiphy == NULL)
		return (ENOTCONN);
	bzero(&sinfo, sizeof(sinfo));
	error = slsi_get_station(c.wiphy, c.ndev, c.target.bssid, &sinfo);
	if (error != 0 || !(sinfo.filled & BIT_ULL(NL80211_STA_INFO_SIGNAL)) || sinfo.signal >= 0)
		return (error < 0 ? -error : EIO);
	lx_os_lock();
	if (c.joined)
		c.target.rssi = sinfo.signal;
	lx_os_unlock();
	quality->rssi = sinfo.signal;
	quality->tx_rate = (sinfo.filled & BIT_ULL(NL80211_STA_INFO_TX_BITRATE)) ? (uint16_t)(sinfo.txrate.legacy / 10) : 0;
	quality->rx_rate = (sinfo.filled & BIT_ULL(NL80211_STA_INFO_RX_BITRATE)) ? (uint16_t)(sinfo.rxrate.legacy / 10) : 0;
	return (0);
}

static unsigned
boundary_channels(struct hakios_sta_channel *channels, unsigned most)
{
	unsigned count = 0;

	for (unsigned band = 0; c.wiphy != NULL && band < NUM_NL80211_BANDS; band++) {
		const struct ieee80211_supported_band *has = c.wiphy->bands[band];

		for (int i = 0; has != NULL && i < has->n_channels && count < most; i++) {
			if (!(has->channels[i].flags & IEEE80211_CHAN_DISABLED)) {
				channels[count].frequency = (uint16_t)has->channels[i].center_freq;
				channels[count++].flags = (uint16_t)(
				    ((has->channels[i].flags & IEEE80211_CHAN_NO_IR) ? HAKIOS_STA_CHANNEL_PASSIVE : 0) |
				    ((has->channels[i].flags & IEEE80211_CHAN_RADAR) ? HAKIOS_STA_CHANNEL_RADAR : 0));
			}
		}
	}
	return (count);
}

static struct hakios_sta_ops boundary = {
	.power = boundary_power,
	.scan = boundary_scan,
	.heard = boundary_heard,
	.join = boundary_join,
	.leave = boundary_leave,
	.state = boundary_state,
	.channels = boundary_channels,
	.link_quality = boundary_link_quality,
};

/* ---- Station attachment (scsc_wlan >= 7) -------------------------------------------------------- */

static void
autojoin_runs(struct work_struct *work)
{
	const struct linuxkpi_firmware *fw = NULL;
	struct hakios_sta_scan scan;
	struct hakios_sta_join j;
	const uint8_t *p, *end;
	int err;

	(void)work;
	if (!lx_scsc_reached(8) || c.joined)
		return;
	if (linuxkpi_firmware_request_nowarn(&fw, "networks", NULL) != 0 || fw == NULL || fw->size == 0)
		return;

	bzero(&scan, sizeof(scan));
	(void)station_scan(&scan);

	p = fw->data;
	end = fw->data + fw->size;
	while (p < end && !c.joined) {
		const uint8_t *nl = p, *tab = NULL;
		while (nl < end && *nl != '\n' && *nl != '\r') {
			if (*nl == '\t' && tab == NULL)
				tab = nl;
			nl++;
		}
		if (tab != NULL && tab > p && (size_t)(tab - p) <= 32) {
			size_t slen = (size_t)(tab - p);
			size_t plen = (size_t)(nl - (tab + 1));
			if (plen <= 63) {
				bzero(&j, sizeof(j));
				memcpy(j.ssid, p, slen);
				j.ssid_length = (unsigned)slen;
				if (plen >= 8) {
					j.security = HAKIOS_STA_WPA2_PSK;
					memcpy(j.passphrase, tab + 1, plen);
					j.passphrase_length = (unsigned)plen;
				}
				if (target_find(&j)) {
					lx_tell("scsc: 8: auto-joining saved network (%u B SSID)\n", j.ssid_length);
					err = station_join(&j);
					if (err == 0) {
						lx_tell("scsc: 8: auto-join succeeded\n");
						break;
					}
					lx_tell("scsc: 8: auto-join failed: %d\n", err);
				}
			}
		}
		p = nl;
		while (p < end && (*p == '\n' || *p == '\r'))
			p++;
	}
	release_firmware(fw);
}

void
lx_cfg_begin(void)
{
	if (!lx_scsc_may(7, "attaching en0 and offering hakios_sta_ops station"))
		return;

	ensure_bss_tables();
	if (c.eapol == NULL)
		c.eapol = kzalloc(EAPOL_MOST * sizeof(*c.eapol), GFP_KERNEL);
	if (c.work == NULL) {
		c.work = alloc_workqueue("scsc-eapol", 0, 1);
		INIT_WORK(&c.eapol_work, eapol_runs);
	}
	if (!c.heard || !c.slots || !c.heard_at || !c.eapol || !c.work || !c.ndev || !c.wiphy) {
		lx_tell("scsc: 7: station memory or wlan0 unavailable\n");
		return;
	}
	c.scan_over = 1;
	lx_os_net_attach(c.ndev->dev_addr);
	memcpy(boundary.address, c.ndev->dev_addr, ETH_ALEN);
	c.offered = true;
	lx_os_station(&boundary);
	lx_tell("scsc: 7: station attached on en0 (%02x:%02x:%02x:..:..:..)%s\n",
	    c.ndev->dev_addr[0], c.ndev->dev_addr[1], c.ndev->dev_addr[2],
	    lx_scsc_reached(8) ? ", scan + WPA2-PSK join enabled" : ", scan only (scsc_wlan=8 enables join)");
}

void
lx_cfg_stop(void)
{
	if (c.offered) {
		lx_os_station(NULL);
		c.offered = false;
	}
	station_down();
}

void
lx_cfg_say_state(void)
{
	lx_tell("  sta: dev=%s up=%d joined=%d heard=%u tx=%llu rx=%llu eapol=%llu\n",
	    c.ndev ? c.ndev->name : "none", c.up, c.joined, c.heard_count, c.sent, c.received, c.eapol_count);
}
