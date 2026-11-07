/*
 * qwz_life.cpp - the DEAD core (group L, C04 to C07 of docs/m0-spec.md).
 *
 * DEAD means "the firmware must not be touched and every wait about it ends
 * now". It lives in ONE atomic word, sc->sc_life (never in sc_flags: set_bit()
 * and clear_bit() are plain read-modify-write, unsafe against the interrupt
 * action and the sentinel thread). The transient ATH12K_FLAG_CRASH_FLUSH keeps
 * its OpenBSD meaning (qwz_stop disallows new tasks) and is OR-ed with DEAD by
 * QWZ_CRASHED(sc) (qwzvar.h).
 *
 * qwz_declare_dead is callable from any context (interrupt action, sentinel
 * thread, engine, inject): it never waits, never allocates, never takes the
 * ioctl lock and never publishes the ring. It is idempotent (one episode, one
 * engine request) and inert while qwz_dead_armed() is false (debug.qwz.recover=0
 * or debug.qwz.m0=0: the legacy RDDM handling runs, S4 and S16).
 *
 * DEAD is cleared in exactly one place (qwz_init_impl, C06), through
 * qwz_clear_dead. The MMIO block bit of the life word is independent of DEAD
 * and is NOT cleared there (S15): only a passed config-space link check in
 * if_qwz_pci.c clears it (qwz_mmio_unblock, two call sites).
 *
 * Compiled into BOTH kexts: no Airport-only symbols, no Airport headers.
 *
 * Copyright (c) 2026. GPL-2.0-or-later (see LICENSE).
 */
#include "qwz_compat.h"
#include "qwzreg.h"
#include "qwzvar.h"
#include "qwz_awdlx.h"

/* indexed by enum qwz_dead_reason (qwz_life.h) */
const char *const qwz_dead_names[] = { "none", "RDDM", "SYS_ERR", "LINK", "WMI_SILENT", "DEADLINE", "INJECT", "USER" };

void
qwz_declare_dead(struct qwz_softc *sc, uint32_t reason, const char *detail)
{
	uint32_t old;

	if (!qwz_dead_armed()) {		/* recover=0 or m0=0: legacy handling, the old RDDM path runs, DEAD is never declared */
		static unsigned int n_ignored;

		if (qwz_log_ratelimit(&n_ignored, 8, 64))
			qwz_log("qwz: life: would declare DEAD %s (%s): %s, ignored\n", qwz_dead_names[reason < QWZ_DEAD_MAX ? reason : 0],
			    detail ? detail : "-", qwz_recover ? "debug.qwz.m0=0" : "debug.qwz.recover=0");
		return;
	}
	if (!(__atomic_load_n(&sc->sc_life, __ATOMIC_ACQUIRE) & QWZ_LIFE_DEAD))
		sc->sc_dead_reason = reason;	/* before DEAD becomes visible: the engine reads it after seeing DEAD */
	/*
	 * DEAD(LINK) closes the BAR in the same atomic step as DEAD becomes
	 * visible (S17), and also when the episode is already open (an RDDM
	 * first, then the link goes): a vanished device is never read. Only a
	 * passed config-space link check opens it again (S15).
	 */
	old = __atomic_fetch_or(&sc->sc_life, QWZ_LIFE_DEAD | QWZ_LIFE_RECOVERY | (reason == QWZ_DEAD_LINK ? QWZ_LIFE_MMIO_BLOCK : 0u),
	    __ATOMIC_ACQ_REL);
	if (old & QWZ_LIFE_DEAD) {		/* absorbed: the episode is open */
		sc->sc_dead_dups++;
		return;
	}
	sc->sc_dead_episodes++;
	sc->sc_dead_ns = getnsecuptime();
	__atomic_store_n(&sc->sc_awdl_quiesce, 1u, __ATOMIC_RELEASE);	/* AWDL TX stops at once (C22) */
	qwz_log("qwz: life: DEAD #%u: %s (%s): fw_initialized %d ifflags 0x%x peers %d vdevs %d, waiters released, engine asked\n",
	    sc->sc_dead_episodes, qwz_dead_names[reason < QWZ_DEAD_MAX ? reason : 0], detail ? detail : "-",
	    sc->fw_initialized, sc->sc_ic.ic_if.if_flags, sc->num_peers, sc->num_created_vdevs);
	qwz_flight_dump(sc);			/* C26: the last WMI commands and events */
	if (__atomic_load_n(&sc->sc_inject, __ATOMIC_ACQUIRE) & (QWZ_INJ_WMI_STARVE | QWZ_INJ_IGNORE_EV)) {	/* M0.1 (F2): inject 2 or 7 armed; the restart's own WMI handshake must not inherit the starvation, and its events must count */
		__atomic_and_fetch(&sc->sc_inject, ~(QWZ_INJ_WMI_STARVE | QWZ_INJ_IGNORE_EV), __ATOMIC_ACQ_REL);
		qwz_log("qwz: inject: credit-starvation arming cleared at DEAD: the restart must not inherit it\n");
	}
	qwz_wake_all_waiters(sc);
	if (sc->sc_fault_cb != NULL)		/* C18: AirportItlwm::faultCb -> engineRequest, legal from the interrupt action */
		sc->sc_fault_cb(sc->sc_fault_arg, reason);
}

void
qwz_clear_dead(struct qwz_softc *sc, const char *where)
{
	uint32_t old = __atomic_fetch_and(&sc->sc_life, ~QWZ_LIFE_DEAD, __ATOMIC_ACQ_REL);	/* the MMIO block stays: only a passed link check clears it (C08, S15) */

	sc->sc_fw_epoch++;			/* a new firmware starts here */
	sc->sc_silent_count = 0;
	if (old & QWZ_LIFE_DEAD)		/* M2a WP0: this is the START of the bring-up (a refused power-up can follow within milliseconds), not the end of the episode: that is the "engine: recovered ..." line */
		qwz_log("qwz: life: DEAD cleared (%s), DEAD #%u was set for %llu ms; the bring-up begins and the episode stays open until a chip-up succeeds, epoch %u\n", where,
		    sc->sc_dead_episodes, (unsigned long long)((getnsecuptime() - sc->sc_dead_ns) / 1000000ULL), sc->sc_fw_epoch);
}

/*
 * Wakes every channel a firmware wait can sleep on (list re-derived by
 * grepping the first argument of every tsleep_nsec( call in qwz.c,
 * if_qwz_pci.c and ItlQwz.cpp: 25 distinct channels). The delay channels
 * (`that` in ItlQwz.cpp, &sc->sc_awdltest_active) are NOT woken;
 * &psc->rddm_data exists only with QWZ_DEBUG. The transport channels
 * (bhi_ee, mhi_state, bhie_off and every xfer ring's cmd_status) are woken
 * by qwz_pci_wake_all.
 *
 * `wakeup` is qwz_wakeup = IOCommandGate::commandWakeup. HEADER FACT:
 * IOCommandGate.h documents no "gate must be held" requirement for it (unlike
 * commandSleep); in xnu it ends in thread_wakeup_prim (UNVERIFIED: from memory
 * of xnu). A missed wakeup (the waiter had not yet slept) only costs that
 * waiter its own timeout, after which every further wait fails at entry; the
 * sentinel re-issues this call once per tick while DEAD and a chip transition
 * is running (C14), which closes the window.
 */
void
qwz_wake_all_waiters(struct qwz_softc *sc)
{
	int i;

	wakeup(&sc->qmi_resp);		wakeup(&sc->scan.state);	wakeup(&sc->peer_mapped);
	wakeup(&sc->peer_assoc_done);	wakeup(&sc->ctl_resp);		wakeup(&sc->wmi.unified_ready);
	wakeup(&sc->wmi.service_ready);	wakeup(&sc->wmi.hw_mode_ready);	wakeup(&sc->vdev_setup_done);
	wakeup(&sc->vdev_delete_done);	wakeup(&sc->sc_req_mem_ind);	wakeup(&sc->qrtr_server);
	wakeup(&sc->peer_delete_done);	wakeup(&sc->install_key_done);	wakeup(&sc->fwmem_ready);
	wakeup(&sc->fw_ready);		wakeup(sc->dp.rx_tid_retire);	wakeup(&sc->dp.htt_tgt_version_received);
	wakeup(&sc->wmi.tx_credits);
	for (i = 0; i < ATH12K_HTC_EP_COUNT; i++)
		wakeup(&sc->htc.endpoint[i].tx_credits);
	qwz_pci_wake_all(sc);		/* if_qwz_pci.c (C10): bhi_ee, mhi_state, bhie_off, every xfer ring's cmd_status */
}

/*
 * Called at the end of every qwz_core_deinit (C06). Clears what survives a
 * firmware restart today and would otherwise make the next teardown or the
 * next association decide from stale state.
 */
void
qwz_fw_epoch_reset(struct qwz_softc *sc)
{
	sc->sc_vif.is_started = 0;
	sc->sc_vif.is_up = 0;
	sc->num_started_vdevs = 0;		/* qwz_mac_op_start resets these only when it runs */
	sc->num_created_vdevs = 0;
	sc->allocated_vdev_map = 0;
	sc->bss_peer_id = HAL_INVALID_PEERID;
	sc->peer_mapped = sc->peer_delete_done = sc->vdev_setup_done = sc->vdev_delete_done = 0;
	sc->peer_assoc_done = sc->install_key_done = 0;
	sc->qfullmsk = 0;			/* a stale full bit keeps the output queue stopped after a restart */
	sc->sc_tx_timer = 0;
	qwz_awdlx_flush();			/* M2a (WP3, I10): slots the firmware will never complete leave the books as flushed, before the masks below are cleared */
	sc->sc_awdl_inflight = 0;
	sc->sc_awdl_pmask = 0;			/* C24: probe-1 slots of the dead firmware (qwz_vif_purge clears neither mask) */
	sc->sc_awdltest_txmask = 0;		/* the existing test-frame mask */
}

/*
 * The BSS peer keyed by (vdev, address): the peer on arvif->vdev_id whose
 * address equals `bssid` (the BSS node's ni_macaddr, which qwz_mac_station_add
 * used to create it) if there is one, otherwise the only peer on that vdev
 * (qwz_peer_create allows one peer per vdev, so the address preference only
 * disambiguates a state that cannot occur today). `bssid` may be NULL. It
 * never takes the ioctl lock and never sleeps: it is called after qwz_stop,
 * under the gate, possibly while another thread holds the ioctl lock after a
 * timeout (ItlQwz::sleepHardware).
 */
struct ath12k_peer *
qwz_peer_find_bss(struct qwz_softc *sc, struct qwz_vif *arvif, const uint8_t *bssid)
{
	struct ath12k_peer *peer, *any = NULL;

	TAILQ_FOREACH(peer, &sc->peers, entry) {
		if ((uint32_t)peer->vdev_id != arvif->vdev_id)
			continue;
		if (bssid != NULL && IEEE80211_ADDR_EQ(peer->addr, bssid))
			return peer;
		if (any == NULL)
			any = peer;
	}
	return any;
}

/*
 * C07: called after a firmware wait timed out (today only from
 * qwz_wmi_cmd_send; M1 adds more callers). A timeout during which
 * sc_fw_rx_events did not advance, with the main gate healthy (gate_ok within
 * 3 s, C14), counts as SILENT; two within 30 s are the Linux-style
 * silent-firmware verdict. Linux's June 2026 fw-stats thread shows commands
 * can time out against a healthy firmware, hence the event counter and the
 * host-stall guard. By debug.qwz.silent: 0 nothing, 1 (default, shadow) log
 * "would declare DEAD(WMI_SILENT)", 2 declare DEAD.
 */
void
qwz_note_wait_timeout(struct qwz_softc *sc, const char *what, uint32_t ev0)
{
	uint64_t now = getnsecuptime();

	if (qwz_silent == 0 || sc->sc_fw_rx_events != ev0 || !sc->fw_initialized)
		return;
	if (now - sc->sc_gate_ok_ns > SEC_TO_NSEC(3) && sc->sc_gate_ok_ns != 0)
		return;				/* the host was stalled, not the firmware */
	if (now - sc->sc_silent_first_ns > SEC_TO_NSEC(30))
		sc->sc_silent_count = 0;
	if (sc->sc_silent_count++ == 0)
		sc->sc_silent_first_ns = now;
	qwz_log("qwz: silence: %s timed out and the firmware sent nothing meanwhile (%u in a row)\n", what, sc->sc_silent_count);
	if (sc->sc_silent_count >= 2) {
		if (qwz_silent >= 2)
			qwz_declare_dead(sc, QWZ_DEAD_SILENT, what);
		else
			qwz_log("qwz: silence: would declare DEAD(WMI_SILENT): %s (shadow, debug.qwz.silent=2 arms it)\n", what);
	}
}
