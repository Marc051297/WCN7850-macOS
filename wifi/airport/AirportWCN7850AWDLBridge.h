//
//  AirportWCN7850AWDLBridge.h
//  AirportWCN7850
//
//  What the Wi-Fi kext and the AWDL kext share. They do not link against
//  each other: the Wi-Fi kext allocates the AWDL interface by class name
//  (it exists once the AWDL kext is loaded by hand) and leaves its log
//  ring on itself as a property, which the AWDL interface reads in init().
//
//  The awdl0 discovery build, with the AWDL classes inside the Wi-Fi kext,
//  froze the Mac at boot; the class alone in its own kext loads cleanly.
//
//  Copyright (c) 2026. GPL-2.0-or-later (see LICENSE).
//

#ifndef AirportWCN7850AWDLBridge_h
#define AirportWCN7850AWDLBridge_h

#include <stdarg.h>
#include <stdint.h>

#define QWZ_AWDL_CLASS		"AirportWCN7850AWDLInterface"
#define QWZ_AWDL_KEXT		"com.razvanccr.AirportWCN7850AWDLProbe"
#define QWZ_AWDL_BRIDGE_KEY	"QwzAwdlBridge"	/* OSData on the controller: struct qwz_awdl_bridge */
#define QWZ_AWDL_BRIDGE_VERSION	5

struct qwz_awdl_bridge {
	uint32_t version;
	int	(*vlog)(const char *fmt, va_list ap);	/* the Wi-Fi kext's QwzMacLog ring */
	void	(*publish)(void);
	/*
	 * Version 2: queue a whole 802.11 AWDL action frame for transmission
	 * from the AWDL listen vdev (debug.qwz.awdltest=6). repeat 0: once;
	 * 1: it replaces the OOB auto request resent every 125 ms (a MIF); 2:
	 * the PSF resent every 110 TU while we are master. A repeat slot with no
	 * frame (len 0) stops. Any thread; it only copies and schedules.
	 * Returns an errno.
	 */
	int	(*tx)(void *ctx, const uint8_t *frame, size_t len, int repeat);
	void	*ctx;
	/*
	 * Version 3: the master the engine follows (setAWDL_SYNC_STATE). self:
	 * it is us. The Wi-Fi kext's AW clock then follows that master's sync
	 * parameters (or free-runs) and reports every availability window to
	 * the engine, as Broadcom's firmware does. Any thread.
	 */
	void	(*master)(void *ctx, const uint8_t *master, int self);
	/* Version 4: the engine's 16-slot channel sequence (channel, flags per slot); an AW start reports its slot's. */
	void	(*chanseq)(void *ctx, const uint32_t *channels, const uint32_t *flags);
	/*
	 * Version 5: a peer the engine caches (setPEER_CACHE_CONTROL): the Wi-Fi
	 * kext gives the firmware a peer for it, so its data frames are
	 * acknowledged and delivered to awdl0 and ours reach it. Any thread;
	 * it only queues the request (the firmware peer is made off the engine's
	 * thread). Returns an errno.
	 *
	 * M2a, version unchanged: add is 0 (nothing), 1 (a firmware peer, whatever
	 * the op was: the M0.1 AWDL kext, and the Wi-Fi kext's own calls) or
	 * QWZ_AWDL_PEER_OP + op, op being the u32 at +4 of the setPEER_CACHE_CONTROL
	 * buffer (0 add, 2 delete, 6 update). The Wi-Fi kext decides what an op
	 * does (debug.qwz.awdlpeers bit 2; this kext cannot read a sysctl) and
	 * treats the code like 1 when that bit is off, as an M0.1 Wi-Fi kext does.
	 */
	int	(*peer)(void *ctx, const uint8_t *mac, int add);
};

/* The add argument of peer() for a setPEER_CACHE_CONTROL op (M2a): QWZ_AWDL_PEER_OP + op. M0.1 passes only 0 and 1 and treats every other value like 1. */
#define QWZ_AWDL_PEER_OP	0x100

/*
 * M2a (still version 5: the struct above does not change). The AWDL kext uses two more values of tx's repeat argument:
 *  3: the PSF template while ANOTHER node is master, as the follower's frame (repeat slot 3). The Wi-Fi kext sends it
 *     every 440 TU only with debug.qwz.awdlclk bit 16, while the master PSF (slot 2) is off, and drops it otherwise;
 *     len 0 clears it.
 *  4: a setter record (struct qwz_awdl_setter_rec in frame, len = its size): the engine's sync parameters, extension
 *     counts, presence mode, master channels and action frame TX mode, which the Wi-Fi kext builds TLV 4 from.
 * A Wi-Fi kext from before M2a answers EINVAL to both (its range check comes first) and an AWDL kext from before M2a
 * never sends them, so the pair need not be replaced together; only the new features need both.
 */
#define QWZ_AWDL_TX_FOLLOWER_PSF	3
#define QWZ_AWDL_TX_SETTER		4
#define QWZ_AWDL_SETTER_MAGIC		0x52535741u	/* "AWSR", little endian */
struct qwz_awdl_setter_rec {
	uint32_t magic;
	uint32_t index;		/* the hook index of AirportWCN7850AWDLHooksImpl.inc: 29 SYNC_PARAMS, 31 EXTENSION_STATE_MACHINE_PARAMETERS, 36 MASTER_CHANNEL, 39 PRESENCE_MODE, 45 SECONDARY_MASTER_CHANNEL, 46 AF_TX_MODE */
	uint32_t v[6];		/* the words of the engine's buffer from byte 4 on (byte 0 is the version), zero where the buffer has none */
};

#endif /* AirportWCN7850AWDLBridge_h */
