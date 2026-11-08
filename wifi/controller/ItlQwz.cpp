/*
 * ItlQwz - itlwm HAL for the Qualcomm WCN7850, driving OpenBSD's qwz(4).
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#include "ItlQwz.hpp"

/* IOKit and net80211 come first: qwz_compat.h then defines malloc/free/... macros. */
#include "qwz_compat.h"
#include "qwzreg.h"
#include "qwzvar.h"
#include "qwz_sysctl.h"
#include "qwz_mon.h"
#undef malloc
#undef free
#undef wakeup

#define QWZLOG(fmt, ...)	qwz_log("qwz: " fmt "\n", ##__VA_ARGS__)

/* if_qwz_pci.c */
struct qwz_softc *qwz_pci_macos_alloc(void);
int	qwz_pci_macos_attach(struct qwz_softc *, struct pci_attach_args *);
void	qwz_pci_macos_detach(struct qwz_softc *);
void	qwz_pci_macos_free(struct qwz_softc *);
void	qwz_pci_macos_quiesced_detach(struct qwz_softc *);
/* qwz.c */
int	qwz_init(struct _ifnet *);

#define super ItlHalService
OSDefineMetaClassAndStructors(ItlQwz, ItlHalService)

/*
 * The controller calls if_start from the network stack's output thread and
 * if_watchdog from its own timer, neither holding the gate. qwz expects the
 * kernel lock there, so both are wrapped to run gated.
 */
static IOCommandGate *qwz_hal_gate;
static void (*qwz_if_start)(struct _ifnet *);
static void (*qwz_if_watchdog)(struct _ifnet *);
/*
 * M0 (C18, S20): attemptAction has never been used on this gate (Apple's IO80211WorkQueue). The
 * sentinel's probe (ItlQwz::healthSnapshot) finds out; until its first success (1), and for good
 * if none comes in 30 probes (-1), the legacy watchdog keeps its blocking runAction (C16).
 */
static int qwz_gate_try;	/* 0 unknown, 1 attemptAction has succeeded on the main gate, -1 it never succeeded in 30 probes */
static unsigned int qwz_wd_skipped;	/* C16: legacy watchdog / data-stall ticks skipped because the gate was busy (health line: wdskip) */

/* WP4 (M2a): the same with AirportItlwm's handle for the frame; qwz_awdl_data_tx_ex is qwz_awdl_data_tx while the pool is off. */
struct AwdlTxEx {
	const uint8_t *frame;
	size_t len;
	uint32_t cookie;
};

int
ItlQwz::awdlDataTxEx(const uint8_t *frame, size_t len, uint32_t cookie)
{
	struct AwdlTxEx a = { frame, len, cookie };
	int ret = ENXIO;

	if (!attached || shutdown || sc == NULL || wakeNeeded || qwz_hal_gate == NULL)
		return ENXIO;
	qwz_hal_gate->runAction(awdlDataTxExGated, this, (void *)&a, &ret);
	return ret;
}

IOReturn
ItlQwz::awdlDataTxExGated(OSObject *, void *arg0, void *arg1, void *arg2, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;
	struct AwdlTxEx *a = (struct AwdlTxEx *)arg1;

	if (that->sc == NULL || !that->attached) {	/* fix2 (memory-R2-2): the caller tested this before it blocked behind the gate; detachGated may have freed the softc meanwhile */
		*(int *)arg2 = ENXIO;
		return kIOReturnSuccess;
	}
	*(int *)arg2 = qwz_awdl_data_tx_ex(that->sc, a->frame, a->len, a->cookie);
	return kIOReturnSuccess;
}

/* The completion callback of the lane M pool (the workloop thread, the gate held); the argument first, as setAwdlDataRx. */
void
ItlQwz::setAwdlTxDone(qwz_awdl_txdone_fn fn, void *arg)
{
	if (sc == NULL)
		return;
	sc->sc_awdl_pool.done_arg = arg;
	__atomic_store_n(&sc->sc_awdl_pool.done_fn, fn, __ATOMIC_RELEASE);
}

/* The lane's state for AirportItlwm's pacing and gate: plain loads, any thread. */
void
ItlQwz::awdlLane(struct qwz_awdl_lane *li)
{
	struct qwz_softc *s = __atomic_load_n(&sc, __ATOMIC_ACQUIRE);	/* fix1 (memory MEM-2): one read of the pointer for the test and the use, as detachGated clears it after it has freed the softc. The window between the test and the reads in qwz_awdl_lane_info (a driver stop without shutdownHardware, with awdl0's output thread inside this function) is not closed here: that needs the gate or a reader count in detachGated (M2b) */

	if (!attached || shutdown || s == NULL || wakeNeeded) {
		memset(li, 0, sizeof(*li));
		li->state = QWZ_AWDL_LANE_DOWN;
		return;
	}
	qwz_awdl_lane_info(s, li);
}

static IOReturn
qwz_start_gated(OSObject *, void *ifp, void *, void *, void *)
{
	qwz_if_start((struct _ifnet *)ifp);
	return kIOReturnSuccess;
}

static void
qwz_if_start_wrapper(struct _ifnet *ifp)
{
	qwz_hal_gate->runAction(qwz_start_gated, ifp);
}

static IOReturn
qwz_watchdog_gated(OSObject *, void *ifp, void *, void *, void *)
{
	qwz_if_watchdog((struct _ifnet *)ifp);
	return kIOReturnSuccess;
}

static void
qwz_if_watchdog_wrapper(struct _ifnet *ifp)
{
	/* M0 (C16): never block behind the gate this tick is meant to watch, but only once attemptAction has proven itself (S20) */
	if (qwz_m0_on && __atomic_load_n(&qwz_gate_try, __ATOMIC_RELAXED) > 0) {
		IOReturn r = qwz_hal_gate->attemptAction(qwz_watchdog_gated, ifp);

		if (r == kIOReturnCannotLock) {
			qwz_wd_skipped++;			/* the gate is busy: this tick is skipped, the sentinel names a busy gate */
			return;
		}
		if (r == kIOReturnSuccess)
			return;
	}
	qwz_hal_gate->runAction(qwz_watchdog_gated, ifp);	/* the legacy blocking call: attemptAction not proven usable yet, or any other result */
}

bool
ItlQwz::initWithController(IOEthernetController *controller, IOWorkLoop *workloop, IOCommandGate *commandGate)
{
	if (!super::initWithController(controller, workloop, commandGate))
		return false;
	qwz_compat_init();
	engineStateName = "UP";
	if (sc == NULL)
		sc = qwz_pci_macos_alloc();
	if (sc != NULL)
		qwz_compat_set_life(&sc->sc_life);
	return sc != NULL;
}

bool
ItlQwz::qwz_match(IOPCIDevice *device)
{
	return device->configRead16(kIOPCIConfigVendorID) == PCI_VENDOR_QUALCOMM &&
	    device->configRead16(kIOPCIConfigDeviceID) == PCI_PRODUCT_QUALCOMM_WCN7850;
}

IOReturn
/*
 * IOCommandGate::runAction() passes the gate's OWNER (the itlwm controller)
 * as the first argument, not the HAL. The HAL is passed explicitly in arg1.
 * Casting owner to ItlQwz wrote sc into the controller object and left the
 * HAL's own sc NULL: the attach "succeeded" and the next line dereferenced
 * NULL, a panic that looks like a hard freeze with the NVIDIA driver.
 */
ItlQwz::attachGated(OSObject *, void *arg0, void *arg1, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg1;
	struct pci_attach_args *pa = (struct pci_attach_args *)arg0;
	int err;

	if (!qwz_stage_ok(3, "inside the qwz attach, holding the gate"))
		return kIOReturnError;

	if (that->sc == NULL && (that->sc = qwz_pci_macos_alloc()) == NULL)
		return kIOReturnNoMemory;
	qwz_compat_set_life(&that->sc->sc_life);	/* M0 (C01): the early return above guarantees that->sc != NULL */
	err = qwz_pci_macos_attach(that->sc, pa);
	if (err == QWZ_EPARKED) {
		/* Parked at a stage >= 15: keep sc and the running firmware. */
		QWZLOG("parked: the driver stays idle with the firmware running");
		return kIOReturnError;
	}
	if (err) {
		QWZLOG("attach failed: %d", err);
		qwz_compat_set_life(NULL);				/* before the softc is freed (C01) */
		qwz_sysctl_set_mon(NULL);				/* fix F7: qwz_mon_attach may have run; close the cell's nodes before the softc is freed */
		qwz_mon_detach(that->sc);				/* fix F7: NULL-safe and idempotent: clears qwz_mon_sc when it is this softc, frees the buffers and the maps */
		qwz_pci_macos_detach(that->sc);
		/* keep get80211Controller() valid: a fresh, empty softc */
		that->sc = qwz_pci_macos_alloc();
		if (that->sc != NULL)				/* the allocation can fail: never form &NULL->sc_life */
			qwz_compat_set_life(&that->sc->sc_life);
		return kIOReturnError;
	}
	that->attached = true;
	qwz_sysctl_set_awdltest(awdlTestEntry, that);
	qwz_sysctl_set_inject(injectEntry, that);	/* M0 (C32, C33) */
	qwz_sysctl_set_mon(that);			/* merge glue (F): the capture cell's nodes are open while an attach stands */
	return kIOReturnSuccess;
}

int
ItlQwz::awdlTestEntry(void *arg, int test)
{
	ItlQwz *that = (ItlQwz *)arg;
	IOWorkLoop *wl;
	int found = -ENXIO;

	if (!that->attached || that->shutdown || that->sc == NULL ||
	    that->wakeNeeded || qwz_hal_gate == NULL)
		return -ENXIO;	/* no chip, or it is powered down */
	wl = qwz_hal_gate->getWorkLoop();
	if (wl != NULL && wl->onThread())
		return -EBUSY;
	qwz_hal_gate->runAction(awdlTestGated, that, (void *)(intptr_t)test, &found);
	return found;
}

IOReturn
ItlQwz::awdlTestGated(OSObject *, void *arg0, void *arg1, void *arg2, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* runAction's owner is the controller */

	*(int *)arg2 = qwz_awdl_listen_test(that->sc, (int)(intptr_t)arg1);
	return kIOReturnSuccess;
}

/*
 * M0 (C33): debug.qwz.inject=<mode>. Mirrors awdlTestEntry: refuses when no
 * chip is attached or it is powered down, and on the workloop thread (the
 * harness sleeps); otherwise qwz_inject runs under the gate. The entry's own
 * refusals are negative errnos like awdlTestEntry's; qwz_inject returns a
 * positive errno or 0 (the sysctl handler normalises the sign).
 */
int
ItlQwz::injectEntry(void *arg, int mode, int a)
{
	ItlQwz *that = (ItlQwz *)arg;
	IOWorkLoop *wl;
	int found = -ENXIO;

	if (that == NULL)	/* the sysctl layer reads the callback and its argument in two steps */
		return -ENXIO;
	if (!that->attached || that->shutdown || that->sc == NULL ||
	    that->wakeNeeded || qwz_hal_gate == NULL)
		return -ENXIO;	/* no chip, or it is powered down */
	wl = qwz_hal_gate->getWorkLoop();
	if (wl != NULL && wl->onThread())
		return -EBUSY;
	qwz_hal_gate->runAction(injectGated, that, (void *)(intptr_t)mode, (void *)(intptr_t)a, &found);
	return found;
}

IOReturn
ItlQwz::injectGated(OSObject *, void *arg0, void *arg1, void *arg2, void *arg3)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* the HAL is passed explicitly: runAction's owner is the controller */
	int mode = (int)(intptr_t)arg1;

	/*
	 * Re-tested under the gate: injectEntry's tests ran before runAction waited for it, and a thread that
	 * holds the gate releases it in every firmware wait, so a detach, a shutdown or a power-down can finish,
	 * or be half done, in between. wakeNeeded is set only AFTER sleepHardware's power-down and fw_initialized
	 * is cleared only at the end of qwz_core_deinit, so neither marks a teardown in flight; the ioctl lock does
	 * (it is held for the whole of qwz_init, qwz_stop, sleepHardware and shutdownGated, and every gate release
	 * of those happens inside it: the same test as qwz_state_work). sc_awdl_quiesce cannot be used: C17 leaves
	 * it set after every teardown.
	 */
	if (!that->attached || that->shutdown || that->sc == NULL || that->wakeNeeded) {
		qwz_log("qwz: inject: mode %d refused: no chip, or it was powered down while this call waited for the gate\n", mode);
		*(int *)arg3 = -ENXIO;
		return kIOReturnSuccess;
	}
	if (that->sc->ioctl_rwl.rwl_owner != NULL) {
		qwz_log("qwz: inject: mode %d refused: an init, stop or power transition holds the ioctl lock; try again when it has finished\n", mode);
		*(int *)arg3 = -EBUSY;
		return kIOReturnSuccess;
	}
	*(int *)arg3 = qwz_inject(that->sc, mode, (int)(intptr_t)arg2);
	return kIOReturnSuccess;
}

void
ItlQwz::setAwdlRx(void (*fn)(void *, const uint8_t *, size_t, int, uint32_t, uint32_t, uint32_t), void *arg)
{
	if (sc == NULL)
		return;
	/* The argument first: the RX path reads fn, then arg. */
	sc->sc_awdl_rx_arg = arg;
	__atomic_store_n(&sc->sc_awdl_rx, fn, __ATOMIC_RELEASE);
}

int
ItlQwz::awdlTx(const uint8_t *frame, size_t len)
{
	int ret = ENXIO;

	if (!attached || shutdown || sc == NULL || wakeNeeded || qwz_hal_gate == NULL)
		return ENXIO;
	/* From the workloop's own thread (AirportItlwm's TX timer) runAction calls through. */
	qwz_hal_gate->runAction(awdlTxGated, this, (void *)frame, (void *)len, &ret);
	return ret;
}

IOReturn
ItlQwz::awdlTxGated(OSObject *, void *arg0, void *arg1, void *arg2, void *arg3)
{
	ItlQwz *that = (ItlQwz *)arg0;

	if (that->sc == NULL || !that->attached) {	/* fix2 (memory-R2-2): as awdlDataTxExGated */
		*(int *)arg3 = ENXIO;
		return kIOReturnSuccess;
	}
	*(int *)arg3 = qwz_awdl_tx(that->sc, (const uint8_t *)arg1, (size_t)arg2);
	return kIOReturnSuccess;
}

void
ItlQwz::setAwdlDataRx(void (*fn)(void *, const uint8_t *, size_t, int), void *arg)
{
	if (sc == NULL)
		return;
	sc->sc_awdl_data_rx_arg = arg;
	__atomic_store_n(&sc->sc_awdl_data_rx, fn, __ATOMIC_RELEASE);
}

int
ItlQwz::awdlPeerAdd(const uint8_t *mac)
{
	int ret = ENXIO;

	if (!attached || shutdown || sc == NULL || wakeNeeded || qwz_hal_gate == NULL)
		return ENXIO;
	qwz_hal_gate->runAction(awdlPeerAddGated, this, (void *)mac, &ret);
	return ret;
}

IOReturn
ItlQwz::awdlPeerAddGated(OSObject *, void *arg0, void *arg1, void *arg2, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;

	if (that->sc == NULL || !that->attached) {	/* fix2 (memory-R2-2): as awdlDataTxExGated */
		*(int *)arg2 = ENXIO;
		return kIOReturnSuccess;
	}
	*(int *)arg2 = qwz_awdl_peer_add(that->sc, (const uint8_t *)arg1);
	return kIOReturnSuccess;
}

void
ItlQwz::setAwdlAddress(const uint8_t *mac)
{
	if (sc == NULL || mac == NULL)
		return;
	/* The bytes first, then the flag: qwz_awdltest_vdev_create reads the flag and then the bytes. */
	memcpy(sc->sc_awdl_addr, mac, IEEE80211_ADDR_LEN);
	__atomic_store_n(&sc->sc_awdl_addr_valid, 1, __ATOMIC_RELEASE);
}

uint32_t
ItlQwz::awdlPeerGeneration()
{
	return sc != NULL ? __atomic_load_n(&sc->sc_awdl_peer_gen, __ATOMIC_RELAXED) : 0;
}

int
ItlQwz::awdlDataTx(const uint8_t *frame, size_t len)
{
	int ret = ENXIO;

	if (!attached || shutdown || sc == NULL || wakeNeeded || qwz_hal_gate == NULL)
		return ENXIO;
	qwz_hal_gate->runAction(awdlDataTxGated, this, (void *)frame, (void *)len, &ret);
	return ret;
}

IOReturn
ItlQwz::awdlDataTxGated(OSObject *, void *arg0, void *arg1, void *arg2, void *arg3)
{
	ItlQwz *that = (ItlQwz *)arg0;

	if (that->sc == NULL || !that->attached) {	/* fix2 (memory-R2-2): as awdlDataTxExGated; M0.1 dereferenced the NULL softc here */
		*(int *)arg3 = ENXIO;
		return kIOReturnSuccess;
	}
	*(int *)arg3 = qwz_awdl_data_tx(that->sc, (const uint8_t *)arg1, (size_t)arg2);
	return kIOReturnSuccess;
}

bool
ItlQwz::attach(IOPCIDevice *device)
{
	struct pci_attach_args pa;
	struct _ifnet *ifp;
	IOReturn ret;

	pciDev = device;
	QWZLOG("QwzMac %s attaching to %04x:%04x subsystem %04x:%04x", QWZMAC_VERSION,
	    device->configRead16(kIOPCIConfigVendorID), device->configRead16(kIOPCIConfigDeviceID),
	    device->configRead16(kIOPCIConfigSubSystemVendorID),
	    device->configRead16(kIOPCIConfigSubSystemID));

#ifdef QWZ_DEBUG
	/* qwz_debug=<mask of QWZ_D_* from qwzvar.h> sets the trace level. */
	PE_parse_boot_argn("qwz_debug", &qwz_debug, sizeof(qwz_debug));
	QWZLOG("debug build, qwz_debug=0x%x", qwz_debug);
#endif
	if (qwz_stage_parked()) {
		QWZLOG("parked by an earlier attempt; reboot before starting again");
		return false;
	}
	qwz_compat_init();
	qwz_stage_reset();	/* each attempt starts with a clean limit flag */
	qwz_hal_gate = getMainCommandGate();
	qwz_compat_set_gate(qwz_hal_gate);
	qwz_compat_set_controller(getController());
	if (!qwz_stage_ok(1, "driver setup: task queue thread (taskq_init)"))
		return false;
	static bool taskq_ready;
	if (!taskq_ready) {	/* a retried bring-up must not start systq twice */
		taskq_init();
		taskq_ready = true;
	}

	bzero(&pa, sizeof(pa));
	pa.pa_tag = device;
	pa.pa_id = device->configRead16(kIOPCIConfigVendorID) |
	    ((pcireg_t)device->configRead16(kIOPCIConfigDeviceID) << 16);
	pa.pa_workloop = getMainWorkLoop();
	pa.pa_owner = this;
	pa.pa_controller = getController();

	/*
	 * Attach runs on the IOService start thread holding the gate, so the
	 * firmware boot can tsleep() while the workloop delivers interrupts.
	 */
	if (!qwz_stage_ok(2, "take the command gate (runAction)"))
		return false;
	ret = qwz_hal_gate->runAction(attachGated, &pa, this);
	if (ret != kIOReturnSuccess)
		return false;

	ifp = &sc->sc_ic.ic_if;
	qwz_if_start = ifp->if_start;
	qwz_if_watchdog = ifp->if_watchdog;
	ifp->if_start = qwz_if_start_wrapper;
	ifp->if_watchdog = qwz_if_watchdog_wrapper;

	snprintf(fwVersion, sizeof(fwVersion), "%s", sc->qmi_target.fw_build_id);
	QWZLOG("attached: MAC %s, firmware %s", ether_sprintf(sc->sc_ic.ic_myaddr), fwVersion);
	QWZLOG("M0: %s %s recover=%d silent=%d m0=%d deadaware=%d mmioguard=%d bme=%d softassert=%d awdlprobe=%d capsae=%d m0.1=late-reset,async-inject,dma-hold latesettle=%d (see debug.qwz.*)",
	    ITLWM_VERSION, GIT_COMMIT, qwz_recover, qwz_silent, qwz_m0_on, qwz_dead_aware, qwz_mmio_guard, qwz_bme,
	    qwz_softassert, qwz_awdlprobe, qwz_capsae, qwz_late_settle);
	QWZLOG("S1: cap73=%d (debug.qwz.cap73; capability bit 73, byte 9 mask 0x02)", qwz_cap73);
	QWZLOG("K1: monnocred=%d monqmax=%u monparams=%d scanraw=%d hwmode=%d (monqmax=inflight-data-cap: K2b semantics, counts user data and control frames in flight, never management; debug.qwz.monnocred, monqmax, monparams, scanraw, hwmode; W1 queue control, W3 cell rate and retry, W2c scan request raw words, K2 hardware mode id asked of the firmware (also boot-arg qwzhwmode=N); W2a/W2b/W2c evidence logging is always on)", qwz_monnocred, qwz_monqmax, qwz_monparams, qwz_scanraw, qwz_hwmode);
	/* M0 (C12): one PCI config snapshot now (device alive, config reads only), so the first wake never starts from none */
	saveConfig();
	qwz_log("qwz: pm: attach-time PCI snapshot taken (valid %d) id %08x cmd %04x\n", saved.valid, saved.id, saved.cmd);
	return true;
}

IOReturn
ItlQwz::detachGated(OSObject *, void *arg0, void *, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* runAction's owner is the controller */

	QWZLOG("detach: attached %d shutdown %d", that->attached, that->shutdown);
	qwz_sysctl_set_awdltest(NULL, NULL);
	qwz_sysctl_set_inject(NULL, NULL);
	qwz_sysctl_set_mon(NULL);			/* merge glue (F): debug.qwz.mon, montx, montxframe, monpeer, monmac and moncap answer ENXIO from here on */
	if (that->sc != NULL)
		qwz_mon_detach(that->sc);		/* merge glue (F): host-only end of a cell, then the capture buffers, the lock and the 64 maps go before qwz_detach frees the softc */
	if (that->sc != NULL) {
		/* R1-05: in the attached, not shut down case qwz_pci_macos_detach clears it itself, directly before it frees the softc, so DEAD (declared by qwz_pci_detach for a stuck lock) stays visible to the dead-aware sleeps of its teardown and of the woken holder */
		if (!that->attached || that->shutdown)
			qwz_compat_set_life(NULL);			/* M0 (C01): before the softc is freed; the free calls below follow */
		if (that->attached && that->shutdown)
			qwz_pci_macos_quiesced_detach(that->sc);	/* chip is off already */
		else if (that->attached)
			qwz_pci_macos_detach(that->sc);
		else
			qwz_pci_macos_free(that->sc);	/* never attached */
		that->sc = NULL;
	}
	that->attached = false;
	that->dirtyTeardown = false;
	that->sleepBaseValid = false;
	that->busResetNext = false;
	bzero(&that->stall, sizeof(that->stall));
	return kIOReturnSuccess;
}

/* qwz.c */
void	qwz_core_deinit(struct qwz_softc *);

IOReturn
ItlQwz::shutdownGated(OSObject *, void *arg0, void *, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;
	struct _ifnet *ifp;

	if (!that->attached || that->shutdown)
		return kIOReturnSuccess;
	qwz_sysctl_set_awdltest(NULL, NULL);
	qwz_sysctl_set_inject(NULL, NULL);
	ifp = &that->sc->sc_ic.ic_if;
	qwz_logc("qwz: shutdown: stopping the interface\n");
	/* AWDL stops at the START of the teardown (C17), also when the lock wait below is long */
	__atomic_store_n(&that->sc->sc_awdl_quiesce, 1u, __ATOMIC_RELEASE);
	/* C02: shutdown must never hang on a stuck holder: stop without the lock, then */
	int locked = rw_enter_write(&that->sc->ioctl_rwl) == 0;

	if (!locked) {
		/*
		 * R1-05: a holder that kept the lock for 20 s is stuck in a firmware wait, and a teardown beside it would be a full
		 * firmware conversation. Declare DEAD as sleepHardware does: the holder's waits end and this teardown is host-only.
		 * `shutdown` goes up FIRST: DEAD asks the engine to restart the firmware (sc_fault_cb) and the sentinel polls
		 * faultPending() every second; this is not a chipClaim and wakeAllowed() does not exclude a system shutdown
		 * (pmStopping is set only by stop(), sleepPending only by a power-off request), so without the flag the engine
		 * could tear down or restart the chip beside this thread. With it faultPending() reads false and every HAL entry
		 * point reads "chip off". The woken holder cannot run while this thread keeps the gate, so wait for the lock again
		 * (rw_enter_write_grace releases the gate): the holder unwinds and the stop below runs with the lock.
		 * With recover=0 or m0=0 DEAD is inert, there is no grace wait, and this is the C02 path (log, proceed).
		 */
		that->shutdown = true;
		qwz_declare_dead(that->sc, QWZ_DEAD_DEADLINE, "ioctl lock");
		if (qwz_dead_armed())
			locked = rw_enter_write_grace(&that->sc->ioctl_rwl, QWZ_RWL_GRACE_MS) == 0;
		if (locked)
			qwz_logc("qwz: shutdown: the ioctl lock was released after DEAD: stopping with it\n");
		else
			qwz_logc("qwz: shutdown: the ioctl lock stayed busy: stopping without it\n");
	}
	ifp->if_flags &= ~IFF_UP;
	if (ifp->if_flags & IFF_RUNNING)
		qwz_stop(ifp);
	qwz_logc("qwz: shutdown: interface stopped; firmware stop + power-down\n");
	if (that->sc->fw_initialized)
		qwz_core_deinit(that->sc);	/* firmware stop + chip power-down */
	if (locked)
		rw_exit(&that->sc->ioctl_rwl);
	that->shutdown = true;
	qwz_logc("qwz: chip powered down for system shutdown\n");
	return kIOReturnSuccess;
}

/*
 * System sleep/wake, modelled on ath12k_core_suspend/_resume and the Linux
 * PCI core's link recovery (pcie_retrain_link, pci_bridge_secondary_bus_reset).
 * ErP cuts the M.2 module's power in S3, so the firmware is gone on wake and
 * the config space may come back blank or the link down. All of this runs
 * holding the command gate (called from the power-state thread call).
 */
#define QWZ_PM_PCIE_LNKCAP	0x0c
#define QWZ_PM_PCIE_LNKCAP_DLLLARC	(1u << 20)
#define QWZ_PM_PCIE_LNKCTL	0x10
#define QWZ_PM_PCIE_LNKCTL_RL	(1u << 5)
#define QWZ_PM_PCIE_LNKSTA	0x12
#define QWZ_PM_PCIE_LNKSTA_LT	(1u << 11)
#define QWZ_PM_PCIE_LNKSTA_DLLLA	(1u << 13)
#define QWZ_PM_BRIDGE_CTL	0x3e
#define QWZ_PM_BRIDGE_CTL_SBR	(1u << 6)

static uint64_t
qwz_pm_ms(void)
{
	return getnsecuptime() / 1000000ULL;
}

IOPCIDevice *
ItlQwz::parentBridge(IOService **bus)
{
	IOService *b, *p;

	*bus = NULL;
	if (pciDev == NULL || (b = pciDev->getProvider()) == NULL ||
	    (p = b->getProvider()) == NULL)
		return NULL;
	*bus = b;
	return OSDynamicCast(IOPCIDevice, p);
}

/* True when the bridge's secondary bus holds this device and nothing else. */
bool
ItlQwz::soleDeviceBehind(IOService *bus)
{
	OSIterator *it;
	OSObject *o;
	int n = 0;
	bool only = true;

	if (bus == NULL || (it = bus->getChildIterator(gIOServicePlane)) == NULL)
		return false;
	while ((o = it->getNextObject()) != NULL) {
		n++;
		if (o != pciDev)
			only = false;
	}
	it->release();
	return only && n == 1;
}

void
ItlQwz::saveConfig()
{
	UInt8 off = 0;
	uint32_t id;

	if (pciDev == NULL)
		return;
	/* M0 (C12): read the id BEFORE the snapshot is zeroed, so an all-ones id cannot destroy a good one */
	id = pciDev->configRead32(0);
	if (id == 0xffffffffu) {
		qwz_log("qwz: pm: save config: id reads ffffffff: keeping the earlier snapshot (valid %d)\n", saved.valid);
		if (qwz_bme)
			saved.cmd &= ~kIOPCICommandBusMaster;	/* the chip may still run its old firmware: no bus mastering until the next power-up has reset it */
		return;		/* nothing sane to save */
	}
	bzero(&saved, sizeof(saved));
	saved.id = id;
	/*
	 * M0 (C12): every snapshot is normalised. INTx stays disabled: after a
	 * secondary bus reset the command register is at its default (INTx
	 * enabled) and qwz_pci_msi_config sets INTx-disable only inside power_up,
	 * after the chip reset (the window that froze the machine); wakeHardware
	 * ORs MEM and BM back in.
	 */
	saved.cmd = pciDev->configRead16(kIOPCIConfigCommand) | kIOPCICommandInterruptDisable;
	saved.cls = pciDev->configRead32(0x0c);
	saved.bar[0] = pciDev->configRead32(kIOPCIConfigBaseAddress0);
	saved.bar[1] = pciDev->configRead32(kIOPCIConfigBaseAddress0 + 4);
	if (pciDev->findPCICapability(kIOPCIMSICapability, &off)) {
		saved.msiOff = off;
		saved.msiCtl = pciDev->configRead16(off + 2);
		saved.msiLo = pciDev->configRead32(off + 4);
		if (saved.msiCtl & 0x80) {
			saved.msiHi = pciDev->configRead32(off + 8);
			saved.msiData = pciDev->configRead16(off + 12);
		} else
			saved.msiData = pciDev->configRead16(off + 8);
	}
	off = 0;
	if (pciDev->findPCICapability(kIOPCIPCIExpressCapability, &off)) {
		saved.expOff = off;
		saved.devCtl = pciDev->configRead16(off + 8);
	}
	off = 0;
	if (pciDev->findPCICapability(kIOPCIPowerManagementCapability, &off))
		saved.pmOff = off;
	saved.msiCtl &= ~0x0001;	/* M0 (C12): MSI stays off until qwz_pci_power_up enables it */
	/* M0 (R1-02): a dead or no-link teardown cleared bus mastering, or tried to (a config write on a flapping link can be lost): the snapshot says off whatever the register reads now */
	if (qwz_bme && sc != NULL && qwz_is_dead(sc))
		saved.cmd &= ~kIOPCICommandBusMaster;
	saved.valid = true;
}

/*
 * In-flight state work: every queued or running net80211 state, set-key or
 * block-ack task holds a task_refs reference (the base reference is 1), the
 * init task is only queued, and a thread inside qwz_init/qwz_stop owns the
 * ioctl lock.
 */
static int
qwz_state_work(struct qwz_softc *sc, int *tasks, int *initq, int *lock)
{
	*tasks = sc->task_refs.r_refs > 1 ? sc->task_refs.r_refs - 1 : 0;
	*initq = task_pending(&sc->init_task) != 0;
	*lock = sc->ioctl_rwl.rwl_owner != NULL;
	return *tasks != 0 || *initq || *lock;
}

#define QWZ_QUIESCE_POLL_MS	10

/*
 * Sleep ordering. A leave-network request from airportd runs a RUN -> SCAN
 * change in the state task: vdev down, vdev stop, peer delete, REO queue
 * retirement, each a WMI round trip with the gate released. qwz_stop() then
 * set CRASH_FLUSH ("disallow new tasks") under it, the peer delete failed
 * with ESHUTDOWN and the BSS peer stayed half deleted. Let that work finish
 * first (it takes tens of milliseconds); qwz_stop() then finds the
 * association torn down in order.
 */
IOReturn
ItlQwz::sleepQuiesceGated(OSObject *, void *arg0, void *arg1, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* runAction's owner is the controller */
	uint32_t maxMs = (uint32_t)(uintptr_t)arg1;
	struct qwz_softc *sc = that->sc;
	IOWorkLoop *wl = qwz_hal_gate->getWorkLoop();
	uint64_t t0 = qwz_pm_ms(), waited;
	int tasks, initq, lock, t, i, l;

	that->sleepFailBase = sc->sc_peer_del_fail;
	that->sleepBaseValid = true;
	if (wl != NULL && wl->onThread()) {
		qwz_log("qwz: pm: sleep: on the workloop thread: not waiting for in-flight state work\n");
		return kIOReturnSuccess;
	}
	if (!qwz_state_work(sc, &tasks, &initq, &lock)) {
		qwz_log("qwz: pm: sleep: waited 0 ms for in-flight state work (none)\n");
		return kIOReturnSuccess;
	}
	/* The gate is released while asleep: the tasks need it to finish. */
	while ((waited = qwz_pm_ms() - t0) < maxMs && qwz_state_work(sc, &t, &i, &l))
		tsleep_delay_nsec(that, 0, "qwzquiesce", MSEC_TO_NSEC(QWZ_QUIESCE_POLL_MS));
	if (qwz_state_work(sc, &t, &i, &l))
		qwz_log("qwz: pm: sleep: waited %llu ms for in-flight state work and gave up: %d task(s), init task %d, ioctl lock %d; stopping anyway\n",
		    qwz_pm_ms() - t0, t, i, l);
	else
		qwz_log("qwz: pm: sleep: waited %llu ms for in-flight state work (%d state/key/BA task(s), init task %d, ioctl lock %d at the start)\n",
		    qwz_pm_ms() - t0, tasks, initq, lock);
	return kIOReturnSuccess;
}

void
ItlQwz::sleepQuiesce(uint32_t maxMs)
{
	if (!attached || shutdown || sc == NULL || qwz_hal_gate == NULL)
		return;
	qwz_hal_gate->runAction(sleepQuiesceGated, this, (void *)(uintptr_t)maxMs);
	qwz_log_publish();
}

bool
ItlQwz::firmwareUp()
{
	return attached && !shutdown && sc != NULL && sc->fw_initialized;
}

void
ItlQwz::sleepHardware()
{
	struct _ifnet *ifp;
	uint64_t t0 = qwz_pm_ms();
	uint32_t nfail;

	if (!attached || shutdown || sc == NULL) {
		qwz_log("qwz: pm: sleep: nothing to do (attached %d shutdown %d)\n", attached, shutdown);
		qwz_log_publish();
		return;
	}
	if (qwz_hal_gate != NULL && qwz_hal_gate->getWorkLoop() != NULL &&
	    qwz_hal_gate->getWorkLoop()->onThread()) {
		/* The teardown waits for the ioctl lock and for the firmware; the workloop thread must not sleep or spin. */
		qwz_log("qwz: pm: sleep: asked on the workloop thread, which cannot wait for the teardown: leaving the chip as it is\n");
		qwz_log_publish();
		return;
	}
	if (wakeNeeded && !sc->fw_initialized) {
		/* Wi-Fi was switched off, or this sleep already ran: the chip is down. */
		qwz_log("qwz: pm: sleep: the chip is already powered down; nothing to do\n");
		qwz_log_publish();
		sleepBaseValid = false;
		return;
	}
	ifp = &sc->sc_ic.ic_if;
	if (!sleepBaseValid)
		sleepFailBase = sc->sc_peer_del_fail;
	sleepBaseValid = false;
	qwz_log("qwz: pm: sleep: stopping the interface, fw_initialized %d\n", sc->fw_initialized);
	qwz_log_publish();
	/* As qwz_activate(DVACT_QUIESCE): the lock covers the stop and the power-down. */
	int err = rw_enter_write(&sc->ioctl_rwl);
	int locked = err == 0;

	/*
	 * M0 (R1-06): recover=0 or m0=0 selects the legacy handling, and qwz_declare_dead below is inert then, so a teardown without
	 * the lock would not be host-only: it would be a full firmware conversation beside the stuck holder. Keep the pre-M0 contract
	 * and wait for the holder (each call waits up to 20 s with the gate released; EBUSY and EDEADLK are final, so this never spins).
	 * Arming the machinery again (sysctl debug.qwz.recover=1) ends the wait and the branch below declares DEAD.
	 */
	while (err == ETIMEDOUT && !qwz_dead_armed()) {
		qwz_log("qwz: pm: sleep: the ioctl lock stayed busy for 20 s and DEAD is not armed (recover=0 or m0=0): waiting for the holder again\n");
		qwz_log_publish();
		err = rw_enter_write(&sc->ioctl_rwl);
		locked = err == 0;
	}
	if (!locked) {
		/* A stuck holder is by now well beyond every budget; DEAD makes the teardown host-only, so it cannot collide with the holder's firmware conversation. */
		qwz_log("qwz: pm: sleep: the ioctl lock stayed busy for 20 s: declaring DEAD and tearing down without it\n");
		qwz_declare_dead(sc, QWZ_DEAD_DEADLINE, "ioctl lock");
	}
	{
		int t, i, l;

		/* Work queued between sleepQuiesce() and here is cut off by the stop. */
		if (qwz_state_work(sc, &t, &i, &l) && (t || i))
			qwz_log("qwz: pm: sleep: in-flight state work appeared after the quiesce (%d task(s), init task %d): stopping anyway\n", t, i);
	}
	QWZ_CRUMB("sl:stop");
	ifp->if_flags &= ~IFF_UP;
	if (ifp->if_flags & IFF_RUNNING)
		qwz_stop(ifp);
	nfail = sc->sc_peer_del_fail - sleepFailBase;
	dirtyTeardown = qwz_peer_find_bss(sc, &sc->sc_vif, NULL) != NULL || nfail != 0;
	if (dirtyTeardown)
		qwz_log("qwz: pm: sleep: DIRTY teardown: %d peer(s) still listed, %u peer delete failure(s) on the way down\n",
		    sc->num_peers, nfail);
	if (sc->fw_initialized) {
		qwz_log("qwz: pm: sleep: firmware stop, MHI stop, MSI off, SoC reset (ath12k power down)\n");
		qwz_log_publish();
		QWZ_CRUMB("sl:deinit");
		qwz_core_deinit(sc);
	}
	if (locked)
		rw_exit(&sc->ioctl_rwl);
	saveConfig();
	QWZ_CRUMB("sl:saved");
	wakeNeeded = true;
	bzero(&stall, sizeof(stall));	/* the next firmware starts a new data-stall episode */
	qwz_log("qwz: pm: sleep: chip down in %llu ms; saved id %08x cmd %04x bar0 %08x:%08x msi %04x/%08x:%08x/%04x devctl %04x\n",
	    qwz_pm_ms() - t0, saved.id, saved.cmd, saved.bar[1], saved.bar[0],
	    saved.msiCtl, saved.msiHi, saved.msiLo, saved.msiData, saved.devCtl);
	qwz_log("qwz: pm: sleep: full re-init required on wake\n");
	qwz_log_publish();
}

bool
ItlQwz::wakeNeedsRecovery()
{
	return attached && !shutdown && wakeNeeded;
}

void
ItlQwz::wakeComplete(bool ok)
{
	if (!ok) {
		/* The firmware is gone either way: nothing recorded about the old one applies to the next try. */
		sleepBaseValid = false;
		busResetNext = false;
		dirtyTeardown = false;
		bzero(&stall, sizeof(stall));
		return;
	}
	wakeNeeded = false;
	bzero(&stall, sizeof(stall));	/* a new firmware: earlier evidence does not count */
	if (dirtyTeardown) {
		/* The data-stall watch is always on; this only notes that its trigger may follow. */
		qwz_log("qwz: pm: wake: the teardown before sleep was dirty: a data-stall detection below is probably its result\n");
		qwz_log_publish();
		dirtyTeardown = false;
	}
}

void
ItlQwz::requestBusReset()
{
	busResetNext = true;
}

/*
 * M0 (C18): the fault state and engine plumbing AirportItlwm's sentinel (C14) and engine (C15) use.
 * healthSnapshot and healthGated are the only ones that enter the gate, and only through attemptAction
 * (a probe that is itself an assumption: qwz_gate_try, S20, spec section 12 item 12); linkAlive is a
 * config read; the rest only touch atomics, so none of them can block.
 */
void
ItlQwz::setFaultHandler(void (*cb)(void *, uint32_t), void *arg)
{
	if (sc == NULL)
		return;
	sc->sc_fault_arg = arg;			/* the argument first: qwz_declare_dead reads the callback, then the argument */
	__atomic_store_n(&sc->sc_fault_cb, cb, __ATOMIC_RELEASE);
}

bool
ItlQwz::faultPending(uint32_t *reason)
{
	bool d = sc != NULL && !shutdown && qwz_is_dead(sc);	/* R1-05: not once shutdownGated runs: its DEAD (stuck lock) must not get the firmware restarted */

	if (reason != NULL)
		*reason = d ? sc->sc_dead_reason : 0;
	return d;
}

void
ItlQwz::declareDead(uint32_t reason, const char *detail)
{
	if (sc != NULL)
		qwz_declare_dead(sc, reason, detail);
}

const char *
ItlQwz::deadName()
{
	uint32_t r = sc != NULL ? sc->sc_dead_reason : 0;

	return qwz_dead_names[r < QWZ_DEAD_MAX ? r : 0];
}

void
ItlQwz::releaseWaiters()
{
	if (sc != NULL)
		qwz_wake_all_waiters(sc);
}

bool
ItlQwz::linkAlive()
{
	if (sc != NULL && qwz_inject_ones_active(sc))	/* inject=3 reads as a link loss here too: T3.6 does not depend on interrupts arriving in the window */
		return false;
	return pciDev == NULL || pciDev->configRead32(0) != 0xffffffffu;
}

void
ItlQwz::awdlQuiesce(bool on)
{
	if (sc == NULL)
		return;
	if (on && !sc->sc_awdl_quiesce && sc->sc_awdl_listening)
		qwz_log("qwz: pm: AWDL quiesced before the teardown (listen vdev %d, %d peer(s))\n", sc->sc_awdl_listening, sc->sc_awdl_npeers);
	__atomic_store_n(&sc->sc_awdl_quiesce, on ? 1u : 0u, __ATOMIC_RELEASE);
}

void
ItlQwz::noteRecovered(uint32_t reason)
{
	if (sc == NULL)
		return;
	__atomic_fetch_and(&sc->sc_life, ~QWZ_LIFE_RECOVERY, __ATOMIC_ACQ_REL);
	if (reason == QWZ_DEAD_RDDM && !qwz_recovery_proven) {	/* a REAL crash recovered in this boot: the AWDL probes may run (C24) */
		qwz_recovery_proven = 1;
		qwz_log("qwz: probe: recovery proven by a real crash (RDDM) in this boot\n");
	}
}

/*
 * M2a WP0: is a recovery episode open? QWZ_LIFE_RECOVERY is set by qwz_declare_dead together with DEAD and cleared only by
 * noteRecovered, so it survives the restart's own clear of DEAD (qwz_init) and a failed attempt: it is the one word that still
 * knows the episode when AirportItlwm's engine memory (engDeferred, engFailCont, engGivenUp) has been re-armed away by the
 * Wi-Fi on or the wake that brings the chip up. Lock-free reads of the life word and of the numbers qwz_declare_dead stored (a
 * torn read can only misprint a log value); false once shutdownHardware() ran, like faultPending().
 */
bool
ItlQwz::recoveryOpen(uint32_t *reason, uint32_t *episode, uint64_t *sinceMs)
{
	bool open = sc != NULL && !shutdown && (__atomic_load_n(&sc->sc_life, __ATOMIC_ACQUIRE) & QWZ_LIFE_RECOVERY) != 0;

	if (reason != NULL)
		*reason = open ? sc->sc_dead_reason : 0;
	if (episode != NULL)
		*episode = open ? sc->sc_dead_episodes : 0;
	if (sinceMs != NULL)
		*sinceMs = open ? (getnsecuptime() - sc->sc_dead_ns) / 1000000ULL : 0;
	return open;
}

void
ItlQwz::setEngineState(const char *state, uint32_t fails, bool givenUp)
{
	if (sc == NULL)
		return;
	if (givenUp)
		__atomic_fetch_or(&sc->sc_life, QWZ_LIFE_GIVEN_UP, __ATOMIC_ACQ_REL);
	else
		__atomic_fetch_and(&sc->sc_life, ~QWZ_LIFE_GIVEN_UP, __ATOMIC_ACQ_REL);
	engineStateName = state;
	engineFails = fails;
}

/*
 * The sentinel's gate probe, once per tick, from the watchdog workloop thread and without the gate:
 * attemptAction on the main gate never sleeps and returns kIOReturnCannotLock at once when the gate is
 * busy. Returns false only for a busy gate, and only after the probe has proven itself (first success);
 * a disabled gate (kIOReturnNotPermitted, around a power transition) is not a stall.
 */
bool
ItlQwz::healthSnapshot(bool print)
{
	static unsigned int probes;
	IOReturn r;
	int st;

	if (!attached || shutdown || sc == NULL || qwz_hal_gate == NULL)
		return true;			/* nothing to probe */
	st = __atomic_load_n(&qwz_gate_try, __ATOMIC_RELAXED);
	if (st < 0)
		return true;			/* attemptAction never succeeded on this gate: no gate accounting */
	r = qwz_hal_gate->attemptAction(healthGated, this, (void *)(intptr_t)print);
	if (r == kIOReturnSuccess) {
		if (st == 0) {
			__atomic_store_n(&qwz_gate_try, 1, __ATOMIC_RELAXED);
			qwz_log("qwz: sentinel: first gate probe: attemptAction on the main gate returned 0x0: usable (the legacy watchdog skips a busy tick from now on)\n");
		}
		return true;
	}
	if (st == 0 && ++probes >= 30) {
		__atomic_store_n(&qwz_gate_try, -1, __ATOMIC_RELAXED);
		qwz_log("qwz: sentinel: attemptAction never succeeded on the main gate in %u probes (last result 0x%x): gate probing is off, the legacy watchdog keeps its blocking runAction\n", probes, (unsigned)r);
		return true;
	}
	return st == 0 || r != kIOReturnCannotLock;	/* only a busy gate is a gate-busy tick, and only once the probe has proven itself; a disabled gate (kIOReturnNotPermitted, around a power transition) is not a stall */
}

IOReturn
ItlQwz::healthGated(OSObject *, void *arg0, void *arg1, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* runAction's owner is the controller: the HAL is passed explicitly */

	if (that->sc == NULL)		/* detached between the probe's test and the gate (cannot happen while the sentinel runs: stop() ends it first) */
		return kIOReturnSuccess;
	that->sc->sc_gate_ok_ns = getnsecuptime();
	if ((intptr_t)arg1)
		qwz_health_log(that->sc, qwz_wd_skipped);		/* C26: under the gate, and safe in every state: it checks fw_initialized and the ring pointer itself (qwz_dp_free frees the array and sets it to NULL) */
	if ((intptr_t)arg1)
		qwz_awdl_pool_log(that->sc);		/* WP4 (M2a): the lane M pool's lines, at the health cadence. LOG ONLY, and it sits in the sentinel's probe body (healthGated), as qwz_health_log above does: it reads the pool, prints, and sets the per-slot 'reported' flag; nothing while the pool is unused */
	return kIOReturnSuccess;
}

/*
 * Data-stall watch, polled once a second by the controller's watchdog. After
 * the build-3 sleep with a half-deleted peer the firmware associated again
 * and again, management frames kept arriving, but not one data frame did (no
 * EAPOL, so no 4-way handshake). Nothing a host driver can do repairs that
 * short of restarting the firmware, so say so: true when the interface has
 * spent QWZ_STALL_RUN_MS in RUN without one data frame while at least
 * QWZ_STALL_MGMT_MIN management frames arrived (the firmware is alive, only
 * its data path is dead). The RUN time is added up over consecutive
 * associations: the AP gives up on a stalled handshake within seconds, so a
 * single association may never last that long. Management frames count from
 * the start of the episode, in RUN or not (the firmware seems to pass few
 * beacons on while associated). One data frame, the first EAPOL message of a
 * working link, ends the episode, and so does a minute without RUN (old
 * evidence does not carry over to a later association). Five minutes in RUN
 * with data seen count as a healthy link: the controller then forgets its
 * earlier restarts.
 */
#define QWZ_STALL_RUN_MS	10000
#define QWZ_STALL_MGMT_MIN	3
#define QWZ_STALL_EPISODE_MS	60000
#define QWZ_STALL_HEALTHY_MS	300000

IOReturn
ItlQwz::stallCheckGated(OSObject *, void *arg0, void *arg1, void *arg2, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* runAction's owner is the controller */
	bool *stalled = (bool *)arg1;
	bool *healthy = (bool *)arg2;
	struct qwz_softc *sc = that->sc;
	struct ieee80211com *ic;
	uint64_t now = qwz_pm_ms();
	uint32_t data, mgmt, dt;
	bool inRun, wasRun;

	*stalled = false;
	*healthy = false;
	if (sc == NULL || !that->attached || that->shutdown || that->wakeNeeded ||
	    !sc->fw_initialized) {
		bzero(&that->stall, sizeof(that->stall));
		return kIOReturnSuccess;
	}
	ic = &sc->sc_ic;
	data = sc->sc_stall_data_seen;		/* M0 (C23): the frames that are not AWDL (sc_probe_* keep counting everything for the probe summary) */
	mgmt = sc->sc_stall_mgmt_seen;
	if (!that->stall.valid) {
		that->stall.valid = true;
		that->stall.data = data;
		that->stall.mgmt = mgmt;
		that->stall.lastMs = now;
		that->stall.lastRunMs = now;
		return kIOReturnSuccess;
	}
	/* A late sample (the gate was busy) must not count seconds that were not RUN. */
	dt = (uint32_t)MIN(now - that->stall.lastMs, 2000);
	that->stall.lastMs = now;
	wasRun = that->stall.inRun;
	inRun = ic->ic_state == IEEE80211_S_RUN && (ic->ic_if.if_flags & IFF_RUNNING);
	that->stall.inRun = inRun;
	if (inRun && !wasRun)
		that->stall.periodData = false;		/* a new association */
	if (inRun)
		that->stall.lastRunMs = now;
	else if (now - that->stall.lastRunMs > QWZ_STALL_EPISODE_MS &&
	    (that->stall.runMs != 0 || that->stall.mgmtIn != 0 || that->stall.logged)) {
		/* A minute without RUN: whatever was counted before is stale. */
		that->stall.runMs = 0;
		that->stall.mgmtIn = 0;
		that->stall.logged = false;
	}
	if (data != that->stall.data) {
		/* A data frame: the data path works. */
		that->stall.periodData = true;
		that->stall.runMs = 0;
		that->stall.mgmtIn = 0;
		that->stall.logged = false;
	} else
		that->stall.mgmtIn += mgmt - that->stall.mgmt;
	if (inRun && wasRun && !that->stall.periodData)
		that->stall.runMs += dt;		/* RUN since the last sample too */
	that->stall.data = data;
	that->stall.mgmt = mgmt;
	if (inRun && wasRun && that->stall.periodData)
		that->stall.healthyMs += dt;
	else if (!inRun || !that->stall.periodData)
		that->stall.healthyMs = 0;
	*healthy = that->stall.healthyMs >= QWZ_STALL_HEALTHY_MS;
	if (!inRun || that->stall.periodData || that->stall.runMs < QWZ_STALL_RUN_MS ||
	    that->stall.mgmtIn < QWZ_STALL_MGMT_MIN)
		return kIOReturnSuccess;
	*stalled = true;
	if (!that->stall.logged) {
		that->stall.logged = true;
		qwz_log("qwz: pm: data-stall: %u ms in RUN without a data frame (data_seen %u), %u management frames meanwhile (mgmt_seen %u), dirty teardown at the last sleep %d\n",
		    that->stall.runMs, data, that->stall.mgmtIn, mgmt, that->dirtyTeardown);
		qwz_log_publish();
	}
	return kIOReturnSuccess;
}

bool
ItlQwz::dataStalled(bool *healthy)
{
	bool stalled = false, ok = false;

	if (attached && !shutdown && qwz_hal_gate != NULL) {
		/* M0 (C16): never block behind the gate this poll is meant to watch, but only once attemptAction has proven itself (S20) */
		IOReturn r = kIOReturnNotReady;

		if (qwz_m0_on && __atomic_load_n(&qwz_gate_try, __ATOMIC_RELAXED) > 0)
			r = qwz_hal_gate->attemptAction(stallCheckGated, this, &stalled, &ok);
		if (r == kIOReturnCannotLock)
			qwz_wd_skipped++;			/* the gate is busy: no verdict this tick (stalled and ok stay false) */
		else if (r != kIOReturnSuccess)
			qwz_hal_gate->runAction(stallCheckGated, this, &stalled, &ok);	/* the legacy blocking call */
	}
	if (healthy != NULL)
		*healthy = ok;
	return stalled;
}

IOReturn
ItlQwz::wakeHardware()
{
	IOService *bus = NULL;
	IOPCIDevice *br;
	uint64_t t0 = qwz_pm_ms();
	uint32_t id;
	uint16_t brCtl = 0, lsta = 0;
	uint32_t lcap = 0;
	UInt8 exp = 0, pm = 0;
	bool haveExp = false, dlla = false;
	bool forceSbr = busResetNext;	/* the data-stall restart asked for the stronger reset */
	const char *step = "none (config already valid)";
	int i;

	busResetNext = false;
	QWZ_CRUMB("wk:begin");
	if (!attached || shutdown || pciDev == NULL)
		return kIOReturnNotReady;
	if (!saved.valid) {
		qwz_log("qwz: pm: wake: no saved PCI state (sleep never ran?)\n");
		qwz_log_publish();
	}

	br = parentBridge(&bus);
	if (br != NULL && br->findPCICapability(kIOPCIPCIExpressCapability, &exp)) {
		haveExp = true;
		lcap = br->configRead32(exp + QWZ_PM_PCIE_LNKCAP);
		dlla = (lcap & QWZ_PM_PCIE_LNKCAP_DLLLARC) != 0;
	}
	id = pciDev->configRead32(0);
	pciDev->findPCICapability(kIOPCIPowerManagementCapability, &pm);
	qwz_log("qwz: pm: wake: dev id %08x cmd %04x bar0 %08x pmcsr %04x\n", id,
	    pciDev->configRead16(kIOPCIConfigCommand),
	    pciDev->configRead32(kIOPCIConfigBaseAddress0),
	    pm ? pciDev->configRead16(pm + 4) : 0xffff);
	if (br != NULL) {
		if (haveExp) {
			lsta = br->configRead16(exp + QWZ_PM_PCIE_LNKSTA);
			brCtl = br->configRead16(QWZ_PM_BRIDGE_CTL);
			qwz_log("qwz: pm: wake: parent %02x:%02x.%x lnkcap %08x lnkctl %04x lnksta %04x (DLLLA %d) brctl %04x buses %08x\n",
			    br->getBusNumber(), br->getDeviceNumber(), br->getFunctionNumber(),
			    lcap, br->configRead16(exp + QWZ_PM_PCIE_LNKCTL), lsta,
			    (lsta & QWZ_PM_PCIE_LNKSTA_DLLLA) != 0, brCtl,
			    br->configRead32(0x18));
		} else
			qwz_log("qwz: pm: wake: parent bridge has no PCIe capability\n");
	} else
		qwz_log("qwz: pm: wake: parent bridge not found in the registry\n");
	qwz_log_publish();

	if (id == 0xffffffffu) {
		/* a. wait: the link may still be training */
		QWZ_CRUMB("wk:poll");
		for (i = 0; i < 100 && id == 0xffffffffu; i++) {
			IOSleep(10);
			id = pciDev->configRead32(0);
		}
		if (id != 0xffffffffu)
			step = "wait";
		qwz_log("qwz: pm: wake: after 1 s poll id %08x\n", id);
		qwz_log_publish();
	}
	if (id == 0xffffffffu && br != NULL && haveExp) {
		/* b. Retrain Link on the parent port (pcie_retrain_link) */
		uint16_t lc = br->configRead16(exp + QWZ_PM_PCIE_LNKCTL);

		qwz_log("qwz: pm: wake: still down: retraining the parent link\n");
		qwz_log_publish();
		QWZ_CRUMB("wk:retrain");
		br->configWrite16(exp + QWZ_PM_PCIE_LNKCTL, lc | QWZ_PM_PCIE_LNKCTL_RL);
		for (i = 0; i < 50; i++) {	/* up to 500 ms */
			IOSleep(10);
			lsta = br->configRead16(exp + QWZ_PM_PCIE_LNKSTA);
			if (dlla) {
				if (lsta & QWZ_PM_PCIE_LNKSTA_DLLLA)
					break;
			} else if (!(lsta & QWZ_PM_PCIE_LNKSTA_LT) &&
			    pciDev->configRead32(0) != 0xffffffffu)
				break;
		}
		IOSleep(100);	/* link-up settle, as pci_bridge_wait_for_secondary_bus */
		id = pciDev->configRead32(0);
		qwz_log("qwz: pm: wake: retrain done after ~%d ms: lnksta %04x id %08x\n",
		    (i + 1) * 10 + 100, br->configRead16(exp + QWZ_PM_PCIE_LNKSTA), id);
		if (id != 0xffffffffu)
			step = "retrain";
		qwz_log_publish();
	}
	if ((id == 0xffffffffu || forceSbr) && br != NULL) {
		/* c. Secondary Bus Reset, only when this is the one device behind the port */
		if (!soleDeviceBehind(bus)) {
			qwz_log("qwz: pm: wake: SBR skipped: other devices behind the parent bridge\n");
		} else {
			uint16_t bc = br->configRead16(QWZ_PM_BRIDGE_CTL);

			qwz_log("qwz: pm: wake: secondary bus reset on the parent bridge%s\n",
			    id != 0xffffffffu ? " (requested, the device still answers)" : "");
			qwz_log_publish();
			QWZ_CRUMB("wk:sbr");
			br->configWrite16(QWZ_PM_BRIDGE_CTL, bc | QWZ_PM_BRIDGE_CTL_SBR);
			IOSleep(2);		/* PCI_RESET_ASSERT 2 ms (>= 1 ms) */
			br->configWrite16(QWZ_PM_BRIDGE_CTL, bc & ~QWZ_PM_BRIDGE_CTL_SBR);
			IOSleep(100);	/* PCIE_RESET_READY: 100 ms before the first access */
			for (i = 0; i < 100; i++) {	/* up to 1 s */
				id = pciDev->configRead32(0);
				if (id != 0xffffffffu)
					break;
				IOSleep(10);
			}
			qwz_log("qwz: pm: wake: SBR done: id %08x brctl %04x\n", id,
			    br->configRead16(QWZ_PM_BRIDGE_CTL));
			if (id != 0xffffffffu)
				step = "secondary bus reset";
		}
		qwz_log_publish();
	}

	if (id == 0xffffffffu) {
		qwz_log("qwz: pm: wake: FAILED: device config still reads ffffffff after %llu ms\n", qwz_pm_ms() - t0);
		qwz_log_publish();
		return kIOReturnNotResponding;
	}
	if (saved.valid && id != saved.id) {
		qwz_log("qwz: pm: wake: FAILED: device id %08x is not the saved %08x\n", id, saved.id);
		qwz_log_publish();
		return kIOReturnNotResponding;
	}

	/* d. config is valid: D0 first, then restore what a power loss wipes */
	if (pm) {
		uint16_t pmcsr = pciDev->configRead16(pm + 4);

		if (pmcsr & 3) {
			qwz_log("qwz: pm: wake: PMCSR %04x: moving to D0 (10 ms settle)\n", pmcsr);
			pciDev->configWrite16(pm + 4, pmcsr & ~0x8003);
			IOSleep(10);
		}
	}
	QWZ_CRUMB("wk:restore");
	if (saved.valid) {
		UInt8 off = saved.msiOff;

		if (pciDev->configRead32(kIOPCIConfigBaseAddress0) != saved.bar[0] ||
		    pciDev->configRead32(kIOPCIConfigBaseAddress0 + 4) != saved.bar[1]) {
			qwz_log("qwz: pm: wake: BAR0 %08x:%08x lost: restoring %08x:%08x\n",
			    pciDev->configRead32(kIOPCIConfigBaseAddress0 + 4),
			    pciDev->configRead32(kIOPCIConfigBaseAddress0), saved.bar[1], saved.bar[0]);
			pciDev->configWrite16(kIOPCIConfigCommand, 0);	/* decode off while BARs move */
			pciDev->configWrite32(kIOPCIConfigBaseAddress0, saved.bar[0]);
			pciDev->configWrite32(kIOPCIConfigBaseAddress0 + 4, saved.bar[1]);
		}
		if (pciDev->configRead32(0x0c) != saved.cls)
			pciDev->configWrite32(0x0c, saved.cls);
		if (off && (pciDev->configRead32(off + 4) != saved.msiLo ||
		    ((saved.msiCtl & 0x80) && pciDev->configRead32(off + 8) != saved.msiHi) ||
		    pciDev->configRead16(off + ((saved.msiCtl & 0x80) ? 12 : 8)) != saved.msiData)) {
			qwz_log("qwz: pm: wake: MSI address/data lost: restoring %08x:%08x/%04x\n",
			    saved.msiHi, saved.msiLo, saved.msiData);
			pciDev->configWrite32(off + 4, saved.msiLo);
			if (saved.msiCtl & 0x80) {
				pciDev->configWrite32(off + 8, saved.msiHi);
				pciDev->configWrite16(off + 12, saved.msiData);
			} else
				pciDev->configWrite16(off + 8, saved.msiData);
			pciDev->configWrite16(off + 2, saved.msiCtl);	/* MSI stays off: power_up enables it */
		}
		if (saved.expOff && pciDev->configRead16(saved.expOff + 8) != saved.devCtl)
			pciDev->configWrite16(saved.expOff + 8, saved.devCtl);
		/*
		 * M0 (R1-02): with the bus-master rule on, BM comes only from the snapshot. The snapshot of a dead or no-link
		 * teardown has it off (qwz_pci_power_down cleared it, saveConfig records it): it stays off until qwz_pci_power_up
		 * has reset the chip and asserts it. A healthy snapshot carries BM (cmd 0406), so a healthy wake writes what it wrote before.
		 */
		pciDev->configWrite16(kIOPCIConfigCommand,
		    saved.cmd | kIOPCICommandMemorySpace | (qwz_bme ? 0 : kIOPCICommandBusMaster));
	} else {
		/* M0 (C12): INTx stays disabled here too (the valid branch carries it in the normalised saved.cmd) */
		pciDev->configWrite16(kIOPCIConfigCommand,
		    pciDev->configRead16(kIOPCIConfigCommand) | kIOPCICommandMemorySpace | (qwz_bme ? 0 : kIOPCICommandBusMaster) | kIOPCICommandInterruptDisable);
	}
	qwz_log("qwz: pm: wake: PCI ready (recovery step: %s) in %llu ms: id %08x cmd %04x bar0 %08x\n",
	    step, qwz_pm_ms() - t0, pciDev->configRead32(0),
	    pciDev->configRead16(kIOPCIConfigCommand), pciDev->configRead32(kIOPCIConfigBaseAddress0));
	qwz_log_publish();
	QWZ_CRUMB("wk:done");
	return kIOReturnSuccess;
}

/* Called from the controller's systemWillShutdown(), before IOKit stops us. */
void
ItlQwz::shutdownHardware()
{
	if (qwz_hal_gate != NULL)
		qwz_hal_gate->runAction(shutdownGated, this);
}

void
ItlQwz::detach(IOPCIDevice *device)
{
	IOWorkLoop *wl = qwz_hal_gate != NULL ? qwz_hal_gate->getWorkLoop() : NULL;

	/*
	 * M0.1 (HF-4): detachGated frees the softc, and the starvation task of debug.qwz.inject=2 or 7 keeps raw pointers into it
	 * for up to 6 s, asleep with the gate released. Close the sysctl first (no new window), then wait for a running one here,
	 * OUTSIDE the gate: the task needs the gate to finish. On the workloop thread or inside the gate an IOSleep would hold the
	 * gate itself, so qwz_inject_quiesce is then told it cannot wait and only logs.
	 */
	qwz_sysctl_set_inject(NULL, NULL);
	qwz_inject_quiesce(wl == NULL || (!wl->onThread() && !wl->inGate()));
	if (qwz_hal_gate != NULL)
		qwz_hal_gate->runAction(detachGated, this);
	qwz_compat_fini();
}

void
ItlQwz::free()
{
	super::free();
}

IOReturn
ItlQwz::enableGated(OSObject *, void *arg0, void *, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* runAction's owner is the controller */
	struct _ifnet *ifp = &that->sc->sc_ic.ic_if;
	int err = 0;

	/* What qwz_ioctl(SIOCSIFFLAGS) does when the interface goes up. */
	if (rw_enter_write(&that->sc->ioctl_rwl) != 0) {
		QWZLOG("enable: the ioctl lock stayed busy");
		return kIOReturnBusy;
	}
	ifp->if_flags |= IFF_UP;
	/*
	 * Merged from the capture cell (qwz_mon.c, awdl-m0-m1): with debug.qwz.monhold set and a cell running, Wi-Fi "on" only marks the
	 * interface UP|RUNNING and the station is not started. qwz_mon_holding() has to read false from the moment a teardown ended the
	 * cell: qwz_core_deinit ends it host-only through qwz_mon_epoch_reset (qwz_fw_epoch_reset does not touch it), and qwz_mon_holding
	 * also reads false for a firmware that is not initialized, crashed or DEAD, or a wake would stop here and never reach qwz_init. sc_awdl_quiesce is not lifted
	 * here: C17 latches it in every teardown and only awdltest=6 lifts it, and the listen vdev that test joins needs the association
	 * a hold refuses, so the hold and the native AWDL lane (lane M) exclude each other. With no hold the else-if below is the test
	 * this tree always had (qwz_init runs when the interface is not yet RUNNING), in upstream's chained form.
	 */
	if (qwz_mon_holding(&that->sc->sc_ic)) {
		/* M3 hold: Wi-Fi "on" over the running cell, no station. */
		ifp->if_flags |= IFF_RUNNING;
		QWZLOG("mon: hold: interface up without the station");
	} else if (!(ifp->if_flags & IFF_RUNNING))
		err = qwz_init(ifp);
	rw_exit(&that->sc->ioctl_rwl);
	if (err)
		QWZLOG("init failed: %d", err);
	return err ? kIOReturnError : kIOReturnSuccess;
}

IOReturn
ItlQwz::enable(IONetworkInterface *netif)
{
	return (attached && !shutdown) ? qwz_hal_gate->runAction(enableGated, this) : kIOReturnNotReady;
}

IOReturn
ItlQwz::disableGated(OSObject *, void *arg0, void *, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* runAction's owner is the controller */
	struct _ifnet *ifp = &that->sc->sc_ic.ic_if;

	if (rw_enter_write(&that->sc->ioctl_rwl) != 0) {
		QWZLOG("disable: the ioctl lock stayed busy");
		return kIOReturnBusy;
	}
	/* AWDL stops at the START of every teardown (C17): its entry points return ENOTCONN while the flag is set */
	__atomic_store_n(&that->sc->sc_awdl_quiesce, 1u, __ATOMIC_RELEASE);
	{
		int t, i, l;

		if ((ifp->if_flags & IFF_RUNNING) && qwz_state_work(that->sc, &t, &i, &l) && (t || i))
			qwz_log("qwz: pm: disable: stopping with state work in flight (%d task(s), init task %d)\n", t, i);
	}
	ifp->if_flags &= ~IFF_UP;
	/*
	 * Capture cell hold (see enableGated): the interface goes down, the station is not stopped and the cell stays. Wi-Fi off in this
	 * tree normally goes on into chipDown (radioOffGated, setPowerStateOff): sleepHardware then skips qwz_stop, as IFF_RUNNING is
	 * clear, and qwz_core_deinit powers the firmware down, which ends the cell host-only (qwz_mon_epoch_reset, called from
	 * qwz_core_deinit). This branch then only covers the short window before that. owlymac does not re-arm a cell: one lost to Wi-Fi off
	 * or to sleep needs `owlymac up` again (or debug.qwz.monhold=1 and debug.qwz.mon=<channel> by hand after the next Wi-Fi on).
	 * With -qwznowakefix no chip reset follows and the cell stays. sc_awdl_quiesce was latched above, before this branch, as for
	 * every teardown.
	 */
	if (qwz_mon_holding(&that->sc->sc_ic)) {
		ifp->if_flags &= ~IFF_RUNNING;
		QWZLOG("mon: hold: interface down, the cell stays");
	} else if (ifp->if_flags & IFF_RUNNING)
		qwz_stop(ifp);
	rw_exit(&that->sc->ioctl_rwl);
	return kIOReturnSuccess;
}

IOReturn
ItlQwz::disable(IONetworkInterface *netif)
{
	if (shutdown)
		return kIOReturnSuccess;	/* the chip is already off */
	return attached ? qwz_hal_gate->runAction(disableGated, this) : kIOReturnNotReady;
}

struct ieee80211com *
ItlQwz::get80211Controller()
{
	return sc ? &sc->sc_ic : NULL;
}

ItlDriverInfo *
ItlQwz::getDriverInfo()
{
	return this;
}

ItlDriverController *
ItlQwz::getDriverController()
{
	return this;
}

const char *
ItlQwz::getFirmwareVersion()
{
	return fwVersion;
}

int16_t
ItlQwz::getBSSNoise()
{
	return -95;	/* dBm; getNOISE() and scan results must not negate */
}

bool
ItlQwz::is5GBandSupport()
{
	return true;
}

int
ItlQwz::getTxNSS()
{
	struct ieee80211com *ic = sc ? &sc->sc_ic : NULL;
	int nss = (sc && sc->num_tx_chains > 0) ? sc->num_tx_chains : 2;

	/* Spatial streams usable towards the BSS: our chains, capped by the AP. */
	if (ic != NULL && ic->ic_bss != NULL && ic->ic_bss->ni_rx_nss > 0 &&
	    ic->ic_bss->ni_rx_nss < nss)
		nss = ic->ic_bss->ni_rx_nss;
	return nss;
}

/* The last unicast data frame received: qwz_dp_rx_h_rate in qwz.c. */
uint32_t
ItlQwz::getRxRateKbps()
{
	return sc ? sc->sc_rx_rate_kbps : 0;
}

int
ItlQwz::getRxRateMcs()
{
	return sc ? sc->sc_rx_rate_mcs : -1;
}

int
ItlQwz::getRxRateNss()
{
	return sc ? sc->sc_rx_rate_nss : 0;
}

int
ItlQwz::getRxRateSgi()
{
	return (sc && sc->sc_rx_rate_kbps != 0) ? sc->sc_rx_rate_sgi : -1;
}

/*
 * The smoothed RSSI of the associated BSS (qwz_bss_rssi_sample in qwz.c), in
 * dBm. Falls back to the caller's raw value when the average does not belong
 * to the BSS we are in now, so a stale value is never reported.
 */
int8_t
ItlQwz::getBSSRssi(int8_t raw)
{
	struct ieee80211com *ic = sc ? &sc->sc_ic : NULL;
	int x, dbm;

	if (ic == NULL || ic->ic_state != IEEE80211_S_RUN || ic->ic_bss == NULL ||
	    !sc->sc_bss_rssi_valid ||
	    !IEEE80211_ADDR_EQ(sc->sc_bss_rssi_bssid, ic->ic_bss->ni_bssid))
		return raw;
	x = sc->sc_bss_rssi_fx;		/* 1/16 dBm */
	dbm = x >= 0 ? (x + 8) / 16 : -((-x + 8) / 16);
	if (dbm >= 0 || dbm < -127)
		return raw;
	return (int8_t)dbm;
}

const char *
ItlQwz::getFirmwareName()
{
	return "qwz-wcn7850-hw2.0";
}

UInt32
ItlQwz::supportedFeatures()
{
	return kIONetworkFeatureMultiPages;
}

const char *
ItlQwz::getFirmwareCountryCode()
{
	return "ZZ";
}

uint32_t
ItlQwz::getTxQueueSize()
{
	return 256;
}

void
ItlQwz::clearScanningFlags()
{
}

IOReturn
ItlQwz::setMulticastList(IOEthernetAddress *addr, int count)
{
	return kIOReturnSuccess;
}

#ifdef AIRPORT_WCL
IOReturn
ItlQwz::setScanHomeAwayTime(uint32_t ms)
{
	if (sc == NULL)
		return kIOReturnNotReady;
	sc->sc_ic.ic_wcl_scan_away_time_ms = ms;
	return kIOReturnSuccess;
}
#endif
