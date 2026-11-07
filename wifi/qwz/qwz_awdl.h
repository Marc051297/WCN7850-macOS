/*
 * qwz_awdl.h - lane M of the AWDL path, the hardened lane of the M2a build (WP4): the definitions that qwz.c (the slot
 * pool), ItlQwz (the calls into it) and AirportItlwm (pacing, gate, requeue) share. Plain C types only; compiled
 * into both kexts.
 *
 * Switches (qwz_life.h, qwz_sysctl.cpp), all default 0, and 0 is the M0.1 behaviour:
 *   debug.qwz.awdlhyg   1  frame hygiene and pacing: 12-bit sequence numbers, the retry bit on a requeue, duration 0,
 *                          QoS TID 0, outputStart stops dequeuing when the lane has no room
 *   debug.qwz.awdltx    1  F1: the own slot pool carries the frames; nothing changes on the wire
 *                       2  F2: + WMI_VDEV_PARAM_MGMT_RATE 12 Mbit/s on the listen vdev after VDEV_UP and the tx_params
 *                          presets for Data frames (needs a proven recovery, as awdlprobe 1 to 3)
 *                       3  F3: + Data frames as WMI_OFFCHAN_DATA_TX_SEND (the same)
 *   debug.qwz.awdlgate  1  peer-activity gate and requeue (appendix A.2 a and c)
 *                       2  + the shared-slot test (A.2 b)
 *   debug.qwz.awdltxopt    mask for awdltx >= 2: 1 NON_AGG_SW_RETRY_TH 4, 2 BMISS parameters raised
 * The slot pool is the carrier whenever awdlhyg, awdlgate or the rung in force (qwz_awdltx_eff) is non-zero.
 * awdlhyg and awdlgate act on awdl0's Data frames only with debug.qwz.awdlprobe=1 (lane M); the engine's PSF and MIF
 * use the pool whatever awdlprobe says. A frame that carries a cookie (the gate's handle for it) is carried by the pool or
 * refused with ECANCELED, never handed to the M0 ring or to the TCL path: no completion of those reaches the gate's entry.
 *
 * Copyright (c) 2026. GPL-2.0-or-later (see LICENSE).
 */
#ifndef QWZ_AWDL_H
#define QWZ_AWDL_H
#include "qwz_life.h"

#define QWZ_AWDL_POOL		16	/* slots (appendix A.1: the hard ceiling is 48) */
#define QWZ_AWDL_POOL_SYNC	4	/* of the pool, four slots are kept for PSF and MIF: Data frames take at most QWZ_AWDL_POOL - QWZ_AWDL_POOL_SYNC at a time (a count; no slot is reserved by its index) */
#define QWZ_AWDL_DESC_BASE	0x40	/* WMI desc_id = QWZ_AWDL_DESC_BASE + slot; the BSS ring uses 0 to 7 */
#define QWZ_AWDL_STUCK_MS	2000	/* a slot in flight this long is reported; a slot is never freed because of it */

/* the classes of the completion tables */
enum { QWZ_AWDL_CLS_PSF, QWZ_AWDL_CLS_MIF, QWZ_AWDL_CLS_UC, QWZ_AWDL_CLS_MC, QWZ_AWDL_CLS_MAX };
/* the firmware's completion status (WMI_MGMT_TX_COMP_STATUS_TYPE); anything else is column 4 of the tables */
#define QWZ_AWDL_ST_OK		0
#define QWZ_AWDL_ST_DISCARD	1
#define QWZ_AWDL_ST_INSPECT	2
#define QWZ_AWDL_ST_NOACK	3
enum { QWZ_AWDL_LANE_OPEN, QWZ_AWDL_LANE_DOWN };

/* A completion, as ItlQwz hands it to AirportItlwm: on the workloop thread, the main gate held, bookkeeping only. */
struct qwz_awdl_txdone {
	uint32_t	cookie;		/* the cookie given at submit; 0: a frame without one (sync frames, ungated Data) */
	uint32_t	epoch;		/* sc_fw_epoch at submit */
	uint32_t	status;		/* QWZ_AWDL_ST_* or the firmware's own value */
	uint32_t	seq;		/* the 12-bit sequence number the frame carried (0 while awdlhyg is off) */
	uint32_t	lat_us;		/* submit to completion */
	int32_t		ack_rssi;
	uint32_t	cls;		/* QWZ_AWDL_CLS_* */
};
/* A snapshot of the lane, readable without the gate (plain loads; it may be a few microseconds old). */
struct qwz_awdl_lane {
	uint32_t	state;		/* QWZ_AWDL_LANE_*: DOWN while there is no listen vdev, a teardown quiesces AWDL, or the firmware is DEAD */
	uint32_t	epoch;		/* sc_fw_epoch: a frame submitted in another epoch is gone */
	uint32_t	data_room;	/* Data frames the pool takes now */
	uint32_t	free_slots;
	uint32_t	eff_tx;		/* the rung in force */
};
typedef void (*qwz_awdl_txdone_fn)(void *arg, const struct qwz_awdl_txdone *d);

static inline int
qwz_awdl_lane_pool(void)
{
	return qwz_awdlhyg != 0 || qwz_awdlgate != 0 || qwz_awdltx_eff != 0;
}
#endif
