/*
 * qwz_awdl_cell - K5 (L2-C1): the inert, pure-C classifier and translator for route (c), the capture cell as the radio of the native AWDL engine.
 *
 * Nothing in the kext calls these functions yet; the object is only linked in. No kernel headers, no allocation, no globals: a host test
 * (tests/awdl_cell_test.c, build and run it with tests/run-awdl-cell-test.sh) exercises it with golden vectors on the MacBook.
 *
 * The two directions of the planned hooks:
 *   receive:  a frame the cell captured (qwz_mon_rx_hook) is classified; an AWDL action frame goes to the engine's management callback
 *             (sc_awdl_rx, as qwz.c does for frames from the firmware: OUI type 8 at offset 28), an AWDL data frame to its data callback
 *             (sc_awdl_data_rx: A3 is the AWDL BSSID, or A1 is awdl0's own address).
 *   transmit: a frame the engine built (awdlTx / awdlDataTx in AirportItlwmV2.cpp) is made the cell's own: A2 = the cell vdev's address,
 *             A3 = the AWDL BSSID, kind 1 (management) or 2 (data) for qwz_mon_tx_bytes, QWZ_MONTX_F_NO_ACK for group-addressed frames. The
 *             engine owns the AWDL header's timestamps (frame offsets 32 to 39: phy_tx_time and target_tx_time); they are never touched.
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#ifndef QWZ_AWDL_CELL_H
#define QWZ_AWDL_CELL_H

#include <stdint.h>
#include <stddef.h>
#include "qwz_mon.h"		/* struct qwz_montx_hdr, QWZ_MONTX_F_NO_ACK, QWZ_MONTX_MAXFRAME (stdint/stddef only) */

#ifdef __cplusplus
extern "C" {
#endif

#define QWZ_AWDL_CELL_OTHER		0	/* not an AWDL frame, or too short to tell */
#define QWZ_AWDL_CELL_ACTION_AWDL	1	/* management action (or action no-ack), vendor category 127, Apple OUI 00:17:f2, OUI type 8 */
#define QWZ_AWDL_CELL_DATA_AWDL		2	/* data frame with A3 = the AWDL BSSID (or A1 = awdl0's address, classify_to) */

#define QWZ_AWDL_CELL_MGMT_MIN		24	/* 802.11 management header */
#define QWZ_AWDL_CELL_ACTION_MIN	32	/* header + category, OUI, OUI type, version, subtype, reserved */
#define QWZ_AWDL_CELL_DATA_MIN		26	/* QoS data header (a frame to transmit); classification needs only the 24 bytes holding A1 to A3 */

/* The AWDL BSSID, 00:25:00:ff:94:73 (the one qwz.c and qwz_mon.c use). */
extern const uint8_t qwz_awdl_cell_bssid[6];

/* A3 of data frames and management frames: AWDL BSSID test. */
int	qwz_awdl_cell_classify_to(const uint8_t *f, size_t len, const uint8_t *awdl0_addr);

/* classify_to without awdl0's address (A3 test only). */
int	qwz_awdl_cell_classify(const uint8_t *f, size_t len);

/*
 * Copies engine_frame (len bytes, no FCS) into out (cap bytes) and rewrites it for the cell. Returns the frame length (equal to len), 0 when the frame
 * is rejected: a management frame shorter than 24 or a data frame shorter than 26 bytes, any other frame type, len above QWZ_MONTX_MAXFRAME, a
 * NULL argument, or cap < len. On success: A2 (offset 10) = self, A3 (offset 16) = the AWDL BSSID, *kind = 1 (management) or 2 (data),
 * h->flags = QWZ_MONTX_F_NO_ACK when A1 (offset 4) is group-addressed, else 0; h->rate = 0 (firmware's choice), h->pad = 0. Every other byte, the
 * timestamps at offsets 32 to 35 (and 36 to 39) included, is the engine's and is copied unchanged.
 */
size_t	qwz_awdl_cell_tx_prepare(uint8_t *out, size_t cap, const uint8_t *engine_frame, size_t len, const uint8_t self[6],
	    struct qwz_montx_hdr *h, uint8_t *kind);

#ifdef __cplusplus
}
#endif

#endif /* QWZ_AWDL_CELL_H */
