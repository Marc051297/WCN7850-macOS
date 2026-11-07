/*
 * qwz_inject.cpp - the fault-injection harness (group I, C33 of docs/m0-spec.md).
 *
 * qwz_inject runs under the HAL gate: debug.qwz.inject -> ItlQwz::injectEntry
 * -> ItlQwz::injectGated. Its argument is debug.qwz.inject_arg. The other two
 * functions are the hooks the power-up preflight (qwz_inject_hold, C09), the
 * MMIO funnel and the config-space link probes (qwz_inject_ones_active, C08,
 * C09, C18) call; with nothing armed each is a test of one word.
 * Compiled into BOTH kexts: no Airport-only symbols, no Airport headers.
 *
 * Copyright (c) 2026. GPL-2.0-or-later (see LICENSE).
 */
#include "qwz_compat.h"
#include "qwzreg.h"
#include "qwzvar.h"

/* qwz.c: the no-op write of modes 2 and 7 (WMI_PDEV_PARAM_ARP_AC_OVERRIDE); no header declares it */
int	qwz_wmi_pdev_set_param(struct qwz_softc *, uint32_t param_id, uint32_t param_value, uint8_t pdev_id);

/*
 * M0.1 (F4): modes 2 and 7 arm the starvation and return at once; the two starved sends (each waits its
 * 3 s credit timeout) run as a task on a private one-thread queue, inside the main gate like every task,
 * so a Wi-Fi toggle typed right after the sysctl overlaps the window. One window at a time (EBUSY). The
 * queue is created on first use and kept for the life of the kext: destroying it from the unregister path
 * would wait for a task that needs the gate that path holds.
 */
static struct taskq *qwz_inject_tq;
static struct task qwz_inject_task;
static volatile uint32_t qwz_inject_busy;
static struct qwz_htc_ep *qwz_inject_ep;		/* the WMI endpoint, taken when the window is armed */
static int qwz_inject_mode, qwz_inject_saved;
static uint32_t qwz_inject_epoch;			/* sc_fw_epoch when armed: a restarted firmware has credits of its own */

static void
qwz_inject_starve_task(void *arg)		/* in the gate, on a thread-call thread: qwz_tsleep_nsec may release the gate */
{
	struct qwz_softc *sc = (struct qwz_softc *)arg;
	struct qwz_htc_ep *ep = qwz_inject_ep;
	uint64_t t0 = getnsecuptime();
	int i, acked = 0;

	/* no send to a firmware that is DEAD, torn down by a toggle meanwhile, or already restarted */
	for (i = 0; i < 2 && !qwz_is_dead(sc) && sc->fw_initialized && sc->sc_fw_epoch == qwz_inject_epoch; i++) {
		if (qwz_wmi_pdev_set_param(sc, WMI_PDEV_PARAM_ARP_AC_OVERRIDE, 0, (uint8_t)sc->pdevs[0].pdev_id) == 0)
			acked++;
	}
	__atomic_and_fetch(&sc->sc_inject, ~(QWZ_INJ_WMI_STARVE | QWZ_INJ_IGNORE_EV), __ATOMIC_ACQ_REL);
	if (!qwz_is_dead(sc) && sc->sc_fw_epoch == qwz_inject_epoch)	/* M0.1 (F2): the credits of a restarted firmware are its own */
		ep->tx_credits = qwz_inject_saved;
	qwz_log("qwz: inject: starvation window over after %llu ms: %d of 2 sends went out, dead %d, firmware epoch %s\n",
	    (unsigned long long)((getnsecuptime() - t0) / 1000000ULL), acked, qwz_is_dead(sc),
	    sc->sc_fw_epoch == qwz_inject_epoch ? "unchanged" : "changed (restarted meanwhile)");
	if (qwz_inject_mode == 7 && acked == 2)
		qwz_log("qwz: inject: type 2: no effect (both writes went out: the firmware answered)\n");
	__atomic_store_n(&qwz_inject_busy, 0u, __ATOMIC_RELEASE);
}

/*
 * M0.1 (HF-4): the softc is freed by ItlQwz::detach (AirportItlwm::stop, a kext unload), and the starvation task above holds
 * raw pointers into it for its whole window: it sleeps up to 2 x 3 s in qwz_wmi_cmd_send with the gate released, so detachGated
 * can run, free the softc and finish while the task is still asleep, and the task then wakes into freed memory. detach calls
 * this first, OUTSIDE the gate (the task needs the gate to finish, and a gate-releasing sleep is refused outside it, hence
 * IOSleep). It returns at once when no window is running, otherwise it waits for the window to end, at most 8 s (a window is 6 s
 * plus gate waits). can_sleep is false on the workloop thread and inside the gate (an IOSleep there would hold the gate the task
 * needs): it then only logs. The caller closes debug.qwz.inject first, so no new window is armed meanwhile.
 */
void
qwz_inject_quiesce(bool can_sleep)
{
	uint64_t t0, end;

	if (!__atomic_load_n(&qwz_inject_busy, __ATOMIC_ACQUIRE))
		return;
	if (!can_sleep) {
		qwz_log("qwz: inject: detach: a starvation window is running and this thread cannot wait for it (workloop thread or inside the gate): detach goes on\n");
		return;
	}
	qwz_log("qwz: inject: detach: a starvation window is running: waiting for it, at most 8 s, before the driver state is freed\n");
	t0 = getnsecuptime();
	end = t0 + SEC_TO_NSEC(8);
	while (__atomic_load_n(&qwz_inject_busy, __ATOMIC_ACQUIRE) && getnsecuptime() < end)
		IOSleep(50);
	qwz_log("qwz: inject: detach: %s after waiting %llu ms\n",
	    __atomic_load_n(&qwz_inject_busy, __ATOMIC_ACQUIRE) ? "the starvation window is STILL running, detach goes on" : "the starvation window is over",
	    (unsigned long long)((getnsecuptime() - t0) / 1000000ULL));
}

/*
 * Modes (docs/m0-spec.md C33). Returns 0 or a positive errno. The modes that
 * start an episode (1, 2, 6, 7) are limited to one per 5 s (EBUSY); the arming
 * modes (0, 3, 4, 5) never are, so inject=4 or inject=5 followed at once by
 * inject=1 works.
 */
int
qwz_inject(struct qwz_softc *sc, int mode, int arg)
{
	static uint64_t last_ns;
	uint64_t now = getnsecuptime();
	int a = arg > 0 ? arg : 0, ret = 0;
	int limited = mode == 1 || mode == 2 || mode == 6 || mode == 7;	/* the arming modes 0, 3, 4, 5 are never limited */

	/*
	 * recover=0 and m0=0 select the legacy handling (section 4.5): DEAD is never
	 * declared and the detectors that would catch these faults are off, so the
	 * modes that rely on them are refused. Mode 6, a REAL firmware assert, stays
	 * available: with the switches off it exercises awdldata8's RDDM path.
	 */
	if ((mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 5 || mode == 7) && !qwz_dead_armed()) {
		qwz_log("qwz: inject: mode %d refused: debug.qwz.recover=0 or debug.qwz.m0=0 (DEAD is never declared, the detectors are off)\n", mode);
		return ENOTSUP;
	}
	/* modes 2, 6 and 7 send a WMI command: nothing to send to without a running firmware */
	if ((mode == 2 || mode == 6 || mode == 7) && !sc->fw_initialized) {
		qwz_log("qwz: inject: mode %d refused: the firmware is not running (fw_initialized 0)\n", mode);
		return ENXIO;
	}
	if ((mode == 2 || mode == 7) && __atomic_load_n(&qwz_inject_busy, __ATOMIC_ACQUIRE)) {	/* M0.1 (F4) */
		qwz_log("qwz: inject: mode %d refused: the starvation window of an earlier injection is still running\n", mode);
		return EBUSY;
	}
	if (limited && now - last_ns < SEC_TO_NSEC(5))
		return EBUSY;
	if (limited)
		last_ns = now;
	qwz_log("qwz: inject: mode %d arg %d\n", mode, arg);
	switch (mode) {
	case 0:
		__atomic_store_n(&sc->sc_inject, 0u, __ATOMIC_RELEASE);
		sc->sc_inject_ones_until_ns = 0;
		sc->sc_inject_powerup_fail = 0;
		return 0;
	case 1:
		qwz_declare_dead(sc, QWZ_DEAD_INJECT, "inject");
		return 0;
	case 2:
	case 7: {
		struct qwz_htc_ep *ep = &sc->htc.endpoint[sc->wmi.wmi[0].eid];

		if (mode == 7) {
			if (!qwz_recovery_proven && !qwz_awdlforce) {
				qwz_log("qwz: inject: mode 7 refused: recovery has not been proven by a real crash in this boot (debug.qwz.inject=6 first)\n");
				return EPERM;
			}
			ret = qwz_wmi_force_fw_hang(sc, 2, 0);
			qwz_log("qwz: inject: type 2 sent: %d\n", ret);
		}
		qwz_inject_saved = ep->tx_credits;		/* restored by the task: the injection must not leave the endpoint starved (taken after the type 2 command, which used a credit of its own) */
		qwz_inject_ep = ep;
		qwz_inject_mode = mode;
		qwz_inject_epoch = sc->sc_fw_epoch;
		__atomic_or_fetch(&sc->sc_inject, QWZ_INJ_WMI_STARVE | QWZ_INJ_IGNORE_EV, __ATOMIC_ACQ_REL);
		if (qwz_inject_tq == NULL)
			qwz_inject_tq = taskq_create("qwzinj", 1, IPL_NET, 0);
		task_set(&qwz_inject_task, qwz_inject_starve_task, sc, "qwzinj");
		__atomic_store_n(&qwz_inject_busy, 1u, __ATOMIC_RELEASE);
		if (qwz_inject_tq == NULL || !task_add(qwz_inject_tq, &qwz_inject_task)) {	/* no queue: the old synchronous behaviour, the sysctl blocks for the window */
			qwz_log("qwz: inject: no task queue for the starvation window: running it in this call\n");
			qwz_inject_starve_task(sc);
		}
		return 0;
	}
	case 3:
		sc->sc_inject_ones_until_ns = getnsecuptime() + MSEC_TO_NSEC(a ? a : 3000);
		return 0;
	case 4:
		sc->sc_inject_powerup_fail = a ? (uint32_t)a : 1u;
		return 0;
	case 5:
		sc->sc_inject_hold_ms = a ? (uint32_t)a : 20000u;
		__atomic_or_fetch(&sc->sc_inject, QWZ_INJ_HOLD, __ATOMIC_ACQ_REL);
		return 0;
	case 6:
		if (!isset(sc->wmi.svc_map, WMI_TLV_SERVICE_FORCE_FW_HANG) && !qwz_awdlforce) {
			qwz_log("qwz: inject: FORCE_FW_HANG service not advertised: not sent\n");
			return ENOTSUP;
		}
		ret = qwz_wmi_force_fw_hang(sc, ATH12K_WMI_FW_HANG_ASSERT_TYPE, ATH12K_WMI_FW_HANG_DELAY);
		qwz_log("qwz: inject: WMI_FORCE_FW_HANG type 1 sent: %d\n", ret);
		return ret;
	}
	return EINVAL;
}

/*
 * Called first thing in qwz_pci_power_up, before the preflight (inject=4, the
 * link check, the MMIO unblock) and before the chip reset. At that point
 * psc->sc_powered_down and the MMIO block are still set from the last
 * power-down, so the interrupt actions that run while the gate is released
 * return at once and nothing reads the BAR of the reset chip. inject=5 holds
 * the power-up for inject_arg ms with the gate RELEASED (a delay, not a
 * firmware wait: DEAD does not wake it, the loop tests DEAD every 100 ms), so
 * the chip-transition deadline of the sentinel can be observed against a
 * stuck step.
 */
int
qwz_inject_hold(struct qwz_softc *sc, const char *where)
{
	uint32_t ms = sc->sc_inject_hold_ms;
	uint64_t t0, end;

	if (!(sc->sc_inject & QWZ_INJ_HOLD))
		return 0;
	__atomic_and_fetch(&sc->sc_inject, ~QWZ_INJ_HOLD, __ATOMIC_ACQ_REL);
	qwz_log("qwz: inject: holding %s for %u ms (the gate is released; DEAD ends it)\n", where, ms);
	t0 = getnsecuptime();
	end = t0 + MSEC_TO_NSEC(ms);
	while (getnsecuptime() < end && !qwz_is_dead(sc)) {
		if (tsleep_delay_nsec(&sc->sc_inject, 0, "qwzinjh", MSEC_TO_NSEC(100)) == EDEADLK) {	/* this thread cannot sleep (workloop thread): never spin holding the gate */
			qwz_log("qwz: inject: hold of %s refused: this thread cannot sleep: not held\n", where);
			return 0;
		}
	}
	qwz_log("qwz: inject: hold of %s ended after %u ms (dead %d)\n", where,
	    (unsigned int)((getnsecuptime() - t0) / 1000000ULL), qwz_is_dead(sc));
	return 0;
}

/*
 * inject=3: while the window is open BAR reads answer 0xffffffff and the
 * config-space link probes read all-ones (qwz_pci_read/write, qwz_pci_link_id,
 * the sentinel's link probe). Called from interrupt, gate and timer contexts:
 * cheap, never sleeps, never logs, never touches the BAR. The first caller
 * after the window closed clears it.
 */
int
qwz_inject_ones_active(struct qwz_softc *sc)
{
	uint64_t until = sc->sc_inject_ones_until_ns;

	if (until == 0)
		return 0;
	if (getnsecuptime() < until)
		return 1;
	sc->sc_inject_ones_until_ns = 0;
	return 0;
}
