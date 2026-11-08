/*
 * qwz_sysctl - the debug.qwz.* sysctls. See qwz_sysctl.h.
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#include "qwz_sysctl.h"
#include "qwz_log.h"
#include "qwz_life.h"
#include "qwz_mon.h"
#include "qwz_awdl0.h"

#include <IOKit/IOLib.h>

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/sysctl.h>

extern int qwz_stage_sysctl;	/* qwz_compat.cpp */

/*
 * debug.qwz.he: per-association kill switch for 802.11ax.  Read by
 * ieee80211_he_supported() (itl80211 ieee80211_proto.c).  It can only turn HE
 * off: HE exists at all only when the itlwm_he=1 boot-arg set HEON before the
 * firmware's capabilities were read (qwz_mac_setup_he_cap).
 */
int qwz_he_sysctl = 1;

/*
 * M0 runtime knobs (docs/m0-spec.md section 4.5, C32), declared in qwz_life.h.
 * The defaults leave the infrastructure behaviour exactly as in awdldata8.
 * They are exposed as debug.qwz.m0, deadaware, mmioguard, bme, softassert,
 * recover, silent, awdlprobe, awdlforce, capsae, inject_arg and proven (the
 * last is read-only: ItlQwz::noteRecovered sets qwz_recovery_proven).
 */
int qwz_m0_on = 1, qwz_dead_aware = 1, qwz_mmio_guard = 1, qwz_bme = 1, qwz_softassert = 1, qwz_recover = 1,
    qwz_silent = 1, qwz_awdlprobe = 0, qwz_awdlforce = 0, qwz_capsae = 0, qwz_inject_arg = 0, qwz_recovery_proven = 0,
    qwz_late_settle = 2000,		/* M0.1 (F1): debug.qwz.latesettle, ms of pause before the force wake and before the MHI reset of the late reset (none before the SoC reset) */
    qwz_late_mode = 0;		/* H1: debug.qwz.latemode, 0 the reviewed M0.1 sequence, 1 a blind SoC reset (posted writes only) before the first BAR read */
volatile int qwz_pm_pending = 0;	/* HF-1, HF-3: see qwz_life.h; not a sysctl */

int qwz_cap73 = 1;	/* S1 (hotspot Wi-Fi): debug.qwz.cap73, see qwz_life.h; its own line, apart from the list above */
int qwz_monnocred = 1;		/* K1 (W1): debug.qwz.monnocred, see qwz_life.h; K2c (C1): the default is 1 (clean in the 11-08 runs), 0 restores the EAGAIN answers of K1 */
unsigned int qwz_monqmax = 0;	/* K1 (W1): debug.qwz.monqmax, see qwz_life.h */
int qwz_monparams = 0;		/* K1 (W3): debug.qwz.monparams, see qwz_life.h */
int qwz_scanraw = 0;		/* K1 (W2c): debug.qwz.scanraw, see qwz_life.h */
int qwz_rssisigned = 0, qwz_beaconmeta = 0, qwz_scanprobe = 0, qwz_rssisrc = 0, qwz_scanrest = 0, qwz_rssilog = 0;	/* K3: debug.qwz.rssisigned, beaconmeta, scanprobe, rssisrc, scanrest, rssilog, see qwz_life.h */
int qwz_monreap = 0, qwz_hintfix = 1;	/* K2c: debug.qwz.monreap (0 = off = K2b, the stale-slot reaper and the live-slot guard of the capture TX ring), debug.qwz.hintfix (1 = the scan-start hint-BSSID copy goes the right way, 0 = the old swapped copy); see qwz_life.h */
int qwz_monfmt = 0;		/* K4 (M1): debug.qwz.monfmt, see qwz_life.h */
/* K5 (L1-C1): debug.qwz.moncoex and its sub-switches, see qwz_mon.h; moncoex 0 = K4 exactly; scan and cred default to 1 and only act while a coex cell runs */
int qwz_moncoex = 0, qwz_moncoexmode = 0, qwz_moncoexpassive = 0, qwz_moncoexfilter = 0, qwz_moncoexscan = 1, qwz_moncoexcred = 1, qwz_moncoexfw = 0, qwz_moncoexnss = 0, qwz_moncoexjoin = 0;
int qwz_moncoexwbm = 1;	/* K6 (K6-1): debug.qwz.moncoexwbm, see qwz_mon.h; no effect unless a coex cell runs (moncoex defaults to 0) */
int qwz_awdlcell = 0;	/* K5 (K5-00 banner, L2-C2): debug.qwz.awdlcell, see qwz_mon.h; no consumer yet (the receive-only tee is not built), the value is only reported in the K5 banner */
volatile unsigned int qwz_scanraw_gen = 0;	/* K2c (C6): bumped by every write to debug.qwz.scanraw; the per-boot evidence budgets (qwz_log.h qwz_log_rearmed) start again when it changes */
#ifndef QWZ_FWSEGS_DEFAULT
#define QWZ_FWSEGS_DEFAULT 0
#endif
int qwz_fwsegs = QWZ_FWSEGS_DEFAULT;	/* K5 (OBSD-2): debug.qwz.fwsegs, see qwz_life.h */
int qwz_hwmode = 1;		/* K2 (H1): debug.qwz.hwmode, see qwz_life.h; 1 = WMI_HOST_HW_MODE_DBS, today's request */
int qwz_hwm_guard = 0;		/* K2: boot-arg qwzhwmguard=1, see qwz_life.h */
unsigned int qwz_hwm_offered_mask = 0;	/* K2 (H1): bit n = the firmware offered hardware mode id n (n < 32); 0 until the extended service-ready event was parsed */

int qwz_awdltruth = 0, qwz_awdlclk = 0;	/* M2a WP5: see qwz_life.h; 0 and 0 is M0.1 on the air */
volatile unsigned int qwz_awdl_lat_us = 0;	/* M2a WP5 (awdlclk 8): see qwz_life.h */
/* WP4 (M2a): lane M hardening, see qwz_awdl.h */
int qwz_awdlhyg = 0, qwz_awdltx = 0, qwz_awdltx_eff = 0, qwz_awdlgate = 0, qwz_awdltxopt = 0;
int qwz_awdlpeers = 0;	/* M2a WP2: debug.qwz.awdlpeers (QWZ_AWDLP_*), declared in qwz_life.h */

static qwz_sysctl_start_fn qwz_sysctl_start_cb;
static void *qwz_sysctl_start_arg;
static qwz_sysctl_awdl_fn qwz_sysctl_awdl_cb;
static void *qwz_sysctl_awdl_arg;
static qwz_sysctl_awdltest_fn qwz_sysctl_awdltest_cb;
static void *qwz_sysctl_awdltest_arg;
static qwz_sysctl_inject_fn qwz_sysctl_inject_cb;	/* debug.qwz.inject (C33): ItlQwz::injectEntry */
static void *qwz_sysctl_inject_arg;
static qwz_sysctl_m0_fn qwz_sysctl_m0_cb;		/* debug.qwz.state, recover=2, health=1 (C34): AirportItlwm::m0Sysctl */
static void *qwz_sysctl_m0_arg;
static int qwz_awdltest_last = -1;	/* debug.qwz.awdltest read-back */
static int qwz_logfile_on;	/* debug.qwz.logfile: file sink kept open */
static bool qwz_sysctl_registered;

static int
qwz_sysctl_start(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = 0;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v != 1)
		return EINVAL;
	if (qwz_sysctl_start_cb == NULL)
		return ENXIO;	/* this product's bring-up cannot be rerun */
	qwz_log_file_begin();
	qwz_log("qwz: sysctl debug.qwz.start=1 received\n");
	qwz_log_file_end();
	return qwz_sysctl_start_cb(qwz_sysctl_start_arg) ? 0 : EBUSY;
}

static int
qwz_sysctl_awdltest(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = qwz_awdltest_last;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (qwz_sysctl_awdltest_cb == NULL)
		return ENXIO;	/* no chip attached */
	qwz_log("qwz: sysctl debug.qwz.awdltest=%d received\n", v);
	v = qwz_sysctl_awdltest_cb(qwz_sysctl_awdltest_arg, v);
	qwz_awdltest_last = v;
	return v < 0 ? -v : 0;
}

static int
qwz_sysctl_awdl(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = 0;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (qwz_sysctl_awdl_cb == NULL)
		return ENXIO;	/* QwzMac.kext, or no controller yet */
	qwz_log("qwz: sysctl debug.qwz.awdl=%d received\n", v);
	return qwz_sysctl_awdl_cb(qwz_sysctl_awdl_arg, v);
}

void
qwz_sysctl_set_awdl(qwz_sysctl_awdl_fn fn, void *arg)
{
	qwz_sysctl_awdl_cb = fn;
	qwz_sysctl_awdl_arg = arg;
}

void
qwz_sysctl_set_awdltest(qwz_sysctl_awdltest_fn fn, void *arg)
{
	qwz_sysctl_awdltest_cb = fn;
	qwz_sysctl_awdltest_arg = arg;
}

/* M0 (C32): same pattern as qwz_sysctl_set_awdltest; the callback stays NULL in QwzMac.kext and the nodes answer ENXIO */
void
qwz_sysctl_set_inject(qwz_sysctl_inject_fn fn, void *arg)
{
	qwz_sysctl_inject_cb = fn;
	qwz_sysctl_inject_arg = arg;
}

void
qwz_sysctl_set_m0(qwz_sysctl_m0_fn fn, void *arg)
{
	qwz_sysctl_m0_cb = fn;
	qwz_sysctl_m0_arg = arg;
}

static int
qwz_sysctl_publish(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = 0;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	qwz_log_publish();	/* refresh QwzMacLog in the I/O Registry */
	return 0;
}

static int
qwz_sysctl_logfile(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = qwz_logfile_on;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	/*
	 * M0 (C32): a non-zero write is refused. The synchronous VNOP_WRITE per
	 * log line from kernel context froze this machine (qwz_log.cpp); the ring
	 * (QwzMacLog) is the only sink. Writing 0 keeps working. debug.qwz.start's
	 * own bring-up file sink (QwzMac.kext) is untouched: it never goes
	 * through this node.
	 */
	if (v) {
		qwz_log("qwz: sysctl debug.qwz.logfile refused: a synchronous file write from kernel context froze this machine\n");
		return EPERM;
	}
	if (qwz_logfile_on) {
		qwz_log("qwz: ==== file log off (sysctl) ====\n");
		qwz_log_file_end();
	}
	qwz_logfile_on = 0;
	return 0;
}

/*
 * M0 handlers (docs/m0-spec.md C32 to C34). Each copies its callback and
 * argument to locals once, so a set(NULL, NULL) from a detach or stop cannot
 * turn the NULL test and the call into two different reads.
 */

/* debug.qwz.state (read-only): the engine and fault state line, composed by AirportItlwm::m0Sysctl */
static int
qwz_sysctl_state(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	qwz_sysctl_m0_fn fn = qwz_sysctl_m0_cb;
	void *arg = qwz_sysctl_m0_arg;
	char buf[160];
	int err;

	if (fn == NULL)
		return ENXIO;	/* QwzMac.kext, or the Airport controller has not started */
	buf[0] = 0;
	err = fn(arg, QWZ_M0_STATE, 0, buf, sizeof(buf));
	if (err)
		return err;
	return sysctl_handle_string(oidp, buf, sizeof(buf), req);
}

/* debug.qwz.recover: 0 legacy crash handling, 1 armed; a write of 2 re-arms the engine (C34) */
static int
qwz_sysctl_recover(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	qwz_sysctl_m0_fn fn;
	void *arg;
	int v = qwz_recover;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v == 2) {
		fn = qwz_sysctl_m0_cb;
		arg = qwz_sysctl_m0_arg;
		if (fn == NULL)
			return ENXIO;
		qwz_log("qwz: sysctl debug.qwz.recover=2 received\n");
		return fn(arg, QWZ_M0_RECOVER, 2, NULL, 0);
	}
	if (v != 0 && v != 1)
		return EINVAL;
	qwz_log("qwz: sysctl debug.qwz.recover=%d (was %d; 0 legacy crash handling, 1 armed)\n", v, qwz_recover);
	qwz_recover = v;
	return 0;
}

/* debug.qwz.health=1: log one health line now (C34) */
static int
qwz_sysctl_health(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	qwz_sysctl_m0_fn fn;
	void *arg;
	int v = 0;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v != 1)
		return EINVAL;
	fn = qwz_sysctl_m0_cb;
	arg = qwz_sysctl_m0_arg;
	if (fn == NULL)
		return ENXIO;
	qwz_log("qwz: sysctl debug.qwz.health=1 received\n");
	return fn(arg, QWZ_M0_HEALTH, 1, NULL, 0);
}

/*
 * debug.qwz.inject=<mode>: the fault-injection harness (C33). The argument is
 * debug.qwz.inject_arg. ItlQwz::injectEntry mirrors awdlTestEntry for its own
 * refusals (a negative errno), qwz_inject returns a positive one: the sign is
 * normalised here, so the shell always sees an errno.
 */
static int
qwz_sysctl_inject(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	qwz_sysctl_inject_fn fn;
	void *arg;
	int v = 0;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	fn = qwz_sysctl_inject_cb;
	arg = qwz_sysctl_inject_arg;
	if (fn == NULL || arg == NULL)
		return ENXIO;	/* QwzMac.kext, or no chip attached; arg too: qwz_sysctl_set_inject stores the two in turn, so a load racing it can pair a live fn with a NULL arg */
	qwz_log("qwz: sysctl debug.qwz.inject=%d received (inject_arg %d)\n", v, qwz_inject_arg);
	v = fn(arg, v, qwz_inject_arg);
	return v < 0 ? -v : v;
}

/* debug.qwz.awdlprobe: the AWDL data-TX probe 0 to 3 with the recovery interlock (the handler text of C24) */
static int
qwz_sysctl_awdlprobe(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = qwz_awdlprobe;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v < 0 || v > 3)
		return EINVAL;
	if (v >= 1 && !qwz_recovery_proven && !qwz_awdlforce) {
		qwz_log("qwz: probe: awdlprobe=%d refused: recovery has not been proven by a real crash in this boot (debug.qwz.inject=6, then wait for \"recovery proven\")\n", v);
		return EPERM;
	}
	qwz_awdlprobe = v;
	qwz_log("qwz: probe: awdlprobe=%d (0 control, 1 mgmt path, 2 TCL plain Data, 3 TCL QoS as awdldata8)\n", v);
	return 0;
}

/*
 * M2a WP2: debug.qwz.awdlpeers (a bit mask, qwz_life.h). A write is checked against the known bits and logged with the old value, so the
 * ring says which arm of an A/B run a later line belongs to. A plain int: every reader tests one bit per use.
 */
static int
qwz_sysctl_awdlpeers(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	static unsigned int logged;
	int v = qwz_awdlpeers;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v < 0 || (v & ~QWZ_AWDLP_ALL) != 0)
		return EINVAL;
	if (qwz_log_ratelimit(&logged, 64, 16))
		qwz_log("qwz: sysctl debug.qwz.awdlpeers=%d received (was %d; bit 1 reset the heard-sender list at each listen, 2 honour the peer cache op, 4 the listen vdev takes awdl0's address)\n",
		    v, qwz_awdlpeers);
	qwz_awdlpeers = v;
	return 0;
}

/*
 * WP4 (M2a): debug.qwz.awdlhyg, awdltx, awdlgate and awdltxopt, the lane M hardening of qwz_awdl.h. A write is logged (the first 64
 * per knob, then every 16th, as debug.qwz.awdlpeers). awdltx 2 and 3 put new commands on the air, so they sit behind the same
 * recovery interlock as awdlprobe 1 to 3; a write of awdltx also sets the rung in force (debug.qwz.awdltxeff, read-only), which a
 * failure lowers at run time.
 */
static int
qwz_sysctl_awdlhyg(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	static unsigned int logged;
	int v = qwz_awdlhyg;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v < 0 || v > 1)
		return EINVAL;
	if (qwz_log_ratelimit(&logged, 64, 16))
		qwz_log("qwz: awdlp: sysctl debug.qwz.awdlhyg=%d (was %d): %s\n", v, qwz_awdlhyg,
		    v ? "own slot pool, 12-bit sequence numbers, the retry bit on a requeue, duration 0, QoS TID 0, pacing in outputStart" : "off");
	qwz_awdlhyg = v;
	return 0;
}

static int
qwz_sysctl_awdltx(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	static unsigned int logged, refused;
	int v = qwz_awdltx;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v < 0 || v > QWZ_AWDL_TX_MAX)
		return EINVAL;
	if (v >= 2 && !qwz_recovery_proven && !qwz_awdlforce) {
		if (qwz_log_ratelimit(&refused, 64, 16))
			qwz_log("qwz: awdlp: awdltx=%d refused: recovery has not been proven by a real crash in this boot (debug.qwz.inject=6, then wait for \"recovery proven\")\n", v);
		return EPERM;
	}
	if (qwz_log_ratelimit(&logged, 64, 16))
		qwz_log("qwz: awdlp: sysctl debug.qwz.awdltx=%d (was %d, in force %d): %s\n", v, qwz_awdltx, qwz_awdltx_eff,
		    v == 0 ? "the M0.1 lane" : v == 1 ? "F1, the own slot pool" : v == 2 ? "F2, + MGMT_RATE 12 Mbit/s (awdltest=8 or a re-join sets it) and the tx_params presets for Data" : "F3, + Data as OFFCHAN_DATA_TX_SEND");
	qwz_awdltx = qwz_awdltx_eff = v;
	return 0;
}

static int
qwz_sysctl_awdlgate(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	static unsigned int logged;
	int v = qwz_awdlgate;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v < 0 || v > 2)
		return EINVAL;
	if (qwz_log_ratelimit(&logged, 64, 16))
		qwz_log("qwz: awdlp: sysctl debug.qwz.awdlgate=%d (was %d): %s\n", v, qwz_awdlgate,
		    v == 0 ? "off" : v == 1 ? "peer-activity gate and requeue" : "+ the shared-slot test");
	qwz_awdlgate = v;
	return 0;
}

static int
qwz_sysctl_awdltxopt(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	static unsigned int logged;
	int v = qwz_awdltxopt;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v < 0 || v > 3)
		return EINVAL;
	if (qwz_log_ratelimit(&logged, 64, 16))
		qwz_log("qwz: awdlp: sysctl debug.qwz.awdltxopt=%d (was %d): 1 NON_AGG_SW_RETRY_TH 4, 2 BMISS counts raised; applied with awdltx >= 2 at the next join or awdltest=8\n", v, qwz_awdltxopt);
	qwz_awdltxopt = v;
	return 0;
}
/*
 * M2a WP5: debug.qwz.awdltruth and debug.qwz.awdlclk. Plain switches the Wi-Fi kext reads on the next frame it builds
 * (the AWDL kext cannot see them), so there is no callback: the handlers validate and log.
 */
static int
qwz_sysctl_awdltruth(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	static unsigned int logged;
	int v = qwz_awdltruth;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v < 0 || v > 3)
		return EINVAL;
	qwz_awdltruth = v;
	if (qwz_log_ratelimit(&logged, 64, 16))
		qwz_log("qwz: sysctl debug.qwz.awdltruth=%d received (0 a peer's TLVs copied, 1 every slot 149, 2 the engine's sequence projected to 149, 3 the followed peer's sequence, usable slots only)\n", v);
	return 0;
}

static int
qwz_sysctl_awdlclk(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	static unsigned int logged;
	int v = qwz_awdlclk;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v < 0 || (v & ~0x1f) != 0)
		return EINVAL;
	qwz_awdlclk = v;
	if (qwz_log_ratelimit(&logged, 64, 16))
		qwz_log("qwz: sysctl debug.qwz.awdlclk=0x%x received (1 AW sequence posted modulo 65536, 2 realign to the best heard peer every 20 s while master, 4 tx_counter rounded, 8 phy_tx_time stamped with the measured delay, 16 PSF as a follower every 440 TU)\n", v);
	return 0;
}

/*
 * origin/awdl-m0-m1 (docs/airdrop-revisited.md, driven by the owlymac daemon): the capture cell with its debug.qwz.mon* knobs, and an
 * awdl0 owned by this kext. It is a second AirDrop design beside lane M above and stays inert until its own knobs are used. Two of
 * its sysctl names differ from upstream's because ours were here first: debug.qwz.awdltx is the lane M preset (WP4), so upstream's
 * awdltx and awdlrx are debug.qwz.awdl0tx and debug.qwz.awdl0rx here (owlymac must read these), and upstream's qwz_awdl.h is
 * qwz_awdl0.h (qwz_awdl.h is the lane M header) with qwz_awdl0_set, _status, _tx_pending, _tx_drain and _rx_inject.
 */

/*
 * Closing hook of the capture cell's softc-facing nodes, in the pattern of qwz_sysctl_set_awdl and qwz_sysctl_set_m0. Upstream's
 * qwz_mon.c keeps its softc in a file-static pointer (set by qwz_mon_attach) and takes the command gate from qwz_compat_gate(), and
 * neither is cleared when the HAL service detaches, while these nodes stay registered until IOPCIEDeviceWrapper::stop() unregisters
 * them. qwz_sysctl_set_mon(NULL) makes debug.qwz.mon, montx, montxframe, monpeer, monmac and moncap answer ENXIO. It belongs beside the
 * other set(NULL) calls of AirportItlwm::stop() and of the detach and shutdown paths of ItlQwz, before the softc goes, and ItlQwz
 * passes itself once an attach has succeeded. The nodes are open until the first NULL, which is upstream's behaviour, so a build that
 * never calls it is unchanged. A flag, not a fence: it keeps new calls out and does not wait for one already inside qwz_mon.c.
 * debug.qwz.monflags, monhold and monbssid are plain variables, and awdlif, awdl0, awdl0tx and awdl0rx never touch the softc, so they
 * stay open.
 */
static volatile int qwz_sysctl_mon_closed;

void
qwz_sysctl_set_mon(void *owner)
{
	qwz_sysctl_mon_closed = (owner == NULL);
}

/* debug.qwz.mon: channel while capturing; write a channel to start, 0 to stop. */
static int
qwz_sysctl_mon(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	if (qwz_sysctl_mon_closed)
		return ENXIO;	/* qwz_sysctl_set_mon(NULL): before the status read below, which dereferences qwz_mon.c's softc */
	int v = qwz_mon_status();
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	return qwz_mon_set(v);
}

/* debug.qwz.scanraw (K1 W2c; K2c C6): the value is a plain int as before; a write also bumps qwz_scanraw_gen, which re-arms the per-boot evidence budgets */
static int
qwz_sysctl_scanraw(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = qwz_scanraw;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	qwz_scanraw = v;
	__atomic_add_fetch(&qwz_scanraw_gen, 1u, __ATOMIC_RELAXED);
	return 0;
}

/* debug.qwz.hwmode (K2 H1): the hardware mode id for the next firmware start. 0, 1 and 5 are always accepted; any other id only when the firmware offered it; else EINVAL. */
static int
qwz_sysctl_hwmode(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = qwz_hwmode;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (!(v == 0 || v == 1 || v == 5 || (v >= 0 && v < 32 && ((qwz_hwm_offered_mask >> v) & 1u))))
		return EINVAL;
	qwz_hwmode = v;
	return 0;
}

/* debug.qwz.montx: M1 transmit ops (bit 1 broadcast once, 2 auto reply, 4 unicast once). */
static int
qwz_sysctl_montx(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	if (qwz_sysctl_mon_closed)
		return ENXIO;
	int v = (int)qwz_mon_tx_mode;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	return qwz_mon_tx(v);
}

/* debug.qwz.montxframe: write a qwz_montx_hdr plus 802.11 frame to transmit it (M2). */
static int
qwz_sysctl_montxframe(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	uint8_t buf[sizeof(struct qwz_montx_hdr) + QWZ_MONTX_MAXFRAME];
	int err;

	if (qwz_sysctl_mon_closed)
		return ENXIO;
	if (req->newptr == USER_ADDR_NULL)
		return 0;	/* nothing to read back */
	if (req->newlen > sizeof(buf))
		return EINVAL;
	err = SYSCTL_IN(req, buf, req->newlen);
	if (err)
		return err;
	return qwz_mon_tx_frame(buf, req->newlen);
}

/* debug.qwz.monpeer: "add <mac>" / "del" for a firmware peer on the capture vdev. */
static char qwz_mon_peer_str[32];
static int
qwz_sysctl_monpeer(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	if (qwz_sysctl_mon_closed)
		return ENXIO;
	int err = sysctl_handle_string(oidp, qwz_mon_peer_str, sizeof(qwz_mon_peer_str), req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	return qwz_mon_peer(qwz_mon_peer_str);
}

/*
 * debug.qwz.awdlif: "feth0", "feth0=en3" or "-feth0" (qwz_mon.h). Merge interplay action: it keeps its own 32-byte string, so a write
 * to monpeer cannot overwrite what awdlif last held, nor the other way round. Upstream's handler names monpeer's buffer; the macro
 * points that name at awdlif's own buffer for this one function, so the handler's lines stay exactly as upstream wrote them.
 */
static char qwz_awdlif_str[32];
#define qwz_mon_peer_str qwz_awdlif_str
static int
qwz_sysctl_awdlif(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int err = sysctl_handle_string(oidp, qwz_mon_peer_str, sizeof(qwz_mon_peer_str), req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	return qwz_mon_awdlif(qwz_mon_peer_str);
}
#undef qwz_mon_peer_str

/* debug.qwz.monmac: the capture vdev's MAC (the AWDL address a node here must use). */
static int
qwz_sysctl_monmac(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	char mac[18];

	if (qwz_sysctl_mon_closed)
		return ENXIO;
	qwz_mon_mac_str(mac, sizeof(mac));
	return sysctl_handle_string(oidp, mac, sizeof(mac), req);
}

/* debug.qwz.moncap: drains the capture ring as a stream of qwz_moncap_hdr records. */
static int
qwz_sysctl_moncap(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	if (qwz_sysctl_mon_closed)
		return ENXIO;
	const size_t MAX = 1024 * 1024;
	size_t avail = qwz_mon_cap_pending(), max, n;
	void *buf;
	int err;

	if (req->newptr != USER_ADDR_NULL)
		return EPERM;
	if (req->oldptr == USER_ADDR_NULL) {
		req->oldidx = avail;
		return 0;
	}
	max = req->oldlen < MAX ? req->oldlen : MAX;
	if (max == 0)
		return 0;
	buf = IOMalloc(max);
	if (buf == NULL)
		return ENOMEM;
	n = qwz_mon_cap_drain(buf, max);
	err = n ? SYSCTL_OUT(req, buf, n) : 0;
	IOFree(buf, max);
	return err;
}


/* debug.qwz.awdl0: "xx:xx:xx:xx:xx:xx" creates the kext's awdl0, "down" destroys it; reads its state. */
static int
qwz_sysctl_awdl0(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	char buf[96];
	int err;

	if (req->newptr != USER_ADDR_NULL) {
		size_t n = req->newlen < sizeof(buf) - 1 ? req->newlen : sizeof(buf) - 1;
		err = SYSCTL_IN(req, buf, n);
		if (err)
			return err;
		buf[n] = 0;
		while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == 0))
			buf[--n] = 0;
		return qwz_awdl0_set(buf);
	}
	qwz_awdl0_status(buf, sizeof(buf));
	return SYSCTL_OUT(req, buf, strlen(buf) + 1);
}

/* debug.qwz.awdl0tx: drains the frames the stack sent on awdl0. */
static int
qwz_sysctl_awdl0tx(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	const size_t MAX = 512 * 1024;
	size_t avail = qwz_awdl0_tx_pending(), max, n;
	void *buf;
	int err;

	if (req->newptr != USER_ADDR_NULL)
		return EPERM;
	if (req->oldptr == USER_ADDR_NULL) {
		req->oldidx = avail;
		return 0;
	}
	max = req->oldlen < MAX ? req->oldlen : MAX;
	if (max == 0)
		return 0;
	buf = IOMalloc(max);
	if (buf == NULL)
		return ENOMEM;
	n = qwz_awdl0_tx_drain(buf, max);
	err = n ? SYSCTL_OUT(req, buf, n) : 0;
	IOFree(buf, max);
	return err;
}

/* debug.qwz.awdl0rx: frames from the air, injected as received on awdl0. */
static int
qwz_sysctl_awdl0rx(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	const size_t MAX = 512 * 1024;
	void *buf;
	int err;

	if (req->newptr == USER_ADDR_NULL)
		return 0;
	if (req->newlen < 4 || req->newlen > MAX)
		return EINVAL;
	buf = IOMalloc(req->newlen);
	if (buf == NULL)
		return ENOMEM;
	err = SYSCTL_IN(req, buf, req->newlen);
	if (err == 0)
		err = qwz_awdl0_rx_inject(buf, req->newlen);
	IOFree(buf, req->newlen);
	return err;
}

SYSCTL_NODE(_debug, OID_AUTO, qwz, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "QwzMac");
SYSCTL_PROC(_debug_qwz, OID_AUTO, mon, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_mon, "I", "monitor-mode capture: channel to start, 0 to stop (Wi-Fi must be off)");
SYSCTL_UINT(_debug_qwz, OID_AUTO, monflags, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_mon_flags, 0, "monitor-mode experiment flags (QWZ_MON_F_* in qwz_mon.h)");
SYSCTL_UINT(_debug_qwz, OID_AUTO, monhold, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_mon_hold, 0, "1: Wi-Fi on keeps the capture cell instead of starting the station");
SYSCTL_INT(_debug_qwz, OID_AUTO, monnocred, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_monnocred, 0, "K1 W1 / K2c: 1 (default) = the user-space frame write answers ENOSPC (not EAGAIN) while both WMI credits are in flight and ENXIO while the firmware is down; 0 = EAGAIN (the K1/K2b default)");
SYSCTL_UINT(_debug_qwz, OID_AUTO, monqmax, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_monqmax, 0, "K1 W1 / K2b: N > 0 = ENOSPC for user DATA and CONTROL frames once N of them are in flight (1..63, larger values act as 63); management frames are never counted; 0 = off (default)");
SYSCTL_INT(_debug_qwz, OID_AUTO, monparams, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_monparams, 0, "K1 W3: 1 = cell vdev MGMT_RATE OFDM 12 Mbit/s after vdev up, 2 = also MGMT_RETRY_LIMIT 3 and NON_AGG_SW_RETRY_TH 4; 0 = off (default); read when the cell starts");
SYSCTL_INT(_debug_qwz, OID_AUTO, fwsegs, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_fwsegs, 0, "K5 OBSD-2: firmware host memory layout: 0 = one 64 KB-aligned block (K4 behaviour, default), 1 = one block per firmware segment, each aligned to the next power of two of its size (single block if an allocation fails), 2 = dry run (allocate, log and free each segment, then as 0); any other value acts as 0; read at each firmware start, so a Wi-Fi off/on applies it");
SYSCTL_INT(_debug_qwz, OID_AUTO, moncoex, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_moncoex, 0, "K5 L1-C1: 1 = a capture cell started while the station is associated on the same channel (6, 44 or 149) runs beside it and the station is not stopped; 0 = off (default), the K4 cell; read when the cell starts");
SYSCTL_INT(_debug_qwz, OID_AUTO, moncoexmode, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_moncoexmode, 0, "K5 L1-C1: coex cell vdev-start channel: 0 = clone the station's chandef (default), 1 = HT20 on the station's primary channel (the K4 cell's parameters); read when the cell starts");
SYSCTL_INT(_debug_qwz, OID_AUTO, moncoexpassive, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_moncoexpassive, 0, "K5 L1-C1: coex cell vdev-start passive flag: 0 = the K4 cell's rule (default), 1 = copy the station vdev's passive flag; read when the cell starts");
SYSCTL_INT(_debug_qwz, OID_AUTO, moncoexfilter, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_moncoexfilter, 0, "K5 L1-C1/C10: coex cell receive filter: 0 = never swap the station's filter (default, KEEP_FILTER forced), 1 = honour debug.qwz.monflags (swap, filter-pass plus monitor-other), 2 = swap with filter-pass only; read when the cell starts");
SYSCTL_INT(_debug_qwz, OID_AUTO, moncoexwbm, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_moncoexwbm, 1, "K6 K6-1: 1 = while a coex cell runs, the cell's peer-less data frames that the firmware releases through the WBM error ring are copied to the capture before they are dropped (default); 0 = dropped as in K5; no effect unless debug.qwz.moncoex is 1; read live");
SYSCTL_INT(_debug_qwz, OID_AUTO, moncoexscan, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_moncoexscan, 0, "K5 L1-C1/C11: 1 = while a coex cell runs, scans are answered from the node table and never reach the firmware (default); 0 = passed to the firmware; read live");
SYSCTL_INT(_debug_qwz, OID_AUTO, moncoexcred, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_moncoexcred, 0, "K5 L1-C1/C9: 1 = the coex cell never takes the last WMI credit, the station keeps it (default); 0 = no reservation; read live");
SYSCTL_INT(_debug_qwz, OID_AUTO, moncoexfw, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_moncoexfw, 0, "K5 L1-C1/C13: 0 = a coex cell stop does not mark the firmware for a restart (default); 1 = the old restart at the next qwz_init; read live");
SYSCTL_INT(_debug_qwz, OID_AUTO, moncoexnss, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_moncoexnss, 0, "K5 L1-C1/C3: 0 = the coex cell vdev's NSS comes from the chain count (default, as K4); 1 = NSS 1; read when the cell starts");
SYSCTL_INT(_debug_qwz, OID_AUTO, moncoexjoin, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_moncoexjoin, 0, "K5 L1-C1/C12: 0 = a join request is refused while a coex cell runs (default); 1 = the cell is stopped and the join proceeds; read live");
SYSCTL_INT(_debug_qwz, OID_AUTO, awdlcell, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_awdlcell, 0, "K5 L2-C2: reserved for the receive-only tee of the cell into the native AWDL engine; no code reads it yet (0 = default); only the K5 banner reports it");
SYSCTL_PROC(_debug_qwz, OID_AUTO, hwmode, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_hwmode, "I", "K2 H1: hardware mode id asked of the firmware at each start (single_pdev_only chips): 1 = DBS (default, the old request), 0 = SINGLE, 5 = DBS_OR_SBS, or any id the firmware offered; others EINVAL; a Wi-Fi off/on applies it");
SYSCTL_PROC(_debug_qwz, OID_AUTO, scanraw, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_scanraw, "I", "K1 W2c / K2c: 1 = log the non-zero 32-bit words of the scan request tail for the first three non-plain scan requests, 2 = also for the first three plain launched ones (a scan answered empty during the monitor hold counts as plain); 0 = off (default); any write re-arms the per-boot budgets (these words, the join-pick line, the IOC 85 blobs); may hold SSID text, redact before sharing");
SYSCTL_INT(_debug_qwz, OID_AUTO, monreap, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_monreap, 0, "K2c C2: 1 = capture TX ring: every 100 ms a slot older than 3 s is unloaded and freed (queued and in-flight counts corrected, 'qwz: mon: tx reap' logged for the first 8), and a send that would take a still-occupied slot reaps it when older than 250 ms or answers ENOSPC; 0 = off (default, as K2b)");
SYSCTL_INT(_debug_qwz, OID_AUTO, hintfix, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_hintfix, 0, "K2c C5: 1 = the scan-start command copies each hint BSSID from the request into the message (default); 0 = the old copy with source and destination swapped; nothing sets a hint BSSID today");
SYSCTL_INT(_debug_qwz, OID_AUTO, monfmt, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_monfmt, 0, "K4 M1: capture cell user frames with a per-frame tx_params block: 1 = frames under 150 bytes with retry limit 2 and OFDM 12 Mbit/s, 2 = also user DATA frames (retry limit 1 when the frame header says NO_ACK, else 4; rate from the header's rate byte, zero or unknown = OFDM 12); 0 = off (default, tx_params_valid 0); 3 and above act as 2");
SYSCTL_INT(_debug_qwz, OID_AUTO, rssisigned, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_rssisigned, 0, "K3 S1: 1 = the 5 GHz SCAN-state RSSI compare reads ni_rssi as a signed byte (strongest sample kept); 0 = today's compare (default)");
SYSCTL_INT(_debug_qwz, OID_AUTO, beaconmeta, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_beaconmeta, 0, "K3 S2: 1 = the beacon record sent to macOS also sets flags 6 (on channel), 7 (from beacon), 12 (noise) and 13 (SNR); 0 = flags 0x4002 (default)");
SYSCTL_INT(_debug_qwz, OID_AUTO, scanprobe, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_scanprobe, 0, "K3 S3: 1 = a macOS active scan with no SSID sends one zero-length SSID, 2 = also the broadcast-probe flag; 0 = off (default)");
SYSCTL_INT(_debug_qwz, OID_AUTO, rssisrc, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_rssisrc, 0, "K3 S4: 1 = data-frame RX RSSI no longer feeds the joined-AP RSSI average (management frames only); 0 = off (default)");
SYSCTL_INT(_debug_qwz, OID_AUTO, scanrest, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_scanrest, 0, "K3 S5a / K2c: N > 0 (ms, values above 500 act as 500) = min rest time N, max rest max(N, 50) for a macOS-requested scan in RUN state; 0 = 50/500 (default)");
SYSCTL_INT(_debug_qwz, OID_AUTO, rssilog, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_rssilog, 0, "K3: 1 = log the first 8 scan-result publications of the boot (raw ni_rssi byte, value sent, flags, no addresses); 0 = off (default)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, monmac, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_monmac, "A", "the capture vdev's MAC: the AWDL address for a node on this card");
SYSCTL_PROC(_debug_qwz, OID_AUTO, montxframe, CTLTYPE_OPAQUE | CTLFLAG_WR | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_montxframe, "S", "transmit a frame from userspace (qwz_montx_hdr + 802.11 frame)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, monpeer, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_monpeer, "A", "firmware peer on the capture vdev: 'add xx:xx:xx:xx:xx:xx' or 'del'");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdlif, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdlif, "A", "\"feth0\" or \"feth0=en3\" (delegate to the Wi-Fi interface too) marks it AWDL (IFEF_AWDL); \"-feth0\" clears");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdl0, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdl0, "A", "the kext's awdl0: write a MAC to create, \"down\" to destroy (qwz_awdl0.h)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdl0tx, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdl0tx, "S", "drain the frames the stack sent on awdl0");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdl0rx, CTLTYPE_OPAQUE | CTLFLAG_WR | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdl0rx, "S", "inject frames received on awdl0");
SYSCTL_PROC(_debug_qwz, OID_AUTO, montx, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_montx, "I", "monitor-mode transmit: 1 broadcast PSF copy once, 2 auto unicast reply, 4 unicast once");
SYSCTL_STRING(_debug_qwz, OID_AUTO, monbssid, CTLFLAG_RW | CTLFLAG_LOCKED,
    qwz_mon_bssid_str, sizeof(qwz_mon_bssid_str), "BSSID the monitor vdev comes up with (xx:xx:xx:xx:xx:xx; empty: own MAC)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, moncap, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_moncap, "S", "drain captured frames (scripts/qwz-moncap.py)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, start, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_start, "I", "write 1 to run the bring-up");
SYSCTL_INT(_debug_qwz, OID_AUTO, stage, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_stage_sysctl, 0, "stop the bring-up before this stage (-1: boot-time limit)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, publish, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_publish, "I", "write 1 to republish the log ring as QwzMacLog");
SYSCTL_PROC(_debug_qwz, OID_AUTO, logfile, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_logfile, "I", "M0: a non-zero write is refused (EPERM), the synchronous file sink froze this machine; 0 is accepted");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdl, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdl, "I", "1: create awdl0 (AirportWCN7850, once per boot), 2: its link up, 3: down; 20, 22, 26: M2a test of a peer cache add, delete, update op for a fixed test address");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdltest, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdltest, "I", "write n to run AWDL test n (1-5, 9 = 1-4); reads the AWDL frames counted");
SYSCTL_INT(_debug_qwz, OID_AUTO, he, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_he_sysctl, 0, "0: no 802.11ax for the next association (needs itlwm_he=1 to be on at all)");
/* M0 (C32 to C34): the runtime switches of docs/m0-spec.md section 4.5 and the entry points of the harness */
SYSCTL_INT(_debug_qwz, OID_AUTO, m0, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_m0_on, 0, "0: the sentinel stops enforcing, DEAD is never declared (legacy RDDM path), the watchdog skip is off");
SYSCTL_INT(_debug_qwz, OID_AUTO, deadaware, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_dead_aware, 0, "0: tsleep ignores DEAD");
SYSCTL_INT(_debug_qwz, OID_AUTO, mmioguard, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_mmio_guard, 0, "0: BAR reads and writes ignore the MMIO block");
SYSCTL_INT(_debug_qwz, OID_AUTO, bme, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_bme, 0, "0: no bus-master clear after a dead power-down, no command-bit re-assert at power-up");
SYSCTL_INT(_debug_qwz, OID_AUTO, softassert, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_softassert, 0, "0: soft asserts panic like KASSERT");
SYSCTL_PROC(_debug_qwz, OID_AUTO, recover, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_recover, "I", "0 legacy, 1 armed, write 2 re-arms");
SYSCTL_INT(_debug_qwz, OID_AUTO, silent, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_silent, 0, "silent firmware and persistent SYS_ERR: 0 off, 1 shadow (logs would-declare), 2 armed");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdlprobe, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdlprobe, "I", "AWDL data TX probe: 0 control, 1 mgmt path, 2 TCL plain Data, 3 TCL QoS; 1 to 3 need a proven recovery");
SYSCTL_INT(_debug_qwz, OID_AUTO, awdlforce, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_awdlforce, 0, "1: allow AWDL probes 1 to 3 and inject 7 without the recovery interlock (unsafe)");
SYSCTL_INT(_debug_qwz, OID_AUTO, capsae, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_capsae, 0, "getCARD_CAPABILITIES SAE experiment 0 to 3; reset to 0 after each cell");
SYSCTL_INT(_debug_qwz, OID_AUTO, cap73, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_cap73, 0, "getCARD_CAPABILITIES capability bit 73 (byte 9, 0x02: SSID-list scans reach the driver): 1 set (default), 0 the old blob; needs a Wi-Fi toggle or reboot to take effect");
SYSCTL_INT(_debug_qwz, OID_AUTO, inject_arg, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_inject_arg, 0, "argument of the next debug.qwz.inject");
SYSCTL_INT(_debug_qwz, OID_AUTO, proven, CTLFLAG_RD | CTLFLAG_LOCKED,
    &qwz_recovery_proven, 0, "1: a real crash (RDDM) was recovered in this boot (read-only)");
SYSCTL_INT(_debug_qwz, OID_AUTO, latesettle, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_late_settle, 0, "M0.1: ms of pause before the force wake and before the MHI reset of the late reset after a no-link teardown (the 1 Hz stream window gets the crumb first; the SoC reset has no pause); 0: no pauses; at most 3000 per pause (2000 with latemode 1), larger values are used as the limit; a pause ends at once on DEAD or a pending system sleep or stop");
SYSCTL_INT(_debug_qwz, OID_AUTO, latemode, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_late_mode, 0, "H1: late reset after a no-link teardown: 0 reads first (M0.1), 1 blind SoC reset first (posted writes, no read), then the same verification");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdlpeers, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdlpeers, "I", "M2a: AWDL peer hygiene bit mask, 0 = M0.1: 1 reset the heard-sender list at each listen start, 2 honour setPEER_CACHE_CONTROL ops (a delete creates no firmware peer), 4 the listen vdev takes awdl0's address");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdlhyg, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdlhyg, "I", "WP4: lane M hygiene and pacing: 0 off (M0.1), 1 own slot pool, sequence numbers, retry bit, pacing in outputStart (awdl0 Data needs awdlprobe=1)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdltx, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdltx, "I", "WP4: lane M preset: 0 M0.1, 1 own slot pool, 2 + MGMT_RATE 12 Mbit/s and tx_params presets, 3 + Data as OFFCHAN_DATA_TX_SEND; 2 and 3 need a proven recovery");
SYSCTL_INT(_debug_qwz, OID_AUTO, awdltxeff, CTLFLAG_RD | CTLFLAG_LOCKED,
    &qwz_awdltx_eff, 0, "WP4: the lane M preset in force (awdltx, lowered by a failure; read-only)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdlgate, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdlgate, "I", "WP4: lane M gate and requeue: 0 off, 1 peer-activity gate, 2 + shared-slot test (awdl0 Data needs awdlprobe=1)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdltxopt, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdltxopt, "I", "WP4: with awdltx >= 2, mask: 1 NON_AGG_SW_RETRY_TH 4, 2 BMISS counts raised");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdltruth, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdltruth, "I", "M2a: AWDL sync TLVs on the air: 0 a peer's copied (M0.1), 1 149 in every slot, 2 the engine's sequence projected to 149, 3 the followed peer's sequence, usable slots only");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdlclk, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdlclk, "I", "M2a: AWDL clock policy, bit mask: 1 AW sequence posted modulo 65536, 2 realign to the best heard peer every 20 s while master, 4 round tx_counter, 8 phy_tx_time + measured delay, 16 PSF as a follower every 440 TU");
SYSCTL_PROC(_debug_qwz, OID_AUTO, inject, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_inject, "I", "fault injection, modes 0 to 7 (argument: debug.qwz.inject_arg)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, state, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_state, "A", "engine and fault state");
SYSCTL_PROC(_debug_qwz, OID_AUTO, health, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_health, "I", "write 1 to log one health line now");
#ifdef QWZ_DEBUG
extern uint32_t qwz_debug;	/* qwz.c: QWZ_D_* mask from qwzvar.h */
SYSCTL_UINT(_debug_qwz, OID_AUTO, debug, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_debug, 0, "qwz debug mask (QWZ_D_* in qwzvar.h)");
#endif

void
qwz_sysctl_register(qwz_sysctl_start_fn start, void *arg)
{
	qwz_sysctl_start_cb = start;
	qwz_sysctl_start_arg = arg;
	if (qwz_sysctl_registered)
		return;
	sysctl_register_oid(&sysctl__debug_qwz);
	sysctl_register_oid(&sysctl__debug_qwz_start);
	sysctl_register_oid(&sysctl__debug_qwz_stage);
	sysctl_register_oid(&sysctl__debug_qwz_publish);
	sysctl_register_oid(&sysctl__debug_qwz_logfile);
	sysctl_register_oid(&sysctl__debug_qwz_he);
	sysctl_register_oid(&sysctl__debug_qwz_awdltest);
	sysctl_register_oid(&sysctl__debug_qwz_awdl);
	/* M0 (C32) */
	sysctl_register_oid(&sysctl__debug_qwz_m0);
	sysctl_register_oid(&sysctl__debug_qwz_deadaware);
	sysctl_register_oid(&sysctl__debug_qwz_mmioguard);
	sysctl_register_oid(&sysctl__debug_qwz_bme);
	sysctl_register_oid(&sysctl__debug_qwz_softassert);
	sysctl_register_oid(&sysctl__debug_qwz_recover);
	sysctl_register_oid(&sysctl__debug_qwz_silent);
	sysctl_register_oid(&sysctl__debug_qwz_awdlprobe);
	sysctl_register_oid(&sysctl__debug_qwz_awdlforce);
	sysctl_register_oid(&sysctl__debug_qwz_capsae);
	sysctl_register_oid(&sysctl__debug_qwz_cap73);
	sysctl_register_oid(&sysctl__debug_qwz_inject_arg);
	sysctl_register_oid(&sysctl__debug_qwz_proven);
	sysctl_register_oid(&sysctl__debug_qwz_latesettle);
	sysctl_register_oid(&sysctl__debug_qwz_latemode);
	/* WP4 (M2a) */
	sysctl_register_oid(&sysctl__debug_qwz_awdlhyg);
	sysctl_register_oid(&sysctl__debug_qwz_awdltx);
	sysctl_register_oid(&sysctl__debug_qwz_awdltxeff);
	sysctl_register_oid(&sysctl__debug_qwz_awdlgate);
	sysctl_register_oid(&sysctl__debug_qwz_awdltxopt);
	/* M2a WP2 */
	sysctl_register_oid(&sysctl__debug_qwz_awdlpeers);
	sysctl_register_oid(&sysctl__debug_qwz_awdltruth);
	sysctl_register_oid(&sysctl__debug_qwz_awdlclk);
	sysctl_register_oid(&sysctl__debug_qwz_inject);
	sysctl_register_oid(&sysctl__debug_qwz_state);
	sysctl_register_oid(&sysctl__debug_qwz_health);
	/* origin/awdl-m0-m1 (the capture cell and the kext-owned awdl0): registered after ours */
	sysctl_register_oid(&sysctl__debug_qwz_mon);
	sysctl_register_oid(&sysctl__debug_qwz_monflags);
	sysctl_register_oid(&sysctl__debug_qwz_monhold);
	sysctl_register_oid(&sysctl__debug_qwz_monnocred);
	sysctl_register_oid(&sysctl__debug_qwz_monqmax);
	sysctl_register_oid(&sysctl__debug_qwz_monparams);
	sysctl_register_oid(&sysctl__debug_qwz_hwmode);
	sysctl_register_oid(&sysctl__debug_qwz_fwsegs);
	sysctl_register_oid(&sysctl__debug_qwz_moncoex);
	sysctl_register_oid(&sysctl__debug_qwz_moncoexmode);
	sysctl_register_oid(&sysctl__debug_qwz_moncoexpassive);
	sysctl_register_oid(&sysctl__debug_qwz_moncoexfilter);
	sysctl_register_oid(&sysctl__debug_qwz_moncoexwbm);
	sysctl_register_oid(&sysctl__debug_qwz_moncoexscan);
	sysctl_register_oid(&sysctl__debug_qwz_moncoexcred);
	sysctl_register_oid(&sysctl__debug_qwz_moncoexfw);
	sysctl_register_oid(&sysctl__debug_qwz_moncoexnss);
	sysctl_register_oid(&sysctl__debug_qwz_moncoexjoin);
	sysctl_register_oid(&sysctl__debug_qwz_awdlcell);
	sysctl_register_oid(&sysctl__debug_qwz_scanraw);
	sysctl_register_oid(&sysctl__debug_qwz_monreap);
	sysctl_register_oid(&sysctl__debug_qwz_hintfix);
	sysctl_register_oid(&sysctl__debug_qwz_monfmt);
	sysctl_register_oid(&sysctl__debug_qwz_rssisigned);
	sysctl_register_oid(&sysctl__debug_qwz_beaconmeta);
	sysctl_register_oid(&sysctl__debug_qwz_scanprobe);
	sysctl_register_oid(&sysctl__debug_qwz_rssisrc);
	sysctl_register_oid(&sysctl__debug_qwz_scanrest);
	sysctl_register_oid(&sysctl__debug_qwz_rssilog);
	sysctl_register_oid(&sysctl__debug_qwz_monbssid);
	sysctl_register_oid(&sysctl__debug_qwz_montx);
	sysctl_register_oid(&sysctl__debug_qwz_monmac);
	sysctl_register_oid(&sysctl__debug_qwz_montxframe);
	sysctl_register_oid(&sysctl__debug_qwz_monpeer);
	sysctl_register_oid(&sysctl__debug_qwz_awdlif);
	sysctl_register_oid(&sysctl__debug_qwz_awdl0);
	sysctl_register_oid(&sysctl__debug_qwz_awdl0tx);
	sysctl_register_oid(&sysctl__debug_qwz_awdl0rx);
	sysctl_register_oid(&sysctl__debug_qwz_moncap);
#ifdef QWZ_DEBUG
	sysctl_register_oid(&sysctl__debug_qwz_debug);
#endif
	qwz_sysctl_registered = true;
}

void
qwz_sysctl_unregister(void)
{
	qwz_sysctl_start_cb = NULL;
	qwz_sysctl_start_arg = NULL;
	if (!qwz_sysctl_registered)
		return;
#ifdef QWZ_DEBUG
	sysctl_unregister_oid(&sysctl__debug_qwz_debug);
#endif
	/* origin/awdl-m0-m1 (the capture cell and the kext-owned awdl0): registered after ours, so unregistered before ours */
	sysctl_unregister_oid(&sysctl__debug_qwz_moncap);
	sysctl_unregister_oid(&sysctl__debug_qwz_monpeer);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdlif);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdl0);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdl0tx);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdl0rx);
	sysctl_unregister_oid(&sysctl__debug_qwz_montxframe);
	sysctl_unregister_oid(&sysctl__debug_qwz_monmac);
	sysctl_unregister_oid(&sysctl__debug_qwz_montx);
	sysctl_unregister_oid(&sysctl__debug_qwz_monbssid);
	sysctl_unregister_oid(&sysctl__debug_qwz_monflags);
	sysctl_unregister_oid(&sysctl__debug_qwz_monhold);
	sysctl_unregister_oid(&sysctl__debug_qwz_monnocred);
	sysctl_unregister_oid(&sysctl__debug_qwz_monqmax);
	sysctl_unregister_oid(&sysctl__debug_qwz_monparams);
	sysctl_unregister_oid(&sysctl__debug_qwz_hwmode);
	sysctl_unregister_oid(&sysctl__debug_qwz_fwsegs);
	sysctl_unregister_oid(&sysctl__debug_qwz_moncoex);
	sysctl_unregister_oid(&sysctl__debug_qwz_moncoexmode);
	sysctl_unregister_oid(&sysctl__debug_qwz_moncoexpassive);
	sysctl_unregister_oid(&sysctl__debug_qwz_moncoexfilter);
	sysctl_unregister_oid(&sysctl__debug_qwz_moncoexwbm);
	sysctl_unregister_oid(&sysctl__debug_qwz_moncoexscan);
	sysctl_unregister_oid(&sysctl__debug_qwz_moncoexcred);
	sysctl_unregister_oid(&sysctl__debug_qwz_moncoexfw);
	sysctl_unregister_oid(&sysctl__debug_qwz_moncoexnss);
	sysctl_unregister_oid(&sysctl__debug_qwz_moncoexjoin);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdlcell);
	sysctl_unregister_oid(&sysctl__debug_qwz_scanraw);
	sysctl_unregister_oid(&sysctl__debug_qwz_monreap);
	sysctl_unregister_oid(&sysctl__debug_qwz_hintfix);
	sysctl_unregister_oid(&sysctl__debug_qwz_monfmt);
	sysctl_unregister_oid(&sysctl__debug_qwz_rssisigned);
	sysctl_unregister_oid(&sysctl__debug_qwz_beaconmeta);
	sysctl_unregister_oid(&sysctl__debug_qwz_scanprobe);
	sysctl_unregister_oid(&sysctl__debug_qwz_rssisrc);
	sysctl_unregister_oid(&sysctl__debug_qwz_scanrest);
	sysctl_unregister_oid(&sysctl__debug_qwz_rssilog);
	sysctl_unregister_oid(&sysctl__debug_qwz_mon);
	/* M0 (C32), in the reverse order of the registration */
	sysctl_unregister_oid(&sysctl__debug_qwz_health);
	sysctl_unregister_oid(&sysctl__debug_qwz_state);
	sysctl_unregister_oid(&sysctl__debug_qwz_inject);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdlclk);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdltruth);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdlpeers);
	/* WP4 (M2a), registered after latemode, so unregistered before it */
	sysctl_unregister_oid(&sysctl__debug_qwz_awdltxopt);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdlgate);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdltxeff);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdltx);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdlhyg);
	sysctl_unregister_oid(&sysctl__debug_qwz_latemode);
	sysctl_unregister_oid(&sysctl__debug_qwz_latesettle);
	sysctl_unregister_oid(&sysctl__debug_qwz_proven);
	sysctl_unregister_oid(&sysctl__debug_qwz_inject_arg);
	sysctl_unregister_oid(&sysctl__debug_qwz_cap73);
	sysctl_unregister_oid(&sysctl__debug_qwz_capsae);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdlforce);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdlprobe);
	sysctl_unregister_oid(&sysctl__debug_qwz_silent);
	sysctl_unregister_oid(&sysctl__debug_qwz_recover);
	sysctl_unregister_oid(&sysctl__debug_qwz_softassert);
	sysctl_unregister_oid(&sysctl__debug_qwz_bme);
	sysctl_unregister_oid(&sysctl__debug_qwz_mmioguard);
	sysctl_unregister_oid(&sysctl__debug_qwz_deadaware);
	sysctl_unregister_oid(&sysctl__debug_qwz_m0);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdl);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdltest);
	sysctl_unregister_oid(&sysctl__debug_qwz_he);
	sysctl_unregister_oid(&sysctl__debug_qwz_logfile);
	sysctl_unregister_oid(&sysctl__debug_qwz_publish);
	sysctl_unregister_oid(&sysctl__debug_qwz_stage);
	sysctl_unregister_oid(&sysctl__debug_qwz_start);
	sysctl_unregister_oid(&sysctl__debug_qwz);
	qwz_sysctl_registered = false;
}

int
qwz_sysctl_logfile_on(void)
{
	return qwz_logfile_on;
}
