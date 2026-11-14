/*
 * qwz_mon - monitor-mode capture on the WCN7850, the first AWDL milestone
 * (docs/airdrop-revisited.md, M0): park the radio on one channel with the
 * station down, pass every frame on the air to the host, and hand the frames
 * to userspace through a sysctl, so an iPhone's AWDL action frames can be
 * seen (or not) before any AirDrop code is written.
 *
 *   debug.qwz.mon=<channel>   start (this fork: Wi-Fi ON and debug.qwz.monhold=1 set first; the kext stops the
 *                             station in place. Wi-Fi off powers the chip down here: docs/upstream-awdl-merge-2026-11-07.md)
 *   debug.qwz.mon=0           stop, restore the station's receive filter
 *   debug.qwz.monflags=<m>    experiment knobs (QWZ_MON_F_* below)
 *   debug.qwz.moncap          read-only: drains captured frames
 *                             (scripts/qwz-moncap.py decodes and writes pcap)
 *
 * Model (Linux ath12k, WCN7850 = rxdma1_enable false): a monitor vdev is
 * created, started on the channel and brought up; the RXDMA buffer rings get
 * the "monitor destination" filter with the filter-pass and monitor-other
 * bits for management, control and data frames; the hardware then routes
 * every MPDU through the RXDMA error destination ring as link descriptors,
 * which a timer reaps. The monitor status ring (per-PPDU RSSI and rates) is
 * not used yet: this milestone only has to see frames.
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#ifndef QWZ_MON_H
#define QWZ_MON_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct qwz_softc;

/* Record header in the capture stream (debug.qwz.moncap), little-endian,
 * followed by len bytes of frame and padding to a multiple of 4. */
struct qwz_moncap_hdr {
	uint32_t len;		/* frame bytes that follow */
	uint32_t ts_lo;		/* uptime at delivery, microseconds, low 32 bits */
	uint16_t flags;		/* QWZ_MONCAP_F_* */
	uint8_t decap;		/* DP_RX_DECAP_TYPE_* of the first MSDU */
	uint8_t chan;		/* channel the radio was parked on */
	uint32_t seq;		/* running count of captured frames */
	int8_t rssi;		/* dBm when known (WMI management path), else 0 */
	uint8_t rate;		/* 500 kbit/s units when known, else 0 */
	uint16_t ts_hi;		/* microseconds, bits 32-47 */
};

/*
 * M2: frames from userspace (debug.qwz.montxframe, opaque write): this
 * header, then the 802.11 frame without FCS. Sent through the firmware's
 * management-transmit command on the capture vdev (AP type with its self
 * peer). ENOSPC when the 64-slot queue is full: try again later.
 */
struct qwz_montx_hdr {
	uint16_t flags;		/* QWZ_MONTX_F_* */
	uint8_t rate;		/* 500 kbit/s units; 0 lets the firmware choose (recorded only for now) */
	uint8_t pad;
};
#define QWZ_MONTX_F_NO_ACK	0x0001
#define QWZ_MONTX_MAXFRAME	2400
#define QWZ_MONCAP_F_RXDMA_ERR	0x0001	/* entrance ring: error push reason */
#define QWZ_MONCAP_F_TRUNC	0x0002	/* frame longer than the record buffer */
#define QWZ_MONCAP_F_NO_MPDU	0x0004	/* first buffer lacks the MPDU_START tag */
#define QWZ_MONCAP_F_CONT	0x0008	/* MSDU spanned several buffers */
#define QWZ_MONCAP_F_DROPCLASS	0x0010	/* flush, length or overflow error code */
#define QWZ_MONCAP_F_FCS	0x0020	/* raw decap: frame ends with the FCS */
#define QWZ_MONCAP_F_HDR_SYNTH	0x0040	/* native-wifi: header rebuilt from the descriptor */
#define QWZ_MONCAP_F_WMI	0x0080	/* delivered by the firmware's WMI management RX event */
#define QWZ_MONCAP_F_REO	0x0100	/* arrived on the ordinary REO receive path (station data path) */
#define QWZ_MONCAP_SKIP		0xffffffffU	/* ring wrap marker in place of len */

/* debug.qwz.monflags */
#define QWZ_MON_F_OFFSETS	0x0001	/* keep the explicit RX TLV offsets in the monitor filter (default on) */
#define QWZ_MON_F_RXMON		0x0002	/* set EN_RXMON in the filter command, as Linux does (default on) */
#define QWZ_MON_F_FP_ONLY	0x0004	/* filter-pass bits only, no monitor-other (experiment) */
#define QWZ_MON_F_KEEP_FILTER	0x0008	/* leave the receive filter alone (vdev only, experiment) */
#define QWZ_MON_F_VERBOSE	0x0010	/* log every MPDU */
#define QWZ_MON_F_DELETE_STA	0x0020	/* delete the station vdev for the capture (qwz_init recreates it) */
#define QWZ_MON_F_PROMISC	0x0040	/* WMI_PDEV_SET_RX_FILTER_PROMISCUOUS on for the capture */
#define QWZ_MON_F_MNTFILTER	0x0080	/* WMI_MNT_FILTER_CMDID set, all frame types, on the monitor vdev */
#define QWZ_MON_F_REO		0x0100	/* also capture what reaches the REO receive path (default on) */
#define QWZ_MON_F_BSSID_MAC	0x0200	/* create the vdev with debug.qwz.monbssid as its own MAC too */
#define QWZ_MON_F_AP		0x0400	/* vdev type AP (hidden SSID) instead of MONITOR */
#define QWZ_MON_F_NRP		0x0800	/* WMI neighbour-rx filter: accept monbssid as an AP BSSID */
#define QWZ_MON_F_NRP_STA	0x1000	/* ... as a station MAC instead */
#define QWZ_MON_F_NO_SELF_PEER	0x2000	/* AP vdev without its self peer (transmit then fails: DISCARD) */
#define QWZ_MON_F_BEACON	0x4000	/* give the AP vdev a minimal hidden-SSID beacon template before vdev up */
#define QWZ_MON_F_PEER_SENDER	0x8000	/* raw firmware peer for the first PSF sender heard (unicast needs?) */
extern uint32_t qwz_mon_flags;
/*
 * K5 (L1-C1): the cell beside the running station, all plain ints defined in qwz_sysctl.cpp, 0 / K4 behaviour by default (see docs/k5 and qwz_sysctl.h).
 * debug.qwz.moncoex 0 = off, K4 exactly; 1 = a cell started while the station is associated on the same channel (6, 44 or 149) runs beside it and the station is not stopped.
 * moncoexmode 0 clone the station's vdev-start chandef, 1 HT20 on the primary; moncoexpassive 0 as the K4 cell, 1 copy the station's passive flag;
 * moncoexfilter 0 never swap the station's RX filter, 1 honour monflags, 2 swap with filter-pass only; moncoexscan 1 answer scans from the node table (live);
 * moncoexcred 1 reserve one WMI credit (live); moncoexfw 0 no firmware restart at cell stop, 1 the old one (live); moncoexnss 0 NSS from the chain count, 1 NSS 1;
 * moncoexjoin 0 refuse joins while the cell runs, 1 stop the cell and join (live). moncoex, mode, passive, filter and nss are read when the cell starts.
 */
extern int qwz_moncoex, qwz_moncoexmode, qwz_moncoexpassive, qwz_moncoexfilter, qwz_moncoexscan, qwz_moncoexcred, qwz_moncoexfw, qwz_moncoexnss, qwz_moncoexjoin;
/* K6 (K6-1): debug.qwz.moncoexwbm, default 1, acts only while a coex cell runs (moncoex defaults to 0): the WBM error path hands the cell's peer-less data frames to the capture; read live */
extern int qwz_moncoexwbm;
extern int qwz_moncoexdwell, qwz_moncoexrest;	/* F2e */
#define QWZ_MON_DWELL_SCAN_ID	0xA0D1	/* F2e: scan id of a dwell visit (ATH12K_SCAN_ID is 0xA000) */
#define QWZ_MON_DWELL_REQ_ID	2	/* the station's scans use 1 */
int	qwz_mon_dwell_start(struct qwz_softc *, uint32_t freq, int dwell_ms);	/* qwz.c: sends the scan; may sleep for a WMI credit */
void	qwz_mon_dwell_event(struct qwz_softc *, uint32_t type, uint32_t freq, uint32_t reason);	/* qwz_mon.c: the visit's scan events */
extern int qwz_moncoexmac;	/* F2d: the ring moncoexfilter 3 swaps (-1 by band) */
/* F2-2: debug.qwz.moncoexmcc, default 0: the coex cell on its own channel (6, 44 or 149) beside a station on another channel; read at cell start and by the probe. */
extern int qwz_moncoexmcc;
/* F2-3: debug.qwz.moncoexquota, default 0: percent of radio time for the cell's channel while an mcc cell runs (firmware channel time quota). */
extern int qwz_moncoexquota;
/* K5 (L2-C2): debug.qwz.awdlcell, default 0; nothing reads it yet (the receive-only tee is not built), the K5 banner reports it */
extern int qwz_awdlcell;
/*
 * M3: debug.qwz.monhold. While set and the cell is active, macOS turning
 * Wi-Fi on does not start the station: the interface reports UP|RUNNING,
 * scans complete empty, joins are refused, and the cell keeps running. Wi-Fi
 * then shows as powered on, which is what sharingd needs before it sends the
 * AirDrop Bluetooth advertisement that wakes a phone's receiver.
 */
extern uint32_t qwz_mon_hold;
struct ieee80211com;
int	qwz_mon_holding(struct ieee80211com *);	/* hold set and cell active */
/* K5 (L1-C5): 1 while a coex cell runs beside the station (cell active, mon_coex set, firmware up); qwz_mon_holding is 0 then */
int	qwz_mon_coex_running(struct qwz_softc *);
/* K5 (L1-C5): the scan policy: 1 when a scan must not reach the firmware, either the classic hold or (moncoexscan) a coex cell */
int	qwz_mon_scan_hold(struct ieee80211com *);
/* K5 (L1-C12): qwz_mon_coex_running for callers that only hold the ieee80211com (WCL.cpp) */
int	qwz_mon_coex_running_ic(struct ieee80211com *);
/*
 * K5 (L1-C6, L1-C7): the per-frame claim tests of a coex cell. f is an 802.11 header (len bytes, at least 24), a1/a2/a3 its three addresses.
 * qwz_mon_is_station_frame: the station's (A1 is our address, or A3 is the BSSID, or a group-addressed frame from the AP's address); it is tested FIRST.
 * qwz_mon_claims_mgmt: the cell's (received on the cell's frequency and A3 is the AWDL BSSID or debug.qwz.monbssid, or A1 is the cell vdev's address).
 * qwz_mon_claims_data: 1 when no coex cell runs (K4 keeps everything); in coex 0 for the station's frames (A1 is our address, A3 the BSSID), else 1 when A3 is
 * the AWDL BSSID or monbssid or A1 is the cell vdev's or awdl0's address, else 0.
 */
int	qwz_mon_is_station_frame(struct qwz_softc *, const uint8_t *f, size_t len);
int	qwz_mon_claims_mgmt(struct qwz_softc *, const uint8_t *f, size_t len, uint32_t chan_freq);
int	qwz_mon_claims_data(struct qwz_softc *, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3);
/* K5 (L1-C13): the station is about to leave RUN (newstate task): stop a running coex cell first; bounded (about 2 s), never blocks on the gate; returns 1 when a cell was running */
int	qwz_mon_coex_halt(struct qwz_softc *);
/* K5 (L1-C13, C14): called once a second from qwz_watchdog under the gate: stops the cell when the station's channel changed, and runs the deaf-station probe after a coex cell stopped */
void	qwz_mon_coex_probe(struct qwz_softc *);
/* K5 (L1-C8): management-transmit descriptor ids below this belong to the station while a coex cell runs; the cell's ring is ids QWZ_MON_COEX_STA_SLOTS to 63 (qwz.c asserts it equals QWZ_BSS_TXMGMT_SLOTS) */
#define QWZ_MON_COEX_STA_SLOTS	8
/*
 * M3/M4: debug.qwz.awdlif = "feth0" | "-feth0". Sets or clears IFEF_AWDL
 * (xnu if_private.h: 0x00100000, which drags LOCALNET_PRIVATE, IPV6_ND6ALT
 * and RESTRICTED_RECV along) on that interface, so mDNSResponder and
 * sharingd treat it as awdl0: Bonjour then announces AirDrop there and
 * sharingd accepts connections from it. Returns errno.
 */
int	qwz_mon_awdlif(const char *cmd);
/* debug.qwz.monbssid: "xx:xx:xx:xx:xx:xx"; when set, vdev_up uses it as the BSSID
 * (the firmware's filter-pass class), and the flags above build on it. Empty: own MAC. */
extern char qwz_mon_bssid_str[32];	/* room for the terminator sysctl(8) sends */

/*
 * F2 (native AWDL, docs/forbiddenitlwm-port-map-2026-11-13.md): a second consumer of every captured frame. Called from the
 * capture emitters in their own context (the command-gate work loop: WMI events, the REO and WBM taps, the cell's ring
 * timer), before the frame goes to the userspace ring, with the record's flags (QWZ_MONCAP_F_FCS: the FCS is included),
 * channel and RSSI (0 when the path has none). NULL unregisters. Plain stores; the consumer must be cheap and never sleep.
 */
typedef void (*qwz_mon_native_rx_fn)(void *arg, const uint8_t *frame, size_t len, uint16_t flags, uint8_t chan, int8_t rssi);
void	qwz_mon_set_native_rx(qwz_mon_native_rx_fn fn, void *arg);
/* F2: free transmit slots on the cell for data frames right now (0 when no cell, the firmware is unavailable or TX is blocked). */
int	qwz_mon_tx_room(void);
/* F2: send one 802.11 frame (no FCS) from the cell, the gate held by the caller; errno (ENOSPC: no slot, try later). */
int	qwz_mon_tx_raw(const uint8_t *frame, size_t len);
/* qwz_attach(): allocates the capture ring and remembers the softc. */
void	qwz_mon_attach(struct qwz_softc *);
/* ItlQwz::detachGated (merge glue F): ends a cell host-only, frees the capture buffers, the lock and the 64 maps, forgets the softc. */
void	qwz_mon_detach(struct qwz_softc *);
/*
 * merge glue (A): ends a capture cell WITHOUT talking to the firmware: host state only (mon_active, mon_fw_dirty, the capture vif's is_up and
 * is_started, the self and sender peers, mon_tx_kind, the cell timer, the transmit queue), no WMI, no sleep, no allocation, idempotent, safe with
 * no cell and before qwz_mon_attach. Called by qwz_core_deinit and qwz_init_impl (qwz.c declares it identically). C linkage.
 */
void	qwz_mon_epoch_reset(struct qwz_softc *);
/* qwz_init(): called under the gate before the station comes back. A cell whose firmware is gone (crashed, DEAD, torn down) ends host-only. */
void	qwz_mon_stop(struct qwz_softc *);
/* qwz_mgmt_rx_event(): a management frame from the firmware while capturing. */
struct mbuf;
void	qwz_mon_capture_mgmt_mbuf(struct qwz_softc *, struct mbuf *, int rssi, uint32_t rate_kbps);
/* qwz_dp_rx_process_received_packets(): an 802.11 frame off the REO path while capturing. */
void	qwz_mon_capture_reo_mbuf(struct qwz_softc *, struct mbuf *);
struct qwz_rx_msdu;
int	qwz_mon_capture_wbm_msdu(struct qwz_softc *, struct qwz_rx_msdu *);	/* K6 (K6-1): 1 = copied into the capture, the caller still frees the mbuf */
/* K1 W2b: evidence hooks, no effect on any decision. what: 0 a WMI credit consumed (htc send), 1 a credit report arrived, 2 a consumed credit was given back unused. */
void	qwz_mon_lat_credit(struct qwz_softc *, int eid, int credits, int what);
/* K1 W2b: ring 0..3 = REO destination ring, n buffers reaped. */
void	qwz_mon_lat_rxring(struct qwz_softc *, int ring, int n);

/*
 * M1, transmitting: debug.qwz.montx. The last PSF heard (any peer) is kept
 * and replayed as our own frame: sender = the capture vdev's MAC, BSSID
 * kept, destination broadcast or the peer. Bits: 1 send one broadcast copy
 * now, 4 send one unicast copy to the last PSF's sender now, 2 auto mode
 * (answer every PSF heard with a unicast copy to its sender while the peer
 * is still on our channel). Completions are counted by status; a status 0
 * on a unicast means the peer's hardware ACKed our frame.
 */
int	qwz_mon_tx(int ops);		/* errno */
extern uint32_t qwz_mon_tx_mode;
/* debug.qwz.montxframe: a qwz_montx_hdr plus frame from userspace (errno). */
int	qwz_mon_tx_frame(const void *buf, size_t len);
/* debug.qwz.monpeer: "add xx:xx:xx:xx:xx:xx" or "del xx:...": a firmware peer
 * on the capture vdev, for unicast data to that station (errno). */
int	qwz_mon_peer(const char *cmd);
/* debug.qwz.monmac (read-only): the capture vdev's MAC while active, else the card's. */
void	qwz_mon_mac_str(char *out, size_t len);
struct wmi_mgmt_tx_compl_event;
/* qwz_wmi_process_mgmt_tx_comp(): 1 when the completion was one of ours. */
int	qwz_mon_tx_compl(struct qwz_softc *, struct wmi_mgmt_tx_compl_event *);

/* From the sysctls (process context; dispatched onto the gate). */
int	qwz_mon_set(int chan);		/* errno; 0 stops */
int	qwz_mon_status(void);		/* channel while active, else 0 */
size_t	qwz_mon_cap_pending(void);	/* bytes a drain would return */
size_t	qwz_mon_cap_drain(void *buf, size_t max);	/* whole records only */

#ifdef __cplusplus
}
#endif

#endif /* QWZ_MON_H */
