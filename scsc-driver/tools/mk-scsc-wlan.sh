#!/bin/bash
# Build the Samsung Exynos 850 (S5E3830, Galaxy A21s SM-A217F) SCSC WLBT Wi-Fi driver
# (119c0000.scsc_wifibt: Maxwell S612 mx140 + slsi_wlan) on Scion's FreeBSD 15.1 LinuxKPI + wifi/lx
# layer, modeled directly after tools/mk-brcmfmac.sh.
#
# Output:
#   xnu-deps/build/scsc/out/scsc_wlan.o   relocatable arm64 kernel object exporting
#                                         _lx_prepare, _lx_start, _lx_stop, _lx_transmit
#   xnu-deps/build/scsc/out/stubs.txt     unbuilt debug/procfs/test driver symbols stubbed to _lx_stub
set -e

FREEBSD=15.1
FREEBSD_SHA256=cf5762da53fd52e1eaf0f9ceee9bf58cbe314c821031d0d9ffa76823185a89e1

ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
REFS="$ROOT/../refs/exynos850-scsc"
B="$ROOT/xnu-deps/build/scsc"
W="$ROOT/wifi/lx"

XCODE_BIN="/Applications/Xcode_27.2.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"
CLANG="${XCODE_BIN}/clang"
LD_BIN="${XCODE_BIN}/ld"
NM_BIN="${XCODE_BIN}/llvm-nm"
SIZE_BIN="${XCODE_BIN}/llvm-size"
[ -x "$CLANG" ] || CLANG=$(xcrun -f clang)
[ -x "$LD_BIN" ] || LD_BIN=$(xcrun -f ld)
[ -x "$NM_BIN" ] || NM_BIN=$(command -v llvm-nm || xcrun -f llvm-nm)
[ -x "$SIZE_BIN" ] || SIZE_BIN=$(command -v llvm-size || xcrun -f llvm-size)

[ -d "$REFS/drivers/misc/samsung/scsc" ] || { echo "mk-scsc-wlan.sh: missing $REFS" >&2; exit 1; }

mkdir -p "$B"
cd "$B"

sha256_check() {
	local expected=$1 file=$2 actual
	actual=$(shasum -a 256 "$file" | awk '{print $1}')
	[ "$actual" = "$expected" ] || { echo "mk-scsc-wlan.sh: sha256 mismatch on $file ($actual != $expected)" >&2; exit 1; }
}

archive() {
	local file=$1 url=$2 sha=$3
	if [ ! -f "$file" ]; then
		for cand in "$ROOT/xnu-deps/build/brcmfmac/$file" "$ROOT/xnu-deps/build/ath11k/$file"; do
			if [ -f "$cand" ]; then
				ln "$cand" "$file" 2>/dev/null || cp "$cand" "$file"
				break
			fi
		done
	fi
	[ -f "$file" ] || curl -sL --fail -o "$file" "$url"
	sha256_check "$sha" "$file"
}

# --- 1. Copy Exynos 850 SCSC WLBT GPL sources & apply clean surgical LinuxKPI adaptations
SCSC_STAMP="scsc-lx-v4"
if [ "$(cat src/.release 2>/dev/null)" != "$SCSC_STAMP" ]; then
	rm -rf src
	mkdir -p src/drivers/misc/samsung src/drivers/net/wireless src/include
	cp -R "$REFS/drivers/misc/samsung/scsc" src/drivers/misc/samsung/scsc
	cp -R "$REFS/drivers/net/wireless/scsc" src/drivers/net/wireless/scsc
	cp -R "$REFS/include/scsc" src/include/scsc

	python3 - <<'PY'
import pathlib, re

root = pathlib.Path("src")

# 1) scsc_mx_module.c: expose lx_scsc_mx_module_init / exit instead of module_init / exit
p = root / "drivers/misc/samsung/scsc/scsc_mx_module.c"
s = p.read_text()
s = s.replace("static int __init scsc_mx_module_init(void)", "int lx_scsc_mx_module_init(void)")
s = s.replace("static void __exit scsc_mx_module_exit(void)", "void lx_scsc_mx_module_exit(void)")
s = re.sub(r"^module_init\(scsc_mx_module_init\);\s*$", "", s, flags=re.M)
s = re.sub(r"^module_exit\(scsc_mx_module_exit\);\s*$", "", s, flags=re.M)
p.write_text(s)

# 2) dev.c: expose lx_slsi_wlan_driver_init / exit + stub Linux in_ifaddr/inet6_ifaddr notifier bodies
p = root / "drivers/net/wireless/scsc/dev.c"
s = p.read_text()
s = re.sub(r"^module_init\(slsi_dev_load\);\s*$",
           "int lx_slsi_wlan_driver_init(void) { return slsi_dev_load(); }", s, flags=re.M)
s = re.sub(r"^module_exit\(slsi_dev_unload\);\s*$",
           "void lx_slsi_wlan_driver_exit(void) { slsi_dev_unload(); }", s, flags=re.M)
s = re.sub(
    r"static int slsi_dev_inetaddr_changed\(struct notifier_block \*nb, unsigned long data, void \*arg\)\s*\{.*?\n\}",
    "static int slsi_dev_inetaddr_changed(struct notifier_block *nb, unsigned long data, void *arg)\n"
    "{\n\t(void)nb; (void)data; (void)arg;\n\treturn NOTIFY_DONE;\n}",
    s, count=1, flags=re.S
)
s = re.sub(
    r"static int slsi_dev_inet6addr_changed\(struct notifier_block \*nb, unsigned long data, void \*arg\)\s*\{.*?\n\}",
    "static int slsi_dev_inet6addr_changed(struct notifier_block *nb, unsigned long data, void *arg)\n"
    "{\n\t(void)nb; (void)data; (void)arg;\n\treturn NOTIFY_DONE;\n}",
    s, count=1, flags=re.S
)
p.write_text(s)

# 3) mx140_file.c: default force_flat = true so flat mx140_*.hcf files are requested directly
p = root / "drivers/misc/samsung/scsc/mx140_file.c"
s = p.read_text()
s = s.replace("static bool force_flat = false;", "static bool force_flat = true;")
p.write_text(s)

# 4) cm_if.c: replace floating-point literals (1.5 / 2.5 * 1024 * 1024) for -mgeneral-regs-only
p = root / "drivers/net/wireless/scsc/cm_if.c"
s = p.read_text()
s = s.replace("1.5 * 1024 * 1024", "(3 * 1024 * 1024 / 2)")
s = s.replace("2.5 * 1024 * 1024", "(5 * 1024 * 1024 / 2)")
p.write_text(s)

# 5) netif.c: disable tcp_ack_suppression, clean UDP header check, guard MHS_DUALSTA, and bypass Linux Qdisc walk
p = root / "drivers/net/wireless/scsc/netif.c"
s = p.read_text()
s = s.replace("#ifndef CONFIG_ARM", "#if 0 /* disabled on LinuxKPI: no tcp_sk() */")
s = re.sub(
    r"static inline bool slsi_netif_is_udp_pkt\(struct sk_buff \*skb\)\s*\{.*?\n\}",
    "#ifndef SLSI_IS_VIF_INDEX_MHS_DUALSTA\n"
    "#define SLSI_IS_VIF_INDEX_MHS_DUALSTA(sdev, ndev_vif) (false)\n"
    "#endif\n"
    "static inline bool slsi_netif_is_udp_pkt(struct sk_buff *skb)\n"
    "{\n"
    "\tconst u8 *nh = skb_network_header(skb);\n"
    "\tif (!nh || nh < skb->head || nh + 20 > skb_tail_pointer(skb))\n"
    "\t\treturn false;\n"
    "\tif ((nh[0] >> 4) == 4)\n"
    "\t\treturn nh[9] == IPPROTO_UDP;\n"
    "\tif ((nh[0] >> 4) == 6 && nh + 40 <= skb_tail_pointer(skb))\n"
    "\t\treturn nh[6] == NEXTHDR_UDP;\n"
    "\treturn false;\n"
    "}",
    s, count=1, flags=re.S
)
s = s.replace(
    "\t/* Move packets from netdev queues */\n\tfor (i = 0; i < SLSI_NETIF_Q_PER_PEER; i++) {",
    "\tslsi_tx_unpause_queues(sdev);\n\tif (1) return;\n#if 0\n\tfor (i = 0; i < SLSI_NETIF_Q_PER_PEER; i++) {"
)
s = s.replace(
    "\t/* Resume the STA and TDLS netdev queues */\n\tslsi_tx_unpause_queues(sdev);\n}",
    "#endif\n\tslsi_tx_unpause_queues(sdev);\n}"
)
s = re.sub(
    r"static void slsi_netif_rps_map_clear\(struct net_device \*dev\)\s*\{.*?\n\}",
    "static void slsi_netif_rps_map_clear(struct net_device *dev) { (void)dev; }",
    s, count=1, flags=re.S
)
s = re.sub(
    r"static int slsi_netif_rps_map_set\(struct net_device \*dev, char \*buf, size_t len\)\s*\{.*?\n\}",
    "static int slsi_netif_rps_map_set(struct net_device *dev, char *buf, size_t len) { (void)dev; (void)buf; return (int)len; }",
    s, count=1, flags=re.S
)
p.write_text(s)

# 6) mgt.c: stub sysfs kobject file creation, expose lx_slsi_set_mac_override_str(), fix unguarded filepath
p = root / "drivers/net/wireless/scsc/mgt.c"
s = p.read_text()
s = s.replace("SLSI_UNUSED_PARAMETER(filepath);", "")
s = re.sub(r"void slsi_create_sysfs_macaddr\(void\)\s*\{",
           "void lx_slsi_set_mac_override_str(const char *m) { if (m) strlcpy(sysfs_mac_override, m, sizeof(sysfs_mac_override)); }\n"
           "void slsi_create_sysfs_macaddr(void)\n{\n\treturn;", s, count=1)
for fn in ["slsi_destroy_sysfs_macaddr", "slsi_create_sysfs_version_info", "slsi_destroy_sysfs_version_info",
           "slsi_create_sysfs_pm", "slsi_destroy_sysfs_pm", "slsi_create_sysfs_ant", "slsi_destroy_sysfs_ant"]:
    s = re.sub(rf"(void\s+{fn}\s*\(void\)\s*\{{)", r"\1 return;", s, count=1)
p.write_text(s)

# 7) cfg80211_ops.c: keep slsi_ops minimal so Linux 4.19 AP/P2P ops don't clash with FreeBSD LinuxKPI 6.x link_id pointers
p = root / "drivers/net/wireless/scsc/cfg80211_ops.c"
s = p.read_text()
s = re.sub(
    r"static struct cfg80211_ops slsi_ops = \{.*?\n\};",
    "static struct cfg80211_ops slsi_ops = {\n"
    "\t.scan = slsi_scan,\n"
    "\t.connect = slsi_connect,\n"
    "\t.disconnect = slsi_disconnect,\n"
    "\t.get_station = slsi_get_station,\n"
    "};",
    s, count=1, flags=re.S
)
p.write_text(s)

# 8) debug.h: FreeBSD LinuxKPI wiphy->dev is struct device *, so use wiphy_dev()
p = root / "drivers/net/wireless/scsc/debug.h"
s = p.read_text()
s = s.replace("&((sdev)->wiphy->dev)", "wiphy_dev((sdev)->wiphy)")
p.write_text(s)
PY

	for p in "$W"/scsc/patches/*.patch; do
		[ ! -f "$p" ] || patch -s --no-backup-if-mismatch -d src -p1 < "$p"
	done
	echo "$SCSC_STAMP" > src/.release
fi
[ "$1" = fetch ] && { echo "mk-scsc-wlan.sh: sources staged at $B/src"; exit 0; }

# --- 2. FreeBSD 15.1 LinuxKPI headers + patches + generated bus headers
FSYS="compat/linuxkpi sys arm64/include net net80211 netinet netinet6 netlink vm kern tools libkern ddb
	security fs/pseudofs crypto opencrypto geom bsm isa cam rpc xdr x86/include ofed/include
	dev/pci dev/ofw dev/fdt dev/acpica dev/usb dev/iicbus dev/backlight
	contrib/ck/include contrib/dev/acpica/include contrib/libnv"
PATCHES=$( (echo "$FSYS scsc-lkpi-v3"; cat "$W"/patches/*.patch 2>/dev/null) | shasum -a 256 | cut -d' ' -f1)
if [ "$(cat freebsd/.release 2>/dev/null)" != "$FREEBSD $FREEBSD_SHA256 $PATCHES" ]; then
	archive freebsd-$FREEBSD-src.txz https://download.freebsd.org/releases/amd64/$FREEBSD-RELEASE/src.txz $FREEBSD_SHA256
	rm -rf freebsd gen obj
	mkdir freebsd
	tar xJf freebsd-$FREEBSD-src.txz -C freebsd --strip-components=2 usr/src/COPYRIGHT \
		$(for d in $FSYS; do echo usr/src/sys/$d; done)
	for p in "$W"/patches/*.patch; do
		[ -f "$p" ] && patch -s --no-backup-if-mismatch -d freebsd -p1 < "$p"
	done
	python3 - <<'PY'
import pathlib

inc = pathlib.Path("freebsd/sys/compat/linuxkpi/common/include")

# 1) net/cfg80211.h: complete cfg80211 structs & enums referenced by slsi_wlan
p = inc / "net/cfg80211.h"
s = p.read_text()
s = s.replace(
    "enum ieee80211_privacy {\n\tIEEE80211_PRIVACY_ANY,\n};",
    "enum ieee80211_privacy {\n\tIEEE80211_PRIVACY_ON,\n\tIEEE80211_PRIVACY_OFF,\n\tIEEE80211_PRIVACY_ANY,\n};"
)
s = s.replace(
    "enum ieee80211_bss_type {\n\tIEEE80211_BSS_TYPE_ANY,\n};",
    "enum ieee80211_bss_type {\n\tIEEE80211_BSS_TYPE_ESS,\n\tIEEE80211_BSS_TYPE_PBSS,\n\tIEEE80211_BSS_TYPE_IBSS,\n\tIEEE80211_BSS_TYPE_MBSS,\n\tIEEE80211_BSS_TYPE_ANY,\n};\n"
    "enum nl80211_acl_policy {\n\tNL80211_ACL_POLICY_ACCEPT_UNLESS_LISTED,\n\tNL80211_ACL_POLICY_DENY_UNLESS_LISTED,\n};"
)
s = s.replace(
    "struct cfg80211_bss {\n\t\t/* XXX TODO */\n",
    "struct cfg80211_bss {\n"
    "\tuint8_t\t\t\t\tbssid[ETH_ALEN];\n"
    "\tuint16_t\t\t\tcapability;\n"
    "\tuint16_t\t\t\tbeacon_interval;\n"
    "\tstruct linuxkpi_ieee80211_channel *channel;\n"
    "\tuint8_t\t\t\t\tchains;\n"
    "\tint8_t\t\t\t\tchain_signal[4];\n"
)
s = s.replace(
    "struct vif_params {\n\t/* XXX TODO */\n\tuint8_t\t\t\tmacaddr[ETH_ALEN];\n};",
    "struct vif_params {\n\tuint8_t\t\t\tmacaddr[ETH_ALEN];\n\tint\t\t\tuse_4addr;\n};"
)
s = s.replace(
    "struct cfg80211_beacon_data {\n\t/* XXX TODO */\n",
    "struct cfg80211_beacon_data {\n"
    "\tconst uint8_t\t\t\t\t*beacon_ies;\n"
    "\tuint32_t\t\t\t\tbeacon_ies_len;\n"
    "\tconst uint8_t\t\t\t\t*probe_resp;\n"
    "\tuint32_t\t\t\t\tprobe_resp_len;\n"
)
s = s.replace(
    "struct cfg80211_ap_settings {\n\t/* XXX TODO */\n",
    "struct cfg80211_ap_settings {\n"
    "\tbool\t\t\t\t\tprivacy;\n"
    "\tconst struct ieee80211_ht_cap\t\t*ht_cap;\n"
    "\tconst struct ieee80211_vht_cap\t\t*vht_cap;\n"
)
s = s.replace(
    "struct cfg80211_mgmt_tx_params {\n\t/* XXX TODO */\n",
    "struct cfg80211_mgmt_tx_params {\n"
    "\tbool\t\t\t\t\toffchan;\n"
    "\tbool\t\t\t\t\tno_cck;\n"
    "\tbool\t\t\t\t\tdont_wait_for_ack;\n"
)
s = s.replace(
    "\t/* XXX TODO */\n\tconst struct cfg80211_pmsr_capabilities\t*pmsr_capa;",
    "\tint\t\t\t\t\tmax_num_csa_counters;\n"
    "\tint\t\t\t\t\tmax_acl_mac_addrs;\n"
    "\tconst void\t\t\t\t*privid;\n"
    "\tconst uint8_t\t\t\t\t*extended_capabilities;\n"
    "\tconst uint8_t\t\t\t\t*extended_capabilities_mask;\n"
    "\tuint8_t\t\t\t\t\textended_capabilities_len;\n"
    "\tconst struct cfg80211_pmsr_capabilities\t*pmsr_capa;"
)
s = s.replace(
    "struct wireless_dev {\n\t\t/* XXX TODO, like ic? */\n",
    "struct cfg80211_acl_data {\n"
    "\tenum nl80211_acl_policy acl_policy;\n"
    "\tint n_acl_entries;\n"
    "\tstruct mac_address mac_addrs[0];\n"
    "};\n"
    "struct cfg80211_csa_settings {\n"
    "\tstruct cfg80211_chan_def chandef;\n"
    "\tstruct cfg80211_beacon_data beacon_csa;\n"
    "\tstruct cfg80211_beacon_data beacon_after;\n"
    "\tconst uint16_t *counter_offsets_beacon;\n"
    "\tconst uint16_t *counter_offsets_presp;\n"
    "\tunsigned int n_counter_offsets_beacon;\n"
    "\tunsigned int n_counter_offsets_presp;\n"
    "\tbool radar_required;\n"
    "\tbool block_tx;\n"
    "\tuint8_t count;\n"
    "};\n"
    "struct wireless_dev {\n"
    "\tbool\t\t\t\t\tuse_4addr;\n"
)
p.write_text(s)

# 2) linux/nl80211.h: add missing enum values used by slsi_wlan
p = inc / "linux/nl80211.h"
s = p.read_text()
s = s.replace(
    "\tNL80211_HIDDEN_SSID_NOT_IN_USE,",
    "\tNL80211_HIDDEN_SSID_NOT_IN_USE,\n\tNL80211_HIDDEN_SSID_ZERO_LEN,\n\tNL80211_HIDDEN_SSID_ZERO_CONTENTS,"
)
s = s.replace(
    "\tNL80211_AUTHTYPE_AUTOMATIC,",
    "\tNL80211_AUTHTYPE_NETWORK_EAP,\n\tNL80211_AUTHTYPE_AUTOMATIC,"
)
s = s.replace(
    "\tNUM_NL80211_EXT_FEATURES",
    "\tNL80211_EXT_FEATURE_MGMT_TX_RANDOM_TA,\n"
    "\tNL80211_EXT_FEATURE_SCHED_SCAN_RELATIVE_RSSI,\n"
    "\tNUM_NL80211_EXT_FEATURES"
)
s = s.replace(
    "\tNL80211_STA_INFO_BSS_PARAM,",
    "\tNL80211_STA_INFO_BSS_PARAM,\n\tNL80211_STA_INFO_AIRTIME_LINK_METRIC,"
)
p.write_text(s)

# 3) linux/ieee80211.h: add vendor_ie fields and reassoc/assoc_resp/wme_action mgmt layouts
p = inc / "linux/ieee80211.h"
s = p.read_text()
s = s.replace(
    "struct ieee80211_vendor_ie {\n};",
    "struct ieee80211_vendor_ie {\n"
    "\tuint8_t element_id;\n"
    "\tuint8_t len;\n"
    "\tuint8_t oui[3];\n"
    "\tuint8_t oui_type;\n"
    "\tuint8_t oui_subtype;\n"
    "\tuint8_t version;\n"
    "\tuint8_t data[0];\n"
    "} __packed;"
)
s = s.replace(
    "\t\t} __packed assoc_req;",
    "\t\t} __packed assoc_req;\n"
    "\t\tstruct {\n"
    "\t\t\tuint16_t\tcapab_info;\n"
    "\t\t\tuint16_t\tstatus_code;\n"
    "\t\t\tuint16_t\taid;\n"
    "\t\t\tuint8_t\t\tvariable[0];\n"
    "\t\t} __packed assoc_resp, reassoc_resp;\n"
    "\t\tstruct {\n"
    "\t\t\tuint16_t\tcapab_info;\n"
    "\t\t\tuint16_t\tlisten_interval;\n"
    "\t\t\tuint8_t\t\tcurrent_ap[ETH_ALEN];\n"
    "\t\t\tuint8_t\t\tvariable[0];\n"
    "\t\t} __packed reassoc_req;\n"
    "\t\tstruct {\n"
    "\t\t\t__le16\t\treason_code;\n"
    "\t\t\tuint8_t\t\tvariable[0];\n"
    "\t\t} __packed deauth, disassoc;"
)
s = s.replace(
    "\t\t\t\t} __packed wnm_timing_msr;",
    "\t\t\t\t} __packed wnm_timing_msr;\n"
    "\t\t\t\tstruct {\n"
    "\t\t\t\t\tuint8_t action_code;\n"
    "\t\t\t\t\tuint8_t dialog_token;\n"
    "\t\t\t\t\tuint8_t status_code;\n"
    "\t\t\t\t\tuint8_t variable[0];\n"
    "\t\t\t\t} __packed wme_action;"
)
p.write_text(s)

# 4) linux/skbuff.h: alias queue_mapping to qmap in struct sk_buff
p = inc / "linux/skbuff.h"
s = p.read_text()
s = s.replace(
    "\tuint16_t\t\tqmap;\t\t/* queue mapping */",
    "\tunion { uint16_t qmap; uint16_t queue_mapping; };"
)
p.write_text(s)

# 5) linux/netdevice.h: add slsi_wlan fields to net_device_ops and net_device
p = inc / "linux/netdevice.h"
s = p.read_text()
s = s.replace(
    "struct net_device_ops {",
    "struct ifreq;\nstruct net_device_stats;\nstruct net_device_ops {"
)
s = s.replace(
    "\tvoid (*ndo_set_rx_mode)(struct net_device *);\n};",
    "\tvoid (*ndo_set_rx_mode)(struct net_device *);\n"
    "\tint (*ndo_do_ioctl)(struct net_device *, struct ifreq *, int);\n"
    "\tvoid (*ndo_tx_timeout)(struct net_device *);\n"
    "\tstruct net_device_stats *(*ndo_get_stats)(struct net_device *);\n"
    "\tuint16_t (*ndo_select_queue)(struct net_device *, struct sk_buff *, struct net_device *);\n"
    "\tnetdev_features_t (*ndo_fix_features)(struct net_device *, netdev_features_t);\n"
    "};"
)
s = s.replace(
    "\tint\tname_assign_type, needed_headroom;",
    "\tuint8_t\tperm_addr[ETH_ALEN];\n"
    "\tnetdev_features_t hw_features;\n"
    "\tunsigned long\tstate;\n"
    "\tint\twatchdog_timeo;\n"
    "\tint\tname_assign_type, needed_headroom, needed_tailroom;"
)
p.write_text(s)
PY
	echo "$FREEBSD $FREEBSD_SHA256 $PATCHES" > freebsd/.release
fi

S="$B/freebsd/sys"
if [ ! -d gen ]; then
	mkdir gen
	(
		cd gen
		ln -s "$S/arm64/include" machine
		for m in kern/device_if.m kern/bus_if.m dev/pci/pci_if.m dev/pci/pci_iov_if.m dev/pci/pcib_if.m \
			dev/iicbus/iicbus_if.m dev/iicbus/iicbb_if.m dev/backlight/backlight_if.m dev/usb/usb_if.m \
			dev/ofw/ofw_bus_if.m; do
			awk -f "$S/tools/makeobjops.awk" "$S/$m" -h
		done
		for o in h p q; do awk -f "$S/tools/vnode_if.awk" "$S/kern/vnode_if.src" -$o; done
		for o in $(grep -rhoE "opt_[a-z0-9_]+\.h" "$S" | sort -u) lkpi_iic_if.h; do : > "$o"; done
		printf '#define INET 1\n#define INET6 1\n' > opt_inet.h
		printf '#define INET6 1\n' > opt_inet6.h
		printf '#define SMP 1\n' > opt_global.h
	)
fi

# --- 3. Compile scsc_mx + scsc_wlan + wifi/lx/src + wifi/lx/scsc
MX_FILES="scsc_mx_module scsc_mx_impl scsc_service mxman miframman mifmboxman mifintrbit
	fwhdr fwimage cpacket_buffer mifstream mxmgmt_transport gdb_transport mxlog mxlog_transport
	fw_panic_record panicmon suspendmon mxsyserr mxfwconfig mx140_file"

WLAN_FILES="cm_if hip hip4 mbulk dev cfg80211_ops netif rx tx ba
	sap_mlme sap_ma sap_dbg sap_test mib mib_text_convert mgt mlme
	scsc_wifi_fcq reg_info log_clients traffic_monitor cac debug"

OURS="$W/src/lx_base.c $W/src/lx_wpa.c"
for f in "$W"/scsc/*.c; do [ ! -f "$f" ] || OURS="$OURS $f"; done

HAKIOS_LX_H=""
for cand in "$ROOT/build/dxb-12377/xnu/bsd/sys/hakios.lx.h" "$ROOT/xnu/files/bsd/sys/hakios.lx.h"; do
	if [ -f "$cand" ]; then
		HAKIOS_LX_H="$cand"
		break
	fi
done
if [ -z "$HAKIOS_LX_H" ]; then
	source "$ROOT/xnu-env.sh"
	[ -f "$XNU_SRC/bsd/sys/hakios.lx.h" ] && HAKIOS_LX_H="$XNU_SRC/bsd/sys/hakios.lx.h"
fi
[ -n "$HAKIOS_LX_H" ] || { echo "mk-scsc-wlan.sh: missing bsd/sys/hakios.lx.h" >&2; exit 1; }

CC="$CLANG -target arm64-apple-ios16.5"
CFLAGS="-mkernel -mgeneral-regs-only -fno-builtin -fno-common -ffreestanding -fno-strict-aliasing
	-funsigned-char -nostdinc -std=gnu17 -O2 -g -w
	-D__FreeBSD__=15 -D_KERNEL -DKLD_MODULE -DHAVE_KERNEL_OPTION_HEADERS -include opt_global.h
	-I$W/scsc/include -I$W/include -Igen -I$S -I$S/contrib/ck/include
	-I$S/compat/linuxkpi/common/include -I$S/compat/linuxkpi/dummy/include
	-include $S/compat/linuxkpi/common/include/linux/kconfig.h -DLINUXKPI_VERSION=70000"

SFLAGS="-include $W/scsc/lx_prelude.h
	-I$B/src/include -I$B/src/drivers/misc/samsung/scsc -I$B/src/drivers/net/wireless/scsc
	-I$W/scsc -I$W/src
	-DLX_HAKIOS_LX_H=\"$HAKIOS_LX_H\"
	-DCONFIG_SOC_EXYNOS3830=1
	-DCONFIG_SCSC_CORE=1
	-DCONFIG_SCSC_CORE_CM=1
	-DCONFIG_SCSC_PLATFORM=1
	-DCONFIG_SCSC_WLAN=1
	-DCONFIG_SCSC_COMMON_HCF=1
	-DCONFIG_SCSC_WLAN_STA_ONLY=1
	-DCONFIG_SCSC_WLAN_MAX_INTERFACES=3
	-DCONFIG_SCSC_CORE_FW_LOCATION=\"/vendor/etc/wifi\"
	-DCONFIG_SCSC_CORE_TOOL_LOCATION=\"/vendor/bin\"
	-DCONFIG_SCSC_WLAN_MAC_ADDRESS_FILENAME=\"efs/wifi/.mac.info\"
	-DKBUILD_MODNAME=\"scsc_wlan\""

mkdir -p obj out
rm -f obj/*.err

compile() {
	local src=$1 obj=$2
	shift 2
	if [ -f "$obj" ] && [ -f "$obj.d" ] && \
	   [ -z "$(find $(sed -e 's/^[^:]*://' -e 's/\\$//' "$obj.d") "$0" "$W/scsc/lx_prelude.h" -newer "$obj" 2>/dev/null | head -1)" ]; then
		return
	fi
	$CC $CFLAGS "$@" -MMD -MF "$obj.d" -c "$src" -o "$obj" 2> "$obj.err" || rm -f "$obj"
}

for f in $MX_FILES; do compile "src/drivers/misc/samsung/scsc/$f.c" "obj/mx_$f.o" $SFLAGS & done
for f in $WLAN_FILES; do compile "src/drivers/net/wireless/scsc/$f.c" "obj/wlan_$f.o" $SFLAGS & done
for f in $OURS; do compile "$f" "obj/lx_$(basename "$f" .c).o" $SFLAGS & done
wait

bad=
for f in obj/*.err; do
	if [ -s "$f" ] && [ ! -f "${f%.err}" ]; then
		echo "--- $(basename "$f" .o.err)"
		grep -m6 -A2 "error:" "$f" || head -n 12 "$f"
		bad=1
	fi
done
[ -z "$bad" ] || exit 1

OBJECTS=$(
	for f in $MX_FILES; do echo "obj/mx_$f.o"; done
	for f in $WLAN_FILES; do echo "obj/wlan_$f.o"; done
	for f in $OURS; do echo "obj/lx_$(basename "$f" .c).o"; done
)

# --- 4. Relocatable link + stubs for unbuilt procfs/udi/gdb/test helpers
KERNEL="memcmp memcpy memmove memset strcmp strncmp strlen strchr strrchr strlcpy strlcat bzero
	snprintf vsnprintf __stack_chk_fail __stack_chk_guard"

$LD_BIN -r -arch arm64 $OBJECTS -o out/all.o 2> out/ld.err || { head -n 30 out/ld.err; exit 1; }
$NM_BIN -u --format=just-symbols out/all.o | sed 's/^_//' | sort -u > out/undefined.txt

: > out/stubs.txt
: > out/stubs.s
missing=
for s in $(cat out/undefined.txt); do
	case " $(echo $KERNEL) " in *" $s "*) continue ;; esac
	case $s in
	lx_os_*|section\$*) continue ;;
	slsi_*|scsc_*|mifproc_*|mxproc_*|gdb_transport_*|hip4_sampler_*|mx140_log_dump|srvman_*)
		;;
	*)
		echo "mk-scsc-wlan.sh: unexpected undefined symbol: $s" >&2
		missing=1
		continue
		;;
	esac
	echo "$s" >> out/stubs.txt
	printf '\t.globl _%s\n_%s:\n\tadrp x0, 1f@PAGE\n\tadd x0, x0, 1f@PAGEOFF\n\tb _lx_stub\n\t.cstring\n1:\t.asciz "%s"\n\t.text\n\t.p2align 2\n' \
		"$s" "$s" "$s" >> out/stubs.s
done
[ -z "$missing" ] || exit 1

$CC -c out/stubs.s -o out/stubs.o 2> out/stubs.err
printf '_lx_prepare\n_lx_start\n_lx_stop\n_lx_transmit\n' > out/exported.txt
$LD_BIN -r -arch arm64 -exported_symbols_list out/exported.txt out/all.o out/stubs.o -o out/scsc_wlan.o 2> out/ld.err || {
	head -n 30 out/ld.err
	exit 1
}
rm -f out/all.o

TOTAL_FILES=$(( $(echo $MX_FILES | wc -w) + $(echo $WLAN_FILES | wc -w) ))
echo "mk-scsc-wlan.sh: out/scsc_wlan.o: $($SIZE_BIN out/scsc_wlan.o | awk 'NR == 2 { print $1 + $2 }') bytes of code and data from" \
	"$TOTAL_FILES SCSC files + $(echo $OURS | wc -w) lx glue files; $(wc -l < out/stubs.txt | tr -d ' ') stubs (out/stubs.txt);" \
	"asks XNU for $($NM_BIN -u --format=just-symbols out/scsc_wlan.o | grep -vc 'section\$') symbols"
