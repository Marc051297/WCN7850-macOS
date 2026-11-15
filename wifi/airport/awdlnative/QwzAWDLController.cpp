//
//  QwzAWDLController.cpp
//  AirportWCN7850
//
//  WCN7850: the AirportItlwm-side half of ForbiddenItlwm's native AWDL port (ForbiddenItlwm/AirportItlwmV2.cpp,
//  commit 59ff7bf, GPL-2.0): interface creation, the hand-offs between the radio and Apple's peer manager, the sync
//  frame builder, awdl0's transmit path and teardown, plus the itlAWDL* boundary functions the radio code calls.
//  Renamed QwzAWDLInterface; createVirtualInterface became awdlNativeCreateVirtualInterface.
//
#include "AirportItlwmV2.hpp"
#if defined(ITL_AWDL_NATIVE)
#include "QwzAWDLInterface.hpp"
extern IO80211WorkQueue *_fWorkloop;	/* AirportItlwmV2.cpp: the controller's IO80211 work queue (the gate the HAL and the hooks share) */
#include <Airport/IO80211ManagementBuffer.h>
#include <AWDLObservation.hpp>
#include <AWDLManagementFrame.hpp>
#include <AWDLLocalSchedule.hpp>
#include <AWDLPeerProbe.hpp>
#include <AWDLDataPayload.hpp>
#include <AWDLDataFrame.hpp>
#include <Airport/IOSkywalkPacketView.h>
#include <net80211/ieee80211_var.h>

extern unsigned itlAWDLFixedChannel();
extern unsigned itlAWDLChannelClass();
extern bool itlAWDLBuildAllowed();
extern bool itlAWDLBuildVerified();
extern const char *itlAWDLBuildStatus();
extern bool itlAWDLTwoStreams();	/* QwzAWDLInterface.cpp */

/* WCN7850: called first by AirportItlwm::createVirtualInterface (AirportItlwmV2.cpp); NULL means "not ours, ask the superclass". */
IO80211VirtualInterface *AirportItlwm::awdlNativeCreateVirtualInterface(ether_addr *address, UInt role)
{
#if defined(ITL_AWDL_NATIVE)
    if (awdlInterfaceTest && role == APPLE80211_VIF_AWDL) {
        setProperty("ItlwmAWDLFactoryStage", "entered");
        if (!address)
            return nullptr;
        QwzAWDLInterface *interface = new QwzAWDLInterface;
        if (!interface) {
            setProperty("ItlwmAWDLFactoryStage", "allocation-failed");
            return nullptr;
        }
        if (!interface->init(this, address, role, "awdl")) {
            setProperty("ItlwmAWDLFactoryStage", "initialization-failed");
            interface->release();
            return nullptr;
        }
        int nativeUp = 0;
        if (PE_parse_boot_argn("-itlawdlnativeup", &nativeUp, sizeof(nativeUp)))
            interface->acceptNativeRequests();
        interface->setProperty("ItlwmAWDLInterfaceBuild", QWZ_AWDL_NATIVE_BUILD);
        setProperty("ItlwmAWDLFactoryStage", "initialized");
        return interface;
    }
#endif
    return nullptr;
}

void AirportItlwm::observeAWDLDataFrame(const uint8_t *frame, size_t length)
{
#if defined(ITL_AWDL_NATIVE)
    if (!awdlInterfaceTest || !_fWorkloop || !_fWorkloop->inGate() ||
        !awdlManagementInputEnabled || !fAWDLInterface) return;
    auto *interface = static_cast<QwzAWDLInterface *>(fAWDLInterface);
    interface->retain(); ++awdlManagementActive;
    interface->observeDataFrame(frame, length);
    interface->release();
    if (!--awdlManagementActive) _fWorkloop->commandWakeup(&awdlManagementActive);
#endif
}

#if defined(ITL_AWDL_NATIVE)
void itlAWDLObserveData(IOEthernetController *owner, const uint8_t *frame, size_t length)
{
    if (auto *controller = OSDynamicCast(AirportItlwm, owner)) controller->observeAWDLDataFrame(frame, length);
}
#endif

IOReturn AirportItlwm::receiveAWDLActionFrame(mbuf_t packet, size_t frameLength, uint32_t channel, int32_t rssi)
{
#if defined(ITL_AWDL_NATIVE)
    // HAL and native control share _fWorkloop. Only deliver inside its gate.
    // Stop disables new handoffs and drains this counter before native teardown.
    // This monitor-specific entry takes a normalized span excluding four FCS
    // bytes; the borrowed Intel packet itself retains those bytes.
    if (!awdlInterfaceTest || !_fWorkloop || !_fWorkloop->inGate() ||
        !awdlManagementInputEnabled || !fAWDLInterface || !packet || mbuf_next(packet) ||
        frameLength > mbuf_len(packet) || mbuf_len(packet)-frameLength != 4)
        return kIOReturnNotReady;
    auto *interface = static_cast<QwzAWDLInterface *>(fAWDLInterface);
    interface->retain();
    ++awdlManagementActive;
    const auto result = interface->receiveManagementFrame(this,
        static_cast<const uint8_t *>(mbuf_data(packet)), frameLength, channel, rssi);
    if (result != kIOReturnSuccess)
        setProperty("ItlwmAWDLManagementLastError", static_cast<uint32_t>(result), 32);
    interface->release();
    if (!--awdlManagementActive)
        _fWorkloop->commandWakeup(&awdlManagementActive);
    return result;
#else
    return kIOReturnUnsupported;
#endif
}

#if defined(ITL_AWDL_NATIVE)
IOReturn itlAWDLReceiveAction(IOEthernetController *owner, mbuf_t packet, size_t frameLength, uint32_t channel, int32_t rssi)
{
    auto *controller = OSDynamicCast(AirportItlwm, owner);
    return controller ? controller->receiveAWDLActionFrame(packet, frameLength, channel, rssi) : kIOReturnNotReady;
}
#endif

// Which sync frames were built: with macOS's MI (services, host name), PSF, or an ordinary frame in an MI
// slot because no MI was available or it failed. Counts only; no frame content is published.

IOReturn AirportItlwm::buildAWDLSyncProbe(mbuf_t *packet, uint64_t nowUS)
{
#if defined(ITL_AWDL_NATIVE)
    static uint32_t awdlSyncMIBuilt, awdlSyncPSFBuilt, awdlSyncFallbackBuilt, awdlSyncMIWanted, awdlSyncMIMissing,
        awdlSyncPeerTLVsRefused, awdlSyncFailed, awdlSyncInfraStripped;
    if (!packet || *packet) return kIOReturnBadArgument;
    if (!awdlInterfaceTest || !_fWorkloop || !_fWorkloop->inGate() ||
        !awdlManagementInputEnabled || !fAWDLInterface) {
        setProperty("ItlwmAWDLSyncProbeStage", !awdlInterfaceTest ? "test-disabled" :
            !_fWorkloop ? "no-workloop" : !_fWorkloop->inGate() ? "outside-gate" :
            !awdlManagementInputEnabled ? "controller-not-ready" : "no-native-interface");
        return kIOReturnNotReady;
    }
    auto *snapshot = IONew(QwzAWDLInterface::FrameConfiguration, 1);
    if (!snapshot) {
        setProperty("ItlwmAWDLSyncProbeStage", "no-memory");
        return kIOReturnNoMemory;
    }
    auto *interface = static_cast<QwzAWDLInterface *>(fAWDLInterface);
    interface->retain();
    const auto status = interface->copyFrameConfiguration(snapshot);
    // Alternate the peer manager's service-carrying MI with sync-only PSF frames.
    // On any problem with the MI, this stays an ordinary frame.
    const bool wantMI = awdlProbeSequence & 1;
    size_t miLength = 0;
    const bool isMI = status == kIOReturnSuccess && wantMI &&
        interface->copyMasterIndication(snapshot->staticTLVs, sizeof(snapshot->staticTLVs), &miLength);
    const bool isPSF = !isMI && !wantMI && interface->hasMasterIndication();
    if (isMI)
        snapshot->templateLength = miLength;
    if (wantMI) {
        ++awdlSyncMIWanted;
        if (!isMI)
            ++awdlSyncMIMissing;
    }
    interface->release();
    if (status != kIOReturnSuccess) {
        setProperty("ItlwmAWDLSyncProbeStage", "snapshot-not-ready");
        IODelete(snapshot, QwzAWDLInterface::FrameConfiguration, 1);
        return status;
    }
    setProperty("ItlwmAWDLSyncProbeTemplateLength", snapshot->templateLength, 32);
    setProperty("ItlwmAWDLSyncProbeStage", "snapshot-copied");
    setProperty("ItlwmAWDLRequestedTimingPresent", snapshot->hasRequestedTiming);
    setProperty("ItlwmAWDLRequestedAWLength", snapshot->parameters.availability_window_length, 32);
    setProperty("ItlwmAWDLRequestedAWPeriod", snapshot->parameters.availability_window_period, 32);
    // Reject contradictory native channel-sequence TLVs rather than claiming
    // the native requested sequence was applied to this fixed-channel test.
    bool valid = true;
    for (size_t off = 0; off < snapshot->templateLength;) {
        if (snapshot->templateLength-off < 3) { valid = false; break; }
        const size_t count = ItlAWDL::read16(snapshot->staticTLVs+off+1);
        if (count > snapshot->templateLength-off-3 || (!isMI && snapshot->staticTLVs[off] == 18)) {
            valid = false; break;
        }
        off += 3+count;
    }
    // -itlawdlhideinfra: macOS's Data Path State TLV names the Mac's Wi-Fi network and channel. With the
    // Mac's Wi-Fi on the listener's channel, the iPhone never answered our Block Ack request (#24); this
    // test leaves that information out, in both the template and the master indication.
    if (valid && awdlHideInfra && ItlAWDL::stripDataPathInfra(snapshot->staticTLVs, &snapshot->templateLength))
        ++awdlSyncInfraStripped;
    // After the check above, which still refuses a native channel-sequence TLV.
    if (valid && !isMI && awdlExtendFrames &&
        !ItlAWDL::extendWithPeerTLVs(snapshot->staticTLVs, &snapshot->templateLength,
                                     sizeof(snapshot->staticTLVs), snapshot->address, uint8_t(itlAWDLFixedChannel()),
                                     uint8_t(itlAWDLChannelClass()), itlAWDLTwoStreams()))
    {
        setProperty("ItlwmAWDLPeerTLVs", "not-added");
        ++awdlSyncPeerTLVsRefused;
    }
    uint8_t sync[73]; size_t syncLength = 0;
    const uint8_t fixedChannel = uint8_t(itlAWDLFixedChannel());
    ItlAWDL::SyncPrefix settings = {fixedChannel,fixedChannel,0,0,16,110,0x1000,16,16,0,3,3,3,3,{}};
    memcpy(settings.master, snapshot->address, 6);
    const auto schedule = ItlAWDL::buildLocalFixedSchedule(settings, 0, nowUS, 0,
        sync, sizeof(sync), &syncLength);
    mbuf_t result = nullptr;
    // Each way a frame can fail has its own stage and return code, so one snapshot shows which.
    IOReturn error = kIOReturnBadArgument;
    const char *stage = "frame-rejected";
    if (!valid)
        stage = "template-invalid";
    else if (schedule != ItlAWDL::BuildResult::Ready) {
        stage = "schedule-rejected";
        error = kIOReturnUnsupported;
    }
    if (valid && schedule == ItlAWDL::BuildResult::Ready) {
        // A fresh getcluster allocation uses a plain mbuf without M_PKTHDR.
        // The packet allocator supplies both the header and 2 KB cluster.
        if (mbuf_getpacket(MBUF_DONTWAIT, &result) || !result) {
            error = kIOReturnNoMemory;
            stage = "no-memory";
        } else {
            error = kIOReturnError;
            ItlAWDL::LocalFrame frame = {{},{255,255,255,255,255,255},uint8_t(isPSF ? 0 : 3),uint16_t(awdlProbeSequence & 4095),
                uint32_t(nowUS),uint32_t(nowUS),sync,syncLength,
                snapshot->staticTLVs,snapshot->templateLength};
            memcpy(frame.source, snapshot->address, 6);
            size_t length = 0;
            if (ItlAWDL::buildLocalFrame(frame, static_cast<uint8_t *>(mbuf_data(result)),
                    mbuf_maxlen(result), &length) == ItlAWDL::BuildResult::Ready) {
                mbuf_setlen(result,length); mbuf_pkthdr_setlen(result,length);
                *packet = result; error = kIOReturnSuccess;
                awdlProbeSequence = uint16_t((awdlProbeSequence + 1) & 4095);
                setProperty("ItlwmAWDLSyncProbeFrameLength", length, 32);
            } else mbuf_freem(result);
        }
    }
    IODelete(snapshot, QwzAWDLInterface::FrameConfiguration, 1);
    // A failed frame still moves the sequence on, so a failing MI doesn't hold the alternation on itself.
    if (error != kIOReturnSuccess)
        awdlProbeSequence = uint16_t((awdlProbeSequence + 1) & 4095);
    setProperty("ItlwmAWDLSyncProbeStage", error == kIOReturnSuccess ? "frame-built" : stage);
    if (error == kIOReturnSuccess)
        ++(isMI ? awdlSyncMIBuilt : isPSF ? awdlSyncPSFBuilt : awdlSyncFallbackBuilt);
    else
        ++awdlSyncFailed;
    const uint32_t syncTotal = awdlSyncMIBuilt + awdlSyncPSFBuilt + awdlSyncFallbackBuilt + awdlSyncFailed;
    if (syncTotal <= 4 || !(syncTotal % 16)) {
        setProperty("ItlwmAWDLSyncMIBuilt", awdlSyncMIBuilt, 32);
        setProperty("ItlwmAWDLSyncPSFBuilt", awdlSyncPSFBuilt, 32);
        setProperty("ItlwmAWDLSyncFallbackBuilt", awdlSyncFallbackBuilt, 32);
        setProperty("ItlwmAWDLSyncMIWanted", awdlSyncMIWanted, 32);
        setProperty("ItlwmAWDLSyncMIMissing", awdlSyncMIMissing, 32);
        setProperty("ItlwmAWDLSyncPeerTLVsRefused", awdlSyncPeerTLVsRefused, 32);
        setProperty("ItlwmAWDLSyncFailed", awdlSyncFailed, 32);
        if (awdlHideInfra)
            setProperty("ItlwmAWDLSyncInfraStripped", awdlSyncInfraStripped, 32);
    }
    return error;
#else
    return kIOReturnUnsupported;
#endif
}

#if defined(ITL_AWDL_NATIVE)
// Under the shared gate. The caller's sender consumes the packet.
IOReturn itlAWDLBuildTransmit(IOEthernetController *owner, mbuf_t *packet)
{
    auto *controller = OSDynamicCast(AirportItlwm, owner);
    return controller ? controller->buildAWDLTransmit(packet) : kIOReturnNotReady;
}

IOReturn itlAWDLBuildTransmit(IOEthernetController *owner, mbuf_t *packet, const uint8_t *holdPayloadFor)
{
    auto *controller = OSDynamicCast(AirportItlwm, owner);
    return controller ? controller->buildAWDLTransmit(packet, holdPayloadFor) : kIOReturnNotReady;
}

IOReturn itlAWDLBuildTransmit(IOEthernetController *owner, mbuf_t *packet, const uint8_t *holdPayloadFor,
                              bool holdEverySize)
{
    auto *controller = OSDynamicCast(AirportItlwm, owner);
    return controller ? controller->buildAWDLTransmit(packet, holdPayloadFor, holdEverySize) : kIOReturnNotReady;
}

unsigned itlAWDLTransmitHeldShort(IOEthernetController *owner)
{
    auto *controller = OSDynamicCast(AirportItlwm, owner);
    return controller ? controller->heldShortAWDLTransmit() : 0;
}

unsigned itlAWDLTransmitQueued(IOEthernetController *owner)
{
    auto *controller = OSDynamicCast(AirportItlwm, owner);
    return controller ? controller->queuedAWDLTransmit() : 0;
}

bool itlAWDLServiceTransmit(IOEthernetController *owner)
{
    auto *controller = OSDynamicCast(AirportItlwm, owner);
    return controller && controller->serviceAWDLTransmit();
}

IOReturn AirportItlwm::buildAWDLTransmit(mbuf_t *packet, const uint8_t *holdPayloadFor, bool holdEverySize)
{
    if (!packet || *packet || !_fWorkloop || !_fWorkloop->inGate() || !awdlManagementInputEnabled || !fAWDLInterface)
        return kIOReturnNotReady;
    mbuf_t result = nullptr;
    if (mbuf_getpacket(MBUF_DONTWAIT, &result) || !result)
        return kIOReturnNoMemory;
    size_t length = 0;
    auto *interface = static_cast<QwzAWDLInterface *>(fAWDLInterface);
    interface->retain();
    const bool ready = interface->popTransmit(static_cast<uint8_t *>(mbuf_data(result)), mbuf_maxlen(result), &length, holdPayloadFor,
                                                 holdEverySize);
    interface->release();
    if (!ready) {
        mbuf_freem(result);
        return kIOReturnNotFound;
    }
    mbuf_setlen(result, length);
    mbuf_pkthdr_setlen(result, length);
    *packet = result;
    return kIOReturnSuccess;
}

bool AirportItlwm::copyAWDLTransmitDestination(uint8_t *destination)
{
    if (!_fWorkloop || !_fWorkloop->inGate() || !awdlManagementInputEnabled || !fAWDLInterface)
        return false;
    return static_cast<QwzAWDLInterface *>(fAWDLInterface)->copyTransmitDestination(destination);
}

// How many frames macOS has queued on awdl0 that the driver hasn't taken yet, for diagnostics only.
unsigned AirportItlwm::queuedAWDLTransmit()
{
    if (!_fWorkloop || !_fWorkloop->inGate() || !awdlManagementInputEnabled || !fAWDLInterface)
        return 0;
    return static_cast<QwzAWDLInterface *>(fAWDLInterface)->queuedTransmit();
}

unsigned AirportItlwm::heldShortAWDLTransmit()
{
    if (!_fWorkloop || !_fWorkloop->inGate() || !awdlManagementInputEnabled || !fAWDLInterface)
        return 0;
    return static_cast<QwzAWDLInterface *>(fAWDLInterface)->heldShortTransmit();
}

bool AirportItlwm::serviceAWDLTransmit()
{
    if (!_fWorkloop || !_fWorkloop->inGate() || !awdlManagementInputEnabled || !fAWDLInterface)
        return false;
    auto *interface = static_cast<QwzAWDLInterface *>(fAWDLInterface);
    interface->retain();
    const bool pending = interface->serviceTransmit();
    interface->release();
    return pending;
}

IOReturn itlAWDLBuildSyncProbe(IOEthernetController *owner, mbuf_t *packet, uint64_t nowUS)
{
    auto *controller = OSDynamicCast(AirportItlwm, owner);
    return controller ? controller->buildAWDLSyncProbe(packet, nowUS) : kIOReturnNotReady;
}
#endif

void AirportItlwm::teardownAWDLInterface()
{
    awdlNativeRadioStop();	/* F2-1: no more hand-offs or sends before the interface goes */
    if (fAWDLInterface) {
        // terminate() needs the live native controller context and invokes our
        // stop() while dismantling the published service. Do not stop it first.
        detachVirtualInterface(fAWDLInterface, true);
#if defined(ITL_AWDL_NATIVE)
        fAWDLInterface->release();
#endif
        fAWDLInterface = nullptr;
    }
}
#endif /* ITL_AWDL_NATIVE */
