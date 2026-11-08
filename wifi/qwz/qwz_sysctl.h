/*
 * qwz_sysctl - the debug.qwz.* sysctls, registered by both kexts
 * (QwzMac.kext and AirportWCN7850.kext):
 *
 *   debug.qwz.publish=1   republish the log ring as QwzMacLog (scripts/qwz-log.sh)
 *   debug.qwz.logfile=1   refused (EPERM) since M0: a synchronous file write per line froze this
 *                         machine; 0 is accepted (the ring QwzMacLog is the only sink)
 *   debug.qwz.stage=<n>   stop the next bring-up before stage n (-1: boot-time limit)
 *   debug.qwz.start=1     rerun the bring-up; only where the product passes a callback
 *   debug.qwz.debug=<m>   qwz trace mask, debug builds (QWZ_D_* in qwzvar.h)
 *   debug.qwz.he=0        no 802.11ax for the next association (default 1; can only turn
 *                         HE off, it exists at all only with the itlwm_he=1 boot-arg)
 *   debug.qwz.awdl=1      create awdl0 (AirportWCN7850 discovery build; once per boot);
 *                         2 raises its link (macOS counts it as usable), 3 lowers it;
 *                         20, 22, 26 (M2a WP2): a peer cache add, delete, update op for a fixed test address
 *   debug.qwz.awdltest=<n> AWDL test n (1-5, 9 = baseline + 1-4; qwz_awdl_listen_test);
 *                         blocks until done, reads back the AWDL frames it counted
 *   M0 (docs/m0-spec.md sections 4.5 and C32 to C34):
 *   debug.qwz.m0, deadaware, mmioguard, bme, softassert, silent, awdlforce, capsae,
 *                         inject_arg = int switches; proven = int, read-only
 *   debug.qwz.cap73       S1: getCARD_CAPABILITIES bit 73 (capabilities[9] 0x02, SSID-list scans), default 1; 0 = the old blob; a Wi-Fi
 *                         toggle or reboot applies it
 *   debug.qwz.monnocred   K1 (W1): 1 = the user-space frame write (montxframe) answers ENOSPC, not EAGAIN, while both WMI credits are in flight, and ENXIO
 *                         while the firmware is down, crashed or a sleep is pending; default 1 since K2c (0 = EAGAIN, the K1/K2b default; plain global, read at call time)
 *   debug.qwz.monqmax     K1 (W1): N > 0 = DATA and CONTROL frames from user space are refused with ENOSPC once N frames are queued in the capture
 *                         TX ring (management frames keep all 63 slots); default 0 = off (plain global, read at call time)
 *   debug.qwz.monparams   K1 (W3): 1 = MGMT_RATE OFDM 12 Mbit/s on the cell vdev right after it is up, 2 = also MGMT_RETRY_LIMIT 3 and
 *                         NON_AGG_SW_RETRY_TH 4; default 0; read when the cell starts
 *   debug.qwz.hwmode      K2 (H1): the hardware mode id asked of the firmware at each start (single_pdev_only chips), default 1 = DBS = the build before K2;
 *                         0 = SINGLE, 5 = DBS_OR_SBS or any id the firmware offered, others EINVAL; read at every firmware start (a Wi-Fi off/on applies
 *                         it); the boot-arg qwzhwmode=N sets the value a boot starts with
 *   debug.qwz.fwsegs      K5 (OBSD-2): firmware host memory layout, 0 = one 64 KB-aligned block (default, K4), 1 = one block per firmware segment aligned to the
 *                         next power of two of its size (falls back to the single block on a failed allocation), 2 = dry run (allocate, log, free); read at each
 *                         firmware start; the boot-arg qwzfwsegs=N (0 to 2) sets the value a boot starts with; compile-time default QWZ_FWSEGS_DEFAULT
 *   debug.qwz.moncoex     K5 (L1-C1): 1 = a capture cell started while the station is associated on the same channel (6, 44 or 149) runs beside the station,
 *                         which is not stopped; 0 = the K4 cell (default); read when the cell starts. Sub-switches, all plain ints: moncoexmode (0 clone the
 *                         station's chandef, 1 HT20 on the primary), moncoexpassive (0 K4 rule, 1 the station's passive flag), moncoexfilter (0 never swap the
 *                         station's RX filter, 1 honour monflags, 2 swap filter-pass only), moncoexnss (0 chain count, 1 NSS 1), all read at cell start;
 *                         moncoexscan (1), moncoexcred (1), moncoexfw (0), moncoexjoin (0), read live; defaults in parentheses
 *   debug.qwz.awdlcell    K5 (L2-C2 placeholder): reserved for the receive-only tee of the cell into the native AWDL engine; nothing reads it yet (default 0); the
 *                         K5 banner reports it
 *   debug.qwz.scanraw     K1 (W2c): 1 = log the non-zero words of the scan request tail for the first three non-plain scan requests, 2 = also for the first
 *                         three plain launched ones; default 0; release builds too; the words may hold SSID text; K2c: any write re-arms the per-boot evidence budgets (raw words, join pick, IOC 85 blobs)
 *   debug.qwz.monreap    K2c (C2): 1 = the capture TX ring's stale-slot reaper (a slot older than 3 s, checked every 100 ms) and live-slot guard (an occupied slot is reaped when older than 250 ms, else ENOSPC); default 0 = K2b
 *   debug.qwz.hintfix    K2c (C5): 1 = the scan-start command copies hint BSSIDs the right way (default); 0 = the old swapped copy
 *   debug.qwz.monfmt     K4 (M1): capture cell frames with a tx_params block: 1 = user frames under 150 bytes (retry limit 2, OFDM 12 Mbit/s), 2 = also user DATA frames (retry 1 with NO_ACK, else 4; rate from the header's rate byte); default 0 = tx_params_valid 0
 *   debug.qwz.rssisigned  K3 (S1): 1 = the 5 GHz SCAN-state RSSI compare reads ni_rssi as a signed byte (strongest sample kept); default 0
 *   debug.qwz.beaconmeta  K3 (S2): 1 = the beacon record sent to macOS also sets flags 6, 7, 12 (noise -95) and 13 (SNR); default 0 (flags 0x4002)
 *   debug.qwz.scanprobe   K3 (S3): 1 = a macOS active scan with no SSID sends one zero-length SSID, 2 = also the broadcast-probe flag; default 0
 *   debug.qwz.rssisrc     K3 (S4): 1 = data-frame RX RSSI no longer feeds the joined-AP RSSI average (management frames only); default 0
 *   debug.qwz.scanrest    K3 (S5a): N > 0 = min rest time N ms (max max(N,50) ms) for a macOS-requested scan while associated; default 0 = 50/500
 *   debug.qwz.rssilog     K3: 1 = log the first 8 scan-result publications of the boot (raw ni_rssi byte, value sent, flags); default 0
 *   debug.qwz.recover     0 legacy crash handling, 1 armed, write 2 re-arms the engine
 *   debug.qwz.awdlprobe   AWDL data TX probe 0 to 3 (1 to 3 need a proven recovery)
 *   debug.qwz.awdlpeers   M2a WP2, AWDL peer hygiene, bit mask (QWZ_AWDLP_* in qwz_life.h), 0 = M0.1: 1 the heard-sender list starts again at
 *                         each listen start, 2 setPEER_CACHE_CONTROL ops are honoured (a delete creates no firmware peer), 4 the listen
 *                         vdev takes awdl0's address
 *   debug.qwz.inject=<n>  fault injection, modes 0 to 7 (argument: debug.qwz.inject_arg)
 *   debug.qwz.state       (read) engine and fault state;  debug.qwz.health=1 logs a health line
 *   awdl-m0-m1 (capture cell and kext-owned awdl0, merged from upstream; docs/airdrop-revisited.md): debug.qwz.mon (channel; 0 stops),
 *                         monflags (QWZ_MON_F_* in qwz_mon.h), monhold (Wi-Fi on keeps the cell), monbssid, montx (M1 transmit ops), monmac (read-only),
 *                         montxframe (a qwz_montx_hdr plus an 802.11 frame), monpeer ("add xx:.." or "del"), moncap (read-only capture stream),
 *                         awdlif ("feth0", "feth0=en3", "-feth0"), awdl0 (a MAC creates the kext's awdl0, "down" destroys it), awdl0tx (read-only)
 *                         and awdl0rx (write-only): upstream's awdltx and awdlrx under this fork's names, since debug.qwz.awdltx is the lane M preset
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#ifndef QWZ_SYSCTL_H
#define QWZ_SYSCTL_H

/* debug.qwz.start=1 calls this; false when a bring-up is already running. */
typedef bool (*qwz_sysctl_start_fn)(void *arg);

/* start may be NULL: debug.qwz.start then answers ENXIO. Registering twice is harmless. */
void	qwz_sysctl_register(qwz_sysctl_start_fn start, void *arg);
void	qwz_sysctl_unregister(void);
/* debug.qwz.awdl=1 calls this (AirportWCN7850 only): create awdl0; returns an errno. NULL: ENXIO. */
typedef int (*qwz_sysctl_awdl_fn)(void *arg, int value);
void	qwz_sysctl_set_awdl(qwz_sysctl_awdl_fn fn, void *arg);
/* debug.qwz.awdltest=<n> calls this: AWDL frames counted, or -errno. NULL: ENXIO. */
typedef int (*qwz_sysctl_awdltest_fn)(void *arg, int test);
void	qwz_sysctl_set_awdltest(qwz_sysctl_awdltest_fn fn, void *arg);
/* M0 (C32). The knob variables themselves are declared in qwz_life.h. */
/* debug.qwz.inject=<mode> calls this (ItlQwz::injectEntry): an errno. NULL: ENXIO. */
typedef int (*qwz_sysctl_inject_fn)(void *arg, int mode, int a);
void	qwz_sysctl_set_inject(qwz_sysctl_inject_fn fn, void *arg);	/* ItlQwz::injectEntry; NULL: ENXIO */
/* debug.qwz.state, debug.qwz.recover=2, debug.qwz.health=1 call this (AirportItlwm::m0Sysctl). */
enum { QWZ_M0_STATE = 0, QWZ_M0_RECOVER = 1, QWZ_M0_HEALTH = 2 };
typedef int (*qwz_sysctl_m0_fn)(void *arg, int what, int val, char *buf, size_t len);
void	qwz_sysctl_set_m0(qwz_sysctl_m0_fn fn, void *arg);		/* AirportItlwm::m0Sysctl; NULL: ENXIO */
/* The capture cell's softc-facing nodes (debug.qwz.mon, montx, montxframe, monpeer, monmac, moncap): set_mon(NULL) makes them answer ENXIO (ItlQwz::detachGated); any other owner opens them. */
void	qwz_sysctl_set_mon(void *owner);
/* 1 while debug.qwz.logfile keeps the file sink open (so a bring-up must not close it). */
int	qwz_sysctl_logfile_on(void);

#endif /* QWZ_SYSCTL_H */
