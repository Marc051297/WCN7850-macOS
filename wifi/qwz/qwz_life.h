/*
 * qwz_life - the DEAD-state vocabulary and the runtime knobs of the M0
 * freeze-proofing build (docs/m0-spec.md, sections 4.1b and 4.5).
 *
 * A tiny header with no dependencies, included by qwz_compat.h, qwzvar.h,
 * qwz_log.cpp, qwz_sysctl.cpp and AirportItlwmV2.cpp. The knob variables are
 * DECLARED here and DEFINED in qwz_sysctl.cpp (C32): qwz_sysctl.h is not
 * included by the files that read them.
 *
 * Copyright (c) 2026. GPL-2.0-or-later (see LICENSE).
 */
#ifndef QWZ_LIFE_H
#define QWZ_LIFE_H
#define QWZ_AWDL_TX_MAX	3	/* the highest awdltx rung built (F3, Data as OFFCHAN_DATA_TX_SEND: spec-WP4.md section 6); WP4-01 defines 2 only if this is absent */
#define QWZ_LIFE_DEAD		0x00000001u	/* the firmware must not be touched; cleared in exactly one place (C06) */
#define QWZ_LIFE_RECOVERY	0x00000002u	/* a recovery episode is open */
#define QWZ_LIFE_GIVEN_UP	0x00000004u	/* the engine stopped trying */
#define QWZ_LIFE_MMIO_BLOCK	0x00000008u	/* BAR reads answer 0xffffffff, writes are dropped; cleared only right after a passed config-space link check (C08) */
enum qwz_dead_reason {
	QWZ_DEAD_NONE, QWZ_DEAD_RDDM, QWZ_DEAD_SYSERR, QWZ_DEAD_LINK, QWZ_DEAD_SILENT,
	QWZ_DEAD_DEADLINE, QWZ_DEAD_INJECT, QWZ_DEAD_USER, QWZ_DEAD_MAX
};
#define QWZ_INJ_WMI_STARVE	0x1u		/* C33: credits zeroed before the credit wait */
#define QWZ_INJ_IGNORE_EV	0x2u		/* C33: the firmware-event counter stands still */
#define QWZ_INJ_HOLD		0x4u		/* C33: the next power-up is held */
extern const char *const qwz_dead_names[];
/* the runtime knobs of section 4.5: defined in qwz_sysctl.cpp (C32) */
extern int qwz_m0_on, qwz_dead_aware, qwz_mmio_guard, qwz_bme, qwz_softassert, qwz_recover, qwz_silent,
    qwz_awdlprobe, qwz_awdlforce, qwz_capsae, qwz_inject_arg, qwz_recovery_proven, qwz_late_settle, qwz_late_mode;
/* S1 (hotspot Wi-Fi, docs/research-hotspot-wifi-2026-11-07.md): debug.qwz.cap73, default 1: getCARD_CAPABILITIES sets bit 73 (capabilities[9] 0x02); 0 = the old blob. Defined in qwz_sysctl.cpp */
extern int qwz_cap73;
/* K1 (docs/awdl-polish-plan-2026-11-07.md, W1 and W3), all default 0 = the build before K1; plain globals read at call time, defined in qwz_sysctl.cpp.
 * debug.qwz.monnocred 1: the user-space frame write answers ENOSPC (not EAGAIN) while both WMI credits are in flight and ENXIO while the firmware is down, crashed or a sleep is pending; K2c (C1): THE DEFAULT IS NOW 1, 0 restores EAGAIN;
 * debug.qwz.monqmax N > 0: DATA and CONTROL frames from user space get ENOSPC once N frames are queued in the capture TX ring (management frames keep all 63 slots);
 * debug.qwz.monparams 1: MGMT_RATE OFDM 12 Mbit/s on the cell vdev after it is up, 2: also MGMT_RETRY_LIMIT 3 and NON_AGG_SW_RETRY_TH 4 */
extern int qwz_monnocred;
extern unsigned int qwz_monqmax;
extern int qwz_monparams;
/* K2 (H1): debug.qwz.hwmode (and the qwzhwmode=N boot-arg), default 1 = WMI_HOST_HW_MODE_DBS = the build before K2: the hardware mode id asked of the firmware at each start when single_pdev_only. Accepted by the sysctl: 0, 1, 5 and any id in qwz_hwm_offered_mask (bit n = the firmware offered id n, n < 32, set from the extended service-ready event). Defined in qwz_sysctl.cpp */
extern int qwz_hwmode;
/* K5 (OBSD-2): debug.qwz.fwsegs (and the qwzfwsegs=N boot-arg, 0 to 2), default QWZ_FWSEGS_DEFAULT = 0 = K4: the firmware host memory is one 64 KB-aligned block; 1 = one block per firmware segment, each aligned to the next power of two of its size (single block if any allocation fails); 2 = dry run (allocate, log, free each segment, then as 0); any other value acts as 0. Read at each firmware start. Defined in qwz_sysctl.cpp */
extern int qwz_fwsegs;
/* K2: boot-arg qwzhwmguard=1 makes qwz_wmi_tlv_hw_mode_caps skip offered hardware mode ids outside the 7-entry priority table (an out-of-bounds read otherwise); default 0 = the old code path exactly. Defined in qwz_sysctl.cpp */
extern int qwz_hwm_guard;
extern unsigned int qwz_hwm_offered_mask;
/* K1 (W2c): debug.qwz.scanraw, default 0: 1 = log the non-zero words of the scan request tail for the first three non-plain scan requests, 2 = also for the first three plain launched ones. Defined in qwz_sysctl.cpp, read in airport/WCL/WCL.cpp */
extern int qwz_scanraw;
/* K2c: debug.qwz.monreap, default 0 = K2b: 1 = the capture TX ring's stale-slot reaper (3 s, every 100 ms) and live-slot guard (250 ms) in qwz_mon.c; debug.qwz.hintfix, default 1: the scan-start hint-BSSID copy goes params -> message, 0 = the old swapped copy (qwz.c). Defined in qwz_sysctl.cpp. qwz_scanraw_gen (K2c C6) is declared in qwz_log.h */
extern int qwz_monreap, qwz_hintfix;
/* K4 (M1): debug.qwz.monfmt, default 0 = tx_params_valid 0 for every cell frame: 1 = user frames under 150 bytes carry a tx_params block (retry limit 2, OFDM 12 Mbit/s); 2 = also user DATA frames (retry limit 1 with the header's NO_ACK flag, else 4; rate from the header's rate byte); 3 (offchan data command) is not implemented and acts as 2 */
extern int qwz_monfmt;
/* K3 (docs/research-scan-rssi-2026-11-06.md F1/F2/F3, K3 diagnosis), all default 0 = the build before K3; plain globals read at call time, defined in qwz_sysctl.cpp.
 * debug.qwz.rssisigned 1: the 5 GHz SCAN-state compare in ieee80211_input.c reads ni_rssi as a signed byte, so the strongest sample is kept (read in ieee80211_input.c);
 * debug.qwz.beaconmeta 1: the beacon record sent to macOS also sets the flags on channel (6), from beacon (7), noise (12) and SNR (13) (read in airport/WCL/WCL.cpp);
 * debug.qwz.scanprobe 1: a macOS active scan with no SSID sends one zero-length SSID, 2: also the broadcast-probe flag (read in qwz.c qwz_scan);
 * debug.qwz.rssisrc 1: data-frame RX RSSI no longer feeds the joined-AP average, beacons and other management frames only (read in qwz.c);
 * debug.qwz.scanrest N > 0: min rest time N ms (max rest max(N, 50) ms) for a macOS-requested scan in RUN state (read in qwz.c qwz_scan);
 * debug.qwz.rssilog 1: log the first 8 scan-result publications of the boot with the raw ni_rssi byte and the value sent (read in airport/WCL/WCL.cpp) */
extern int qwz_rssisigned, qwz_beaconmeta, qwz_scanprobe, qwz_rssisrc, qwz_scanrest, qwz_rssilog;
/* WP4 (M2a): lane M hardening (qwz_awdl.h), all default 0 = M0.1 behaviour. qwz_awdltx_eff is the rung in force: the configured qwz_awdltx, lowered at run time (three DISCARD or INSPECT completions in a row, or a DEAD with preset frames in flight) */
extern int qwz_awdlhyg, qwz_awdltx, qwz_awdltx_eff, qwz_awdlgate, qwz_awdltxopt;
#ifndef QWZ_AWDL_TX_MAX
#define QWZ_AWDL_TX_MAX	2	/* the highest awdltx rung built; the separate change F3 (spec-WP4.md section 6, WP4-40) defines it as 3 at the head of this file, above this #ifndef */
#endif
/*
 * M2a WP2 (docs/research-airdrop-2026-11-06.md, 4.4 and 4.5): AWDL peer hygiene. A bit mask, 0 = M0.1 exactly. debug.qwz.awdlpeers;
 * defined in qwz_sysctl.cpp, read in qwz.c and AirportItlwmV2.cpp.
 */
extern int qwz_awdlpeers;
#define QWZ_AWDLP_RESET		0x01	/* the host's list of senders given a firmware peer starts again whenever a listen starts (awdltest=6 joins or lifts a quiesce) */
#define QWZ_AWDLP_OPS		0x02	/* setPEER_CACHE_CONTROL: op 0 add and 6 update ensure a firmware peer; 2 delete and any other op create none */
#define QWZ_AWDLP_MAC		0x04	/* the listen vdev takes awdl0's address instead of deriving one from the current en1 address at every join */
#define QWZ_AWDLP_ALL		(QWZ_AWDLP_RESET | QWZ_AWDLP_OPS | QWZ_AWDLP_MAC)
/* HF-1, HF-3: 1 from the system sleep request (AirportItlwm::setPowerState) until the wake request, and from stop(): the late reset's diagnostic pauses end at once and stay off. Set and read with __atomic builtins; defined in qwz_sysctl.cpp */
extern volatile int qwz_pm_pending;

/* M2a WP5 (docs/research-airdrop-2026-11-06.md 4.5): debug.qwz.awdltruth (0 to 3) and debug.qwz.awdlclk (bit mask 1|2|4|8|16); both 0 is M0.1 on the air; defined in qwz_sysctl.cpp, read by AirportItlwm */
extern int qwz_awdltruth, qwz_awdlclk;
/* awdlclk 8: running mean (us, 0 until the first sample) of the submit-to-completion time of the AWDL action frames; written under the main gate by the completion handler, read without a lock (one aligned word) */
extern volatile unsigned int qwz_awdl_lat_us;
static inline void qwz_awdl_lat_note(unsigned long long lat_ns)
{
	unsigned int x = lat_ns > 6000000ULL ? 6000u : (unsigned int)(lat_ns / 1000ULL);
	unsigned int m = __atomic_load_n(&qwz_awdl_lat_us, __ATOMIC_RELAXED);

	if (x == 0)
		x = 1;
	__atomic_store_n(&qwz_awdl_lat_us, m == 0 ? x : (unsigned int)((int)m + ((int)x - (int)m) / 8), __ATOMIC_RELAXED);
}
/* DEAD may be declared, and the engine restarts the firmware, only while BOTH switches are on:
 * recover=0 and m0=0 each select the legacy RDDM handling verbatim (audit round 1, conflict 9) */
static inline int qwz_dead_armed(void) { return qwz_recover != 0 && qwz_m0_on != 0; }
#endif
