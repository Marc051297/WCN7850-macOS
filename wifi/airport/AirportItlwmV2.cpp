//
//  AirportItlwmV2.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "AirportItlwmV2.hpp"
#include <sys/_netstat.h>
#include <crypto/sha1.h>
#include <net80211/ieee80211_priv.h>
#include <net80211/ieee80211_var.h>

#include "AirportItlwmSkywalkInterface.hpp"
#include "awdlnative/QwzAWDLInterface.hpp"	/* F1 (native AWDL) */
#include "qwz_log.h"
#include "qwz_sysctl.h"
#include "qwz_life.h"
#include "qwz_awdlx.h"
#include <kern/clock.h>
#include "IOPCIEDeviceWrapper.hpp"
extern "C" int qwz_awdl0_up(void);			/* qwz/qwz_awdl0.h (merge glue H2): the kext-owned awdl0 exists */
extern "C" int qwz_awdl0_set(const char *cmd);		/* qwz/qwz_awdl0.h (H4): "down" destroys it */
extern "C" int qwz_mon_set(int chan);			/* qwz/qwz_mon.h (H4): 0 ends a capture cell */
extern "C" int qwz_mon_status(void);			/* qwz/qwz_mon.h (F1): channel while a cell is active, else 0; lock-free */

#define super IO80211Controller
OSDefineMetaClassAndStructors(AirportItlwm, IO80211Controller);
OSDefineMetaClassAndStructors(CTimeout, OSObject)

IO80211WorkQueue *_fWorkloop;
IOCommandGate *_fCommandGate;

/* M0 (C16): does the calling thread hold the main gate? (a log field of the thread-identity lines only) */
static bool pmGateHeld(void)
{
    IOWorkLoop *wl = _fCommandGate != NULL ? _fCommandGate->getWorkLoop() : NULL;

    return wl != NULL && wl->inGate();
}

void AirportItlwm::checkInterfaceRole(const char *when, bool restore)
{
    if (!fNetIf)
        return;
    OSString *role = OSDynamicCast(OSString, fNetIf->getProperty("IO80211InterfaceRole"));
    XYLog("interface role property %s: %s\n", when, role ? role->getCStringNoCopy() : "missing");
    if (!role && restore && fNetIf->getInterfaceRole() == 1) {
        fNetIf->setProperty("IO80211InterfaceRole", "Infrastructure");
        XYLog("interface role property put back\n");
    }
}

void AirportItlwm::roleTimerFired(OSObject *owner, IOTimerEventSource *sender)
{
    static const uint32_t delays[] = {2000, 7000, 20000};
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);
    if (!self)
        return;
    self->checkInterfaceRole("timer", true);
    if (self->roleChecks < sizeof(delays) / sizeof(delays[0]))
        sender->setTimeoutMS(delays[self->roleChecks++]);
}

void AirportItlwm::releaseAll()
{
    teardownAWDLInterface();	/* F1 (native AWDL): idempotent; stop() normally did it already */
    quiescePM();
    /* M0 (C14): quiescePM already ended the sentinel; again, idempotently, before fHalService is released below (the sentinel calls the HAL once a second) */
    stopSentinel();
#ifdef AIRPORT_WCL
    if (fNetIf)
        static_cast<AirportItlwmSkywalkInterface *>(fNetIf)->stopWCL();
    OSSafeReleaseNULL(driverFaultReporter);
    OSSafeReleaseNULL(driverCoreFaultReporter);
    OSSafeReleaseNULL(driverFaultStream);
    OSSafeReleaseNULL(driverLogger);
    // Stop only successfully started pipes, while their owner workloop exists.
    if (driverSnapshotsPipeStarted) {
        driverSnapshotsPipe->CCPipe::stopPipe();
        driverSnapshotsPipeStarted = false;
    }
    if (driverDataPathPipeStarted) {
        driverDataPathPipe->CCPipe::stopPipe();
        driverDataPathPipeStarted = false;
    }
    if (driverLogPipeStarted) {
        driverLogPipe->CCPipe::stopPipe();
        driverLogPipeStarted = false;
    }
#endif
    OSSafeReleaseNULL(driverLogPipe);
    OSSafeReleaseNULL(driverDataPathPipe);
    OSSafeReleaseNULL(driverSnapshotsPipe);
#ifndef AIRPORT_WCL
    OSSafeReleaseNULL(driverFaultReporter);
#endif
    if (fHalService) {
        fHalService->release();
        fHalService = NULL;
    }
    if (_fWorkloop) {
        if (_fCommandGate) {
//            _fCommandGate->disable();
            _fWorkloop->removeEventSource(_fCommandGate);
            _fCommandGate->release();
            _fCommandGate = NULL;
        }
        if (scanSource) {
            scanSource->cancelTimeout();
            scanSource->disable();
            _fWorkloop->removeEventSource(scanSource);
            scanSource->release();
            scanSource = NULL;
        }
        if (fWatchdogWorkLoop && watchdogTimer) {
            watchdogTimer->cancelTimeout();
            fWatchdogWorkLoop->removeEventSource(watchdogTimer);
            watchdogTimer->release();
            watchdogTimer = NULL;
            fWatchdogWorkLoop->release();
            fWatchdogWorkLoop = NULL;
        }
        _fWorkloop->release();
        _fWorkloop = NULL;
    }
    unregistPM();
}

void AirportItlwm::
eventHandler(struct ieee80211com *ic, int msgCode, void *data)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, ic->ic_ac.ac_if.controller);
#ifdef AIRPORT_WCL
    if (!that)
        return;
#endif
    IO80211SkywalkInterface *interface = that->fNetIf;
    if (!interface)
        return;
#ifdef AIRPORT_WCL
    static_cast<AirportItlwmSkywalkInterface *>(interface)->handleWCLEvent(msgCode, data);
#else
    switch (msgCode) {
        case IEEE80211_EVT_COUNTRY_CODE_UPDATE:
            interface->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, NULL, 0, 0);
            break;
        case IEEE80211_EVT_STA_ASSOC_DONE:
            interface->postMessage(APPLE80211_M_ASSOC_DONE, NULL, 0, 0);
            break;
        case IEEE80211_EVT_STA_DEAUTH:
            interface->postMessage(APPLE80211_M_DEAUTH_RECEIVED, NULL, 0, 0);
            break;
        default:
            break;
    }
#endif
}

void AirportItlwm::watchdogAction(IOTimerEventSource *timer)
{
    if (fHalService == NULL || pmStopping)
        return;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    (*ifp->if_watchdog)(ifp);
    checkDataStall();

    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
}

void AirportItlwm::fakeScanDone(OSObject *owner, IOTimerEventSource *sender)
{
#ifndef AIRPORT_WCL
    UInt32 msg = 0;
#endif
    AirportItlwm *that = (AirportItlwm *)owner;
#ifdef AIRPORT_WCL
    if (that->fNetIf)
        static_cast<AirportItlwmSkywalkInterface *>(that->fNetIf)->completeWCLScan();
#else
    that->fNetIf->postMessage(APPLE80211_M_SCAN_DONE, &msg, 4, 0);
#endif
}

bool AirportItlwm::init(OSDictionary *properties)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    bool ret = super::init(properties);
    awdlSyncEnable = true;
    /* F1 (native AWDL): the master switch and the build guard (airport/awdlnative/AWDLBuildGuard.cpp). */
    {
        extern bool itlAWDLBuildAllowed();
        extern const char *itlAWDLBuildStatus();
        extern bool qwzAwdlRawSamples;
        int awdlBootValue = 0;
        fAWDLInterface = nullptr;
        awdlInterfaceTest = itlAWDLBuildAllowed() &&
            PE_parse_boot_argn("-qwzawdlnative", &awdlBootValue, sizeof(awdlBootValue));
        setProperty("ItlwmAWDLBuildCheck", itlAWDLBuildStatus());
        qwzAwdlRawSamples = PE_parse_boot_argn("-itlawdlsamples", &awdlBootValue, sizeof(awdlBootValue));
        awdlExtendFrames = PE_parse_boot_argn("-itlawdlelicit", &awdlBootValue, sizeof(awdlBootValue));
        awdlHideInfra = PE_parse_boot_argn("-itlawdlhideinfra", &awdlBootValue, sizeof(awdlBootValue));
        awdlProbeSequence = 0;
        awdlCreateRequests = 0;
        awdlManagementInputEnabled = false;
        awdlManagementActive = 0;
        setProperty("ItlwmAWDLInterfaceBuild", QWZ_AWDL_NATIVE_BUILD);
        setProperty("ItlwmAWDLInterfaceTestEnabled", awdlInterfaceTest ? kOSBooleanTrue : kOSBooleanFalse);
    }
    power_state = 0;
    memset(geo_location_cc, 0, sizeof(geo_location_cc));
    return ret;
}

IOService* AirportItlwm::probe(IOService *provider, SInt32 *score)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    IOPCIEDeviceWrapper *wrapper = OSDynamicCast(IOPCIEDeviceWrapper, provider);
    if (!wrapper) {
        XYLog("%s Not a IOPCIEDeviceWrapper instance\n", __FUNCTION__);
        return NULL;
    }
    pciNub = wrapper->pciNub;
    fHalService = wrapper->fHalService;
    if (!pciNub || !fHalService) {
        XYLog("%s Not a valid IOPCIEDeviceWrapper instance\n", __FUNCTION__);
        return NULL;
    }
    return super::probe(provider, score);
}

#define LOWER32(x)  ((uint64_t)(x) & 0xffffffff)
#define HIGHER32(x) ((uint64_t)(x) >> 32)

bool AirportItlwm::
initCCLogs()
{
    CCPipeOptions driverLogOptions = { 0 };
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 1;
    driverLogOptions.pipe_size = 0x200000;
    driverLogOptions.min_log_size_notify = 0xccccc;
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "Itlwm_Logs", sizeof(driverLogOptions.file_name));
    snprintf(driverLogOptions.name, sizeof(driverLogOptions.name), "wlan%d", 0);
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = 0x1000000;
    driverLogOptions.pad10 = 2;
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverLogPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "DriverLogs", &driverLogOptions);
    XYLog("%s driverLogPipeRet %d\n", __FUNCTION__, driverLogPipe != NULL);
#ifdef AIRPORT_WCL
    if (!driverLogPipe || !getWorkLoop())
        return false;
    // The factory only initializes the pipe. startPipe creates its event sources.
    // Qualify the native method: do not dispatch through the legacy CCPipe header.
    driverLogPipeStarted = driverLogPipe->CCPipe::startPipe();
    if (!driverLogPipeStarted)
        return false;
    CCStreamOptions loggerOptions = {};
    CCStream *logger = CCStream::withPipeAndName(driverLogPipe, "AirportItlwm", &loggerOptions);
    driverLogger = OSDynamicCast(CCLogStream, logger);
    if (!driverLogger) {
        OSSafeReleaseNULL(logger);
        return false;
    }
#endif

    memset(&driverLogOptions, 0, sizeof(driverLogOptions));
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 0;
    driverLogOptions.pipe_size = 0x200000;
    driverLogOptions.min_log_size_notify = 0xccccc;
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "AppleBCMWLAN_Datapath", sizeof(driverLogOptions.file_name));
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = HIGHER32(0x202800000);
    driverLogOptions.pad10 = LOWER32(0x202800000);
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverDataPathPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "DatapathEvents", &driverLogOptions);
    XYLog("%s driverDataPathPipeRet %d\n", __FUNCTION__, driverDataPathPipe != NULL);
#ifdef AIRPORT_WCL
    if (!driverDataPathPipe)
        return false;
    driverDataPathPipeStarted = driverDataPathPipe->CCPipe::startPipe();
    if (!driverDataPathPipeStarted)
        return false;
#endif

    memset(&driverLogOptions, 0, sizeof(driverLogOptions));
    driverLogOptions.pipe_type = 0x200000001;
    driverLogOptions.log_data_type = 2;
    strlcpy(driverLogOptions.file_name, "StateSnapshots", sizeof(driverLogOptions.file_name));
    strlcpy(driverLogOptions.name, "0", sizeof(driverLogOptions.name));
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pipe_size = 128;
    driverSnapshotsPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "StateSnapshots", &driverLogOptions);
    XYLog("%s driverSnapshotsPipeRet %d\n", __FUNCTION__, driverSnapshotsPipe != NULL);
#ifdef AIRPORT_WCL
    if (!driverSnapshotsPipe)
        return false;
    driverSnapshotsPipeStarted = driverSnapshotsPipe->CCPipe::startPipe();
    if (!driverSnapshotsPipeStarted)
        return false;
#endif

    CCStreamOptions faultReportOptions = { 0 };
    faultReportOptions.stream_type = 1;
    faultReportOptions.console_level = 0xFFFFFFFFFFFFFFFF;
#ifdef AIRPORT_WCL
    driverFaultStream = CCStream::withPipeAndName(driverSnapshotsPipe, "FaultReporter", &faultReportOptions);
    CCDataStream *dataStream = OSDynamicCast(CCDataStream, driverFaultStream);
    if (!dataStream || !_fWorkloop) {
        return false;
    }
    driverCoreFaultReporter = CCFaultReporter::withStreamWorkloop(dataStream, _fWorkloop);
    if (!driverCoreFaultReporter) {
        return false;
    }
    driverFaultReporter = IO80211FaultReporter::allocWithParams(driverCoreFaultReporter);
    if (!OSDynamicCast(CommonFaultReporter, driverFaultReporter)) {
        return false;
    }
#else
    driverFaultReporter = CCStream::withPipeAndName(driverSnapshotsPipe, "FaultReporter", &faultReportOptions);
#endif
    XYLog("%s driverFaultReporterRet %d\n", __FUNCTION__, driverFaultReporter != NULL);
    return driverLogPipe && driverDataPathPipe && driverSnapshotsPipe && driverFaultReporter;
}

bool AirportItlwm::isCommandProhibited(int command)
{
    static uint8_t seen[1024 / 8];

    if (command >= 0 && command < 1024 && !(seen[command / 8] & (1 << (command % 8)))) {
        seen[command / 8] |= 1 << (command % 8);
        qwz_log("airport: ioctl %d (%s) first asked of the driver\n", command, convertApple80211IOCTLToString(command));
    }
    return false;
}

bool AirportItlwm::start(IOService *provider)
{
#if __IO80211_TARGET == __MAC_15_2
    if (version_major != 24 || version_minor < 2)
        return false;
#endif
    XYLog("%s\n", __PRETTY_FUNCTION__);
    struct IOSkywalkEthernetInterface::RegistrationInfo registInfo;
    int boot_value = 0;

    UInt8 builtIn = 0;
    setProperty("built-in", OSData::withBytes(&builtIn, sizeof(builtIn)));
    setProperty("DriverKitDriver", kOSBooleanFalse);
#ifdef AIRPORT_WCL
    qwz_log("qwz: trace: AirportItlwm::start: initCCLogs\n");
    if (!initCCLogs()) {
        releaseAll();
        return false;
    }
#endif
    qwz_log("qwz: trace: AirportItlwm::start: IO80211Controller::start\n");
    if (!super::start(provider)) {
#ifdef AIRPORT_WCL
        releaseAll();
#endif
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: super started; PCI enables\n");
    pciNub->setBusMasterEnable(true);
    pciNub->setIOEnable(true);
    pciNub->setMemoryEnable(true);
    /* QwzMac: MSI only; never let the card fall back to its INTx line. */
    pciNub->configWrite16(kIOPCIConfigCommand,
        pciNub->configRead16(kIOPCIConfigCommand) | 0x0400);
    if (pciNub->requestPowerDomainState(kIOPMPowerOn,
                                        (IOPowerConnection *) getParentEntry(gIOPowerPlane), IOPMLowestState) != IOPMNoErr) {
        super::stop(provider);
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: initPCIPowerManagment\n");
    if (initPCIPowerManagment(pciNub) == false) {
        super::stop(pciNub);
        return false;
    }
    if (_fWorkloop == NULL) {
        XYLog("No _fWorkloop!!\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: command gate\n");
    _fCommandGate = IOCommandGate::commandGate(this, (IOCommandGate::Action)AirportItlwm::tsleepHandler);
    if (_fCommandGate == 0) {
        XYLog("No command gate!!\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    _fWorkloop->addEventSource(_fCommandGate);
    const IONetworkMedium *primaryMedium;
    if (!createMediumTables(&primaryMedium) ||
        !setCurrentMedium(primaryMedium) || !setSelectedMedium(primaryMedium)) {
        XYLog("setup medium fail\n");
        releaseAll();
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: initWithController\n");
    fHalService->initWithController(this, _fWorkloop, _fCommandGate);
    fHalService->get80211Controller()->ic_event_handler = eventHandler;

    qwz_log("qwz: pm: sleep/wake build 5 + awdltest + awdl0 via the AWDL kext: hal ring + firmware memory reset on restart, setPOWER waits for the system wake, quiesce, single-owner recovery, Wi-Fi off/on reset, data-stall restart\n");
    if (PE_parse_boot_argn("-qwznowakefix", &boot_value, sizeof(boot_value))) {
        noWakeFix = true;
        qwz_log("qwz: pm: -qwznowakefix: sleep/wake keeps the old interface-down/up handlers\n");
        qwz_log("qwz: pm: -qwznowakefix: no wake ordering, Wi-Fi off/on reset or data-stall restart either\n");
    }
    if (PE_parse_boot_argn("-qwznoradioreset", &boot_value, sizeof(boot_value))) {
        noRadioReset = true;
        qwz_log("qwz: pm: -qwznoradioreset: Wi-Fi off/on only stops and starts the interface\n");
    }
    if (PE_parse_boot_argn("-qwznostallfix", &boot_value, sizeof(boot_value))) {
        noStallFix = true;
        qwz_log("qwz: pm: -qwznostallfix: no data-stall restart\n");
    }
    if (PE_parse_boot_argn("-qwzstallsbr", &boot_value, sizeof(boot_value))) {
        stallSbr = true;
        qwz_log("qwz: pm: -qwzstallsbr: the second data-stall restart also does a secondary bus reset\n");
    }
    {
        /* K2 (H1): qwzhwmode=N starts the boot with debug.qwz.hwmode N (the firmware's offered list is not known yet; an id that is not 0, 1 or 5 and not offered falls back to 1 at the firmware start, with a log line) */
        int hwm_boot = 0;
        if (PE_parse_boot_argn("qwzhwmode", &hwm_boot, sizeof(hwm_boot))) {
            if (hwm_boot >= 0 && hwm_boot < 32) {
                qwz_hwmode = hwm_boot;
                qwz_log("qwz: hwmode: boot-arg qwzhwmode=%d: debug.qwz.hwmode starts at %d\n", hwm_boot, hwm_boot);
            } else
                qwz_log("qwz: hwmode: boot-arg qwzhwmode=%d ignored (0 to 31 only)\n", hwm_boot);
        }
    }
    {
        /* K5 (OBSD-2): qwzfwsegs=N (0 to 2) starts the boot with debug.qwz.fwsegs N, so the first firmware start of a boot can use it; one banner line per boot */
#ifndef QWZ_FWSEGS_DEFAULT
#define QWZ_FWSEGS_DEFAULT 0
#endif
        int fws_boot = 0;
        if (PE_parse_boot_argn("qwzfwsegs", &fws_boot, sizeof(fws_boot))) {
            if (fws_boot >= 0 && fws_boot <= 2) {
                qwz_fwsegs = fws_boot;
                qwz_log("qwz: fwsegs: boot-arg qwzfwsegs=%d\n", fws_boot);
            } else
                qwz_log("qwz: fwsegs: boot-arg qwzfwsegs=%d ignored (0 to 2 only)\n", fws_boot);
        }
        qwz_log("qwz: fwsegs: mode %d (default %d)\n", qwz_fwsegs, (int)QWZ_FWSEGS_DEFAULT);
    }
    if (PE_parse_boot_argn("qwzhwmguard", &boot_value, sizeof(boot_value)) && boot_value == 1) {
        qwz_hwm_guard = 1;
        qwz_log("qwz: hwmode: boot-arg qwzhwmguard=1: offered hardware mode ids outside the priority table are skipped\n");
    }
    if (PE_parse_boot_argn("-novht", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOVHT;
    if (PE_parse_boot_argn("-noht40", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOHT40;

#ifdef AIRPORT_WCL
    int he_enabled = 0;
    if (PE_parse_boot_argn("itlwm_he", &he_enabled, sizeof(he_enabled)) && he_enabled == 1)
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_HEON;
#endif

    qwz_log("qwz: trace: AirportItlwm::start: HAL attach\n");
    if (!fHalService->attach(pciNub)) {
        qwz_log("qwz: trace: AirportItlwm::start: HAL attach failed; stopping\n");
        super::stop(pciNub);
        qwz_log("qwz: trace: AirportItlwm::start: super stopped; releaseAll\n");
        releaseAll();
        qwz_log("qwz: trace: AirportItlwm::start: released\n");
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: HAL attached; watchdog\n");
    /* M0 (C15): the HAL tells the engine about every new DEAD episode (a fault raised before this line is found by the sentinel's poll, C14) */
    fHalService->setFaultHandler(AirportItlwm::faultCb, this);

    fWatchdogWorkLoop = IOWorkLoop::workLoop();
    if (fWatchdogWorkLoop == NULL) {
        XYLog("init watchdog workloop fail\n");
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    watchdogTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &AirportItlwm::watchdogAction));
    if (!watchdogTimer) {
        XYLog("init watchdog fail\n");
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    fWatchdogWorkLoop->addEventSource(watchdogTimer);
    scanSource = IOTimerEventSource::timerEventSource(this, &fakeScanDone);
#ifdef AIRPORT_WCL
    if (!scanSource || _fWorkloop->addEventSource(scanSource) != kIOReturnSuccess) {
        super::stop(provider);
        releaseAll();
        return false;
    }
#else
    _fWorkloop->addEventSource(scanSource);
#endif
    scanSource->enable();

    qwz_log("qwz: trace: AirportItlwm::start: new AirportItlwmSkywalkInterface\n");
    fNetIf = new AirportItlwmSkywalkInterface;
#ifdef AIRPORT_WCL
    if (!fNetIf || !static_cast<AirportItlwmSkywalkInterface *>(fNetIf)->init(this)) {
#else
    if (!fNetIf->init(this)) {
#endif
        XYLog("Skywalk interface init fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: interface init done; setInterfaceRole\n");
    fNetIf->setInterfaceRole(1);
    checkInterfaceRole("after setInterfaceRole", false);
    fNetIf->setInterfaceId(1);

#ifndef AIRPORT_WCL
    if (!initCCLogs()) {
        XYLog("CCLog init fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
#endif
    checkInterfaceRole("before attach", false);
    if (!fNetIf->attach(this)) {
        XYLog("attach to service fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: attachInterface\n");
    if (!attachInterface(fNetIf, this)) {
        XYLog("attach to interface fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: attach bsd interface\n");
    if (!IONetworkController::attachInterface((IONetworkInterface **)&bsdInterface,
#ifdef AIRPORT_WCL
                                             false
#else
                                             true
#endif
                                             )) {
        XYLog("attach to IONetworkController interface fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    memset(&registInfo, 0, sizeof(registInfo));
    if (!fNetIf->initRegistrationInfo(&registInfo, 1, sizeof(registInfo))) {
        XYLog("initRegistrationInfo fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
#ifdef AIRPORT_WCL
    fNetIf->mExpansionData->fRegistrationInfo = (struct IOSkywalkNetworkInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkNetworkInterface::RegistrationInfo));
    fNetIf->mExpansionData2->fRegistrationInfo = (struct IOSkywalkEthernetInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkEthernetInterface::RegistrationInfo));
    if (!fNetIf->mExpansionData->fRegistrationInfo || !fNetIf->mExpansionData2->fRegistrationInfo) {
        super::stop(provider);
        releaseAll();
        return false;
    }
    memcpy(fNetIf->mExpansionData->fRegistrationInfo, &registInfo, sizeof(registInfo));
    memcpy(fNetIf->mExpansionData2->fRegistrationInfo, &registInfo, sizeof(registInfo));
    if (fNetIf->getInterfaceRole() == 1)
        fNetIf->deferBSDAttach(true);
    checkInterfaceRole("before interface start", false);
    if (!fNetIf->start(this)) {
        super::stop(provider);
        releaseAll();
        return false;
    }

#else
    if (!fNetIf->initRegistrationInfo(&registInfo, 1, sizeof(registInfo))) {
        XYLog("initRegistrationInfo fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    fNetIf->mExpansionData->fRegistrationInfo = (struct IOSkywalkNetworkInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkNetworkInterface::RegistrationInfo));
    fNetIf->mExpansionData2->fRegistrationInfo = (struct IOSkywalkEthernetInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkEthernetInterface::RegistrationInfo));
    memcpy(fNetIf->mExpansionData->fRegistrationInfo, &registInfo, sizeof(registInfo));
    memcpy(fNetIf->mExpansionData2->fRegistrationInfo, &registInfo, sizeof(registInfo));
    if (fNetIf->getInterfaceRole() == 1)
        fNetIf->deferBSDAttach(true);
    fNetIf->start(this);

#endif
    checkInterfaceRole("after interface start", true);
    setLinkStatus(kIONetworkLinkValid);
    if (TAILQ_EMPTY(&fHalService->get80211Controller()->ic_ess))
        fHalService->get80211Controller()->ic_flags |= IEEE80211_F_AUTO_JOIN;
#ifdef AIRPORT_WCL
    qwz_log("qwz: trace: AirportItlwm::start: registerService\n");
    awdlManagementInputEnabled = true;	/* F1 (native AWDL): hand-offs to the peer manager may start */
    bsdInterface->registerService();
#endif
    registerService();
    checkInterfaceRole("after registerService", true);
    roleChecks = 0;
    roleTimer = IOTimerEventSource::timerEventSource(this, &AirportItlwm::roleTimerFired);
    if (roleTimer && _fWorkloop->addEventSource(roleTimer) == kIOReturnSuccess)
        roleTimer->setTimeoutMS(1000);
    else
        OSSafeReleaseNULL(roleTimer);
    qwz_sysctl_set_awdl(awdlSysctl, this);
    qwz_sysctl_set_m0(m0Sysctl, this);		/* M0 (C32, C34): debug.qwz.state, recover=2, health=1 */
    /*
     * M0 (C14): the sentinel, a second timer on the watchdog workloop's thread, created last and
     * for the life of the driver; it never takes the main gate. Note (S20 consequence): until the
     * first successful gate probe, and for the whole boot if none succeeds in 30 probes, the
     * legacy watchdog on this same thread keeps its blocking runAction, so a main-gate holder
     * that never releases the gate would also keep this timer from running (no DEADLINE, no
     * 'gate busy' line, no health line). The last crumb in the stream is then the only evidence.
     * A dedicated IOWorkLoop for the sentinel (one more kernel thread) was considered and not added:
     * a dedicated kernel thread is known to reset this machine (itl80211/openbsd/sys/_task.cpp:31-36).
     */
    sentinelTimer = IOTimerEventSource::timerEventSource(this, &AirportItlwm::sentinelFired);
    if (sentinelTimer && fWatchdogWorkLoop->addEventSource(sentinelTimer) == kIOReturnSuccess)
        sentinelTimer->setTimeoutMS(1000);
    else {
        qwz_log("qwz: sentinel: could not start the sentinel timer: no chip-transition deadlines, no gate probe, no periodic health line\n");
        OSSafeReleaseNULL(sentinelTimer);
    }
    return true;
}

/*
 * Without this the firmware keeps running (and the driver's stop() tears it
 * down late, with parts of the system gone) through power-off, which froze
 * the shutdown. Quiet the chip while everything is still alive.
 */
void AirportItlwm::systemWillShutdown(IOOptionBits specifier)
{
    if ((specifier == kIOMessageSystemWillPowerOff ||
         specifier == kIOMessageSystemWillRestart) && fHalService != NULL) {
        qwz_logc("qwz: ==== system shutdown (0x%x): turning the radio off ====\n", (unsigned)specifier);
        fHalService->shutdownHardware();
        qwz_logc("qwz: shutdown: radio off; calling super\n");
    }
    super::systemWillShutdown(specifier);
    qwz_logc("qwz: shutdown: super::systemWillShutdown returned\n");
}

void AirportItlwm::stop(IOService *provider)
{
    qwz_logc("qwz: stop: enter\n");
#if defined(ITL_AWDL_NATIVE)
    /* F1 (native AWDL, from ForbiddenItlwm): no new hand-offs, drain the active ones, then stop the peer manager before super::stop(). */
    if (_fWorkloop) {
        _fWorkloop->runAction([](OSObject *owner, void*, void*, void*, void*) -> IOReturn {
            auto *controller = static_cast<AirportItlwm *>(owner);
            controller->awdlManagementInputEnabled = false;
            while (controller->awdlManagementActive)
                _fWorkloop->commandSleep(&controller->awdlManagementActive, 0);
            return kIOReturnSuccess;
        }, this, nullptr, nullptr, nullptr, nullptr);
    }
    teardownAWDLInterface();
#endif
    qwz_sysctl_set_awdl(NULL, NULL);
    qwz_sysctl_set_m0(NULL, NULL);		/* M0 (C32): the sysctl nodes answer ENXIO from here on */
    static_cast<ItlQwz *>(fHalService)->setAwdlRx(NULL, NULL);
    static_cast<ItlQwz *>(fHalService)->setAwdlDataRx(NULL, NULL);
    /* The AWDL engine can still call the bridge: the queue stays, nothing more is sent. */
    awdlTxStopped = true;
    static_cast<ItlQwz *>(fHalService)->setAwdlTxDone(NULL, NULL);	/* WP4 (M2a) */
    awdlGateStopped = true;
    if (awdlGateTimer != NULL) {
        awdlGateTimer->cancelTimeout();
        getWorkLoop()->removeEventSource(awdlGateTimer);
    }
    qwz_awdlx_link(0);	/* M2a (WP3) */
    qwz_awdlx_clk(0, 0, 0, 0, 0);	/* M2a (WP3): stop() clears awdlAwOn below without awdlAwStart, so the clock mirror is switched off here */
    if (awdlTxTimer != NULL) {
        awdlTxTimer->cancelTimeout();
        getWorkLoop()->removeEventSource(awdlTxTimer);
    }
    awdlAwOn = false;
    if (awdlAwTimer != NULL) {
        awdlAwTimer->cancelTimeout();
        getWorkLoop()->removeEventSource(awdlAwTimer);
    }
    if (awdlPeerCall != NULL)
        thread_call_cancel_wait(awdlPeerCall);	/* awdlTxStopped stops a running one */
    if (fHalService != NULL) {		/* merge glue (H4): the capture cell's awdl0 and cell end on this sleepable thread, before the power management and the HAL go */
        if (qwz_awdl0_up() != 0)	/* F1: inert with no awdl0 and no cell, so stop() runs no new blocking call (qwz_mon_set takes the gate and the ioctl lock) */
            (void)qwz_awdl0_set("down");
        if (qwz_mon_status() != 0)	/* lock-free read of mon_active (NULL-safe) */
            (void)qwz_mon_set(0);
    }
    quiescePM();
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    if (roleTimer) {
        roleTimer->cancelTimeout();
        _fWorkloop->removeEventSource(roleTimer);
        OSSafeReleaseNULL(roleTimer);
    }
    qwz_logc("qwz: stop: role timer gone; stopWCL\n");
#ifdef AIRPORT_WCL
    if (fNetIf)
        static_cast<AirportItlwmSkywalkInterface *>(fNetIf)->stopWCL();
#endif
    qwz_logc("qwz: stop: super::stop\n");
    super::stop(provider);
    qwz_logc("qwz: stop: disableAdapter\n");
    disableAdapter(bsdInterface);
    setLinkStatus(kIONetworkLinkValid);
    qwz_logc("qwz: stop: HAL detach\n");
    fHalService->detach(pciNub);
    qwz_logc("qwz: stop: ether_ifdetach\n");
    ether_ifdetach(ifp);
    qwz_logc("qwz: stop: detachInterface\n");
    detachInterface(fNetIf, true);
    OSSafeReleaseNULL(fNetIf);
    qwz_logc("qwz: stop: releaseAll\n");
    releaseAll();
    qwz_logc("qwz: stop: done\n");
}

void AirportItlwm::free()
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    if (fHalService != NULL) {
        fHalService->release();
        fHalService = NULL;
    }
    if (syncFrameTemplate != NULL && syncFrameTemplateLength > 0) {
        IOFree(syncFrameTemplate, syncFrameTemplateLength);
        syncFrameTemplateLength = 0;
        syncFrameTemplate = NULL;
    }
    if (roamProfile != NULL) {
        IOFree(roamProfile, sizeof(struct apple80211_roam_profile_band_data));
        roamProfile = NULL;
    }
    if (btcProfile != NULL) {
        IOFree(btcProfile, sizeof(struct apple80211_btc_profiles_data));
        btcProfile = NULL;
    }
    super::free();
}

bool AirportItlwm::createWorkQueue()
{
    XYLog("%s %d\n", __FUNCTION__, _fWorkloop != 0);
    return _fWorkloop != 0;
}

IO80211WorkQueue *AirportItlwm::getWorkQueue()
#ifdef AIRPORT_WCL
const
#endif
{
    return _fWorkloop;
}

#ifdef AIRPORT_WCL
CommonFaultReporter *AirportItlwm::getFaultReporterFromDriver()
#else
void *AirportItlwm::getFaultReporterFromDriver()
#endif
{
    return driverFaultReporter;
}

IOReturn AirportItlwm::enable(IO80211SkywalkInterface *netif)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    /* M0 (C16): the same thread-identity line as setPOWER (unreachable in this build, but cheap) */
    qwz_log("qwz: pm: enable(netif) on thread %llx, workloop thread %d, gate held %d, chipBusy %d\n",
            (unsigned long long)thread_tid(current_thread()), onWorkLoopThread(), pmGateHeld(), chipBusy);
    super::enable(netif);
    _fCommandGate->enable();
    if (power_state)
        enableAdapter(bsdInterface);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::disable(IO80211SkywalkInterface *netif)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    qwz_log("qwz: pm: disable(netif) on thread %llx, workloop thread %d, gate held %d, chipBusy %d\n",
            (unsigned long long)thread_tid(current_thread()), onWorkLoopThread(), pmGateHeld(), chipBusy);
    super::disable(netif);
    setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}

bool AirportItlwm::configureInterface(IONetworkInterface *netif)
{
    IONetworkData *nd;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;

    if (super::configureInterface(netif) == false) {
        XYLog("super failed\n");
        return false;
    }
    if (creatingAwdlBsd) {
        /* awdl0: never the net80211 interface's statistics or ether_ifattach (that is en1's). */
#ifdef __PRIVATE_SPI__
        netif->configureOutputPullModel(fHalService->getDriverInfo()->getTxQueueSize(), 0, 0, IOEthernetInterface::kOutputPacketSchedulingModelNormal, 0);
#endif
        return true;
    }

    nd = netif->getParameter(kIONetworkStatsKey);
    if (!nd || !(fpNetStats = (IONetworkStats *)nd->getBuffer())) {
        XYLog("network statistics buffer unavailable?\n");
        return false;
    }
    ifp->netStat = fpNetStats;
    ether_ifattach(ifp, OSDynamicCast(IOEthernetInterface, netif));
    fpNetStats->collisions = 0;
#ifdef __PRIVATE_SPI__
    netif->configureOutputPullModel(fHalService->getDriverInfo()->getTxQueueSize(), 0, 0, IOEthernetInterface::kOutputPacketSchedulingModelNormal, 0);
#endif

    return true;
}

IONetworkInterface *AirportItlwm::createInterface()
{
    AirportItlwmEthernetInterface *netif = new AirportItlwmEthernetInterface;
    if (!netif)
        return NULL;
    /* Before init: IONetworkInterface::init publishes the prefix (IOInterfaceNamePrefix) configd names it by. */
    if (creatingAwdlBsd)
        netif->setNamePrefix("awdl");
    if (!netif->initWithSkywalkInterfaceAndProvider(this, creatingAwdlBsd ?
            (IO80211SkywalkInterface *)awdlIf : (IO80211SkywalkInterface *)fNetIf)) {
        netif->release();
        return NULL;
    }
    return netif;
}

bool AirportItlwm::createMediumTables(const IONetworkMedium **primary)
{
    IONetworkMedium    *medium;

    OSDictionary *mediumDict = OSDictionary::withCapacity(2);
    if (mediumDict == NULL) {
        XYLog("Cannot allocate OSDictionary\n");
        return false;
    }

    medium = IONetworkMedium::medium(kIOMediumIEEE80211, 54000000);
    IONetworkMedium::addMedium(mediumDict, medium);
    medium->release();
    if (primary) {
        *primary = medium;
    }
    medium = IONetworkMedium::medium(kIOMediumIEEE80211None, 0);
    IONetworkMedium::addMedium(mediumDict, medium);
    medium->release();

    bool result = publishMediumDictionary(mediumDict);
    if (!result) {
        XYLog("Cannot publish medium dictionary!\n");
    }

    mediumDict->release();
    return result;
}

IOReturn AirportItlwm::selectMedium(const IONetworkMedium *medium) {
    setSelectedMedium(medium);
    return kIOReturnSuccess;
}

bool AirportItlwm::
setLinkStatus(UInt32 status, const IONetworkMedium * activeMedium, UInt64 speed, OSData * data)
{
    struct _ifnet *ifq = &fHalService->get80211Controller()->ic_ac.ac_if;
    if (status == currentStatus) {
        return true;
    }
    bool ret = super::setLinkStatus(status, activeMedium, speed, data);
    currentStatus = status;
    if (fNetIf) {
        if (status & kIONetworkLinkActive) {
#ifdef __PRIVATE_SPI__
            bsdInterface->startOutputThread();
#endif
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkUp, (void *)0);
            /* link quality: handleWCLLink (run by setLinkStateGated above) reports it with setLQM; the pre-Skywalk setLinkQualityMetric(100) is not on this interface */
        } else if (!(status & kIONetworkLinkNoNetworkChange)) {
#ifdef __PRIVATE_SPI__
            bsdInterface->stopOutputThread();
            bsdInterface->flushOutputQueue();
#endif
            ifq_flush(&ifq->if_snd);
            mq_purge(&fHalService->get80211Controller()->ic_mgtq);
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkDown, (void *)fHalService->get80211Controller()->ic_deauth_reason);
        }
    }
    return ret;
}

IOReturn AirportItlwm::
setLinkStateGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, target);
#ifdef AIRPORT_WCL
    that->bsdInterface->setLinkState((IO80211LinkState)(uint64_t)arg0);
    static_cast<AirportItlwmSkywalkInterface *>(that->fNetIf)->handleWCLLink(
        (IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp);
    return kIOReturnSuccess;
#else
    IOReturn ret = that->fNetIf->setLinkState((IO80211LinkState)(uint64_t)arg0, (unsigned int)(uint64_t)arg1);
    that->fNetIf->setRunningState((IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp);
    that->fNetIf->postMessage(APPLE80211_M_LINK_CHANGED, NULL, 0, false);
    that->fNetIf->postMessage(APPLE80211_M_BSSID_CHANGED, NULL, 0, false);
    that->fNetIf->postMessage(APPLE80211_M_SSID_CHANGED, NULL, 0, false);
    if ((IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp) {
        that->fNetIf->reportLinkStatus(3, 0x80);
    } else {
        that->fNetIf->reportLinkStatus(1, 0);
    }
    that->bsdInterface->setLinkState((IO80211LinkState)(uint64_t)arg0);
    return ret;
#endif
}

#ifdef __PRIVATE_SPI__
IOReturn AirportItlwm::outputStart(IONetworkInterface *interface, IOOptionBits options)
{
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    mbuf_t m = NULL;
    if (awdlBsd != NULL && interface == awdlBsd) {
        /*
         * awdl0's packets: each Ethernet frame becomes an AWDL data frame
         * (QoS data, no DS: the peer, our AWDL address, the AWDL BSSID; the
         * AWDL LLC/SNAP header; the AWDL data header with a sequence number
         * and the ethertype; the payload) sent from the listen vdev to the
         * firmware peer the engine had us cache for that address.
         */
        static const uint8_t awdlSnap[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00 };
        static const uint8_t awdlBssidBytes[6] = { 0x00, 0x25, 0x00, 0xff, 0x94, 0x73 };
        uint8_t frame[1600];
        size_t len, off;
        int ret;

        for (;;) {				/* WP4 (M2a): was while (kIOReturnSuccess == interface->dequeueOutputPackets(1, &m)); pacing: no dequeue without room */
            if (awdlPaced() && !awdlLaneHasRoom())
                return kIOReturnNoResources;	/* the next completion (or the gate timer) starts the output thread again */
            if (kIOReturnSuccess != interface->dequeueOutputPackets(1, &m))
                break;
            len = mbuf_pkthdr_len(m);
            qwz_awdlx_handed();	/* M2a (WP3, I10): every packet awdl0 hands us is accounted for exactly once below */
            if (len < 14 || len - 14 + 42 > sizeof(frame) || !awdlLinkUp) {
                qwz_awdlx_drop_pre(len < 14 ? 0 : len - 14 + 42 > sizeof(frame) ? 1 : 2);
                mbuf_freem(m);
                awdlDropped++;
                continue;
            }
            mbuf_copydata(m, 0, len, frame + 28);	/* the Ethernet header lands where the AWDL headers go */
            qwz_awdlx_trace(1, frame + 28, frame + 34, (unsigned)(frame[40] << 8 | frame[41]), frame + 42, len - 14);	/* M2a (WP3, I6): L3/L4 tracer */
            memset(frame, 0, 26);
            frame[0] = 0x88;	/* QoS data */
            memcpy(frame + 4, frame + 28, 6);	/* A1: the destination */
            memcpy(frame + 10, awdlMac, 6);	/* A2: our AWDL address */
            memcpy(frame + 16, awdlBssidBytes, 6);
            off = 26;
            memcpy(frame + off, awdlSnap, 8);
            off += 8;
            frame[off] = 0x03;	/* the AWDL data header: 0x0403, sequence, pad, ethertype */
            frame[off + 1] = 0x04;
            frame[off + 2] = awdlDataSeq & 0xff;
            frame[off + 3] = awdlDataSeq >> 8;
            frame[off + 4] = frame[off + 5] = 0;
            frame[off + 6] = frame[28 + 12];
            frame[off + 7] = frame[28 + 13];
            off += 8;
            awdlDataSeq++;
            memmove(frame + off, frame + 28 + 14, len - 14);
            len = off + len - 14;
            mbuf_freem(m);
            ret = awdlGateOn() ? awdlGateSubmit(frame, len) : static_cast<ItlQwz *>(fHalService)->awdlDataTx(frame, len);	/* WP4 (M2a): the gate holds the frame until A.2 lets it go */
            qwz_awdlx_tx_result(qwz_awdlx_class(frame, len), ret);	/* M2a (WP3, I10) */
            if (ret == 0)
                awdlDataTxCount++;
            else
                awdlDataTxFail++;
            if ((ret == 0 && (awdlDataTxCount <= 6 || (awdlDataTxCount & 255) == 0)) ||
                (ret != 0 && (awdlDataTxFail <= 6 || (awdlDataTxFail & 255) == 0))) {
                qwz_log("airport: awdl0: data tx %s #%u: to %02x:%02x:%02x:%02x:%02x:%02x type %02x%02x, %zu bytes (%d)\n",
                        ret == 0 ? "sent" : "FAILED", ret == 0 ? awdlDataTxCount : awdlDataTxFail,
                        frame[4], frame[5], frame[6], frame[7], frame[8], frame[9], frame[40], frame[41], len, ret);
                qwz_log_publish();
            }
        }
        return kIOReturnSuccess;
    }
    if (ifq_is_oactive(&ifp->if_snd))
        return kIOReturnNoResources;
    while (kIOReturnSuccess == interface->dequeueOutputPackets(1, &m)) {
        if (outputPacket(m, NULL)!= kIOReturnOutputSuccess ||
            ifq_is_oactive(&ifp->if_snd))
            return kIOReturnNoResources;
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::networkInterfaceNotification(
                    IONetworkInterface * interface,
                    uint32_t              type,
                    void *                  argument )
{
    XYLog("%s\n", __FUNCTION__);
    return kIOReturnSuccess;
}
#endif

extern const char* hexdump(uint8_t *buf, size_t len);

#ifdef AIRPORT_WCL
UInt32 AirportItlwm::outputPacket(mbuf_t m, void *param)
{
//    XYLog("%s\n", __FUNCTION__);
    IOReturn ret = kIOReturnOutputSuccess;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;

    if (fHalService->get80211Controller()->ic_state != IEEE80211_S_RUN || ifp->if_snd.queue == NULL) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE)
            freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (!m || mbuf_type(m) == MBUF_TYPE_FREE) {
        ifp->netStat->outputErrors++;
        return kIOReturnOutputDropped;
    }
    if (!(mbuf_flags(m) & MBUF_PKTHDR)) {
        ifp->netStat->outputErrors++;
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (!ifp->if_snd.queue->lockEnqueue(m)) {
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    (*ifp->if_start)(ifp);
    return ret;
}
#else
UInt32 AirportItlwm::outputPacket(mbuf_t m, void *param)
{
//    XYLog("%s\n", __FUNCTION__);
    IOReturn ret = kIOReturnOutputSuccess;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    
    if (fHalService->get80211Controller()->ic_state != IEEE80211_S_RUN || ifp->if_snd.queue == NULL) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE)
            freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (m == NULL) {
        XYLog("%s m==NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        ret = kIOReturnOutputDropped;
    }
    if (!(mbuf_flags(m) & MBUF_PKTHDR) ){
        XYLog("%s pkthdr is NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    if (mbuf_type(m) == MBUF_TYPE_FREE) {
        XYLog("%s mbuf is FREE!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        ret = kIOReturnOutputDropped;
    }
    size_t len = mbuf_len(m);
    ether_header_t *eh = (ether_header_t *)mbuf_data(m);
    if (len >= sizeof(ether_header_t) && eh->ether_type == htons(ETHERTYPE_PAE)) { // EAPOL packet
        const char* dump = hexdump((uint8_t*)mbuf_data(m), len);
        XYLog("output EAPOL packet, len: %zu, data: %s\n", len, dump ? dump : "Failed to allocate memory");
        if (dump)
            IOFree((void*)dump, 3 * len + 1);
    }
    if (!ifp->if_snd.queue->lockEnqueue(m)) {
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    (*ifp->if_start)(ifp);
    return ret;
}
#endif

const OSString * AirportItlwm::newVendorString() const
{
    return OSString::withCString("Apple");
}

const OSString * AirportItlwm::newModelString() const
{
    return OSString::withCString(fHalService->getDriverInfo()->getFirmwareName());
}

IOReturn AirportItlwm::getHardwareAddress(IOEthernetAddress *addrP)
{
    if (IEEE80211_ADDR_EQ(etheranyaddr, fHalService->get80211Controller()->ic_myaddr))
        return kIOReturnError;
    else {
        IEEE80211_ADDR_COPY(addrP, fHalService->get80211Controller()->ic_myaddr);
        return kIOReturnSuccess;
    }
}

IOReturn AirportItlwm::setHardwareAddress(const void *addrP, UInt32 addrBytes)
{
    if (!fNetIf || !addrP)
        return kIOReturnError;
#ifdef AIRPORT_WCL
    const UInt8 *address = static_cast<const UInt8 *>(addrP);
    auto ic = fHalService->get80211Controller();
    bool initializing = (ic->ic_if.if_flags & IFF_UP) && !(ic->ic_if.if_flags & IFF_RUNNING);
    auto update = AirportWCL::planMacUpdate(ic->ic_myaddr, address, addrBytes, initializing);
    if (update == AirportWCL::MacUpdate::Invalid)
        return kIOReturnBadArgument;
    if (update == AirportWCL::MacUpdate::Busy) {
        return kIOReturnBusy;
    }
    if (!bsdInterface || !bsdInterface->getIfnet())
        return kIOReturnNotReady;
    // WCL calls this directly, bypassing IOEthernetInterface's normal MAC ioctl.
    // Update the BSD address used by DHCP/ARP as well as the firmware address.
    errno_t error = ifnet_set_lladdr(bsdInterface->getIfnet(), address, addrBytes);
    if (error)
        return kIOReturnError;
    bsdInterface->setProperty(kIOMACAddress, const_cast<UInt8 *>(address), addrBytes);
    // WCL replays the current address after every driver-available notification.
    // Keep BSD synchronized, but do not restart firmware for a replay.
    if (update == AirportWCL::MacUpdate::Unchanged) {
        return kIOReturnSuccess;
    }
#endif
    if_setlladdr(&fHalService->get80211Controller()->ic_ac.ac_if, (const UInt8 *)addrP);
#ifdef AIRPORT_WCL
    if (ic->ic_if.if_flags & IFF_RUNNING) {
        ic->ic_wcl_mac_reconfig = true;
#else
    if (fHalService->get80211Controller()->ic_state > IEEE80211_S_INIT) {
#endif
        fHalService->disable(bsdInterface);
        fHalService->enable(bsdInterface);
    }
    return kIOReturnSuccess;
}

UInt32 AirportItlwm::getFeatures() const
{
    return fHalService->getDriverInfo()->supportedFeatures();
}

IOReturn AirportItlwm::setPromiscuousMode(IOEnetPromiscuousMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::setMulticastMode(IOEnetMulticastMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::setMulticastList(IOEthernetAddress* addr, UInt32 len)
{
    return fHalService->getDriverController()->setMulticastList(addr, len);
}

IOReturn AirportItlwm::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    IOReturn    rtn = kIOReturnSuccess;
    if (group == gIOEthernetWakeOnLANFilterGroup && magicPacketSupported)
        *filters = kIOEthernetWakeOnMagicPacket;
    else if (group == gIONetworkFilterGroup)
        *filters = kIOPacketFilterMulticast | kIOPacketFilterPromiscuous;
    else
        rtn = IOEthernetController::getPacketFilters(group, filters);
    return rtn;
}

SInt32 AirportItlwm::
enableFeature(IO80211FeatureCode code, void *data)
{
    if (code == kIO80211Feature80211n) {
        return 0;
    }
    return 102;
}

bool AirportItlwm::getLogPipes(CCPipe**logPipe, CCPipe**eventPipe, CCPipe**snapshotsPipe)
{
    bool ret = false;
    if (logPipe) {
        *logPipe = driverLogPipe;
        ret = true;
    }
    if (eventPipe) {
        *eventPipe = driverDataPathPipe;
        ret = true;
    }
    if (snapshotsPipe) {
        *snapshotsPipe = driverSnapshotsPipe;
        ret = true;
    }
    return ret;
}

#define APPLE80211_CAPA_AWDL_FEATURE_AUTO_UNLOCK    0x00000004
#define APPLE80211_CAPA_AWDL_FEATURE_WOW            0x00000080

IOReturn AirportItlwm::
getCARD_CAPABILITIES(OSObject *object,
                                     struct apple80211_capability_data *cd)
{
    uint32_t caps = fHalService->get80211Controller()->ic_caps;
    memset(cd, 0, sizeof(struct apple80211_capability_data));

    if (caps & IEEE80211_C_WEP)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_WEP;
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_TKIP | 1 << APPLE80211_CAP_AES_CCM;
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_PMGT)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_PMGT;
    // if (caps & IEEE80211_C_IBSS)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_IBSS;
    // if (caps & IEEE80211_C_HOSTAP)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_HOSTAP;
    // AES not enabled, like on Apple cards

    if (caps & IEEE80211_C_SHSLOT)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHSLOT - 8);
    if (caps & IEEE80211_C_SHPREAMBLE)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHPREAMBLE - 8);
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_WPA1 - 8) | 1 << (APPLE80211_CAP_WPA2 - 8) | 1 << (APPLE80211_CAP_TKIPMIC - 8);
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_TXPMGT)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_TXPMGT - 8);
    // if (caps & IEEE80211_C_MONITOR)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_MONITOR - 8);
    // WPA not enabled, like on Apple cards

    cd->version = APPLE80211_VERSION;
#ifdef AIRPORT_WCL
    // WPA2 can use TKIP for group traffic even when unicast uses CCMP.
    // net80211 handles TKIP/MIC in software; hiding it makes Apple80211
    // reject these networks before it submits an association request.
    cd->capabilities[0] = (caps & IEEE80211_C_RSN) ?
        (1 << APPLE80211_CAP_AES_CCM) | (1 << APPLE80211_CAP_TKIP) : 0;
    cd->capabilities[1] &= ~(1 << (APPLE80211_CAP_WPA1 - 8));
    cd->capabilities[5] = 0x40; // Scan completion events.
    cd->capabilities[2] = (1 << (APPLE80211_CAP_WME - 16)) |
        (1 << (APPLE80211_CAP_SHORT_GI_40MHZ - 16)) |
        (1 << (APPLE80211_CAP_SHORT_GI_20MHZ - 16));
    // Tahoe WCLDeviceConfiguration::isHeSupported reads byte 17, bit 7.
    // WCL uses this capability together with the BSS HE IE for PHY reporting.
    if (caps & IEEE80211_C_HE)
        cd->capabilities[17] |= 0x80;

    // Capability index 32: wifip2pd only requests an AWDL interface when it is set,
    // and System Information prints "AirDrop: Supported" from the same index.
    cd->capabilities[4] |= 0x01;
    // Capability index 29: dual band (the pre-WCL branch below sets it in 0x2B). Without
    // it IO80211AWDLPeerManager::configureValidAWDLChannels logs "Dual band not
    // supported" and zeroes the 5 GHz channels, so awdl0 runs on channel 6 alone
    // while every peer here is on 149.
    cd->capabilities[3] |= 0x20;
    // Capability index 73 (byte 9, mask 0x02): Apple80211Scan in the host framework tests it when the scan
    // parameters hold an SSID or BSSID list (the tether scan of Instant Hotspot). Clear, such a scan dies
    // with -3903 inside IO80211 and never reaches setWCL_SCAN_REQ. Apple's own driver seeds the bit
    // unconditionally. debug.qwz.cap73 (default 1); 0 gives the old blob byte for byte. The framework keeps
    // its own copy: a Wi-Fi toggle, an airportd restart or a reboot is needed to see a change.
    if (qwz_cap73)
        cd->capabilities[9] |= 0x02;
    // M0 (C30): debug.qwz.capsae, a discovery matrix for the SAE join (nothing in M0 implements SAE; the matrix tells which
    // bits make macOS issue it). 0 leaves the blob byte-identical; read at every call and applied after the assignments above.
    // 1: the Ersiter route (caps[6] 0x01, caps[9] 0x08), 2: the MtkWireless route (caps[2] 0x01, caps[5] 0x08), 3: both.
    // Reset to 0 after each cell (T9).
    if (qwz_capsae == 1 || qwz_capsae == 3) {
        cd->capabilities[6] |= 0x01;
        cd->capabilities[9] |= 0x08;
    }
    if (qwz_capsae == 2 || qwz_capsae == 3) {
        cd->capabilities[2] |= 0x01;
        cd->capabilities[5] |= 0x08;
    }
#else
    cd->capabilities[2] = 0xFF; // BURST, WME, SHORT_GI_40MHZ, SHORT_GI_20MHZ, WOW, TSN, ?, ?
    cd->capabilities[3] = 0x2B;
    cd->capabilities[5] = 0x40;
    cd->capabilities[6] = (
//                           1 |    //MFP capable
                           0x8 |
                           0x4 |
                           0x80
                           );
    *(uint16_t *)&cd->capabilities[8] = 0x201;
//
//    cd->capabilities[2] |= 0x10;
//    cd->capabilities[5] |= 0x1;
//
//    cd->capabilities[2] |= 0x2;
//
//    cd->capabilities[3] |= 0x20;
//
//    cd->capabilities[0] |= 0x80;
//
//    cd->capabilities[3] |= 0x80;
//    cd->capabilities[4] |= 0x4;
//
//    cd->capabilities[4] |= 0x1;
//    cd->capabilities[3] |= 0x1;
//    cd->capabilities[6] |= 0x8;
//
//    cd->capabilities[3] |= 3;
//    cd->capabilities[4] |= 2;
//    cd->capabilities[6] |= 0x10;
//    cd->capabilities[5] |= 0x20;
//    cd->capabilities[5] |= 0x80;
//
//    if (cd->capabilities[6] & 0x20) {
//        cd->capabilities[2] |= 8;
//    }
//    cd->capabilities[5] |= 8;
//    cd->capabilities[8] |= 2;
//
//    cd->capabilities[11] |= (2 | 4 | 8 | 0x10 | 0x20 | 0x40 | 0x80);

#endif
    {
        // M0 (C30): is the capability blob re-read on a Wi-Fi toggle? One line per call (rate limited), with the bytes that matter.
        static unsigned int calls;
        if (qwz_log_ratelimit(&calls, 200, 20))
            qwz_log("airport: getCARD_CAPABILITIES #%u (capsae %d): caps[2] %02x [3] %02x [4] %02x [5] %02x [6] %02x [9] %02x [17] %02x\n",
                    calls, qwz_capsae, cd->capabilities[2], cd->capabilities[3], cd->capabilities[4], cd->capabilities[5],
                    cd->capabilities[6], cd->capabilities[9], cd->capabilities[17]);
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getDRIVER_VERSION(OSObject *object,
                                  struct apple80211_version_data *hv)
{
    if (!hv)
        return kIOReturnError;
    hv->version = APPLE80211_VERSION;
    snprintf(hv->string, sizeof(hv->string), "itlwm: %s%s fw: %s", ITLWM_VERSION, GIT_COMMIT, fHalService->getDriverInfo()->getFirmwareVersion());
    hv->string_len = strlen(hv->string);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getHARDWARE_VERSION(OSObject *object,
                                    struct apple80211_version_data *hv)
{
    if (!hv)
        return kIOReturnError;
    hv->version = APPLE80211_VERSION;
    strncpy(hv->string, fHalService->getDriverInfo()->getFirmwareVersion(), sizeof(hv->string));
    hv->string_len = strlen(fHalService->getDriverInfo()->getFirmwareVersion());
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getCOUNTRY_CODE(OSObject *object,
                                struct apple80211_country_code_data *cd)
{
    char user_override_cc[3];
    const char *cc_fw = fHalService->getDriverInfo()->getFirmwareCountryCode();

    if (!cd)
        return kIOReturnError;
    cd->version = APPLE80211_VERSION;
    memset(user_override_cc, 0, sizeof(user_override_cc));
    PE_parse_boot_argn("itlwm_cc", user_override_cc, 3);
    /* user_override_cc > firmware_cc > geo_location_cc */
    strncpy((char*)cd->cc, user_override_cc[0] ? user_override_cc : ((cc_fw[0] == 'Z' && cc_fw[1] == 'Z' && geo_location_cc[0]) ? geo_location_cc : cc_fw), sizeof(cd->cc));
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
setCOUNTRY_CODE(OSObject *object, struct apple80211_country_code_data *data)
{
#ifdef AIRPORT_WCL
    if (!data)
        return kIOReturnBadArgument;
    if (data->cc[0] != 'x' && data->cc[0] != 'X' &&
        memcmp(geo_location_cc, data->cc, sizeof(geo_location_cc)) != 0) {
        memcpy(geo_location_cc, data->cc, sizeof(geo_location_cc));
        if (fNetIf)
            static_cast<AirportItlwmSkywalkInterface *>(fNetIf)->handleWCLEvent(
                IEEE80211_EVT_COUNTRY_CODE_UPDATE, nullptr);
    }
#else
    XYLog("%s cc=%s\n", __FUNCTION__, data->cc);
    if (data && data->cc[0] != 120 && data->cc[0] != 88) {
        memcpy(geo_location_cc, data->cc, sizeof(geo_location_cc));
        fNetIf->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, NULL, 0, 0);
    }
#endif
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
    if (!pd)
        return kIOReturnError;
    pd->version = APPLE80211_VERSION;
    pd->num_radios = 4;
    pd->power_state[0] = power_state;
    pd->power_state[1] = power_state;
    pd->power_state[2] = power_state;
    pd->power_state[3] = power_state;
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
setPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
#ifdef AIRPORT_WCL
    if (!pd)
        return kIOReturnError;
    IOLog("itlwm: setPOWER: num_radios[%d]  power_state(0:%u  1:%u  2:%u  3:%u)\n", pd->num_radios, pd->power_state[0], pd->power_state[1], pd->power_state[2], pd->power_state[3]);
    if (pd->num_radios > 0) {
        bool isRunning = (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & IFF_RUNNING) != 0;
        /* M0 (C16): which thread delivers setPOWER (architecture-map question 8) */
        qwz_log("qwz: pm: setPOWER(%u) on thread %llx, workloop thread %d, gate held %d, isRunning %d, chipBusy %d\n", pd->power_state[0],
                (unsigned long long)thread_tid(current_thread()), onWorkLoopThread(),
                pmGateHeld(), isRunning, chipBusy);
        /* The requested state counts from here on, through the long teardown or recovery below. */
        power_state = (pd->power_state[0]);
        if (pd->power_state[0] == 0) {
            bool isUp = (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & (IFF_UP | IFF_RUNNING)) != 0;
            /*
             * Wi-Fi off is a real reset (stop the firmware, power the chip down
             * as for system sleep), but only when there is something to reset:
             * at boot with Wi-Fi left off the chip was never used, and the
             * workloop thread cannot sleep for the teardown.
             */
            bool reset = !noWakeFix && !noRadioReset && (isUp || chipUsed) && !onWorkLoopThread();

            fHalService->get80211Controller()->ic_wcl_mac_reconfig = false;
            changePowerStateToPriv(kPowerStateOff);
            if (isUp)
                net80211_ifstats(fHalService->get80211Controller());
            if (reset) {
                /*
                 * If the power-state machine also runs setPowerStateOff for the
                 * change above, its chip-down finds the chip down and does nothing.
                 */
                _fCommandGate->runAction(radioOffGated);
            } else if (isUp) {
                if (onWorkLoopThread()) {
                    /* M0 (C16): disableAdapter reaches rw_enter_write and qwz_stop, which sleep: not on this thread */
                    qwz_log("qwz: pm: setPOWER(0) on the workloop thread: Wi-Fi off queued on a thread that may sleep\n");
                    scheduleRadioOff();
                } else
                    disableAdapter(bsdInterface);
            }
        } else {
            changePowerStateToPriv(kPowerStateOn);
            engineRearm("setPOWER(1)");		/* M0 (C15): a new user action is a new episode */
            if (!isRunning)
                enableAdapter(bsdInterface, kWakeViaSetPower);
        }
    }

    return kIOReturnSuccess;
#else
    if (!pd)
        return kIOReturnError;
    IOLog("itlwm: setPOWER: num_radios[%d]  power_state(0:%u  1:%u  2:%u  3:%u)\n", pd->num_radios, pd->power_state[0], pd->power_state[1], pd->power_state[2], pd->power_state[3]);
    if (pd->num_radios > 0) {
        bool isRunning = (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & (IFF_UP | IFF_RUNNING)) != 0;
        power_state = (pd->power_state[0]);
        if (pd->power_state[0] == 0) {
            changePowerStateToPriv(1);
            if (isRunning) {
                net80211_ifstats(fHalService->get80211Controller());
                disableAdapter(bsdInterface);
            }
        } else {
            changePowerStateToPriv(2);
            if (!isRunning)
                enableAdapter(bsdInterface, kWakeViaSetPower);
        }
    }

    return kIOReturnSuccess;
#endif
}

SInt32 AirportItlwm::apple80211_ioctl(IO80211SkywalkInterface *interface,unsigned long cmd,void *data, bool b1, bool b2)
{
    if (!ml_at_interrupt_context())
        XYLog("%s cmd: %s b1: %d b2: %d\n", __FUNCTION__, convertApple80211IOCTLToString((unsigned int)cmd), b1, b2);
#ifdef AIRPORT_WCL
    return kIOReturnUnsupported;
#else
    return super::apple80211_ioctl(interface, cmd, data, b1, b2);
#endif
}

SInt32 AirportItlwm::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data)
{
    if (!ml_at_interrupt_context())
        XYLog("%s 1 cmd: %s request: %d\n", __FUNCTION__, convertApple80211IOCTLToString(cmd), request);
    return kIOReturnUnsupported;
}

SInt32 AirportItlwm::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data,void *)
{
    if (!ml_at_interrupt_context())
        XYLog("%s 2 cmd: %s request: %d\n", __FUNCTION__, convertApple80211IOCTLToString(cmd), request);
    return kIOReturnUnsupported;
}

/* Indexed by kWakeVia*: the "recovery run from ..." log line. */
static const char *const wakeViaName[] = {
    "power-state thread", "setPOWER", "enable()", "retry", "data-stall restart", "deferred request", "dead-firmware restart"
};
static_assert(sizeof(wakeViaName) / sizeof(wakeViaName[0]) == kWakeViaCount,
              "wakeViaName[] needs one entry per kWakeVia* value");

/* M2a WP0: indexed by kWakeVia*: the "engine: recovered by ..." line of a chip-up that closed an open episode (the engine's own via has its line in engineRun) */
static const char *const wakeViaDoneName[] = {
    "wake", "Wi-Fi on", "enable()", "retry", "data-stall restart", "deferred request", "dead-firmware restart"
};
static_assert(sizeof(wakeViaDoneName) / sizeof(wakeViaDoneName[0]) == kWakeViaCount,
              "wakeViaDoneName[] needs one entry per kWakeVia* value");

/* How long a thread waits for a running chip transition (the gate is released meanwhile). */
#define QWZ_CHIP_WAIT_MS	10000
#define QWZ_PM_WAIT_MS		4000	/* the power-state thread: its acknowledge budget is 15 s / 20 s */
#define QWZ_PM_QUIESCE_SKIP_MS	6000	/* no quiesce wait if the power-down thread already used this much */
#define QWZ_QUIESCE_MS		3000
/*
 * M0 (C13, S19): the chip-transition deadlines the sentinel (C14) enforces, each about ten times the
 * measured healthy time (chip-down 1.19 s, wake 0.81 s). The two PM limits are timed from the power
 * change request (pmAckStartMs), so the claim wait of up to QWZ_PM_WAIT_MS is inside them; the others
 * from the claim. The sentinel ticks once a second, so DEAD arrives up to 1 s after a limit: 8.0 to 9.0 s
 * (sleep) and 12.0 to 13.0 s (wake) after the request, then the unwind (at most about 3 s, an estimate)
 * against the 15 s and 20 s acknowledge budgets (setPowerState): margins of about 3 s and 4 s.
 */
#define QWZ_DEADLINE_SLEEP_MS	8000	/* system sleep on the power-state thread, from the request */
#define QWZ_DEADLINE_PMWAKE_MS	12000	/* system wake on the power-state thread, from the request */
#define QWZ_DEADLINE_OFF_MS	12000	/* Wi-Fi off, restart teardown, data-stall teardown, from the claim (also the claim's default) */
#define QWZ_DEADLINE_WAKE_MS	15000	/* every other bring-up (engine, setPOWER(1), retry), from the claim */

static uint64_t pmNowMs(void)
{
    uint64_t t;

    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &t);
    return t / 1000000ULL;
}

static uint32_t waitMsFor(int via)
{
    return via == kWakeViaPmThread ? QWZ_PM_WAIT_MS : QWZ_CHIP_WAIT_MS;
}

/* The bare enable: wakeRecover (and the build-2 handlers) end up here. */
IOReturn AirportItlwm::enableAdapterNow(IONetworkInterface *netif)
{
#ifdef AIRPORT_WCL
    IOReturn status = fHalService->enable(netif);
    if (status != kIOReturnSuccess)
        return status;
#else
    fHalService->enable(netif);
#endif
    chipUsed = true;
    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
    watchdogTimer->enable();
    return kIOReturnSuccess;
}

/*
 * Every enable request lands here. After a chip power-down (sleep, Wi-Fi off,
 * data-stall restart) a bare enable would run qwz_init on a chip nobody has
 * brought back yet: airportd's setPOWER(1) did that before the power-state
 * thread restored the PCI state. Whoever asks first runs the whole recovery.
 */
IOReturn AirportItlwm::enableAdapter(IONetworkInterface *netif, int via)
{
    if (noWakeFix)
        return enableAdapterNow(netif);
    return _fCommandGate->runAction(enableAdapterGated, netif, (void *)(intptr_t)via);
}

IOReturn AirportItlwm::enableAdapterGated(OSObject *owner, void *netif, void *via, void *, void *)
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);

    if (self == NULL || self->fHalService == NULL)
        return kIOReturnError;
    if (!self->chipWait(wakeViaName[(intptr_t)via], waitMsFor((int)(intptr_t)via)))
        return kIOReturnBusy;
    if (self->fHalService->wakeNeedsRecovery()) {
        if ((intptr_t)via == kWakeViaSetPower &&
            (self->pmPowerState != kPowerStateOn || self->sleepPending)) {
            /*
             * airportd's setPOWER(1) can arrive before the system wake reaches
             * this driver, with the bridge above the chip not restored yet
             * (brctl 0000): the firmware boot then failed with 'link down
             * error during global reset'. setPowerStateOn runs the recovery.
             */
            qwz_log("qwz: pm: wake: setPOWER(1) came before the system wake reached the driver: the power-state thread recovers\n");
            qwz_log_publish();
            return kIOReturnNotReady;
        }
        /* The firmware init sleeps: the workloop thread cannot run it. A thread call will. */
        if (self->onWorkLoopThread()) {
            qwz_log("qwz: pm: wake: %s asked on the workloop thread: queueing the recovery on a thread that may sleep\n",
                    wakeViaName[(intptr_t)via]);
            qwz_log_publish();
            self->scheduleWakeRetry(100, kWakeViaDeferred);
            return kIOReturnBusy;
        }
        self->wakeRecover((int)(intptr_t)via);
        return self->fHalService->wakeNeedsRecovery() ? kIOReturnNotReady : kIOReturnSuccess;
    }
    /*
     * fix2 (review round 2, dead-R2-1): the bare enable is a chip-up too when the firmware is DEAD but nothing tore the chip down (the
     * engine gave up, or is in its cool-down, so wakeNeeded is false): qwz_init's "init with a DEAD firmware" branch restarts the
     * firmware and clears DEAD, and this path never reaches wakeRecover's tail. Close the episode the restart ended here too, as the
     * wake does; an ordinary interface enable finds no episode open and wakeCloseEpisode returns at once.
     */
    uint64_t t0 = pmNowMs();
    IOReturn r = self->enableAdapterNow((IONetworkInterface *)netif);

    if (r == kIOReturnSuccess)
        self->wakeCloseEpisode((int)(intptr_t)via, pmNowMs() - t0);
    return r;
}

void AirportItlwm::disableAdapter(IONetworkInterface *netif)
{
    watchdogTimer->cancelTimeout();
    watchdogTimer->disable();
    fHalService->disable(netif);
}

IOReturn AirportItlwm::
tsleepHandler(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3)
{
    AirportItlwm* dev = OSDynamicCast(AirportItlwm, owner);
    if (dev == 0)
        return kIOReturnError;

    if (arg1 == 0) {
        if (_fCommandGate->commandSleep(arg0, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    } else {
        AbsoluteTime deadline;
        clock_interval_to_deadline((*(int*)arg1), kNanosecondScale, reinterpret_cast<uint64_t*> (&deadline));
        if (_fCommandGate->commandSleep(arg0, deadline, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    }
}

bool AirportItlwm::initPCIPowerManagment(IOPCIDevice *provider)
{
    UInt16 reg16;

    reg16 = provider->configRead16(kIOPCIConfigCommand);

    reg16 |= ( kIOPCICommandBusMaster       |
               kIOPCICommandMemorySpace     |
               kIOPCICommandMemWrInvalidate );

    reg16 &= ~kIOPCICommandIOSpace;  // disable I/O space

    provider->configWrite16( kIOPCIConfigCommand, reg16 );
    provider->findPCICapability(kIOPCIPowerManagementCapability,
                                &pmPCICapPtr);
    if (pmPCICapPtr) {
        UInt16 pciPMCReg = provider->configRead32( pmPCICapPtr ) >> 16;
        if (pciPMCReg & kPCIPMCPMESupportFromD3Cold)
            magicPacketSupported = true;
        provider->configWrite16((pmPCICapPtr + 4), 0x8000 );
        IOSleep(10);
    }
    return true;
}

static IOPMPowerState powerStateArray[kPowerStateCount] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

/*
 * M0 (R1-03): stop() waits for a firmware restart in flight (engineBusy) and wants every wait of it to end now.
 * DEAD ends the firmware waits and the power-up hold. qwz_init_impl clears DEAD at the start of its full init (the one
 * place DEAD is cleared), so a restart that is still on its way to that init would run its whole bring-up against a flag
 * that was set for nothing: declare again whenever the flag is clear. The sleepers are flushed again only while DEAD
 * is set: a wake-all with DEAD clear hands a fresh init wait a spurious success (spec C15, retry rule). Gate-free and
 * never blocking, like the sentinel's calls; pmStopping is already set, so the DEAD callback (faultCb -> engineRequest)
 * cannot start another run.
 */
static void stopEndRestart(ItlHalService *hal)
{
    if (!hal->faultPending(NULL))
        hal->declareDead(QWZ_DEAD_USER, "stop");
    else
        hal->releaseWaiters();
}

/*
 * Stop every thread call and wait for one that is running: the handlers use
 * the gate and the HAL, which the teardown below frees. Idempotent.
 */
void AirportItlwm::quiescePM()
{
    thread_call_t calls[] = { wakeRetryCall, stallCall, powerOffThreadCall, powerOnThreadCall, deadCall, radioOffCall };
    bool first = !pmStopping;	/* M0 (R1-03): stop() is the first caller; releaseAll() and unregistPM() call this again and must not wait again (a stuck restart would hold stop() for 3 x 20 s) */

    pmStopping = true;
    __atomic_store_n(&qwz_pm_pending, 1, __ATOMIC_RELEASE);	/* HF-1: first, before stopEndRestart and the cancel_wait loop below wait for a run that sits in a late-reset pause: the pause ends within 100 ms */
    /*
     * M0 (C14, R1-03): the sentinel is NOT stopped here. Its deadlines (8, 12, 15 s) and its waiter flush keep bounding
     * every chip transition that the thread_call_cancel_wait calls and the engine wait below wait for; stopSentinel() is
     * the LAST statement of this function. pmStopping makes engineRequest, scheduleEngine and scheduleRadioOff refuse,
     * so a tick in this window can declare DEAD but cannot start a new engine run, and sentinelFired tests
     * sentinelStopping, not pmStopping.
     */
    if (first && fHalService != NULL && __atomic_load_n(&engineBusy, __ATOMIC_ACQUIRE)) {
        qwz_logc("qwz: engine: stop: a firmware restart is in flight: ending its waits (DEAD, reason USER)\n");
        stopEndRestart(fHalService);	/* before the cancel_wait loop: that wait blocks for the natural duration of a run that has no second invocation queued */
    }
    if (watchdogTimer) {
        watchdogTimer->cancelTimeout();
        watchdogTimer->disable();
    }
    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        /* TRUE: it was queued and is cancelled now; FALSE: idle, or it ran to the end meanwhile */
        if (calls[i] != NULL && thread_call_cancel_wait(calls[i]))
            release();	/* the cancelled call's reference */
    }
    stallPending = false;
    /*
     * M0 (C15, third-auditor finding): thread_call_cancel_wait waits only for the most recent invocation
     * of a call, and deadCall is not THREAD_CALL_OPTIONS_ONCE, so an older engine run (a second, absorbed
     * invocation was queued behind it) can still be inside engineRun, using the HAL and the gate, while
     * stop() goes on to detach and release them. engineBusy is true for exactly that time: wait for it
     * (stop() holds no gate). M0 (R1-03): the wait is bounded by the sentinel's deadlines, which are still ticking, and by
     * the DEAD declared above and re-declared every 500 ms, not by the claim that every wait of a restart is short. Only
     * the first quiescePM call waits.
     */
    if (first) {
        unsigned n;

        for (n = 0; n < 2000 && __atomic_load_n(&engineBusy, __ATOMIC_ACQUIRE); n++) {
            if (n == 0)
                qwz_log("qwz: engine: stop: waiting for a restart in flight to finish\n");
            else if ((n % 50) == 0 && fHalService != NULL)
                stopEndRestart(fHalService);	/* re-declare if the restart's own init cleared DEAD, else flush the sleepers again */
            IOSleep(10);
        }
        if (__atomic_load_n(&engineBusy, __ATOMIC_ACQUIRE))
            qwz_logc("qwz: engine: stop: the restart did not finish in %u ms: stop() goes on and the restart may touch freed state\n", n * 10);
        else if (n != 0)
            qwz_log("qwz: engine: stop: the restart finished after %u ms\n", n * 10);
    }
    stopSentinel();	/* M0 (C14, R1-03): LAST: returns after a tick in flight has finished; the HAL outlives the last tick (stop() detaches it afterwards) */
}

void AirportItlwm::unregistPM()
{
    quiescePM();
    if (wakeRetryCall) {
        thread_call_free(wakeRetryCall);
        wakeRetryCall = NULL;
    }
    if (stallCall) {
        thread_call_free(stallCall);
        stallCall = NULL;
    }
    if (powerOffThreadCall) {
        thread_call_free(powerOffThreadCall);
        powerOffThreadCall = NULL;
    }
    if (powerOnThreadCall) {
        thread_call_free(powerOnThreadCall);
        powerOnThreadCall = NULL;
    }
    if (deadCall) {		/* M0 (C15) */
        thread_call_free(deadCall);
        deadCall = NULL;
    }
    if (radioOffCall) {		/* M0 (C16) */
        thread_call_free(radioOffCall);
        radioOffCall = NULL;
    }
}

IOReturn AirportItlwm::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{
    IOReturn result = IOPMAckImplied;

    if (pmStopping)
        return result;
    /* Before the early return below: a queued stall restart must see the sleep coming, and the wake clears it. */
    if (powerStateOrdinal == kPowerStateOff) {
        sleepPending = true;
        __atomic_store_n(&qwz_pm_pending, 1, __ATOMIC_RELEASE);	/* HF-3: an attempt that sits in a late-reset pause ends it within 100 ms and finishes inside the power-state thread's 4 s claim wait */
    } else if (powerStateOrdinal == kPowerStateOn) {
        sleepPending = false;
        __atomic_store_n(&qwz_pm_pending, 0, __ATOMIC_RELEASE);
    }
    if (pmPowerState == powerStateOrdinal)
        return result;
    /*
     * M0 (C13, S19): the clock of the power-state thread's chip-transition deadline starts at the power
     * change request, before the thread-call latency and the runAction wait of the handler (the thread
     * call is entered only below): the claim wait is inside the deadline and the deadline plus the unwind
     * fit the acknowledge budgets below.
     */
    __atomic_store_n(&pmAckStartMs, pmNowMs(), __ATOMIC_RELEASE);
    switch (powerStateOrdinal) {
        case kPowerStateOff:
            if (powerOffThreadCall) {
                retain();
                if (thread_call_enter(powerOffThreadCall))
                    release();
                result = 15000000;	/* chip power-down before sleep: QMI stop + MHI M3 poll */
            }
            break;
        case kPowerStateOn:
            if (powerOnThreadCall) {
                retain();
                if (thread_call_enter(powerOnThreadCall))
                    release();
                result = 20000000;	/* wake recovery: PCIe link + full firmware load */
            }
            break;

        default:
            break;
    }
    return result;
}

IOReturn AirportItlwm::setWakeOnMagicPacket(bool active)
{
    magicPacketEnabled = active;
    return kIOReturnSuccess;
}

static void handleSetPowerStateOff(thread_call_param_t param0,
                             thread_call_param_t param1)
{
    AirportItlwm *self = (AirportItlwm *)param0;

    if (param1 == 0)
    {
        if (!self->pmReady())
        {
            /* Stopping: no gate or HAL to work with, but the power change still wants its acknowledge. */
            self->pmAcknowledge();
            self->release();
            return;
        }
        self->getCommandGate()->runAction((IOCommandGate::Action)
                                           handleSetPowerStateOff,
                                           (void *) 1);
    }
    else
    {
        self->setPowerStateOff();
        self->release();
    }
}

static void handleSetPowerStateOn(thread_call_param_t param0,
                            thread_call_param_t param1)
{
    AirportItlwm *self = (AirportItlwm *) param0;

    if (param1 == 0)
    {
        if (!self->pmReady())
        {
            self->pmAcknowledge();
            self->release();
            return;
        }
        self->getCommandGate()->runAction((IOCommandGate::Action)
                                           handleSetPowerStateOn,
                                           (void *) 1);
    }
    else
    {
        self->setPowerStateOn();
        self->release();
    }
}

static void handleWakeRetry(thread_call_param_t param0, thread_call_param_t param1);
static void handleStallRestart(thread_call_param_t param0, thread_call_param_t param1);
static void handleEngine(thread_call_param_t param0, thread_call_param_t param1);	/* M0 (C15) */
static void handleRadioOff(thread_call_param_t param0, thread_call_param_t param1);	/* M0 (C16) */

IOReturn AirportItlwm::registerWithPolicyMaker(IOService *policyMaker)
{
    IOReturn ret;

    pmPowerState = kPowerStateOn;
    __atomic_store_n(&qwz_pm_pending, 0, __ATOMIC_RELEASE);	/* a new instance: the flag of a stopped one is stale */
    pmPolicyMaker = policyMaker;

    powerOffThreadCall = thread_call_allocate(
                                            (thread_call_func_t)handleSetPowerStateOff,
                                            (thread_call_param_t)this);
    wakeRetryCall = thread_call_allocate((thread_call_func_t)handleWakeRetry,
                                         (thread_call_param_t)this);
    stallCall = thread_call_allocate((thread_call_func_t)handleStallRestart,
                                     (thread_call_param_t)this);
    /* M0: the dead-firmware engine (C15) and the Wi-Fi-off hop off the workloop thread (C16); neither is THREAD_CALL_OPTIONS_ONCE (spec section 12, item 13) */
    deadCall = thread_call_allocate((thread_call_func_t)handleEngine,
                                    (thread_call_param_t)this);
    radioOffCall = thread_call_allocate((thread_call_func_t)handleRadioOff,
                                        (thread_call_param_t)this);
    powerOnThreadCall  = thread_call_allocate(
                                            (thread_call_func_t)handleSetPowerStateOn,
                                              (thread_call_param_t)this);
    ret = pmPolicyMaker->registerPowerDriver(this,
                                             powerStateArray,
                                             kPowerStateCount);
    return ret;
}

bool AirportItlwm::pmReady()
{
    return !pmStopping && fHalService != NULL && getCommandGate() != NULL;
}

void AirportItlwm::pmAcknowledge()
{
    if (pmPolicyMaker != NULL)
        pmPolicyMaker->acknowledgeSetPowerState();
}

bool AirportItlwm::onWorkLoopThread()
{
    IOWorkLoop *wl = _fCommandGate != NULL ? _fCommandGate->getWorkLoop() : NULL;

    return wl != NULL && wl->onThread();
}

/* A recovery may run: the system is awake and Wi-Fi is (or, without the radio reset, need not be) on. */
bool AirportItlwm::wakeAllowed()
{
    return !pmStopping && !sleepPending && pmPowerState == kPowerStateOn &&
        (power_state || noRadioReset);
}

/*
 * The acknowledge budgets are 15 s (off) and 20 s (on); the power-state thread
 * therefore waits at most QWZ_PM_WAIT_MS for a transition another thread is
 * running, and skips the quiesce wait if that wait used most of its time.
 * The acknowledge happens exactly once on every path, after the work.
 */
void AirportItlwm::setPowerStateOff()
{
    uint64_t start = pmNowMs();

    pmPowerState = kPowerStateOff;
    if (noWakeFix) {
        disableAdapter(bsdInterface);
        pmAcknowledge();
        return;
    }
    cancelWakeRetry();
    cancelStallRestart();
    stallRestarts = 0;		/* a new wake/boot window */
    stallNoted = false;
    wakeRetryCount = 0;
    engineRearm("system sleep");	/* M0 (C15): a system action is a new episode */
    if (!chipDown("system sleep", "system wake", QWZ_PM_WAIT_MS, start)) {
        qwz_log("qwz: pm: system sleep: the chip was not powered down by this thread (see above); acknowledging anyway\n");
        qwz_log_publish();
    }
    pmAcknowledge();
}

void AirportItlwm::setPowerStateOn()
{
    pmPowerState = kPowerStateOn;
    if (noWakeFix) {
        enableAdapterNow(bsdInterface);
        pmAcknowledge();
        return;
    }
    engineRearm("system wake");		/* M0 (C15): a system action is a new episode */
    if (fHalService->wakeNeedsRecovery()) {
        if (!power_state && !noRadioReset)
            qwz_log("qwz: pm: wake: Wi-Fi is off: the chip stays down until it is switched on\n");
        else if (!wakeRecover(kWakeViaPmThread)) {
            /* Another thread owns the chip and may fail or be cut off: make sure someone looks again. */
            qwz_log("qwz: pm: wake: the chip is busy with another transition: a recovery check is queued\n");
            scheduleWakeRetry(5000, kWakeViaDeferred);
        }
    } else if (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & IFF_RUNNING) {
        /* airportd's setPOWER(1) got here first and recovered the chip. */
        qwz_log("qwz: pm: wake: already recovered; the power-state thread only acknowledges\n");
    } else
        enableAdapter(bsdInterface, kWakeViaPmThread);
    qwz_log_publish();
    pmAcknowledge();
}

/*
 * Wait, with the gate released, until no chip power transition is running;
 * false if that took longer than maxMs, or if this is the workloop thread
 * (which must stay free for the interrupts the other thread is waiting for).
 */
bool AirportItlwm::chipWait(const char *who, uint32_t maxMs)
{
    uint64_t deadline;

    if (!chipBusy)
        return true;
    if (onWorkLoopThread()) {
        qwz_log("qwz: pm: %s: a chip power transition is running and this is the workloop thread: not waiting\n", who);
        return false;
    }
    qwz_log("qwz: pm: %s: a chip power transition is running; waiting up to %u ms for it\n", who, (unsigned)maxMs);
    qwz_log_publish();
    clock_interval_to_deadline(maxMs, NSEC_PER_MSEC, &deadline);
    while (chipBusy) {
        /*
         * M0 (C13): the kernel answers an expired deadline with THREAD_TIMED_OUT (1), not kIOReturnTimeout,
         * so the old test never matched and this loop spun on an expired deadline (the ring showed the raw
         * value as "commandSleep returned 0x1"). A passed deadline ends it whatever the code.
         */
        IOReturn r = _fCommandGate->commandSleep(&chipBusy, deadline, THREAD_UNINT);
        uint64_t now;

        if (r == THREAD_AWAKENED)
            continue;
        clock_get_uptime(&now);
        if (r == THREAD_TIMED_OUT || r == kIOReturnTimeout || now >= deadline) {
            qwz_log("qwz: pm: %s: gave up waiting for the running chip power transition (%u ms)\n", who, (unsigned)maxMs);
            qwz_log_publish();
            return false;
        }
    }
    return true;
}

/*
 * M0 (C13): the claim also records who holds the chip, since when and with which limit; the sentinel
 * (C14) reads them to declare DEAD when a transition overruns. The fields are written BEFORE chipBusy is
 * published (release store) and the sentinel reads chipBusy first (acquire), otherwise it could pair a
 * fresh chipBusy with a stale start time. sinceMs and limitMs are arguments, published together with
 * chipBusy: the power-state thread passes the time of its power change request (S19); 0 means "now".
 */
bool AirportItlwm::chipClaim(const char *who, uint32_t maxMs, uint64_t sinceMs, uint32_t limitMs)
{
    if (!chipWait(who, maxMs))
        return false;
    chipBusyPm = sinceMs != 0;			/* the power-state thread: its clock started at the power change request */
    chipBusySince = sinceMs != 0 ? sinceMs : pmNowMs();
    chipBusyWho = who;
    chipBusyLimitMs = limitMs != 0 ? limitMs : QWZ_DEADLINE_OFF_MS;
    deadlineFired = false;
    __atomic_store_n(&chipBusy, true, __ATOMIC_RELEASE);
    return true;
}

void AirportItlwm::chipRelease()
{
    chipBusy = false;
    _fCommandGate->commandWakeup(&chipBusy);
}

/*
 * Interface down and chip power-down: what system sleep does (stop the
 * association in order, firmware stop, MHI stop, SoC reset, save the PCI
 * config). Wi-Fi off and the data-stall restart use it too, so each of them
 * is a real reset. Runs holding the command gate. Returns false when it did
 * not own the chip (another transition still running, or the workloop thread):
 * the hardware is then untouched. A chip that is already down (Wi-Fi off,
 * then sleep) is left alone, whoever got there first. budgetStartMs is the
 * start of a time-limited caller (the power-state thread), 0 for none.
 */
bool AirportItlwm::chipDown(const char *why, const char *wakeName, uint32_t maxWaitMs, uint64_t budgetStartMs)
{
    bool pm = budgetStartMs != 0;		/* M0 (C13): only the power-state thread's "system sleep" passes a budget start */

    if (onWorkLoopThread()) {
        qwz_log("qwz: pm: %s: asked on the workloop thread, which cannot sleep for the teardown: chip left as it is\n", why);
        qwz_log_publish();
        return false;
    }
    if (!chipClaim(why, maxWaitMs, pm ? pmAckStartMs : 0, pm ? QWZ_DEADLINE_SLEEP_MS : QWZ_DEADLINE_OFF_MS)) {
        qwz_log("qwz: pm: %s: another chip transition is still running: the hardware is not touched\n", why);
        qwz_log_publish();
        return false;
    }
    if (chipIsDown && fHalService->wakeNeedsRecovery() && !fHalService->firmwareUp()) {
        qwz_log("qwz: pm: %s: the chip is already powered down\n", why);
        qwz_log_publish();
    } else {
        wakeLabel = wakeName;
        qwz_log("qwz: pm: ==== %s: interface down, chip power-down ====\n", why);
        qwz_log_publish();
        fHalService->awdlQuiesce(true);		/* M0 (C17): AWDL stops at the START of every real teardown (not for a chip that is already down) */
        if (budgetStartMs != 0 && pmNowMs() - budgetStartMs > QWZ_PM_QUIESCE_SKIP_MS) {
            qwz_log("qwz: pm: %s: %llu ms of the acknowledge budget are gone: not waiting for in-flight state work\n",
                    why, pmNowMs() - budgetStartMs);
            qwz_log_publish();
        } else
            fHalService->sleepQuiesce(QWZ_QUIESCE_MS);
        disableAdapter(bsdInterface);
        fHalService->sleepHardware();
        chipIsDown = fHalService->wakeNeedsRecovery();
        if (chipIsDown)
            chipUsed = false;
    }
    chipRelease();
    return true;
}

IOReturn AirportItlwm::radioOffGated(OSObject *owner, void *, void *, void *, void *)
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);

    if (self == NULL || self->fHalService == NULL)
        return kIOReturnError;
    self->cancelWakeRetry();
    self->cancelStallRestart();
    self->stallRestarts = 0;
    self->stallNoted = false;
    self->wakeRetryCount = 0;
    self->engineRearm("Wi-Fi off");	/* M0 (C15): a user action is a new episode */
    if (!self->chipDown("Wi-Fi off", "Wi-Fi on", QWZ_CHIP_WAIT_MS, 0)) {
        qwz_log("qwz: pm: Wi-Fi off: the chip reset did not run (see above); the interface is stopped only\n");
        qwz_log_publish();
        if (!self->onWorkLoopThread())
            self->disableAdapter(self->bsdInterface);
    }
    return kIOReturnSuccess;
}

/*
 * Wake after a chip power-down: PCIe link and config recovery, then the
 * interface enable, whose qwz_init does the full firmware init (the firmware
 * is gone: fw_initialized was cleared at sleep). Runs holding the command gate.
 * A failure queues a retry with back-off (wakeRetryNext).
 *
 * Several threads can ask for this at once after a wake (airportd's setPOWER,
 * the power-state thread): the first runs it, the others wait for it (qwz_init
 * sleeps with the gate released) and find nothing left to do, so the PCI
 * restore never runs beside a firmware boot and the init runs once.
 * Returns false if the chip could not be claimed (nothing was done).
 */
#define QWZ_WAKE_RETRY_MAX	12

static void wakeRetryNext(AirportItlwm *self)
{
    /* 5 s, 30 s, 60 s, then every 5 minutes; 12 tries in all (about 50 minutes) */
    static const uint32_t delaysSec[] = { 5, 30, 60 };
    uint32_t n = self->wakeRetryCount, sec;

    if (n >= QWZ_WAKE_RETRY_MAX) {
        if (n == QWZ_WAKE_RETRY_MAX) {
            self->wakeRetryCount++;
            qwz_log("qwz: pm: wake: GIVING UP after %u failed recoveries: the chip stays down; toggle Wi-Fi or sleep and wake\n", n);
            qwz_log_publish();
        }
        return;
    }
    sec = n < sizeof(delaysSec) / sizeof(delaysSec[0]) ? delaysSec[n] : 300;
    self->wakeRetryCount = n + 1;
    qwz_log("qwz: pm: wake: recovery failed; retry %u of %u in %u s\n", n + 1, QWZ_WAKE_RETRY_MAX, sec);
    qwz_log_publish();
    self->scheduleWakeRetry(sec * 1000, kWakeViaRetry);
}

bool AirportItlwm::wakeRecover(int via)
{
    uint64_t t0, t1;
    bool ok = false;
    bool isRetry = via == kWakeViaRetry;
    bool pm = via == kWakeViaPmThread;		/* M0 (C13): only the power-state thread's clock starts at the power change request */
    IOReturn r;

    if (!chipClaim(wakeViaName[via], waitMsFor(via), pm ? pmAckStartMs : 0, pm ? QWZ_DEADLINE_PMWAKE_MS : QWZ_DEADLINE_WAKE_MS))
        return false;
    if (!fHalService->wakeNeedsRecovery()) {
        qwz_log("qwz: pm: wake: already recovered; %s has nothing left to do\n", wakeViaName[via]);
        qwz_log_publish();
        chipRelease();
        return true;
    }
    chipIsDown = false;
    clock_get_uptime(&t0);
    qwz_log("qwz: pm: ==== %s%s: recovering the Wi-Fi chip ====\n", wakeLabel ? wakeLabel : "system wake",
            isRetry ? " (retry)" : "");
    qwz_log("qwz: pm: wake: recovery run from %s\n", wakeViaName[via]);
    qwz_log_publish();
    r = fHalService->wakeHardware();
    if (r == kIOReturnSuccess) {
        qwz_log("qwz: pm: wake: firmware re-init (enable interface)\n");
        qwz_log_publish();
        r = enableAdapterNow(bsdInterface);
        if (r == kIOReturnSuccess) {
            ok = true;
        } else {
            qwz_log("qwz: pm: wake: firmware re-init FAILED (0x%x): leaving the interface down\n", (unsigned)r);
            disableAdapter(bsdInterface);
        }
    } else
        qwz_log("qwz: pm: wake: PCI recovery FAILED (0x%x): leaving the chip down\n", (unsigned)r);
    fHalService->wakeComplete(ok);
    clock_get_uptime(&t1);
    absolutetime_to_nanoseconds(t1 - t0, &t1);
    qwz_log("qwz: pm: wake: %s, total wake recovery %llu ms\n", ok ? "SUCCESS" : "FAILED", t1 / 1000000ULL);
    qwz_log_publish();
    if (ok)
        wakeRetryCount = 0;
    chipRelease();
    if (!ok) {
        if (via == kWakeViaDead) {
            /* M0 (C15): the engine owns the retry (engineRun looks at the result, counts the failure and schedules the next attempt) */
        } else if (wakeAllowed())
            wakeRetryNext(this);
        else {
            qwz_log("qwz: pm: wake: no retry: the system is going to sleep, or Wi-Fi is off; the next wake or Wi-Fi on recovers\n");
            qwz_log_publish();
        }
    }
    if (ok && via != kWakeViaDead)	/* M2a WP0: the engine closes its own episodes (engineRun); every other chip-up closes the one it ended */
        wakeCloseEpisode(via, t1 / 1000000ULL);
    return true;
}

/*
 * M2a WP0: a chip-up other than the engine's attempt succeeded (Wi-Fi on, system wake, a legacy retry, a deferred request, a data-stall
 * restart): if a recovery episode is open, close it the way the engine does. The engine closes only the episodes its own attempt ends
 * (engineRun: noteRecovered, the flap rule, "recovered in N ms"); the re-arm that precedes a Wi-Fi on or a wake (engineRearm) clears the
 * engine's memory (engDeferred, engFailCont, engGivenUp, engLastOkMs) BEFORE the chip comes up, so only the life word still knows the
 * episode is open. Not the flap memory (engLastOkMs): a recovery by Wi-Fi on or wake is not an engine success for the flap rule. The
 * engine's failure memory (GIVEN_UP, the failure count, the cool-down, a deferral) is already clear after a Wi-Fi on or a wake (the
 * re-arm), and is cleared here for the other vias (fix1, dead D1: a legacy retry or a data-stall restart is not preceded by one), unless
 * an engine attempt is running. Does nothing while recover=0 or m0=0 (fix1, dead D2). Runs in wakeRecover, so: the command gate held, a
 * thread that may sleep (never the workloop thread, never interrupt level), the chip-transition claim already released; it takes no
 * lock, waits for nothing and touches no register. Also from enableAdapterGated's bare enable (fix2, dead-R2-1: a restart by qwz_init's
 * DEAD branch, with no teardown before it, is a chip-up that never reaches wakeRecover): the gate is held there too, and nothing in here
 * depends on the thread.
 */
void AirportItlwm::wakeCloseEpisode(int via, uint64_t chipMs)
{
    uint32_t deadReason = 0, episode = 0;
    uint64_t sinceMs = 0;
    const char *cleared = "";

    if (!qwz_dead_armed())
        return;				/* fix1 (dead D2): recover=0 or m0=0 switches the engine machinery off (engineRun and sentinelTick honour it), so no episode is closed, no "recovery proven" latched and nothing logged on a path the operator turned off mid-episode */
    if ((unsigned)via >= (unsigned)kWakeViaCount || via == kWakeViaDead || pmStopping || fHalService == NULL)
        return;
    if (!fHalService->recoveryOpen(&deadReason, &episode, &sinceMs))
        return;				/* no episode is open: an ordinary wake or Wi-Fi on, nothing to close */
    if (!fHalService->firmwareUp() || fHalService->wakeNeedsRecovery() || fHalService->faultPending(NULL))
        return;				/* the engine's own success test (engineRun): a DEAD that arrived during the bring-up keeps the episode open for the engine, which faultCb has asked */
    fHalService->noteRecovered(deadReason);	/* clears RECOVERY; latches "recovery proven" for a real crash (RDDM) exactly as the engine's success does (C24) */
    if (!engineBusy && (engGivenUp || engFailCont != 0 || engDeferred)) {	/* fix1 (dead D1): only Wi-Fi on and a wake are preceded by engineRearm; a legacy retry, a deferred request or a data-stall restart can end the episode with the engine still holding GIVEN_UP, a failure count, a cool-down or a deferral of an episode that is over (a later crash would be ignored or start at "attempt 2"). The chip is up and verified above: the engine's own success state, except the flap memory (engLastOkMs), which stays; not while an engine attempt is running (engineBusy: it owns that state and ends it itself) */
        cleared = engGivenUp ? "; engine state GIVEN_UP cleared" : engFailCont != 0 ? "; engine failure count cleared" : "; engine deferral cleared";
        engGivenUp = false;
        engFailCont = 0;
        engCooldownMs = 0;
        engDeferred = false;
        fHalService->setEngineState("UP", 0, false);
    }
    if (qwz_log_ratelimit(&engClosed, 24, 8)) {
        qwz_log("qwz: engine: recovered by %s in %llu ms (DEAD #%u %s declared %llu ms earlier)%s\n", wakeViaDoneName[via],
                (unsigned long long)chipMs, episode, qwz_dead_names[deadReason < QWZ_DEAD_MAX ? deadReason : 0], (unsigned long long)sinceMs,
                cleared);
        qwz_log_publish();
    }
}

void AirportItlwm::scheduleWakeRetry(uint32_t ms, int via)
{
    uint64_t deadline;

    if (wakeRetryCall == NULL || pmStopping)
        return;
    wakeRetryVia = via;
    clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
    retain();
    if (thread_call_enter_delayed(wakeRetryCall, deadline))
        release();	/* was already queued: that one holds a reference */
}

IOReturn AirportItlwm::wakeRetryGated(OSObject *owner, void *, void *, void *, void *)
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);

    if (self == NULL || self->fHalService == NULL || self->pmStopping)
        return kIOReturnError;
    if (!self->wakeAllowed() || !self->fHalService->wakeNeedsRecovery()) {
        qwz_log("qwz: pm: wake retry: not needed any more\n");
        qwz_log_publish();
        return kIOReturnSuccess;
    }
    self->wakeRecover(self->wakeRetryVia);
    return kIOReturnSuccess;
}

static void handleWakeRetry(thread_call_param_t param0, thread_call_param_t)
{
    AirportItlwm *self = (AirportItlwm *) param0;

    if (self->pmReady())
        self->getCommandGate()->runAction(AirportItlwm::wakeRetryGated);
    self->release();
}

void AirportItlwm::cancelWakeRetry()
{
    if (wakeRetryCall != NULL && thread_call_cancel(wakeRetryCall))
        release();	/* the cancelled call's reference */
}

/*
 * Data-stall restart. ItlQwz::dataStalled() reports an interface that sat in
 * RUN without one data frame while management frames kept coming: the
 * firmware is up but its data path is dead (seen after a sleep that left a
 * half-deleted peer). The cure is what a wake does, a full firmware restart.
 * The watchdog (once a second, no gate) only polls; a thread call does the
 * work holding the gate. At most twice per wake/boot window, 60 s apart; with
 * -qwzstallsbr the second one also resets the PCIe link. A window ends at
 * sleep, Wi-Fi off, or after five minutes of a healthy link.
 */
#define QWZ_STALL_MAX_RESTARTS	2
#define QWZ_STALL_BACKOFF_MS	60000

void AirportItlwm::checkDataStall()
{
    uint64_t now;
    bool healthy = false, stalled;

    if (noWakeFix || noStallFix || stallCall == NULL || pmStopping || sleepPending ||
        pmPowerState != kPowerStateOn || !power_state || chipBusy)
        return;
    stalled = fHalService->dataStalled(&healthy);
    if (healthy && (stallRestarts != 0 || stallNoted)) {
        qwz_log("qwz: pm: data-stall: the link has been healthy for a while: the restart count (%u) is reset\n", stallRestarts);
        qwz_log_publish();
        stallRestarts = 0;
        stallNoted = false;
    }
    if (!stalled || stallPending)
        return;
    now = pmNowMs();
    if (stallRestarts >= QWZ_STALL_MAX_RESTARTS) {
        if (!stallNoted) {
            stallNoted = true;
            qwz_log("qwz: pm: data-stall: still stalled after %u restarts in this wake/boot window: no more restarts (toggle Wi-Fi, or sleep and wake)\n",
                    stallRestarts);
            qwz_log_publish();
        }
        return;
    }
    if (stallRestarts > 0 && now - stallLastMs < QWZ_STALL_BACKOFF_MS) {
        if (!stallNoted) {
            stallNoted = true;
            qwz_log("qwz: pm: data-stall: back-off: the next restart is allowed in %llu s\n",
                    (QWZ_STALL_BACKOFF_MS - (now - stallLastMs)) / 1000ULL);
            qwz_log_publish();
        }
        return;
    }
    stallPending = true;
    retain();
    if (thread_call_enter(stallCall))
        release();	/* was already queued: that one holds a reference */
}

void AirportItlwm::stallRestart()
{
    if (noStallFix || pmStopping || sleepPending || pmPowerState != kPowerStateOn || !power_state || chipBusy ||
        !fHalService->dataStalled(NULL)) {
        qwz_log("qwz: pm: data-stall: gone, or the power state changed, before the restart ran: leaving it\n");
        qwz_log_publish();
        return;
    }
    stallRestarts++;
    stallLastMs = pmNowMs();
    stallNoted = false;
    qwz_log("qwz: pm: data-stall: restarting the firmware (restart %u of %u in this wake/boot window)\n",
            stallRestarts, QWZ_STALL_MAX_RESTARTS);
    if (stallSbr && stallRestarts > 1) {
        qwz_log("qwz: pm: data-stall: this restart also resets the PCIe link (-qwzstallsbr)\n");
        fHalService->requestBusReset();
    }
    qwz_log_publish();
    if (!chipDown("data-stall restart", "data-stall restart", QWZ_CHIP_WAIT_MS, 0))
        return;
    /* chipDown sleeps with the gate released: a sleep or Wi-Fi off may have arrived meanwhile. */
    if (!wakeAllowed()) {
        qwz_log("qwz: pm: data-stall: a sleep or Wi-Fi off arrived during the power-down: the chip stays down for the wake / Wi-Fi on\n");
        qwz_log_publish();
        return;
    }
    wakeRecover(kWakeViaStall);
}

IOReturn AirportItlwm::stallRestartGated(OSObject *owner, void *, void *, void *, void *)
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);

    if (self == NULL || self->fHalService == NULL || self->pmStopping) {
        if (self != NULL)
            self->stallPending = false;
        return kIOReturnError;
    }
    self->stallRestart();
    self->stallPending = false;
    return kIOReturnSuccess;
}

static void handleStallRestart(thread_call_param_t param0, thread_call_param_t)
{
    AirportItlwm *self = (AirportItlwm *) param0;

    if (self->pmReady())
        self->getCommandGate()->runAction(AirportItlwm::stallRestartGated);
    else
        self->stallPending = false;
    self->release();
}

void AirportItlwm::cancelStallRestart()
{
    if (stallCall != NULL && thread_call_cancel(stallCall)) {
        stallPending = false;
        release();	/* the cancelled call's reference */
    }
}

/*
 * ---- M0 (docs/m0-spec.md): the sentinel (C14), the dead-firmware engine (C15), the Wi-Fi-off hop (C16) ----
 */

/*
 * C16: Wi-Fi off delivered on the workloop thread cannot run disableAdapter inline (it reaches
 * rw_enter_write and qwz_stop, which sleep). A thread call runs it on a thread that may.
 */
static void handleRadioOff(thread_call_param_t param0, thread_call_param_t)
{
    AirportItlwm *self = (AirportItlwm *)param0;

    if (self->pmReady())
        self->getCommandGate()->runAction(AirportItlwm::radioOffDisableGated);
    self->release();
}

IOReturn AirportItlwm::radioOffDisableGated(OSObject *owner, void *, void *, void *, void *)
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);

    if (self == NULL || self->fHalService == NULL || self->pmStopping)
        return kIOReturnError;
    if (!self->power_state)		/* Wi-Fi was switched on again meanwhile: leave it */
        self->disableAdapter(self->bsdInterface);
    return kIOReturnSuccess;
}

void AirportItlwm::scheduleRadioOff()
{
    if (radioOffCall == NULL || pmStopping)
        return;
    retain();
    if (thread_call_enter(radioOffCall))
        release();	/* already queued: that one holds a reference */
}

/*
 * C14: the sentinel, a gate-free timer on the watchdog workloop's thread (never the main gate's).
 * Once a second: the chip-transition deadline (ARMED), the waiter flush re-issue, the gate probe
 * (SHADOW), the link probe (ARMED), the engine poll and the evidence cadence. The existing
 * watchdogTimer cannot do this: it runs only while the adapter is enabled and its body takes the main
 * gate, the very thing the sentinel has to watch. A timer event source has a defined lifecycle
 * (stopSentinel); a self-re-arming thread call can queue itself again after thread_call_cancel_wait
 * returned and then be freed while queued.
 */
void AirportItlwm::sentinelFired(OSObject *owner, IOTimerEventSource *sender)
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);

    if (self == NULL || self->sentinelStopping)		/* M0 (R1-03): not pmStopping: the ticks (deadlines, waiter flush) must run while quiescePM waits */
        return;
    self->sentinelTick();
    if (!self->sentinelStopping)
        sender->setTimeoutMS(1000);
}

void AirportItlwm::sentinelTick()
{
    uint64_t now = pmNowMs();
    uint32_t reason = 0;
    bool busy = __atomic_load_n(&chipBusy, __ATOMIC_ACQUIRE), faulted, active, printHealth, ok;

    if (fHalService == NULL || !qwz_m0_on)
        return;
    faulted = fHalService->faultPending(&reason);
    if (busy && !deadlineFired) {
        /*
         * The claim's fields are read once, after chipBusy (acquire), and the claim is re-validated before
         * acting: a release followed by a new claim between the two loads must not pair an old start with a
         * new limit, and a claim made after `now` was read has a start time that is not in the past.
         */
        uint64_t since = chipBusySince;
        uint32_t limit = chipBusyLimitMs;
        const char *who = chipBusyWho;
        bool pmClock = chipBusyPm;

        if (now > since && now - since > limit && __atomic_load_n(&chipBusy, __ATOMIC_ACQUIRE) && since == chipBusySince) {
            deadlineFired = true;
            qwz_log("qwz: sentinel: chip transition '%s' has run %llu ms (limit %u, clock from the %s): declaring DEAD\n",
                    who != NULL ? who : "?", (unsigned long long)(now - since), limit, pmClock ? "power change request" : "claim");
            fHalService->declareDead(QWZ_DEAD_DEADLINE, who);
        }
    }
    if (busy && faulted)
        fHalService->releaseWaiters();		/* closes the lost-wakeup window of a flush issued before a waiter reached commandSleep */
    /*
     * "Active": a chip transition, DEAD, an engine episode, AWDL up, an episode in the last 60 s (lastEpisodeMs 0
     * means none yet: the spec's sketch compared it unconditionally, which made the first 60 s of uptime "active").
     * Evidence cadence: a health line every 5 s while active, every 60 s idle; the first one on the first
     * successful probe (sentinelHealthMs 0 = nothing printed yet).
     */
    active = busy || faulted || engineBusy || awdlLinkUp || (lastEpisodeMs != 0 && now - lastEpisodeMs < 60000);
    printHealth = sentinelHealthMs == 0 || now - sentinelHealthMs >= (active ? 5000u : 60000u);
    ok = fHalService->healthSnapshot(printHealth);
    if (ok) {
        if (printHealth)
            sentinelHealthMs = now;
        gateBusyTicks = 0;
    } else if (!sleepPending && pmPowerState == kPowerStateOn) {
        gateBusyTicks++;
        if (gateBusyTicks == 10 || (gateBusyTicks % 30) == 0)
            qwz_log("qwz: sentinel: the main gate has been busy for %u s (last crumb '%s' %u ms ago, chipBusy %d '%s', dead %d)\n",
                    gateBusyTicks, qwz_crumb_text(), qwz_crumb_age_ms(), busy, busy ? chipBusyWho : "-", faulted);
        if (gateBusyTicks == 15)
            qwz_log("qwz: sentinel: gate stall for 15 s: would declare DEAD(gate stall) (shadow)\n");
    }
    if (fHalService->firmwareUp() && !busy && !sleepPending && pmPowerState == kPowerStateOn) {
        if (!fHalService->linkAlive()) {
            if (++linkBadTicks >= 2)
                fHalService->declareDead(QWZ_DEAD_LINK, "config id reads ffffffff");
        } else
            linkBadTicks = 0;
    }
    if (faulted && !engineBusy && !engGivenUp && !engDeferred && now >= engCooldownMs)
        engineRequest(kEngDead, reason);		/* covers a fault callback missed before registration, or declared by the sentinel itself */
    if (sentinelPubMs == 0 || now - sentinelPubMs >= (active ? 2000u : 30000u)) {
        sentinelPubMs = now;
        qwz_log_publish();
    }
}

/*
 * One idempotent helper ends the sentinel: quiescePM (its LAST statement, after the waits for the transitions in
 * flight: R1-03) and releaseAll (before fHalService is released). removeEventSource returns after a sentinelTick in flight has finished
 * (IOWorkLoop.h: "does not return until the work loop has acknowledged the removal"; UNVERIFIED, spec
 * section 12 item 8) and releases the workloop's reference; the HAL outlives the last tick because stop()
 * calls quiescePM before fHalService->detach and releaseAll.
 */
void AirportItlwm::stopSentinel()
{
    sentinelStopping = true;		/* first: a tick in flight must not re-arm the timer */
    if (sentinelTimer == NULL)
        return;
    sentinelTimer->cancelTimeout();
    sentinelTimer->disable();
    if (fWatchdogWorkLoop != NULL)
        fWatchdogWorkLoop->removeEventSource(sentinelTimer);	/* returns after a sentinelTick in flight has finished; releases the workloop's reference */
    sentinelTimer->release();					/* ours */
    sentinelTimer = NULL;
}

/*
 * C15: the dead-firmware engine, v0. ONE thread call, deadCall, owns recovery from the new triggers: a DEAD
 * episode (the sc_fault_cb callback, the sentinel's poll, debug.qwz.recover). It REUSES the proven bodies
 * chipDown and wakeRecover under the existing chipBusy exclusion (the pattern stallRestart uses) and adds
 * coalescing, a "wanted" re-check, a limiter (Linux ATH12K_RESET_MAX_FAIL_COUNT_FIRST 3 / FINAL 5,
 * ATH12K_RESET_FAIL_TIMEOUT_HZ 20 s), a visible GIVEN_UP state and a re-arm. It is single flight by the
 * engineBusy test at the top of engineRun, not by the thread call (spec section 12, item 13), and never a
 * kernel_thread_start. The legacy sleep, wake, Wi-Fi off/on and data-stall flows keep their bodies and
 * their 12-try wake retry; only DEAD-triggered episodes use the limiter.
 */
#define QWZ_FAIL_FIRST		3	/* Linux ATH12K_RESET_MAX_FAIL_COUNT_FIRST */
#define QWZ_FAIL_FINAL		5	/* ATH12K_RESET_MAX_FAIL_COUNT_FINAL */
#define QWZ_COOLDOWN_MS		20000	/* ATH12K_RESET_FAIL_TIMEOUT_HZ */
#define QWZ_FLAP_MS		60000

void AirportItlwm::faultCb(void *arg, uint32_t reason)	/* sc_fault_cb: interrupt action, sentinel or inject thread: never takes the gate, never blocks */
{
    AirportItlwm *self = (AirportItlwm *)arg;

    if (self == NULL)
        return;
    self->lastEpisodeMs = pmNowMs();
    self->engineRequest(kEngDead, reason);
}

void AirportItlwm::engineRequest(uint32_t bits, uint32_t reason)
{
    __atomic_fetch_or(&engineReq, bits, __ATOMIC_ACQ_REL);
    __atomic_store_n(&engineReason, reason, __ATOMIC_RELAXED);
    if (deadCall != NULL && !pmStopping) {
        retain();
        if (thread_call_enter(deadCall))
            release();	/* already queued: that one holds a reference */
    }
}

void AirportItlwm::scheduleEngine(uint32_t ms)
{
    uint64_t deadline;

    if (deadCall == NULL || pmStopping)
        return;
    clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
    retain();
    if (thread_call_enter_delayed(deadCall, deadline))
        release();
}

static void handleEngine(thread_call_param_t param0, thread_call_param_t)	/* like handleStallRestart */
{
    AirportItlwm *self = (AirportItlwm *)param0;

    if (self->pmReady())
        self->getCommandGate()->runAction(AirportItlwm::engineGated);
    self->release();
}

IOReturn AirportItlwm::engineGated(OSObject *owner, void *, void *, void *, void *)	/* in the gate, on a thread that may sleep */
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);

    if (self == NULL || self->fHalService == NULL || self->pmStopping)
        return kIOReturnError;
    self->engineRun();
    return kIOReturnSuccess;
}

void AirportItlwm::engineRun()
{
    uint32_t reason = 0, req;
    uint64_t now = pmNowMs(), done;
    uint32_t attempt = engFailCont + 1;
    bool exempt, ok, pending, retrying, asked;

    /* Single flight (round-2 audit, conflict 1). deadCall comes from thread_call_allocate() without THREAD_CALL_OPTIONS_ONCE, so
     * thread_call_enter() on a call that is already RUNNING starts a second concurrent invocation, and faultCb requests the engine on
     * every new DEAD episode, also those raised during the restart's own bring-up (a sentinel DEADLINE, an RDDM, a DEAD(LINK) found by
     * the power-down). The first run releases the gate in every commandSleep, so the second one enters the gate at once. This test
     * runs inside runAction, hence race-free against the other gated runs; nothing is lost, because the end of the first run and the
     * sentinel poll (faultPending() with !engineBusy) look at the state again. engineReq is not consumed here. */
    if (engineBusy) {
        if (qwz_log_ratelimit(&engAbsorbed, 4, 64))
            qwz_log("qwz: engine: a restart is already running: this request is absorbed\n");
        return;
    }
    req = __atomic_exchange_n(&engineReq, 0, __ATOMIC_ACQ_REL);	/* kEngDead and/or kEngRecover */
    if (!qwz_dead_armed())
        return;				/* recover=0 or m0=0: legacy handling, nothing declares DEAD, nothing to restart */
    pending = fHalService->faultPending(&reason);
    asked = !pending && (req & kEngRecover) != 0 && fHalService->wakeNeedsRecovery();	/* debug.qwz.recover=2 on a chip that is down after GIVEN_UP */
    retrying = !pending && !asked && engFailCont != 0 && fHalService->wakeNeedsRecovery();	/* the retry rule: a failed attempt left DEAD clear (qwz_init clears it before the bring-up) */
    if (!pending && !asked && !retrying)
        return;				/* cleared meanwhile (the legacy path or a toggle restarted the firmware) */
    if (pending)
        engReason = reason;		/* the reason of the first attempt is kept for the retries */
    else
        reason = asked ? (engReason = QWZ_DEAD_USER) : engReason;	/* a user request starts a USER episode; a retry keeps the first reason */
    if (engGivenUp) {
        qwz_log("qwz: engine: GIVEN_UP: request ignored (toggle Wi-Fi, sleep and wake, or sysctl debug.qwz.recover=2 re-arms)\n");
        return;
    }
    if (!wakeAllowed()) {
        engDeferred = true;
        qwz_log("qwz: engine: recovery deferred: Wi-Fi is off or the system is asleep (the next Wi-Fi on or wake restarts the firmware)\n");
        return;
    }
    engDeferred = false;
    if (now < engCooldownMs) {
        qwz_log("qwz: engine: cool-down, next attempt in %llu ms\n", engCooldownMs - now);
        scheduleEngine((uint32_t)(engCooldownMs - now));
        return;
    }
    /* Another transition (power-state thread, Wi-Fi toggle, stall restart, legacy retry) owns the chip. Look again later, as stallRestart
     * does, instead of waiting in chipClaim for up to 10 s: a claim that waited out a SUCCESSFUL recovery would find the firmware that
     * recovery just brought up and tear it down (chipDown's only skip is chipIsDown && wakeNeedsRecovery() && !firmwareUp(), and
     * wakeRecover clears chipIsDown). This test and the claim in chipDown run in the same gate hold with no sleep between them, so
     * nothing can claim the chip in between. */
    if (chipBusy) {
        __atomic_fetch_or(&engineReq, req, __ATOMIC_ACQ_REL);		/* keep kEngRecover for the re-run */
        if (qwz_log_ratelimit(&engChipBusy, 2, 16))
            qwz_log("qwz: engine: another chip transition ('%s') is running: looking again in 1 s\n", chipBusyWho != NULL ? chipBusyWho : "?");
        scheduleEngine(1000);
        return;
    }
    exempt = reason == QWZ_DEAD_INJECT || reason == QWZ_DEAD_USER;
    engineBusy = true;
    engBeginMs = now;
    engEpisodes++;
    qwz_log("qwz: engine: ==== firmware restart: attempt %u of %u, reason %s ====\n", attempt, QWZ_FAIL_FINAL,
            qwz_dead_names[reason < QWZ_DEAD_MAX ? reason : 0]);	/* not deadName(): for a user request the softc still holds the previous episode's reason */
    fHalService->setEngineState("RESTARTING", engFailCont, false);
    qwz_log_publish();
    QWZ_CRUMB("eng:teardown");
    if (!chipDown("firmware dead", "firmware restart", QWZ_CHIP_WAIT_MS, 0)) {
        engineBusy = false;
        engineNoteFailure("chip busy: another transition owns it");
        return;
    }
    if (!wakeAllowed()) {		/* chipDown sleeps with the gate released: a sleep or Wi-Fi off may have arrived */
        engineBusy = false;
        engDeferred = true;
        qwz_log("qwz: engine: a sleep or Wi-Fi off arrived during the power-down: the chip stays down for the wake or Wi-Fi on\n");
        return;
    }
    QWZ_CRUMB("eng:bringup");
    wakeRecover(kWakeViaDead);		/* on a retry the chip is already down, but wakeRecover cleared chipIsDown, so chipDown ran its teardown branch and ItlQwz::sleepHardware logged "the chip is already powered down; nothing to do" */
    ok = !fHalService->wakeNeedsRecovery() && fHalService->firmwareUp() && !fHalService->faultPending(NULL);
    engineBusy = false;
    QWZ_CRUMB("eng:done");
    if (!ok) {
        engineNoteFailure("restart failed");
        return;
    }
    done = pmNowMs();
    fHalService->noteRecovered(reason);		/* a success is a success, flapping or not: clears RECOVERY, latches "recovery proven" for a real crash (C24) */
    if (!exempt && engLastOkMs != 0 && done - engLastOkMs < QWZ_FLAP_MS) {
        /* flap rule: the chip is up, so there is nothing to retry and no timer to arm; only the count and the cool-down for the next crash */
        qwz_log("qwz: engine: recovered, but only %llu ms after the previous recovery: counted as a failure (flap)\n", done - engLastOkMs);
        engLastOkMs = done;
        if (++engFailCont >= QWZ_FAIL_FINAL) {
            engineGiveUp("5 consecutive failed recoveries");
            return;
        }
        if (engFailCont >= QWZ_FAIL_FIRST)
            engCooldownMs = done + QWZ_COOLDOWN_MS;
        fHalService->setEngineState("UP", engFailCont, false);
        qwz_log("qwz: engine: flap %u of %u%s\n", engFailCont, QWZ_FAIL_FINAL,
                engFailCont >= QWZ_FAIL_FIRST ? ": the next restart waits for a 20 s cool-down" : "");
        qwz_log_publish();
        return;
    }
    engFailCont = 0;
    engLastOkMs = done;
    fHalService->setEngineState("UP", 0, false);
    qwz_log("qwz: engine: recovered in %llu ms (attempt %u)\n", done - engBeginMs, attempt);
    qwz_log_publish();
}

void AirportItlwm::engineNoteFailure(const char *why)
{
    uint32_t n = ++engFailCont, ms;

    if (n >= QWZ_FAIL_FINAL) {
        engineGiveUp("5 consecutive failed recoveries");
        return;
    }
    ms = n >= QWZ_FAIL_FIRST ? QWZ_COOLDOWN_MS : 2000 * n;			/* 2 s, 4 s, then 20 s, 20 s */
    engCooldownMs = pmNowMs() + ms;
    qwz_log("qwz: engine: recovery failed (%s): %u of %u, next attempt in %u ms\n", why, n, QWZ_FAIL_FINAL, ms);
    fHalService->setEngineState("RETRY", n, false);
    qwz_log_publish();
    scheduleEngine(ms);
}

void AirportItlwm::engineGiveUp(const char *why)
{
    bool down = fHalService->wakeNeedsRecovery() || fHalService->faultPending(NULL);	/* a flap give-up leaves the chip up */

    engGivenUp = true;
    fHalService->setEngineState("GIVEN_UP", engFailCont, true);
    qwz_log("qwz: engine: ==== GIVEN UP: %s; %s. Re-arm: toggle Wi-Fi, sleep and wake, or sysctl debug.qwz.recover=2 ====\n", why,
            down ? "the chip stays down" : "the chip is up, but a further crash will not be restarted by itself");
    qwz_log_publish();
}

void AirportItlwm::engineRearm(const char *who)
{
    if (engGivenUp || engFailCont != 0)
        qwz_log("qwz: engine: re-armed by %s (was %s, %u failure(s))\n", who, engGivenUp ? "GIVEN_UP" : "retrying", engFailCont);
    engGivenUp = false;
    engFailCont = 0;
    engCooldownMs = 0;
    engLastOkMs = 0;			/* a user or system action also ends the crash-loop memory of the flap rule */
    engDeferred = false;
    if (fHalService != NULL)
        fHalService->setEngineState("UP", 0, false);
}

/*
 * AWDL virtual interface creation — called by IO80211Controller::attachVirtualInterface.
 * Based on ReAWDL project (maddog860/Sonoma-ReAWDL-Project).
 */
/*
 * AWDL virtual interface creation — called by IO80211Controller::attachVirtualInterface.
 * Based on ReAWDL project (maddog860/Sonoma-ReAWDL-Project).
 *
 * We let the base IO80211Controller::createVirtualInterface handle it,
 * which allocates IO80211VirtualInterface and calls init().
 * The base implementation logs "failure to create virtual interface" and
 * returns NULL — but only because it doesn't know the prefix.
 * On Tahoe, attachVirtualInterface handles the full lifecycle.
 */
/*
 * awdl0, as Apple's driver makes its AWDL interface (AppleBCMWLANCore's
 * createProxInterface: the object with prefix "awdl", role 6, interface id
 * 2, registration info, start), with the infra interface's BSD side: a second
 * AirportItlwmEthernetInterface, named awdl. IO80211VirtualInterface::start
 * creates the AWDL engine (IO80211AWDLPeerManager) for role 6.
 *
 * Discovery build: runs once, from debug.qwz.awdl=1 (the sysctl caller's
 * thread, as start() runs the infra steps from its own), logs every step and
 * stops at the first failure. A half-built interface is left in place: tearing
 * down an IO80211 interface the family has half adopted is riskier than
 * leaving it until the reboot, and the sysctl does not survive one.
 *
 * The AWDL interface's class is in the AWDL kext, which is loaded by hand
 * (AirportWCN7850AWDLBridge.h): until it is, the class does not exist and
 * nothing is tried.
 */
#define AWDL_TX_SLOTS	8
#define AWDL_TX_MAX	1600
static const uint8_t awdlBssidBytes[6] = { 0x00, 0x25, 0x00, 0xff, 0x94, 0x73 };
#define AWDL_TLV_SYNC_PARAMS	4
#define AWDL_TU_NS		1024000ULL
#define AWDL_AUTO_SLOTS	3	/* repeat 1: the OOB auto request (a MIF); 2: the PSF while we are master; 3 (M2a WP5, awdlclk 16): the PSF as a follower */
#define AWDL_AUTO_FOLLOW	2	/* the index of repeat 3 */

/* How often each repeated frame goes out: a MIF every 125 ms, a PSF every 110 TU (the sync interval). */
static const uint64_t awdlAutoNs[AWDL_AUTO_SLOTS] = { 125ULL * 1000 * 1000, 110ULL * 1024 * 1000, 440ULL * 1024 * 1000 };

struct AwdlTxQueue {
    uint32_t head, tail;	/* tail - head frames queued */
    uint16_t len[AWDL_TX_SLOTS];
    uint8_t frame[AWDL_TX_SLOTS][AWDL_TX_MAX];
    uint16_t autoLen[AWDL_AUTO_SLOTS];	/* frames resent periodically; 0: none */
    uint64_t autoLastNs[AWDL_AUTO_SLOTS];
    uint8_t autoFrame[AWDL_AUTO_SLOTS][AWDL_TX_MAX];
};

int AirportItlwm::awdlTxEnqueue(void *ctx, const uint8_t *frame, size_t len, int repeat)
{
    AirportItlwm *self = (AirportItlwm *)ctx;
    struct AwdlTxQueue *q = self->awdlTxq;
    bool full = false;

    if (q == NULL || self->awdlTxStopped)
        return ENXIO;
    if (repeat == QWZ_AWDL_TX_SETTER)		/* M2a WP5: the engine's sync setters, not a frame */
        return self->awdlSetterIn(frame, len);
    if (repeat == QWZ_AWDL_TX_FOLLOWER_PSF && (qwz_awdlclk & 16) == 0)
        return 0;				/* M2a WP5: the follower's PSF is only kept with awdlclk 16 */
    if (repeat < 0 || repeat > AWDL_AUTO_SLOTS)
        return EINVAL;
    if (repeat && len == 0) {	/* no frame: stop resending that slot */
        IOLockLock(self->awdlTxLock);
        q->autoLen[repeat - 1] = 0;
        IOLockUnlock(self->awdlTxLock);
        return 0;
    }
    if (len < 24 || len > AWDL_TX_MAX) {
        qwz_awdlx_bridge(EINVAL);	/* M2a (WP3, I10) */
        return EINVAL;
    }
    IOLockLock(self->awdlTxLock);
    if (repeat) {
        memcpy(q->autoFrame[repeat - 1], frame, len);
        q->autoLen[repeat - 1] = (uint16_t)len;
        q->autoLastNs[repeat - 1] = 0;	/* due now */
    } else if (q->tail - q->head < AWDL_TX_SLOTS) {
        memcpy(q->frame[q->tail % AWDL_TX_SLOTS], frame, len);
        q->len[q->tail % AWDL_TX_SLOTS] = (uint16_t)len;
        q->tail++;
    } else {
        full = true;
        self->awdlTxDropped++;
    }
    IOLockUnlock(self->awdlTxLock);
    self->awdlTxTimer->setTimeoutUS(1);
    qwz_awdlx_bridge(full ? ENOSPC : 0);	/* M2a (WP3, I10): the bridge queue's verdict */
    return full ? ENOSPC : 0;
}

/* The value of the first TLV of a type in an AWDL TLV list (type u8, length u16 LE, value); NULL if none. */
static const uint8_t *awdlTlvFind(const uint8_t *tlvs, size_t len, uint8_t type, size_t *vlen)
{
    size_t off = 0, l;

    while (off + 3 <= len) {
        l = tlvs[off + 1] | (tlvs[off + 2] << 8);
        if (off + 3 + l > len)
            return NULL;
        if (tlvs[off] == type) {
            *vlen = l;
            return tlvs + off + 3;
        }
        off += 3 + l;
    }
    return NULL;
}

/* The TLV types of an AWDL TLV list, as text, for the log. */
static void awdlTlvTypes(const uint8_t *tlvs, size_t len, char *out, size_t outLen)
{
    size_t off = 0, l, n = 0;

    out[0] = '\0';
    while (off + 3 <= len && n + 6 < outLen) {
        l = tlvs[off + 1] | (tlvs[off + 2] << 8);
        if (off + 3 + l > len)
            break;
        n += snprintf(out + n, outLen - n, "%02x ", tlvs[off]);
        off += 3 + l;
    }
}

/*
 * The AWDL engine's frames carry no synchronization parameters TLV (type 4)
 * and no channel sequence TLV (type 0x12): Broadcom's firmware adds them,
 * as the keeper of the AW clock. A peer cannot place a frame without them
 * and drops the sender. The host adds them here from the last ones a peer
 * sent (captured in awdlRxGated), with the sync parameters patched to our
 * clock: next AW channel and master channel 149, tx_counter = TU to our
 * next AW, master address, aw_seq_number = the current slot.
 */
#define AWDL_TLV_CHANSEQ	0x12

/*
 * M2a WP5 (docs/research-airdrop-2026-11-06.md 3.5 Y1 to Y3): the channel sequence and the synchronization
 * parameters TLVs of the frames we send, built instead of copied. Layouts [PUB: Owly docs/SPEC.md 3.2,
 * "Synchronization Parameters (4)" and "Channel Sequence (18)", https://github.com/t4t5/owly; OWL frame.h],
 * offsets into the TLV value, after the 3-byte TLV header:
 *   channel sequence: 0 slot count less one (15), 1 encoding (0 simple: 1 byte per slot; 1 legacy: flags, then
 *   the centre channel; 3 opclass: channel, then operating class), 2 duplicate count (0), 3 step count (3), 4 fill
 *   channel (2 bytes, 0xffff), 6 the 16 slots (1 or 2 bytes each). TLV 0x12's value is that and 3 zero bytes.
 *   TLV 4: 0 next AW channel, 1 TX down-counter (u16 LE, TU), 3 master channel, 4 guard time, 5 AW period (u16),
 *   7 action frame period (u16), 9 flags (u16, 0x1800), 11 extended AW length (u16), 13 common AW length (u16),
 *   15 remaining AW length (u16), 17 minimum extension and the maximum for multicast, unicast and action frames
 *   (4 bytes), 21 master address, 27 presence mode, 28 reserved, 29 next AW sequence number (u16), 31 AP
 *   alignment (u16), 33 the channel sequence, then 2 zero bytes.
 * With 2-byte slots: TLV 4 is 3 + 33 + 38 + 2 = 76 bytes and TLV 0x12 is 3 + 38 + 3 = 44 [the sizes of the 299-byte
 * frames of M0: 299 = 40 + 139 + 76 + 44, INF].
 */
#define AWDL_SELF_CHAN		149	/* the listen vdev sits on the infra channel; D18 pins the router to 149 */
#define AWDL_SEQ_SLOTS		16
#define AWDL_SEQ_MAX		(6 + 2 * AWDL_SEQ_SLOTS)	/* 38: header and 16 slots of 2 bytes */
#define AWDL_T4_FIXED		33
#define AWDL_T4_MAX		(3 + AWDL_T4_FIXED + AWDL_SEQ_MAX + 2)	/* 76 */
#define AWDL_T12_MAX		(3 + AWDL_SEQ_MAX + 3)			/* 44 */

struct AwdlSeq {
    uint8_t enc, dup, step, fill[2], w;	/* w: bytes per slot (1 or 2) */
    uint8_t slot[AWDL_SEQ_SLOTS][2];
};

/* The channel sequence structure v[0..vlen) into s; false unless it is the 16-slot form Apple's devices send. */
static bool awdlSeqParse(const uint8_t *v, size_t vlen, struct AwdlSeq *s)
{
    size_t i;

    if (vlen < 6 || v[0] != AWDL_SEQ_SLOTS - 1 || (v[1] != 0 && v[1] != 1 && v[1] != 3))
        return false;
    s->enc = v[1];
    s->dup = v[2];
    s->step = v[3];
    s->fill[0] = v[4];
    s->fill[1] = v[5];
    s->w = s->enc == 0 ? 1 : 2;
    if (s->step != 3 || vlen < 6 + (size_t)AWDL_SEQ_SLOTS * s->w)
        return false;
    for (i = 0; i < AWDL_SEQ_SLOTS; i++) {
        s->slot[i][0] = v[6 + i * s->w];
        s->slot[i][1] = s->w == 2 ? v[6 + i * 2 + 1] : 0;
    }
    return true;
}

/* s as the on-air structure into out (6 + 16 * w bytes at most AWDL_SEQ_MAX); its length. */
static size_t awdlSeqPut(const struct AwdlSeq *s, uint8_t *out)
{
    size_t i, n = 0;

    out[n++] = AWDL_SEQ_SLOTS - 1;
    out[n++] = s->enc;
    out[n++] = s->dup;
    out[n++] = s->step;
    out[n++] = s->fill[0];
    out[n++] = s->fill[1];
    for (i = 0; i < AWDL_SEQ_SLOTS; i++) {
        out[n++] = s->slot[i][0];
        if (s->w == 2)
            out[n++] = s->slot[i][1];
    }
    return n;
}

/*
 * Does the slot entry e (s->w bytes) put the node on the 149 group? simple and opclass entries carry the channel
 * number first [PUB: Owly AWDL-10]. A legacy entry is flags, then the centre channel; 0x10 is 5 GHz, and centre
 * 149 (20 MHz), 151 (40) or 155 (80) contain 149 [INF: the decode of one iPhone's sequence in the MT7925 project,
 * ROADMAP-AWDL.md 10.30: 1d 97 is 40 MHz centre 151, 2b 06 is channel 6; AWDL's only social channel above 100 is
 * 149, so a 5 GHz entry in that block is 149 with a wider width].
 */
static bool awdlSlotCovers(const struct AwdlSeq *s, const uint8_t *e)
{
    switch (s->enc) {
    case 0:
    case 3:
        return e[0] == AWDL_SELF_CHAN;
    case 1:
        return (e[0] & 0x10) != 0 && (e[1] == 149 || e[1] == 151 || e[1] == 155);
    }
    return false;
}

/* The first slot of s that covers 149, into e (2 bytes). */
static bool awdlSeqFind149(const struct AwdlSeq *s, uint8_t *e)
{
    size_t i;

    for (i = 0; i < AWDL_SEQ_SLOTS; i++)
        if (awdlSlotCovers(s, s->slot[i])) {
            e[0] = s->slot[i][0];
            e[1] = s->slot[i][1];
            return true;
        }
    return false;
}

/* FNV-1a, 32 bit: the hash of what we announce, so a change shows as one ring line. */
static uint32_t awdlFnv(uint32_t h, const uint8_t *p, size_t n)
{
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

/* The values of TLV 4 that change with the clock and the engine's setters. */
struct AwdlT4Vals {
    uint8_t nextChan, masterChan, presence;
    uint8_t ext[4], master[6];
    uint16_t txCounter, awPeriod, afPeriod, extLen, comLen, remaining, awSeq;
};

static void awdlPut16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8;
}

/* TLV 4 into out (at most AWDL_T4_MAX bytes) from t and the sequence structure seq[0..seqLen); its length. */
static size_t awdlTlv4Put(uint8_t *out, const struct AwdlT4Vals *t, const uint8_t *seq, size_t seqLen)
{
    uint8_t *v = out + 3;
    size_t vl = AWDL_T4_FIXED + seqLen + 2;

    memset(v, 0, vl);
    out[0] = 4;
    awdlPut16(out + 1, (uint16_t)vl);
    v[0] = t->nextChan;
    awdlPut16(v + 1, t->txCounter);
    v[3] = t->masterChan;
    awdlPut16(v + 5, t->awPeriod);
    awdlPut16(v + 7, t->afPeriod);
    awdlPut16(v + 9, 0x1800);
    awdlPut16(v + 11, t->extLen);
    awdlPut16(v + 13, t->comLen);
    awdlPut16(v + 15, t->remaining);
    memcpy(v + 17, t->ext, 4);
    memcpy(v + 21, t->master, 6);
    v[27] = t->presence;
    awdlPut16(v + 29, t->awSeq);
    memcpy(v + AWDL_T4_FIXED, seq, seqLen);
    return 3 + vl;
}

/* TLV 0x12 into out (at most AWDL_T12_MAX bytes) from the sequence structure seq[0..seqLen); its length. */
static size_t awdlTlv12Put(uint8_t *out, const uint8_t *seq, size_t seqLen)
{
    size_t vl = seqLen + 3;

    out[0] = 0x12;
    awdlPut16(out + 1, (uint16_t)vl);
    memcpy(out + 3, seq, seqLen);
    memset(out + 3 + seqLen, 0, 3);
    return 3 + vl;
}

/*
 * The sequence we announce, in s (which comes from a peer's sequence and keeps its encoding, step count and fill
 * channel). arm 1: every slot is e149. arm 2: e149 where the engine's own slot eng[i] is 149, empty elsewhere. arm 3:
 * the slots of s that cover 149 stay as the peer wrote them, the others are emptied. Returns the slots served.
 */
static uint32_t awdlSeqProject(struct AwdlSeq *s, uint32_t arm, const uint32_t *eng, const uint8_t *e149)
{
    uint32_t i, served = 0;

    for (i = 0; i < AWDL_SEQ_SLOTS; i++) {
        bool serve = arm == 1 || (arm == 2 ? eng[i] == AWDL_SELF_CHAN : awdlSlotCovers(s, s->slot[i]));

        if (arm != 3) {
            s->slot[i][0] = serve ? e149[0] : 0;
            s->slot[i][1] = serve ? e149[1] : 0;
        } else if (!serve) {
            s->slot[i][0] = s->slot[i][1] = 0;
        }
        if (serve)
            served++;
    }
    return served;
}

/* TU from ns to the next slot start: truncated as M0.1 announced it, or rounded to the nearest TU (awdlclk 4). */
static uint32_t awdlTuLeft(uint64_t nextNs, uint64_t ns, bool round)
{
    return nextNs > ns ? (uint32_t)((nextNs - ns + (round ? AWDL_TU_NS / 2 : 0)) / AWDL_TU_NS) : 0;
}

/* The AW number at ns, as M0.1 reckoned it: the slot in progress (nextSeq - 4) plus its 16-TU window (0 to 3). */
static uint32_t awdlSeqAt(uint64_t ns, uint64_t nextNs, uint64_t periodNs, uint32_t nextSeq)
{
    uint64_t lastStart = nextNs - periodNs;
    uint32_t seq = nextSeq - 4;

    if (ns > lastStart)
        seq += (uint32_t)((ns - lastStart) / (16 * AWDL_TU_NS)) & 3;
    return seq;
}

/* TU left in the 16-TU AW in progress at ns (1 to 16). */
static uint32_t awdlAwLeft(uint64_t ns, uint64_t nextNs, uint64_t periodNs)
{
    uint64_t lastStart = nextNs - periodNs;

    return ns > lastStart ? 16 - (uint32_t)(((ns - lastStart) / AWDL_TU_NS) % 16) : 16;
}

/*
 * awdlclk 2: is the sender of the frame heard at rxNs the clock reference? The reference (ref, *refRoot, *refNs: the
 * time it was last heard) is sticky: the first node heard, a node that is its own master over one that follows
 * another, and a new node after 3 s of silence. true: sender is the reference; *changed: it just became it.
 */
static bool awdlRefPick(uint8_t *ref, bool *refRoot, uint64_t *refNs, const uint8_t *sender, bool root,
                        uint64_t rxNs, bool *changed)
{
    bool same;

    *changed = false;
    if (*refNs == 0 || rxNs - *refNs > 3000000000ULL || (!*refRoot && root && memcmp(ref, sender, 6) != 0)) {
        if (*refNs == 0 || memcmp(ref, sender, 6) != 0) {
            memcpy(ref, sender, 6);
            *changed = true;
        }
        *refRoot = root;
    }
    same = memcmp(ref, sender, 6) == 0;
    if (same) {
        *refNs = rxNs;
        *refRoot = root;
    }
    return same;
}

/* The frame with TLV 4 and TLV 0x12 (when withChan) in front of its own TLVs: out gets its first 40 bytes, the TLVs, the rest. false: it does not fit. */
static bool awdlFrameCompose(uint8_t *out, size_t outSize, const uint8_t *frame, size_t len, const uint8_t *t4,
                             size_t n4, const uint8_t *t12, size_t n12, bool withChan, size_t *outLen)
{
    size_t off = 40 + n4 + (withChan ? n12 : 0);

    if (len < 40 || off + (len - 40) > outSize)
        return false;
    memcpy(out, frame, 40);
    memcpy(out + 40, t4, n4);
    if (withChan)
        memcpy(out + 40 + n4, t12, n12);
    memcpy(out + off, frame + 40, len - 40);
    *outLen = off + (len - 40);
    return true;
}

/* A setter value if the engine has sent that setter (valid has the bit) and it is in lo..hi, else the default. */
static uint32_t awdlSetVal(uint32_t valid, uint32_t bit, uint32_t v, uint32_t def, uint32_t lo, uint32_t hi)
{
    return (valid & (1u << bit)) != 0 && v >= lo && v <= hi ? v : def;
}

/* The bytes of p as hex text (3 characters each, trailing space) for the ring. */
static void awdlHexStr(char *out, size_t outLen, const uint8_t *p, size_t n)
{
    size_t i, len = 0;

    out[0] = '\0';
    for (i = 0; i < n && len + 4 < outLen; i++)
        len += (size_t)snprintf(out + len, outLen - len, "%02x ", p[i]);
}

void AirportItlwm::awdlTxSend(uint8_t *frame, size_t len)
{
    uint8_t out[AWDL_TX_MAX];
    char types[80];
    uint64_t ns;
    uint32_t us, usPhy, seq, txCounter;
    bool truthBuilt = false;	/* M2a WP5: the TLVs of this frame were built by awdltruth, not copied from a peer */
    size_t vlen, n;
    int ret;

    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    if (qwz_awdlgate >= 2 && awdlAfSuppressed() && len >= 31 && frame[24] == 0x7f && frame[28] == 0x08) {	/* WP4 (M2a): AF_TX_MODE 3, "suppress action frames" (Infra on 2G): the engine's PSF and MIF stay home */
        awdlAfHeld++;
        if (qwz_log_ratelimit(&awdlAfLog, 4, 64))
            qwz_log("airport: awdl0: tx held back: AF_TX_MODE 3 (suppress action frames), subtype %u, %u so far\n", frame[30], awdlAfHeld);
        return;
    }
    if (len >= 40 && frame[24] == 0x7f && frame[28] == 0x08) {
        if (qwz_awdltruth != awdlCfgTruth || qwz_awdlclk != awdlCfgClk) {
            /* M2a WP5: the switches changed since the last frame: say so, and log the TLVs again with the next build */
            awdlCfgTruth = qwz_awdltruth;
            awdlCfgClk = qwz_awdlclk;
            awdlTruthHash = 0;
            awdlClkSample = 8;
            if (qwz_log_ratelimit(&awdlCfgLog, 16, 16))
                qwz_log("qwz: awdls: config truth=%d clk=0x%x phy_tx_time = target + %u us (lat mean %u us), setters seen 0x%x\n",
                        awdlCfgTruth, awdlCfgClk, (awdlCfgClk & 8) ? awdlPhyDelayUs() : 0, qwz_awdl_lat_us, awdlSet.valid);
        }
        if (qwz_awdltruth != 0 && awdlTlvFind(frame + 40, len - 40, AWDL_TLV_SYNC_PARAMS, &vlen) == NULL &&
            awdlTruthBuild(out, sizeof(out), frame, len, ns, &n)) {
            /* M2a WP5 (awdltruth 1 to 3): TLVs 4 and 0x12 built, not copied; awdlTruthBuild wrote the whole frame */
            frame = out;
            len = n;
            truthBuilt = true;
        } else if (awdlTlvFind(frame + 40, len - 40, AWDL_TLV_SYNC_PARAMS, &vlen) == NULL && awdlSyncTlvLen != 0 &&
            len + awdlSyncTlvLen + awdlChanTlvLen <= sizeof(out)) {
            memcpy(out, frame, 40);
            n = 40;
            memcpy(out + n, awdlSyncTlv, awdlSyncTlvLen);
            if (awdlAwOn) {
                uint8_t *s = out + n + 3;
                uint64_t lastStart = awdlAwNextNs - awdlAwPeriodNs;

                seq = awdlAwSeq - 4;	/* the window in progress */
                if (ns > lastStart)
                    seq += (uint32_t)((ns - lastStart) / (16 * AWDL_TU_NS)) & 3;
                txCounter = awdlTxCounterTu(ns);	/* M2a WP5: the M0.1 expression, rounded with awdlclk 4 */
                s[0] = 149;
                s[1] = txCounter & 0xff;
                s[2] = (txCounter >> 8) & 0xff;
                s[3] = 149;
                memcpy(s + 21, awdlAwSelfMaster ? frame + 10 : awdlAwMaster, 6);
                s[29] = seq & 0xff;
                s[30] = (seq >> 8) & 0xff;
            }
            n += awdlSyncTlvLen;
            if (awdlChanTlvLen != 0 && awdlTlvFind(frame + 40, len - 40, AWDL_TLV_CHANSEQ, &vlen) == NULL) {
                memcpy(out + n, awdlChanTlv, awdlChanTlvLen);
                n += awdlChanTlvLen;
            }
            memcpy(out + n, frame + 40, len - 40);
            len = n + len - 40;
            frame = out;
        }
        /* phy_tx_time and target_tx_time (the AWDL fixed header), us, as debug.qwz.awdltest=5. */
        us = (uint32_t)(ns / 1000);
        frame[32] = frame[36] = us & 0xff;
        frame[33] = frame[37] = (us >> 8) & 0xff;
        frame[34] = frame[38] = (us >> 16) & 0xff;
        frame[35] = frame[39] = (us >> 24) & 0xff;
        usPhy = us;
        if (qwz_awdlclk & 8) {
            /* M2a WP5 (awdlclk 8): phy_tx_time is target_tx_time plus the measured submit-to-completion delay; the four stamps above stay as M0.1 had them */
            usPhy = us + awdlPhyDelayUs();
            frame[32] = usPhy & 0xff;
            frame[33] = (usPhy >> 8) & 0xff;
            frame[34] = (usPhy >> 16) & 0xff;
            frame[35] = (usPhy >> 24) & 0xff;
        }
        if (awdlClkSample != 0 && awdlAwOn) {
            /* the first frames after a switch change: what the clock policy announced, with or without the builder */
            awdlClkSample--;
            if (qwz_log_ratelimit(&awdlClkSampleLog, 256, 16))
                qwz_log("qwz: awdls: clock sample: %llu us to the slot start, tx_counter %u, AW %u, phy_tx_time = target + %u us, truth %d clk 0x%x\n",
                        (unsigned long long)(awdlAwNextNs > ns ? (awdlAwNextNs - ns) / 1000 : 0), awdlTxCounterTu(ns),
                        awdlSeqAt(ns, awdlAwNextNs, awdlAwPeriodNs, awdlAwSeq) & 0xffff, usPhy - us, qwz_awdltruth, qwz_awdlclk);
        }
        static unsigned int tlvChgLogs;
        /* M2a (WP3, I4): TLV evidence of the frame as it will leave: hex of TLV 4 and 0x12 on change, the first PSF and MIF whole */
        bool tlvChanged = qwz_awdlx_tlvs(1, frame, len) != 0 && qwz_log_ratelimit(&tlvChgLogs, 16, 64);

        if (awdlTxTlvLogged < 6 || tlvChanged) {
            awdlTxTlvLogged++;
            awdlTlvTypes(frame + 40, len - 40, types, sizeof(types));
            qwz_log("airport: awdl0: tx subtype %u, %zu bytes, TLVs: %s(sync TLV %s)\n", frame[30], len, types,
                    truthBuilt ? "built from this node's state (awdltruth)" : awdlSyncTlvLen ? "from a peer, patched" : "NONE CAPTURED YET");
        }
    }
    ret = static_cast<ItlQwz *>(fHalService)->awdlTx(frame, len);
    qwz_awdlx_tx_result(qwz_awdlx_class(frame, len), ret);	/* M2a (WP3, I10) */
    if (ret == 0)
        awdlTxSent++;
    else
        awdlTxFailed++;
    if ((ret == 0 && (awdlTxSent <= 8 || (awdlTxSent & 255) == 0)) ||
        (ret != 0 && (awdlTxFailed <= 4 || (awdlTxFailed & 255) == 0))) {
        qwz_log("airport: awdl0: tx %s #%u: to %02x:%02x:%02x:%02x:%02x:%02x, subtype %u, %zu bytes (%d)%s\n",
                ret == 0 ? "sent" : "FAILED", ret == 0 ? awdlTxSent : awdlTxFailed,
                frame[4], frame[5], frame[6], frame[7], frame[8], frame[9],
                len > 30 ? frame[30] : 0xff, len, ret,
                ret == ENOTCONN ? ": no AWDL listen (debug.qwz.awdltest=6)" : "");
        qwz_log_publish();
    }
}

void AirportItlwm::awdlTxTimerFired(OSObject *owner, IOTimerEventSource *)
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);
    struct AwdlTxQueue *q = self ? self->awdlTxq : NULL;
    uint8_t frame[AWDL_TX_MAX];
    size_t len;
    uint64_t now, next = 0;
    bool autoDue, autoKept = false;
    int i;

    if (q == NULL || self->awdlTxStopped)
        return;
    for (;;) {
        IOLockLock(self->awdlTxLock);
        if (q->head == q->tail) {
            IOLockUnlock(self->awdlTxLock);
            break;
        }
        len = q->len[q->head % AWDL_TX_SLOTS];
        memcpy(frame, q->frame[q->head % AWDL_TX_SLOTS], len);
        q->head++;
        IOLockUnlock(self->awdlTxLock);
        self->awdlTxSend(frame, len);
    }
    for (i = 0; i < AWDL_AUTO_SLOTS; i++) {
        absolutetime_to_nanoseconds(mach_absolute_time(), &now);
        IOLockLock(self->awdlTxLock);
        len = q->autoLen[i];
        if (i == AWDL_AUTO_FOLLOW && ((qwz_awdlclk & 16) == 0 || self->awdlAwSelfMaster || q->autoLen[1] != 0)) {
            /* M2a WP5: slot 3 is the follower's PSF: only with awdlclk 16, while another node is master and our own PSF (slot 2) is off */
            if ((qwz_awdlclk & 16) == 0)
                q->autoLen[i] = 0;	/* the lock is held: a template kept while the bit was on is not sent when it comes back on */
            len = 0;
        }
        autoDue = len != 0 && now - q->autoLastNs[i] >= awdlAutoNs[i];
        if (autoDue) {
            memcpy(frame, q->autoFrame[i], len);
            q->autoLastNs[i] = now;
        }
        if (len != 0) {
            /* The earliest time a kept frame is due again. */
            uint64_t due = q->autoLastNs[i] + awdlAutoNs[i];
            if (!autoKept || due < next)
                next = due;
            autoKept = true;
        }
        IOLockUnlock(self->awdlTxLock);
        if (autoDue && self->awdlLinkUp && i == AWDL_AUTO_FOLLOW) {
            self->awdlTxFollower++;
            if (qwz_log_ratelimit(&self->awdlFollowerLog, 8, 64))
                qwz_log("qwz: awdls: follower PSF #%u (every 440 TU, awdlclk 16), master %02x:%02x:%02x:%02x:%02x:%02x\n",
                        self->awdlTxFollower, self->awdlAwMaster[0], self->awdlAwMaster[1], self->awdlAwMaster[2],
                        self->awdlAwMaster[3], self->awdlAwMaster[4], self->awdlAwMaster[5]);
        }
        if (autoDue && self->awdlLinkUp)
            self->awdlTxSend(frame, len);
    }
    if (autoKept && self->awdlLinkUp) {
        absolutetime_to_nanoseconds(mach_absolute_time(), &now);
        self->awdlTxTimer->setTimeoutUS(next > now ? (uint32_t)((next - now) / 1000) + 1 : 1);
    }
}

/*
 * The AW clock. AWDL time is availability windows (AWs) of aw_period TU
 * (64 by default), numbered by a sequence counter every peer shares with
 * its master. Broadcom's firmware runs it and reports each window
 * (handleAvailabilityWindowStart/End); IO80211AWDLPeerManager stores the
 * sequence number (AW0 = seq & ~63 must advance, or no peer is safe for
 * election), checks the interval and the channel, and times out without
 * the events ("Did not get AW start event"). The Qualcomm firmware knows
 * nothing of AWDL, so the host runs it: a workloop timer, aligned to the
 * master's sync parameters TLV whenever one of its frames arrives.
 */
#define AWDL_M_AW_START		41	/* APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START (36 B; Tahoe: 41, 42 is the extension log) */
#define AWDL_M_AW_END		43	/* APPLE80211_M_AWDL_AVAILABILITY_WINDOW_EXTENSIONS_END (40 B) */
#define AWDL_AW_PERIOD_TU	64
#define AWDL_AW_LENGTH_TU	16

/*
 * WP4 (M2a): awdl0's data lane — pacing, the gate and the requeue (docs/research-airdrop-2026-11-06.md section 2.5 and
 * appendix A.2; spec-WP4.md). Pacing is behind debug.qwz.awdlhyg, the gate behind debug.qwz.awdlgate; with both 0 none of it runs.
 *
 * Threads. outputStart runs on awdl0's output thread: it builds the frame and calls awdlGateSubmit, which enters the
 * command gate (runAction). Everything else runs on the workloop thread with the gate held: the completion callback
 * from ItlQwz (awdlTxDone: bookkeeping, it never transmits), the receive observers (awdlGateObserve: bookkeeping) and
 * awdlGateTimer, which does the releases. The state below is only touched inside the command gate, so it needs no lock.
 * Nothing here sleeps: the lane refuses (EAGAIN) instead of waiting for a WMI credit.
 */
#define AWDL_GATE_ENTRIES	16		/* frames held by the gate, waiting or in flight, all peers */
#define AWDL_GATE_PEERS		8
#define AWDL_GATE_GROUP		AWDL_GATE_PEERS	/* the pseudo peer of group-addressed frames */
#define AWDL_GATE_PEER_MAX	4		/* frames per peer, waiting or in flight */
#define AWDL_GATE_ATT_MAX	3		/* transmissions of one frame */
#define AWDL_GATE_WAIT_NS	(2000ULL * 1000 * 1000)		/* a frame that waits this long is dropped */
#define AWDL_GATE_HEARD_NS	(30ULL * 1000 * 1000)		/* A.2 (a): a frame from the peer this recently */
#define AWDL_GATE_ACK_NS	(16 * AWDL_TU_NS)		/* A.2 (c): the AW extension */
#define AWDL_GATE_GUARD_NS	(3 * AWDL_TU_NS)		/* A.2 (b) and the group rule: clear of the slot's edges */
#define AWDL_GATE_SLOT_NS	(64 * AWDL_TU_NS)
#define AWDL_GATE_SCHED_NS	(5000ULL * 1000 * 1000)		/* a peer's channel sequence older than this is not used */

enum { kGateFree, kGateWait, kGateFlight };
enum { kRelHeard = 1, kRelAck, kRelSched, kRelGroup };
static const char *const awdlRelName[] = { "-", "peer heard", "peer acked", "shared slot", "served slot" };

struct AwdlGEntry {
    uint8_t state, peer, att, retry, disc, cls, pad[2];
    uint16_t len, seq;
    uint32_t gen, epoch;
    uint64_t enqSeq, enqNs, notBeforeNs;
    uint8_t frame[AWDL_TX_MAX];
};

struct AwdlGPeer {
    uint8_t mac[6], used, schedOk, inflight, queued, pad[2];
    uint32_t logSched;		/* limiter of the "channel sequence" ring line, per peer, so that every peer is shown */
    uint8_t sched[16];		/* the peer's channel sequence (its TLV 0x12), one channel per 64 TU slot */
    uint64_t heardNs, ackNs, schedNs;
};

struct AwdlGate {
    struct AwdlGEntry e[AWDL_GATE_ENTRIES];
    struct AwdlGPeer p[AWDL_GATE_PEERS + 1];
    uint64_t enqCount;
    uint32_t gen, epoch, live;
    uint32_t logSig, logQuiet;		/* awdlGateLog: the counters' signature at the last print, and 1 + the quiet samples since (0: nothing printed yet) */
    struct {
        uint32_t enq, relHeard, relAck, relSched, relGroup, ok, noack, disc, rq, dNoack, dFull, dStale, dDown, dOther;
        uint32_t stale, flushed, hold, logRel, logCmp, logDrop, logFlush;
    } cnt;
};

struct AwdlGateReq {
    const uint8_t *frame;
    size_t len;
    int ret;
};

/*
 * WP3's books for awdl0's data frames (qwz_awdlx.h; spec-WP3.md 10.2): handed = pre + ref + cmpl + out + flush - rq + held + wdrop.
 * outputStart's call site (WP3 V8c) counts the first hand-over of every packet, here the hand-over to the gate: a refusal is `ref`,
 * an acceptance leaves the packet in `held` (below) until it is released, then in `out` (its pool slot), then in `cmpl`.
 *   held   +1 when a frame enters the gate or goes back into it, -1 when it is released or leaves unsent
 *   rq     +1 when a frame goes back into the gate after a completion: that completion is already in `cmpl`, and the frame is
 *          counted again by `held` and later by `out`, so `rq` takes the extra one out at the same instant (the books close at every instant)
 *   wdrop  +1 for a frame that leaves the gate unsent (waited 2 s, the lane is closed, the lane refused it); a frame dropped after its
 *          LAST completion (no ACK three times) is already counted by that completion and adds nothing
 * Without WP3 (QWZ_AWDLX_H not defined) they expand to nothing.
 */
#ifdef QWZ_AWDLX_H
#define AWDL_EV_HELD(cls, d)		((void)__atomic_add_fetch(&qwz_awdlx.held[(cls)], (uint32_t)(d), __ATOMIC_RELAXED))
#define AWDL_EV_WDROP(cls)		((void)__atomic_add_fetch(&qwz_awdlx.wdrop[(cls)], 1u, __ATOMIC_RELAXED))
#define AWDL_EV_REQUEUE(cls)		((void)__atomic_add_fetch(&qwz_awdlx.rq[(cls)], 1u, __ATOMIC_RELAXED))
#else
#define AWDL_EV_HELD(cls, d)		((void)0)
#define AWDL_EV_WDROP(cls)		((void)0)
#define AWDL_EV_REQUEUE(cls)		((void)0)
#endif

/*
 * Where `ns` falls in the cluster's schedule: the 64 TU slot (the engine's numbering, (aw_seq / 4) % 16) and the time
 * into it. awdlAwSeq and awdlAwNextNs are the NEXT window's sequence number and start, so the period in progress is
 * awdlAwSeq - 4, as awdlTxSend has it. False while the AW clock is not running.
 */
bool AirportItlwm::awdlSlotAt(uint64_t ns, uint32_t *slot, uint64_t *phaseNs)
{
    int64_t d, periods;

    if (!awdlAwOn || awdlAwPeriodNs == 0)
        return false;
    d = (int64_t)ns - (int64_t)awdlAwNextNs;
    periods = d >= 0 ? d / (int64_t)awdlAwPeriodNs : -((-d + (int64_t)awdlAwPeriodNs - 1) / (int64_t)awdlAwPeriodNs);
    *phaseNs = (uint64_t)(d - periods * (int64_t)awdlAwPeriodNs);
    *slot = ((uint32_t)(awdlAwSeq + (uint32_t)(periods * 4)) >> 2) & 15;
    return true;
}

/* Whether we are on the radio's channel in this slot, as our announcement says. The radio is on the infrastructure channel in every slot outside a scan. WP5 (awdltruth >= 2) announces fewer slots (its tx12 lines) but does not narrow this: the gate still treats every slot as served (an open item of M2a). */
bool AirportItlwm::awdlServedSlot(uint32_t slot)
{
    (void)slot;
    return true;
}

struct AwdlGPeer *AirportItlwm::awdlGatePeer(const uint8_t *mac, bool create)
{
    struct AwdlGate *g = awdlGate;
    int i, victim = -1;

    for (i = 0; i < AWDL_GATE_PEERS; i++)
        if (g->p[i].used && memcmp(g->p[i].mac, mac, 6) == 0)
            return &g->p[i];
    if (!create)
        return NULL;
    for (i = 0; i < AWDL_GATE_PEERS; i++) {
        if (!g->p[i].used) {
            victim = i;
            break;
        }
        if (g->p[i].queued == 0 && g->p[i].inflight == 0 && (victim < 0 || g->p[i].heardNs < g->p[victim].heardNs))
            victim = i;
    }
    if (victim < 0)
        return NULL;
    memset(&g->p[victim], 0, sizeof(g->p[victim]));
    memcpy(g->p[victim].mac, mac, 6);
    g->p[victim].used = 1;
    return &g->p[victim];
}

/* Every AWDL frame heard (the receive observers, inside the gate): when the sender was last heard, and its channel sequence (TLV 0x12: 16 slots, simple, legacy or opclass encoding). */
void AirportItlwm::awdlGateObserve(const uint8_t *frame, size_t len, uint64_t ns)
{
    struct AwdlGate *g = awdlGate;
    struct AwdlGPeer *p;
    const uint8_t *v;
    size_t vlen;
    struct AwdlSeq q;
    int i;

    if (g == NULL || awdlGateStopped || len < 24 || (frame[10] & 1) || memcmp(frame + 10, awdlMac, 6) == 0)
        return;
    p = awdlGatePeer(frame + 10, true);
    if (p == NULL)
        return;
    p->heardNs = ns;
    if (len > 40 && frame[24] == 0x7f && frame[28] == 0x08 &&
        (v = awdlTlvFind(frame + 40, len - 40, AWDL_TLV_CHANSEQ, &vlen)) != NULL && awdlSeqParse(v, vlen, &q)) {
        for (i = 0; i < 16; i++)	/* fix1 (hooks H1): the test WP5 uses for "this slot is on 149" (awdlSlotCovers), so that a legacy entry's centre channel (151 for a 40 MHz block, 155 for 80 MHz) counts as the 149 the rule below asks for; every other entry is its channel byte as before */
            p->sched[i] = awdlSlotCovers(&q, q.slot[i]) ? AWDL_SELF_CHAN : (q.enc == 1 ? q.slot[i][1] : q.slot[i][0]);
        p->schedOk = 1;
        p->schedNs = ns;
        if (qwz_log_ratelimit(&p->logSched, 4, 256))	/* the raw first entry is printed too, so that the first hardware run shows the real bytes whether or not the legacy decode (spec D5) holds */
            qwz_log("airport: awdl0: gate: channel sequence of %02x:%02x:%02x:%02x:%02x:%02x (enc %u, entry 0 %02x %02x): %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
                    p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5], q.enc, q.slot[0][0], q.slot[0][1], p->sched[0], p->sched[1],
                    p->sched[2], p->sched[3], p->sched[4], p->sched[5], p->sched[6], p->sched[7], p->sched[8], p->sched[9],
                    p->sched[10], p->sched[11], p->sched[12], p->sched[13], p->sched[14], p->sched[15]);
    }
    if (p->queued != 0)
        awdlGateArm(0);			/* a frame is waiting for this peer: look again at once */
}

IOReturn AirportItlwm::awdlGateObserveGated(OSObject *target, void *arg0, void *, void *, void *)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, target);
    struct AwdlGateReq *r = (struct AwdlGateReq *)arg0;
    uint64_t ns;

    if (that == NULL)
        return kIOReturnNotReady;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    that->awdlGateObserve(r->frame, r->len, ns);
    return kIOReturnSuccess;
}

/* The next run of the gate at `atNs` (0: as soon as possible); an earlier pending run stays. Any thread. */
void AirportItlwm::awdlGateArm(uint64_t atNs)
{
    uint64_t now;

    if (awdlGate == NULL || awdlGateTimer == NULL || awdlGateStopped)
        return;
    absolutetime_to_nanoseconds(mach_absolute_time(), &now);
    if (atNs <= now)
        atNs = now + 1000;
    if (awdlGateAtNs > now && awdlGateAtNs <= atNs)
        return;
    awdlGateAtNs = atNs;
    awdlGateTimer->setTimeoutUS((uint32_t)((atNs - now) / 1000));
}

void AirportItlwm::awdlGateTimerFired(OSObject *owner, IOTimerEventSource *)
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);

    if (self == NULL || self->awdlGate == NULL || self->awdlGateStopped)
        return;
    self->awdlGateAtNs = 0;
    self->awdlGateRun();
    self->awdlKickOutput();		/* the safety net of a missed completion: a stalled output thread starts again, and stalls again if there is still no room */
}

/* outputStart stopped for lack of room; the next completion (or the gate timer) starts the output thread again. */
void AirportItlwm::awdlKickOutput()
{
#ifdef __PRIVATE_SPI__
    if (awdlBsd != NULL && awdlLinkUp && __atomic_exchange_n(&awdlOutBlocked, 0u, __ATOMIC_ACQ_REL))
        awdlBsd->startOutputThread();
#endif
}

/*
 * Pacing, on the output thread: is there room for one more packet? With the gate it is a free entry and no peer at its
 * limit of 4 frames, without it a free Data slot of the pool. A closed lane (no listen vdev, a teardown, DEAD) counts as room: the packet is taken
 * and refused, as before, rather than held for a lane that may stay down. When there is no room the blocked flag is
 * set, and the room is looked at once more so that a completion between the two reads is not missed.
 */
bool AirportItlwm::awdlLaneHasRoom()
{
    struct qwz_awdl_lane li;
    bool room;
    int pass, i;

    for (pass = 0; pass < 2; pass++) {
        static_cast<ItlQwz *>(fHalService)->awdlLane(&li);
        if (li.state != QWZ_AWDL_LANE_OPEN)
            return true;
        room = li.data_room > 0;
        if (awdlGateOn()) {		/* a free entry and no peer at its limit of 4 (the next packet's destination is not known before it is dequeued) */
            room = awdlGate->live < AWDL_GATE_ENTRIES;
            for (i = 0; room && i < AWDL_GATE_PEERS; i++)
                if (awdlGate->p[i].used && awdlGate->p[i].queued + awdlGate->p[i].inflight >= AWDL_GATE_PEER_MAX)
                    room = false;
        }
        if (room) {
            if (pass == 1)
                __atomic_exchange_n(&awdlOutBlocked, 0u, __ATOMIC_ACQ_REL);
            return true;
        }
        if (pass == 0) {
            __atomic_store_n(&awdlOutBlocked, 1u, __ATOMIC_RELEASE);
            uint64_t now;

            absolutetime_to_nanoseconds(mach_absolute_time(), &now);
            awdlGateArm(now + AWDL_GATE_SLOT_NS);		/* about 65 ms: the safety net */
        }
    }
    return false;
}

/* The lane as the gate sees it: false (and the held frames flushed) while it is closed; a new firmware epoch flushes what the old one held. */
bool AirportItlwm::awdlGateSync(struct qwz_awdl_lane *li)
{
    struct AwdlGate *g = awdlGate;

    static_cast<ItlQwz *>(fHalService)->awdlLane(li);
    if (li->state != QWZ_AWDL_LANE_OPEN) {
        if (g->live != 0)
            awdlGateFlush("the lane is closed (no listen vdev, a teardown, or DEAD)");
        return false;
    }
    if (li->epoch != g->epoch) {
        if (g->live != 0)
            awdlGateFlush("the firmware restarted");
        g->epoch = li->epoch;
    }
    return true;
}

void AirportItlwm::awdlGateDrop(struct AwdlGEntry *e, uint32_t *counter, const char *why)
{
    struct AwdlGate *g = awdlGate;
    struct AwdlGPeer *p = &g->p[e->peer];

    if (e->state == kGateWait) {		/* a frame that completed (kGateFlight) had its in-flight count taken back by awdlTxDone already, and its completion is the WP3 books' last word on it */
        if (p->queued != 0)
            p->queued--;
        AWDL_EV_HELD(e->cls, -1);
        AWDL_EV_WDROP(e->cls);
    }
    (*counter)++;
    if (qwz_log_ratelimit(&g->cnt.logDrop, 8, 64))
        qwz_log("airport: awdl0: gate: frame to %02x:%02x:%02x:%02x:%02x:%02x dropped: %s (attempt %u of %u)\n", e->frame[4],
                e->frame[5], e->frame[6], e->frame[7], e->frame[8], e->frame[9], why, e->att, AWDL_GATE_ATT_MAX);
    e->state = kGateFree;
    g->live--;
}

/* The lane is gone: every held frame is dropped (nothing will complete). */
void AirportItlwm::awdlGateFlush(const char *why)
{
    struct AwdlGate *g = awdlGate;
    uint32_t n = 0;
    int i;

    for (i = 0; i < AWDL_GATE_ENTRIES; i++) {
        if (g->e[i].state == kGateFree)
            continue;
        if (g->e[i].state == kGateWait) {
            AWDL_EV_HELD(g->e[i].cls, -1);
            AWDL_EV_WDROP(g->e[i].cls);
        }
        g->e[i].state = kGateFree;
        n++;
    }
    for (i = 0; i <= AWDL_GATE_PEERS; i++)
        g->p[i].queued = g->p[i].inflight = 0;
    g->live = 0;
    g->cnt.flushed += n;
    g->cnt.dDown += n;
    if (n != 0 && qwz_log_ratelimit(&g->cnt.logFlush, 8, 64)) {	/* a lane that reads open but refuses every frame (ic_bss gone, gate disabled) flushes once per packet: the line is limited, flushed and dDown above keep counting */
        qwz_log("airport: awdl0: gate: %u frame(s) dropped: %s\n", n, why);
        qwz_log_publish();
    }
}

/* A frame that was not acknowledged or was discarded: back to the queue with the retry bit, not before the next window starts. */
void AirportItlwm::awdlGateRequeue(struct AwdlGEntry *e, uint64_t ns)
{
    struct AwdlGate *g = awdlGate;

    e->state = kGateWait;
    e->retry = 1;
    e->notBeforeNs = awdlAwOn ? awdlAwNextNs : ns + 16 * AWDL_TU_NS;
    g->p[e->peer].queued++;
    AWDL_EV_HELD(e->cls, 1);
    AWDL_EV_REQUEUE(e->cls);
    g->cnt.rq++;
}

/* The A.2 release conditions for one waiting frame. */
bool AirportItlwm::awdlGateEligible(struct AwdlGEntry *e, uint64_t ns, int *why)
{
    struct AwdlGate *g = awdlGate;
    struct AwdlGPeer *p = &g->p[e->peer];
    struct ieee80211com *ic;
    uint32_t slot;
    uint64_t phase;
    int ch;

    if (e->notBeforeNs > ns)
        return false;
    if (qwz_awdlgate == 0) {			/* the gate was switched off while frames were held: they drain at once (counted as "peer heard") */
        *why = kRelHeard;
        return true;
    }
    if (e->peer == AWDL_GATE_GROUP) {		/* no ACK to wait for: inside the availability window of a served slot, 3 TU clear of its edges. This tree's engine reports the window for the whole 64 TU slot (start event at the slot start, end event 1 TU before the next: awdlAwTimerFired), so the window here is the slot (A.2: "never outside an AW") */
        *why = kRelGroup;
        if (!awdlSlotAt(ns, &slot, &phase))
            return true;			/* no AW clock: there is nothing to wait for */
        return awdlServedSlot(slot) && phase >= AWDL_GATE_GUARD_NS && phase + AWDL_GATE_GUARD_NS <= AWDL_GATE_SLOT_NS;
    }
    if (p->inflight >= AWDL_GATE_PEER_MAX)
        return false;
    if (p->heardNs != 0 && ns - p->heardNs < AWDL_GATE_HEARD_NS) {
        *why = kRelHeard;
        return true;
    }
    if (p->ackNs != 0 && ns - p->ackNs < AWDL_GATE_ACK_NS) {
        *why = kRelAck;
        return true;
    }
    if (qwz_awdlgate >= 2 && p->schedOk && ns - p->schedNs < AWDL_GATE_SCHED_NS && awdlSlotAt(ns, &slot, &phase)) {
        ic = fHalService->get80211Controller();
        ch = ic->ic_bss != NULL ? ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan) : 0;
        if (ch != 0 && p->sched[slot] == ch &&
            (phase >= AWDL_GATE_GUARD_NS || p->sched[(slot + 15) & 15] == ch) &&
            (phase + AWDL_GATE_GUARD_NS <= AWDL_GATE_SLOT_NS || p->sched[(slot + 1) & 15] == ch)) {
            *why = kRelSched;
            return true;
        }
    }
    return false;
}

/* One frame to the lane. 0 and the frame is in flight; else the lane's errno and the frame is still waiting. */
int AirportItlwm::awdlGateRelease(struct AwdlGEntry *e, uint64_t ns, int why)
{
    struct AwdlGate *g = awdlGate;
    struct AwdlGPeer *p = &g->p[e->peer];
    int ret;

    if (e->retry) {				/* the retry bit and the number the first transmission had */
        e->frame[1] |= 0x08;
        e->frame[22] = (uint8_t)((e->seq & 0xf) << 4);
        e->frame[23] = (uint8_t)(e->seq >> 4);
    }
    ret = static_cast<ItlQwz *>(fHalService)->awdlDataTxEx(e->frame, e->len, ((e->gen & 0xffffff) << 8) | (uint32_t)((e - g->e) + 1));
    if (ret != 0)
        return ret;
    e->att++;
    e->state = kGateFlight;
    if (p->queued != 0)
        p->queued--;
    p->inflight++;
    AWDL_EV_HELD(e->cls, -1);
    if (why == kRelHeard)
        g->cnt.relHeard++;
    else if (why == kRelAck)
        g->cnt.relAck++;
    else if (why == kRelSched)
        g->cnt.relSched++;
    else
        g->cnt.relGroup++;
    if (qwz_log_ratelimit(&g->cnt.logRel, 16, 128))
        qwz_log("airport: awdl0: gate: sent to %02x:%02x:%02x:%02x:%02x:%02x: %s, attempt %u, waited %llu ms\n", e->frame[4],
                e->frame[5], e->frame[6], e->frame[7], e->frame[8], e->frame[9], awdlRelName[why], e->att,
                (unsigned long long)((ns - e->enqNs) / 1000000ULL));
    return 0;
}

/*
 * Release what may go, oldest first, and keep the order per peer (a waiting frame blocks the later ones of its peer).
 * Re-tested at every AW start, whenever a frame from a waiting peer arrives, and after every completion.
 */
void AirportItlwm::awdlGateRun()
{
    struct AwdlGate *g = awdlGate;
    struct qwz_awdl_lane li;
    uint8_t order[AWDL_GATE_ENTRIES];
    uint16_t blocked = 0;
    uint64_t ns, wake = 0;
    int n = 0, i, j, k, ret, why = 0;

    if (g == NULL || awdlGateStopped || !awdlGateSync(&li))
        return;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    for (i = 0; i < AWDL_GATE_ENTRIES; i++) {
        if (g->e[i].state != kGateWait)
            continue;
        for (j = n; j > 0 && g->e[order[j - 1]].enqSeq > g->e[i].enqSeq; j--)
            order[j] = order[j - 1];
        order[j] = (uint8_t)i;
        n++;
    }
    for (k = 0; k < n; k++) {
        struct AwdlGEntry *e = &g->e[order[k]];
        uint16_t bit = (uint16_t)(1u << e->peer);
        uint64_t at;

        if (ns - e->enqNs > AWDL_GATE_WAIT_NS) {
            awdlGateDrop(e, &g->cnt.dStale, "it waited 2 s for a window");
            continue;
        }
        if (!(blocked & bit) && li.data_room > 0 && awdlGateEligible(e, ns, &why)) {
            ret = awdlGateRelease(e, ns, why);
            if (ret == 0) {
                li.data_room--;
                continue;
            }
            if (ret == EAGAIN || ret == ENOSPC || ret == EBUSY)
                g->cnt.hold++;		/* no credit, no room, or the lane is closed for tx_params: it waits */
            else if (ret == ENOTCONN || ret == ENXIO || ret == ESHUTDOWN) {
                awdlGateFlush("the lane refused: not connected");
                return;
            } else {
                awdlGateDrop(e, &g->cnt.dOther, "the lane refused it");
                continue;
            }
        }
        blocked |= bit;
        at = e->notBeforeNs > ns ? e->notBeforeNs : 0;
        if (at != 0 && (wake == 0 || at < wake))
            wake = at;
        at = e->enqNs + AWDL_GATE_WAIT_NS;
        if (wake == 0 || at < wake)
            wake = at;
    }
    if (blocked != 0) {		/* something still waits: look again at the next window start, and never sooner than 1 ms (a late AW timer must not make this a loop) */
        uint64_t next = awdlAwOn && awdlAwNextNs > ns ? awdlAwNextNs : ns + 16 * AWDL_TU_NS;

        if (wake == 0 || next < wake)
            wake = next;
        if (wake < ns + 1000000)
            wake = ns + 1000000;
        awdlGateArm(wake);
    }
}

/* A frame from awdl0 (the Ethernet frame already turned into an 802.11 Data frame by outputStart): into the gate, then try to send. */
int AirportItlwm::awdlGateEnqueue(const uint8_t *frame, size_t len)
{
    struct AwdlGate *g = awdlGate;
    struct AwdlGEntry *e = NULL;
    struct AwdlGPeer *p = NULL;
    struct qwz_awdl_lane li;
    uint64_t ns;
    int i, peer;

    if (g == NULL || awdlGateStopped || len < 26 || len > AWDL_TX_MAX)
        return EINVAL;
    if (!awdlGateSync(&li))
        return ENOTCONN;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    if (frame[4] & 1)
        peer = AWDL_GATE_GROUP;
    else {
        p = awdlGatePeer(frame + 4, true);
        if (p == NULL || p->queued + p->inflight >= AWDL_GATE_PEER_MAX) {
            g->cnt.dFull++;
            return ENOBUFS;
        }
        peer = (int)(p - g->p);
    }
    for (i = 0; i < AWDL_GATE_ENTRIES; i++)
        if (g->e[i].state == kGateFree) {
            e = &g->e[i];
            break;
        }
    if (e == NULL) {
        g->cnt.dFull++;
        return ENOBUFS;
    }
    memcpy(e->frame, frame, len);
    e->len = (uint16_t)len;
    e->seq = 0;
    e->state = kGateWait;
    e->peer = (uint8_t)peer;
    e->att = e->retry = e->disc = 0;
    e->cls = peer == AWDL_GATE_GROUP ? QWZ_AWDL_CLS_MC : QWZ_AWDL_CLS_UC;
    e->gen = (++g->gen) & 0xffffff;
    e->epoch = g->epoch;
    e->enqSeq = ++g->enqCount;
    e->enqNs = ns;
    e->notBeforeNs = 0;
    g->p[peer].queued++;
    g->live++;
    g->cnt.enq++;
    AWDL_EV_HELD(e->cls, 1);
    awdlGateRun();
    return 0;
}

IOReturn AirportItlwm::awdlGateSubmitGated(OSObject *target, void *arg0, void *, void *, void *)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, target);
    struct AwdlGateReq *r = (struct AwdlGateReq *)arg0;

    if (that == NULL)
        return kIOReturnNotReady;
    r->ret = that->awdlGateEnqueue(r->frame, r->len);
    return kIOReturnSuccess;
}

/* outputStart's call (the output thread): one Data frame into the gate. */
int AirportItlwm::awdlGateSubmit(const uint8_t *frame, size_t len)
{
    struct AwdlGateReq r = { frame, len, EIO };
    IOCommandGate *gate = getCommandGate();

    if (gate == NULL)
        return ENXIO;
    gate->runAction(awdlGateSubmitGated, &r);
    return r.ret;
}

/*
 * ItlQwz's completion callback (the workloop thread, the gate held, from inside the completion event): bookkeeping
 * only. OK frees the entry (and, with an ACK RSSI, opens the peer for A.2 (c)); NO_ACK requeues it with the retry bit
 * and the same sequence number, up to AWDL_GATE_ATT_MAX transmissions; DISCARD and INSPECT requeue it once. The sends
 * happen in the gate timer, which this arms. The pacing flag is cleared and the output thread started in every case.
 */
void AirportItlwm::awdlTxDone(void *arg, const struct qwz_awdl_txdone *d)
{
    AirportItlwm *self = (AirportItlwm *)arg;
    struct AwdlGate *g;
    struct AwdlGEntry *e;
    struct AwdlGPeer *p;
    uint64_t ns;
    uint32_t idx;

    if (self == NULL)		/* setAwdlTxDone(NULL, NULL) stores the argument first: a completion that loaded the function just before the clear arrives with NULL */
        return;
    g = self->awdlGate;
    if (g != NULL && d->cookie != 0 && !self->awdlGateStopped) {
        idx = (d->cookie & 0xff) - 1;
        e = idx < AWDL_GATE_ENTRIES ? &g->e[idx] : NULL;
        if (e == NULL || e->gen != (d->cookie >> 8) || e->state != kGateFlight || e->epoch != d->epoch) {
            g->cnt.stale++;
        } else {
            absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
            p = &g->p[e->peer];
            if (p->inflight != 0)
                p->inflight--;
            e->seq = (uint16_t)d->seq;
            if (qwz_log_ratelimit(&g->cnt.logCmp, 16, 128) || (d->status != 0 && (g->cnt.noack + g->cnt.disc) <= 16))
                qwz_log("airport: awdl0: gate: completion status %u for the frame to %02x:%02x:%02x:%02x:%02x:%02x: ack_rssi %d, %u us, seq %u, attempt %u\n",
                        d->status, e->frame[4], e->frame[5], e->frame[6], e->frame[7], e->frame[8], e->frame[9], d->ack_rssi,
                        d->lat_us, d->seq, e->att);
            if (d->status == QWZ_AWDL_ST_OK) {
                g->cnt.ok++;
                if (e->peer != AWDL_GATE_GROUP && d->ack_rssi != 0)
                    p->ackNs = ns;			/* acknowledged: the peer is listening (A.2 c) */
                e->state = kGateFree;
                g->live--;
            } else if (d->status == QWZ_AWDL_ST_NOACK) {
                g->cnt.noack++;
                if (e->peer == AWDL_GATE_GROUP) {	/* nobody acknowledges a group-addressed frame: nothing to repeat, and it counts as done */
                    g->cnt.ok++;
                    e->state = kGateFree;
                    g->live--;
                } else if (e->att >= AWDL_GATE_ATT_MAX)
                    self->awdlGateDrop(e, &g->cnt.dNoack, "no ACK");
                else
                    self->awdlGateRequeue(e, ns);
            } else if (d->status == QWZ_AWDL_ST_DISCARD || d->status == QWZ_AWDL_ST_INSPECT) {
                g->cnt.disc++;
                if (e->disc != 0 || e->att >= AWDL_GATE_ATT_MAX)
                    self->awdlGateDrop(e, &g->cnt.dOther, "discarded twice");
                else {
                    e->disc = 1;
                    self->awdlGateRequeue(e, ns);
                }
            } else
                self->awdlGateDrop(e, &g->cnt.dOther, "unknown completion status");
            self->awdlGateArm(0);
        }
    }
    self->awdlKickOutput();
}

/* awdl0's link: down empties the gate (the output thread is stopped and flushed by awdlLinkGated itself); a change either way forgets the peers, as WP2 forgets its peer-seen list at a listen start. */
void AirportItlwm::awdlGateLink(bool up)
{
    if (awdlGate == NULL)
        return;
    if (!up) {
        if (awdlGate->live != 0)
            awdlGateFlush("awdl0's link went down");
        if (awdlGateTimer != NULL && !awdlGateStopped)	/* stop() removes the timer from the workloop */
            awdlGateTimer->cancelTimeout();
        awdlGateAtNs = 0;
        __atomic_store_n(&awdlOutBlocked, 0u, __ATOMIC_RELEASE);
    }
    if (awdlGate->live == 0)			/* nothing is held, so no peer has a count to lose; the stamps (heard, acked, channel sequence) are all younger than 5 s anyway */
        memset(awdlGate->p, 0, sizeof(awdlGate->p));
}

/*
 * The gate's summary, two ring lines: called from awdlAwTimerFired about every 5 s (every 76th window) while awdl0 is up, NOT from the
 * sentinel. Plain reads; nothing but the signature below is written. Printed when a counter moved since the last print, and at least once a minute.
 */
void AirportItlwm::awdlGateLog()
{
    struct AwdlGate *g = awdlGate;
    const uint32_t *c;
    uint32_t sig = 0;
    size_t i;

    if (g == NULL || (g->cnt.enq == 0 && !qwz_awdlgate && !qwz_awdlhyg))
        return;
    c = (const uint32_t *)&g->cnt;
    for (i = 0; i < sizeof(g->cnt) / sizeof(uint32_t); i++)
        sig = sig * 31 + c[i];
    sig = sig * 31 + g->live + awdlOutBlocked + awdlAfHeld;
    if (g->logQuiet != 0 && sig == g->logSig && ++g->logQuiet < 12)
        return;
    g->logSig = sig;
    g->logQuiet = 1;
    qwz_log("qwz: awdlg: health[on=%d live=%u enq=%u rel=%u/%u/%u/%u ok=%u noack=%u disc=%u rq=%u hold=%u blocked=%u af=%u]\n",
            qwz_awdlgate, g->live, g->cnt.enq, g->cnt.relHeard, g->cnt.relAck, g->cnt.relSched, g->cnt.relGroup, g->cnt.ok,
            g->cnt.noack, g->cnt.disc, g->cnt.rq, g->cnt.hold, awdlOutBlocked, awdlAfHeld);
    qwz_log("qwz: awdlg: health drop[noack=%u full=%u stale=%u down=%u other=%u] stale=%u flushed=%u\n", g->cnt.dNoack,
            g->cnt.dFull, g->cnt.dStale, g->cnt.dDown, g->cnt.dOther, g->cnt.stale, g->cnt.flushed);
}

void AirportItlwm::awdlMaster(void *ctx, const uint8_t *master, int self)
{
    AirportItlwm *that = (AirportItlwm *)ctx;

    memcpy(that->awdlAwMaster, master, 6);
    that->awdlAwSelfMaster = self != 0;
    qwz_awdlx_master(master, self);	/* M2a (WP3, I7): role flip log and the mirror of the master we follow */
    if ((qwz_awdlclk & 16) && that->awdlTxq != NULL && that->awdlTxLock != NULL && !that->awdlTxStopped) {
        /* M2a WP5: the follower's PSF (repeat slot 3) belongs to whoever is master: with a new role the old frame is dropped, and as a follower one may be due now */
        if (self != 0) {
            IOLockLock(that->awdlTxLock);
            that->awdlTxq->autoLen[AWDL_AUTO_FOLLOW] = 0;
            IOLockUnlock(that->awdlTxLock);
        } else if (that->awdlTxTimer != NULL) {
            that->awdlTxTimer->setTimeoutUS(1);
        }
    }
}

void AirportItlwm::awdlChannelSequence(void *ctx, const uint32_t *channels, const uint32_t *flags)
{
    AirportItlwm *that = (AirportItlwm *)ctx;

    if (that->awdlTxLock != NULL)
        IOLockLock(that->awdlTxLock);	/* M2a WP5: awdlTruthBuild copies the sequence under the same lock */
    memcpy(that->awdlAwChan, channels, sizeof(that->awdlAwChan));
    memcpy(that->awdlAwChanFlags, flags, sizeof(that->awdlAwChanFlags));
    if (that->awdlTxLock != NULL)
        IOLockUnlock(that->awdlTxLock);
    qwz_awdlx_chanseq(channels, flags);	/* M2a (WP3, I5): all 16 entries of the engine's sequence, once per distinct sequence */
}

/*
 * M2a WP5: the engine's sync setters from the AWDL kext (bridge tx, repeat 4: struct qwz_awdl_setter_rec). Called on
 * the engine's thread with the engine's lock held, so it only copies under awdlTxLock and returns. The values are
 * kept either way; they are used (and logged) only with awdltruth or awdlclk on.
 */
int AirportItlwm::awdlSetterIn(const uint8_t *rec, size_t len)
{
    struct qwz_awdl_setter_rec r;
    struct AwdlSetters old;
    uint32_t bit = 0, i;
    bool changed;

    if (rec == NULL || len != sizeof(r) || awdlTxLock == NULL)
        return EINVAL;
    memcpy(&r, rec, sizeof(r));
    if (r.magic != QWZ_AWDL_SETTER_MAGIC)
        return EINVAL;
    IOLockLock(awdlTxLock);
    old = awdlSet;
    switch (r.index) {
    case 29:	/* setAWDL_SYNC_PARAMS: AW length, AW period, extension length, synchronization frame period */
        bit = 0;
        awdlSet.awLen = r.v[0];
        awdlSet.awPeriod = r.v[1];
        awdlSet.extLen = r.v[2];
        awdlSet.afPeriod = r.v[3];
        break;
    case 31:	/* setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS: four extension counts */
        bit = 1;
        for (i = 0; i < 4; i++)
            awdlSet.extCnt[i] = r.v[i];
        break;
    case 36:	/* setAWDL_MASTER_CHANNEL */
        bit = 2;
        awdlSet.masterChan = r.v[0];
        break;
    case 39:	/* setAWDL_PRESENCE_MODE */
        bit = 3;
        awdlSet.presence = r.v[0];
        break;
    case 45:	/* setAWDL_SECONDARY_MASTER_CHANNEL */
        bit = 4;
        awdlSet.secChan = r.v[0];
        break;
    case 46:	/* setAWDL_AF_TX_MODE: 3 suppress, 0 allow [KC setAFTXMode] */
        bit = 5;
        awdlSet.afTxMode = r.v[0];
        awdlAfTxMode = r.v[0];
        break;
    default:
        IOLockUnlock(awdlTxLock);
        return EINVAL;
    }
    awdlSet.valid |= 1u << bit;
    changed = memcmp(&old, &awdlSet, sizeof(old)) != 0;
    IOLockUnlock(awdlTxLock);
    if (changed && (qwz_awdltruth != 0 || qwz_awdlclk != 0) && qwz_log_ratelimit(&awdlSetLog, 32, 64))
        qwz_log("qwz: awdls: setter %u: %u %u %u %u %u %u (valid 0x%x)\n", r.index, r.v[0], r.v[1], r.v[2], r.v[3],
                r.v[4], r.v[5], awdlSet.valid);
    return 0;
}

/* The one place a refused build is counted and logged (awdltruth 1 to 3 then sends the M0.1 copy). */
bool AirportItlwm::awdlTruthRefuse(const char *why)
{
    awdlTruthRefused++;
    if (qwz_log_ratelimit(&awdlTruthRefuseLog, 8, 256))
        qwz_log("qwz: awdls: truth=%d not applied (%s): the copy of a peer's TLVs goes out instead, %u refused so far\n",
                qwz_awdltruth, why, awdlTruthRefused);
    return false;
}

/* The TX down-counter: TU to the next slot start, truncated as in M0.1, or rounded to the nearest TU (awdlclk 4). */
uint32_t AirportItlwm::awdlTxCounterTu(uint64_t ns)
{
    return awdlTuLeft(awdlAwNextNs, ns, (qwz_awdlclk & 4) != 0);
}

/* awdlclk 8: how far phy_tx_time is stamped behind target_tx_time, us: the mean the HAL measured, 1730 until it has a sample [HK: awdltest=5, 300 frames, mean 1.73 ms, docs/research/run-logs-2026-11-05.md:148]. */
uint32_t AirportItlwm::awdlPhyDelayUs()
{
    uint32_t d = qwz_awdl_lat_us;

    if (d == 0)
        d = 1730;
    return d < 300 ? 300 : d > 3000 ? 3000 : d;
}

/*
 * debug.qwz.awdltruth 1 to 3 (Y1): the TLVs 4 and 0x12 of an engine frame, built from this node's own state instead of
 * copied from the last peer. out gets the first 40 bytes of frame, TLV 4, TLV 0x12 (unless frame has one) and the
 * engine's TLVs; *outLen its length. false: nothing built (the reason is logged) and the caller sends the M0.1 copy.
 * Workloop thread (awdlTxTimerFired); takes awdlTxLock only to copy the setters and the engine's sequence. It calls
 * nothing in the HAL, so DEAD, a late reset or Wi-Fi off cannot make it wait.
 */
bool AirportItlwm::awdlTruthBuild(uint8_t *out, size_t outSize, const uint8_t *frame, size_t len, uint64_t ns,
                                  size_t *outLen)
{
    struct AwdlSetters set;
    struct AwdlSeq tpl, mst, mine;
    struct AwdlT4Vals t;
    struct ieee80211com *ic;
    uint32_t eng[AWDL_SEQ_SLOTS];
    uint8_t e149[2] = { 0, 0 }, seqb[AWDL_SEQ_MAX], t4[AWDL_T4_MAX], t12[AWDL_T12_MAX];
    size_t seqLen, n4, n12, vlen;
    uint32_t cur, i, served, h, arm = (uint32_t)qwz_awdltruth;
    bool haveTpl, haveMst, haveEng = false, hasChan;

    if (!awdlAwOn || awdlAwPeriodNs == 0 || awdlTxLock == NULL)
        return awdlTruthRefuse("the AW clock is off");
    /* D18: the announcement says 149, and the radio is on the infrastructure channel (the listen vdev's): true only while that is 149 */
    ic = fHalService != NULL ? fHalService->get80211Controller() : NULL;
    if (ic == NULL || ic->ic_bss == NULL || ic->ic_bss->ni_chan == NULL || ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan) != AWDL_SELF_CHAN)
        return awdlTruthRefuse("the infrastructure channel is not 149 (D18)");
    /* The captured sequences: the followed master's if fresh, the last peer's TLV 0x12, the copy inside the last peer's TLV 4. */
    haveMst = !awdlAwSelfMaster && awdlMasterSeqLen != 0 && ns - awdlMasterSeqNs < 10000000000ULL &&
        memcmp(awdlMasterSeqMac, awdlAwMaster, 6) == 0 && awdlSeqParse(awdlMasterSeq, awdlMasterSeqLen, &mst);
    haveTpl = (awdlChanTlvLen > 3 + 6 && awdlSeqParse(awdlChanTlv + 3, awdlChanTlvLen - 3u, &tpl)) ||
        (awdlSyncTlvLen > 3 + AWDL_T4_FIXED + 6 &&
         awdlSeqParse(awdlSyncTlv + 3 + AWDL_T4_FIXED, awdlSyncTlvLen - 3u - AWDL_T4_FIXED, &tpl));
    if (haveMst)
        mine = mst;
    else if (haveTpl)
        mine = tpl;
    else
        return awdlTruthRefuse("no channel sequence of a peer captured yet");
    if (arm == 3 && !haveMst)
        arm = 2;	/* no fresh sequence of a followed master: the engine's own */
    IOLockLock(awdlTxLock);
    set = awdlSet;
    memcpy(eng, awdlAwChan, sizeof(eng));
    IOLockUnlock(awdlTxLock);
    for (i = 0; i < AWDL_SEQ_SLOTS; i++)
        if (eng[i] != 0)
            haveEng = true;
    if (arm != 3) {
        /* The entry for 149: a peer's own bytes, so the encoding is the peers' (the engine's is legacy, encoding 1 [HK: hook 35 dumps]). */
        if (!awdlSeqFind149(&mine, e149) && !(haveTpl && tpl.enc == mine.enc && awdlSeqFind149(&tpl, e149))) {
            if (qwz_log_ratelimit(&awdlTruthTplLog, 4, 256)) {
                /* open question 1 of the spec: the bytes of the sequence that has no slot this code decodes as 149 */
                char thex[3 * AWDL_SEQ_MAX + 4];

                awdlHexStr(thex, sizeof(thex), seqb, awdlSeqPut(&mine, seqb));
                qwz_log("qwz: awdls: the sequence of the %s has no slot that covers 149 (enc %u): %s\n", haveMst ? "followed master" : "last peer",
                        mine.enc, thex);
            }
            return awdlTruthRefuse("no captured peer sequence has a slot on 149 to take the entry from");
        }
        if (arm == 2 && !haveEng)
            return awdlTruthRefuse("the engine has not set a channel sequence yet");
    }
    served = awdlSeqProject(&mine, arm, eng, e149);	/* 1, 2: our 149 entry where we serve; 3: the master's own entries that cover 149 */
    cur = awdlSeqAt(ns, awdlAwNextNs, awdlAwPeriodNs, awdlAwSeq);	/* the AW number as M0.1 reckoned it */
    memset(&t, 0, sizeof(t));
    /* fix1 (hooks H2): the channel of the slot that starts when the TX down-counter runs out (the engine's numbering, (aw_seq / 4) % 16, of the NEXT window: awdlSlotAt), read from the sequence we announce, so that this frame does not say 149 for a slot its own TLV 0x12 leaves empty (Owly SPEC 3.2 type 4, offset 0: the channel of the next AW [PUB]). Arm 1 serves every slot: 149 as before */
    t.nextChan = awdlSlotCovers(&mine, mine.slot[(awdlAwSeq >> 2) & 15]) ? AWDL_SELF_CHAN : 0;
    t.masterChan = AWDL_SELF_CHAN;
    t.txCounter = (uint16_t)awdlTxCounterTu(ns);
    t.awPeriod = (uint16_t)awdlSetVal(set.valid, 0, set.awPeriod, 16, 4, 1024);
    t.comLen = (uint16_t)awdlSetVal(set.valid, 0, set.awLen, 16, 4, 1024);
    t.extLen = (uint16_t)awdlSetVal(set.valid, 0, set.extLen, 16, 0, 1024);
    t.afPeriod = (uint16_t)awdlSetVal(set.valid, 0, set.afPeriod, awdlAwSelfMaster ? 110 : 440, 16, 4096);
    t.remaining = (uint16_t)awdlAwLeft(ns, awdlAwNextNs, awdlAwPeriodNs);
    for (i = 0; i < 4; i++)
        t.ext[i] = (uint8_t)awdlSetVal(set.valid, 1, set.extCnt[i], 3, 0, 255);
    memcpy(t.master, awdlAwSelfMaster ? frame + 10 : awdlAwMaster, 6);
    t.presence = (uint8_t)awdlSetVal(set.valid, 3, set.presence, 4, 1, 16);
    t.awSeq = (uint16_t)cur;
    seqLen = awdlSeqPut(&mine, seqb);
    n4 = awdlTlv4Put(t4, &t, seqb, seqLen);
    n12 = awdlTlv12Put(t12, seqb, seqLen);
    hasChan = awdlTlvFind(frame + 40, len - 40, AWDL_TLV_CHANSEQ, &vlen) != NULL;
    if (!awdlFrameCompose(out, outSize, frame, len, t4, n4, t12, n12, !hasChan, outLen))
        return awdlTruthRefuse("the frame is too long to take the TLVs");
    awdlTruthBuilt++;
    /* The proof in the ring: a hash of what does not change with the clock; a new value logs both TLVs as they went out. */
    h = awdlFnv(2166136261u, t4, 3);				/* header only: the next AW channel follows the slot the clock is in (fix1, hooks H2), so it is not part of what "does not change with the clock" */
    h = awdlFnv(h, t4 + 3 + 3, 12);				/* value 3 to 14: master channel to the AW lengths */
    h = awdlFnv(h, t4 + 3 + 17, 12);				/* 17 to 28: extension counts, master, presence */
    h = awdlFnv(h, t4 + 3 + 31, n4 - 3 - 31);			/* 31 on: AP alignment, the sequence, the padding */
    h = awdlFnv(h, t12, n12);
    if (h != awdlTruthHash) {
        awdlTruthHash = h;
        if (qwz_log_ratelimit(&awdlTruthLog, 12, 64)) {
            char hex[3 * AWDL_T4_MAX + 4], engs[4 * AWDL_SEQ_SLOTS + 1];
            size_t k, m = 0;

            /* clamped to 3 digits and guarded: a value of the engine's buffer must never run this buffer over (refuter must_fix) */
            engs[0] = '\0';
            for (k = 0; k < AWDL_SEQ_SLOTS && m + 5 < sizeof(engs); k++)
                m += (size_t)snprintf(engs + m, sizeof(engs) - m, "%s%u", k ? "," : "", eng[k] > 999 ? 999u : eng[k]);
            qwz_log("qwz: awdls: tx truth=%d arm=%u clk=0x%x aw %u/%u/%u psf %u ext %u,%u,%u,%u pres %u seq=%u txc=%u (%llu us) rem=%u "
                    "served=%u/16 master=%02x:%02x:%02x:%02x:%02x:%02x tpl=%s set=0x%x built=%u\n", qwz_awdltruth, arm,
                    qwz_awdlclk, t.awPeriod, t.comLen, t.extLen, t.afPeriod, t.ext[0], t.ext[1], t.ext[2], t.ext[3],
                    t.presence, cur, t.txCounter, (unsigned long long)(awdlAwNextNs > ns ? (awdlAwNextNs - ns) / 1000 : 0), t.remaining, served, t.master[0], t.master[1], t.master[2], t.master[3],
                    t.master[4], t.master[5], haveMst ? "master" : "last peer", set.valid, awdlTruthBuilt);
            awdlHexStr(hex, sizeof(hex), t4, n4);
            qwz_log("qwz: awdls: tx4 %s\n", hex);
            awdlHexStr(hex, sizeof(hex), t12, n12);
            qwz_log("qwz: awdls: tx12 %s| engine %s\n", hex, engs);
            qwz_log_publish();
        }
    }
    return true;
}

/*
 * Every AWDL action frame with a TLV 4, from awdlAwFollow (the main gate, the HAL's receive path): the followed
 * master's channel sequence for awdltruth 3, and the clock reference for awdlclk 2. Returns NULL, or the reason to
 * realign the AW clock to this frame now (awdlclk 2, the engine says we are master, 20 s since the last alignment).
 */
const char *AirportItlwm::awdlSyncNote(const uint8_t *frame, size_t len, uint64_t rxNs)
{
    const uint8_t *tlvs = frame + 40, *v4, *v12;
    size_t l4 = 0, l12 = 0, n;
    struct AwdlSeq s;
    bool root, changed;

    v4 = awdlTlvFind(tlvs, len - 40, AWDL_TLV_SYNC_PARAMS, &l4);
    if (v4 == NULL || l4 < AWDL_T4_FIXED)
        return NULL;
    if (qwz_awdltruth == 3 && !awdlAwSelfMaster && memcmp(frame + 10, awdlAwMaster, 6) == 0) {
        v12 = awdlTlvFind(tlvs, len - 40, AWDL_TLV_CHANSEQ, &l12);
        if (v12 == NULL) {
            v12 = v4 + AWDL_T4_FIXED;	/* the copy inside TLV 4 */
            l12 = l4 - AWDL_T4_FIXED;
        }
        if (awdlSeqParse(v12, l12, &s)) {
            n = 6 + (size_t)AWDL_SEQ_SLOTS * s.w;
            if (n != awdlMasterSeqLen || memcmp(awdlMasterSeq, v12, n) != 0) {
                memcpy(awdlMasterSeq, v12, n);
                awdlMasterSeqLen = (uint8_t)n;
                if (qwz_log_ratelimit(&awdlRefLog, 8, 64)) {
                    char hex[3 * 38 + 4];

                    awdlHexStr(hex, sizeof(hex), v12, n);
                    qwz_log("qwz: awdls: sequence of the followed master %02x:%02x:%02x:%02x:%02x:%02x (enc %u): %s\n",
                            frame[10], frame[11], frame[12], frame[13], frame[14], frame[15], s.enc, hex);
                }
            }
            awdlMasterSeqNs = rxNs;
            memcpy(awdlMasterSeqMac, frame + 10, 6);	/* the guard above: this is the master the engine follows */
        }
    }
    if ((qwz_awdlclk & 2) == 0 || !awdlAwSelfMaster || memcmp(frame + 10, awdlMac, 6) == 0)
        return NULL;
    /* awdlclk 2: the reference is sticky: the first node heard, a node that is its own master over one that follows another, and a new one after 3 s of silence. */
    root = memcmp(v4 + 21, frame + 10, 6) == 0;
    if (!awdlRefPick(awdlBestMac, &awdlBestRoot, &awdlBestNs, frame + 10, root, rxNs, &changed))
        return NULL;
    if (changed && qwz_log_ratelimit(&awdlRefLog, 8, 64))
        qwz_log("qwz: awdls: clock reference %02x:%02x:%02x:%02x:%02x:%02x (%s)\n", frame[10], frame[11], frame[12],
                frame[13], frame[14], frame[15], root ? "master of its own tree" : "follows another node");
    if (awdlClkAlignNs != 0 && rxNs - awdlClkAlignNs < 20000000000ULL)
        return NULL;
    return root ? "master, 20 s policy, reference is a root" : "master, 20 s policy, reference follows another node";
}

int AirportItlwm::awdlPeerCb(void *ctx, const uint8_t *mac, int add)
{
    AirportItlwm *that = (AirportItlwm *)ctx;
    bool full;

    if (!add)
        return 0;	/* peers stay until the listen ends */
    if (that->awdlPeerCall == NULL || that->awdlTxLock == NULL || that->awdlTxStopped)
        return ENXIO;
    /*
     * M2a WP2. The AWDL kext of M2a passes setPEER_CACHE_CONTROL's op as QWZ_AWDL_PEER_OP + op (AirportWCN7850AWDLBridge.h); the M0.1 one passes 1
     * for every op, and so do awdlRxGated's first-sight calls. Broadcom's setter takes 0 (ADD) and 6 (UPDATE) as one command, 2 (DELETE) as
     * another and rejects the rest ("invalid opcode") [KC]. With debug.qwz.awdlpeers bit 2: add and update go on and ensure a firmware peer
     * (qwz_awdl_peer_add is idempotent); delete and any other op create nothing, and a firmware peer that exists stays until the listen ends,
     * as before. The counters count with the switch off too: they say which ops the engine uses. Runs on the engine's thread with its lock held:
     * it only counts, logs and queues.
     */
    if (add >= QWZ_AWDL_PEER_OP) {
        int op = add - QWZ_AWDL_PEER_OP;
        int kind = op == 0 ? 0 : op == 6 ? 1 : op == 2 ? 2 : 3;
        static unsigned int logged[4];
        static const char *const names[4] = { "add", "update", "delete", "other" };

        __atomic_fetch_add(&that->awdlPeerOps[kind], 1u, __ATOMIC_RELAXED);
        if ((qwz_awdlpeers & QWZ_AWDLP_OPS) != 0) {
            if (qwz_log_ratelimit(&logged[kind], 8, 64))
                qwz_log("airport: awdl0: peer cache op %d (%s) for %02x:%02x:%02x:%02x:%02x:%02x: %s\n", op, names[kind],
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                        kind < 2 ? "firmware peer requested (queue permitting)" : "no firmware peer created");
            if (kind >= 2)
                return 0;
        }
    }
    IOLockLock(that->awdlTxLock);
    full = that->awdlPeerQTail - that->awdlPeerQHead >= 8;
    if (!full) {
        memcpy(that->awdlPeerQueue[that->awdlPeerQTail % 8], mac, 6);
        that->awdlPeerQTail++;
    }
    IOLockUnlock(that->awdlTxLock);
    if (full)
        return ENOSPC;
    thread_call_enter(that->awdlPeerCall);
    return 0;
}

void AirportItlwm::awdlPeerCallFired(thread_call_param_t p0, thread_call_param_t)
{
    AirportItlwm *self = (AirportItlwm *)p0;
    uint8_t mac[6];
    int ret;

    for (;;) {
        IOLockLock(self->awdlTxLock);
        if (self->awdlPeerQHead == self->awdlPeerQTail || self->awdlTxStopped) {
            IOLockUnlock(self->awdlTxLock);
            return;
        }
        memcpy(mac, self->awdlPeerQueue[self->awdlPeerQHead % 8], 6);
        self->awdlPeerQHead++;
        IOLockUnlock(self->awdlTxLock);
        ret = static_cast<ItlQwz *>(self->fHalService)->awdlPeerAdd(mac);
        qwz_log("airport: awdl0: firmware peer for %02x:%02x:%02x:%02x:%02x:%02x -> %d%s\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ret,
                ret == ENOTCONN ? " (no AWDL listen: debug.qwz.awdltest=6)" : "");
        qwz_log_publish();
    }
}

/*
 * A data frame from an AWDL peer (the whole 802.11 frame): after the
 * header (QoS or not) the AWDL LLC/SNAP (aa aa 03 00 17 f2 08 00) and the
 * 8-byte AWDL data header, whose last two bytes are the ethertype. It goes
 * to awdl0 as an Ethernet frame (A1, A2, ethertype, payload).
 */
void AirportItlwm::awdlDataRx(void *arg, const uint8_t *frame, size_t len, int rssi)
{
    AirportItlwm *self = (AirportItlwm *)arg;
    static const uint8_t awdlSnap[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00 };
    size_t hdr, body;
    mbuf_t m;
    uint8_t *p;
    char hex[3 * 40 + 1];
    size_t i, n = 0;

    if (qwz_awdlgate && len >= 24 && self->getCommandGate() != NULL) {	/* WP4 (M2a): the sender was heard; awdlDataRx may be called outside the gate */
        struct AwdlGateReq gr = { frame, len, 0 };

        self->getCommandGate()->runAction(awdlGateObserveGated, &gr);
    }
    if (self->awdlBsd == NULL || !self->awdlLinkUp || len < 24)
        return;
    hdr = 24 + ((frame[0] & 0x80) ? 2 : 0);	/* QoS control */
    if (++self->awdlDataRxCount <= 3) {
        hex[0] = '\0';
        for (i = 0; i < 40 && hdr + i < len; i++)
            n += snprintf(hex + n, sizeof(hex) - n, "%02x ", frame[hdr + i]);
        qwz_log("airport: awdl0: data rx #%u from %02x:%02x:%02x:%02x:%02x:%02x, fc %02x%02x, %zu bytes, rssi %d | %s\n",
                self->awdlDataRxCount, frame[10], frame[11], frame[12], frame[13], frame[14], frame[15],
                frame[0], frame[1], len, rssi, hex);
        qwz_log_publish();
    }
    if (len < hdr + 16 || memcmp(frame + hdr, awdlSnap, 8) != 0)
        return;
    body = len - hdr - 16;
    qwz_awdlx_trace(0, frame + 4, frame + 10, (unsigned)(frame[hdr + 14] << 8 | frame[hdr + 15]), frame + hdr + 16, body);	/* M2a (WP3, I6) */
    m = self->allocatePacket((UInt32)(14 + body));
    if (m == NULL)
        return;
    p = (uint8_t *)mbuf_data(m);
    memcpy(p, frame + 4, 6);	/* the destination: A1 */
    memcpy(p + 6, frame + 10, 6);	/* the source: A2 */
    p[12] = frame[hdr + 14];	/* the ethertype: the AWDL data header's last two bytes */
    p[13] = frame[hdr + 15];
    memcpy(p + 14, frame + hdr + 16, body);
    self->awdlBsd->inputPacket(m, (UInt32)(14 + body), 0, NULL);
}

/*
 * A frame from the master we follow: its sync parameters TLV (type 4, after
 * the 3-byte TLV header: next_aw_channel u8, tx_counter u16 = TU to the next
 * AW, master_channel u8, guard_time u8, aw_period u16, af_period u16, flags
 * u16, aw_ext_length u16, aw_com_length u16, remaining_aw_length u16, 4 u8
 * extension limits, master_addr[6], presence_mode u8, reserved u8,
 * aw_seq_number u16 = the current AW, ap_beacon_alignment_delta u16) aligns
 * our clock. The engine numbers 16 TU slots, reports a start every 64 TU
 * (its sanity check: 65 +/- 10 ms, sequence + 4 each) on a multiple of 4,
 * and expects the channel of its sequence table at slot (seq / 4) % 16. So
 * the next start is the next multiple of 4 above aw_seq_number, tx_counter
 * TU after receipt. The TLV's aw_period is in other units (16 here): unused.
 */
void AirportItlwm::awdlAwFollow(const uint8_t *frame, size_t len, uint64_t rxNs)
{
    const uint8_t *p = frame + 40;	/* 802.11 header + AWDL fixed header */
    size_t off = 0, l;
    uint32_t seq, txCounter, period;
    int64_t drift;

    const char *why = NULL;	/* M2a WP5 (awdlclk 2): set when this frame realigns a clock that free-runs because the engine says we are master */

    if (len > 40 && (qwz_awdltruth == 3 || (qwz_awdlclk & 2)))
        why = awdlSyncNote(frame, len, rxNs);
    if (why == NULL && (awdlAwSelfMaster || len <= 40 || memcmp(frame + 10, awdlAwMaster, 6) != 0))
        return;
    len -= 40;
    while (off + 3 <= len) {
        l = p[off + 1] | (p[off + 2] << 8);
        if (off + 3 + l > len)
            return;
        if (p[off] == AWDL_TLV_SYNC_PARAMS && l >= 33) {
            const uint8_t *s = p + off + 3;
            txCounter = s[1] | (s[2] << 8);
            period = s[5] | (s[6] << 8);
            seq = s[29] | (s[30] << 8);
            /* Keep the 32-bit counter continuous across the TLV's 16 bits. */
            seq = (((awdlAwSeq & 0xffff0000u) | seq) & ~3u) + 4;
            if ((int32_t)(seq - awdlAwSeq) < -0x8000)
                seq += 0x10000;
            else if ((int32_t)(seq - awdlAwSeq) > 0x8000)
                seq -= 0x10000;
            drift = (int64_t)(rxNs + txCounter * AWDL_TU_NS) - (int64_t)awdlAwNextNs -
                (int64_t)(seq - awdlAwSeq) * (int64_t)awdlAwPeriodNs;
            /*
             * Realign now, window in progress or not (the end event is 1 TU
             * before the next start, so a window is nearly always in
             * progress): the pending end event fires as scheduled, the next
             * start carries the master's number.
             */
            awdlAwSeq = seq;
            awdlAwNextNs = rxNs + txCounter * AWDL_TU_NS;
            if (why != NULL) {
                awdlRealigns++;
                if (qwz_log_ratelimit(&awdlRealignLog, 16, 8)) {
                    qwz_log("qwz: awdls: AW realign #%u (%s): to %02x:%02x:%02x:%02x:%02x:%02x, seq %u, next AW in %u TU, "
                            "drift %lld us, %llu ms after the last alignment\n", awdlRealigns, why, frame[10], frame[11],
                            frame[12], frame[13], frame[14], frame[15], seq, txCounter, (long long)(drift / 1000),
                            (unsigned long long)(awdlClkAlignNs != 0 ? (rxNs - awdlClkAlignNs) / 1000000 : 0));
                    qwz_log_publish();
                }
            }
            awdlClkAlignNs = rxNs;
            qwz_awdlx_clk(1, awdlAwNextNs, awdlAwPeriodNs, awdlAwSeq, awdlAwSelfMaster);	/* M2a (WP3) */
            if (++awdlAwResyncs <= 8 || (awdlAwResyncs & 255) == 0) {
                qwz_log("airport: awdl0: AW clock #%u from %02x:%02x:%02x:%02x:%02x:%02x: next AW %u in %u TU, "
                        "tlv period %u, drift %lld us\n", awdlAwResyncs, frame[10], frame[11], frame[12],
                        frame[13], frame[14], frame[15], seq, txCounter, period, (long long)(drift / 1000));
                qwz_log_publish();
            }
            if (awdlAwTimer != NULL && awdlAwOn) {
                /* In a window: its end event first, 1 TU before the new start; else the start itself. */
                uint64_t at = awdlAwInWindow && awdlAwNextNs > rxNs + AWDL_TU_NS ? awdlAwNextNs - AWDL_TU_NS : awdlAwNextNs;

                awdlAwTimer->setTimeoutUS(at > rxNs ? (uint32_t)((at - rxNs) / 1000) + 1 : 1);
            }
            return;
        }
        off += 3 + l;
    }
}

void AirportItlwm::awdlAwTimerFired(OSObject *owner, IOTimerEventSource *)
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);
    uint8_t msg[40];
    uint64_t now;
    IOReturn ret;

    if (self == NULL || !self->awdlAwOn || self->awdlIf == NULL)
        return;
    absolutetime_to_nanoseconds(mach_absolute_time(), &now);
    /* M2a (WP3, I7): how late this timer event is against the time it was armed for */
    qwz_awdlx_late((int64_t)now - (int64_t)(self->awdlAwInWindow ? self->awdlAwNextNs - AWDL_TU_NS : self->awdlAwNextNs));
    memset(msg, 0, sizeof(msg));
    if (self->awdlAwInWindow) {
        /*
         * The period's end, just before the next start: the firmware time at
         * 4, big-endian as Broadcom copies it from its event (logAWEnd
         * byte-swaps it), 64 TU after the start's (sanityCheckAWEnd: the slot
         * time (end - start) >> 10 must be 64 +/- 10 TU); no CCA statistics
         * (the u16 at 0x10 is 0: none to read).
         */
        self->awdlAwInWindow = false;
        *(uint32_t *)(msg + 4) = OSSwapInt32((uint32_t)(self->awdlAwStartUs + AWDL_AW_PERIOD_TU * 1024));
        self->postMessage(self->awdlIf, AWDL_M_AW_END, msg, 40, true);
        self->awdlAwTimer->setTimeoutUS(self->awdlAwNextNs > now ? (uint32_t)((self->awdlAwNextNs - now) / 1000) + 1 : 1);
        return;
    }
    /*
     * A window's start, as Broadcom's handleAvailabilityWindowStart builds
     * it. The channel is the engine's own sequence entry for the slot (its
     * sanity check wants that, 0 for an empty slot), not the radio's. The
     * time at 0x14 is big-endian (logAWStart byte-swaps it into the slot
     * time's start); the one at 8 is only printed.
     */
    self->awdlAwStartUs = now / 1000;
    /*
     * M2a WP5 (awdlclk 1): the peers' sequence number is 16 bits wide and wraps every 17.9 min; the engine compares
     * ours with theirs, so it is given ours that way. The 32-bit count stays here.
     */
    *(uint32_t *)(msg + 0) = (qwz_awdlclk & 1) ? (self->awdlAwSeq & 0xffffu) : self->awdlAwSeq;
    if ((qwz_awdlclk & 1) && (self->awdlAwSeq & 0xffffu) < self->awdlAwPosted &&
        qwz_log_ratelimit(&self->awdlWrapLog, 16, 16))
        qwz_log("qwz: awdls: AW seq posted %u after %u (internal count %u): %s\n", self->awdlAwSeq & 0xffffu,
                self->awdlAwPosted, self->awdlAwSeq, self->awdlAwPosted > 0xfff0u ? "the 16-bit wrap" : "a step back");
    self->awdlAwPosted = self->awdlAwSeq & 0xffffu;
    *(uint32_t *)(msg + 4) = 1;
    *(uint32_t *)(msg + 8) = (uint32_t)self->awdlAwStartUs;
    *(uint32_t *)(msg + 0x14) = OSSwapInt32((uint32_t)self->awdlAwStartUs);
    *(uint32_t *)(msg + 0x18) = APPLE80211_VERSION;
    *(uint32_t *)(msg + 0x1c) = self->awdlAwChan[(self->awdlAwSeq >> 2) & 15];
    *(uint32_t *)(msg + 0x20) = self->awdlAwChanFlags[(self->awdlAwSeq >> 2) & 15];
    ret = self->postMessage(self->awdlIf, AWDL_M_AW_START, msg, 36, true);
    if (++self->awdlAwStarts <= 4 || (self->awdlAwStarts & 1023) == 0) {
        qwz_log("airport: awdl0: AW start #%u: seq %u (AW0 %u), %s, period %llu TU -> 0x%x\n", self->awdlAwStarts,
                self->awdlAwSeq, self->awdlAwSeq & ~63u, self->awdlAwSelfMaster ? "master" : "following",
                (unsigned long long)(self->awdlAwPeriodNs / AWDL_TU_NS), ret);
        qwz_log_publish();
    }
    if ((self->awdlAwStarts % 76) == 0 && self->awdlLinkUp)	/* WP4 (M2a): the gate's summary about every 5 s (76 windows of 65.5 ms) while awdl0 is up; from the AW clock, not from the sentinel */
        self->awdlGateLog();
    self->awdlAwSeq += 4;
    self->awdlAwNextNs += self->awdlAwPeriodNs;
    if (self->awdlAwNextNs <= now + 1000000)	/* fell behind (a late timer): skip ahead */
        self->awdlAwNextNs = now + self->awdlAwPeriodNs;
    self->awdlAwInWindow = true;
    qwz_awdlx_clk(1, self->awdlAwNextNs, self->awdlAwPeriodNs, self->awdlAwSeq, self->awdlAwSelfMaster);	/* M2a (WP3) */
    /* The end event 1 TU before the next start. */
    self->awdlAwTimer->setTimeoutUS(self->awdlAwNextNs > now + AWDL_TU_NS ?
                                    (uint32_t)((self->awdlAwNextNs - now - AWDL_TU_NS) / 1000) : 1);
}

void AirportItlwm::awdlAwStart(bool on)
{
    uint64_t now;

    if (awdlAwTimer == NULL)
        return;
    awdlAwTimer->cancelTimeout();
    awdlAwOn = on;
    awdlClkAlignNs = 0;		/* M2a WP5: no clock reference, followed-master sequence or posted number outlives a link change */
    awdlBestNs = 0;
    awdlMasterSeqLen = 0;
    awdlAwPosted = 0;
    if (!on)
        qwz_awdlx_clk(0, 0, 0, 0, 0);	/* M2a (WP3, I2, I7): the clock mirror */
    awdlAwInWindow = false;
    if (!on)
        return;
    absolutetime_to_nanoseconds(mach_absolute_time(), &now);
    awdlAwPeriodNs = AWDL_AW_PERIOD_TU * AWDL_TU_NS;
    awdlAwSeq &= ~3u;
    awdlAwNextNs = now + 1000000;
    qwz_awdlx_clk(1, awdlAwNextNs, awdlAwPeriodNs, awdlAwSeq, awdlAwSelfMaster);
    awdlAwTimer->setTimeoutMS(1);
}

int AirportItlwm::awdlCreate()
{
    struct qwz_awdl_bridge bridge = { QWZ_AWDL_BRIDGE_VERSION, qwz_vlog, qwz_log_publish, NULL, this, awdlMaster,
                                      awdlChannelSequence, awdlPeerCb };
    struct IOSkywalkEthernetInterface::RegistrationInfo info;
    IOEthernetAddress hw;
    ether_addr mac;
    const char *step;
    OSObject *obj;
    bool ok;

    if (qwz_awdl0_up()) {	/* merge glue (H2): two awdl0 must not exist */
        qwz_log("airport: awdl0: refused: the kext-owned awdl0 is up (debug.qwz.awdl0=down first)\n");
        return EEXIST;
    }
    if (awdlTried) {
        qwz_log("airport: awdl0: already %s; reboot to try again\n", awdlBsd ? "created" : "attempted");
        qwz_log_publish();
        return EEXIST;
    }
    obj = OSMetaClass::allocClassWithName(QWZ_AWDL_CLASS);
    awdlIf = OSDynamicCast(IO80211VirtualInterface, obj);
    if (awdlIf == NULL) {
        qwz_log("airport: awdl0: no %s class: load the AWDL kext first (sudo kmutil load -b %s)\n",
                QWZ_AWDL_CLASS, QWZ_AWDL_KEXT);
        qwz_log_publish();
        OSSafeReleaseNULL(obj);
        return ENOENT;
    }
    awdlTried = true;
    /* The AWDL engine's frames: a queue the bridge fills and awdlTxTimer drains on the workloop. */
    awdlTxq = (struct AwdlTxQueue *)IOMallocZero(sizeof(*awdlTxq));
    awdlTxLock = IOLockAlloc();
    awdlTxTimer = IOTimerEventSource::timerEventSource(this, awdlTxTimerFired);
    awdlAwTimer = IOTimerEventSource::timerEventSource(this, awdlAwTimerFired);
    awdlPeerCall = thread_call_allocate(awdlPeerCallFired, this);
    awdlGate = (struct AwdlGate *)IOMallocZero(sizeof(struct AwdlGate));	/* WP4 (M2a): pacing and gate state; like awdlTxq it lives until the kext goes */
    awdlGateTimer = IOTimerEventSource::timerEventSource(this, awdlGateTimerFired);
    if (awdlGateTimer != NULL && getWorkLoop()->addEventSource(awdlGateTimer) != kIOReturnSuccess)
        OSSafeReleaseNULL(awdlGateTimer);
    if (awdlAwTimer != NULL && getWorkLoop()->addEventSource(awdlAwTimer) != kIOReturnSuccess)
        OSSafeReleaseNULL(awdlAwTimer);
    if (awdlAwTimer == NULL)
        qwz_log("airport: awdl0: no AW timer: availability windows are not reported\n");
    awdlAwSelfMaster = true;
    if (awdlTxq != NULL && awdlTxLock != NULL && awdlTxTimer != NULL &&
        getWorkLoop()->addEventSource(awdlTxTimer) == kIOReturnSuccess) {
        bridge.tx = awdlTxEnqueue;
    } else {
        qwz_log("airport: awdl0: no TX queue: the AWDL engine's frames are not sent\n");
        if (awdlTxq != NULL)
            IOFree(awdlTxq, sizeof(*awdlTxq));
        awdlTxq = NULL;
        if (awdlTxLock != NULL)
            IOLockFree(awdlTxLock);
        awdlTxLock = NULL;
        OSSafeReleaseNULL(awdlTxTimer);
    }
    setProperty(QWZ_AWDL_BRIDGE_KEY, (void *)&bridge, sizeof(bridge));
    if (getHardwareAddress(&hw) != kIOReturnSuccess) {
        qwz_log("airport: awdl0: no hardware address\n");
        qwz_log_publish();
        return ENXIO;
    }
    /*
     * The AWDL listen vdev's address (qwz_awdltest_vdev_create): the firmware
     * sends awdl0's frames from that vdev, so peers see one address.
     */
    memcpy(&mac, &hw, sizeof(mac));
    mac.octet[0] |= 0x02;
    mac.octet[5] ^= 0x5a;
    memcpy(awdlMac, mac.octet, sizeof(awdlMac));
    qwz_log("airport: awdl0: ==== creating the AWDL interface (role 6) ====\n");
    qwz_log_publish();

    /* The AWDL kext's init(): init(), then IO80211VirtualInterface::init with role 6, interface id 2. */
    step = "init";
    if (!awdlIf->init(this, &mac, 6, "awdl"))
        goto fail;
    /*
     * M2a WP2 (debug.qwz.awdlpeers bit 4): awdl0 has its address from here on; the HAL keeps it for the listen vdev, which otherwise derives one
     * from the en1 address of the moment (qwz_awdltest_vdev_create). No gate, no wait: two stores.
     */
    static_cast<ItlQwz *>(fHalService)->setAwdlAddress(awdlMac);
    qwz_log("airport: awdl0: address %02x:%02x:%02x:%02x:%02x:%02x recorded for the listen vdev (debug.qwz.awdlpeers bit 4)\n",
            awdlMac[0], awdlMac[1], awdlMac[2], awdlMac[3], awdlMac[4], awdlMac[5]);
    /*
     * IO80211VirtualInterface::attach. Not IO80211Controller::attachInterface:
     * that stores the controller's primary (infra) interface, and Apple's
     * createProxInterface never calls it for awdl0.
     */
    step = "attach to the controller";
    if (!awdlIf->attach(this))
        goto fail;
    qwz_log("airport: awdl0: attached; creating its BSD interface\n");
    qwz_log_publish();
    step = "BSD interface";
    creatingAwdlBsd = true;
    ok = IONetworkController::attachInterface((IONetworkInterface **)&awdlBsd, false);
    creatingAwdlBsd = false;
    if (!ok || awdlBsd == NULL)
        goto fail;
    step = "initRegistrationInfo";
    memset(&info, 0, sizeof(info));
    if (!awdlIf->initRegistrationInfo(&info, 1, sizeof(info)))
        goto fail;
    /* The fields createProxInterface sets after initRegistrationInfo (AppleBCMWLANCoreMac, 26.7.1). */
    {
        static const char awdlPrefix[] = "awdl";
        uint8_t *ri = info.bytes;
        const char *prefix = awdlPrefix;

        *(uint32_t *)(ri + 0x0c) = 3;
        *(uint32_t *)(ri + 0x14) = 2;
        *(uint64_t *)(ri + 0x24) = 0x8000000080ULL;
        memcpy(ri + 0x30, &prefix, sizeof(prefix));
        *(uint32_t *)(ri + 0x38) = 0;
        *(uint16_t *)(ri + 0x3c) = 0x40;
        *(uint32_t *)(ri + 0x40) = 4;	/* 6 with IOPMResetPowerStateOnWake, which this controller does not set */
        memcpy(ri + 0x108, &mac, sizeof(mac));
    }
    step = "registration info copies";
    static_assert(sizeof(struct IOSkywalkNetworkInterface::RegistrationInfo) == sizeof(info),
                  "both copies take the whole registration info");
    awdlIf->mExpansionData->fRegistrationInfo = (struct IOSkywalkNetworkInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkNetworkInterface::RegistrationInfo));
    awdlIf->mExpansionData2->fRegistrationInfo = (struct IOSkywalkEthernetInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkEthernetInterface::RegistrationInfo));
    if (!awdlIf->mExpansionData->fRegistrationInfo || !awdlIf->mExpansionData2->fRegistrationInfo)
        goto fail;
    memcpy(awdlIf->mExpansionData->fRegistrationInfo, &info, sizeof(info));
    memcpy(awdlIf->mExpansionData2->fRegistrationInfo, &info, sizeof(info));
    awdlIf->deferBSDAttach(true);
    qwz_log("airport: awdl0: starting (IO80211VirtualInterface::start creates the AWDL engine)\n");
    qwz_log_publish();
    step = "start";
    if (!awdlIf->start(this))
        goto fail;
    qwz_log("airport: awdl0: started, role %d; registering the BSD interface\n", awdlIf->getInterfaceRole());
    qwz_log_publish();
    awdlBsd->setOwnAddress(mac.octet);
    awdlBsd->registerService();
    qwz_log("airport: awdl0: created. Check: ifconfig awdl0\n");
    qwz_log_publish();
    return 0;

fail:
    qwz_log("airport: awdl0: creation stopped at step \"%s\"; the interface stays as it is until a reboot\n", step);
    qwz_log_publish();
    return EIO;
}

/*
 * awdl0's link, in the gate as en1's (setLinkStateGated). Until the AWDL
 * interface's IO80211 link is up, macOS does not count it as usable: sharingd
 * never asks for AWDL. Broadcom raises it from setPOWER ("Indicate AWDL
 * interface LINK UP": setLinkState(2, 1)).
 */
IOReturn AirportItlwm::awdlLinkGated(OSObject *target, void *arg0, void *, void *, void *)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, target);
    IO80211LinkState state = (IO80211LinkState)(uint64_t)arg0;

    if (that == NULL || that->awdlIf == NULL || that->awdlBsd == NULL)
        return kIOReturnNotReady;
    that->awdlIf->setLinkState(state, state == kIO80211NetworkLinkUp ? 1 : 0);
    that->awdlBsd->setLinkState(state);
    that->awdlLinkUp = state == kIO80211NetworkLinkUp;
    that->awdlAwStart(that->awdlLinkUp);
    that->awdlGateLink(that->awdlLinkUp);	/* WP4 (M2a): a link down empties the gate */
    qwz_awdlx_link(that->awdlLinkUp);	/* M2a (WP3): the evidence layer follows awdl0's link */
    /*
     * awdl0's output thread, as setLinkStatus starts en1's: without it the
     * pull-model interface answers every send with ENETDOWN ("Network is
     * down"), and awdl0 never hands us a packet.
     */
#ifdef __PRIVATE_SPI__
    if (that->awdlLinkUp) {
        that->awdlBsd->startOutputThread();
    } else {
        that->awdlBsd->stopOutputThread();
        that->awdlBsd->flushOutputQueue();
    }
#endif
    return kIOReturnSuccess;
}

/*
 * Received AWDL action frames, as Broadcom's AppleBCMWLANProximityInterface::
 * handleActionFrame_rx hands them to the AWDL engine (IO80211Family 1200.13.1):
 * a buffer from awdl0's management frame pool holding the action frame body
 * (from the category byte, no 802.11 header), then
 * IO80211VirtualInterface::bpfTapInput(buffer, 12, header, 0x48). The 0x48-byte
 * header is Broadcom's: sender at 0, apple80211_channel at 0x0c, rate (500 kb/s)
 * at 0x28, RSSI at 0x2c, receive time (us) at 0x30. The firmware gives no
 * TSF with a management frame, so the time is the host's.
 */
extern "C++" void *qwz_vif_mgmt_frame_pool(IO80211VirtualInterface *vif)
    __asm__("__ZN23IO80211VirtualInterface16getMgmtFramePoolEv");
extern "C++" void qwz_vif_bpf_tap_input(IO80211VirtualInterface *vif, void *buffer, unsigned int dlt,
    void *header, unsigned long headerLength)
    __asm__("__ZN23IO80211VirtualInterface11bpfTapInputEP13IO80211BufferjPvm");

/* IO80211BufferPool and IO80211Buffer virtuals by slot (26.7.1 vtables, as Broadcom calls them). */
enum {
    kPoolGetBuffer = 35,	/* IO80211BufferPool::getBuffer() */
    kBufReturn = 35,		/* IO80211Buffer::returnBuffer() */
    kBufSetLength = 39,		/* IO80211Buffer::setLength(unsigned long) */
    kBufGetCapacity = 40,	/* IO80211Buffer::getCapacity() const */
    kBufWriteBytes = 47,	/* IO80211Buffer::writeBytes(unsigned long, void const*, unsigned long) */
};

template <typename R, typename... A>
static inline R qwzVcall(void *obj, unsigned slot, A... args)
{
    return ((R (*)(void *, A...))((*(void ***)obj)[slot]))(obj, args...);
}

struct AwdlRxFrame {
    const uint8_t *frame;
    size_t len;
    int rssi;
    uint32_t freq, chan, rate;
};

void AirportItlwm::awdlRx(void *arg, const uint8_t *frame, size_t len, int rssi, uint32_t freq,
                          uint32_t chan, uint32_t rate)
{
    AirportItlwm *self = (AirportItlwm *)arg;
    struct AwdlRxFrame r = { frame, len, rssi, freq, chan, rate };
    IOCommandGate *gate = self->getCommandGate();

    if (gate == NULL)
        return;
    /* runAction from the workloop's own thread just calls through. */
    gate->runAction(awdlRxGated, &r);
}

IOReturn AirportItlwm::awdlRxGated(OSObject *target, void *arg0, void *, void *, void *)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, target);
    const struct AwdlRxFrame *r = (const struct AwdlRxFrame *)arg0;
    struct apple80211_channel ch;
    uint8_t header[0x48];
    size_t body;
    uint64_t ns;
    void *pool, *buf;

    if (that == NULL || that->awdlIf == NULL || !that->awdlLinkUp || r->len <= 24)
        return kIOReturnNotReady;
    body = r->len - 24;
    pool = qwz_vif_mgmt_frame_pool(that->awdlIf);
    buf = pool ? qwzVcall<void *>(pool, kPoolGetBuffer) : NULL;
    if (buf == NULL || body > qwzVcall<unsigned long>(buf, kBufGetCapacity)) {
        if (buf != NULL)
            qwzVcall<void>(buf, kBufReturn);
        if (++that->awdlRxDropped <= 4 || (that->awdlRxDropped & 255) == 0)
            qwz_log("airport: awdl0: rx dropped #%u (%s, %zu bytes)\n", that->awdlRxDropped,
                    pool == NULL ? "no frame pool" : buf == NULL ? "pool empty" : "too long", body);
        return kIOReturnNoResources;
    }
    qwzVcall<void>(buf, kBufWriteBytes, (unsigned long)0, (const void *)(r->frame + 24), (unsigned long)body);
    qwzVcall<void>(buf, kBufSetLength, (unsigned long)body);

    memset(header, 0, sizeof(header));
    memcpy(header, r->frame + 10, 6);	/* the sender: 802.11 addr2 */
    ch.version = APPLE80211_VERSION;
    ch.channel = r->chan;
    ch.flags = (r->freq >= 5000 ? APPLE80211_C_FLAG_5GHZ : APPLE80211_C_FLAG_2GHZ) | APPLE80211_C_FLAG_20MHZ;
    memcpy(header + 0x0c, &ch, sizeof(ch));
    *(uint32_t *)(header + 0x28) = r->rate / 500;
    *(int32_t *)(header + 0x2c) = r->rssi;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    *(uint32_t *)(header + 0x30) = (uint32_t)(ns / 1000);
    qwz_vif_bpf_tap_input(that->awdlIf, buf, 12, header, sizeof(header));
    if (qwz_awdlgate)			/* WP4 (M2a): when the sender was last heard, and its channel sequence (inside the gate here) */
        that->awdlGateObserve(r->frame, r->len, ns);
    /* M2a (WP3, I4, I7): the TLV evidence and the clock error of this frame, taken BEFORE the clock follows it */
    static unsigned int rxChgLogs;
    bool tlvChanged = qwz_awdlx_tlvs(0, r->frame, r->len) != 0 && qwz_log_ratelimit(&rxChgLogs, 16, 64);

    if (that->awdlAwOn)
        that->awdlAwFollow(r->frame, r->len, ns);
    /* A sender not seen before: a firmware peer for it (queued; created off this thread). */
    {
        int i;

        /*
         * M2a WP2 (debug.qwz.awdlpeers bit 1): the list records senders given a firmware peer, and the firmware peers die with the listen vdev
         * (awdltest=7, a failed join, a firmware restart). The listen starts again with awdltest=6, which counts the HAL's generation up (so
         * does the lift of a quiesced listen): a changed count means the record is stale, so every sender audible now gets its firmware peer
         * again. Under the gate, like the list.
         */
        if ((qwz_awdlpeers & QWZ_AWDLP_RESET) != 0 && that->fHalService != NULL) {
            uint32_t gen = static_cast<ItlQwz *>(that->fHalService)->awdlPeerGeneration();

            if (gen != that->awdlPeerGen) {
                static unsigned int resets;

                if (that->awdlPeersSeen > 0 && qwz_log_ratelimit(&resets, 16, 16))
                    qwz_log("airport: awdl0: peer list reset (listen restarted, generation %u -> %u): %d sender(s) forgotten\n",
                            that->awdlPeerGen, gen, that->awdlPeersSeen);
                that->awdlPeersSeen = 0;
                that->awdlPeerGen = gen;
            }
        }

        for (i = 0; i < that->awdlPeersSeen; i++)
            if (memcmp(that->awdlPeerSeen[i], r->frame + 10, 6) == 0)
                break;
        if (i == that->awdlPeersSeen && i < 8) {
            memcpy(that->awdlPeerSeen[i], r->frame + 10, 6);
            that->awdlPeersSeen++;
            awdlPeerCb(that, r->frame + 10, 1);
        }
    }
    if (r->len > 40) {
        /* A peer's sync parameters and channel sequence TLVs: the templates awdlTxSend inserts. */
        const uint8_t *v;
        size_t vlen;
        char types[80];

        v = awdlTlvFind(r->frame + 40, r->len - 40, AWDL_TLV_SYNC_PARAMS, &vlen);
        if (v != NULL && vlen >= 33 && vlen + 3 <= sizeof(that->awdlSyncTlv)) {
            memcpy(that->awdlSyncTlv, v - 3, vlen + 3);
            that->awdlSyncTlvLen = (uint16_t)(vlen + 3);
            /*
             * The convention check: a peer's aw_seq_number and tx_counter
             * against our clock at the moment of receipt (a peer that follows
             * us should agree with it).
             */
            if (that->awdlAwOn && (that->awdlSyncLogged < 12 || (that->awdlSyncLogged & 1023) == 0)) {
                uint64_t lastStart = that->awdlAwNextNs - that->awdlAwPeriodNs;
                uint32_t ours = that->awdlAwSeq - 4 + (ns > lastStart ? (uint32_t)((ns - lastStart) / (16 * AWDL_TU_NS)) & 3 : 0);

                qwz_log("airport: awdl0: sync check: %02x:%02x:%02x:%02x:%02x:%02x subtype %u says aw_seq %u, next AW in %u TU; "
                        "our slot %u, next start in %lld TU, master %s\n", r->frame[10], r->frame[11], r->frame[12],
                        r->frame[13], r->frame[14], r->frame[15], r->frame[30], v[29] | (v[30] << 8), v[1] | (v[2] << 8),
                        ours, (long long)((int64_t)(that->awdlAwNextNs - ns) / (int64_t)AWDL_TU_NS),
                        that->awdlAwSelfMaster ? "us" : "them");
            }
            that->awdlSyncLogged++;
        }
        v = awdlTlvFind(r->frame + 40, r->len - 40, AWDL_TLV_CHANSEQ, &vlen);
        if (v != NULL && vlen >= 9 && vlen + 3 <= sizeof(that->awdlChanTlv)) {
            memcpy(that->awdlChanTlv, v - 3, vlen + 3);
            that->awdlChanTlvLen = (uint16_t)(vlen + 3);
        }
        if (that->awdlRxTlvLogged < 3 || tlvChanged) {
            that->awdlRxTlvLogged++;
            awdlTlvTypes(r->frame + 40, r->len - 40, types, sizeof(types));
            qwz_log("airport: awdl0: rx subtype %u from %02x:%02x:%02x:%02x:%02x:%02x, TLVs: %s\n", r->frame[30],
                    r->frame[10], r->frame[11], r->frame[12], r->frame[13], r->frame[14], r->frame[15], types);
        }
    }

    if (++that->awdlRxDelivered <= 8 || (that->awdlRxDelivered & 255) == 0) {
        qwz_log("airport: awdl0: rx #%u to the AWDL engine: from %02x:%02x:%02x:%02x:%02x:%02x, "
                "subtype %u, %zu bytes, ch %u, rssi %d\n", that->awdlRxDelivered,
                r->frame[10], r->frame[11], r->frame[12], r->frame[13], r->frame[14], r->frame[15],
                r->len > 30 ? r->frame[30] : 0xff, body, r->chan, r->rssi);
        qwz_log_publish();
    }
    return kIOReturnSuccess;
}

int AirportItlwm::awdlSysctl(void *arg, int value)
{
    AirportItlwm *self = (AirportItlwm *)arg;
    IO80211LinkState state;
    IOReturn ret;

    if (value == 1)
        return self->awdlInterfaceTest ? EBUSY : self->awdlCreate();	/* F1: the native awdl0 owns the role while -qwzawdlnative is set */
    if (value == 20 || value == 22 || value == 26) {
        /*
         * M2a WP2: a setPEER_CACHE_CONTROL op (add 20, delete 22, update 26: the engine's op + 20) for a fixed test address, through awdlPeerCb as the
         * AWDL kext's hook calls it, so debug.qwz.awdlpeers bit 2 can be shown without waiting for the engine. ENXIO until awdl=1 made awdl0;
         * ENOTCONN shows in the next line when no listen runs. The test address is not a real station: one of the 8 firmware peers until the listen ends.
         */
        static const uint8_t testMac[6] = { 0x02, 0x00, 0x57, 0x32, 0x00, 0x01 };
        static unsigned int tested;
        int op = value - 20;
        int ret = awdlPeerCb(self, testMac, QWZ_AWDL_PEER_OP + op);

        if (qwz_log_ratelimit(&tested, 32, 16)) {
            qwz_log("airport: awdl0: test peer cache op %d for %02x:%02x:%02x:%02x:%02x:%02x -> %d\n", op,
                    testMac[0], testMac[1], testMac[2], testMac[3], testMac[4], testMac[5], ret);
            qwz_log_publish();
        }
        return ret;
    }
    if (value != 2 && value != 3)
        return EINVAL;
    /* 2: awdl0's link up, 3: down. AWDL frames reach the engine while it is up. */
    state = value == 2 ? kIO80211NetworkLinkUp : kIO80211NetworkLinkDown;
    if (state == kIO80211NetworkLinkDown) {
        static_cast<ItlQwz *>(self->fHalService)->setAwdlRx(NULL, NULL);
        static_cast<ItlQwz *>(self->fHalService)->setAwdlDataRx(NULL, NULL);
        static_cast<ItlQwz *>(self->fHalService)->setAwdlTxDone(NULL, NULL);	/* WP4 (M2a) */
    }
    ret = self->getCommandGate()->runAction(awdlLinkGated, (void *)(uint64_t)state);
    if (ret == kIOReturnSuccess && state == kIO80211NetworkLinkUp) {
        static_cast<ItlQwz *>(self->fHalService)->setAwdlRx(awdlRx, self);
        static_cast<ItlQwz *>(self->fHalService)->setAwdlDataRx(awdlDataRx, self);
        static_cast<ItlQwz *>(self->fHalService)->setAwdlTxDone(awdlTxDone, self);	/* WP4 (M2a): the pool's completions, while awdl0's link is up */
    }
    qwz_log("airport: awdl0: link %s -> 0x%x%s\n", value == 2 ? "up" : "down", ret,
            ret == kIOReturnSuccess && value == 2 ?
            "; AWDL frames go to the engine (debug.qwz.awdltest=6 makes the firmware pass them)" : "");
    qwz_log_publish();
    return ret == kIOReturnSuccess ? 0 : ENXIO;
}

/*
 * M0 (C34): debug.qwz.state, debug.qwz.recover=2 and debug.qwz.health=1 (qwz_sysctl.cpp calls this through
 * qwz_sysctl_set_m0, on the sysctl thread, without the gate). A non-zero return reaches the reader as the errno.
 * STATE gets a zeroed 160-byte buffer; RECOVER and HEALTH get NULL and 0: they must not touch buf.
 */
int AirportItlwm::m0Sysctl(void *arg, int what, int val, char *buf, size_t len)
{
    AirportItlwm *self = (AirportItlwm *)arg;
    uint32_t reason = 0;

    if (self == NULL || self->fHalService == NULL || self->pmStopping)
        return ENXIO;
    switch (what) {
    case QWZ_M0_STATE:
        if (buf == NULL || len == 0)
            return EINVAL;
        snprintf(buf, len, "%s dead=%d episodes=%u given_up=%d fails=%u proven=%d recover=%d silent=%d m0=%d",
                 self->engGivenUp ? "GIVEN_UP" : self->engineBusy ? "RESTARTING" :
                 (self->engFailCont != 0 && self->fHalService->wakeNeedsRecovery()) ? "RETRY" : "UP", self->fHalService->faultPending(NULL),
                 self->engEpisodes, self->engGivenUp, self->engFailCont, qwz_recovery_proven, qwz_recover, qwz_silent, qwz_m0_on);
        return 0;
    case QWZ_M0_RECOVER:
        self->engineRearm("sysctl");
        if (!self->fHalService->faultPending(&reason) && self->fHalService->wakeNeedsRecovery())
            self->engineRequest(kEngRecover, QWZ_DEAD_USER);
        else if (self->fHalService->faultPending(&reason))
            self->engineRequest(kEngDead, reason);
        return 0;
    case QWZ_M0_HEALTH:
        self->fHalService->healthSnapshot(true);
        return 0;
    }
    return EINVAL;
}

IO80211VirtualInterface *
AirportItlwm::createVirtualInterface(ether_addr *ether, UInt role)
{
    qwz_log("airport: AWDL createVirtualInterface role=%u\n", role);
    qwz_log_publish();
    /* F1 (native AWDL): with -qwzawdlnative the AWDL role gets our QwzAWDLInterface (QwzAWDLController.cpp). */
    if (IO80211VirtualInterface *native = awdlNativeCreateVirtualInterface(ether, role))
        return native;

    /* Let the superclass handle it — on Tahoe, IO80211Controller's
     * attachVirtualInterface does the allocation internally when
     * createVirtualInterface returns non-NULL. We just need to not
     * return NULL for AWDL role. */
    return super::createVirtualInterface(ether, role);
}
