/*
 * qwz_awdl_cell - see qwz_awdl_cell.h. Pure C, no kernel headers: it compiles as C++ in the kext (build.sh) and as C on the host (tests/).
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#include <stdint.h>
#include <string.h>
#include "qwz_awdl_cell.h"

const uint8_t qwz_awdl_cell_bssid[6] = { 0x00, 0x25, 0x00, 0xff, 0x94, 0x73 };

int
qwz_awdl_cell_classify_to(const uint8_t *f, size_t len, const uint8_t *awdl0_addr)
{
	uint8_t type;

	if (f == NULL || len < 2)
		return QWZ_AWDL_CELL_OTHER;
	if ((f[0] & 0x03) != 0)		/* protocol version 0 only */
		return QWZ_AWDL_CELL_OTHER;
	type = (f[0] >> 2) & 3;
	if (type == 0) {
		/* action (0xd0) or action no-ack (0xe0): category 127 (vendor), Apple OUI, OUI type 8 (AWDL), as qwz.c tests */
		if ((f[0] & 0xf0) != 0xd0 && (f[0] & 0xf0) != 0xe0)
			return QWZ_AWDL_CELL_OTHER;
		if (len < QWZ_AWDL_CELL_ACTION_MIN)
			return QWZ_AWDL_CELL_OTHER;
		if (f[24] == 127 && f[25] == 0x00 && f[26] == 0x17 && f[27] == 0xf2 && f[28] == 0x08)
			return QWZ_AWDL_CELL_ACTION_AWDL;
		return QWZ_AWDL_CELL_OTHER;
	}
	if (type == 2) {
		/* data: A3 (offset 16) the AWDL BSSID, or A1 (offset 4) awdl0's own address, as qwz_dp_rx_deliver_msdu decides */
		if (len < QWZ_AWDL_CELL_MGMT_MIN)
			return QWZ_AWDL_CELL_OTHER;
		if (memcmp(f + 16, qwz_awdl_cell_bssid, 6) == 0)
			return QWZ_AWDL_CELL_DATA_AWDL;
		if (awdl0_addr != NULL && memcmp(f + 4, awdl0_addr, 6) == 0)
			return QWZ_AWDL_CELL_DATA_AWDL;
		return QWZ_AWDL_CELL_OTHER;
	}
	return QWZ_AWDL_CELL_OTHER;
}

int
qwz_awdl_cell_classify(const uint8_t *f, size_t len)
{
	return qwz_awdl_cell_classify_to(f, len, NULL);
}

size_t
qwz_awdl_cell_tx_prepare(uint8_t *out, size_t cap, const uint8_t *engine_frame, size_t len, const uint8_t self[6],
    struct qwz_montx_hdr *h, uint8_t *kind)
{
	uint8_t type;
	uint8_t k;

	if (out == NULL || engine_frame == NULL || self == NULL || h == NULL || kind == NULL)
		return 0;
	if (len < 2 || len > QWZ_MONTX_MAXFRAME || cap < len)
		return 0;
	if ((engine_frame[0] & 0x03) != 0)
		return 0;
	type = (engine_frame[0] >> 2) & 3;
	if (type == 0) {
		if (len < QWZ_AWDL_CELL_MGMT_MIN)
			return 0;
		k = 1;
	} else if (type == 2) {
		if (len < QWZ_AWDL_CELL_DATA_MIN)
			return 0;
		k = 2;
	} else
		return 0;
	if (out != engine_frame)
		memmove(out, engine_frame, len);
	memcpy(out + 10, self, 6);
	memcpy(out + 16, qwz_awdl_cell_bssid, 6);
	h->flags = (out[4] & 0x01) ? QWZ_MONTX_F_NO_ACK : 0;
	h->rate = 0;
	h->pad = 0;
	*kind = k;
	return len;
}
