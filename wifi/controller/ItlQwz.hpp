/*
 * ItlQwz - itlwm HAL for the Qualcomm WCN7850, driving OpenBSD's qwz(4).
 *
 * The controller (itlwm.cpp) talks to this class through ItlHalService; this
 * class owns the qwz softc and runs every call into qwz holding the
 * workloop's command gate, which stands in for OpenBSD's kernel lock.
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#ifndef ItlQwz_hpp
#define ItlQwz_hpp

#include <HAL/ItlHalService.hpp>
#include <HAL/ItlDriverInfo.hpp>
#include <HAL/ItlDriverController.hpp>
#include "qwz_awdl.h"	/* WP4 (M2a): lane M */

struct qwz_softc;

class ItlQwz : public ItlHalService, ItlDriverInfo, ItlDriverController {
	OSDeclareDefaultStructors(ItlQwz)

public:
	static bool qwz_match(IOPCIDevice *device);

	/* ItlHalService */
	bool attach(IOPCIDevice *device) override;
	void detach(IOPCIDevice *device) override;
	IOReturn enable(IONetworkInterface *netif) override;
	IOReturn disable(IONetworkInterface *netif) override;
	struct ieee80211com *get80211Controller() override;
	ItlDriverInfo *getDriverInfo() override;
	ItlDriverController *getDriverController() override;
	void free() override;

	/* ItlDriverInfo */
	const char *getFirmwareVersion() override;
	int16_t getBSSNoise() override;
	int16_t getChannelNoise(uint32_t frequency) override;
	int getChannelBusy(uint32_t frequency) override;
	bool is5GBandSupport() override;
	int getTxNSS() override;
	const char *getFirmwareName() override;
	UInt32 supportedFeatures() override;
	const char *getFirmwareCountryCode() override;
	uint32_t getTxQueueSize() override;
	uint32_t getRxRateKbps() override;
	int getRxRateMcs() override;
	int getRxRateNss() override;
	int getRxRateSgi() override;
	int8_t getBSSRssi(int8_t raw) override;

	/* ItlDriverController */
	void clearScanningFlags() override;
	bool initWithController(IOEthernetController *controller, IOWorkLoop *workloop, IOCommandGate *commandGate) override;
	void shutdownHardware() override;
	static IOReturn shutdownGated(OSObject *, void *, void *, void *, void *);
	void sleepQuiesce(uint32_t maxMs) override;
	bool firmwareUp() override;
	void sleepHardware() override;
	bool wakeNeedsRecovery() override;
	IOReturn wakeHardware() override;
	void wakeComplete(bool ok) override;
	bool dataStalled(bool *healthy) override;
	void requestBusReset() override;
	/* M0 (C18): fault state and engine plumbing; AirportItlwm's sentinel and engine (C14, C15) call these */
	void setFaultHandler(void (*cb)(void *, uint32_t), void *arg) override;
	bool faultPending(uint32_t *reason) override;
	void declareDead(uint32_t reason, const char *detail) override;
	const char *deadName() override;
	bool healthSnapshot(bool print) override;
	bool linkAlive() override;
	void releaseWaiters() override;
	void awdlQuiesce(bool on) override;
	void noteRecovered(uint32_t reason) override;
	bool recoveryOpen(uint32_t *reason, uint32_t *episode, uint64_t *sinceMs) override;
	void setEngineState(const char *state, uint32_t fails, bool givenUp) override;
	IOReturn setMulticastList(IOEthernetAddress *addr, int count) override;
#ifdef AIRPORT_WCL
	IOReturn setScanHomeAwayTime(uint32_t ms) override;
#endif
	/* AirportWCN7850's awdl0: where qwz_rx_observe_mgmt sends AWDL action frames (NULL: nowhere). */
	void setAwdlRx(void (*fn)(void *, const uint8_t *, size_t, int, uint32_t, uint32_t, uint32_t), void *arg);
	/* One AWDL action frame out of the AWDL listen vdev (qwz_awdl_tx, under the gate); an errno. */
	int awdlTx(const uint8_t *frame, size_t len);
	/* awdl0's data path: where data frames with the AWDL BSSID go, a firmware peer, one data frame out. */
	void setAwdlDataRx(void (*fn)(void *, const uint8_t *, size_t, int), void *arg);
	int awdlPeerAdd(const uint8_t *mac);
	/* M2a WP2: awdl0's address for the listen vdev (any thread, no gate, no wait); the generation of the AWDL peer set (read under the gate). */
	void setAwdlAddress(const uint8_t *mac);
	uint32_t awdlPeerGeneration();
	int awdlDataTx(const uint8_t *frame, size_t len);
	/* WP4 (M2a): awdl0's Data frame with AirportItlwm's handle for it (0: none), the completions handed back, and the lane's state without the gate. */
	int awdlDataTxEx(const uint8_t *frame, size_t len, uint32_t cookie);
	void setAwdlTxDone(qwz_awdl_txdone_fn fn, void *arg);
	void awdlLane(struct qwz_awdl_lane *li);

private:
	static IOReturn attachGated(OSObject *owner, void *arg0, void *, void *, void *);
	static IOReturn detachGated(OSObject *owner, void *, void *, void *, void *);
	static IOReturn enableGated(OSObject *owner, void *, void *, void *, void *);
	static IOReturn disableGated(OSObject *owner, void *, void *, void *, void *);
	static IOReturn sleepQuiesceGated(OSObject *owner, void *, void *, void *, void *);
	static IOReturn stallCheckGated(OSObject *owner, void *, void *, void *, void *);
	/* M0 (C18): the sentinel's gate probe body (attemptAction on the main gate): stamps sc_gate_ok_ns, optionally prints the health line. */
	static IOReturn healthGated(OSObject *owner, void *, void *, void *, void *);
	/* debug.qwz.awdltest: qwz_awdl_listen_test under the gate, from the sysctl thread. */
	static int awdlTestEntry(void *arg, int test);
	static IOReturn awdlTestGated(OSObject *owner, void *, void *, void *, void *);
	/* debug.qwz.inject (M0, C33): qwz_inject under the gate, from the sysctl thread. */
	static int injectEntry(void *arg, int mode, int a);
	static IOReturn injectGated(OSObject *owner, void *, void *, void *, void *);
	static IOReturn awdlTxGated(OSObject *owner, void *, void *, void *, void *);
	static IOReturn awdlPeerAddGated(OSObject *owner, void *, void *, void *, void *);
	static IOReturn awdlDataTxGated(OSObject *owner, void *, void *, void *, void *);
	static IOReturn awdlDataTxExGated(OSObject *owner, void *, void *, void *, void *);

	/*
	 * The softc exists from initWithController() on, so get80211Controller()
	 * is always valid (the native Wi-Fi layer sets ic_event_handler before
	 * attach); attached says whether the hardware is behind it.
	 */
	struct qwz_softc *sc;
	bool attached;
	bool shutdown;		/* shutdownHardware() ran: the chip is already off */
	char fwVersion[64];
	/* M0 (C18): the engine's last reported state ("UP", "RESTARTING", "RETRY", "GIVEN_UP") and failure count, set by setEngineState */
	const char *engineStateName;
	uint32_t engineFails;

	/* System sleep/wake: PCI config saved at sleep, restored on wake. */
	IOPCIDevice *pciDev;
	bool wakeNeeded;	/* the chip was powered down for sleep: full re-init required */
	struct {
		bool valid;
		uint32_t id;
		uint16_t cmd;
		uint32_t cls;		/* config 0x0c */
		uint32_t bar[2];	/* BAR0 (64-bit memory BAR) */
		uint8_t msiOff, expOff, pmOff;
		uint16_t msiCtl, msiData, devCtl;
		uint32_t msiLo, msiHi;
	} saved;
	IOPCIDevice *parentBridge(IOService **bus);
	bool soleDeviceBehind(IOService *bus);
	void saveConfig();

	/* Sleep ordering: peer delete failures counted when the sleep began (sleepQuiesce). */
	uint32_t sleepFailBase;
	bool sleepBaseValid;
	/* The teardown before the last sleep left a peer behind or failed to delete one. */
	bool dirtyTeardown;
	bool busResetNext;	/* requestBusReset(): wakeHardware() does an SBR even though the device answers */

	/*
	 * Data-stall watch (dataStalled): RUN time without a single data frame,
	 * added up over consecutive associations, and the management frames that
	 * arrived since the episode began. A data frame shows the data path works
	 * and ends the episode; a new firmware (wakeComplete) starts it afresh.
	 */
	struct {
		bool valid;		/* data/mgmt/lastMs hold a sample */
		bool inRun;		/* the last sample was in RUN */
		bool periodData;	/* a data frame arrived in this RUN period */
		bool logged;		/* the detection was logged for this episode */
		uint32_t data, mgmt;	/* sc_probe_data_seen / sc_probe_mgmt_seen at the last sample */
		uint32_t runMs;		/* RUN time without a data frame */
		uint32_t mgmtIn;	/* management frames received in this episode */
		uint64_t lastMs;
		uint64_t lastRunMs;	/* last time in RUN (or the first sample): evidence older than a minute is dropped */
		uint32_t healthyMs;	/* RUN time with data seen, consecutive */
	} stall;
};

#endif /* ItlQwz_hpp */
