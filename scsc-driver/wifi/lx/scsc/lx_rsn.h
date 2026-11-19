/*
 * RSN element inspection and construction for WPA2-PSK station joins
 * (IEEE 802.11-2020 9.4.2.24: version 1, group cipher TKIP/CCMP, pairwise CCMP, AKM PSK).
 */
#ifndef _LX_RSN_H_
#define _LX_RSN_H_

#include <stdint.h>

#define LX_RSN_ELEMENT		48
#define LX_RSN_OURS_LENGTH	22
#define LX_RSN_TKIP		2
#define LX_RSN_CCMP		4

enum { LX_RSN_OPEN, LX_RSN_WPA2, LX_RSN_OLDER };

static inline const uint8_t *
lx_rsn_element(const uint8_t *elements, unsigned length, unsigned id)
{
	for (unsigned at = 0; at + 2 <= length && at + 2 + elements[at + 1] <= length; at += 2 + elements[at + 1]) {
		if (elements[at] == id) {
			return (elements + at);
		}
	}
	return (0);
}

static inline int
lx_rsn_kind(const uint8_t *elements, unsigned length, uint16_t capability)
{
	if (lx_rsn_element(elements, length, LX_RSN_ELEMENT) != 0) {
		return (LX_RSN_WPA2);
	}
	return ((capability & 0x0010) ? LX_RSN_OLDER : LX_RSN_OPEN);
}

static inline int
lx_rsn_suite(const uint8_t *at, unsigned type)
{
	return (at[0] == 0x00 && at[1] == 0x0f && at[2] == 0xac && at[3] == type);
}

static inline const char *
lx_rsn_choose(const uint8_t *elements, unsigned length, uint8_t ours[LX_RSN_OURS_LENGTH], unsigned *group)
{
	static const uint8_t template[LX_RSN_OURS_LENGTH] = {
		LX_RSN_ELEMENT, 20, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
		0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00,
	};
	const uint8_t *rsn = lx_rsn_element(elements, length, LX_RSN_ELEMENT), *at, *end;
	int ccmp = 0, psk = 0;
	unsigned count;

	if (rsn == 0) {
		return ("it has no RSN element (not WPA2)");
	}
	at = rsn + 2;
	end = at + rsn[1];
	if (end - at < 6 || at[0] != 1 || at[1] != 0 || at[2] != 0x00 || at[3] != 0x0f || at[4] != 0xac) {
		return ("its RSN element is not one this reads");
	}
	*group = at[5];
	at += 6;
	if (end - at < 2) {
		return ("its RSN element names no pairwise cipher");
	}
	count = at[0] | at[1] << 8;
	at += 2;
	for (; count != 0 && end - at >= 4; count--, at += 4) {
		ccmp |= lx_rsn_suite(at, LX_RSN_CCMP);
	}
	if (count != 0 || end - at < 2) {
		return ("its RSN element names no kind of key management");
	}
	count = at[0] | at[1] << 8;
	at += 2;
	for (; count != 0 && end - at >= 4; count--, at += 4) {
		psk |= lx_rsn_suite(at, 2);
	}
	if (count != 0) {
		return ("its RSN element is cut short");
	}
	if (*group != LX_RSN_CCMP && *group != LX_RSN_TKIP) {
		return ("its group's cipher is neither CCMP nor TKIP");
	}
	if (!ccmp) {
		return ("it does not offer CCMP");
	}
	if (!psk) {
		return ("it does not take a WPA2-PSK passphrase (WPA3 only, or enterprise)");
	}
	if (end - at >= 2 && (at[0] & 0x40)) {
		return ("it requires protected management frames");
	}
	for (unsigned i = 0; i < LX_RSN_OURS_LENGTH; i++) {
		ours[i] = template[i];
	}
	ours[7] = (uint8_t)*group;
	return (0);
}

#endif
