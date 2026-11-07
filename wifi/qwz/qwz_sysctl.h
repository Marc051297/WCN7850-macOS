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
