//
//  AirportItlwmV2.hpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef AirportItlwmV2_hpp
#define AirportItlwmV2_hpp

#include "Apple80211.h"
#ifdef AIRPORT_WCL
#if __IO80211_TARGET == __MAC_15_2
#include <Airport/macOS15/FaultReporter.h>
#else
#include <Airport/macOS26/FaultReporter.h>
#endif
#endif

#include "IOKit/network/IOGatedOutputQueue.h"
#include <libkern/c++/OSString.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <IOKit/IOFilterInterruptEventSource.h>

#include "ItlQwz.hpp"

/* Intel's RSSI floor; the MediaTek HAL keeps ni_rssi the same way (dBm + 100). */
#ifndef IWM_MIN_DBM
#define IWM_MIN_DBM -100
#endif

#include "AirportItlwmEthernetInterface.hpp"
/* awdl0's class is in the AWDL kext (AirportWCN7850AWDLBridge.h); this kext only calls Apple's base. */
#include "macOS26/IO80211VirtualInterface.h"
#include "AirportWCN7850AWDLBridge.h"
/* F1 (native AWDL): defines ITL_AWDL_NATIVE for Tahoe WCL builds (from ForbiddenItlwm). */
#include <ItlAWDLTargets.hpp>

enum
{
    kPowerStateOff = 0,
    kPowerStateOn,
    kPowerStateCount
};

/* Who asked for a wake recovery; logged as "recovery run from ...". */
enum
{
    kWakeViaPmThread = 0,
    kWakeViaSetPower,
    kWakeViaEnable,
    kWakeViaRetry,
    kWakeViaStall,
    kWakeViaDeferred,
    kWakeViaDead,		/* M0 (C15): the engine's restart after a DEAD episode */
    kWakeViaCount
};

/* M0 (C15): the engine's request bits (engineRequest); kEngRecover restarts a chip that is down after failed recoveries (debug.qwz.recover=2, C34) */
enum { kEngDead = 1, kEngRecover = 2 };

#define kWatchDogTimerPeriod 1000
#ifdef AIRPORT_WCL
#define AIRPORT_LEGACY_OVERRIDE
#else
#define AIRPORT_LEGACY_OVERRIDE override
#endif

extern "C" {
const char *convertApple80211IOCTLToString(signed int cmd);
}

class AirportItlwm : public IO80211Controller {
    OSDeclareDefaultStructors(AirportItlwm)
#define IOCTL(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCGA80211) { \
ret = get##REQ(interface, (struct DATA_TYPE* )data); \
} else { \
ret = set##REQ(interface, (struct DATA_TYPE* )data); \
}
    
#define IOCTL_GET(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCGA80211) { \
ret = get##REQ(interface, (struct DATA_TYPE* )data); \
}
#define IOCTL_SET(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCSA80211) { \
ret = set##REQ(interface, (struct DATA_TYPE* )data); \
}
#define FUNC_IOCTL(REQ, DATA_TYPE) \
FUNC_IOCTL_GET(REQ, DATA_TYPE) \
FUNC_IOCTL_SET(REQ, DATA_TYPE)
#define FUNC_IOCTL_GET(REQ, DATA_TYPE) \
IOReturn get##REQ(OSObject *object, struct DATA_TYPE *data);
#define FUNC_IOCTL_SET(REQ, DATA_TYPE) \
IOReturn set##REQ(OSObject *object, struct DATA_TYPE *data);
    
public:
    virtual bool init(OSDictionary *properties) override;
    virtual void free() override;
    virtual IOService* probe(IOService* provider, SInt32* score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void systemWillShutdown(IOOptionBits specifier) override;
    virtual IOReturn enable(IO80211SkywalkInterface *netif) AIRPORT_LEGACY_OVERRIDE;
    virtual IOReturn disable(IO80211SkywalkInterface *netif) AIRPORT_LEGACY_OVERRIDE;
    virtual IOReturn setHardwareAddress(const void *addr, UInt32 addrBytes) override;
    virtual IOReturn getHardwareAddress(IOEthernetAddress* addrP) override;
    virtual IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const override;
    virtual IOReturn setPromiscuousMode(IOEnetPromiscuousMode mode) override;
    virtual IOReturn setMulticastMode(IOEnetMulticastMode mode) override;
    virtual IOReturn setMulticastList(IOEthernetAddress* addr, UInt32 len) override;
    virtual UInt32 getFeatures() const override;
    virtual const OSString * newVendorString() const override;
    virtual const OSString * newModelString() const override;
    virtual IOReturn selectMedium(const IONetworkMedium *medium) override;
    virtual bool createWorkQueue() override;
    virtual IONetworkInterface * createInterface() override;
    virtual bool configureInterface(IONetworkInterface *netif) override;
    virtual UInt32 outputPacket(mbuf_t, void * param) override;
#ifdef __PRIVATE_SPI__
    virtual IOReturn outputStart(IONetworkInterface *interface, IOOptionBits options) override;
    virtual IOReturn networkInterfaceNotification(
                        IONetworkInterface * interface,
                        uint32_t              type,
                        void *                  argument ) override;
#endif
    virtual bool setLinkStatus(
                               UInt32                  status,
                               const IONetworkMedium * activeMedium = 0,
                               UInt64                  speed        = 0,
                               OSData *                data         = 0) override;
    static IOReturn setLinkStateGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);
    
    static IOReturn tsleepHandler(OSObject* owner, void* arg0 = 0, void* arg1 = 0, void* arg2 = 0, void* arg3 = 0);
    static void eventHandler(struct ieee80211com *, int, void *);
    /* While a wake recovery is pending this runs it (PCI, then the full firmware init) instead of a bare enable. */
    IOReturn enableAdapter(IONetworkInterface *netif, int via = kWakeViaEnable);
    IOReturn enableAdapterNow(IONetworkInterface *netif);
    static IOReturn enableAdapterGated(OSObject *owner, void *netif, void *via, void *, void *);
    void disableAdapter(IONetworkInterface *netif);
    bool initCCLogs();
    
    #ifdef AIRPORT_WCL
    virtual IO80211WorkQueue *getWorkQueue() const override;
    virtual CCLogStream *getLogger() const override { return driverLogger; }
    CCLogStream *driverLogger;
#else
    virtual IO80211WorkQueue *getWorkQueue() override;
#endif
    virtual bool requiresExplicitMBufRelease() override {
        return false;
    }
    virtual bool flowIdSupported() override {
        return false;
    }
    virtual SInt32 monitorModeSetEnabled(bool, UInt) override {
        return kIOReturnSuccess;
    }
    virtual IOReturn requestQueueSizeAndTimeout(unsigned short *queue, unsigned short *timeout) override {
        XYLog("%s\n", __FUNCTION__);
        return kIOReturnSuccess;
    }
    
    virtual bool getLogPipes(CCPipe**, CCPipe**, CCPipe**) override;
    
#ifdef AIRPORT_WCL
    virtual CommonFaultReporter *getFaultReporterFromDriver() override;
#else
    virtual void *getFaultReporterFromDriver() override;
#endif
    
    virtual SInt32 apple80211_ioctl(IO80211SkywalkInterface *,unsigned long,void *, bool, bool) AIRPORT_LEGACY_OVERRIDE;
    virtual SInt32 apple80211SkywalkRequest(UInt,int,IO80211SkywalkInterface *,void *) AIRPORT_LEGACY_OVERRIDE;
    virtual SInt32 apple80211SkywalkRequest(UInt,int,IO80211SkywalkInterface *,void *,void *) AIRPORT_LEGACY_OVERRIDE;

    bool createMediumTables(const IONetworkMedium **primary);
    void releaseAll();
    /*
     * macOS (Apple80211's _isInfraInterface, used by airportd and the installer)
     * only takes an interface whose registry entry has IO80211InterfaceRole =
     * Infrastructure. IO80211SkywalkInterface::setInterfaceRole() sets it, yet it
     * was gone after start; these log where it disappears and put it back.
     */
    void checkInterfaceRole(const char *when, bool restore);
    static void roleTimerFired(OSObject *owner, IOTimerEventSource *sender);
    IOTimerEventSource *roleTimer;
    uint32_t roleChecks;
    void watchdogAction(IOTimerEventSource *timer);

    /* AWDL virtual interface — override to create awdl0 */
    virtual IO80211VirtualInterface *createVirtualInterface(ether_addr *ether, UInt role) override;

    virtual SInt32 enableFeature(IO80211FeatureCode, void*) override;
    /* Every request the family does not answer itself passes here first; the first of each id is logged. */
    virtual bool isCommandProhibited(int command) override;
    virtual SInt32 handleCardSpecific(IO80211SkywalkInterface *,unsigned long,void *,bool) override {
        XYLog("%s\n", __FUNCTION__);
        return 0;
    };
    virtual IOReturn getDRIVER_VERSION(IO80211SkywalkInterface *interface,apple80211_version_data *data) override {
        XYLog("%s\n", __FUNCTION__);
        return getDRIVER_VERSION((OSObject *)interface, data);
    };
    virtual IOReturn getHARDWARE_VERSION(IO80211SkywalkInterface *interface,apple80211_version_data *data) override {
        XYLog("%s\n", __FUNCTION__);
        return getHARDWARE_VERSION((OSObject *)interface, data);
    };
    virtual IOReturn getCARD_CAPABILITIES(IO80211SkywalkInterface *interface,apple80211_capability_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return getCARD_CAPABILITIES((OSObject *)interface, data);
    }
    virtual IOReturn getPOWER(IO80211SkywalkInterface *interface,apple80211_power_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return getPOWER((OSObject *)interface, data);
    }
    virtual IOReturn setPOWER(IO80211SkywalkInterface *interface,apple80211_power_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return setPOWER((OSObject *)interface, data);
    }
    virtual IOReturn getCOUNTRY_CODE(IO80211SkywalkInterface *interface,apple80211_country_code_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return getCOUNTRY_CODE((OSObject *)interface, data);
    }
    virtual IOReturn setCOUNTRY_CODE(IO80211SkywalkInterface *interface,apple80211_country_code_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return setCOUNTRY_CODE((OSObject *)interface, data);
    }
    virtual IOReturn setGET_DEBUG_INFO(IO80211SkywalkInterface *interface,apple80211_debug_command *data) override {
        XYLog("%s\n", __FUNCTION__);
        return kIOReturnSuccess;
    }
    
    //scan
    static void fakeScanDone(OSObject *owner, IOTimerEventSource *sender);
    
    //-----------------------------------------------------------------------
    // Power management support.
    //-----------------------------------------------------------------------
    virtual IOReturn registerWithPolicyMaker( IOService * policyMaker ) override;
    virtual IOReturn setPowerState( unsigned long powerStateOrdinal,
                                    IOService *   policyMaker) override;
    virtual IOReturn setWakeOnMagicPacket( bool active ) override;
    void setPowerStateOff(void);
    void setPowerStateOn(void);
    void unregistPM();
    bool initPCIPowerManagment(IOPCIDevice *provider);

    FUNC_IOCTL_GET(CARD_CAPABILITIES, apple80211_capability_data)
    FUNC_IOCTL(POWER, apple80211_power_data)
    FUNC_IOCTL_GET(DRIVER_VERSION, apple80211_version_data)
    FUNC_IOCTL_GET(HARDWARE_VERSION, apple80211_version_data)
    FUNC_IOCTL(COUNTRY_CODE, apple80211_country_code_data)
    
public:
    IOInterruptEventSource* fInterrupt;
    IOTimerEventSource *watchdogTimer;
    IOPCIDevice *pciNub;
    IONetworkStats *fpNetStats;
    AirportItlwmEthernetInterface *bsdInterface;
    #ifdef AIRPORT_WCL
    IO80211InfraInterface *fNetIf;
#else
    IO80211SkywalkInterface *fNetIf;
#endif
    IOWorkLoop *fWatchdogWorkLoop;
    ItlHalService *fHalService;
    
    //IO80211
    uint8_t power_state;
    struct ieee80211_node *fNextNodeToSend;
    bool fScanResultWrapping;
    IOTimerEventSource *scanSource;
    
    u_int32_t current_authtype_lower;
    u_int32_t current_authtype_upper;
    UInt64 currentSpeed;
    UInt32 currentStatus;
    bool disassocIsVoluntary;
    char geo_location_cc[3];
    
    //pm
    thread_call_t powerOnThreadCall;
    thread_call_t powerOffThreadCall;
    thread_call_t wakeRetryCall;
    thread_call_t stallCall;
    thread_call_t deadCall, radioOffCall;	/* M0 (C15, C16): the dead-firmware engine, the Wi-Fi-off hop off the workloop thread */
    bool noWakeFix;
    bool noRadioReset;  /* -qwznoradioreset: Wi-Fi off/on only stops and starts the interface */
    bool noStallFix;  /* -qwznostallfix: no data-stall restart */
    bool stallSbr;  /* -qwzstallsbr: the second data-stall restart also resets the PCIe link */
    /*
     * A chip power-down or wake recovery is running. Both sleep with the
     * gate released (the firmware stop and load wait for interrupts), so a
     * second thread must wait for the first, never run beside it.
     */
    bool chipBusy;
    /*
     * M0 (C13, C14): who holds the chip, since when and with which limit; written by chipClaim BEFORE chipBusy is
     * published (release store), read by the sentinel after chipBusy (acquire). sinceMs and limitMs are arguments of
     * the claim: the power-state thread passes the time of its power change request (S19), 0 means "now"/default.
     */
    volatile uint64_t chipBusySince;
    const char *volatile chipBusyWho;
    volatile uint32_t chipBusyLimitMs;
    volatile bool deadlineFired, chipBusyPm;	/* chipBusyPm: the clock started at the power change request (C13) */
    volatile uint64_t pmAckStartMs;		/* time of the last setPowerState call (C13) */
    bool chipClaim(const char *who, uint32_t maxMs, uint64_t sinceMs = 0, uint32_t limitMs = 0);
    bool chipWait(const char *who, uint32_t maxMs);
    void chipRelease();
    /* M0 (C14): the sentinel, a gate-free timer on fWatchdogWorkLoop, independent of the adapter, never takes the gate */
    IOTimerEventSource *sentinelTimer;
    volatile bool sentinelStopping;	/* M0 (C14, R1-03): stopSentinel() began; sentinelFired neither ticks nor re-arms. NOT pmStopping: the sentinel keeps its deadlines while stop() waits inside quiescePM */
    uint32_t gateBusyTicks, linkBadTicks;
    uint64_t sentinelPubMs, sentinelHealthMs, lastEpisodeMs;
    void sentinelTick();
    void stopSentinel();
    static void sentinelFired(OSObject *owner, IOTimerEventSource *sender);
    /* M0 (C15): the dead-firmware engine v0 (single flight by engineBusy, limiter, GIVEN_UP, re-arm) */
    volatile uint32_t engineReq, engineReason;
    uint32_t engReason;			/* reason of the first attempt of the open episode: retries reuse it (the DEAD flag is clear by then) */
    bool engineBusy, engGivenUp, engDeferred;
    uint32_t engFailCont, engEpisodes;
    unsigned int engAbsorbed, engChipBusy, engClosed;	/* rate limiters of the single-flight logs (engClosed: the "recovered by" line, M2a WP0) */
    uint64_t engCooldownMs, engLastOkMs, engBeginMs;
    void engineRequest(uint32_t bits, uint32_t reason);
    void engineRun();
    void engineNoteFailure(const char *why);
    void engineGiveUp(const char *why);
    void engineRearm(const char *who);
    void scheduleEngine(uint32_t ms);
    static void faultCb(void *arg, uint32_t reason);
    static IOReturn engineGated(OSObject *owner, void *, void *, void *, void *);
    /* M0 (C16): setPOWER(0) on the workloop thread queues disableAdapter on a thread that may sleep */
    void scheduleRadioOff();
    static IOReturn radioOffDisableGated(OSObject *owner, void *, void *, void *, void *);
    /* M0 (C34): debug.qwz.state, recover=2, health=1 (qwz_sysctl_m0_fn) */
    static int m0Sysctl(void *arg, int what, int val, char *buf, size_t len);
    const char *wakeLabel;  /* what the next recovery is called in the log */
    bool chipIsDown;  /* a chip power-down completed and no recovery has started since */
    bool chipUsed;  /* the interface was enabled since the firmware was last (re)started */
    volatile bool sleepPending;  /* setPowerState(Off) arrived and the wake has not: no stall restarts */
    bool pmStopping;  /* stop() began: the thread calls do nothing more */
    uint32_t wakeRetryCount;  /* failed recoveries in this chain; the back-off index */
    int wakeRetryVia;  /* what the queued thread call runs the recovery as */
    bool chipDown(const char *why, const char *wakeName, uint32_t maxWaitMs, uint64_t budgetStartMs);
    bool wakeRecover(int via);
    void wakeCloseEpisode(int via, uint64_t chipMs);	/* M2a WP0: a chip-up other than the engine's succeeded: closes the open recovery episode */
    bool wakeAllowed();
    bool onWorkLoopThread();
    bool pmReady();
    void pmAcknowledge();
    void quiescePM();
    void scheduleWakeRetry(uint32_t ms, int via);
    void cancelWakeRetry();
    static IOReturn wakeRetryGated(OSObject *owner, void *, void *, void *, void *);
    static IOReturn radioOffGated(OSObject *owner, void *, void *, void *, void *);
    /* Data-stall restart: the watchdog polls, a thread call restarts the firmware. */
    bool stallPending;  /* a restart is queued or running */
    bool stallNoted;  /* why no restart is made was logged for this stall */
    uint32_t stallRestarts;  /* restarts in this wake/boot window */
    uint64_t stallLastMs;  /* when the last one began */
    void checkDataStall();
    void stallRestart();
    void cancelStallRestart();
    static IOReturn stallRestartGated(OSObject *owner, void *, void *, void *, void *);
    UInt32 pmPowerState;
    IOService *pmPolicyMaker;
    UInt8 pmPCICapPtr;
    bool magicPacketEnabled;
    bool magicPacketSupported;
    
    //AWDL
    uint8_t *syncFrameTemplate;
    uint32_t syncFrameTemplateLength;
    uint8_t awdlBSSID[6];
    uint32_t awdlSyncState;
    uint32_t awdlElectionId;
    uint32_t awdlPresenceMode;
    uint16_t awdlMasterChannel;
    uint16_t awdlSecondaryMasterChannel;
    uint8_t *roamProfile;
    struct apple80211_btc_profiles_data *btcProfile;
    struct apple80211_btc_config_data btcConfig;
    uint32_t btcMode;
    uint32_t btcOptions;
    bool awdlSyncEnable;

    /*
     * awdl0 (debug.qwz.awdl=1, discovery build): the IO80211AWDLProtocol
     * object Tahoe's AWDL engine drives, and its BSD interface. Created once,
     * on request, never torn down before a reboot.
     */
    int awdlCreate();
    /*
     * F1 (native AWDL, docs/forbiddenitlwm-port-map-2026-11-13.md): Apple's AWDL engine on a QwzAWDLInterface inside
     * this kext, with real Skywalk pools and queues, behind -qwzawdlnative. Ported from ForbiddenItlwm
     * (airport/awdlnative/QwzAWDLController.cpp). Mutually exclusive with awdlCreate() above.
     */
    IO80211VirtualInterface *awdlNativeCreateVirtualInterface(ether_addr *ether, UInt role);
    IOReturn receiveAWDLActionFrame(mbuf_t, size_t, uint32_t, int32_t);
    void observeAWDLDataFrame(const uint8_t*, size_t);
    IOReturn buildAWDLSyncProbe(mbuf_t*, uint64_t);
    IOReturn buildAWDLTransmit(mbuf_t*, const uint8_t *holdPayloadFor = nullptr, bool holdEverySize = false);
    bool serviceAWDLTransmit();
    unsigned queuedAWDLTransmit();
    unsigned heldShortAWDLTransmit();
    void teardownAWDLInterface();
    IO80211VirtualInterface *fAWDLInterface;
    bool awdlInterfaceTest;		/* -qwzawdlnative and the build guard agreed: the native path is on */
    bool awdlExtendFrames;		/* -itlawdlelicit */
    bool awdlHideInfra;			/* -itlawdlhideinfra */
    uint16_t awdlProbeSequence;
    bool awdlManagementInputEnabled;
    UInt32 awdlManagementActive;
    UInt32 awdlCreateRequests;
    /* F2-1 (native AWDL radio glue, airport/awdlnative/QwzAWDLRadio.cpp): the cell is the radio; a gated timer sends. */
    void awdlNativeRadioStart();
    void awdlNativeRadioStop();
    static void awdlNativeRadioRx(void *arg, const uint8_t *frame, size_t len, uint16_t flags, uint8_t chan, int8_t rssi);
    void awdlNativeRadioInput(const uint8_t *frame, size_t len, uint16_t flags, uint8_t chan, int8_t rssi);
    static void awdlNativeTimerFired(OSObject *owner, IOTimerEventSource *sender);
    IOTimerEventSource *awdlNativeTimer;
    uint8_t *awdlNativeScratch;
    uint64_t awdlNativeLastSyncUS, awdlNativeLastDataUS, awdlNativeLastPublishUS;
    uint64_t awdlNativeLastRxBytes, awdlNativeLastAckedBytes;
    int awdlNativeRssiFallback;		/* itlawdlrssi=N (dBm) stands in when the capture path has no RSSI; default -60 */
    struct {
        uint64_t timerTicks, cellTimerTicks;
        uint64_t rxAction, rxActionHanded, rxActionRefused, rxActionTrimmed, rxActionTooLong;
        uint64_t rxData, rxDataHanded, rxDataTooLong, rxNotInGate, rxNoMbuf, rssiFallbacks;
        uint64_t syncSent, syncBuildFailures, syncTxErrors, dataSent, dataTxErrors, queued;
        uint64_t rxDataBytes, txSubmittedBytes;
        uint32_t rxActionLastError, syncLastBuild, syncLastTxError, dataLastTxError;
    } awdlNativeStats;
    static int awdlSysctl(void *arg, int value);
    static IOReturn awdlLinkGated(OSObject *target, void *arg0, void *, void *, void *);
    /* AWDL action frames from qwz (ItlQwz::setAwdlRx) into the AWDL engine, while awdl0's link is up. */
    static void awdlRx(void *arg, const uint8_t *frame, size_t len, int rssi, uint32_t freq,
                       uint32_t chan, uint32_t rate);
    static IOReturn awdlRxGated(OSObject *target, void *arg0, void *, void *, void *);
    bool awdlLinkUp;
    uint32_t awdlRxDelivered;
    uint32_t awdlRxDropped;
    /*
     * AWDL frames from the AWDL engine (the bridge's tx): queued on any thread,
     * sent from the workloop by awdlTxTimer; the latest OOB auto request is
     * resent every 125 ms.
     */
    static int awdlTxEnqueue(void *ctx, const uint8_t *frame, size_t len, int repeat);
    static void awdlTxTimerFired(OSObject *owner, IOTimerEventSource *sender);
    void awdlTxSend(uint8_t *frame, size_t len);
    struct AwdlTxQueue *awdlTxq;
    IOLock *awdlTxLock;
    IOTimerEventSource *awdlTxTimer;
    bool awdlTxStopped;		/* stop() ran: the engine may still call, nothing is sent */
    /*
     * The AW clock: APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START/_EXTENSIONS_END
     * to the engine every availability window, aligned to the master's sync
     * parameters while we follow one (awdlRxGated), free-running as master.
     */
    static void awdlMaster(void *ctx, const uint8_t *master, int self);
    static void awdlChannelSequence(void *ctx, const uint32_t *channels, const uint32_t *flags);
    /* awdl0's data path: the engine's cached peers get firmware peers; data frames with the AWDL BSSID become awdl0 packets. */
    static int awdlPeerCb(void *ctx, const uint8_t *mac, int add);
    /*
     * The engine calls the peer hook holding its lock; the firmware peer
     * needs the driver's gate, which the AW timer holds while posting into
     * the engine. So the hook only queues, and this thread call creates.
     */
    static void awdlPeerCallFired(thread_call_param_t p0, thread_call_param_t p1);
    thread_call_t awdlPeerCall;
    uint8_t awdlPeerSeen[8][6];	/* senders of AWDL frames given a firmware peer */
    int awdlPeersSeen;
    uint32_t awdlPeerGen;	/* M2a WP2: ItlQwz::awdlPeerGeneration() that the list above belongs to */
    uint32_t awdlPeerOps[4];	/* M2a WP2: setPEER_CACHE_CONTROL ops that reached awdlPeerCb: add, update, delete, other (counted with the switch off too) */
    uint8_t awdlPeerQueue[8][6];
    uint32_t awdlPeerQHead, awdlPeerQTail;
    static void awdlDataRx(void *arg, const uint8_t *frame, size_t len, int rssi);
    uint8_t awdlMac[6];		/* awdl0's address: the listen vdev's */
    uint32_t awdlDataRxCount;
    uint32_t awdlDataTxCount;
    uint32_t awdlDataTxFail;
    uint16_t awdlDataSeq;
    uint32_t awdlAwChan[16];	/* the engine's channel sequence: what each AW start reports */
    uint32_t awdlAwChanFlags[16];
    static void awdlAwTimerFired(OSObject *owner, IOTimerEventSource *sender);
    void awdlAwFollow(const uint8_t *frame, size_t len, uint64_t rxNs);
    void awdlAwStart(bool on);
    IOTimerEventSource *awdlAwTimer;
    bool awdlAwOn;
    bool awdlAwInWindow;	/* the next timer event is the window's end */
    bool awdlAwSelfMaster;
    uint8_t awdlAwMaster[6];
    uint32_t awdlAwSeq;		/* the next window's sequence number */
    uint64_t awdlAwNextNs;	/* when it starts */
    uint64_t awdlAwStartUs;	/* the last start's firmware time (host us) */
    uint64_t awdlAwPeriodNs;
    uint32_t awdlAwStarts;
    uint32_t awdlAwResyncs;
    /* A peer's sync parameters (type 4) and channel sequence (0x12) TLVs, whole, inserted into our frames. */
    uint8_t awdlSyncTlv[192];
    uint16_t awdlSyncTlvLen;
    uint8_t awdlChanTlv[192];
    uint16_t awdlChanTlvLen;
    uint32_t awdlTxTlvLogged;
    uint32_t awdlRxTlvLogged;
    uint32_t awdlSyncLogged;
    uint32_t awdlTxSent;
    uint32_t awdlTxFailed;
    uint32_t awdlTxDropped;

    /*
     * M2a WP5 (truthful sync, docs/research-airdrop-2026-11-06.md 3.5 Y1 to Y3). Idle with debug.qwz.awdltruth and
     * debug.qwz.awdlclk at 0 (the setters are stored either way).
     */
    struct AwdlSetters {		/* the engine's sync setters, from the AWDL kext (bridge tx, repeat 4) */
        uint32_t valid;			/* bit 0 SYNC_PARAMS, 1 EXTENSION_STATE_MACHINE_PARAMETERS, 2 MASTER_CHANNEL, 3 PRESENCE_MODE, 4 SECONDARY_MASTER_CHANNEL, 5 AF_TX_MODE */
        uint32_t awLen, awPeriod, extLen, afPeriod;	/* setAWDL_SYNC_PARAMS */
        uint32_t extCnt[4];		/* setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS */
        uint32_t masterChan, presence, secChan, afTxMode;
    } awdlSet;				/* guarded by awdlTxLock */
    /* awdlAfTxMode (setAWDL_AF_TX_MODE as the engine last set it) and awdlAfSuppressed() are WP4's members (further down, with the gate's); awdlSetterIn case 46 stores into them */
    uint8_t awdlMasterSeq[38];		/* awdltruth 3: the followed master's channel sequence structure */
    uint8_t awdlMasterSeqMac[6];	/* the master it was heard from: it counts only while that is still the master the engine follows */
    uint8_t awdlMasterSeqLen;		/* 0: none */
    uint64_t awdlMasterSeqNs;		/* when it was last heard */
    uint8_t awdlBestMac[6];		/* awdlclk 2: the clock reference while the engine says we are master */
    bool awdlBestRoot;
    uint64_t awdlBestNs, awdlClkAlignNs;
    uint32_t awdlAwPosted;		/* the last AW start sequence number posted to the engine */
    uint32_t awdlTruthHash;		/* the hash of the static part of the TLVs last logged */
    uint32_t awdlTruthBuilt, awdlTruthRefused, awdlRealigns, awdlTxFollower;
    unsigned int awdlTruthLog, awdlTruthRefuseLog, awdlTruthTplLog, awdlRealignLog, awdlWrapLog, awdlFollowerLog, awdlSetLog, awdlRefLog, awdlCfgLog, awdlClkSampleLog;
    int awdlCfgTruth, awdlCfgClk;	/* the switches as awdlTxSend last saw them */
    uint32_t awdlClkSample;		/* clock sample lines still to log after a switch change */
    int awdlSetterIn(const uint8_t *rec, size_t len);
    bool awdlTruthBuild(uint8_t *out, size_t outSize, const uint8_t *frame, size_t len, uint64_t ns, size_t *outLen);
    bool awdlTruthRefuse(const char *why);
    const char *awdlSyncNote(const uint8_t *frame, size_t len, uint64_t rxNs);
    uint32_t awdlTxCounterTu(uint64_t ns);
    uint32_t awdlPhyDelayUs();
    IO80211VirtualInterface *awdlIf;	/* an AirportWCN7850AWDLInterface, from the AWDL kext */
    AirportItlwmEthernetInterface *awdlBsd;
    bool creatingAwdlBsd;	/* createInterface()/configureInterface() are for awdl0 */
    bool awdlTried;
    uint32_t awdlDropped;	/* packets the stack queued on awdl0: no AWDL data path yet */
    /*
     * WP4 (M2a): awdl0's data lane: pacing (debug.qwz.awdlhyg), the gate and the requeue (debug.qwz.awdlgate). The gate's state is
     * touched only inside the command gate; awdlLaneHasRoom and awdlGateSubmit run on awdl0's output thread.
     */
    struct AwdlGate *awdlGate;
    IOTimerEventSource *awdlGateTimer;
    uint64_t awdlGateAtNs;			/* when the pending gate run is due; 0: none */
    volatile uint32_t awdlOutBlocked;		/* outputStart stopped for lack of room: the next completion starts the output thread again */
    bool awdlGateStopped;			/* stop() ran */
    bool awdlGateOn() const { return qwz_awdlgate != 0 && qwz_awdlprobe == 1 && awdlGate != NULL && awdlGateTimer != NULL && !awdlGateStopped; }
    bool awdlPaced() const { return qwz_awdlhyg != 0 || qwz_awdlgate != 0; }
    static void awdlTxDone(void *arg, const struct qwz_awdl_txdone *d);	/* ItlQwz::setAwdlTxDone: a lane M completion */
    static void awdlGateTimerFired(OSObject *owner, IOTimerEventSource *sender);
    static IOReturn awdlGateSubmitGated(OSObject *target, void *arg0, void *, void *, void *);
    static IOReturn awdlGateObserveGated(OSObject *target, void *arg0, void *, void *, void *);
    int awdlGateSubmit(const uint8_t *frame, size_t len);
    int awdlGateEnqueue(const uint8_t *frame, size_t len);
    void awdlGateRun();
    bool awdlGateSync(struct qwz_awdl_lane *li);
    bool awdlGateEligible(struct AwdlGEntry *e, uint64_t ns, int *why);
    int awdlGateRelease(struct AwdlGEntry *e, uint64_t ns, int why);
    void awdlGateRequeue(struct AwdlGEntry *e, uint64_t ns);
    void awdlGateDrop(struct AwdlGEntry *e, uint32_t *counter, const char *why);
    void awdlGateFlush(const char *why);
    struct AwdlGPeer *awdlGatePeer(const uint8_t *mac, bool create);
    void awdlGateObserve(const uint8_t *frame, size_t len, uint64_t ns);
    void awdlGateArm(uint64_t atNs);
    void awdlGateLink(bool up);
    void awdlGateLog();
    bool awdlLaneHasRoom();
    void awdlKickOutput();
    bool awdlSlotAt(uint64_t ns, uint32_t *slot, uint64_t *phaseNs);
    bool awdlServedSlot(uint32_t slot);
    /*
     * awdlgate 2 and AF_TX_MODE: the engine's setAWDL_AF_TX_MODE (3 suppress action frames, 0 allow). WP5 stores the value (awdlSetterIn, case 46);
     * WP4 consumes it in awdlTxSend. Without WP5 nothing stores it, it stays 0 and nothing is held back.
     */
    volatile uint32_t awdlAfTxMode;
    bool awdlAfSuppressed() const { return awdlAfTxMode == 3; }
    uint32_t awdlAfHeld;			/* engine action frames not sent because of it */
    uint32_t awdlAfLog;

    CCPipe *driverLogPipe;
    CCPipe *driverDataPathPipe;
    CCPipe *driverSnapshotsPipe;
    
#ifdef AIRPORT_WCL
    bool driverLogPipeStarted = false;
    bool driverDataPathPipeStarted = false;
    bool driverSnapshotsPipeStarted = false;
    CCStream *driverFaultStream;
    CCFaultReporter *driverCoreFaultReporter;
    IO80211FaultReporter *driverFaultReporter;
#else
    CCStream *driverFaultReporter;
#endif
};

#endif /* AirportItlwmV2_hpp */
