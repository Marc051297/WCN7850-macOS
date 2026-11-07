/*
 * qwz_awdlx.h - the AWDL evidence layer of the M2a build (docs/research-airdrop-2026-11-06.md, appendix A.3 I1 to
 * I10, section 3.5 Y0).
 *
 * LOG ONLY. Counters, three small tables and ring lines: nothing here changes a frame, a decision, a timer or the
 * order of any call. One global block (qwz_awdlx), shared by the qwz code (management-TX submit and completion, the
 * RX observers, the health printer, the TCL descriptor) and by AirportItlwm (the AWDL TX/RX paths, the AW clock).
 * Compiled into BOTH kexts: no IOKit, no Airport symbols, no qwz_softc layout (the softc fields the printer needs
 * come in through struct qwz_awdlx_env).
 *
 * Contexts (m0-spec 7.2 style): every counter is bumped with __atomic builtins, so any thread may write. The
 * summary is printed by qwz_health_log (under the main gate, from the sentinel's attemptAction); the AW-lateness
 * ring and the peer table are written under the same gate (workloop, interrupt action) and read there.
 *
 * Copyright (c) 2026. GPL-2.0-or-later (see LICENSE).
 */
#ifndef QWZ_AWDLX_H
#define QWZ_AWDLX_H

#include <stdint.h>
#include <stddef.h>

/* classes: the rows of every AWDL TX table. QX_UC and QX_MC are awdl0 data frames (Ethernet from the stack). */
enum { QX_PSF, QX_MIF, QX_UC, QX_MC, QX_OTH, QX_NCLS };
/* completion status columns: 0 OK, 1 DISCARD, 2 INSPECT, 3 COMPLETE_NO_ACK, anything else */
#define QX_NST		5
/* refusal causes (errno classes of the submit paths) */
enum { QX_E_NOSPC, QX_E_NOBUFS, QX_E_NOENT, QX_E_NOTCONN, QX_E_CANCEL, QX_E_INVAL, QX_E_NXIO, QX_E_OTHER, QX_NERR };
#define QX_NLEN		4	/* frame length buckets: <=128, <=512, <=1024, above */
#define QX_NLAT		5	/* latency buckets: <2, <5, <20, <100, >=100 ms */
#define QX_NDST		4	/* per-destination completion counters */
#define QX_NPEER	8	/* peers heard (the AWDL kext's peer cache is 8 too) */
#define QX_NKEY		32	/* (direction, transmitter, subtype) keys of the TLV-change log: 8 peers x (PSF, MIF) + our two = 18 would thrash a table of 16 */
#define QX_NPHASE	17	/* 16 bins of 4 TU in the 64-TU period, bin 16 = no clock */
/*
 * Completion slots: 0-7 are the BSS vif's management ring (desc_id of WMI_MGMT_TX_SEND today); 16-63 are reserved for
 * the own pool of the lane-M package (WP4, desc_id 0x40 + i): it passes 16 + i.
 */
#define QX_SLOTS	64
#define QX_LATE_N	256	/* AW timer lateness samples kept per summary interval */
#define QX_RATE_CMPL	0	/* indexes of rate[] */
#define QX_RATE_RXA	1
#define QX_RATE_RXD	2

struct qwz_softc;

/* what the health printer reads from the softc (qwz_health_log fills it; no qwz headers here) */
struct qwz_awdlx_env {
	int		listening;	/* sc_awdl_listening */
	int		quiesce;	/* sc_awdl_quiesce */
	int		dead;		/* the life word has DEAD */
	int		probe;		/* debug.qwz.awdlprobe */
	uint32_t	txmask;		/* sc_awdltest_txmask: slots the existing code believes are in flight */
	uint32_t	tcl_done;	/* QWZ_STAT_AWDL_DONE: TCL completions (awdlprobe 2 and 3 only) */
	uint32_t	tcl_inflight;	/* sc_awdl_inflight */
};

struct qwz_awdlx_slot {			/* per management-TX slot, filled at submit, consumed at completion */
	uint64_t	t_ns;
	uint32_t	len;
	uint8_t		valid, cls, lb, dst, ph, sl, age, pad;
};

struct qwz_awdlx_peer {			/* every transmitter heard (management actions and data) */
	uint8_t		mac[6];
	uint8_t		used, nlog;
	uint32_t	first_ms, last_ms;
	uint32_t	rx_m, rx_du, rx_dm, dup, retry;
	uint16_t	last_sc;
	uint8_t		have_sc, pad;
	uint32_t	h4, h12, h0c;	/* hashes of the TLV 4 (clock fields masked), 0x12 and 0x0c last logged */
	uint8_t		n4, n12, n0c, pad2;
	int8_t		rssi;
	uint8_t		pad3[3];
	uint32_t	pr_ms, pr_sig;	/* the P line: when this peer was last printed, and the counters it showed */
	uint32_t	hist[16];	/* frames heard by our 16-slot position */
};

struct qwz_awdlx_key {			/* the TLV-change log: one entry per (direction, transmitter, subtype) */
	uint8_t		used, dir, st, nlog;
	uint8_t		ta[6];
	uint16_t	len;
	uint32_t	sig;
};

struct qwz_awdlx {
	/* ---- control, set by AirportItlwm ---- */
	uint32_t	link;			/* awdl0 link up (awdlLinkGated) */
	uint32_t	started;		/* an evidence session began (first link up or listen) */
	uint32_t	start_ms;
	uint32_t	sum_ms[8];		/* per summary line: uptime of its last print */
	uint32_t	sum_sig[8];		/* ... and the signature it printed */
	/* ---- the AW clock mirror, written by AirportItlwm, read at submit and receive ---- */
	uint64_t	clk_next_ns;		/* start of the next availability window */
	uint64_t	clk_period_ns;
	uint32_t	clk_seq;		/* its sequence number (the window in progress is clk_seq - 4) */
	uint32_t	clk_on;
	uint32_t	clk_self;		/* we are master (free-running) */
	uint8_t		clk_master[6];
	uint8_t		pad0[2];
	/* ---- I10 / I1: TX bookkeeping, per class ---- */
	uint32_t	o_handed;		/* packets dequeued from awdl0 (outputStart) */
	uint32_t	o_pre[3];		/* ... dropped before the lane: too short, too long, link down */
	uint32_t	sub[QX_NCLS];		/* submits to the lane (calls of ItlQwz::awdlTx and awdlDataTx) */
	uint32_t	acc[QX_NCLS];		/* ... that returned 0 */
	uint32_t	ref[QX_NCLS][QX_NERR];	/* ... refused, by errno class */
	uint32_t	cm[QX_NCLS][QX_NST];	/* completions by status */
	uint32_t	cl[QX_NCLS][QX_NLEN][2];/* completions by frame length: [ok, fail] */
	uint32_t	lat[QX_NCLS][QX_NLAT];	/* submit-to-completion time */
	uint32_t	flush[QX_NCLS];		/* slots freed by a teardown without a completion */
	uint32_t	orphan;			/* completions for a slot with no submit record */
	uint32_t	rq[QX_NCLS];		/* requeued submits (WP4): sub counts them again */
	uint32_t	held[QX_NCLS];		/* frames waiting in the gate queue (WP4), a level, not a counter */
	uint32_t	wdrop[QX_NCLS];		/* frames dropped after their attempts (WP4) */
	uint32_t	b_in, b_full, b_bad;	/* the bridge queue: frames from the engine, refused full, refused other */
	struct qwz_awdlx_slot slot[QX_SLOTS];
	uint64_t	slotmask;
	uint32_t	ack_n, ack_zero;	/* unicast completions with status 0: count, and how many carried ack_rssi 0 */
	int64_t		ack_sum;
	int32_t		ack_min;
	uint32_t	ctlv_len, ctlv_x0, ctlv_nlog;	/* the completion TLV: length, the dword at +20, lines logged */
	struct { uint8_t mac[6]; uint8_t used, pad; uint32_t ok, fail; } dst[QX_NDST];
	uint32_t	dst_other[2];
	uint32_t	ph[QX_NPHASE][2];	/* unicast completions by 4-TU phase at submit: [ok, fail] */
	uint32_t	sl[16][2];		/* ... by our 16-slot position at submit */
	uint32_t	ag[4][2];		/* ... by how long ago the destination was last heard: <30 ms, <100 ms, <1 s, older */
	uint32_t	nfail_log, nok_log[QX_NCLS];
	/* ---- I3: receive split ---- */
	uint32_t	rx_m[3];		/* management actions: to us, group, other */
	uint32_t	rx_m_st[3];		/* ... PSF, MIF, other subtype */
	uint32_t	rx_m_retry, rx_m_dup;
	uint32_t	rx_d[3];		/* data: to us, group, other */
	uint32_t	rx_d_retry, rx_d_dup;
	uint32_t	rx_log_m, rx_log_d, rx_log_u;
	struct { uint32_t sec_ms, n; } rate[3];	/* time caps of the lines that can run at the frame rate: failed completions, received actions, received data */
	struct qwz_awdlx_peer peer[QX_NPEER];
	uint32_t	peer_full;		/* evictions: a ninth transmitter replaces the least recently heard peer (refuter nice_to_fix) */
	/* ---- I4: TLV evidence ---- */
	struct qwz_awdlx_key key[QX_NKEY];
	uint32_t	key_next, key_evict, key_logs, hex_logs, tx_hex_logs, frame_dumped;	/* hex_logs: peers' TLVs; tx_hex_logs: ours (own budget) */
	uint32_t	tx4, tx12, tx_flips;	/* hashes of the TLV 4 (clock fields masked) and 0x12 we last transmitted */
	uint32_t	mi_len, mi_t2, mi_t10, mi_arpa;	/* the last MIF we transmitted */
	/* ---- I7: clock ---- */
	uint32_t	ce[5];			/* clock error of every peer frame with a TLV 4, this interval: <1, <3, <10, <32, >=32 TU */
	uint32_t	ce_n;
	int32_t		ce_maxabs_us;
	uint32_t	fr_n;			/* ... from the master we follow */
	int32_t		fr_last_us, fr_maxabs_us;
	uint32_t	ce_total, fr_total;	/* the same two counts, cumulative: never reset (the role line prints these) */
	uint32_t	clock_logs;		/* the limiter of the 'clock' ring line: cumulative, never reset (the K line zeroes ce_n) */
	uint32_t	late_n;
	int32_t		late_us[QX_LATE_N];
	uint32_t	late_total;
	uint32_t	flips, flip_logs;
	/* ---- I6: L3/L4 tracer ---- */
	uint32_t	l4_tcp[2], l4_syn[2], l4_rst[2], l4_fin[2], l4_icmp[2], l4_mdns[2], l4_other[2];	/* [0] rx, [1] tx */
	uint32_t	l4_logs_tcp, l4_logs_icmp, l4_logs_mdns, l4_logs_eth, l4_8770[2];
	/* ---- I9: TCL descriptors ---- */
	uint32_t	tcl_left[2];		/* [0] AWDL vif, [1] infra vif (baseline), armed at link up */
	uint32_t	tcl_n[2];
};

extern struct qwz_awdlx qwz_awdlx;

/* every packet awdl0 hands to outputStart, before anything is decided about it */
static inline void
qwz_awdlx_handed(void)
{
	(void)__atomic_fetch_add(&qwz_awdlx.o_handed, 1u, __ATOMIC_RELAXED);
}

/* ---- classification (inline: both kexts, no state) ---- */
static inline int
qwz_awdlx_class(const uint8_t *f, size_t len)
{
	if (len < 24)
		return QX_OTH;
	if ((f[0] & 0x0c) == 0x08)			/* data frame, any subtype */
		return (f[4] & 1) ? QX_MC : QX_UC;
	if ((f[0] & 0x0c) == 0x00 && len >= 31 && f[24] == 0x7f && f[25] == 0x00 && f[26] == 0x17 && f[27] == 0xf2 &&
	    f[28] == 0x08)				/* management, Apple OUI 00:17:f2 type 8 = AWDL */
		return f[30] == 0 ? QX_PSF : f[30] == 3 ? QX_MIF : QX_OTH;
	return QX_OTH;
}

/* ---- AirportItlwm: the control plane ---- */
void	qwz_awdlx_link(int up);			/* awdl0's link went up or down (also at driver stop) */
void	qwz_awdlx_clk(int on, uint64_t next_ns, uint64_t period_ns, uint32_t seq, int self);
void	qwz_awdlx_master(const uint8_t *master, int self);	/* the engine's top master changed (bridge master()) */
void	qwz_awdlx_late(int64_t late_ns);		/* AW timer lateness, one sample per timer event */
void	qwz_awdlx_chanseq(const uint32_t *channels, const uint32_t *flags);
int	qwz_awdlx_errcls(int err);

/* ---- AirportItlwm: TX results and frame evidence ---- */
void	qwz_awdlx_tx_result(int cls, int ret);	/* one submit: cls from qwz_awdlx_class, ret from the lane */
void	qwz_awdlx_drop_pre(int why);		/* outputStart dropped a packet before the lane (0 short, 1 long, 2 link down) */
void	qwz_awdlx_bridge(int result);		/* awdlTxEnqueue: 0 queued, ENOSPC, other */
int	qwz_awdlx_tlvs(int dir, const uint8_t *frame, size_t len);	/* TX: the final frame; RX: before the clock follow. 1 if the TLV signature changed */
void	qwz_awdlx_trace(int dir, const uint8_t *dst, const uint8_t *src, unsigned ethertype, const uint8_t *p, size_t len);

/* ---- qwz: the lane, the RX observers, the printers ---- */
void	qwz_awdlx_sub(int slot, const uint8_t *frame, size_t len);
void	qwz_awdlx_unsub(int slot);
void	qwz_awdlx_cmpl(int slot, uint32_t status, int32_t ack_rssi);
void	qwz_awdlx_cmpl_tlv(uint32_t tlv_len, const uint8_t *ev);
void	qwz_awdlx_flush(void);
void	qwz_awdlx_rx_mgmt(const uint8_t *frame, size_t len, int rssi, int ours);
void	qwz_awdlx_rx_data(const uint8_t *frame, size_t len, int rssi, int ours);
void	qwz_awdlx_tcl(int awdl, int slot, unsigned vdev, unsigned bank, const uint32_t *info /* info0..info4 */,
	    unsigned len, unsigned tid, const uint8_t *wh, size_t wlen);
void	qwz_awdlx_health(const struct qwz_awdlx_env *env);

#endif /* QWZ_AWDLX_H */
