/*
 * qwz_health.cpp - the evidence rails of the M0 freeze-proofing build:
 * health line, statistics names, WMI flight recorder, event notes and the
 * firmware dossier (group X, C26 to C29 of docs/m0-spec.md).
 *
 * Everything here is log-only or counter-only: no behaviour change. The RAM
 * ring QwzMacLog stays the vehicle (qwz_log.cpp); this file only decides what
 * goes into it. Compiled into BOTH kexts: no Airport-only symbols, no Airport
 * headers.
 *
 * Copyright (c) 2026. GPL-2.0-or-later (see LICENSE).
 */
#include "qwz_compat.h"
#include "qwzreg.h"
#include "qwzvar.h"
#include "qwz_awdlx.h"
#include "qwz_mon.h"	/* merge glue (I): qwz_mon_hold */

/*
 * Statistics (C26): sc->sc_stat[] is indexed by QWZ_STAT_* (qwzvar.h); these
 * are the labels the health line prints, so a label is the text between the
 * brackets of its group: htt[ok=.. drop=.. ...], tqm[0=.. 1=.. ...].
 */
extern const char *const qwz_stat_names[QWZ_STAT_MAX];
const char *const qwz_stat_names[QWZ_STAT_MAX] = {
	/* QWZ_STAT_HTT0: HAL_WBM_REL_HTT_TX_COMP_STATUS_* 0..6, slot 7 = anything above */
	"ok", "drop", "ttl", "reinj", "insp", "mec", "6", "7+",
	/* QWZ_STAT_TQM0: HAL_WBM_TQM_REL_REASON_* 0..6, slot 7 = above */
	"0", "1", "2", "3", "4", "5", "6", "7+",
	/* QWZ_STAT_ENOSPC .. QWZ_STAT_PM_FAIL */
	"enospc", "wall", "tx", "done", "cap", "drop", "pm_ok", "pm_fail"
};

/*
 * Appends printf text to buf[off..size) and returns the new offset. A line
 * that does not fit is cut (qwz_vlog cuts at 384 bytes anyway), never
 * overflowed.
 */
static size_t
qwz_hl_add(char *buf, size_t size, size_t off, const char *fmt, ...) __attribute__((format(__printf__, 4, 5)));

static size_t
qwz_hl_add(char *buf, size_t size, size_t off, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (off + 1 >= size)
		return off;
	va_start(ap, fmt);
	n = vsnprintf(buf + off, size - off, fmt, ap);
	va_end(ap);
	if (n < 0)
		return off;
	if ((size_t)n >= size - off)
		return size - 1;
	return off + (size_t)n;
}

/* " <label>=<value>" for one statistic */
static size_t
qwz_hl_stat(char *buf, size_t size, size_t off, struct qwz_softc *sc, int idx, int first)
{
	return qwz_hl_add(buf, size, off, "%s%s=%u", first ? "" : " ", qwz_stat_names[idx], sc->sc_stat[idx]);
}

/*
 * The engine's own state name (ItlQwz::engineStateName) lives in the
 * controller, and this function only gets the softc, so the state is derived
 * from the life word: GIVEN_UP, DEAD (the engine has been asked, or is tearing
 * down), RECOVERY (DEAD already cleared by the restart, the episode still
 * open: the engine's RESTARTING and RETRY), else UP.
 */
static const char *
qwz_hl_state(uint32_t life)
{
	if (life & QWZ_LIFE_GIVEN_UP)
		return "GIVEN_UP";
	if (life & QWZ_LIFE_DEAD)
		return "DEAD";
	if (life & QWZ_LIFE_RECOVERY)
		return "RECOVERY";
	return "UP";
}

static uint32_t
qwz_hl_now_ms(void)
{
	return (uint32_t)(getnsecuptime() / 1000000ULL);
}

/*
 * M2a (WP3, I8): the AWDL evidence summary (qwz_awdlx.cpp). It prints nothing unless awdl0's link is up or the listen vdev is
 * joined, and it reads only the evidence block and the few softc fields copied into `env`: safe in every state, like line 1.
 */
static void
qwz_hl_awdlx(struct qwz_softc *sc, uint32_t life)
{
	struct qwz_awdlx_env env;

	env.listening = sc->sc_awdl_listening;
	env.quiesce = (int)__atomic_load_n(&sc->sc_awdl_quiesce, __ATOMIC_RELAXED);
	env.dead = (life & QWZ_LIFE_DEAD) != 0;
	env.probe = qwz_awdlprobe;
	env.txmask = sc->sc_awdltest_txmask;
	env.tcl_done = sc->sc_stat[QWZ_STAT_AWDL_DONE];
	env.tcl_inflight = sc->sc_awdl_inflight > 0 ? (uint32_t)sc->sc_awdl_inflight : 0;
	qwz_awdlx_health(&env);
}

/*
 * C26: up to three ring lines, each under the 384-byte limit of qwz_vlog (the
 * line head included). Called ONLY under the main gate (ItlQwz::healthGated)
 * and SAFE IN EVERY STATE: the sentinel samples every tick, also while the
 * chip is down (Wi-Fi off, system sleep, a restart in progress, GIVEN_UP), and
 * qwz_dp_free() frees sc->dp.tx_ring[0].data and sets it to NULL while `queued`
 * keeps its stale value.
 *
 *  (a) line 1 reads only fields that live inside the softc or the PCI softc
 *      (the life word, the counters, the HTC credits after a range check, the
 *      peer COUNT and never a list walk, the task and lock words, the cached EE
 *      and MHI words) and prints in every state;
 *  (b) lines 2 and 3 scan the TX ring and are printed only when
 *      fw_initialized && tx_ring[0].data != NULL, tested at the point of use
 *      under the gate (no sleep between the test and the scan, so nothing can
 *      free the array in between), the scan bounded by hw_params.tx_ring_size,
 *      the constant the array was allocated with; otherwise one short
 *      "txq=down" line is printed instead;
 *  (c) the management slots are the inline array sc_vif.txmgmt.data[8], valid
 *      in every state (only their count is printed).
 */
void
qwz_health_log(struct qwz_softc *sc, unsigned int wdskip)
{
	struct dp_tx_ring *ring;
	uint32_t life, ee = 0, mhi = 0, now_ms, oldest = 0, awdl_oldest = 0;
	uint32_t i, ring_size;
	int cred = -1, tasks, ic_state;
	const char *ic_name;
	char buf[352];
	size_t off;

	if (sc == NULL)
		return;
	life = __atomic_load_n(&sc->sc_life, __ATOMIC_ACQUIRE);
	qwz_pci_cached_ee_state(sc, &ee, &mhi);
	if ((unsigned int)sc->wmi.wmi[0].eid < (unsigned int)ATH12K_HTC_EP_COUNT)
		cred = sc->htc.endpoint[sc->wmi.wmi[0].eid].tx_credits;
	ic_state = (int)sc->sc_ic.ic_state;
	ic_name = ic_state >= 0 && ic_state < (int)IEEE80211_S_MAX ? ieee80211_state_name[ic_state] : "?";
	tasks = (int)sc->task_refs.r_refs - 1;		/* refcnt_init counts the base reference: what is left is tasks in flight */
	if (tasks < 0)
		tasks = 0;

	qwz_log("qwz: health: st=%s dead=%d ep=%u eps=%u gu=%d dups=%u fw=%d ee=%u mhi=0x%x cred=%d | ic=%s ifflags=0x%x scan=%d peers=%d vdevs=%d/%d tasks=%d rwl=%d | "
	    "fwev=%u silent=%u refused=%u mmioblk=%d mem=%u/%lluKB wdskip=%u\n",
	    qwz_hl_state(life), (life & QWZ_LIFE_DEAD) != 0, sc->sc_fw_epoch, sc->sc_dead_episodes, (life & QWZ_LIFE_GIVEN_UP) != 0,
	    sc->sc_dead_dups, sc->fw_initialized, ee, mhi, cred,
	    ic_name, sc->sc_ic.ic_if.if_flags, (int)sc->scan.state, sc->num_peers, sc->num_created_vdevs, sc->num_started_vdevs, tasks,
	    sc->ioctl_rwl.rwl_owner != NULL,
	    __atomic_load_n(&sc->sc_fw_rx_events, __ATOMIC_RELAXED), sc->sc_silent_count, qwz_refused_count(),
	    (life & QWZ_LIFE_MMIO_BLOCK) != 0,
	    __atomic_load_n(&qwz_mem_blocks, __ATOMIC_RELAXED),
	    (unsigned long long)(__atomic_load_n(&qwz_mem_bytes, __ATOMIC_RELAXED) >> 10), wdskip);

	if (sc->mon_active)	/* merge glue (I): the capture cell (qwz_mon.c), printed only while one runs; the counters are qwz_mon.c's own */
		qwz_log("qwz: health: mon[active=%d chan=%d hold=%u dirty=%d frames=%u mgmt=%u reo=%u psf=%u tx=%u ok=%u fail=%u qd=%d] inflight=%u\n",
		    sc->mon_active, sc->mon_chan, qwz_mon_hold, sc->mon_fw_dirty, sc->mon_stats.frames, sc->mon_stats.mgmt, sc->mon_stats.reo,
		    sc->mon_stats.rx_psf, sc->mon_stats.tx_sent, sc->mon_stats.tx_ok, sc->mon_stats.tx_fail, sc->sc_monvif.txmgmt.queued, sc->mon_stats.ufr_inflight);

	if (sc->mon_coex)	/* K5 (L1-C15): the coex cell beside the station: the WMI credit reserve in force and the claim-test totals */
		qwz_log("qwz: health: coex=1 resv=%d cell_vs_station mgmt %u/%u data %u/%u drop %u probe=%d\n", qwz_moncoexcred, sc->mon_stats.coex_cell_mgmt,
		    sc->mon_stats.coex_st_mgmt, sc->mon_stats.coex_cell_data, sc->mon_stats.coex_st_data, sc->mon_stats.coex_drop, sc->mon_coex_probe);

	/* rule (b): the test and the scan below are one gate hold with no sleep between them */
	ring = &sc->dp.tx_ring[0];
	if (!sc->fw_initialized || ring->data == NULL) {
		qwz_log("qwz: health: txq=down (fw_initialized %d, ring %s)\n", sc->fw_initialized, ring->data == NULL ? "none" : "present");
		qwz_hl_awdlx(sc, life);
		return;
	}
	ring_size = sc->hw_params.tx_ring_size;
	now_ms = qwz_hl_now_ms();
	for (i = 0; i < ring_size; i++) {
		struct qwz_tx_data *d = &ring->data[i];
		int32_t age;

		if (d->m == NULL || d->t_ms == 0)		/* t_ms 0: never stamped (C20 stamps every publish) */
			continue;
		age = (int32_t)(now_ms - d->t_ms);
		if (age < 0)
			age = 0;
		if ((uint32_t)age > oldest)
			oldest = (uint32_t)age;
		if ((d->flags & QWZ_TXF_AWDL) && (uint32_t)age > awdl_oldest)
			awdl_oldest = (uint32_t)age;
	}

	off = qwz_hl_add(buf, sizeof(buf), 0, "txq=%d/%u cur=%d old=%ums ", ring->queued, ring_size, ring->cur, oldest);
	off = qwz_hl_stat(buf, sizeof(buf), off, sc, QWZ_STAT_TXWALL, 1);
	off = qwz_hl_add(buf, sizeof(buf), off, " mgmtq=%d ", sc->sc_vif.txmgmt.queued);
	off = qwz_hl_stat(buf, sizeof(buf), off, sc, QWZ_STAT_ENOSPC, 1);
	off = qwz_hl_add(buf, sizeof(buf), off, " | htt[");
	for (i = 0; i < 8; i++)
		off = qwz_hl_stat(buf, sizeof(buf), off, sc, QWZ_STAT_HTT0 + (int)i, i == 0);
	off = qwz_hl_add(buf, sizeof(buf), off, "] tqm[");
	for (i = 0; i < 8; i++)
		off = qwz_hl_stat(buf, sizeof(buf), off, sc, QWZ_STAT_TQM0 + (int)i, i == 0);
	qwz_hl_add(buf, sizeof(buf), off, "]");
	qwz_log("qwz: health: %s\n", buf);

	off = qwz_hl_add(buf, sizeof(buf), 0, "awdl[listen=%d peers=%d ", sc->sc_awdl_listening, sc->sc_awdl_npeers);
	off = qwz_hl_stat(buf, sizeof(buf), off, sc, QWZ_STAT_AWDL_TX, 1);
	off = qwz_hl_stat(buf, sizeof(buf), off, sc, QWZ_STAT_AWDL_DONE, 0);
	off = qwz_hl_stat(buf, sizeof(buf), off, sc, QWZ_STAT_AWDL_CAP, 0);
	off = qwz_hl_stat(buf, sizeof(buf), off, sc, QWZ_STAT_AWDL_DROP, 0);
	off = qwz_hl_stat(buf, sizeof(buf), off, sc, QWZ_STAT_PM_OK, 0);
	off = qwz_hl_stat(buf, sizeof(buf), off, sc, QWZ_STAT_PM_FAIL, 0);
	off = qwz_hl_add(buf, sizeof(buf), off, " inflight=%d zombies=%d old=%ums probe=%d] rx[mgmt_act=%u mgmt_a3=%u data_a3=%u]",
	    sc->sc_awdl_inflight, sc->sc_awdl_nzombie, awdl_oldest, qwz_awdlprobe,
	    sc->sc_awdl_mgmt_action, sc->sc_awdl_mgmt_bssid, sc->sc_awdl_data_bssid);
	qwz_log("qwz: health: %s\n", buf);
	qwz_hl_awdlx(sc, life);
}

/*
 * C26 flight recorder: the last 32 WMI commands sent (dir 0) and events
 * received (dir 1), as {ms since boot, id}. qwz_flight_note runs from
 * qwz_wmi_cmd_send (under the gate) and qwz_wmi_tlv_op_rx (the interrupt
 * action, also under the gate), one writer per direction; the entry is stored
 * before the counter that publishes it.
 */
void
qwz_flight_note(struct qwz_softc *sc, int dir, uint32_t id)
{
	struct qwz_flight_ent *e;
	uint32_t n;

	if (sc == NULL || dir < 0 || dir > 1)
		return;
	n = sc->sc_flight_n[dir];
	e = &sc->sc_flight[dir][n % nitems(sc->sc_flight[dir])];
	e->t_ms = qwz_hl_now_ms();
	e->id = id;
	__atomic_store_n(&sc->sc_flight_n[dir], n + 1, __ATOMIC_RELEASE);
}

/*
 * The last 12 of each direction, newest first, as TWO ring lines: one line of
 * 24 entries would exceed qwz_vlog's 384-byte buffer and cut off the NEWEST
 * events, the most valuable ones. Each line is at most 12 x 25 + 14 + 18 bytes.
 */
#define QWZ_FLIGHT_SHOW	12

void
qwz_flight_dump(struct qwz_softc *sc)
{
	static const char *const names[2] = { "cmds", "events" };
	uint32_t now_ms = qwz_hl_now_ms();
	int dir;

	if (sc == NULL)
		return;
	for (dir = 0; dir < 2; dir++) {
		char buf[QWZ_FLIGHT_SHOW * 26 + 8];
		uint32_t n = __atomic_load_n(&sc->sc_flight_n[dir], __ATOMIC_ACQUIRE), k, shown;
		size_t off = 0;

		shown = n < QWZ_FLIGHT_SHOW ? n : QWZ_FLIGHT_SHOW;
		buf[0] = '\0';
		for (k = 0; k < shown; k++) {
			const struct qwz_flight_ent *e = &sc->sc_flight[dir][(n - 1 - k) % nitems(sc->sc_flight[dir])];
			uint32_t age = now_ms - e->t_ms;

			if (age > 99999)
				age = 99999;
			off = qwz_hl_add(buf, sizeof(buf), off, "%s0x%x@-%ums", k == 0 ? "" : " ", e->id, age);
		}
		if (shown == 0)
			qwz_log("qwz: flight: %s (none)\n", names[dir]);
		else
			qwz_log("qwz: flight: %s %s\n", names[dir], buf);
	}
}

/*
 * C27: log-only notes for WMI events the host never acted on. "unhandled":
 * each id once (a 16-entry seen table) with its length. Anything else (the
 * peer kickout): the id and the first 16 bytes of the payload, for the first
 * 3 occurrences. `m` has already had the WMI command header removed.
 */
void
qwz_evt_note(struct qwz_softc *sc, uint32_t id, struct mbuf *m, const char *what)
{
	static uint32_t seen[16];
	static unsigned int nseen, notes, table_full;
	int len = m != NULL ? M_PKTLEN(m) : 0;
	unsigned int i;

	(void)sc;
	if (what != NULL && strcmp(what, "unhandled") == 0) {
		for (i = 0; i < nseen; i++) {
			if (seen[i] == id)
				return;
		}
		if (nseen >= nitems(seen)) {
			if (!table_full) {
				table_full = 1;
				qwz_log("qwz: evt: unhandled wmi event table full (%u ids): further new ids are not listed\n", (unsigned int)nitems(seen));
			}
			return;
		}
		seen[nseen++] = id;
		qwz_log("qwz: evt: wmi event 0x%x unhandled: len %d\n", id, len);
		return;
	}
	if (++notes <= 3) {
		char hex[3 * 16 + 1];
		const uint8_t *p = m != NULL ? mtod(m, const uint8_t *) : NULL;
		unsigned int n = m != NULL && M_LEN(m) > 0 ? (unsigned int)M_LEN(m) : 0;
		unsigned int k;

		if (n > 16)
			n = 16;
		for (k = 0; p != NULL && k < n; k++)
			snprintf(hex + 3 * k, sizeof(hex) - 3 * k, "%02x ", p[k]);
		hex[p != NULL && n > 0 ? 3 * n - 1 : 0] = '\0';	/* drop the trailing blank */
		qwz_log("qwz: evt: wmi event 0x%x %s #%u: len %d first bytes %s\n", id, what != NULL ? what : "?", notes, len, hex);
	}
}

/*
 * K1 W2a: the hardware-mode facts, log only. Which modes the firmware offers,
 * each phy's pdev id, bands and chain masks, how many pdevs the driver
 * counted, the mode index it asked for and the payload of the set-mode
 * response. One block per firmware boot, about ten lines; nothing here
 * changes state or feeds a decision.
 */
static const char *
qwz_hwmode_name(uint32_t id)
{
	switch (id) {
	case 0: return "SINGLE";
	case 1: return "DBS";
	case 2: return "SBS_PASSIVE";
	case 3: return "SBS";
	case 4: return "DBS_SBS";
	case 5: return "DBS_OR_SBS";
	default: return "?";
	}
}

static void
qwz_dossier_hwmode(struct qwz_softc *sc)
{
	unsigned int i;

	qwz_log("qwz: dossier: hwmode summary: offered %u (kept %u) phys %u (kept %u) preferred %d asked %d%s single_pdev_only %d target_pdevs %d num_radios %d\n",
	    sc->hwm_nmodes_total, (unsigned int)sc->hwm_nmodes, sc->hwm_nphys_total, (unsigned int)sc->hwm_nphys,
	    (int)sc->wmi.preferred_hw_mode, sc->hwm_asked, sc->hwm_asked_set ? "" : " (never asked)",
	    (int)sc->hw_params.single_pdev_only, (int)sc->target_pdev_count, sc->num_radios);
	for (i = 0; i < sc->hwm_nmodes; i++)
		qwz_log("qwz: dossier: hwmode mode %u: id %u (%s) cfg_type %u phy_id_map 0x%x\n", i,
		    sc->hwm_modes[i].hw_mode_id, qwz_hwmode_name(sc->hwm_modes[i].hw_mode_id),
		    sc->hwm_modes[i].cfg_type, sc->hwm_modes[i].phy_id_map);
	for (i = 0; i < sc->hwm_nphys; i++)
		qwz_log("qwz: dossier: hwmode phy %u: hw_mode_id %u pdev_id %u phy_id %u lmac %u bands 0x%x chains 2g tx 0x%x rx 0x%x 5g tx 0x%x rx 0x%x\n", i,
		    sc->hwm_phys[i].hw_mode_id, sc->hwm_phys[i].pdev_id, sc->hwm_phys[i].phy_id, sc->hwm_phys[i].lmac_id,
		    sc->hwm_phys[i].bands, sc->hwm_phys[i].tx2g, sc->hwm_phys[i].rx2g, sc->hwm_phys[i].tx5g, sc->hwm_phys[i].rx5g);
	for (i = 0; i < sc->target_pdev_count && i < MAX_RADIOS; i++)
		qwz_log("qwz: dossier: hwmode target pdev %u: pdev_id %u bands 0x%x\n", i,
		    sc->target_pdev_ids[i].pdev_id, (unsigned int)sc->target_pdev_ids[i].supported_bands);
	if (!sc->hwm_resp_seen) {
		qwz_log("qwz: dossier: hwmode resp: none seen (the driver sets a mode only when single_pdev_only)\n");
		return;
	}
	qwz_log("qwz: dossier: hwmode resp: seen %d len %u tlvs %u event %s vdev-mac entries found %u raw %08x %08x %08x %08x %08x %08x\n",
	    sc->hwm_resp_seen, sc->hwm_resp_len, sc->hwm_resp_ntlv, sc->hwm_resp_have_ev ? "yes" : "no",
	    (unsigned int)sc->hwm_resp_nent,
	    sc->hwm_resp_raw[0], sc->hwm_resp_raw[1], sc->hwm_resp_raw[2], sc->hwm_resp_raw[3], sc->hwm_resp_raw[4], sc->hwm_resp_raw[5]);
	if (sc->hwm_resp_have_ev) {
		if (sc->hwm_resp_ev_tlen >= 16)
			qwz_log("qwz: dossier: hwmode resp w0=%u w1=%u w2=%u w3=%u (tlen %u; layout unverified; firmware fixed_param order is believed to be pdev_id, status, cfgd_hw_mode_index, num_vdev_mac_entries)\n",
			    sc->hwm_resp_w0, sc->hwm_resp_w1, sc->hwm_resp_w2, sc->hwm_resp_w3, sc->hwm_resp_ev_tlen);
		else if (sc->hwm_resp_ev_tlen >= 12)
			qwz_log("qwz: dossier: hwmode resp w0=%u w1=%u w2=%u (w3 absent) (tlen %u; layout unverified; firmware fixed_param order is believed to be pdev_id, status, cfgd_hw_mode_index, num_vdev_mac_entries)\n",
			    sc->hwm_resp_w0, sc->hwm_resp_w1, sc->hwm_resp_w2, sc->hwm_resp_ev_tlen);
		else if (sc->hwm_resp_ev_tlen >= 8)
			qwz_log("qwz: dossier: hwmode resp w0=%u w1=%u (w2 absent) (tlen %u; layout unverified; firmware fixed_param order is believed to be pdev_id, status, cfgd_hw_mode_index, num_vdev_mac_entries)\n",
			    sc->hwm_resp_w0, sc->hwm_resp_w1, sc->hwm_resp_ev_tlen);
		else if (sc->hwm_resp_ev_tlen >= 4)
			qwz_log("qwz: dossier: hwmode resp w0=%u (w1, w2 absent) (tlen %u; layout unverified; firmware fixed_param order is believed to be pdev_id, status, cfgd_hw_mode_index, num_vdev_mac_entries)\n",
			    sc->hwm_resp_w0, sc->hwm_resp_ev_tlen);
		else
			qwz_log("qwz: dossier: hwmode resp event TLV empty (tlen %u)\n", sc->hwm_resp_ev_tlen);
	}
	for (i = 0; i < sc->hwm_resp_nent; i++)
		qwz_log("qwz: dossier: hwmode resp entry %u: vdev %u -> pdev %u\n", i,
		    sc->hwm_resp_ent[i].vdev_id, sc->hwm_resp_ent[i].pdev_id);
	/* K2 (H2): the vdev-to-mac mapping from qwz_hwmode_map_note */
	if (sc->hwm_map_have_ev && sc->hwm_map_tlen >= 16)
		qwz_log("qwz: dossier: hwmode map (4-word layout pdev_id, status, cfgd_hw_mode_index, num_vdev_mac_entries; UNVERIFIED, from memory): pdev %u status %u configured index %u declared %u found %u (%u payload bytes after the event TLV)\n",
		    sc->hwm_map_pdev, sc->hwm_map_status, sc->hwm_map_index, sc->hwm_map_declared, (unsigned int)sc->hwm_map_n, sc->hwm_map_trailing);
	else if (sc->hwm_map_have_ev)
		qwz_log("qwz: dossier: hwmode map: short event body %u bytes: w0 %u w1 %u w2 %u (layout unverified) found %u\n",
		    sc->hwm_map_tlen, sc->hwm_map_w[0], sc->hwm_map_w[1], sc->hwm_map_w[2], (unsigned int)sc->hwm_map_n);
	for (i = 0; i < sc->hwm_map_n; i++)
		qwz_log("qwz: dossier: hwmode map: vdev %u -> mac %u\n", sc->hwm_map[i].vdev_id, sc->hwm_map[i].mac_id);
}

/*
 * C29: what THIS module's firmware reports, once per firmware boot (called
 * from qwz_core_start, after WMI service ready). Read-only: nothing here
 * changes state, except that each firmware boot starts a fresh budget of
 * event notes (sc_log_evt), so the T7 evidence is not eaten by earlier
 * toggles and recoveries.
 *
 * The board-data line comes from qwz_core_fetch_bdf and the regulatory line
 * from qwz_reg_chan_list_event (both in qwz.c).
 */
void
qwz_dossier(struct qwz_softc *sc)
{
	static const char hexd[] = "0123456789abcdef";
	char hex[2 * sizeof(sc->wmi.svc_map) + 1];
	uint32_t bands = 0;
	unsigned int i, nradios;

	if (sc == NULL)
		return;
	sc->sc_log_evt = 0;
	nradios = (unsigned int)sc->target_pdev_count;
	if (nradios > MAX_RADIOS)
		nradios = MAX_RADIOS;
	for (i = 0; i < nradios; i++)
		bands |= (uint32_t)sc->target_pdev_ids[i].supported_bands;

	qwz_log("qwz: dossier: fw 0x%x build '%.100s' chip_id 0x%x board_id 0x%x bdf_ext '%.32s' radios %d bands 0x%x chains tx %d rx %d rf_chains %u\n",
	    sc->qmi_target.fw_version, sc->qmi_target.fw_build_id, sc->qmi_target.chip_id, sc->qmi_target.board_id,
	    sc->qmi_target.bdf_ext, sc->num_radios, bands, sc->num_tx_chains, sc->num_rx_chains, sc->target_caps.num_rf_chains);
	qwz_log("qwz: dossier: caps phy 0x%x ht 0x%x vht 0x%x reg 2g %u-%u 5g %u-%u\n",
	    sc->target_caps.phy_capability, sc->target_caps.ht_cap_info, sc->target_caps.vht_cap_info,
	    sc->hal_reg_cap[0].low_2ghz_chan, sc->hal_reg_cap[0].high_2ghz_chan,
	    sc->hal_reg_cap[0].low_5ghz_chan, sc->hal_reg_cap[0].high_5ghz_chan);
	/* the whole service bitmap, byte k = services 8k to 8k+7 (service N is bit N % 8 of byte N / 8) */
	for (i = 0; i < sizeof(sc->wmi.svc_map); i++) {
		hex[2 * i] = hexd[sc->wmi.svc_map[i] >> 4];
		hex[2 * i + 1] = hexd[sc->wmi.svc_map[i] & 15];
	}
	hex[2 * sizeof(sc->wmi.svc_map)] = '\0';
	qwz_log("qwz: dossier: services %s\n", hex);
	qwz_log("qwz: dossier: named TDLS(36)=%d NAN(52)=%d MGMT_TX_WMI(76)=%d 11AX(98)=%d P2P_LISTEN_OFFLOAD(106)=%d OFFCHAN_TX_WMI(123)=%d "
	    "OFFCHAN_DATA_TID(144)=%d FORCE_FW_HANG(24)=%d raw289(11BE)=%d raw205=%d raw212=%d\n",
	    isset(sc->wmi.svc_map, WMI_TLV_SERVICE_TDLS) ? 1 : 0,
	    isset(sc->wmi.svc_map, WMI_TLV_SERVICE_NAN) ? 1 : 0,
	    isset(sc->wmi.svc_map, WMI_TLV_SERVICE_MGMT_TX_WMI) ? 1 : 0,
	    isset(sc->wmi.svc_map, WMI_TLV_SERVICE_11AX) ? 1 : 0,
	    isset(sc->wmi.svc_map, WMI_TLV_SERVICE_P2P_LISTEN_OFFLOAD_SUPPORT) ? 1 : 0,
	    isset(sc->wmi.svc_map, WMI_TLV_SERVICE_OFFCHAN_TX_WMI) ? 1 : 0,
	    isset(sc->wmi.svc_map, WMI_TLV_SERVICE_OFFCHAN_DATA_TID_SUPPORT) ? 1 : 0,
	    isset(sc->wmi.svc_map, WMI_TLV_SERVICE_FORCE_FW_HANG) ? 1 : 0,
	    isset(sc->wmi.svc_map, 289) ? 1 : 0,
	    isset(sc->wmi.svc_map, 205) ? 1 : 0,
	    isset(sc->wmi.svc_map, 212) ? 1 : 0);
	qwz_dossier_hwmode(sc);	/* K1 W2a: hardware-mode facts (log only) */
}
