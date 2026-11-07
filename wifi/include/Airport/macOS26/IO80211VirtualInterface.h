//
//  IO80211VirtualInterface.h (macOS 26)
//  AirportWCN7850
//
//  Generated from IO80211Family 1200.13.1 (macOS 26.7.1, 25G241): the
//  IO80211AWDLProtocol and IO80211VirtualInterface vtables in the boot kernel
//  collection. Every slot is declared in order: the overrides of
//  IO80211SkywalkInterface slots so a subclass inherits this class's
//  implementations, then this class's own slots 461-504.
//
#ifndef IO80211VirtualInterface_macOS26_h
#define IO80211VirtualInterface_macOS26_h

class IO80211Controller;
struct apple80211_debug_command;
struct peerSlotDataStats;
struct packetCounters_t;

class IO80211VirtualInterface : public IO80211SkywalkInterface {
    OSDeclareAbstractStructors(IO80211VirtualInterface)
public:
    /* IO80211SkywalkInterface slots this class overrides */
    virtual void free() APPLE_KEXT_OVERRIDE;	/* slot 18 */
    virtual IOReturn configureReport(IOReportChannelList*, unsigned int, void*, void*) APPLE_KEXT_OVERRIDE;	/* slot 133 */
    virtual IOReturn updateReport(IOReportChannelList*, unsigned int, void*, void*) APPLE_KEXT_OVERRIDE;	/* slot 134 */
    virtual bool start(IOService*) APPLE_KEXT_OVERRIDE;	/* slot 184 */
    virtual void stop(IOService*) APPLE_KEXT_OVERRIDE;	/* slot 185 */
    virtual bool terminate(IOOptionBits options = 0) APPLE_KEXT_OVERRIDE;	/* slot 192 */
    virtual bool attach(IOService * provider) APPLE_KEXT_OVERRIDE;	/* slot 206 */
    virtual void detach(IOService * provider) APPLE_KEXT_OVERRIDE;	/* slot 207 */
    virtual IOReturn newUserClient(task*, void*, unsigned int, OSDictionary*, IOUserClient**) APPLE_KEXT_OVERRIDE;	/* slot 238 */
    virtual const char* stringFromReturn(int) APPLE_KEXT_OVERRIDE;	/* slot 240 */
    virtual IOReturn enable(unsigned int) APPLE_KEXT_OVERRIDE;	/* slot 266 */
    virtual IOReturn prepareBSDInterface(struct __ifnet*, unsigned int) APPLE_KEXT_OVERRIDE;	/* slot 285 */
    virtual IOReturn processBSDCommand(struct __ifnet*, unsigned int, void*) APPLE_KEXT_OVERRIDE;	/* slot 290 */
    virtual SInt32 setInterfaceEnable(bool) APPLE_KEXT_OVERRIDE;	/* slot 294 */
    virtual UInt32 getFeatureFlags() APPLE_KEXT_OVERRIDE;	/* slot 311 */
    virtual bool bpfTap(unsigned int, unsigned int) APPLE_KEXT_OVERRIDE;	/* slot 316 */
    virtual bool setMulticastAddresses(ether_addr const*, unsigned int) APPLE_KEXT_OVERRIDE;	/* slot 337 */
    virtual void * createPeerManager() APPLE_KEXT_OVERRIDE;	/* slot 353 */
    virtual void postMessage(unsigned int, void*, unsigned long, bool) APPLE_KEXT_OVERRIDE;	/* slot 355 */
    virtual void logTxPacket(IO80211NetworkPacket*, PacketSkywalkScratch*, apple80211_wme_ac, bool) APPLE_KEXT_OVERRIDE;	/* slot 359 */
    virtual void logTxCompletionPacket(IO80211NetworkPacket*, PacketSkywalkScratch*, unsigned char*, apple80211_wme_ac, int, unsigned int, bool, bool) APPLE_KEXT_OVERRIDE;	/* slot 360 */
    virtual IOReturn inputPacket(IO80211NetworkPacket*, packet_info_tag*, ether_header*, bool*, bool) APPLE_KEXT_OVERRIDE;	/* slot 362 */
    virtual void logSkywalkTxReqPacket(IO80211NetworkPacket*, PacketSkywalkScratch*, unsigned char*, apple80211_wme_ac, bool) APPLE_KEXT_OVERRIDE;	/* slot 364 */
    virtual SInt64 pendingPackets(unsigned char) APPLE_KEXT_OVERRIDE;	/* slot 365 */
    virtual SInt64 packetSpace(unsigned char) APPLE_KEXT_OVERRIDE;	/* slot 366 */
    virtual IO80211LinkState linkState() APPLE_KEXT_OVERRIDE;	/* slot 369 */
    virtual void setScanningState(unsigned int, bool, apple80211_scan_data*, int) APPLE_KEXT_OVERRIDE;	/* slot 370 */
    virtual void updateLinkParameters(apple80211_interface_availability*) APPLE_KEXT_OVERRIDE;	/* slot 374 */
    virtual void updateInterfaceCoexRiskPct(unsigned long long) APPLE_KEXT_OVERRIDE;	/* slot 375 */
    virtual void setInterfaceNF(apple80211_channel, long long) APPLE_KEXT_OVERRIDE;	/* slot 381 */
    virtual void setDebugFlags(unsigned long long, unsigned int) APPLE_KEXT_OVERRIDE;	/* slot 384 */
    virtual SInt64 debugFlags() APPLE_KEXT_OVERRIDE;	/* slot 385 */
    virtual void setInterfaceChipCounters(apple80211_stat_report*, apple80211_chip_counters_tx*, apple80211_chip_error_counters_tx*, apple80211_chip_counters_rx*) APPLE_KEXT_OVERRIDE;	/* slot 386 */
    virtual void setInterfaceMIBdot11(apple80211_stat_report*, apple80211_ManagementInformationBasedot11_counters*) APPLE_KEXT_OVERRIDE;	/* slot 387 */
    virtual void setFrameStats(apple80211_stat_report*, apple80211_frame_counters*) APPLE_KEXT_OVERRIDE;	/* slot 388 */
    virtual void setInfraSpecificFrameStats(apple80211_stat_report*, apple80211_infra_specific_stats*) APPLE_KEXT_OVERRIDE;	/* slot 389 */
    virtual SInt64 getWmeTxCounters(unsigned long long*) APPLE_KEXT_OVERRIDE;	/* slot 391 */
    virtual void setWoWEnabled(bool) APPLE_KEXT_OVERRIDE;	/* slot 393 */
    virtual bool wowEnabled() APPLE_KEXT_OVERRIDE;	/* slot 394 */
    virtual void printDataPath(userPrintCtx*) APPLE_KEXT_OVERRIDE;	/* slot 395 */
private: virtual void _abi_0c68() __asm__("__ZN23IO80211VirtualInterface21findOrCreateFlowQueueE20IO80211FlowQueueHash"); public:	/* slot 397 */
private: virtual void _abi_0c70() __asm__("__ZN23IO80211VirtualInterface30findOrCreateFlowQueueWithCacheE20IO80211FlowQueueHashPb"); public:	/* slot 398 */
private: virtual void _abi_0c78() __asm__("__ZN23IO80211VirtualInterface21findExistingFlowQueueE20IO80211FlowQueueHash"); public:	/* slot 399 */
private: virtual void _abi_0c80() __asm__("__ZN23IO80211VirtualInterface17removePacketQueueEPK20IO80211FlowQueueHash"); public:	/* slot 400 */
    virtual void flushPacketQueues() APPLE_KEXT_OVERRIDE;	/* slot 401 */
    virtual void cachePeer(ether_addr*, unsigned int*) APPLE_KEXT_OVERRIDE;	/* slot 402 */
    virtual bool shouldLog(unsigned long long) APPLE_KEXT_OVERRIDE;	/* slot 403 */
    virtual void vlogDebug(unsigned long long, char const*, va_list) APPLE_KEXT_OVERRIDE;	/* slot 404 */
    virtual void vlogDebugBPF(unsigned long long, char const*, va_list) APPLE_KEXT_OVERRIDE;	/* slot 405 */
    virtual void * getP2PSkywalkPeerMgr() APPLE_KEXT_OVERRIDE;	/* slot 408 */
private: virtual void _abi_0cd0() __asm__("__ZN23IO80211VirtualInterface8findPeerER10ether_addr"); public:	/* slot 410 */
    virtual int getEventPipeSize() APPLE_KEXT_OVERRIDE;	/* slot 439 */
    virtual UInt64 createEventPipe(IO80211APIUserClient*) APPLE_KEXT_OVERRIDE;	/* slot 440 */
    /* this class's own slots */
    virtual bool init(IO80211Controller *, ether_addr *, unsigned int, char const *);	/* slot 461 */
    virtual void setLinkState(IO80211LinkState, unsigned int);	/* slot 462 */
private: virtual void _vi_463() __asm__("__ZN23IO80211VirtualInterface13forwardPacketEP20IO80211NetworkPacket"); public:	/* slot 463 */
private: virtual void _vi_464() __asm__("__ZN23IO80211VirtualInterface11handleIoctlEP7__ifnetmPv"); public:	/* slot 464 */
private: virtual void _vi_465() __asm__("__ZN23IO80211VirtualInterface14addPeerToCacheEPh"); public:	/* slot 465 */
private: virtual void _vi_466() __asm__("__ZN23IO80211VirtualInterface19deletePeerFromCacheEPh"); public:	/* slot 466 */
private: virtual void _vi_467() __asm__("__ZN23IO80211VirtualInterface14handleDebugCmdEP24apple80211_debug_command"); public:	/* slot 467 */
private: virtual void _vi_468() __asm__("__ZN23IO80211VirtualInterface20getPeerSlotDataStatsEP10ether_addrbP17peerSlotDataStats"); public:	/* slot 468 */
private: virtual void _vi_469() __asm__("__ZN23IO80211VirtualInterface16getPeerDataStatsEP10ether_addrbP16packetCounters_t"); public:	/* slot 469 */
private: virtual void _vi_470() __asm__("__ZN23IO80211VirtualInterface18clearPeerDataStatsEP10ether_addrb"); public:	/* slot 470 */
private: virtual void _vi_471() __asm__("__ZN23IO80211VirtualInterface34setAwdlCurrentChannelSequenceIndexEj"); public:	/* slot 471 */
private: virtual void _vi_472() __asm__("__ZN23IO80211VirtualInterface22isOutputFlowControlledEv"); public:	/* slot 472 */
private: virtual void _vi_473() __asm__("__ZN23IO80211VirtualInterface23setOutputFlowControlledEv"); public:	/* slot 473 */
private: virtual void _vi_474() __asm__("__ZN23IO80211VirtualInterface25clearOutputFlowControlledEv"); public:	/* slot 474 */
private: virtual void _vi_475() __asm__("__ZN23IO80211VirtualInterface15flushFlowQueuesEP10ether_addr"); public:	/* slot 475 */
private: virtual void _vi_476() __asm__("__ZN23IO80211VirtualInterface18configureAQMOutputEv"); public:	/* slot 476 */
private: virtual void _vi_477() __asm__("__ZN23IO80211VirtualInterface11attachToBpfEv"); public:	/* slot 477 */
private: virtual void _vi_478() __asm__("__ZN23IO80211VirtualInterface14configureIfnetEv"); public:	/* slot 478 */
private: virtual void _vi_479() __asm__("__ZN23IO80211VirtualInterface5resetEv"); public:	/* slot 479 */
private: virtual void _vi_480() __asm__("__ZN23IO80211VirtualInterface25dupAndTransmitMcastPacketEP20IO80211NetworkPacketPhS2_"); public:	/* slot 480 */
private: virtual void _vi_481() __asm__("__ZN23IO80211VirtualInterface19freeMulticastPacketEP20IO80211NetworkPacket"); public:	/* slot 481 */
private: virtual void _vi_482() __asm__("__ZN23IO80211VirtualInterface22triggerAWDLMulticastTxEjh"); public:	/* slot 482 */
private: virtual void _vi_483() __asm__("__ZN23IO80211VirtualInterface14p2pMulticastTxEPP20IO80211NetworkPacketj"); public:	/* slot 483 */
private: virtual void _vi_484() __asm__("__ZN23IO80211VirtualInterface14p2pMulticastTxEjb"); public:	/* slot 484 */
private: virtual void _vi_485() __asm__("__ZN23IO80211VirtualInterface22signalMulticastPacketsEjj"); public:	/* slot 485 */
private: virtual void _vi_486() __asm__("__ZN23IO80211VirtualInterface25sendPendingPacketsToStackEPP20IO80211NetworkPacketj"); public:	/* slot 486 */
private: virtual void _vi_487() __asm__("__ZN23IO80211VirtualInterface18freePendingPacketsEPP20IO80211NetworkPacketj"); public:	/* slot 487 */
    virtual IOReturn setMacAddress(ether_addr &) = 0;	/* slot 488 */
private: virtual void _vi_489() __asm__("__ZN23IO80211VirtualInterface33_RESERVEDIO80211VirtualInterface0Ev"); public:	/* slot 489 */
private: virtual void _vi_490() __asm__("__ZN23IO80211VirtualInterface33_RESERVEDIO80211VirtualInterface1Ev"); public:	/* slot 490 */
private: virtual void _vi_491() __asm__("__ZN23IO80211VirtualInterface33_RESERVEDIO80211VirtualInterface2Ev"); public:	/* slot 491 */
private: virtual void _vi_492() __asm__("__ZN23IO80211VirtualInterface33_RESERVEDIO80211VirtualInterface3Ev"); public:	/* slot 492 */
private: virtual void _vi_493() __asm__("__ZN23IO80211VirtualInterface33_RESERVEDIO80211VirtualInterface4Ev"); public:	/* slot 493 */
private: virtual void _vi_494() __asm__("__ZN23IO80211VirtualInterface33_RESERVEDIO80211VirtualInterface5Ev"); public:	/* slot 494 */
private: virtual void _vi_495() __asm__("__ZN23IO80211VirtualInterface33_RESERVEDIO80211VirtualInterface6Ev"); public:	/* slot 495 */
private: virtual void _vi_496() __asm__("__ZN23IO80211VirtualInterface33_RESERVEDIO80211VirtualInterface7Ev"); public:	/* slot 496 */
private: virtual void _vi_497() __asm__("__ZN23IO80211VirtualInterface33_RESERVEDIO80211VirtualInterface8Ev"); public:	/* slot 497 */
private: virtual void _vi_498() __asm__("__ZN23IO80211VirtualInterface33_RESERVEDIO80211VirtualInterface9Ev"); public:	/* slot 498 */
private: virtual void _vi_499() __asm__("__ZN23IO80211VirtualInterface34_RESERVEDIO80211VirtualInterface10Ev"); public:	/* slot 499 */
private: virtual void _vi_500() __asm__("__ZN23IO80211VirtualInterface34_RESERVEDIO80211VirtualInterface11Ev"); public:	/* slot 500 */
private: virtual void _vi_501() __asm__("__ZN23IO80211VirtualInterface34_RESERVEDIO80211VirtualInterface12Ev"); public:	/* slot 501 */
private: virtual void _vi_502() __asm__("__ZN23IO80211VirtualInterface34_RESERVEDIO80211VirtualInterface13Ev"); public:	/* slot 502 */
private: virtual void _vi_503() __asm__("__ZN23IO80211VirtualInterface34_RESERVEDIO80211VirtualInterface14Ev"); public:	/* slot 503 */
private: virtual void _vi_504() __asm__("__ZN23IO80211VirtualInterface34_RESERVEDIO80211VirtualInterface15Ev"); public:	/* slot 504 */
private:
    void *_vi_member;	/* the one field this class adds (0x128..0x130) */
};
static_assert(sizeof(IO80211VirtualInterface) == 0x130, "WCL ABI size");

#endif
