/*
 * qwz_mon - monitor-mode capture on the WCN7850 (see qwz_mon.h).
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#include "qwz_compat.h"
#include "qwzreg.h"
#include "qwzvar.h"
#include "qwz_mon.h"
#include "qwz_awdl0.h"

/* qwz.c keeps these Linux porting macros private; same definitions. */
#ifndef __bf_shf
#define __bf_shf(x) (__builtin_ffsll(x) - 1)
#endif
#ifndef FIELD_GET
#define FIELD_GET(_m, _v) ((typeof(_m))(((_v) & (_m)) >> __bf_shf(_m)))
#endif
#ifndef FIELD_PREP
#define FIELD_PREP(_m, _v) (((typeof(_m))(_v) << __bf_shf(_m)) & (_m))
#endif

/* qwz.c (no shared header: OpenBSD keeps its prototypes in the .c) */
int	qwz_wmi_vdev_create(struct qwz_softc *, uint8_t *, struct vdev_create_params *);
int	qwz_wmi_vdev_start(struct qwz_softc *, struct wmi_vdev_start_req_arg *, int, int);
int	qwz_wmi_vdev_up(struct qwz_softc *, uint32_t, uint32_t, uint32_t, const uint8_t *, uint8_t *, uint32_t, uint32_t);
int	qwz_wmi_vdev_down(struct qwz_softc *, uint32_t, uint8_t);
int	qwz_wmi_vdev_stop(struct qwz_softc *, uint8_t, uint8_t);
int	qwz_wmi_vdev_set_param_cmd(struct qwz_softc *, uint32_t, uint8_t, uint32_t, uint32_t);
int	qwz_wmi_pdev_set_param(struct qwz_softc *, uint32_t, uint32_t, uint8_t);	/* K1 (W3) */
int	qwz_mac_vdev_setup_sync(struct qwz_softc *);
void	qwz_mac_vdev_chan_args(struct qwz_softc *, struct ieee80211_node *, struct wmi_vdev_start_req_arg *);	/* K5 (L1-C4): the station's own vdev-start channel arguments */
int	qwz_mac_setup_vdev_create_params(struct qwz_softc *, struct qwz_vif *, struct qwz_pdev *, struct vdev_create_params *);
int	qwz_mac_op_update_vif_offload(struct qwz_softc *, struct qwz_pdev *, struct qwz_vif *);
int	qwz_get_num_chains(uint32_t);
int	qwz_dp_tx_htt_rx_filter_setup(struct qwz_softc *, uint32_t, int, enum hal_ring_type, size_t, struct htt_rx_ring_tlv_filter *);
int	qwz_dp_rxdma_ring_sel_config_wcn7850(struct qwz_softc *);
void	qwz_hal_srng_access_begin(struct qwz_softc *, struct hal_srng *);
void	qwz_hal_srng_access_end(struct qwz_softc *, struct hal_srng *);
uint32_t *qwz_hal_srng_dst_get_next_entry(struct qwz_softc *, struct hal_srng *);
void	qwz_hal_rx_reo_ent_paddr_get(struct qwz_softc *, void *, uint64_t *, uint32_t *);
void	qwz_hal_rx_buf_addr_info_set(void *, uint64_t, uint32_t, uint8_t);
void	qwz_hal_rx_buf_addr_info_get(void *, uint64_t *, uint32_t *, uint8_t *);
int	qwz_dp_rx_link_desc_return(struct qwz_softc *, uint32_t *, enum hal_wbm_rel_bm_act);
struct qwz_rx_msdu *qwz_dp_rx_reap_desc(struct qwz_softc *, uint32_t, void *);
int	qwz_dp_rxbufs_replenish(struct qwz_softc *, struct dp_rxdma_ring *, void *, int);
struct mbuf *qwz_wmi_alloc_mbuf(size_t);
int	qwz_wmi_cmd_send(struct qwz_pdev_wmi *, struct mbuf *, uint32_t);
int	qwz_mac_mgmt_tx_wmi(struct qwz_softc *, struct qwz_vif *, uint8_t, struct ieee80211_node *, struct mbuf *);
int	qwz_peer_create(struct qwz_softc *, struct qwz_vif *, uint8_t, struct ieee80211_node *, struct peer_create_params *);
int	qwz_wmi_send_peer_create_cmd(struct qwz_softc *, uint8_t, struct peer_create_params *);
int	qwz_wmi_send_peer_delete_cmd(struct qwz_softc *, const uint8_t *, uint8_t, uint8_t);
int	qwz_peer_delete(struct qwz_softc *, uint32_t, uint8_t, struct ath12k_peer *);
struct ath12k_peer *qwz_peer_find_by_addr(struct qwz_softc *, const uint8_t *);
IOCommandGate *qwz_compat_gate(void);
/*
 * qwz.c (merge glue): C++ linkage ON PURPOSE, it is defined there outside extern "C" (__Z18qwz_awdl_vdev_busyP9qwz_softc). Do not move it into a
 * C-linkage block. Nonzero when lane M0.1's listen vdev (awdltest=3..6) or its test lane is up.
 */
int	qwz_awdl_vdev_busy(struct qwz_softc *);
/* K5 (L1-C7): qwz.c, C++ linkage like qwz_awdl_vdev_busy: 1 when the address is awdl0's own (the listen lane's vdev address while it listens) */
int	qwz_mon_awdl_addr_ours(struct qwz_softc *, const uint8_t *);
/* K5 (L1-C14): qwz.c, the station vdev restart used as step 3 of the deaf-station escalation */
int	qwz_mac_vdev_restart(struct qwz_softc *, struct qwz_vif *, int);
/* qwz.c (fix F2): the station bring-up, C++ linkage like qwz_stop (qwzvar.h); the failure path of an in-place stop calls it under the same ioctl lock as enableGated */
int	qwz_init(struct ifnet *);

/* ---- M1 transmit state ----------------------------------------------- */

static void qwz_mon_sender_peer(struct qwz_softc *, const uint8_t *, int);
static int qwz_mon_parse_mac(const char *, uint8_t *);
static struct qwz_softc *qwz_mon_sc;	/* the one softc; set by qwz_mon_attach() */

uint32_t qwz_mon_tx_mode;		/* bit 2: auto unicast reply */
uint32_t qwz_mon_hold;			/* debug.qwz.monhold (qwz_mon.h) */
static int qwz_mon_run_raised;		/* fix F4: THIS cell raised IFF_RUNNING (qwz_mon_start); qwz_mon_stop and qwz_mon_epoch_reset clear the flag only then, not by the monhold value read at stop time */

/* Private KPI (xnu kpi_interface.h under KERNEL_PRIVATE); C linkage. */
#ifdef __cplusplus
extern "C" {
#endif
extern errno_t ifnet_set_eflags(ifnet_t, u_int32_t new_flags, u_int32_t mask);
extern u_int32_t ifnet_eflags(ifnet_t);
/* xnu: the delegate lends its family/subfamily to IFNET_IS_WIFI() & co. */
extern errno_t ifnet_set_delegate(ifnet_t, ifnet_t delegated);
#ifdef __cplusplus
}
#endif
#define QWZ_IFEF_AWDL	0x00100000

int
qwz_mon_awdlif(const char *cmd)
{
	ifnet_t ifp, dlg = NULL;
	char name[IFNAMSIZ], dname[IFNAMSIZ];
	const char *p = cmd, *eq;
	int clear = 0, err;
	size_t n;
	u_int32_t before, after;

	/* "feth0", "feth0=en3" (delegate to the Wi-Fi interface too), "-feth0". */
	if (p[0] == '-') {
		clear = 1;
		p++;
	}
	eq = strchr(p, '=');
	n = eq ? (size_t)(eq - p) : strlen(p);
	if (n == 0 || n >= sizeof(name))
		return EINVAL;
	memcpy(name, p, n);
	name[n] = 0;
	dname[0] = 0;
	if (eq && !clear) {
		if (strlen(eq + 1) == 0 || strlen(eq + 1) >= sizeof(dname))
			return EINVAL;
		strlcpy(dname, eq + 1, sizeof(dname));
	}
	err = ifnet_find_by_name(name, &ifp);
	if (err)
		return err;
	if (dname[0] != 0) {
		err = ifnet_find_by_name(dname, &dlg);
		if (err) {
			ifnet_release(ifp);
			return err;
		}
	}
	if (clear)
		(void)ifnet_set_delegate(ifp, NULL);
	else if (dlg != NULL) {
		err = ifnet_set_delegate(ifp, dlg);
		printf("qwz: mon: awdlif %s: delegate %s -> %d\n", name, dname, err);
	}
	before = ifnet_eflags(ifp);
	err = ifnet_set_eflags(ifp, clear ? 0 : QWZ_IFEF_AWDL, QWZ_IFEF_AWDL);
	after = ifnet_eflags(ifp);
	printf("qwz: mon: awdlif %s: %s IFEF_AWDL -> %d, eflags 0x%08x -> 0x%08x\n",
	    name, clear ? "clear" : "set", err, before, after);
	if (dlg != NULL)
		ifnet_release(dlg);
	ifnet_release(ifp);
	return err;
}

int
qwz_mon_holding(struct ieee80211com *ic)
{
	struct qwz_softc *sc = ic ? (struct qwz_softc *)ic->ic_if.if_softc : NULL;

	/* K5 (L1-C5): a coex cell runs beside a genuinely running station; that is not the hold state (Wi-Fi off/on must run the real stop/init) */
	if (sc != NULL && sc->mon_coex)
		return 0;
	/* merge glue (D1): a torn-down, crashed or DEAD firmware never holds; the next enable then runs the full qwz_init */
	return sc != NULL && qwz_mon_hold != 0 && sc->mon_active && sc->fw_initialized && !QWZ_CRASHED(sc);
}

/* K5 (L1-C5): a coex cell is running beside the station (mon_coex is 1 only then; it is set just before mon_active and cleared with it) */
int
qwz_mon_coex_running(struct qwz_softc *sc)
{
	return sc != NULL && sc->mon_active && sc->mon_coex && sc->fw_initialized && !QWZ_CRASHED(sc);
}

/* K5 (L1-C5): scans must not reach the firmware: the classic hold, or a coex cell with debug.qwz.moncoexscan (live) */
int
qwz_mon_scan_hold(struct ieee80211com *ic)
{
	struct qwz_softc *sc = ic ? (struct qwz_softc *)ic->ic_if.if_softc : NULL;

	return qwz_mon_holding(ic) || (qwz_moncoexscan != 0 && qwz_mon_coex_running(sc));
}

/* K5 (L1-C12): qwz_mon_coex_running for a caller that only has the ieee80211com */
int
qwz_mon_coex_running_ic(struct ieee80211com *ic)
{
	return qwz_mon_coex_running(ic ? (struct qwz_softc *)ic->ic_if.if_softc : NULL);
}

/*
 * K5 (L1-C2): 1 while qwz_mon_start of a coex cell is in progress (mon_coex_setup) or the coex cell runs (mon_coex). The create, start and filter code of
 * a cell takes its coex branches on this, because mon_coex itself is only set just before mon_active.
 */
#define QWZ_MON_COEX_ON(sc)	((sc)->mon_coex || (sc)->mon_coex_setup)
/* K5 (L1-C2): ends the setup window of a coex start (the newstate task waits on it for at most 2 s) */
#define QWZ_MON_COEX_SETUP_END(sc)	do { (sc)->mon_coex_setup = 0; wakeup(&(sc)->mon_coex_setup); } while (0)
/* K5 (L1-C14): the deaf-station probe after a coex cell stop: station frame counters at the stop, and how far the escalation went (0 none, 1 silent logged, 2 filter restored, 3 vdev restarted, 4 init queued) */
static uint32_t qwz_mon_probe_data0, qwz_mon_probe_mgmt0;
static int qwz_mon_probe_step;
static uint8_t mon_psf[512];		/* last PSF heard, without FCS */
static size_t mon_psf_len;
static uint8_t mon_psf_src[6];
static uint64_t mon_last_tx_ns;
static uint16_t mon_tx_seq;
/* merge glue (D7): ItlQwz re-allocates the softc after a failed attach, so "the maps exist" is read from the softc, never from a static flag */
static inline int
qwz_mon_have_maps(struct qwz_softc *sc)
{
	return sc->sc_monvif.txmgmt.data[0].map != NULL;
}
static const uint8_t qwz_awdl_bssid[6] = { 0x00, 0x25, 0x00, 0xff, 0x94, 0x73 };
static const uint8_t qwz_bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/*
 * K5 (L1-C6): the station's management frames while a coex cell runs, tested before the cell's claim. A unicast reply to our address is the station's by
 * A1 alone; a frame of the joined BSS by A3; a group-addressed frame the associated AP sent (beacons keep A3 = BSSID, other broadcasts A2 = the AP).
 */
int
qwz_mon_is_station_frame(struct qwz_softc *sc, const uint8_t *f, size_t len)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *bss = ic->ic_bss;

	if (len < 24)
		return 0;
	if (IEEE80211_ADDR_EQ(f + 4, ic->ic_myaddr) || IEEE80211_ADDR_EQ(f + 4, sc->sc_vif.vdev_addr))
		return 1;
	if (bss != NULL && IEEE80211_ADDR_EQ(f + 16, bss->ni_bssid))
		return 1;
	if ((f[4] & 0x01) && bss != NULL && IEEE80211_ADDR_EQ(f + 10, bss->ni_macaddr))
		return 1;
	return 0;
}

/* K5 (L1-C6): the cell's management frames: on the cell's frequency and in the AWDL BSS (or the debug.qwz.monbssid one), or sent to the cell vdev's address */
int
qwz_mon_claims_mgmt(struct qwz_softc *sc, const uint8_t *f, size_t len, uint32_t chan_freq)
{
	if (len < 24 || chan_freq != sc->mon_freq)
		return 0;
	if (IEEE80211_ADDR_EQ(f + 16, qwz_awdl_bssid))
		return 1;
	if (sc->mon_bssid_set && IEEE80211_ADDR_EQ(f + 16, sc->mon_bssid))
		return 1;
	return IEEE80211_ADDR_EQ(f + 4, sc->sc_monvif.vdev_addr);
}

/* K5 (L1-C7): data frames, see qwz_mon.h. The station's frames are never the cell's; everything not positively the cell's is not claimed. */
int
qwz_mon_claims_data(struct qwz_softc *sc, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *bss = ic->ic_bss;

	(void)a2;
	if (!sc->mon_coex)
		return 1;
	if (IEEE80211_ADDR_EQ(a1, ic->ic_myaddr) || (bss != NULL && IEEE80211_ADDR_EQ(a3, bss->ni_bssid)))
		return 0;
	if (IEEE80211_ADDR_EQ(a3, qwz_awdl_bssid) || (sc->mon_bssid_set && IEEE80211_ADDR_EQ(a3, sc->mon_bssid)) ||
	    IEEE80211_ADDR_EQ(a1, sc->sc_monvif.vdev_addr) || qwz_mon_awdl_addr_ours(sc, a1))
		return 1;
	return 0;
}

/* Management action, category 127, Apple OUI, AWDL type 8, subtype 0 (PSF). */
static int
qwz_mon_is_psf(const uint8_t *f, size_t len)
{
	return len >= 32 && f[0] == 0xd0 && f[24] == 127 &&
	    f[25] == 0x00 && f[26] == 0x17 && f[27] == 0xf2 && f[28] == 8 && f[30] == 0;
}

/* merge glue (D2): what our qwz_wmi_cmd_send returns for a crashed firmware counts as a firmware that stopped answering, like EAGAIN and ETIMEDOUT */
#define QWZ_MON_FW_SILENT(r)	((r) == EAGAIN || (r) == ETIMEDOUT || (r) == ESHUTDOWN)

/*
 * K1 (W1): the two halves of qwz_mon_tx_blocked below, so that the user-space write (qwz_mon_tx_bytes, debug.qwz.monnocred) can answer each exactly:
 * qwz_mon_fw_unavail is "the firmware cannot be used": crashed or DEAD, not initialised, a sleep pending (ENXIO);
 * qwz_mon_no_credit is "no WMI credit right now": the endpoint's credits are all in flight or the starve injection is on (ENOSPC). Same tests, same operands.
 */
static int
qwz_mon_fw_unavail(struct qwz_softc *sc)
{
	if (sc->mon_coex_halt)	/* K5 (L1-C13): the station is leaving, the cell is being stopped: user frames answer ENXIO */
		return 1;
	return QWZ_CRASHED(sc) || !sc->fw_initialized || __atomic_load_n(&qwz_pm_pending, __ATOMIC_ACQUIRE) != 0;
}

/*
 * K5 (L1-C9): beside the station the cell leaves debug.qwz.moncoexcred (default 1) WMI credits to the station: 1 when only that many are left. 0 outside a
 * coex cell and with the switch at 0, so K4 is unchanged. Read live.
 */
static int
qwz_mon_cred_reserved(struct qwz_softc *sc)
{
	struct qwz_pdev_wmi *wmi = &sc->wmi.wmi[sc->pdevs[0].pdev_id];

	if (!sc->mon_coex || qwz_moncoexcred <= 0 || (unsigned int)wmi->eid >= (unsigned int)ATH12K_HTC_EP_COUNT)
		return 0;
	return sc->htc.endpoint[wmi->eid].tx_credits <= qwz_moncoexcred;
}

static int
qwz_mon_no_credit(struct qwz_softc *sc)
{
	struct qwz_pdev_wmi *wmi = &sc->wmi.wmi[sc->pdevs[0].pdev_id];

	if (qwz_mon_cred_reserved(sc))	/* K5 (L1-C9) */
		return 1;
	return (unsigned int)wmi->eid >= (unsigned int)ATH12K_HTC_EP_COUNT || sc->htc.endpoint[wmi->eid].tx_credits == 0 ||
	    (__atomic_load_n(&sc->sc_inject, __ATOMIC_ACQUIRE) & QWZ_INJ_WMI_STARVE) != 0;
}

/*
 * merge glue (D4): OUR credit pre-check, the predicate of qwz_awdltest_tx's fix2 (qwz.c). qwz_wmi_cmd_send waits up to 3 s for a WMI credit with
 * the gate released, and the capture path must never sleep that way: a teardown could run meanwhile. Nonzero: refuse at once with EAGAIN.
 */
static int
qwz_mon_tx_blocked(struct qwz_softc *sc)
{
	struct qwz_pdev_wmi *wmi = &sc->wmi.wmi[sc->pdevs[0].pdev_id];

	if (QWZ_CRASHED(sc) || !sc->fw_initialized || __atomic_load_n(&qwz_pm_pending, __ATOMIC_ACQUIRE) != 0)
		return 1;
	if (sc->mon_coex_halt)	/* K5 (L1-C13) */
		return 1;
	if (qwz_mon_cred_reserved(sc))	/* K5 (L1-C9) */
		return 1;
	if ((unsigned int)wmi->eid >= (unsigned int)ATH12K_HTC_EP_COUNT || sc->htc.endpoint[wmi->eid].tx_credits == 0 ||
	    (__atomic_load_n(&sc->sc_inject, __ATOMIC_ACQUIRE) & QWZ_INJ_WMI_STARVE) != 0)
		return 1;
	return 0;
}

/*
 * K1 W2b: evidence helpers for the 'qwz: mon: lat:' line. They only read the clock and fill sc->mon_lat; nothing here changes what is sent.
 */
static void
qwz_mon_lat_push(uint32_t *a, uint32_t *n, uint32_t *lost, uint32_t cap, uint64_t ns)
{
	uint64_t us = ns / 1000ULL;

	if (*n >= cap) {
		(*lost)++;
		return;
	}
	a[(*n)++] = us > 0xffffffffULL ? 0xffffffffU : (uint32_t)us;
}

static void
qwz_mon_lat_stamp(struct qwz_softc *sc, int slot)
{
	if (slot >= 0 && slot < (int)nitems(sc->mon_lat.tx_ns))
		sc->mon_lat.tx_ns[slot] = getnsecuptime();
}

/*
 * K2c (C2): free one occupied slot of the capture TX ring whose completion never came back. Under the command gate only (the cell timer and the
 * user-frame write both run there). Counts like a completion would (queued, ufr_inflight for kind 2 and 3) and clears the slot's stamp and kind. The
 * caller has checked the age. The first 8 per boot are logged.
 */
static void
qwz_mon_reap_slot(struct qwz_softc *sc, int slot, uint64_t now)
{
	static unsigned int reap_logged;
	struct qwz_txmgmt_queue *q = &sc->sc_monvif.txmgmt;
	struct qwz_tx_data *td = &q->data[slot];
	uint8_t kind = sc->mon_tx_kind[slot];
	uint64_t st = sc->mon_lat.tx_ns[slot];

	if (td->m == NULL)
		return;
	if (reap_logged < 8) {
		reap_logged++;
		printf("qwz: mon: tx reap: slot %d kind %u age %llu ms\n", slot, (unsigned)kind,
		    (unsigned long long)(st != 0 && now > st ? (now - st) / 1000000ULL : 0));
	}
	bus_dmamap_unload(sc->sc_dmat, td->map);
	m_freem(td->m);
	td->m = NULL;
	td->ni = NULL;
	if (q->queued > 0)
		q->queued--;
	if ((kind == 2 || kind == 3) && sc->mon_stats.ufr_inflight > 0)
		sc->mon_stats.ufr_inflight--;
	sc->mon_tx_kind[slot] = 0;
	sc->mon_lat.tx_ns[slot] = 0;
	sc->mon_stats.ufr_reaped++;
}

/* K2c (C2): the cell timer, every 100 ms: free slots older than 3 s. A held slot without a stamp (it cannot happen: every send stamps) gets one now so it ages. */
static void
qwz_mon_reap_stale(struct qwz_softc *sc, uint64_t now)
{
	struct qwz_txmgmt_queue *q = &sc->sc_monvif.txmgmt;
	int i;

	for (i = 0; i < (int)nitems(q->data) && i < (int)nitems(sc->mon_lat.tx_ns); i++) {
		if (q->data[i].m == NULL)
			continue;
		if (sc->mon_lat.tx_ns[i] == 0)
			sc->mon_lat.tx_ns[i] = now;
		else if (now - sc->mon_lat.tx_ns[i] > 3000000000ULL)
			qwz_mon_reap_slot(sc, i, now);
	}
}

/* K2c (C4): age in ms of the oldest slot still held in the capture TX ring (0 = none held); evidence only */
static uint32_t
qwz_mon_oldest_ms(struct qwz_softc *sc, uint64_t now)
{
	struct qwz_txmgmt_queue *q = &sc->sc_monvif.txmgmt;
	uint64_t oldest = 0;
	int i;

	for (i = 0; i < (int)nitems(q->data) && i < (int)nitems(sc->mon_lat.tx_ns); i++)
		if (q->data[i].m != NULL && sc->mon_lat.tx_ns[i] != 0 && now > sc->mon_lat.tx_ns[i] &&
		    now - sc->mon_lat.tx_ns[i] > oldest)
			oldest = now - sc->mon_lat.tx_ns[i];
	return (uint32_t)(oldest / 1000000ULL);
}

void
qwz_mon_lat_credit(struct qwz_softc *sc, int eid, int credits, int what)
{
	uint64_t now;

	if (sc == NULL || !sc->mon_active || credits <= 0 || eid != (int)sc->wmi.wmi[sc->pdevs[0].pdev_id].eid)
		return;
	now = getnsecuptime();
	if (what == 0) {
		if (sc->mon_lat.cr_out == 0)
			sc->mon_lat.cr_stamp = now;
		sc->mon_lat.cr_out += (uint32_t)credits;
	} else if (sc->mon_lat.cr_out > 0) {
		if (what == 1) {
			sc->mon_lat.cr_reports++;
			qwz_mon_lat_push(sc->mon_lat.cr, &sc->mon_lat.ncr, &sc->mon_lat.cr_lost, nitems(sc->mon_lat.cr), now - sc->mon_lat.cr_stamp);
		}
		sc->mon_lat.cr_out = sc->mon_lat.cr_out > (uint32_t)credits ? sc->mon_lat.cr_out - (uint32_t)credits : 0;
		sc->mon_lat.cr_stamp = now;
	}
}

void
qwz_mon_lat_rxring(struct qwz_softc *sc, int ring, int n)
{
	if (sc != NULL && sc->mon_active && ring >= 0 && ring < 4 && n > 0)
		sc->mon_lat.rxring[ring] += (uint32_t)n;
}

/* p50, p90 and max of the first n samples (sorted in place, shell sort); all zero for n == 0 */
static void
qwz_mon_lat_pct(uint32_t *a, uint32_t n, uint32_t *p50, uint32_t *p90, uint32_t *max)
{
	uint32_t gap, i, j, t;

	*p50 = *p90 = *max = 0;
	if (n == 0)
		return;
	for (gap = n / 2; gap > 0; gap /= 2) {
		for (i = gap; i < n; i++) {
			t = a[i];
			for (j = i; j >= gap && a[j - gap] > t; j -= gap)
				a[j] = a[j - gap];
			a[j] = t;
		}
	}
	*p50 = a[n / 2];
	*p90 = a[(n * 9) / 10 < n ? (n * 9) / 10 : n - 1];
	*max = a[n - 1];
}

/*
 * Replay the kept PSF as ours: sender = the capture vdev's MAC, so the
 * hardware recognises the ACK; BSSID unchanged; new sequence number.
 */
static int
qwz_mon_tx_copy(struct qwz_softc *sc, const uint8_t *dst)
{
	struct qwz_vif *vif = &sc->sc_monvif;
	struct mbuf *m;
	uint8_t *p;
	int ret;
	int slot_lat;	/* K1 W2b */

	if (qwz_mon_tx_blocked(sc))	/* merge glue (D4) */
		return EAGAIN;
	if (!sc->mon_active || !vif->is_up || mon_psf_len == 0 || !qwz_mon_have_maps(sc))
		return ENXIO;
	m = m_gethdr(M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return ENOBUFS;
	MCLGET(m, M_DONTWAIT);
	if ((M_FLAGS(m) & M_EXT) == 0) {
		m_freem(m);
		return ENOBUFS;
	}
	M_SETLEN(m, mon_psf_len);
	p = mtod(m, uint8_t *);
	memcpy(p, mon_psf, mon_psf_len);
	p[2] = p[3] = 0;				/* duration */
	memcpy(p + 4, dst, 6);				/* addr1 */
	memcpy(p + 10, vif->vdev_addr, 6);		/* addr2: ours */
	p[22] = (uint8_t)((mon_tx_seq & 0x0f) << 4);	/* sequence control */
	p[23] = (uint8_t)(mon_tx_seq >> 4);
	mon_tx_seq = (mon_tx_seq + 1) & 0x0fff;

	slot_lat = vif->txmgmt.cur;	/* K1 W2b */
	ret = qwz_mac_mgmt_tx_wmi(sc, vif, sc->pdevs[0].pdev_id, NULL, m);
	if (ret) {
		if (ret != ENOBUFS)	/* ENOBUFS: the mbuf was already freed */
			m_freem(m);
		sc->mon_stats.tx_fail++;
		return ret;
	}
	qwz_mon_lat_stamp(sc, slot_lat);	/* K1 W2b: completion latency starts here */
	sc->mon_lat.tx_bytes += mon_psf_len;
	sc->mon_lat.tx_frames++;
	sc->mon_stats.tx_sent++;
	mon_last_tx_ns = getnsecuptime();
	return 0;
}

/* Every captured frame passes here (under the gate) before the ring. */
static void
qwz_mon_rx_hook(struct qwz_softc *sc, const uint8_t *frame, size_t len, uint16_t flags)
{
	if ((flags & QWZ_MONCAP_F_FCS) && len >= 4)
		len -= 4;
	if (!qwz_mon_is_psf(frame, len) || len > sizeof(mon_psf) ||
	    memcmp(frame + 16, qwz_awdl_bssid, 6) != 0)
		return;
	sc->mon_stats.rx_psf++;
	memcpy(mon_psf, frame, len);
	mon_psf_len = len;
	memcpy(mon_psf_src, frame + 10, 6);
	if ((qwz_mon_flags & QWZ_MON_F_PEER_SENDER) && !sc->mon_sender_peer)
		qwz_mon_sender_peer(sc, mon_psf_src, 1);
	if ((qwz_mon_tx_mode & 2) && getnsecuptime() - mon_last_tx_ns > 50000000ULL &&
	    !qwz_mon_tx_blocked(sc))	/* merge glue (D4): no credit, no reply (never sleep here) */
		qwz_mon_tx_copy(sc, mon_psf_src);	/* the peer is on our channel right now */
}

int
qwz_mon_tx_compl(struct qwz_softc *sc, struct wmi_mgmt_tx_compl_event *ev)
{
	struct qwz_txmgmt_queue *q = &sc->sc_monvif.txmgmt;
	struct qwz_tx_data *td;

	/*
	 * K5 (L1-C8): beside the station the ids are partitioned: 0 to QWZ_MON_COEX_STA_SLOTS-1 are the station's ring, the cell's are the rest (the lane M
	 * pool starts at 64 and was handled by the caller). No slot-occupancy guess: a station id is never the cell's, a cell id never the station's.
	 */
	if (sc->mon_active && sc->mon_coex) {
		if (ev->desc_id < QWZ_MON_COEX_STA_SLOTS)
			return 0;
		if (ev->desc_id < nitems(q->data) && q->data[ev->desc_id].m == NULL) {
			sc->mon_stats.ufr_compl_nom++;
			return 1;
		}
	}
	/* K2c (C3): the three silent early returns below are counted (ufr_compl_range / _nom / _sta) so a lost completion is no longer invisible */
	if (!sc->mon_active)	/* outside a cell every completion is the station's: not counted */
		;
	else if (ev->desc_id >= nitems(q->data))
		sc->mon_stats.ufr_compl_range++;
	else if (q->data[ev->desc_id].m == NULL)
		sc->mon_stats.ufr_compl_nom++;
	else if (sc->sc_vif.txmgmt.data[ev->desc_id].m != NULL)
		sc->mon_stats.ufr_compl_sta++;
	if (ev->desc_id >= nitems(q->data))
		return 0;
	td = &q->data[ev->desc_id];
	if (td->m == NULL || sc->sc_vif.txmgmt.data[ev->desc_id].m != NULL)
		return 0;
	if (sc->mon_lat.tx_ns[ev->desc_id] != 0 && ev->desc_id < nitems(sc->mon_lat.tx_ns)) {	/* K1 W2b: completion latency */
		qwz_mon_lat_push(sc->mon_lat.cl, &sc->mon_lat.ncl, &sc->mon_lat.cl_lost, nitems(sc->mon_lat.cl),
		    getnsecuptime() - sc->mon_lat.tx_ns[ev->desc_id]);
		sc->mon_lat.tx_ns[ev->desc_id] = 0;
	}
	bus_dmamap_unload(sc->sc_dmat, td->map);
	m_freem(td->m);
	td->m = NULL;
	td->ni = NULL;
	if (q->queued > 0)
		q->queued--;
	switch (sc->mon_tx_kind[ev->desc_id]) {
	case 1: if (ev->status == 0) sc->mon_stats.ufr_mgmt_ok++; else sc->mon_stats.ufr_mgmt_bad++; break;
	case 2: if (ev->status == 0) sc->mon_stats.ufr_data_ok++; else sc->mon_stats.ufr_data_bad++; break;
	case 3: if (ev->status == 0) sc->mon_stats.ufr_ctrl_ok++; else sc->mon_stats.ufr_ctrl_bad++; break;
	default: break;
	}
	if ((sc->mon_tx_kind[ev->desc_id] == 2 || sc->mon_tx_kind[ev->desc_id] == 3) && sc->mon_stats.ufr_inflight > 0)
		sc->mon_stats.ufr_inflight--;	/* K2b (W1b): a user DATA or CONTROL frame left the ring (never below zero) */
	sc->mon_tx_kind[ev->desc_id] = 0;
	/* WMI_MGMT_TX_COMP_TYPE_*: 0 complete ok, 1 discard, 2 inspect, 3 complete no ack. */
	if (ev->status == 0) {
		sc->mon_stats.tx_ok++;
		sc->mon_stats.last_ack_rssi = (int32_t)ev->ack_rssi;
	} else if (ev->status == 1)
		sc->mon_stats.tx_discard++;
	else if (ev->status == 2)
		sc->mon_stats.tx_inspect++;
	else if (ev->status == 3)
		sc->mon_stats.tx_noack++;
	else
		sc->mon_stats.tx_other++;
	if (qwz_mon_flags & QWZ_MON_F_VERBOSE)
		printf("qwz: mon: tx compl desc %u status %u ack_rssi %d\n", ev->desc_id,
		    ev->status, (int32_t)ev->ack_rssi);
	return 1;
}

/*
 * K4 (M1), debug.qwz.monfmt: the per-frame header of a user frame (qwz_montx_hdr) reaches qwz_mon_tx_bytes through these two
 * statics, which qwz_mon_txframe_gated sets and clears around its one call (it runs inside the command gate); the other caller path
 * (qwz_mon_tx_copy) never sets qwz_mon_fmt_user, so its frames always go with tx_params_valid 0. The presets are the lane M ones
 * (QWZ_AWDL_TXP_MC0 / UC0 / DATA1 in qwz.c: 20 dBm, 1 stream, 20 MHz, OFDM preamble).
 */
#define QWZ_MON_TXP_MC0		0x10104028u
#define QWZ_MON_TXP_UC0		0x4011c028u
#define QWZ_MON_TXP_DATA1	0x00108400u
static int qwz_mon_fmt_user;
static uint16_t qwz_mon_fmt_flags;
static uint8_t qwz_mon_fmt_rate;

/* The daemon's rate byte (500 kbit/s units) as the index of the OFDM rate in the firmware's mcs_mask (bit 4 + index): 6, 9, 12, 18, 24, 36, 48, 54 Mbit/s; zero, CCK and anything else is OFDM 12 (index 2) */
static int
qwz_mon_fmt_rate_idx(uint8_t rate)
{
	switch (rate) {
	case 12: return 0;
	case 18: return 1;
	case 24: return 2;
	case 36: return 3;
	case 48: return 4;
	case 72: return 5;
	case 96: return 6;
	case 108: return 7;
	}
	return 2;
}

/* 1 and the two tx_params dwords when this user frame is sent with a tx_params block under debug.qwz.monfmt, else 0 */
static int
qwz_mon_fmt_pick(uint8_t kind, size_t len, uint32_t *d0, uint32_t *d1, unsigned *retry, unsigned *rate)
{
	int mode = qwz_monfmt > 2 ? 2 : qwz_monfmt;	/* 3 (offchan data command) is not implemented: acts as 2 */
	uint32_t preset = QWZ_MON_TXP_MC0;
	unsigned r = 2, idx = 2;

	if (mode <= 0 || !qwz_mon_fmt_user)
		return 0;
	if (kind == 2 && mode >= 2) {
		preset = QWZ_MON_TXP_UC0;
		r = (qwz_mon_fmt_flags & QWZ_MONTX_F_NO_ACK) ? 1 : (QWZ_MON_TXP_UC0 >> 28);
		idx = qwz_mon_fmt_rate_idx(qwz_mon_fmt_rate);
	} else if (len >= 150)
		return 0;
	*d0 = (preset & 0x000000ffu) | ((1u << (4 + idx)) << 8) | (preset & 0x0ff00000u) | (r << 28);
	*d1 = kind == 2 ? QWZ_MON_TXP_DATA1 : (QWZ_MON_TXP_DATA1 & ~(1u << 20));	/* frame_type bit: Data = 1, management and control 0 */
	*retry = r;
	{
		static const uint8_t units[8] = { 12, 18, 24, 36, 48, 72, 96, 108 };

		*rate = units[idx];	/* 500 kbit/s units, as logged */
	}
	return 1;
}

/* Transmit `frame` (no FCS) on the capture vdev; the slot remembers the frame type. */
static int
qwz_mon_tx_bytes(struct qwz_softc *sc, const uint8_t *frame, size_t len, uint8_t kind)
{
	struct qwz_vif *vif = &sc->sc_monvif;
	struct mbuf *m;
	int slot, ret;
	int fmt_used = 0;	/* K4 (M1) */
	static int fmt_logged, fmt_fail_logged;	/* K4 (M1): first 8 tx_params frames and first 4 fallbacks per boot */

	if (sc->mon_coex_halt)	/* K5 (L1-C13): the cell is being stopped because the station leaves RUN */
		return ENXIO;

	/* K1 (W1), debug.qwz.monnocred: XNU re-runs a sysctl handler while it returns EAGAIN, so EAGAIN here spins inside the kernel; answer exactly instead (the checks below stay for monnocred 0) */
	if (qwz_monnocred) {
		if (qwz_mon_fw_unavail(sc))
			return ENXIO;
		if (qwz_mon_no_credit(sc)) {
			sc->mon_stats.ufr_nocred++;
			return ENOSPC;
		}
	}
	if (qwz_mon_tx_blocked(sc))	/* merge glue (D4) */
		return EAGAIN;
	if (!sc->mon_active || !vif->is_up || !qwz_mon_have_maps(sc))
		return ENXIO;
	if (len < 10 || len > QWZ_MONTX_MAXFRAME)
		return EINVAL;
	/* K1 (W1), debug.qwz.monqmax: DATA and CONTROL frames (kind 2, 3) stop at N IN FLIGHT; management frames (kind 1, PSF/MIF sync) keep all 63 slots and are never counted. K2b: N is compared with ufr_inflight (user data and control frames only), NOT with txmgmt.queued, which also holds the management sync frames and starved every data frame at N below the management queue depth */
	if (qwz_monqmax != 0 && kind != 1 &&
	    sc->mon_stats.ufr_inflight >= (qwz_monqmax > 63U ? 63U : qwz_monqmax)) {
		sc->mon_stats.ufr_qmax++;
		return ENOSPC;
	}
	if (sc->mon_coex && vif->txmgmt.queued >= (int)nitems(vif->txmgmt.data) - 1 - QWZ_MON_COEX_STA_SLOTS)	/* K5 (L1-C8): the cell's ring is ids 8 to 63 */
		return ENOSPC;
	if (vif->txmgmt.queued >= (int)nitems(vif->txmgmt.data) - 1)
		return ENOSPC;
	/* K2c (C2), debug.qwz.monreap: the slot the next send takes is still occupied (a lost completion, then a wrap): qwz_mac_mgmt_tx_wmi would overwrite its mbuf and leak it. Reap it when it is older than 250 ms, else refuse */
	if (qwz_monreap && vif->txmgmt.cur >= 0 && vif->txmgmt.cur < (int)nitems(vif->txmgmt.data) &&
	    vif->txmgmt.data[vif->txmgmt.cur].m != NULL) {
		int cs = vif->txmgmt.cur;
		uint64_t now = getnsecuptime(), st = cs < (int)nitems(sc->mon_lat.tx_ns) ? sc->mon_lat.tx_ns[cs] : 0;

		if (st != 0 && now - st > 250000000ULL)
			qwz_mon_reap_slot(sc, cs, now);
		else {
			if (st == 0 && cs < (int)nitems(sc->mon_lat.tx_ns))
				sc->mon_lat.tx_ns[cs] = now;
			sc->mon_stats.ufr_livefull++;
			return ENOSPC;
		}
	}
	m = m_gethdr(M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return ENOBUFS;
	if (len <= MCLBYTES)
		MCLGET(m, M_DONTWAIT);
	else
		MCLGETL(m, M_DONTWAIT, len);
	if ((M_FLAGS(m) & M_EXT) == 0) {
		m_freem(m);
		return ENOBUFS;
	}
	M_SETLEN(m, len);
	memcpy(mtod(m, uint8_t *), frame, len);
	slot = vif->txmgmt.cur;
	{	/* K4 (M1), debug.qwz.monfmt: this frame goes with a tx_params block (one-shot fields of sc, read by qwz_wmi_mgmt_send) */
		uint32_t fd0 = 0, fd1 = 0;
		unsigned fretry = 0, frate = 0;

		fmt_used = qwz_mon_fmt_pick(kind, len, &fd0, &fd1, &fretry, &frate);
		if (fmt_used) {
			sc->mon_txp_d0 = fd0;
			sc->mon_txp_d1 = fd1;
			sc->mon_txp_err = 0;
			sc->mon_txp_valid = 1;
			if (fmt_logged < 8) {
				fmt_logged++;
				printf("qwz: mon: fmt: kind %u len %u retry %u rate %u params 0x%08x 0x%08x\n", (unsigned)kind, (unsigned)len, fretry, frate, fd0, fd1);
			}
		}
	}
	ret = qwz_mac_mgmt_tx_wmi(sc, vif, sc->pdevs[0].pdev_id, NULL, m);
	if (fmt_used) {	/* K4 (M1): a failed tx_params send must not stop the cell: the same frame goes again as the plain command */
		sc->mon_txp_valid = 0;
		if (ret == 0)
			sc->mon_stats.ufr_fmt++;
		else if (sc->mon_txp_err != 0 && ret != ESHUTDOWN) {
			sc->mon_stats.ufr_fmtfail++;
			if (fmt_fail_logged < 4) {
				fmt_fail_logged++;
				printf("qwz: mon: fmt: the tx_params send failed (%d), the frame goes without\n", ret);
			}
			sc->mon_txp_err = 0;
			ret = qwz_mac_mgmt_tx_wmi(sc, vif, sc->pdevs[0].pdev_id, NULL, m);
		}
	}
	if (ret) {
		if (ret != ENOBUFS)
			m_freem(m);
		return ret;
	}
	sc->mon_tx_kind[slot] = kind;
	if (kind == 2 || kind == 3)
		sc->mon_stats.ufr_inflight++;	/* K2b (W1b): counted only after the firmware accepted the frame */
	qwz_mon_lat_stamp(sc, slot);	/* K1 W2b */
	sc->mon_lat.tx_bytes += len;
	sc->mon_lat.tx_frames++;
	return 0;
}

static IOReturn
qwz_mon_txframe_gated(OSObject *, void *arg0, void *arg1, void *arg2, void *)
{
	struct qwz_softc *sc = qwz_mon_sc;
	const uint8_t *buf = (const uint8_t *)arg0;
	size_t len = (size_t)(uintptr_t)arg1;
	int *errp = (int *)arg2;
	const struct qwz_montx_hdr *h = (const struct qwz_montx_hdr *)buf;
	const uint8_t *frame = buf + sizeof(*h);
	size_t flen = len - sizeof(*h);
	uint8_t type, kind;
	int ret;

	if (sc == NULL) {
		*errp = ENXIO;
		return kIOReturnSuccess;
	}
	sc->mon_stats.ufr_gate++;	/* K2c (C4): evidence only */
	type = (frame[0] >> 2) & 3;
	kind = type == 0 ? 1 : type == 2 ? 2 : 3;
	qwz_mon_fmt_flags = h->flags;	/* K4 (M1): the per-frame header, for qwz_mon_fmt_pick */
	qwz_mon_fmt_rate = h->rate;
	qwz_mon_fmt_user = 1;
	ret = qwz_mon_tx_bytes(sc, frame, flen, kind);
	qwz_mon_fmt_user = 0;
	if (ret == 0)
		sc->mon_stats.ufr_sent++;
	else if (ret == ENOSPC)
		sc->mon_stats.ufr_full++;
	else
		sc->mon_stats.ufr_fail++;
	(void)h;	/* flags and rate are recorded by the caller for now */
	*errp = ret;
	return kIOReturnSuccess;
}

int
qwz_mon_tx_frame(const void *buf, size_t len)
{
	IOCommandGate *gate = qwz_compat_gate();
	int err = ENXIO;

	if (gate == NULL || qwz_mon_sc == NULL)
		return ENXIO;
	if (len < sizeof(struct qwz_montx_hdr) + 10 ||
	    len > sizeof(struct qwz_montx_hdr) + QWZ_MONTX_MAXFRAME)
		return EINVAL;
	gate->runAction(qwz_mon_txframe_gated, (void *)buf, (void *)(uintptr_t)len, &err);
	return err;
}

static IOReturn
qwz_mon_peer_gated(OSObject *, void *arg0, void *arg1, void *, void *)
{
	struct qwz_softc *sc = qwz_mon_sc;
	const char *cmd = (const char *)arg0;
	int *errp = (int *)arg1;
	uint8_t mac[6];

	if (sc == NULL || !sc->mon_active) {
		*errp = ENXIO;
		return kIOReturnSuccess;
	}
	if (strncmp(cmd, "add ", 4) == 0 && qwz_mon_parse_mac(cmd + 4, mac)) {
		if (sc->mon_sender_peer && memcmp(sc->mon_sender_addr, mac, 6) != 0)
			qwz_mon_sender_peer(sc, NULL, 0);	/* one peer at a time for now */
		qwz_mon_sender_peer(sc, mac, 1);
		*errp = sc->mon_sender_peer ? 0 : EIO;
	} else if (strncmp(cmd, "del", 3) == 0) {
		qwz_mon_sender_peer(sc, NULL, 0);
		*errp = 0;
	} else
		*errp = EINVAL;
	return kIOReturnSuccess;
}

int
qwz_mon_peer(const char *cmd)
{
	IOCommandGate *gate = qwz_compat_gate();
	int err = ENXIO;

	if (gate == NULL || qwz_mon_sc == NULL)
		return ENXIO;
	gate->runAction(qwz_mon_peer_gated, (void *)cmd, &err);
	return err;
}

static void
qwz_mon_txq_purge(struct qwz_softc *sc)
{
	struct qwz_txmgmt_queue *q = &sc->sc_monvif.txmgmt;
	int i;

	for (i = 0; i < nitems(q->data); i++) {
		if (q->data[i].m) {
			bus_dmamap_unload(sc->sc_dmat, q->data[i].map);
			m_freem(q->data[i].m);
			q->data[i].m = NULL;
		}
		q->data[i].ni = NULL;
	}
	q->cur = q->queued = 0;
	if (sc->mon_active && sc->mon_coex)	/* K5 (L1-C8): a purge of a running coex cell starts at the cell's first id (the stop and teardown paths run with mon_active 0: cur 0 as K4) */
		q->cur = QWZ_MON_COEX_STA_SLOTS;
	memset(sc->mon_lat.tx_ns, 0, sizeof(sc->mon_lat.tx_ns));	/* K2c (C3): no per-slot stamp or kind outlives the slots */
	memset(sc->mon_tx_kind, 0, sizeof(sc->mon_tx_kind));
}

/*
 * Two firmware-level receive filters that Linux ath12k never sends but the
 * firmware API (wmi_unified.h) documents; experiments for frames the HTT
 * filter alone does not deliver.
 */
struct qwz_wmi_mnt_filter_cmd {
	uint32_t tlv_header;
	uint32_t vdev_id;
	uint32_t clear_or_set;
	uint32_t configure_type;	/* 0 mgmt, 1 ctrl, 2 data, 3 all */
} __packed;
#define QWZ_WMI_MNT_FILTER_CONFIG_ALL	3

struct qwz_wmi_pdev_set_rx_filter_promiscuous_cmd {
	uint32_t tlv_header;
	uint32_t pdev_id;
	uint32_t rx_filter_promiscuous_enable;
} __packed;

static int
qwz_mon_wmi_mnt_filter(struct qwz_softc *sc, uint32_t vdev_id, int set)
{
	struct qwz_pdev_wmi *wmi = &sc->wmi.wmi[0];
	struct qwz_wmi_mnt_filter_cmd *cmd;
	struct mbuf *m = qwz_wmi_alloc_mbuf(sizeof(*cmd));
	int ret;

	if (m == NULL)
		return ENOMEM;
	cmd = (struct qwz_wmi_mnt_filter_cmd *)(mtod(m, uint8_t *) +
	    sizeof(struct ath12k_htc_hdr) + sizeof(struct wmi_cmd_hdr));
	cmd->tlv_header = FIELD_PREP(WMI_TLV_TAG, WMI_TAG_MNT_FILTER_CMD) |
	    FIELD_PREP(WMI_TLV_LEN, sizeof(*cmd) - TLV_HDR_SIZE);
	cmd->vdev_id = vdev_id;
	cmd->clear_or_set = set ? 1 : 0;
	cmd->configure_type = QWZ_WMI_MNT_FILTER_CONFIG_ALL;
	ret = qwz_wmi_cmd_send(wmi, m, WMI_MNT_FILTER_CMDID);
	if (ret)
		m_freem(m);
	printf("qwz: mon: WMI_MNT_FILTER vdev %u %s: %d\n", vdev_id, set ? "set" : "clear", ret);
	return ret;
}

struct qwz_wmi_vdev_filter_nrp_cmd {
	uint32_t tlv_header;
	uint32_t vdev_id;
	uint32_t addr[2];	/* wmi_mac_addr */
	uint32_t action;	/* 1 add, 2 remove */
	uint32_t type;		/* 1 AP BSSID, 2 station MAC */
	uint32_t flag;		/* 1 capture RX only */
	uint32_t bssid_idx;
} __packed;

static int
qwz_mon_wmi_nrp(struct qwz_softc *sc, uint32_t vdev_id, const uint8_t *mac, int add, int sta)
{
	struct qwz_pdev_wmi *wmi = &sc->wmi.wmi[0];
	struct qwz_wmi_vdev_filter_nrp_cmd *cmd;
	struct mbuf *m = qwz_wmi_alloc_mbuf(sizeof(*cmd));
	int ret;

	if (m == NULL)
		return ENOMEM;
	cmd = (struct qwz_wmi_vdev_filter_nrp_cmd *)(mtod(m, uint8_t *) +
	    sizeof(struct ath12k_htc_hdr) + sizeof(struct wmi_cmd_hdr));
	cmd->tlv_header = FIELD_PREP(WMI_TLV_TAG, WMI_TAG_VDEV_FILTER_NRP_CONFIG_CMD) |
	    FIELD_PREP(WMI_TLV_LEN, sizeof(*cmd) - TLV_HDR_SIZE);
	cmd->vdev_id = vdev_id;
	memcpy(cmd->addr, mac, 6);	/* wmi_mac_addr: bytes 0-3 in word 0, 4-5 low in word 1 */
	cmd->action = add ? 1 : 2;
	cmd->type = sta ? 2 : 1;
	cmd->flag = 1;
	cmd->bssid_idx = 0;
	ret = qwz_wmi_cmd_send(wmi, m, WMI_VDEV_FILTER_NEIGHBOR_RX_PACKETS_CMDID);
	if (ret)
		m_freem(m);
	printf("qwz: mon: WMI neighbour-rx filter vdev %u %s %s (%s): %d\n", vdev_id,
	    add ? "add" : "remove", ether_sprintf((u_char *)mac), sta ? "sta" : "bssid", ret);
	return ret;
}

static int
qwz_mon_wmi_promisc(struct qwz_softc *sc, int enable)
{
	struct qwz_pdev_wmi *wmi = &sc->wmi.wmi[0];
	struct qwz_wmi_pdev_set_rx_filter_promiscuous_cmd *cmd;
	struct mbuf *m = qwz_wmi_alloc_mbuf(sizeof(*cmd));
	int ret;

	if (m == NULL)
		return ENOMEM;
	cmd = (struct qwz_wmi_pdev_set_rx_filter_promiscuous_cmd *)(mtod(m, uint8_t *) +
	    sizeof(struct ath12k_htc_hdr) + sizeof(struct wmi_cmd_hdr));
	cmd->tlv_header = FIELD_PREP(WMI_TLV_TAG, WMI_TAG_PDEV_SET_RX_FILTER_PROMISCUOUS_CMD) |
	    FIELD_PREP(WMI_TLV_LEN, sizeof(*cmd) - TLV_HDR_SIZE);
	cmd->pdev_id = sc->pdevs[0].pdev_id;
	cmd->rx_filter_promiscuous_enable = enable ? 1 : 0;
	ret = qwz_wmi_cmd_send(wmi, m, WMI_PDEV_SET_RX_FILTER_PROMISCUOUS_CMDID);
	if (ret)
		m_freem(m);
	printf("qwz: mon: WMI_PDEV_SET_RX_FILTER_PROMISCUOUS %d: %d (service bit %s)\n", enable, ret,
	    isset(sc->wmi.svc_map, WMI_TLV_SERVICE_RX_PROMISC_ENABLE_SUPPORT) ? "advertised" : "absent");
	return ret;
}

uint32_t qwz_mon_flags = QWZ_MON_F_OFFSETS | QWZ_MON_F_RXMON | QWZ_MON_F_REO;
char qwz_mon_bssid_str[32];

/* "xx:xx:xx:xx:xx:xx" -> 6 bytes; 0 when empty or malformed. */
static int
qwz_mon_parse_mac(const char *s, uint8_t *out)
{
	int i, v;

	if (s[0] == 0)
		return 0;
	for (i = 0; i < 6; i++) {
		char a = s[i * 3], b = s[i * 3 + 1];
		int ha = (a >= '0' && a <= '9') ? a - '0' : (a >= 'a' && a <= 'f') ? a - 'a' + 10 : (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
		int hb = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
		if (ha < 0 || hb < 0)
			return 0;
		v = ha * 16 + hb;
		out[i] = (uint8_t)v;
		if (i < 5 && s[i * 3 + 2] != ':')
			return 0;
	}
	return 1;
}


/* ---- capture ring ----------------------------------------------------- */

#define QWZ_MONCAP_SIZE		(512 * 1024)
#define QWZ_MONCAP_MAXFRAME	4096

static uint8_t *moncap_ring;
static size_t moncap_head, moncap_tail;	/* producer, consumer */
static IOLock *moncap_lock;
static uint32_t moncap_seq, moncap_dropped;

static size_t
moncap_free(void)
{
	return (moncap_tail + QWZ_MONCAP_SIZE - moncap_head - 1) % QWZ_MONCAP_SIZE;
}

static void
moncap_put(const uint8_t *frame, size_t len, uint16_t flags, uint8_t decap,
    uint8_t chan, int8_t rssi, uint8_t rate)
{
	struct qwz_moncap_hdr hdr;
	size_t rec = sizeof(hdr) + ((len + 3) & ~(size_t)3), need = rec;

	if (qwz_mon_sc != NULL) {	/* K1 W2b: bytes received through the cell (every frame that reaches the capture path) */
		qwz_mon_sc->mon_lat.rx_bytes += len;
		qwz_mon_sc->mon_lat.rx_frames++;
	}
	if (moncap_ring == NULL)
		return;
	IOLockLock(moncap_lock);
	if (moncap_head + rec > QWZ_MONCAP_SIZE)
		need += QWZ_MONCAP_SIZE - moncap_head;	/* the tail of the buffer is skipped */
	if (need > moncap_free()) {
		moncap_dropped++;
		IOLockUnlock(moncap_lock);
		return;
	}
	if (moncap_head + rec > QWZ_MONCAP_SIZE) {
		if (QWZ_MONCAP_SIZE - moncap_head >= sizeof(uint32_t)) {
			uint32_t skip = QWZ_MONCAP_SKIP;
			memcpy(moncap_ring + moncap_head, &skip, sizeof(skip));
		}
		moncap_head = 0;
	}
	uint64_t us = getnsecuptime() / 1000ULL;

	hdr.len = (uint32_t)len;
	hdr.ts_lo = (uint32_t)us;
	hdr.flags = flags;
	hdr.decap = decap;
	hdr.chan = chan;
	hdr.seq = moncap_seq++;
	hdr.rssi = rssi;
	hdr.rate = rate;
	hdr.ts_hi = (uint16_t)(us >> 32);
	memcpy(moncap_ring + moncap_head, &hdr, sizeof(hdr));
	memcpy(moncap_ring + moncap_head + sizeof(hdr), frame, len);
	moncap_head = (moncap_head + rec) % QWZ_MONCAP_SIZE;
	IOLockUnlock(moncap_lock);
}

size_t
qwz_mon_cap_pending(void)
{
	size_t n;

	if (moncap_ring == NULL)
		return 0;
	IOLockLock(moncap_lock);
	n = (moncap_head + QWZ_MONCAP_SIZE - moncap_tail) % QWZ_MONCAP_SIZE;
	IOLockUnlock(moncap_lock);
	return n;
}

size_t
qwz_mon_cap_drain(void *buf, size_t max)
{
	size_t n = 0, rec;
	uint32_t len;

	if (moncap_ring == NULL)
		return 0;
	IOLockLock(moncap_lock);
	while (moncap_tail != moncap_head) {
		if (QWZ_MONCAP_SIZE - moncap_tail < sizeof(struct qwz_moncap_hdr)) {
			moncap_tail = 0;
			continue;
		}
		memcpy(&len, moncap_ring + moncap_tail, sizeof(len));
		if (len == QWZ_MONCAP_SKIP) {
			moncap_tail = 0;
			continue;
		}
		if (len > QWZ_MONCAP_MAXFRAME) {	/* corrupt ring: give up on it */
			moncap_tail = moncap_head;
			break;
		}
		rec = sizeof(struct qwz_moncap_hdr) + ((len + 3) & ~3U);
		if (n + rec > max)
			break;
		memcpy((uint8_t *)buf + n, moncap_ring + moncap_tail, rec);
		n += rec;
		moncap_tail = (moncap_tail + rec) % QWZ_MONCAP_SIZE;
	}
	IOLockUnlock(moncap_lock);
	return n;
}

/* ---- channel ------------------------------------------------------------ */

static uint32_t
qwz_mon_chan_freq(int chan)
{
	if (chan >= 1 && chan <= 13)
		return 2407 + 5 * chan;
	if (chan == 14)
		return 2484;
	if (chan >= 36 && chan <= 177)
		return 5000 + 5 * chan;
	return 0;
}

/* ---- receive filter ------------------------------------------------------ */

/*
 * The monitor filter on the RXDMA buffer rings, as Linux
 * ath12k_dp_tx_htt_rx_monitor_mode_ring_config() sets it for a chip without
 * rxdma1: monitor-destination TLVs, logging of all three frame types, and
 * the filter-pass plus monitor-other packet filters. Unlike Linux we keep
 * the explicit TLV offsets the station path needs on this firmware
 * (qwz_dp_rxdma_ring_sel_config_wcn7850), so popped buffers keep the layout
 * the descriptor getters expect; QWZ_MON_F_OFFSETS turns that off.
 */
static int
qwz_mon_rx_filter(struct qwz_softc *sc, int enable)
{
	struct qwz_dp *dp = &sc->dp;
	struct htt_rx_ring_tlv_filter f;
	int i, ret;

	if (!enable)
		return qwz_dp_rxdma_ring_sel_config_wcn7850(sc);

	memset(&f, 0, sizeof(f));
	f.rx_filter = HTT_RX_MON_FILTER_TLV_FLAGS_MON_DEST_RING;
	f.drop_threshold_valid = 1;
	f.rx_drop_threshold = HTT_RX_RING_TLV_DROP_THRESHOLD_VALUE;
	f.enable_log_mgmt_type = 1;
	f.enable_log_ctrl_type = 1;
	f.enable_log_data_type = 1;
	f.conf_len_mgmt = HTT_RX_RING_DEFAULT_DMA_LENGTH;
	f.conf_len_ctrl = HTT_RX_RING_DEFAULT_DMA_LENGTH;
	f.conf_len_data = HTT_RX_RING_DEFAULT_DMA_LENGTH;
	f.enable_rx_tlv_offset = 1;
	f.rx_tlv_offset = HTT_RX_RING_PKT_TLV_OFFSET;
	f.en_rxmon = !!(qwz_mon_flags & QWZ_MON_F_RXMON);
	f.pkt_filter_flags0 = HTT_RX_MON_FP_MGMT_FILTER_FLAGS0;
	f.pkt_filter_flags1 = HTT_RX_MON_FP_MGMT_FILTER_FLAGS1;
	f.pkt_filter_flags2 = HTT_RX_MON_FP_CTRL_FILTER_FLASG2;
	f.pkt_filter_flags3 = HTT_RX_MON_FP_CTRL_FILTER_FLASG3 |
	    HTT_RX_MON_FP_DATA_FILTER_FLASG3;
	if (!(qwz_mon_flags & QWZ_MON_F_FP_ONLY)) {
		f.pkt_filter_flags0 |= HTT_RX_MON_MO_MGMT_FILTER_FLAGS0;
		f.pkt_filter_flags1 |= HTT_RX_MON_MO_MGMT_FILTER_FLAGS1;
		f.pkt_filter_flags2 |= HTT_RX_MON_MO_CTRL_FILTER_FLASG2;
		f.pkt_filter_flags3 |= HTT_RX_MON_MO_CTRL_FILTER_FLASG3 |
		    HTT_RX_MON_MO_DATA_FILTER_FLASG3;
	}
	if (QWZ_MON_COEX_ON(sc) && qwz_moncoexfilter == 2) {
		/* K5 (L1-C10): moncoexfilter 2 = swap, but filter-pass only (the QWZ_MON_F_FP_ONLY result), whatever monflags says */
		f.pkt_filter_flags0 = HTT_RX_MON_FP_MGMT_FILTER_FLAGS0;
		f.pkt_filter_flags1 = HTT_RX_MON_FP_MGMT_FILTER_FLAGS1;
		f.pkt_filter_flags2 = HTT_RX_MON_FP_CTRL_FILTER_FLASG2;
		f.pkt_filter_flags3 = HTT_RX_MON_FP_CTRL_FILTER_FLASG3 | HTT_RX_MON_FP_DATA_FILTER_FLASG3;
	}
	if (qwz_mon_flags & QWZ_MON_F_OFFSETS) {
		f.offset_valid = 1;
		f.rx_packet_offset = sc->hal.hal_desc_sz;
		f.rx_header_offset = offsetof(struct hal_rx_desc_wcn7850, pkt_hdr_tlv);
		f.rx_mpdu_start_offset = offsetof(struct hal_rx_desc_wcn7850, mpdu_start_tag);
		f.rx_msdu_end_offset = offsetof(struct hal_rx_desc_wcn7850, msdu_end_tag);
	}

	for (i = 0; i < sc->hw_params.num_rxmda_per_pdev; i++) {
		ret = qwz_dp_tx_htt_rx_filter_setup(sc, dp->rx_mac_buf_ring[i].ring_id,
		    i, HAL_RXDMA_BUF, DP_RXDMA_REFILL_RING_SIZE, &f);
		if (ret) {
			printf("qwz: mon: rx_mac_buf_ring%d filter failed: %d\n", i, ret);
			return ret;
		}
	}
	return 0;
}

/* ---- reaping the RXDMA error destination ring ---------------------------- */

#define QWZ_MON_FRAG_LEN(sc)	((DP_RX_BUFFER_SIZE - (sc)->hal.hal_desc_sz) & ~127U)
#define QWZ_MON_MAX_LINKS	8

static uint8_t *mon_frame;	/* assembly buffer, QWZ_MONCAP_MAXFRAME bytes */

/* 802.11 MAC header length from the frame control field. */
static size_t
qwz_mon_hdrlen(const uint8_t *fc)
{
	uint8_t type = fc[0] & 0x0c, subtype = fc[0] & 0xf0;

	if (type == 0x04) {	/* control */
		if (subtype == 0xc0 || subtype == 0xd0)	/* CTS, ACK */
			return 10;
		return 16;
	}
	if (type == 0x08) {	/* data */
		size_t n = (fc[1] & 0x03) == 0x03 ? 30 : 24;
		if (subtype & 0x80)	/* QoS */
			n += 2;
		return n;
	}
	return 24;	/* management, extension */
}

/*
 * One entrance-ring entry: an MPDU as a chain of MSDU link descriptors whose
 * buffers come from the station's refill ring. Returns the number of
 * buffers consumed (they go to used_list for replenishing).
 */
static int
qwz_mon_pop_mpdu(struct qwz_softc *sc, struct hal_reo_entrance_ring *ent,
    void *used_list)
{
	struct qwz_dp *dp = &sc->dp;
	struct dp_link_desc_bank *bank;
	struct hal_rx_msdu_link *link;
	struct hal_wbm_release_ring rel;
	struct qwz_rx_msdu *msdu;
	uint64_t paddr, last_paddr = 0;
	uint32_t cookie, info, msdu_cnt, desc_bank, push, ecode;
	uint16_t flags = 0;
	uint8_t rbm, decap = 0, hdr[32];
	size_t flen = 0, hdrlen = 0;
	int nbuf = 0, first = 1, links = 0, i;
	bus_size_t offset;

	qwz_hal_rx_reo_ent_paddr_get(sc, ent, &paddr, &cookie);
	rbm = FIELD_GET(BUFFER_ADDR_INFO1_RET_BUF_MGR, le32toh(ent->buf_addr_info.info1));
	msdu_cnt = FIELD_GET(RX_MPDU_DESC_INFO0_MSDU_COUNT, le32toh(ent->rx_mpdu_info.info0));
	push = FIELD_GET(HAL_REO_ENTR_RING_INFO1_RXDMA_PUSH_REASON, le32toh(ent->info1));
	ecode = FIELD_GET(HAL_REO_ENTR_RING_INFO1_RXDMA_ERROR_CODE, le32toh(ent->info1));
	if (push == HAL_REO_DEST_RING_PUSH_REASON_ERR_DETECTED) {
		flags |= QWZ_MONCAP_F_RXDMA_ERR;
		if (ecode == HAL_REO_ENTR_RING_RXDMA_ECODE_FLUSH_REQUEST_ERR ||
		    ecode == HAL_REO_ENTR_RING_RXDMA_ECODE_MPDU_LEN_ERR ||
		    ecode == HAL_REO_ENTR_RING_RXDMA_ECODE_OVERFLOW_ERR)
			flags |= QWZ_MONCAP_F_DROPCLASS;
	}
	sc->mon_stats.entries++;
	sc->mon_lat.rxring[4]++;	/* K1 W2b: entries reaped from the cell's RXDMA error ring */
	if (qwz_mon_flags & QWZ_MON_F_VERBOSE)
		printf("qwz: mon: entry paddr 0x%llx cookie 0x%x msdus %u push %u ecode %u\n",
		    (unsigned long long)paddr, cookie, msdu_cnt, push, ecode);

	do {
		if (paddr == last_paddr) {
			sc->mon_stats.duplink++;
			break;
		}
		last_paddr = paddr;
		desc_bank = FIELD_GET(DP_LINK_DESC_BANK_MASK, cookie);
		if (desc_bank >= nitems(dp->link_desc_banks)) {
			sc->mon_stats.nolink++;
			break;
		}
		bank = &dp->link_desc_banks[desc_bank];
		if (bank->mem == NULL || bank->size < sizeof(*link) ||
		    paddr < bank->paddr ||
		    paddr - bank->paddr > bank->size - sizeof(*link)) {
			sc->mon_stats.nolink++;
			break;
		}
		offset = paddr - bank->paddr;
		link = (struct hal_rx_msdu_link *)((uint8_t *)bank->vaddr + offset);
		bus_dmamap_sync(sc->sc_dmat, bank->mem->map, offset, sizeof(*link),
		    BUS_DMASYNC_POSTREAD);

		for (i = 0; i < HAL_NUM_RX_MSDUS_PER_LINK_DESC; i++) {
			struct hal_rx_msdu_details *d = &link->msdu_link[i];
			uint32_t mcookie, mlen, cont;
			size_t l3pad, avail, plen;
			uint8_t *data, *payload;

			if (!FIELD_GET(BUFFER_ADDR_INFO0_ADDR, le32toh(d->buf_addr_info.info0)))
				break;
			mcookie = FIELD_GET(BUFFER_ADDR_INFO1_SW_COOKIE, le32toh(d->buf_addr_info.info1));
			info = le32toh(d->rx_msdu_info.info0);
			mlen = FIELD_GET(RX_MSDU_DESC_INFO0_MSDU_LENGTH, info);
			cont = info & RX_MSDU_DESC_INFO0_MSDU_CONTINUATION;
			if (!cont && msdu_cnt > 0)
				msdu_cnt--;

			msdu = qwz_dp_rx_reap_desc(sc, mcookie, used_list);
			if (msdu == NULL) {
				sc->mon_stats.badcookie++;
				continue;
			}
			nbuf++;
			data = mtod(msdu->m, uint8_t *);
			l3pad = sc->hal_rx_ops->rx_desc_get_l3_pad_bytes(msdu->rx_desc);
			if (first) {
				first = 0;
				if (sc->hal_rx_ops->rx_desc_get_mpdu_start_tag(msdu->rx_desc) !=
				    HAL_RX_MPDU_START)
					flags |= QWZ_MONCAP_F_NO_MPDU;
				decap = sc->hal_rx_ops->rx_desc_get_decap_type(msdu->rx_desc);
				if (mlen == 0)
					mlen = sc->hal_rx_ops->rx_desc_get_msdu_len(msdu->rx_desc);
				if (decap == DP_RX_DECAP_TYPE_NATIVE_WIFI) {
					/* The hardware stripped the 802.11 header; it is in the descriptor. */
					const uint8_t *h = msdu->rx_desc->u.wcn7850.pkt_hdr_tlv.rx_pkt_hdr;
					hdrlen = qwz_mon_hdrlen(h);
					if (hdrlen > sizeof(hdr))
						hdrlen = sizeof(hdr);
					memcpy(hdr, h, hdrlen);
					memcpy(mon_frame, hdr, hdrlen);
					flen = hdrlen;
					flags |= QWZ_MONCAP_F_HDR_SYNTH;
				} else if (decap == DP_RX_DECAP_TYPE_RAW)
					flags |= QWZ_MONCAP_F_FCS;
			}
			if (cont)
				flags |= QWZ_MONCAP_F_CONT;
			payload = data + sc->hal.hal_desc_sz + l3pad;
			avail = DP_RX_BUFFER_SIZE - sc->hal.hal_desc_sz - l3pad;
			plen = cont ? QWZ_MON_FRAG_LEN(sc) : mlen;
			if (plen > avail)
				plen = avail;
			if (flen + plen > QWZ_MONCAP_MAXFRAME) {
				plen = QWZ_MONCAP_MAXFRAME - flen;
				flags |= QWZ_MONCAP_F_TRUNC;
			}
			memcpy(mon_frame + flen, payload, plen);
			flen += plen;
			m_freem(msdu->m);
			msdu->m = NULL;
		}

		/* Return this link descriptor and follow the chain. */
		memset(&rel, 0, sizeof(rel));
		qwz_hal_rx_buf_addr_info_set(&rel.buf_addr_info, paddr, cookie, rbm);
		qwz_hal_rx_buf_addr_info_get(&link->buf_addr_info, &paddr, &cookie, &rbm);
		bus_dmamap_sync(sc->sc_dmat, bank->mem->map, offset, sizeof(*link),
		    BUS_DMASYNC_PREREAD);
		if (qwz_dp_rx_link_desc_return(sc, (uint32_t *)&rel,
		    HAL_WBM_REL_BM_ACT_PUT_IN_IDLE))
			sc->mon_stats.linkret_fail++;
	} while (paddr != 0 && msdu_cnt > 0 && ++links < QWZ_MON_MAX_LINKS);

	sc->mon_stats.mpdus++;
	if (flen > 0) {
		sc->mon_stats.frames++;
		if (flen >= 2 && ((mon_frame[0] >> 2) & 3) == 0)	/* K2c (C4): a management frame that came through the RX ring path (evidence only) */
			sc->mon_stats.ring_mgmt_frames++;
		if (sc->mon_coex) {
			/*
			 * K5 (L1-C7): beside the station only the cell's frames reach the capture ring: A1, A2 and A3 at offsets 4, 10 and 16 must make the claim
			 * (control frames and anything shorter than a header are not the cell's). The buffers are returned either way (this function's loop above
			 * already freed them), so nothing leaks.
			 */
			if (flen >= 24 && ((mon_frame[0] >> 2) & 3) != 1 && qwz_mon_claims_data(sc, mon_frame + 4, mon_frame + 10, mon_frame + 16)) {
				if (((mon_frame[0] >> 2) & 3) == 0)
					sc->mon_stats.coex_cell_mgmt++;
				else
					sc->mon_stats.coex_cell_data++;
			} else {
				sc->mon_stats.coex_drop++;
				return nbuf;
			}
		}
		qwz_mon_rx_hook(sc, mon_frame, flen, flags);
		moncap_put(mon_frame, flen, flags, decap, (uint8_t)sc->mon_chan, 0, 0);
	}
	return nbuf;
}

static int
qwz_mon_reap_ring(struct qwz_softc *sc)
{
	struct qwz_dp *dp = &sc->dp;
	TAILQ_HEAD(, ath12k_rx_desc_info) used_list;
	struct hal_srng *srng;
	struct hal_reo_entrance_ring *ent;
	int nbuf = 0, budget = 64;

	TAILQ_INIT(&used_list);
	srng = &sc->hal.srng_list[dp->rxdma_err_dst_ring[0].ring_id];
	qwz_hal_srng_access_begin(sc, srng);
	while (budget-- > 0 && (ent = (struct hal_reo_entrance_ring *)
	    qwz_hal_srng_dst_get_next_entry(sc, srng)) != NULL)
		nbuf += qwz_mon_pop_mpdu(sc, ent, &used_list);
	qwz_hal_srng_access_end(sc, srng);
	if (nbuf) {
		sc->mon_stats.bufs += nbuf;
		qwz_dp_rxbufs_replenish(sc, &dp->rx_refill_buf_ring, &used_list, nbuf);
	}
	return nbuf;
}

static void
qwz_mon_reap(void *arg)
{
	struct qwz_softc *sc = (struct qwz_softc *)arg;
	uint64_t now;
	int n;

	if (!sc->mon_active)
		return;
	n = qwz_mon_reap_ring(sc);
	now = getnsecuptime();
	{	/* K2c (C2), debug.qwz.monreap: every 100 ms free the capture TX slots whose frame is older than 3 s; this timer runs on the command-gate work loop, like the ring work above */
		static uint64_t reap_last_ns;

		if (qwz_monreap && now - reap_last_ns >= 100000000ULL) {
			reap_last_ns = now;
			qwz_mon_reap_stale(sc, now);
		}
	}
	if (now - sc->mon_stats.last_log > 5000000000ULL) {
		sc->mon_stats.last_log = now;
		printf("qwz: mon: ch %u: entries %u mpdus %u frames %u mgmt %u reo %u "
		    "reo_err %u wbm_err %u bufs %u badcookie %u nolink %u duplink %u "
		    "linkret_fail %u capdrop %u wmi_rx %u ring_mgmt %u\n",
		    sc->mon_chan, sc->mon_stats.entries, sc->mon_stats.mpdus,
		    sc->mon_stats.frames, sc->mon_stats.mgmt, sc->mon_stats.reo,
		    sc->mon_stats.reo_err, sc->mon_stats.wbm_err, sc->mon_stats.bufs,
		    sc->mon_stats.badcookie, sc->mon_stats.nolink, sc->mon_stats.duplink,
		    sc->mon_stats.linkret_fail, moncap_dropped, sc->mon_stats.wmi_mgmt_rx_events, sc->mon_stats.ring_mgmt_frames);
		if (sc->mon_coex)	/* K5 (L1-C15): totals since the cell started of what the claim tests did, plus the credit reserve, the credits left and the filter mode */
			printf("qwz: mon: coex: counters st_mgmt %u cell_mgmt %u st_data %u cell_data %u drop %u; resv %d credits_left %d filter %d\n",
			    sc->mon_stats.coex_st_mgmt, sc->mon_stats.coex_cell_mgmt, sc->mon_stats.coex_st_data, sc->mon_stats.coex_cell_data,
			    sc->mon_stats.coex_drop, qwz_moncoexcred, (unsigned int)sc->wmi.wmi[sc->pdevs[0].pdev_id].eid < (unsigned int)ATH12K_HTC_EP_COUNT ?
			    (int)sc->htc.endpoint[sc->wmi.wmi[sc->pdevs[0].pdev_id].eid].tx_credits : -1, qwz_moncoexfilter);
		if (sc->mon_stats.ufr_sent || sc->mon_stats.ufr_full || sc->mon_stats.ufr_fail)
			printf("qwz: mon: utx: sent %u full %u fail %u; mgmt ok %u bad %u, data ok %u bad %u, ctrl ok %u bad %u; nocred %u qmax %u inflight %u; pending %d reaped %u livefull %u compl range %u nom %u sta %u; held_ms %u gate %u fmt %u fmtfail %u\n",
			    sc->mon_stats.ufr_sent, sc->mon_stats.ufr_full, sc->mon_stats.ufr_fail,
			    sc->mon_stats.ufr_mgmt_ok, sc->mon_stats.ufr_mgmt_bad,
			    sc->mon_stats.ufr_data_ok, sc->mon_stats.ufr_data_bad,
			    sc->mon_stats.ufr_ctrl_ok, sc->mon_stats.ufr_ctrl_bad,
			    sc->mon_stats.ufr_nocred, sc->mon_stats.ufr_qmax, sc->mon_stats.ufr_inflight,
			    (int)(sc->mon_stats.ufr_sent - (sc->mon_stats.ufr_mgmt_ok + sc->mon_stats.ufr_mgmt_bad + sc->mon_stats.ufr_data_ok +
			    sc->mon_stats.ufr_data_bad + sc->mon_stats.ufr_ctrl_ok + sc->mon_stats.ufr_ctrl_bad)),	/* K2c (C3): user frames sent minus all completions seen */
			    sc->mon_stats.ufr_reaped, sc->mon_stats.ufr_livefull, sc->mon_stats.ufr_compl_range,
			    sc->mon_stats.ufr_compl_nom, sc->mon_stats.ufr_compl_sta, qwz_mon_oldest_ms(sc, now), sc->mon_stats.ufr_gate,
			    sc->mon_stats.ufr_fmt, sc->mon_stats.ufr_fmtfail);
		if (sc->mon_stats.rx_psf || sc->mon_stats.tx_sent)
			printf("qwz: mon: tx: psf heard %u, sent %u fail %u, compl ok %u discard %u inspect %u noack %u other %u, last ack rssi %d, mode 0x%x\n",
			    sc->mon_stats.rx_psf, sc->mon_stats.tx_sent, sc->mon_stats.tx_fail,
			    sc->mon_stats.tx_ok, sc->mon_stats.tx_discard, sc->mon_stats.tx_inspect,
			    sc->mon_stats.tx_noack, sc->mon_stats.tx_other,
			    sc->mon_stats.last_ack_rssi, qwz_mon_tx_mode);
		{	/* K1 W2b: the one new line per 5 s; the latency windows restart here */
			uint32_t cp50, cp90, cmax, rp50, rp90, rmax;

			qwz_mon_lat_pct(sc->mon_lat.cl, sc->mon_lat.ncl, &cp50, &cp90, &cmax);
			qwz_mon_lat_pct(sc->mon_lat.cr, sc->mon_lat.ncr, &rp50, &rp90, &rmax);
			printf("qwz: mon: lat: rx %llu B/%u fr (total %llu B) tx %llu B/%u fr (total %llu B); reap reo %u/%u/%u/%u err0 %u; "
			    "compl us n %u lost %u p50 %u p90 %u max %u; credit us n %u lost %u p50 %u p90 %u max %u out %u\n",
			    (unsigned long long)(sc->mon_lat.rx_bytes - sc->mon_lat.rx_bytes_l), sc->mon_lat.rx_frames - sc->mon_lat.rx_frames_l,
			    (unsigned long long)sc->mon_lat.rx_bytes,
			    (unsigned long long)(sc->mon_lat.tx_bytes - sc->mon_lat.tx_bytes_l), sc->mon_lat.tx_frames - sc->mon_lat.tx_frames_l,
			    (unsigned long long)sc->mon_lat.tx_bytes,
			    sc->mon_lat.rxring[0] - sc->mon_lat.rxring_l[0], sc->mon_lat.rxring[1] - sc->mon_lat.rxring_l[1],
			    sc->mon_lat.rxring[2] - sc->mon_lat.rxring_l[2], sc->mon_lat.rxring[3] - sc->mon_lat.rxring_l[3],
			    sc->mon_lat.rxring[4] - sc->mon_lat.rxring_l[4],
			    sc->mon_lat.ncl, sc->mon_lat.cl_lost, cp50, cp90, cmax,
			    sc->mon_lat.ncr, sc->mon_lat.cr_lost, rp50, rp90, rmax, sc->mon_lat.cr_out);
			sc->mon_lat.rx_bytes_l = sc->mon_lat.rx_bytes;
			sc->mon_lat.tx_bytes_l = sc->mon_lat.tx_bytes;
			sc->mon_lat.rx_frames_l = sc->mon_lat.rx_frames;
			sc->mon_lat.tx_frames_l = sc->mon_lat.tx_frames;
			memcpy(sc->mon_lat.rxring_l, sc->mon_lat.rxring, sizeof(sc->mon_lat.rxring_l));
			sc->mon_lat.ncl = sc->mon_lat.cl_lost = 0;
			sc->mon_lat.ncr = sc->mon_lat.cr_lost = 0;
		}
	}
	timeout_add_msec(&sc->mon_cell_timer, n ? 2 : 10);
}

/* ---- the monitor vdev ------------------------------------------------------ */

static int
qwz_mon_vdev_create(struct qwz_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct qwz_pdev *pdev = &sc->pdevs[0];
	struct qwz_vif *vif = &sc->sc_monvif;
	struct vdev_create_params params;
	int ret, bit, nss;

	if (sc->free_vdev_map == 0)
		return EBUSY;
	bit = ffs(sc->free_vdev_map) - 1;

	memset(vif, 0, offsetof(struct qwz_vif, txmgmt));	/* the DMA maps persist */
	vif->vdev_id = bit;
	vif->vdev_type = (qwz_mon_flags & QWZ_MON_F_AP) ? WMI_VDEV_TYPE_AP : WMI_VDEV_TYPE_MONITOR;
	vif->vdev_subtype = WMI_VDEV_SUBTYPE_NONE;
	vif->bank_id = -1;
	/* Derived from the card's own address, not the interface's: macOS
	 * gives the station a private (random) address when Wi-Fi comes on,
	 * and an AWDL address that changes between runs leaves phones with
	 * stale records. Locally administered (bit 1 of the first byte) so it
	 * differs from the station vdev's: two vdevs with one address left the
	 * station unable to associate after a cell session. iPhones use a
	 * local address for AWDL too. */
	IEEE80211_ADDR_COPY(vif->vdev_addr, sc->mac_addr);
	vif->vdev_addr[0] |= 0x02;
	if ((qwz_mon_flags & QWZ_MON_F_BSSID_MAC) && sc->mon_bssid_set)
		IEEE80211_ADDR_COPY(vif->vdev_addr, sc->mon_bssid);
	/*
	 * K5 (L1-C3): beside a running station the cell's address must differ from the station vdev's and from the interface address (two vdevs with one
	 * address left the station unable to associate, commit 884a7e8). Flip the low bits of the last octet, the local bit in the first octet stays.
	 * The daemon reads the address from debug.qwz.monmac after the start, so a changed address is harmless.
	 */
	if (QWZ_MON_COEX_ON(sc)) {
		uint8_t last = vif->vdev_addr[5];
		int k;

		for (k = 1; k < 4 && (IEEE80211_ADDR_EQ(vif->vdev_addr, sc->sc_vif.vdev_addr) || IEEE80211_ADDR_EQ(vif->vdev_addr, ic->ic_myaddr)); k++) {
			vif->vdev_addr[5] = last ^ (uint8_t)k;
			if (k == 1)
				printf("qwz: mon: coex: cell MAC adjusted\n");
		}
	}

	memset(&params, 0, sizeof(params));
	ret = qwz_mac_setup_vdev_create_params(sc, vif, pdev, &params);
	if (ret)
		return ret;
	ret = qwz_wmi_vdev_create(sc, vif->vdev_addr, &params);
	if (ret) {
		printf("qwz: mon: vdev %d create failed: %d\n", bit, ret);
		return ret;
	}
	sc->num_created_vdevs++;
	sc->allocated_vdev_map |= 1U << bit;
	sc->free_vdev_map &= ~(1U << bit);
	printf("qwz: mon: vdev %d created, type %s, mac %s\n", bit,
	    vif->vdev_type == WMI_VDEV_TYPE_AP ? "AP" : "MONITOR", ether_sprintf(vif->vdev_addr));

	ret = qwz_mac_op_update_vif_offload(sc, pdev, vif);
	if (ret)
		return ret;
	nss = qwz_get_num_chains(sc->cfg_tx_chainmask) ? : 1;
	if (QWZ_MON_COEX_ON(sc) && qwz_moncoexnss == 1)	/* K5 (L1-C3): optional, no Linux precedent */
		nss = 1;
	ret = qwz_wmi_vdev_set_param_cmd(sc, vif->vdev_id, pdev->pdev_id,
	    WMI_VDEV_PARAM_NSS, nss);
	if (ret)
		printf("qwz: mon: vdev %d nss failed: %d\n", bit, ret);

	/*
	 * Linux creates an AP vdev's own peer right after the vdev (its
	 * management frames go out through that entry); without it the
	 * firmware completes our transmits with DISCARD. Goes through the
	 * port's peer list so the map/unmap events find it.
	 */
	sc->mon_self_peer = 0;
	if (vif->vdev_type == WMI_VDEV_TYPE_AP && !(qwz_mon_flags & QWZ_MON_F_NO_SELF_PEER)) {
		struct peer_create_params pp;
		struct qwz_node dummy;

		memset(&pp, 0, sizeof(pp));
		memset(&dummy, 0, sizeof(dummy));
		pp.peer_addr = vif->vdev_addr;
		pp.peer_type = WMI_PEER_TYPE_DEFAULT;
		pp.vdev_id = vif->vdev_id;
		ret = qwz_peer_create(sc, vif, pdev->pdev_id, &dummy.ni, &pp);
		printf("qwz: mon: self peer %s on vdev %d: %d\n",
		    ether_sprintf(vif->vdev_addr), bit, ret);
		sc->mon_self_peer = (ret == 0);
	}
	return 0;
}

/*
 * The smallest beacon an AP vdev can carry: hidden SSID, basic rates, DS
 * parameter, an empty TIM (the firmware fills the timestamp and TIM). Sent
 * with WMI_BCN_TMPL as Linux ath12k_wmi_bcn_tmpl() does.
 */
static int
qwz_mon_bcn_tmpl(struct qwz_softc *sc, int chan)
{
	struct qwz_vif *vif = &sc->sc_monvif;
	struct qwz_pdev_wmi *wmi = &sc->wmi.wmi[0];
	struct wmi_bcn_tmpl_cmd *cmd;
	struct wmi_bcn_prb_info *info;
	struct wmi_tlv *tlv;
	struct mbuf *m;
	uint8_t bcn[96], *p = bcn;
	uint32_t blen, aligned, tim_off;
	int len, ret;

	memset(bcn, 0, sizeof(bcn));
	p[0] = 0x80;					/* beacon */
	memset(p + 4, 0xff, 6);				/* addr1 */
	memcpy(p + 10, vif->vdev_addr, 6);		/* addr2 */
	memcpy(p + 16, sc->mon_bssid_set ? sc->mon_bssid : vif->vdev_addr, 6);	/* addr3 */
	p += 24;
	p += 8;						/* timestamp: firmware */
	p[0] = 100; p[1] = 0; p += 2;			/* beacon interval, TU */
	p[0] = 0x01; p[1] = 0x00; p += 2;		/* capability: ESS */
	*p++ = 0; *p++ = 0;				/* SSID, hidden */
	*p++ = 1; *p++ = 8;				/* supported rates */
	memcpy(p, "\x82\x84\x8b\x96\x0c\x12\x18\x24", 8); p += 8;
	*p++ = 3; *p++ = 1; *p++ = (uint8_t)chan;	/* DS parameter set */
	tim_off = (uint32_t)(p - bcn);
	*p++ = 5; *p++ = 4; *p++ = 0; *p++ = 1; *p++ = 0; *p++ = 0;	/* TIM */
	blen = (uint32_t)(p - bcn);
	aligned = roundup(blen, 4);

	len = sizeof(*cmd) + sizeof(*info) + TLV_HDR_SIZE + aligned;
	m = qwz_wmi_alloc_mbuf(len);
	if (m == NULL)
		return ENOMEM;
	cmd = (struct wmi_bcn_tmpl_cmd *)(mtod(m, uint8_t *) +
	    sizeof(struct ath12k_htc_hdr) + sizeof(struct wmi_cmd_hdr));
	cmd->tlv_header = FIELD_PREP(WMI_TLV_TAG, WMI_TAG_BCN_TMPL_CMD) |
	    FIELD_PREP(WMI_TLV_LEN, sizeof(*cmd) - TLV_HDR_SIZE);
	cmd->vdev_id = vif->vdev_id;
	cmd->tim_ie_offset = tim_off;
	cmd->buf_len = blen;
	info = (struct wmi_bcn_prb_info *)((uint8_t *)cmd + sizeof(*cmd));
	info->tlv_header = FIELD_PREP(WMI_TLV_TAG, WMI_TAG_BCN_PRB_INFO) |
	    FIELD_PREP(WMI_TLV_LEN, sizeof(*info) - TLV_HDR_SIZE);
	tlv = (struct wmi_tlv *)((uint8_t *)info + sizeof(*info));
	tlv->header = FIELD_PREP(WMI_TLV_TAG, WMI_TAG_ARRAY_BYTE) |
	    FIELD_PREP(WMI_TLV_LEN, aligned);
	memcpy(tlv->value, bcn, blen);
	ret = qwz_wmi_cmd_send(wmi, m, WMI_BCN_TMPL_CMDID);
	if (ret)
		m_freem(m);
	printf("qwz: mon: beacon template (%u bytes) for vdev %u: %d\n", blen, vif->vdev_id, ret);
	return ret;
}

/* A firmware peer for the peer we talk to, bypassing the port's single-peer list. */
static void
qwz_mon_sender_peer(struct qwz_softc *sc, const uint8_t *addr, int add)
{
	struct peer_create_params pp;
	int ret;

	if (add) {
		if (sc->mon_sender_peer)
			return;
		memset(&pp, 0, sizeof(pp));
		pp.peer_addr = (uint8_t *)addr;
		pp.peer_type = WMI_PEER_TYPE_DEFAULT;
		pp.vdev_id = sc->sc_monvif.vdev_id;
		ret = qwz_wmi_send_peer_create_cmd(sc, sc->pdevs[0].pdev_id, &pp);
		if (ret == 0) {
			sc->mon_sender_peer = 1;
			memcpy(sc->mon_sender_addr, addr, 6);
		}
		printf("qwz: mon: raw peer %s on vdev %u: %d\n", ether_sprintf((u_char *)addr),
		    sc->sc_monvif.vdev_id, ret);
	} else if (sc->mon_sender_peer) {
		ret = qwz_wmi_send_peer_delete_cmd(sc, sc->mon_sender_addr,
		    sc->sc_monvif.vdev_id, sc->pdevs[0].pdev_id);
		printf("qwz: mon: raw peer %s deleted: %d\n", ether_sprintf(sc->mon_sender_addr), ret);
		sc->mon_sender_peer = 0;
	}
}

/*
 * K1 (W3), debug.qwz.monparams: right after the cell vdev is up. 1: MGMT_RATE OFDM 12 Mbit/s (the rate code of qwz_awdl_vdev_params, lane M's listen vdev);
 * 2: also WMI_PDEV_PARAM_MGMT_RETRY_LIMIT 3 and WMI_VDEV_PARAM_NON_AGG_SW_RETRY_TH 4. Each set logs one "qwz: mon: params:" line; a failure logs and
 * continues, the cell start is never aborted. The firmware restarts after every cell (mon_fw_dirty), so nothing leaks into the station.
 */
static void
qwz_mon_cell_params(struct qwz_softc *sc)
{
	struct qwz_vif *vif = &sc->sc_monvif;
	uint8_t pdev_id = sc->pdevs[0].pdev_id;
	uint32_t rate = ATH12K_HW_RATE_CODE(ATH12K_HW_RATE_OFDM_12M, 0, WMI_RATE_PREAMBLE_OFDM);
	int mode = qwz_monparams, ret;

	if (mode <= 0)
		return;
	if (qwz_mon_fw_unavail(sc)) {
		printf("qwz: mon: params: skipped, firmware unavailable (monparams %d)\n", mode);
		return;
	}
	ret = qwz_wmi_vdev_set_param_cmd(sc, vif->vdev_id, pdev_id, WMI_VDEV_PARAM_MGMT_RATE, rate);
	printf("qwz: mon: params: vdev %u MGMT_RATE 0x%08x (OFDM 12 Mbit/s): %d\n", vif->vdev_id, (unsigned)rate, ret);
	if (mode < 2)
		return;
	if (qwz_mon_fw_unavail(sc)) {
		printf("qwz: mon: params: skipped the rest, firmware unavailable\n");
		return;
	}
	ret = qwz_wmi_pdev_set_param(sc, WMI_PDEV_PARAM_MGMT_RETRY_LIMIT, 3, pdev_id);
	printf("qwz: mon: params: pdev %u MGMT_RETRY_LIMIT 3: %d\n", pdev_id, ret);
	if (qwz_mon_fw_unavail(sc)) {
		printf("qwz: mon: params: skipped NON_AGG_SW_RETRY_TH, firmware unavailable\n");
		return;
	}
	ret = qwz_wmi_vdev_set_param_cmd(sc, vif->vdev_id, pdev_id, WMI_VDEV_PARAM_NON_AGG_SW_RETRY_TH, 4);
	printf("qwz: mon: params: vdev %u NON_AGG_SW_RETRY_TH 4: %d\n", vif->vdev_id, ret);
}

static int
qwz_mon_vdev_start(struct qwz_softc *sc, int chan, uint32_t freq)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct qwz_pdev *pdev = &sc->pdevs[0];
	struct qwz_vif *vif = &sc->sc_monvif;
	struct wmi_vdev_start_req_arg arg;
	int ret;

	memset(&arg, 0, sizeof(arg));
	arg.vdev_id = vif->vdev_id;
	arg.channel.freq = freq;
	arg.channel.band_center_freq1 = freq;
	arg.channel.band_center_freq2 = freq;
	arg.channel.allow_ht = true;
	arg.channel.mode = chan <= 14 ? MODE_11NG_HT20 : MODE_11NA_HT20;
	arg.channel.passive = chan >= 52 && chan <= 144;	/* DFS: never transmit */
	arg.channel.min_power = 0;
	arg.channel.max_power = 20;
	arg.channel.max_reg_power = 20;
	arg.channel.max_antenna_gain = 0;
	arg.pref_tx_streams = sc->num_tx_chains;
	arg.pref_rx_streams = sc->num_rx_chains;
	if (vif->vdev_type == WMI_VDEV_TYPE_AP) {
		static uint8_t ssid[] = "qwzmon";
		arg.ssid = ssid;
		arg.ssid_len = 6;
		arg.hidden_ssid = true;
		arg.bcn_intval = 100;
		arg.dtim_period = 1;
	}
	/*
	 * K5 (L1-C4): beside the running station the firmware should see ONE chandef for both vdevs, as under Linux (one channel context). Mode 0 asks the
	 * station's own routine for freq, centre frequency and phy mode (VHT80 / HE80 on 149), mode 1 keeps the HT20-on-the-primary block above. passive:
	 * moncoexpassive 1 copies the station vdev's rule (qwz_mac_vdev_start_restart: passive from channel 52 up), 0 keeps the K4 cell's. The beacon
	 * interval is the station's BSS (Linux wants the two to match). No beacon template (QWZ_MON_F_BEACON stays the fallback if vdev-up is refused).
	 */
	if (QWZ_MON_COEX_ON(sc) && ic->ic_bss != NULL && ic->ic_bss->ni_chan != NULL) {
		struct ieee80211_node *bn = ic->ic_bss;

		if (qwz_moncoexmode == 0) {
			qwz_mac_vdev_chan_args(sc, bn, &arg);
			arg.channel.min_power = 0;
			arg.channel.max_power = 20;
			arg.channel.max_reg_power = 20;
			arg.channel.max_antenna_gain = 0;
			if (vif->vdev_type == WMI_VDEV_TYPE_AP) {
				arg.bcn_intval = bn->ni_intval ? bn->ni_intval : 100;
				arg.dtim_period = 1;
			}
		}
		if (qwz_moncoexpassive != 0)
			arg.channel.passive = ieee80211_chan2ieee(ic, bn->ni_chan) >= 52;
		printf("qwz: mon: coex: cell vdev %u start freq %u cfreq1 %u mode %u passive %u bcn_intval %u\n", vif->vdev_id,
		    (unsigned int)arg.channel.freq, (unsigned int)arg.channel.band_center_freq1, (unsigned int)arg.channel.mode,
		    (unsigned int)arg.channel.passive, (unsigned int)arg.bcn_intval);
	}

	sc->vdev_setup_done = 0;
	ret = qwz_wmi_vdev_start(sc, &arg, pdev->pdev_id, 0);
	if (ret) {
		printf("qwz: mon: vdev %d start failed: %d\n", vif->vdev_id, ret);
		return ret;
	}
	ret = qwz_mac_vdev_setup_sync(sc);
	if (ret) {
		printf("qwz: mon: vdev %d start: no response: %d\n", vif->vdev_id, ret);
		return ret;
	}
	sc->num_started_vdevs++;
	vif->is_started = 1;
	vif->chan_mode = arg.channel.mode;
	vif->chan_cfreq1 = freq;
	if (QWZ_MON_COEX_ON(sc))	/* K5 (L1-C4): the centre frequency actually sent (the primary only in mode 1) */
		vif->chan_cfreq1 = arg.channel.band_center_freq1;

	if (vif->vdev_type == WMI_VDEV_TYPE_AP && (qwz_mon_flags & QWZ_MON_F_BEACON))
		qwz_mon_bcn_tmpl(sc, chan);

	{
		const uint8_t *bssid = sc->mon_bssid_set ? sc->mon_bssid : vif->vdev_addr;

		ret = qwz_wmi_vdev_up(sc, vif->vdev_id, pdev->pdev_id, 0, bssid, NULL, 0, 0);
		if (ret) {
			printf("qwz: mon: vdev %d up failed: %d\n", vif->vdev_id, ret);
			return ret;
		}
		vif->is_up = 1;
		printf("qwz: mon: vdev %d up on channel %d (%u MHz), bssid %s\n", vif->vdev_id,
		    chan, freq, ether_sprintf((u_char *)bssid));
	}
	qwz_mon_cell_params(sc);	/* K1 (W3): debug.qwz.monparams, 0 = nothing */
	return 0;
}

/* Returns 1 when the firmware stopped answering (WMI timeouts). */
static int
qwz_mon_vdev_teardown(struct qwz_softc *sc)
{
	struct qwz_pdev *pdev = &sc->pdevs[0];
	struct qwz_vif *vif = &sc->sc_monvif;
	int ret, wedged = 0;

	qwz_mon_txq_purge(sc);
	qwz_mon_sender_peer(sc, NULL, 0);

	if (vif->is_up) {
		ret = qwz_wmi_vdev_down(sc, vif->vdev_id, pdev->pdev_id);
		if (ret)
			printf("qwz: mon: vdev %d down failed: %d\n", vif->vdev_id, ret);
		if (QWZ_MON_FW_SILENT(ret))
			wedged = 1;
		vif->is_up = 0;
	}
	if (vif->is_started) {
		sc->vdev_setup_done = 0;
		ret = qwz_wmi_vdev_stop(sc, vif->vdev_id, pdev->pdev_id);
		if (ret == 0)
			ret = qwz_mac_vdev_setup_sync(sc);
		if (ret)
			printf("qwz: mon: vdev %d stop failed: %d\n", vif->vdev_id, ret);
		if (QWZ_MON_FW_SILENT(ret))
			wedged = 1;
		vif->is_started = 0;
		if (sc->num_started_vdevs > 0)
			sc->num_started_vdevs--;
	}
	if (sc->mon_self_peer) {
		struct ath12k_peer *peer = qwz_peer_find_by_addr(sc, vif->vdev_addr);

		if (peer != NULL) {
			ret = qwz_peer_delete(sc, vif->vdev_id, pdev->pdev_id, peer);
			printf("qwz: mon: self peer deleted: %d\n", ret);
		}
		sc->mon_self_peer = 0;
	}
	if (sc->allocated_vdev_map & (1U << vif->vdev_id)) {
		ret = qwz_mac_vdev_delete_wait(sc, vif);
		if (ret)
			printf("qwz: mon: vdev %d delete failed: %d\n", vif->vdev_id, ret);
		else
			printf("qwz: mon: vdev %d deleted\n", vif->vdev_id);
		if (QWZ_MON_FW_SILENT(ret))
			wedged = 1;
	}
	return wedged;
}

/*
 * merge glue (D3): the capture cell's firmware stopped answering. With the engine armed (recover and m0 both on) the engine owns the restart:
 * declare DEAD (the waiters wake, the engine tears down and brings the firmware back) and end the cell host-only. In legacy mode (recover=0 or
 * m0=0) upstream's inline firmware restart stays.
 */
static void
qwz_mon_fw_wedged(struct qwz_softc *sc, const char *why)
{
	if (qwz_dead_armed()) {
		/*
		 * fix F6: one cell wedge is not the two-timeouts-in-30-s verdict of qwz_note_wait_timeout, so debug.qwz.silent decides as it does there:
		 * DEAD only when the switch is at its declaring level (2); otherwise the cell ends host-only and the next qwz_init restarts the firmware
		 * (mon_fw_dirty, which qwz_init_impl tests apart from mon_active, so it is reachable with no cell). silent=0 stays quiet.
		 */
		if (qwz_silent >= 2) {
			qwz_declare_dead(sc, QWZ_DEAD_SILENT, why);
			qwz_mon_epoch_reset(sc);
		} else {
			if (qwz_silent != 0)
				printf("qwz: mon: would declare DEAD (%s): cell ended host-only\n", why);
			qwz_mon_epoch_reset(sc);
			sc->mon_fw_dirty = 1;	/* after the reset, which clears it */
		}
	} else if (sc->fw_initialized)
		qwz_core_deinit(sc);
}

/*
 * Under the gate; the caller (qwz_mon_gated) holds ioctl_rwl.
 *
 * merge glue (G): on this driver Wi-Fi off is a real chip power-down (setPOWER(0) -> chipDown -> sleepHardware -> qwz_core_deinit, fw_initialized 0),
 * so upstream's precondition "turn Wi-Fi off, then debug.qwz.mon=<ch>" can never be met. The precondition here is "the station is quiescent": a
 * running station is stopped in place (qwz_stop, the call ItlQwz::disableGated makes: ordered deauth, the firmware stays up, IFF_RUNNING cleared)
 * when debug.qwz.monhold=1 says the caller wants the hold state, and refused otherwise.
 */
static int
qwz_mon_start(struct qwz_softc *sc, int chan)
{
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	uint32_t freq = qwz_mon_chan_freq(chan);
	int ret;
	int stopped_in_place = 0;	/* fix F2: this call stopped a running station */
	int coex = 0;			/* K5 (L1-C2): this start runs beside the associated station (debug.qwz.moncoex) */

	if (freq == 0)
		return EINVAL;
	if (!sc->attached)
		return ENXIO;
	if (!sc->fw_initialized) {
		printf("qwz: mon: refused: the firmware is down (turn Wi-Fi on first)\n");
		return ENXIO;
	}
	if (QWZ_CRASHED(sc) || __atomic_load_n(&qwz_pm_pending, __ATOMIC_ACQUIRE) != 0) {	/* merge glue (D6) */
		printf("qwz: mon: refused: the firmware is crashed or DEAD, or a sleep/stop is pending\n");
		return EBUSY;
	}
	if (sc->mon_active)
		return EALREADY;
	/*
	 * merge glue (C): the completion demux tells a capture completion from a BSS one only by slot occupancy in the shared desc_id space 0..63, so
	 * none of our own management lanes may be in flight while a cell runs.
	 */
	if (qwz_awdl_vdev_busy(sc) != 0) {
		printf("qwz: mon: refused: lane M's AWDL listen vdev or test lane is up (debug.qwz.awdltest=7 first)\n");
		return EBUSY;
	}
	if (sc->sc_awdltest_txmask != 0 || sc->sc_awdl_pmask != 0) {
		printf("qwz: mon: refused: AWDL management frames are still in flight (txmask 0x%x pmask 0x%x)\n",
		    sc->sc_awdltest_txmask, sc->sc_awdl_pmask);
		return EBUSY;
	}
	/* fix F2: the refusals that need no firmware conversation come before the station is stopped in place, so a start that fails anyway leaves it running */
	if (mon_frame == NULL || moncap_ring == NULL || !qwz_mon_have_maps(sc))
		return ENOMEM;
	/*
	 * K5 (L1-C2): debug.qwz.moncoex 1 and a station that is up: the cell starts BESIDE the associated station, on its own channel (6, 44 or 149), and
	 * the station is never stopped. Every refusal here is EBUSY and changes nothing. No station running at all (IFF_RUNNING clear) falls through to the
	 * cell-alone path below, unchanged. moncoex 0: this block is skipped and the code below is K4's.
	 */
	if (qwz_moncoex != 0 && (ifp->if_flags & IFF_RUNNING)) {
		struct ieee80211com *cic = &sc->sc_ic;
		int stachan, ci;

		if (cic->ic_state != IEEE80211_S_RUN || !sc->sc_vif.is_up || !sc->sc_vif.is_started || cic->ic_bss == NULL || cic->ic_bss->ni_chan == NULL) {
			printf("qwz: mon: coex: refused: the station is not associated (state %d, up %d, started %d)\n",
			    (int)cic->ic_state, sc->sc_vif.is_up, sc->sc_vif.is_started);
			return EBUSY;
		}
		stachan = ieee80211_chan2ieee(cic, cic->ic_bss->ni_chan);
		if (stachan != chan || !(chan == 6 || chan == 44 || chan == 149) || (chan >= 52 && chan <= 144)) {
			printf("qwz: mon: coex: refused: the station is on channel %d, the cell asked for %d (the cell runs only on the station's channel, 6, 44 or 149, never 52 to 144)\n",
			    stachan, chan);
			return EBUSY;
		}
		if (qwz_mon_flags & QWZ_MON_F_DELETE_STA) {
			printf("qwz: mon: coex: refused: monflags has DELETE_STA, which deletes the station vdev (clear 0x20 first)\n");
			return EBUSY;
		}
		if (sc->mon_coex_setup || sc->ns_nstate != cic->ic_state) {
			printf("qwz: mon: coex: refused: a state change is in flight (ns %d, state %d, setup %d)\n",
			    (int)sc->ns_nstate, (int)cic->ic_state, sc->mon_coex_setup);
			return EBUSY;
		}
		sc->mon_coex_setup = 1;		/* qwz_newstate_task waits on this for at most 2 s */
		coex = 1;
		sc->mon_coex_base_vdevs = sc->num_started_vdevs;
		sc->mon_coex_base_peers = sc->num_peers;
		sc->mon_coex_swapped = 0;
		sc->mon_coex_probe = 0;		/* K5 (L1-C14): a new cell ends any watch of the previous one */
		IEEE80211_ADDR_COPY(sc->mon_coex_sta_addr, cic->ic_myaddr);
		/* the station's management ring must be empty: up to 100 ms in 10 ms steps, asleep with the gate released (a bounded delay where sleeping is refused) */
		for (ci = 0; ci < 10 && sc->sc_vif.txmgmt.queued != 0; ci++) {
			if (tsleep_delay_nsec(&sc->mon_active, 0, "qwzcoexq", MSEC_TO_NSEC(10)) == EDEADLK)
				delay(10000);
		}
		if (!sc->fw_initialized || QWZ_CRASHED(sc) || __atomic_load_n(&qwz_pm_pending, __ATOMIC_ACQUIRE) != 0 ||
		    !(ifp->if_flags & IFF_RUNNING) || cic->ic_state != IEEE80211_S_RUN || !sc->sc_vif.is_up) {
			printf("qwz: mon: coex: refused: the station or the firmware changed state while the cell waited\n");
			QWZ_MON_COEX_SETUP_END(sc);
			return EBUSY;
		}
		if (sc->sc_vif.txmgmt.queued != 0) {
			printf("qwz: mon: coex: refused: station management frames are still queued (%d)\n", sc->sc_vif.txmgmt.queued);
			QWZ_MON_COEX_SETUP_END(sc);
			return EBUSY;
		}
		if (sc->num_started_vdevs > sc->mon_coex_base_vdevs || sc->num_peers > sc->mon_coex_base_peers) {
			printf("qwz: mon: coex: refused: a vdev or peer appeared during the admission (vdevs %d > %d or peers %d > %d)\n",
			    sc->num_started_vdevs, sc->mon_coex_base_vdevs, sc->num_peers, sc->mon_coex_base_peers);
			QWZ_MON_COEX_SETUP_END(sc);
			return EBUSY;
		}
		goto k5_coex_guards_done;	/* skips the stop-in-place block and the two cell-alone guards below */
	}
	if (ifp->if_flags & IFF_RUNNING) {
		int i;

		if (!qwz_mon_hold) {
			printf("qwz: mon: refused: the station is running (set debug.qwz.monhold=1 first; Wi-Fi off powers the chip down on this driver)\n");
			return EBUSY;
		}
		printf("qwz: mon: stopping the station in place for the cell\n");
		qwz_stop(ifp);
		stopped_in_place = 1;
		/* up to 500 ms in 10 ms steps, asleep with the gate released (a bounded delay where sleeping is refused) */
		for (i = 0; i < 50 && (sc->sc_vif.txmgmt.queued != 0 || sc->num_started_vdevs > 0 || !TAILQ_EMPTY(&sc->peers)); i++) {
			if (tsleep_delay_nsec(&sc->mon_active, 0, "qwzmonq", MSEC_TO_NSEC(10)) == EDEADLK)
				delay(10000);
			if (!sc->fw_initialized || QWZ_CRASHED(sc) || __atomic_load_n(&qwz_pm_pending, __ATOMIC_ACQUIRE) != 0)
				break;
		}
		if (!sc->fw_initialized || QWZ_CRASHED(sc)) {
			printf("qwz: mon: refused: the firmware went away while the station stopped\n");
			return EBUSY;
		}
		if (__atomic_load_n(&qwz_pm_pending, __ATOMIC_ACQUIRE) != 0) {	/* fix F3: a sleep or stop began during the wait */
			printf("qwz: mon: refused: a sleep or stop began while the station stopped\n");
			return EBUSY;
		}
		if (sc->sc_vif.txmgmt.queued != 0 || sc->num_started_vdevs > 0 || !TAILQ_EMPTY(&sc->peers)) {
			printf("qwz: mon: refused: the station did not quiesce in 500 ms (queued %d, started vdevs %d)\n",
			    sc->sc_vif.txmgmt.queued, sc->num_started_vdevs);
			return EBUSY;	/* nothing to restore: the next Wi-Fi on restarts the station */
		}
	}
	if (sc->sc_vif.txmgmt.queued != 0) {
		printf("qwz: mon: refused: station management frames are still queued (%d)\n", sc->sc_vif.txmgmt.queued);
		return EBUSY;
	}
	if (sc->num_started_vdevs > 0 || !TAILQ_EMPTY(&sc->peers)) {
		printf("qwz: mon: refused: a vdev is still started or peers remain\n");
		return EBUSY;
	}
k5_coex_guards_done:	/* K5 (L1-C2) */
	memset(&sc->mon_stats, 0, sizeof(sc->mon_stats));
	sc->mon_stats.ufr_inflight = 0;	/* K2b (W1b): no user frame is in flight in a new cell */
	memset(sc->mon_tx_kind, 0, sizeof(sc->mon_tx_kind));	/* K2b (W1b): no stale per-slot kind from the previous cell may count against the cap */
	memset(&sc->mon_lat, 0, sizeof(sc->mon_lat));	/* K1 W2b */
	if (coex)	/* K5 (L1-C8): the cell's management ids start above the station's ring (0 to 7) */
		sc->sc_monvif.txmgmt.cur = QWZ_MON_COEX_STA_SLOTS;
	sc->mon_chan = chan;
	sc->mon_freq = freq;
	sc->mon_bssid_set = qwz_mon_parse_mac(qwz_mon_bssid_str, sc->mon_bssid);
	if (qwz_mon_bssid_str[0] != 0 && !sc->mon_bssid_set)
		printf("qwz: mon: monbssid '%s' is not xx:xx:xx:xx:xx:xx; ignored\n", qwz_mon_bssid_str);
	printf("qwz: mon: starting on channel %d, flags 0x%x\n", chan, qwz_mon_flags);

	if ((qwz_mon_flags & QWZ_MON_F_DELETE_STA) && sc->num_created_vdevs > 0 &&
	    (sc->allocated_vdev_map & (1U << sc->sc_vif.vdev_id))) {
		/* qwz_init() recreates it on the next interface-up (num_created_vdevs == 0). */
		ret = qwz_mac_vdev_delete_wait(sc, &sc->sc_vif);
		printf("qwz: mon: station vdev %u deleted for the capture: %d\n",
		    sc->sc_vif.vdev_id, ret);
		if (ret)
			return ret;
	}

	ret = qwz_mon_vdev_create(sc);
	if (ret)
		goto fail;
	ret = qwz_mon_vdev_start(sc, chan, freq);
	if (ret)
		goto fail;
	if (coex) {
		/* K5 (L1-C10): beside the station the filter is never swapped (KEEP_FILTER forced) unless moncoexfilter says 1 or 2; remember the swap so the stop restores exactly then */
		sc->mon_coex_swapped = (qwz_moncoexfilter != 0 && !(qwz_mon_flags & QWZ_MON_F_KEEP_FILTER)) ? 1 : 0;
		if (!sc->mon_coex_swapped)
			goto k5_coex_filter_kept;
	}
	if (!(qwz_mon_flags & QWZ_MON_F_KEEP_FILTER)) {
		ret = qwz_mon_rx_filter(sc, 1);
		if (ret)
			goto fail;
	}
k5_coex_filter_kept:	/* K5 (L1-C10) */
	if (qwz_mon_flags & QWZ_MON_F_MNTFILTER)
		qwz_mon_wmi_mnt_filter(sc, sc->sc_monvif.vdev_id, 1);
	if (qwz_mon_flags & QWZ_MON_F_PROMISC)
		qwz_mon_wmi_promisc(sc, 1);
	if ((qwz_mon_flags & (QWZ_MON_F_NRP | QWZ_MON_F_NRP_STA)) && sc->mon_bssid_set)
		qwz_mon_wmi_nrp(sc, sc->sc_monvif.vdev_id, sc->mon_bssid, 1,
		    !!(qwz_mon_flags & QWZ_MON_F_NRP_STA));
	if (coex) {
		/* K5 (L1-C2): the station must still be in RUN with its vdev up, else the cell goes again (the caller may retry) */
		if (sc->sc_ic.ic_state != IEEE80211_S_RUN || !sc->sc_vif.is_up || !(ifp->if_flags & IFF_RUNNING)) {
			printf("qwz: mon: coex: the station left RUN during the cell start (state %d): cell torn down\n", (int)sc->sc_ic.ic_state);
			ret = EAGAIN;
			goto fail;
		}
		sc->mon_coex = 1;	/* just before mon_active: every K5 branch tests this */
	}
	sc->mon_active = 1;
	timeout_set(&sc->mon_cell_timer, qwz_mon_reap, sc);	/* merge glue (E): the cell has its own timer; mon_reap_timer belongs to the DP code (qwz_stop deletes it) */
	timeout_add_msec(&sc->mon_cell_timer, 10);
	/* merge glue (G): the hold state IS "interface UP|RUNNING, no station": macOS sees Wi-Fi on and sharingd advertises */
	if (coex)
		goto k5_coex_no_hold;	/* K5 (L1-C2): the station already is UP|RUNNING; qwz_mon_run_raised stays 0 so the stop never clears its flag */
	if (qwz_mon_hold) {
		ifp->if_flags |= IFF_RUNNING;
		qwz_mon_run_raised = 1;	/* fix F4 */
	}
k5_coex_no_hold:
	qwz_awdl0_link(1);	/* merge glue (H3) */
	if (coex) {
		printf("qwz: mon: coex: station RUN on %d kept (vdev %u, mode %u, cfreq %u)\n", chan, sc->sc_monvif.vdev_id,
		    (unsigned int)sc->sc_monvif.chan_mode, (unsigned int)sc->sc_monvif.chan_cfreq1);
		printf("qwz: mon: coex: base_vdevs %d base_peers %d, cred reserve %d, filter mode %d (swapped %d), mgmt ids %d to %d, scan %d join %d fw %d\n",
		    sc->mon_coex_base_vdevs, sc->mon_coex_base_peers, qwz_moncoexcred, qwz_moncoexfilter, sc->mon_coex_swapped,
		    QWZ_MON_COEX_STA_SLOTS, (int)nitems(sc->sc_monvif.txmgmt.data) - 1, qwz_moncoexscan, qwz_moncoexjoin, qwz_moncoexfw);
		QWZ_MON_COEX_SETUP_END(sc);
	}
	printf("qwz: mon: capturing\n");
	return 0;

fail:
	if (coex)
		goto k5_coex_fail;	/* K5 (L1-C2): nothing was stopped, the firmware restart below is not wanted */
	sc->mon_fw_dirty = 1;
	if (qwz_mon_vdev_teardown(sc)) {
		printf("qwz: mon: start failed and the firmware stopped answering: restarting it\n");
		qwz_mon_fw_wedged(sc, "mon start: the firmware stopped answering");
	} else if (stopped_in_place) {
		/* fix F2: the station was stopped for this start; bring it back (the caller holds the ioctl lock, as enableGated does). mon_fw_dirty makes qwz_init restart the firmware */
		int rv = qwz_init(ifp);

		printf("qwz: mon: start failed (%d) after the station was stopped in place: station restart %s (%d)\n",
		    ret, rv ? "failed; the next Wi-Fi toggle recovers" : "ok", rv);
	}
	return ret;

k5_coex_fail:
	/*
	 * K5 (L1-C2): a coex start failed. The station was never stopped, so nothing is restarted: the cell vdev is torn down, the receive filter is
	 * restored when it was swapped, and mon_fw_dirty is set only when the teardown found the firmware silent (the engine then owns the restart)
	 * or moncoexfw is 1 (the old restart at the next qwz_init). qwz_init is never called.
	 */
	{
		int wedged = qwz_mon_vdev_teardown(sc);

		if (sc->mon_coex_swapped && !wedged && sc->fw_initialized && !QWZ_CRASHED(sc))
			printf("qwz: mon: coex: station filter restored: %d\n", qwz_mon_rx_filter(sc, 0));
		sc->mon_coex_swapped = 0;
		sc->mon_coex = 0;
		if (wedged) {
			printf("qwz: mon: coex: start failed (%d) and the firmware stopped answering: restarting it\n", ret);
			qwz_mon_fw_wedged(sc, "mon coex start: the firmware stopped answering");
		} else {
			if (qwz_moncoexfw == 1)
				sc->mon_fw_dirty = 1;
			printf("qwz: mon: coex: start failed (%d): cell torn down, the station was not touched\n", ret);
		}
		QWZ_MON_COEX_SETUP_END(sc);
		return ret;
	}
}

void
qwz_mon_stop(struct qwz_softc *sc)
{
	if (!sc->mon_active)
		return;
	/* merge glue (D2): a firmware that is gone cannot be told anything; the cell ends host-only (the daemon's mon=0 after a Wi-Fi off succeeds silently) */
	if (QWZ_CRASHED(sc) || !sc->fw_initialized) {
		qwz_mon_epoch_reset(sc);
		return;
	}
	sc->mon_active = 0;
	timeout_del(&sc->mon_cell_timer);
	/*
	 * Every cell leaves the firmware unable to serve the station: its own
	 * frames went out but no replies reached the host any more, so no
	 * network could be joined until a reboot. Mark it, and qwz_init
	 * restarts the firmware (the full bring-up, which is what works at
	 * boot) before the station comes up. Not here: another cell may
	 * follow without Wi-Fi in between, and it needs the firmware running.
	 */
	if (sc->mon_coex && qwz_moncoexfw != 1)
		goto k5_coex_no_dirty;	/* K5 (L1-C13): the station kept running beside the coex cell; the restart is only the old behaviour (debug.qwz.moncoexfw 1) */
	sc->mon_fw_dirty = 1;
k5_coex_no_dirty:
	/* merge glue (G, H3): the hold flag raised IFF_RUNNING at start; no station runs now, so the interface must not claim it, and awdl0 loses its link */
	if (qwz_mon_run_raised) {	/* fix F4: this cell raised it (the monhold value now may differ) */
		sc->sc_ic.ic_if.if_flags &= ~IFF_RUNNING;
		qwz_mon_run_raised = 0;
	}
	qwz_awdl0_link(0);
	if (qwz_mon_flags & QWZ_MON_F_PROMISC)
		qwz_mon_wmi_promisc(sc, 0);
	if ((qwz_mon_flags & QWZ_MON_F_MNTFILTER) && sc->sc_monvif.is_up)
		qwz_mon_wmi_mnt_filter(sc, sc->sc_monvif.vdev_id, 0);
	if ((qwz_mon_flags & (QWZ_MON_F_NRP | QWZ_MON_F_NRP_STA)) && sc->mon_bssid_set &&
	    sc->sc_monvif.is_up)
		qwz_mon_wmi_nrp(sc, sc->sc_monvif.vdev_id, sc->mon_bssid, 0,
		    !!(qwz_mon_flags & QWZ_MON_F_NRP_STA));
	if (qwz_mon_vdev_teardown(sc)) {
		printf("qwz: mon: the firmware stopped answering: restarting it now\n");
		qwz_mon_reap_ring(sc);
		qwz_mon_fw_wedged(sc, "mon stop: the firmware stopped answering");
		sc->mon_coex = 0;	/* K5 (L1-C2) */
		sc->mon_coex_swapped = 0;
		sc->mon_coex_halt = 0;	/* K5 (L1-C13) */
		return;
	}
	qwz_mon_reap_ring(sc);	/* return the buffers still in the ring */
	if (sc->mon_coex) {
		/* K5 (L1-C10): beside the station the filter is restored exactly when the start swapped it */
		if (sc->mon_coex_swapped)
			printf("qwz: mon: coex: station filter restored: %d\n", qwz_mon_rx_filter(sc, 0));
		goto k5_coex_filter_done;
	}
	if (!(qwz_mon_flags & QWZ_MON_F_KEEP_FILTER)) {
		if (qwz_mon_rx_filter(sc, 0))
			printf("qwz: mon: could not restore the station filter\n");
	}
k5_coex_filter_done:
	printf("qwz: mon: stopped: entries %u mpdus %u frames %u mgmt %u reo %u "
	    "reo_err %u wbm_err %u bufs %u badcookie %u nolink %u duplink %u "
	    "linkret_fail %u capdrop %u\n",
	    sc->mon_stats.entries, sc->mon_stats.mpdus, sc->mon_stats.frames,
	    sc->mon_stats.mgmt, sc->mon_stats.reo, sc->mon_stats.reo_err,
	    sc->mon_stats.wbm_err, sc->mon_stats.bufs, sc->mon_stats.badcookie,
	    sc->mon_stats.nolink, sc->mon_stats.duplink, sc->mon_stats.linkret_fail,
	    moncap_dropped);
	printf("qwz: mon: tx: psf heard %u, sent %u fail %u, compl ok %u discard %u inspect %u noack %u other %u, last ack rssi %d\n",
	    sc->mon_stats.rx_psf, sc->mon_stats.tx_sent, sc->mon_stats.tx_fail,
	    sc->mon_stats.tx_ok, sc->mon_stats.tx_discard, sc->mon_stats.tx_inspect,
	    sc->mon_stats.tx_noack, sc->mon_stats.tx_other, sc->mon_stats.last_ack_rssi);
	if (sc->mon_coex) {
		/* K5 (L1-C15, C14): the station kept running; watch it for 20 s unless debug.qwz.moncoexfw restores the old restart */
		printf("qwz: mon: coex: stopped, station frames since stop 0, fw_dirty %d\n", sc->mon_fw_dirty);
		if (qwz_moncoexfw != 1) {
			qwz_mon_probe_data0 = sc->sc_stall_data_seen;
			qwz_mon_probe_mgmt0 = sc->sc_stall_mgmt_seen;
			qwz_mon_probe_step = 0;
			sc->mon_coex_stopped_ns = getnsecuptime();
			sc->mon_coex_probe = 1;
		}
	}
	sc->mon_coex = 0;		/* K5 (L1-C2): the coex cell is over (mon_active went 0 above) */
	sc->mon_coex_swapped = 0;
	sc->mon_coex_halt = 0;		/* K5 (L1-C13) */
}

/*
 * merge glue (A): ends a capture cell WITHOUT talking to the firmware. Host state only: no WMI, no sleep, no allocation, idempotent, safe with no cell,
 * before qwz_mon_attach and with the DP rings already freed. qwz_core_deinit (when any cell field is set) and qwz_init_impl (a cell whose firmware is
 * gone) call it; qwz_mon_stop and qwz_mon_fw_wedged do too. C linkage: declared in qwz_mon.h and, identically, in qwz.c.
 */
void
qwz_mon_epoch_reset(struct qwz_softc *sc)
{
	int was_active;

	if (sc == NULL)
		return;
	was_active = sc->mon_active;
	if (was_active)
		printf("qwz: mon: capture cell ended by a firmware teardown (epoch %u)\n", sc->sc_fw_epoch);
	sc->mon_active = 0;
	sc->mon_coex = 0;		/* K5 (L1-C2): a firmware teardown ends a coex cell too */
	sc->mon_coex_swapped = 0;
	sc->mon_coex_halt = 0;		/* K5 (L1-C13) */
	sc->mon_coex_probe = 0;		/* K5 (L1-C14): no firmware left to watch */
	sc->mon_fw_dirty = 0;
	if (sc->mon_cell_timer != NULL)	/* never set up: nothing to cancel (timeout_del needs the gate) */
		timeout_del(&sc->mon_cell_timer);
	sc->sc_monvif.is_up = 0;
	sc->sc_monvif.is_started = 0;
	sc->mon_self_peer = 0;
	sc->mon_sender_peer = 0;
	memset(sc->mon_tx_kind, 0, sizeof(sc->mon_tx_kind));
	sc->mon_stats.ufr_inflight = 0;	/* K2b (W1b): the ring is purged below */
	if (sc->sc_monvif.txmgmt.data[0].map != NULL)
		qwz_mon_txq_purge(sc);
	/* a hold raised IFF_RUNNING for the cell (qwz_mon_start); it must not outlive the cell, or enableGated would skip qwz_init */
	if (qwz_mon_run_raised) {	/* fix F4: this cell raised it; the flag is only ever set while a cell runs */
		sc->sc_ic.ic_if.if_flags &= ~IFF_RUNNING;
		qwz_mon_run_raised = 0;
	}
	qwz_awdl0_link(0);
}

/* ---- entry points ----------------------------------------------------------- */

/*
 * Management frames never reach the RXDMA rings on this firmware: it
 * delivers them through WMI_MGMT_RX_EVENTID, with the channel, SNR and rate.
 * The event handler calls this while a capture runs instead of feeding
 * net80211. No FCS on this path.
 */
static uint8_t *mon_mgmt_buf;	/* QWZ_MONCAP_MAXFRAME bytes; WMI events and the REO path run under the gate */

void
qwz_mon_capture_reo_mbuf(struct qwz_softc *sc, struct mbuf *m)
{
	int len = M_PKTLEN(m);

	if (!sc->mon_active || mon_mgmt_buf == NULL || len <= 0 ||
	    len > QWZ_MONCAP_MAXFRAME)
		return;
	m_copydata(m, 0, len, mon_mgmt_buf);
	sc->mon_stats.reo++;
	moncap_put(mon_mgmt_buf, (size_t)len, QWZ_MONCAP_F_REO,
	    DP_RX_DECAP_TYPE_NATIVE_WIFI, (uint8_t)sc->mon_chan, 0, 0);
}

void
qwz_mon_capture_mgmt_mbuf(struct qwz_softc *sc, struct mbuf *m, int rssi,
    uint32_t rate_kbps)
{
	uint32_t r = rate_kbps / 500;
	int len = M_PKTLEN(m);

	if (!sc->mon_active || mon_mgmt_buf == NULL || len <= 0 ||
	    len > QWZ_MONCAP_MAXFRAME)
		return;
	m_copydata(m, 0, len, mon_mgmt_buf);
	sc->mon_stats.mgmt++;
	qwz_mon_rx_hook(sc, mon_mgmt_buf, (size_t)len, QWZ_MONCAP_F_WMI);
	moncap_put(mon_mgmt_buf, (size_t)len, QWZ_MONCAP_F_WMI,
	    DP_RX_DECAP_TYPE_RAW, (uint8_t)sc->mon_chan,
	    (int8_t)(rssi < -127 ? -127 : rssi > 0 ? 0 : rssi),
	    (uint8_t)(r > 255 ? 255 : r));
}

static void qwz_mon_coex_task_fn(void *);

void
qwz_mon_attach(struct qwz_softc *sc)
{
	qwz_mon_sc = sc;
	task_set(&sc->mon_coex_task, qwz_mon_coex_task_fn, sc, "qwzcoex");	/* K5 (L1-C13, C14) */
	sc->mon_coex_task_why = 0;
	if (moncap_lock == NULL)
		moncap_lock = IOLockAlloc();
	if (moncap_ring == NULL)
		moncap_ring = (uint8_t *)IOMalloc(QWZ_MONCAP_SIZE);
	if (mon_frame == NULL)
		mon_frame = (uint8_t *)IOMalloc(QWZ_MONCAP_MAXFRAME);
	if (mon_mgmt_buf == NULL)
		mon_mgmt_buf = (uint8_t *)IOMalloc(QWZ_MONCAP_MAXFRAME);
	if (!qwz_mon_have_maps(sc)) {	/* merge glue (D7): the softc decides, not a static flag (ItlQwz re-allocates it after a failed attach) */
		struct qwz_txmgmt_queue *q = &sc->sc_monvif.txmgmt;
		const bus_size_t size = IEEE80211_MAX_LEN;
		int i, ret = 0;

		memset(q, 0, sizeof(*q));
		for (i = 0; i < nitems(q->data) && ret == 0; i++)
			ret = bus_dmamap_create(sc->sc_dmat, size, 1, size, 0,
			    BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW, &q->data[i].map);
		if (ret) {
			for (i = 0; i < nitems(q->data); i++)
				if (q->data[i].map != NULL) {
					bus_dmamap_destroy(sc->sc_dmat, q->data[i].map);
					q->data[i].map = NULL;
				}
			printf("qwz: mon: transmit DMA maps failed: %d (no M1 transmit)\n", ret);
		}
	}
	moncap_head = moncap_tail = 0;
}

/*
 * merge glue (F): the softc is about to go (ItlQwz::detachGated, under the gate, after qwz_sysctl_set_mon(NULL)). Ends a cell host-only first (the
 * queue's mbufs are unloaded through the maps), frees the capture buffers and the lock, destroys the 64 maps and forgets the softc. Attach
 * allocates them again.
 */
void
qwz_mon_detach(struct qwz_softc *sc)
{
	struct qwz_txmgmt_queue *q;
	uint8_t *ring = moncap_ring, *frame = mon_frame, *mgmt = mon_mgmt_buf;
	IOLock *lock = moncap_lock;
	int i;

	if (sc == NULL)
		return;
	task_del(systq, &sc->mon_coex_task);	/* K5 (L1-C13, C14) */
	qwz_mon_epoch_reset(sc);
	q = &sc->sc_monvif.txmgmt;
	for (i = 0; i < (int)nitems(q->data); i++)
		if (q->data[i].map != NULL) {
			bus_dmamap_destroy(sc->sc_dmat, q->data[i].map);
			q->data[i].map = NULL;
		}
	if (qwz_mon_sc == sc)
		qwz_mon_sc = NULL;
	if (lock != NULL)
		IOLockLock(lock);
	moncap_ring = NULL;
	moncap_head = moncap_tail = 0;
	if (lock != NULL)
		IOLockUnlock(lock);
	mon_frame = NULL;
	mon_mgmt_buf = NULL;
	moncap_lock = NULL;
	if (ring != NULL)
		IOFree(ring, QWZ_MONCAP_SIZE);
	if (frame != NULL)
		IOFree(frame, QWZ_MONCAP_MAXFRAME);
	if (mgmt != NULL)
		IOFree(mgmt, QWZ_MONCAP_MAXFRAME);
	if (lock != NULL)
		IOLockFree(lock);
}

static IOReturn
qwz_mon_gated(OSObject *, void *arg0, void *arg1, void *, void *)
{
	struct qwz_softc *sc = qwz_mon_sc;
	int chan = (int)(intptr_t)arg0;
	int *errp = (int *)arg1;
	int ret;

	if (sc == NULL) {
		*errp = ENXIO;
		return kIOReturnSuccess;
	}
	ret = rw_enter_write(&sc->ioctl_rwl);	/* merge glue (D5): ours returns EBUSY when the lock stays busy */
	if (ret != 0) {
		*errp = ret;
		return kIOReturnSuccess;
	}
	if (chan == 0) {
		qwz_mon_stop(sc);
		*errp = 0;
	} else {
		if (sc->mon_active)
			qwz_mon_stop(sc);
		*errp = qwz_mon_start(sc, chan);
	}
	rw_exit(&sc->ioctl_rwl);
	return kIOReturnSuccess;
}

/*
 * K5 (L1-C13): the station is about to leave RUN (the newstate task, which runs inside the gate on a thread-call thread): stop a running coex cell first,
 * through the same gated entry the sysctl uses (ioctl_rwl, qwz_mon_stop; the gate is recursive for its holder). mon_coex_halt makes the user-frame write
 * answer ENXIO meanwhile. Bounded: 20 tries, 100 ms apart, then the cell ends host-only (qwz_mon_epoch_reset), the firmware's cell vdev is then left to the
 * teardown that follows. Not counted in task_refs; qwz_stop stops the cell before it waits for the tasks, so the lock order has no cycle.
 */
int
qwz_mon_coex_halt(struct qwz_softc *sc)
{
	IOCommandGate *gate = qwz_compat_gate();
	int i, err = 0;

	if (sc == NULL || gate == NULL || !qwz_mon_coex_running(sc))
		return 0;
	printf("qwz: mon: coex: stopped (station leaving)\n");
	sc->mon_coex_halt = 1;
	for (i = 0; i < 20 && sc->mon_active; i++) {
		err = 0;
		gate->runAction(qwz_mon_gated, (void *)(intptr_t)0, &err);
		if (!sc->mon_active)
			break;
		if (tsleep_delay_nsec(&sc->mon_coex_halt, 0, "qwzhalt", MSEC_TO_NSEC(100)) == EDEADLK)
			break;
	}
	if (sc->mon_active) {
		printf("qwz: mon: coex: the cell did not stop in 2 s (%d): ended host-only\n", err);
		qwz_mon_epoch_reset(sc);
	}
	sc->mon_coex_halt = 0;
	return 1;
}

/* K5 (L1-C13, C14): the work the 1 s tick hands over (bit 1 stop the cell, 2 restore the RX filter, 4 restart the station vdev); runs in the gate on systq, where sleeping is allowed */
static void
qwz_mon_coex_task_fn(void *arg)
{
	struct qwz_softc *sc = (struct qwz_softc *)arg;
	struct ieee80211com *ic;
	int why, ret;

	if (sc == NULL || sc != qwz_mon_sc)
		return;
	why = __atomic_exchange_n(&sc->mon_coex_task_why, 0, __ATOMIC_ACQ_REL);
	ic = &sc->sc_ic;
	if (why & 1)
		qwz_mon_coex_halt(sc);
	if ((why & 2) && !sc->mon_active && sc->fw_initialized && !QWZ_CRASHED(sc) && ic->ic_state == IEEE80211_S_RUN) {
		ret = qwz_dp_rxdma_ring_sel_config_wcn7850(sc);
		printf("qwz: mon: coex: step 2: station receive filter restored explicitly: %d\n", ret);
	}
	if ((why & 4) && !sc->mon_active && sc->fw_initialized && !QWZ_CRASHED(sc) && ic->ic_state == IEEE80211_S_RUN &&
	    sc->sc_vif.is_started && ic->ic_bss != NULL) {
		ret = qwz_mac_vdev_restart(sc, &sc->sc_vif, sc->pdevs[0].pdev_id);
		if (ret == 0)
			ret = qwz_wmi_vdev_up(sc, sc->sc_vif.vdev_id, sc->pdevs[0].pdev_id, sc->sc_vif.aid, sc->sc_vif.bssid, NULL, 0, 0);
		printf("qwz: mon: coex: step 3: station vdev restarted and brought up: %d\n", ret);
	}
}

/*
 * K5 (L1-C13 (5), C14): called once a second from qwz_watchdog, on the workloop thread (under the gate, may not sleep: everything slow is handed to
 * qwz_mon_coex_task_fn). While a coex cell runs: the cell stops when the station left RUN or its channel is no longer the cell's. After a coex cell
 * stopped (mon_coex_probe, 20 s): the station's received-frame counters (sc_stall_data_seen and sc_stall_mgmt_seen, the ones the data-stall watcher uses)
 * must move; if nothing arrived by 5 s while the station is in RUN, escalate once per cell: step 1 log (the null-data probe frame is NOT implemented, the
 * tree has no null-data sender), step 2 at 6 s restore the RX filter, step 3 at 8 s restart the station vdev, step 4 at 11 s mon_fw_dirty and init_task
 * (today's behaviour). debug.qwz.moncoexfw 1 never arms the probe: the old restart stays.
 */
void
qwz_mon_coex_probe(struct qwz_softc *sc)
{
	struct ieee80211com *ic;
	uint64_t now, el;
	uint32_t frames;

	if (sc == NULL || sc != qwz_mon_sc)
		return;
	ic = &sc->sc_ic;
	if (sc->mon_active && sc->mon_coex) {
		if (sc->mon_coex_halt || sc->mon_coex_task_why != 0)
			return;
		if (ic->ic_state != IEEE80211_S_RUN || ic->ic_bss == NULL || ic->ic_bss->ni_chan == NULL ||
		    ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan) != sc->mon_chan) {
			printf("qwz: mon: coex: the station left RUN or changed channel (state %d): stopping the cell\n", (int)ic->ic_state);
			__atomic_fetch_or(&sc->mon_coex_task_why, 1, __ATOMIC_ACQ_REL);
			task_add(systq, &sc->mon_coex_task);
		}
		return;
	}
	if (!sc->mon_coex_probe)
		return;
	if (sc->mon_active || ic->ic_state != IEEE80211_S_RUN || !sc->fw_initialized || QWZ_CRASHED(sc)) {
		sc->mon_coex_probe = 0;
		return;
	}
	now = getnsecuptime();
	el = (now - sc->mon_coex_stopped_ns) / 1000000ULL;
	frames = (sc->sc_stall_data_seen - qwz_mon_probe_data0) + (sc->sc_stall_mgmt_seen - qwz_mon_probe_mgmt0);
	if (frames != 0) {
		printf("qwz: mon: coex: station frames since stop %u after %llu ms: the station hears (escalation step %d)\n", frames,
		    (unsigned long long)el, qwz_mon_probe_step);
		sc->mon_coex_probe = 0;
		return;
	}
	if (el >= 20000) {
		printf("qwz: mon: coex: station silent for 20 s after the cell stop (escalation step %d), probe ends\n", qwz_mon_probe_step);
		sc->mon_coex_probe = 0;
		return;
	}
	if (qwz_mon_probe_step == 0 && el >= 5000) {
		qwz_mon_probe_step = 1;
		printf("qwz: mon: coex: station silent after cell stop (%llu ms, 0 frames); probe frame not implemented (no null-data sender in this tree)\n", (unsigned long long)el);
	} else if (qwz_mon_probe_step == 1 && el >= 6000) {
		qwz_mon_probe_step = 2;
		__atomic_fetch_or(&sc->mon_coex_task_why, 2, __ATOMIC_ACQ_REL);
		task_add(systq, &sc->mon_coex_task);
	} else if (qwz_mon_probe_step == 2 && el >= 8000) {
		qwz_mon_probe_step = 3;
		__atomic_fetch_or(&sc->mon_coex_task_why, 4, __ATOMIC_ACQ_REL);
		task_add(systq, &sc->mon_coex_task);
	} else if (qwz_mon_probe_step == 3 && el >= 11000) {
		qwz_mon_probe_step = 4;
		printf("qwz: mon: coex: step 4: still silent after %llu ms: firmware restart queued (mon_fw_dirty, init_task)\n", (unsigned long long)el);
		sc->mon_fw_dirty = 1;
		sc->mon_coex_probe = 0;
		task_add(systq, &sc->init_task);
	}
}

int
qwz_mon_set(int chan)
{
	IOCommandGate *gate = qwz_compat_gate();
	int err = ENXIO;

	if (gate == NULL || qwz_mon_sc == NULL)
		return ENXIO;
	if (chan < 0 || (chan != 0 && qwz_mon_chan_freq(chan) == 0))
		return EINVAL;
	gate->runAction(qwz_mon_gated, (void *)(intptr_t)chan, &err);
	return err;
}

static IOReturn
qwz_mon_tx_gated(OSObject *, void *arg0, void *arg1, void *, void *)
{
	struct qwz_softc *sc = qwz_mon_sc;
	int ops = (int)(intptr_t)arg0, *errp = (int *)arg1, ret = 0;

	if (sc == NULL) {
		*errp = ENXIO;
		return kIOReturnSuccess;
	}
	qwz_mon_tx_mode = ops & 2;
	if (ops & 1)
		ret = qwz_mon_tx_copy(sc, qwz_bcast);
	if ((ops & 4) && ret == 0)
		ret = qwz_mon_tx_copy(sc, mon_psf_src);
	if (ops & 5)
		printf("qwz: mon: tx: ops 0x%x -> %d (psf %zu bytes from %s)\n", ops, ret,
		    mon_psf_len, ether_sprintf(mon_psf_src));
	*errp = ret;
	return kIOReturnSuccess;
}

int
qwz_mon_tx(int ops)
{
	IOCommandGate *gate = qwz_compat_gate();
	int err = ENXIO;

	if (gate == NULL || qwz_mon_sc == NULL)
		return ENXIO;
	gate->runAction(qwz_mon_tx_gated, (void *)(intptr_t)ops, &err);
	return err;
}

void
qwz_mon_mac_str(char *out, size_t len)
{
	struct qwz_softc *sc = qwz_mon_sc;
	const uint8_t *m;

	if (sc == NULL) {
		snprintf(out, len, "00:00:00:00:00:00");
		return;
	}
	m = sc->mon_active ? sc->sc_monvif.vdev_addr : sc->mac_addr;
	snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

int
qwz_mon_status(void)
{
	struct qwz_softc *sc = qwz_mon_sc;

	return (sc != NULL && sc->mon_active) ? sc->mon_chan : 0;
}
