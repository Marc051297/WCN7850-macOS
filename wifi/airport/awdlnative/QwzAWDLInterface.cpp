//
//  QwzAWDLInterface.cpp
//  AirportWCN7850
//
//  WCN7850: the methods of ForbiddenItlwm's ItlAWDLInterface (ForbiddenItlwm/AirportItlwmV2.cpp, commit 59ff7bf,
//  GPL-2.0), renamed QwzAWDLInterface, otherwise unchanged. See airport/awdlnative/NOTICE.
//
#include "AirportItlwmV2.hpp"
#if defined(ITL_AWDL_NATIVE)
#include "QwzAWDLInterface.hpp"
#include <Airport/IO80211ManagementBuffer.h>
#include <AWDLObservation.hpp>
#include <AWDLManagementFrame.hpp>
#include <AWDLLocalSchedule.hpp>
#include <AWDLPeerProbe.hpp>
#include <AWDLDataPayload.hpp>
#include <AWDLDataFrame.hpp>
#include <Airport/IOSkywalkPacketView.h>
#include <libkern/OSDebug.h>
#include <mach/vm_param.h>
#include <sys/proc.h>

// -itlawdlsamples: raw samples of native requests, templates and packets in the registry. They can hold
// addresses and service records, and any local user can read the registry, so they are off by default.
bool qwzAwdlRawSamples;

OSDefineMetaClassAndStructors(QwzAWDLInterface, IO80211AWDLProtocol)

IOReturn QwzAWDLInterface::getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *data)
{
    return nativeState.getEnabled(data);
}

IOReturn QwzAWDLInterface::getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap *data)
{
    if (!nativeState.accept)
        return unsupported("getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES");
    if (!data)
        return kIOReturnBadArgument;
    // No secondary payload capability is claimed.
    bzero(data, sizeof(*data));
    data->version = APPLE80211_VERSION;
    return kIOReturnSuccess;
}

IOReturn QwzAWDLInterface::setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params *data)
{
    if (!nativeState.accept)
        return unsupported("setAWDL_ELECTION_PARAMS");
    // Acknowledged only: there is no firmware election, and the contents aren't read.
    return data ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn QwzAWDLInterface::setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration *data)
{
    if (!nativeState.accept)
        return unsupported("setAWDL_PEER_TRAFFIC_REGISTRATION");
    // Acknowledged only. Its embedded pointer is neither read nor kept.
    return data ? kIOReturnSuccess : kIOReturnBadArgument;
}

extern bool itlAWDLBuildVerified();

void QwzAWDLInterface::acceptNativeRequests()
{
    nativeState.accept = true;
    setProperty("ItlwmAWDLNativeUp", true);
    int debug = 0;
    debugFlagsWanted = PE_parse_boot_argn("-itlawdlpmdebug", &debug, sizeof(debug));
    // The request carries a raw pointer at a fixed offset, a layout checked only for the verified
    // builds. On any other build (expected, or allowed by -itlawdlanybuild) the copy stays off.
    const bool wanted = PE_parse_boot_argn("-itlawdlmitx", &debug, sizeof(debug));
    masterIndicationEnabled = wanted && itlAWDLBuildVerified();
    setProperty("ItlwmAWDLMiState", !wanted ? "off" : masterIndicationEnabled ? "on" : "blocked: build not verified");
}

IOReturn QwzAWDLInterface::getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode *data)
{
    if (!nativeState.accept)
        return unsupported("getAWDL_AF_TX_MODE");
    if (!data)
        return kIOReturnBadArgument;
    // The local struct's 64-bit mode doesn't match the native record, which holds
    // a 32-bit mode after the version. Touch only those two fields.
    data->version = APPLE80211_VERSION;
    memcpy(reinterpret_cast<uint8_t *>(data) + 4, &afTxModeValue, sizeof(afTxModeValue));
    return kIOReturnSuccess;
}

IOReturn QwzAWDLInterface::setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode *data)
{
    if (!nativeState.accept)
        return unsupported("setAWDL_AF_TX_MODE");
    if (!data)
        return kIOReturnBadArgument;
    memcpy(&afTxModeValue, reinterpret_cast<const uint8_t *>(data) + 4, sizeof(afTxModeValue));
    setProperty("ItlwmAWDLAfTxMode", afTxModeValue, 32);
    return kIOReturnSuccess;
}

// Reports success, but nothing is sent yet: the request's format is unknown, so
// this build only records it. The sample is the fixed header before offset 0x28 only: on Tahoe and
// Sequoia the length sits at 0x28 and a raw kernel pointer at 0x30, and neither may be published.
IOReturn QwzAWDLInterface::noteOobRequest(const apple80211_awdl_oob_request *data, const char *name)
{
    if (!nativeState.accept)
        return unsupported(name);
    if (!data)
        return kIOReturnBadArgument;
    ++oobRequests;
    if (oobRequests == 1 || !(oobRequests % 16))
        setProperty("ItlwmAWDLOobRequests", oobRequests, 32);
    if (!oobSampled && qwzAwdlRawSamples) {
        oobSampled = true;
        uint16_t length;
        memcpy(&length, reinterpret_cast<const uint8_t *>(data) + 0x28, sizeof(length));
        if (auto *sample = OSData::withBytes(data, 0x28)) {
            setProperty("ItlwmAWDLOobSample", sample);
            sample->release();
        }
        setProperty("ItlwmAWDLOobFirstName", name);
        setProperty("ItlwmAWDLOobFirstLength", length, 32);
    }
    return kIOReturnSuccess;
}

IOReturn QwzAWDLInterface::setAWDL_OOB_REQUEST(apple80211_awdl_oob_request *data)
{
    return noteOobRequest(data, "setAWDL_OOB_REQUEST");
}

IOReturn QwzAWDLInterface::setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request *data)
{
    const IOReturn result = noteOobRequest(data, "setAWDL_OOB_AUTO_REQUEST");
    if (result == kIOReturnSuccess && masterIndicationEnabled)
        storeMasterIndication(data);
    return result;
}

void QwzAWDLInterface::storeMasterIndication(const apple80211_awdl_oob_request *data)
{
    const auto *raw = reinterpret_cast<const uint8_t *>(data);
    uint16_t length;
    const uint8_t *source;
    memcpy(&length, raw + 0x28, sizeof(length));
    memcpy(&source, raw + 0x30, sizeof(source));
    // Only ever a kernel buffer from the peer manager, and never over its capacity.
    if (reinterpret_cast<uintptr_t>(source) < 0xffffff8000000000ULL || !length || length > sizeof(masterIndication) ||
        !ItlAWDL::masterIndicationWellFormed(source, length)) {
        ++masterIndicationRejected;
        setProperty("ItlwmAWDLMiRejected", masterIndicationRejected, 32);
        return;
    }
    memcpy(masterIndication, source, length);
    masterIndicationLength = length;
    ++masterIndicationUpdates;
    if (masterIndicationUpdates == 1 || !(masterIndicationUpdates % 64))
        setProperty("ItlwmAWDLMiUpdates", masterIndicationUpdates, 32);
    setProperty("ItlwmAWDLMiLength", length, 32);
    // Keep the current template, since its first version is small and has no services.
    if (qwzAwdlRawSamples && masterIndicationSampledLength != length) {
        masterIndicationSampledLength = length;
        if (auto *sample = OSData::withBytes(source, length)) {
            setProperty("ItlwmAWDLMiSample", sample);
            sample->release();
        }
    }
}

extern unsigned itlAWDLFixedChannel();
extern unsigned itlAWDLChannelClass();
extern bool itlAWDLBuildAllowed();
extern const char *itlAWDLBuildStatus();
// With -itlawdlcaps the driver reports two HT streams and VHT to macOS; our TLV 7 says the same.
bool itlAWDLTwoStreams()
{
    static int cached = -1;
    if (cached < 0) {
        int dummy = 0;
        cached = itlAWDLBuildAllowed() && PE_parse_boot_argn("-itlawdlcaps", &dummy, sizeof(dummy)) &&
            !PE_parse_boot_argn("-itlawdlcapshtonly", &dummy, sizeof(dummy)) ? 1 : 0;
    }
    return cached == 1;
}

bool QwzAWDLInterface::copyMasterIndication(uint8_t *output, size_t capacity, size_t *written)
{
    if (!masterIndicationLength || !output || !written || !capacity)
        return false;
    // Built in scratch and copied only on success: a failure partway through must leave the caller's
    // buffer, which holds the ordinary frame's template, exactly as it was.
    auto *scratch = static_cast<uint8_t *>(IOMalloc(capacity));
    if (!scratch)
        return false;
    size_t length = 0;
    const bool built = ItlAWDL::buildMasterIndicationTLVs(masterIndication, masterIndicationLength, localFrameAddress,
                                                          scratch, capacity, &length, uint8_t(itlAWDLFixedChannel()),
                                                          uint8_t(itlAWDLChannelClass()), itlAWDLTwoStreams());
    if (built && length <= capacity) {
        memcpy(output, scratch, length);
        *written = length;
    } else {
        setProperty("ItlwmAWDLMiBuildFailed", true);
    }
    IOFree(scratch, capacity);
    return built && length <= capacity;
}

IOReturn QwzAWDLInterface::setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *data)
{
    const IOReturn result = nativeState.setEnabled(data);
    if (result == kIOReturnUnsupported) {
        setProperty("ItlwmAWDLActivationBlocked", "radio-transport-unimplemented");
        return unsupported("setAWDL_SYNC_ENABLED");
    }
    if (result == kIOReturnSuccess && nativeState.accept) {
        setProperty("ItlwmAWDLSyncEnabledRequested", data->enabled, 32);
        // The peer manager exists by now. Reject reasons are behind these flags.
        if (data->enabled && debugFlagsWanted && !debugFlagsSet) {
            debugFlagsSet = true;
            setDebugFlags(~0ULL, 0);
            setProperty("ItlwmAWDLDebugFlagsSet", true);
        }
    }
    return result;
}

IOReturn QwzAWDLInterface::setAWDL_SYNC_STATE(apple80211_awdl_sync_state *data)
{
    const IOReturn result = nativeState.setState(data);
    if (result == kIOReturnUnsupported) {
        setProperty("ItlwmAWDLActivationBlocked", "radio-transport-unimplemented");
        return unsupported("setAWDL_SYNC_STATE");
    }
    if (result == kIOReturnSuccess && nativeState.accept)
        setProperty("ItlwmAWDLSyncStateRequested", data->state, 32);
    return result;
}

IOReturn QwzAWDLInterface::setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template *data)
{
    if (!data || data->version > APPLE80211_VERSION)
        return kIOReturnBadArgument;
    if (!configurationLock)
        return kIOReturnNotReady;
    // This native protocol callback receives a kernel-owned payload, already
    // marshalled by IO80211Family. Retain a bounded copy, never its pointer.
    IOLockLock(configurationLock);
    const IOReturn result = frameTemplate.set(data->payload, data->payload_len);
    IOLockUnlock(configurationLock);
    if (result == kIOReturnSuccess) {
        setProperty("ItlwmAWDLTemplateStored", data->payload_len != 0);
        setProperty("ItlwmAWDLLastStoredRequest", "AWDL_SYNC_FRAME_TEMPLATE");
    }
    return result;
}

IOReturn QwzAWDLInterface::copyFrameConfiguration(FrameConfiguration *output)
{
    if (!output) return kIOReturnBadArgument;
    if (!configurationLock || !nativeStarted) return kIOReturnNotReady;
    IOLockLock(configurationLock);
    apple80211_awdl_sync_params parameters = {};
    const IOReturn status = requested_AWDL_SYNC_PARAMS.get(&parameters);
    const size_t length = frameTemplate.size();
    if ((status != kIOReturnSuccess && status != kIOReturnNotReady) || !length) {
        IOLockUnlock(configurationLock);
        return kIOReturnNotReady;
    }
    // Both template replacement and requested settings use this same lock.
    // No borrowed native template pointer crosses the interface boundary.
    const IOReturn result = frameTemplate.copy(output->staticTLVs, sizeof(output->staticTLVs));
    if (result == kIOReturnSuccess) {
        output->parameters = parameters;
        output->hasRequestedTiming = status == kIOReturnSuccess;
        memcpy(output->address, localFrameAddress, sizeof(localFrameAddress));
        output->templateLength = length;
    }
    IOLockUnlock(configurationLock);
    return result;
}

void QwzAWDLInterface::observeDataFrame(const uint8_t *frame, size_t length)
{
    if ((!dataDeliverEnabled && (!dataCopyProbeEnabled || dataCopyAttempts >= 8)) || !nativeStarted || !rxPool ||
        !queueWorkLoop || !queueWorkLoop->inGate()) return;
    ItlAWDL::DataFrameView view;
    if (ItlAWDL::parseDataFrame(frame, length, true, &view) != ItlAWDL::ParseResult::Valid) return;
    // Copy only multicast or our local AWDL destination; drop self echoes.
    if ((!(*view.destination & 1) && memcmp(view.destination, localFrameAddress, 6)) ||
        !memcmp(view.source, localFrameAddress, 6)) return;
    ++dataCopyAttempts;
    auto *pool = reinterpret_cast<ItlSkywalkPoolView *>(rxPool);
    ItlSkywalkPacketView *packet = nullptr;
    const IOReturn allocated = pool->allocatePacket(1, &packet, 1);
    bool copied = false;
    IOReturn prepared = kIOReturnNotReady, completed = kIOReturnNotReady;
    if (allocated == kIOReturnSuccess && packet) {
        prepared = packet->prepareWithQueue(packetQueues[2], 0, 0);
        const size_t wanted = 14 + view.ip.length;
        if (prepared == kIOReturnSuccess && packet->getPacketBufferCount() == 1 &&
            packet->setDataLength(unsigned(wanted)) == kIOReturnSuccess && packet->getDataLength() == wanted) {
            auto *bytes = reinterpret_cast<uint8_t *>(packet->getDataVirtualAddress());
            if (bytes) {
                size_t written = 0;
                copied = ItlAWDL::copyDataEthernet(view, bytes, wanted, &written) == ItlAWDL::BuildResult::Ready &&
                    written == wanted && !memcmp(bytes, view.destination, 6) && !memcmp(bytes+6, view.source, 6) &&
                    bytes[12] == 0x86 && bytes[13] == 0xdd && !memcmp(bytes+14, view.ip.bytes, view.ip.length);
                if (copied && dataDeliverEnabled) {
                    // Same framing a software Ethernet driver gives the stack.
                    IOReturn delivered = itlPacketSetHeadroom(packet, 0);
                    if (delivered == kIOReturnSuccess)
                        delivered = itlPacketSetLinkHeaderLength(packet, 14);
                    if (delivered == kIOReturnSuccess)
                        delivered = itlRxEnqueue(packetQueues[3], &packet, 1, 0);
                    if (delivered == kIOReturnSuccess) {
                        // The queue owns the packet now; don't touch or return it.
                        ++dataDelivered;
                        if (dataDelivered == 1 || !(dataDelivered % 16))
                            setProperty("ItlwmAWDLDataDelivered", dataDelivered, 32);
                        return;
                    }
                    ++dataDeliverFailed;
                    if (dataDeliverFailed == 1 || !(dataDeliverFailed % 32))
                        noteQueueState("ItlwmAWDLQueueStateRefused");
                    setProperty("ItlwmAWDLDataDeliverFailed", dataDeliverFailed, 32);
                    setProperty("ItlwmAWDLDataDeliverLastError", delivered, 32);
                }
                bzero(bytes, wanted);
            }
        }
        const auto reset = packet->setDataLength(0);
        completed = packet->completeWithQueue(packetQueues[2], 0, 0);
        copied = copied && reset == kIOReturnSuccess && completed == kIOReturnSuccess;
        pool->deallocatePacket(packet);
    }
    if (copied) ++dataCopySuccess;
    // Unbounded delivery must not write the registry for every failed frame.
    if (dataCopyAttempts > 16 && (dataCopyAttempts % 64)) return;
    setProperty("ItlwmAWDLDataCopyAttempts", dataCopyAttempts, 32);
    setProperty("ItlwmAWDLDataCopySuccess", dataCopySuccess, 32);
    setProperty("ItlwmAWDLDataCopyLastAllocation", allocated, 32);
    setProperty("ItlwmAWDLDataCopyLastPrepare", prepared, 32);
    setProperty("ItlwmAWDLDataCopyLastComplete", completed, 32);
    // Deliberately no native RX enqueue, peer admission or data delivery.
}

IOReturn QwzAWDLInterface::receiveManagementFrame(IO80211Controller *controller,
                                                 const uint8_t *frame, size_t length,
                                                 uint32_t channel, int32_t rssi)
{
    if (!nativeStarted || !queueWorkLoop || !queueWorkLoop->inGate())
        return kIOReturnNotReady;
#if __IO80211_TARGET == __MAC_14_4
    auto *pool = itlGetInterfaceMgmtFramePool(this);   // Sonoma: the virtual interface owns the pool
#else
    auto *pool = itlGetMgmtFramePool(controller);
#endif
    if (!pool)
        return kIOReturnNotReady;
    struct Sink {
        IO80211BufferPool *pool;
        QwzAWDLInterface *interface;
        IO80211Buffer *acquire() { return pool->getBuffer(); }
        void *bytes(IO80211Buffer *p) { return p->getBytesNoCopy(); }
        size_t capacity(IO80211Buffer *p) { return p->getCapacity(); }
        void giveBack(IO80211Buffer *p) { p->returnBuffer(); }
        void setLength(IO80211Buffer *p, size_t n) { p->setLength(n); }
        void submit(IO80211Buffer *p, ItlAWDL::ManagementMetadata *metadata) {
            itlSubmitMgmtFrame(interface, p, 12, metadata, sizeof(*metadata));
        }
    } sink = {pool, this};
    const uint32_t band = channel <= 14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ;
    const IOReturn result = ItlAWDL::copyManagementFrame(sink, frame, length, channel, band, rssi);
    if (result == kIOReturnSuccess) {
        ++managementSubmissions;
        // Count submissions only. Native peer acceptance needs separate evidence.
        if (managementSubmissions == 1 || !(managementSubmissions % 100))
            setProperty("ItlwmAWDLManagementSubmissions", managementSubmissions, 32);
    }
    return result;
}

// Both flags enqueuePackets requires, per queue: IOEventSource's enabled state and
// the native flag at +0xcc, which only IOSkywalkPacketQueue::initialize sets (25G241).
void QwzAWDLInterface::noteQueueState(const char *key)
{
    char state[64];
    size_t used = 0;
    static const char *const names[] = {"txs", "txc", "rxs", "rxc"};
    for (unsigned i = 0; i < 4 && packetQueues[i]; ++i)
        used += snprintf(state + used, sizeof(state) - used, "%s %u %u; ", names[i],
                         packetQueues[i]->isEnabled() ? 1U : 0U,
                         unsigned(reinterpret_cast<const uint8_t *>(packetQueues[i])[0xcc] & 1));
    setProperty(key, state);
}

void QwzAWDLInterface::noteTransmit(IOSkywalkPacket *const *packets, UInt32 count)
{
    ++transmitCallbacks;
    transmitPackets += count;
    if (transmitCallbacks == 1 || !(transmitCallbacks % 64)) {
        setProperty("ItlwmAWDLTxCallbacks", transmitCallbacks, 32);
        setProperty("ItlwmAWDLTxPacketsOffered", transmitPackets, 32);
    }
    if (!qwzAwdlRawSamples || transmitSampled || !packets || !count || !packets[0]) return;
    auto *packet = reinterpret_cast<ItlSkywalkPacketView *>(packets[0]);
    const UInt32 length = packet->getDataLength();
    if (!length || !packet->getPacketBufferCount()) return;
    const auto *bytes = reinterpret_cast<const uint8_t *>(packet->getDataVirtualAddress());
    if (!bytes) return;
    transmitSampled = true;
    if (auto *sample = OSData::withBytes(bytes, min(length, 128U))) {
        setProperty("ItlwmAWDLTxSample", sample);
        sample->release();
    }
}

// A short accepted count leaves the remaining packets owned by Skywalk. Do not
// complete and discard valid packets just because the radio FIFO is full.
UInt32 QwzAWDLInterface::acceptTransmit(IOSkywalkPacket *const *packets, UInt32 count)
{
    UInt32 accepted = 0;
    for (; accepted < count && transmitDoneCount < transmitFrameCount; ++accepted) {
#if __IO80211_TARGET == __MAC_26_0
        if (transmitQueued >= transmitFrameCount) {
            if (!transmitBackpressured) ++transmitBackpressureEvents;
            transmitBackpressured = true;
            break;
        }
#endif
        auto *packet = reinterpret_cast<ItlSkywalkPacketView *>(packets[accepted]);
        const UInt32 length = packet ? packet->getDataLength() : 0;
        const auto *base = packet && packet->getPacketBufferCount() ?
            reinterpret_cast<const uint8_t *>(packet->getDataVirtualAddress()) : nullptr;
        bool queued = false;
        if (base && length >= 14 && length <= sizeof(transmitFrames->bytes) && transmitFrames &&
            transmitQueued < transmitFrameCount) {
            auto &frame = transmitFrames[(transmitHead + transmitQueued) % transmitFrameCount];
            memcpy(frame.bytes, base + itlPacketGetDataOffset(packet), length);
            frame.length = uint16_t(length);
            ++transmitQueued;
            queued = true;
        }
        if (!queued) ++transmitDropped;
        transmitDone[transmitDoneCount++] = packets[accepted];
    }
#if __IO80211_TARGET == __MAC_26_0
    if (accepted < count && !transmitBackpressured) {
        ++transmitBackpressureEvents;
        transmitBackpressured = true;
    }
#endif
    transmitAccepted += accepted;
    return accepted;
}

bool QwzAWDLInterface::copyTransmitDestination(uint8_t *destination) const
{
    if (!destination || !transmitFrames || !transmitQueued || transmitFrames[transmitHead].length < 14)
        return false;
    memcpy(destination, transmitFrames[transmitHead].bytes, 6);
    return true;
}

bool QwzAWDLInterface::popTransmit(uint8_t *output, size_t available, size_t *written,
                                   const uint8_t *holdPayloadFor, bool holdEverySize)
{
    ++transmitDepthBins[ItlAWDL::depthBin(transmitQueued)];
    while (transmitQueued) {
        const auto &frame = transmitFrames[transmitHead];
        const auto result = ItlAWDL::buildDataFrameFromEthernet(frame.bytes, frame.length, localFrameAddress,
                                                                transmitSequence, output, available, written);
        // Hold what is for the capped peer. Leave the FIFO head and sequence intact. With holdEverySize
        // (iface160) that is every unicast data frame for it: a short frame let past would overtake the
        // held packets of the same QUIC connection on the listener's unpaused queue, and QUIC counts
        // overtaken packets as lost. Without it, only payload-sized frames, as on iface157 to iface159.
        if (result == ItlAWDL::BuildResult::Ready && holdPayloadFor && (holdEverySize || *written >= 1200) &&
            output[0] == 0x08 && ItlAWDL::sameAddress(output + 4, holdPayloadFor)) {
            // Once per frame, not once per attempt: the pump asks again every millisecond.
            if (*written < 1200 && transmitHeldMark != transmitPopped + 1) {
                transmitHeldMark = transmitPopped + 1;
                ++transmitHeldShort;
            }
            return false;
        }
        transmitHead = (transmitHead + 1) % transmitFrameCount;
        --transmitQueued;
        ++transmitPopped;
        if (result == ItlAWDL::BuildResult::Ready) {
            ++transmitSequence;
            return true;
        }
        ++transmitDropped;
    }
    return false;
}

bool QwzAWDLInterface::serviceTransmit()
{
    if (transmitDoneCount) {
        const unsigned count = min(transmitDoneCount, 32U);
        const IOReturn result = itlTxComplete(packetQueues[1], reinterpret_cast<void *const *>(transmitDone), count, 0);
        if (result == kIOReturnSuccess) {
            transmitDoneCount -= count;
            memmove(transmitDone, transmitDone + count, transmitDoneCount * sizeof(transmitDone[0]));
            transmitCompleted += count;
        } else {
            setProperty("ItlwmAWDLTxCompleteLastError", result, 32);
        }
    }
#if __IO80211_TARGET == __MAC_26_0
    if (transmitBackpressured && transmitQueued < transmitFrameCount &&
        transmitDoneCount < transmitFrameCount && dataTransmitEnabled &&
        addedQueues > 1 && packetQueues[0]) {
        // Clear before the request: native dequeue may synchronously call back
        // and fill the FIFO again. Never clear a new backpressure indication.
        transmitBackpressured = false;
        const IOReturn result = itlTxRequestDequeue(packetQueues[0], nullptr, 1);
        if (result == kIOReturnSuccess)
            ++transmitResumeRequests;
        else {
            transmitBackpressured = true;
            setProperty("ItlwmAWDLTxResumeLastError", result, 32);
        }
    }
#endif
    if (transmitCompleted != transmitLastPublished) {
        transmitLastPublished = transmitCompleted;
        setProperty("ItlwmAWDLTxAccepted", transmitAccepted, 32);
        setProperty("ItlwmAWDLTxCompleted", transmitCompleted, 32);
        setProperty("ItlwmAWDLTxDropped", transmitDropped, 32);
        setProperty("ItlwmAWDLTxBackpressureEvents", transmitBackpressureEvents, 32);
        setProperty("ItlwmAWDLTxResumeRequests", transmitResumeRequests, 32);
        if (auto *depths = OSData::withBytes(transmitDepthBins, sizeof(transmitDepthBins))) {
            setProperty("ItlwmAWDLTxDepthBins", depths);
            depths->release();
        }
    }
    return transmitQueued || transmitDoneCount || transmitBackpressured;
}

UInt32 QwzAWDLInterface::rejectTransmit(OSObject *owner, IOSkywalkTxSubmissionQueue*,
                                     IOSkywalkPacket *const *packets, UInt32 count, void*)
{
    // Native legacyDequeue interprets this as an accepted packet count and stops
    // on zero without retrying. Keep ownership with the queue until an AWDL radio
    // transport is implemented; only record that macOS tried to send.
    if (auto *interface = OSDynamicCast(QwzAWDLInterface, owner)) {
        interface->noteTransmit(packets, count);
        if (interface->dataTransmitEnabled)
            return interface->acceptTransmit(packets, count);
    }
    return 0;
}

UInt32 QwzAWDLInterface::deferReceive(OSObject *owner, IOSkywalkRxSubmissionQueue*,
                                   IOSkywalkPacket *const*, UInt32, void*)
{
    if (auto *interface = OSDynamicCast(QwzAWDLInterface, owner)) {
        ++interface->receiveSupplyCallbacks;
        if (interface->receiveSupplyCallbacks == 1 || !(interface->receiveSupplyCallbacks % 64))
            interface->setProperty("ItlwmAWDLRxSupplyCallbacks", interface->receiveSupplyCallbacks, 32);
    }
    return 0;
}

// Hands every packet still held for completion back to Skywalk, while the completion queue still
// exists. Without this, packets accepted but not yet completed stayed owned by us when the queues
// and pools were released.
void QwzAWDLInterface::drainTransmitDone()
{
    if (!transmitDoneCount)
        return;
    if (addedQueues > 1 && packetQueues[1]) {
        while (transmitDoneCount) {
            const unsigned count = min(transmitDoneCount, 32U);
            const IOReturn result = itlTxComplete(packetQueues[1], reinterpret_cast<void *const *>(transmitDone), count, 0);
            if (result != kIOReturnSuccess) {
                setProperty("ItlwmAWDLTxCompleteLastError", result, 32);
                break;
            }
            transmitDoneCount -= count;
            memmove(transmitDone, transmitDone + count, transmitDoneCount * sizeof(transmitDone[0]));
            transmitCompleted += count;
        }
    }
    // Whatever is left can't be completed any more; never hand it back later, to a new queue.
    if (transmitDoneCount)
        setProperty("ItlwmAWDLTxAbandonedAtStop", transmitDoneCount, 32);
    transmitDoneCount = 0;
}

void QwzAWDLInterface::releaseNetworkResources()
{
    // In the queues' work loop, like serviceTransmit, so the two never run at once.
    if (queueWorkLoop && transmitDoneCount)
        queueWorkLoop->runAction([](OSObject *owner, void*, void*, void*, void*) -> IOReturn {
            static_cast<QwzAWDLInterface *>(owner)->drainTransmitDone();
            return kIOReturnSuccess;
        }, this);
    transmitQueued = 0;
    transmitHead = 0;
    transmitBackpressured = false;
    if (registrationAttempted) {
        registrationAttempted = false;
        deregisterEthernetInterface(0);
    }
    for (UInt32 i = addedQueues; i; --i) {
        packetQueues[i-1]->disable();
        queueWorkLoop->removeEventSource(packetQueues[i-1]);
    }
    addedQueues = 0;
    for (auto &queue : packetQueues)
        OSSafeReleaseNULL(queue);
    OSSafeReleaseNULL(txPool);
    OSSafeReleaseNULL(rxPool);
    OSSafeReleaseNULL(queueWorkLoop);
}

// #13: on Sequoia 15.8, awdl0 is missing although this interface carries "BSD Name" = awdl0. The
// BSD client writes that name before it creates the ifnet, so a failed start and a later detach look
// the same from outside. Each entry records the time since boot, the client's class, whether that
// client is already terminating (a detach) or still active (a failed start), the process that caused
// the call, and for closes and finalisations the unslid return addresses, which map onto the build's
// boot kernel collection. Nothing here changes behaviour.
void QwzAWDLInterface::noteClientEvent(const char *what, IOService *client, long result, bool trace)
{
    uint64_t now = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &now);
    char process[32] = {};
    proc_selfname(process, sizeof(process));
    const char *name = client && client->getMetaClass() ? client->getMetaClass()->getClassName() : "-";
    char *entry = clientEvents[clientEventCount % clientEventCapacity];
    int used = snprintf(entry, sizeof(clientEvents[0]), "%llu ms %s %s r=%ld inactive=%d proc=%s",
                        now / 1000000ULL, what, name, result, client ? int(client->isInactive()) : -1, process);
    if (trace && used > 0 && size_t(used) < sizeof(clientEvents[0])) {
        void *frames[8] = {};
        const unsigned depth = OSBacktrace(frames, 8);
        for (unsigned i = 1; i < depth && size_t(used) < sizeof(clientEvents[0]) - 20; ++i) {
            vm_offset_t unslid = 0;
            vm_kernel_unslide_or_perm_external(vm_offset_t(frames[i]), &unslid);
            used += snprintf(entry + used, sizeof(clientEvents[0]) - size_t(used), " %lx", (unsigned long)unslid);
        }
    }
    ++clientEventCount;
    if (OSArray *list = OSArray::withCapacity(clientEventCapacity)) {
        const unsigned count = clientEventCount < clientEventCapacity ? clientEventCount : clientEventCapacity;
        for (unsigned i = clientEventCount - count; i < clientEventCount; ++i) {
            if (OSString *text = OSString::withCString(clientEvents[i % clientEventCapacity])) {
                list->setObject(text);
                text->release();
            }
        }
        setProperty("ItlwmAWDLClientEvents", list);
        list->release();
    }
    setProperty("ItlwmAWDLClientEventCount", clientEventCount, 32);
}

bool QwzAWDLInterface::handleOpen(IOService *client, IOOptionBits options, void *argument)
{
    const bool opened = IO80211AWDLProtocol::handleOpen(client, options, argument);
    noteClientEvent("open", client, opened ? 1 : 0, false);
    return opened;
}

void QwzAWDLInterface::handleClose(IOService *client, IOOptionBits options)
{
    noteClientEvent("close", client, long(options), true);
    IO80211AWDLProtocol::handleClose(client, options);
}

#if __IO80211_TARGET == __MAC_15_2
IOReturn QwzAWDLInterface::prepareBSDInterface(struct __ifnet *interface, unsigned int flags)
{
    const IOReturn result = IO80211AWDLProtocol::prepareBSDInterface(interface, flags);
    noteClientEvent("prepare-bsd", nullptr, long(result), false);
    return result;
}

void QwzAWDLInterface::finalizeBSDInterface(struct __ifnet *interface, unsigned int flags)
{
    noteClientEvent("finalize-bsd", nullptr, long(flags), true);
    IO80211AWDLProtocol::finalizeBSDInterface(interface, flags);
}
#endif

void QwzAWDLInterface::stop(IOService *provider)
{
    const bool started = nativeStarted;
    nativeStarted = false;
    releaseNetworkResources();
    if (started)
        IO80211AWDLProtocol::stop(provider);
}

void QwzAWDLInterface::free()
{
    if (nativeStarted)
        stop(getProvider());
    else
        releaseNetworkResources();
    if (transmitFrames) {
        IOFree(transmitFrames, sizeof(TransmitFrame) * transmitFrameCount);
        transmitFrames = nullptr;
    }
    if (configurationLock) {
        IOLockFree(configurationLock);
        configurationLock = nullptr;
    }
    IO80211AWDLProtocol::free();
}

IOReturn QwzAWDLInterface::initializeNetwork(IO80211Controller *controller,
                                           IOWorkLoop *workLoop,
                                           const ether_addr &address)
{
    auto stage = [this, controller](const char *name) {
        setProperty("ItlwmAWDLNetworkStage", name);
        controller->setProperty("ItlwmAWDLNetworkStage", name);
    };
    auto fail = [this, controller](IOReturn result) {
        stop(controller);
        return result;
    };
    if (!controller || !workLoop || nativeStarted)
        return kIOReturnBadArgument;
    if (!configurationLock)
        configurationLock = IOLockAlloc();
    if (!configurationLock)
        return kIOReturnNoMemory;
    static_assert(sizeof(address) == sizeof(localFrameAddress), "native AWDL address size");
    memcpy(localFrameAddress, &address, sizeof(localFrameAddress));
    int dataCopyArgument = 0;
    dataCopyProbeEnabled = PE_parse_boot_argn("-itlawdldatacopy", &dataCopyArgument, sizeof(dataCopyArgument));
    // -itlawdlcoexdata (normal Wi-Fi with the listener) needs delivery, transmit and the queues enabled too.
    const bool coexData = PE_parse_boot_argn("-itlawdlcoexdata", &dataCopyArgument, sizeof(dataCopyArgument));
    dataDeliverEnabled = coexData || PE_parse_boot_argn("-itlawdldatarx", &dataCopyArgument, sizeof(dataCopyArgument));
    // The copy counters belong to the -itlawdldatacopy probe; published as zeros without it, they read as a
    // failure next to working delivery (#7). ItlwmAWDLTransportReady was never set true by anything and is
    // no longer published; ItlwmAWDLDataDelivered and the TX counters show the real state.
    if (dataCopyProbeEnabled) {
        setProperty("ItlwmAWDLDataCopyAttempts", uint64_t(0), 32);
        setProperty("ItlwmAWDLDataCopySuccess", uint64_t(0), 32);
    }
    setProperty("ItlwmAWDLConfigurationMode", "requested-only");
    stage("starting");
    if (!IO80211AWDLProtocol::start(controller)) {
        stage("native-start-failed");
        return kIOReturnError;
    }
    nativeStarted = true;
    queueWorkLoop = workLoop;
    queueWorkLoop->retain();

    stage("allocating-pools");
    // iface162: the TX pool must outlast macOS's own awdl0 queue. The flowswitch copies every outgoing
    // packet into this pool before FQ-CoDel queues it (672 packets allowed), and the submission and
    // completion queues hold 64 each on top. The bring-up size (128 requested, 896 after Skywalk's
    // rounding) ran dry at the queue's peak. The flowswitch then drops the batch (ENOMEM). When FQ-CoDel
    // had not also flow-controlled the flow, QUIC's link stayed flow controlled with nothing to release
    // it: the persistent stalls of 2026-11-10 at 15:32, 15:42, 16:41 and 17:33 each began within 60 ms of
    // "skc.kmd.itlwm-awdl-tx: failed to allocate slab". itlawdltxpool=N
    // (896 to 8192) overrides the default for tests. The RX pool keeps the bring-up size.
    uint32_t txPoolPackets = 2048;
    int requestedPool = 0;
    if (PE_parse_boot_argn("itlawdltxpool", &requestedPool, sizeof(requestedPool)) &&
        requestedPool >= 896 && requestedPool <= 8192)
        txPoolPackets = uint32_t(requestedPool);
    const IOSkywalkPacketBufferPool::PoolOptions txOptions = {
        txPoolPackets, txPoolPackets, 2048, 1, 4096, 0, 0
    };
    const IOSkywalkPacketBufferPool::PoolOptions options = {
        128, 128, 2048, 1, 4096, 0, 0
    };
    // Pool type: IOSkywalkFamily turns 0 into KBIF_QUANTUM and 2 into KBIF_BUFFER_ON_DEMAND. On Sequoia
    // (XNU 11417) and Sonoma, a quantum pool gets quantum metadata, which nx_check_pp refuses for a network
    // interface (EINVAL from kern_nexus_controller_alloc_net_provider_instance), so awdl0 was never attached
    // (#7, #13, #16; found by #19 on 15.6). 1 makes a packet pool, as Apple's own drivers use. Tahoe keeps
    // the 0 every Tahoe test ran with. itlawdlpooltype=0..2 overrides it for tests.
#if __IO80211_TARGET == __MAC_15_2 || __IO80211_TARGET == __MAC_14_4
    UInt32 poolType = 1;
#else
    UInt32 poolType = 0;
#endif
    int requestedPoolType = -1;
    if (PE_parse_boot_argn("itlawdlpooltype", &requestedPoolType, sizeof(requestedPoolType)) &&
        requestedPoolType >= 0 && requestedPoolType <= 2)
        poolType = UInt32(requestedPoolType);
    setProperty("ItlwmAWDLPoolType", poolType, 32);
    txPool = IOSkywalkPacketBufferPool::withName("itlwm-awdl-tx", this, poolType, &txOptions);
    rxPool = IOSkywalkPacketBufferPool::withName("itlwm-awdl-rx", this, poolType, &options);
    setProperty("ItlwmAWDLTxPoolPackets", txPoolPackets, 32);
    if (!txPool || !rxPool)
        return fail(kIOReturnNoMemory);

    stage("allocating-queues");
    packetQueues[0] = itlMakeTxSubmission(txPool, 64, 0, this, rejectTransmit, nullptr, 0);
    packetQueues[1] = itlMakeTxCompletion(txPool, 64, 0, this, nullptr, nullptr, 0);
    packetQueues[2] = itlMakeRxSubmission(rxPool, 64, 1, 0, this, deferReceive, nullptr, 0);
    packetQueues[3] = itlMakeRxCompletion(rxPool, 64, 0, this, nullptr, nullptr, 0);
    for (auto queue : packetQueues)
        if (!queue)
            return fail(kIOReturnNoMemory);

    stage("adding-queues");
    for (auto queue : packetQueues) {
        IOReturn result = queueWorkLoop->addEventSource(queue);
        if (result != kIOReturnSuccess)
            return fail(result);
        ++addedQueues;
    }

    int poolProbe = 0;
    if (PE_parse_boot_argn("-itlawdlpoolprobe", &poolProbe, sizeof(poolProbe))) {
        auto probe = [this](IOSkywalkPacketBufferPool *nativePool, IOEventSource *queue, const char *key) {
            auto *pool = reinterpret_cast<ItlSkywalkPoolView *>(nativePool);
            ItlSkywalkPacketView *packet = nullptr;
            const IOReturn allocation = pool->allocatePacket(1, &packet, 1);
            setProperty(key, allocation, 32);
            if (allocation != kIOReturnSuccess || !packet) return;
            const IOReturn prepared = packet->prepareWithQueue(queue, 0, 0);
            const UInt32 count = packet->getPacketBufferCount();
            const UInt32 initialLength = packet->getDataLength();
            const IOReturn set = prepared == kIOReturnSuccess && count == 1 ? packet->setDataLength(56) : kIOReturnUnsupported;
            const UInt32 length = packet->getDataLength();
            bool copied = false;
            if (prepared == kIOReturnSuccess && count == 1 && set == kIOReturnSuccess && length == 56) {
                auto *bytes = reinterpret_cast<uint8_t *>(packet->getDataVirtualAddress());
                if (bytes) {
                    // Offline test vector only: never enqueue these bytes.
                    const uint8_t ip[40] = {0x60}; size_t written = 0;
                    ItlAWDL::IPv6PayloadView view;
                    copied = ItlAWDL::buildIPv6DataPayload(ip, sizeof(ip), 0x1234, bytes, 56, &written) ==
                        ItlAWDL::BuildResult::Ready && written == 56 &&
                        ItlAWDL::parseIPv6DataPayload(bytes, written, &view) == ItlAWDL::ParseResult::Valid &&
                        view.sequence == 0x1234 && view.length == sizeof(ip) && !memcmp(view.bytes, ip, sizeof(ip));
                    bzero(bytes, 56);
                }
            }
            const IOReturn reset = packet->setDataLength(0);
            const IOReturn completed = packet->completeWithQueue(queue, 0, 0);
            pool->deallocatePacket(packet);
            auto *details = OSDictionary::withCapacity(10);
            if (details) {
                auto add = [details](const char *name, UInt32 value) {
                    if (auto *number = OSNumber::withNumber(value, 32)) {
                        details->setObject(name, number); number->release();
                    }
                };
                add("allocation", allocation); add("prepareStatus", prepared); add("completeStatus", completed); add("buffers", count);
                add("initialLength", initialLength); add("setStatus", set);
                add("length", length); add("resetStatus", reset); add("returnCalled", 1); add("payloadRoundtrip", copied ? 1 : 0);
                setProperty(nativePool == txPool ? "ItlwmAWDLTxPoolDetails" : "ItlwmAWDLRxPoolDetails", details);
                details->release();
            }
            const bool ok = prepared == kIOReturnSuccess && completed == kIOReturnSuccess && count == 1 && initialLength == 0 && set == kIOReturnSuccess &&
                length == 56 && copied && reset == kIOReturnSuccess;
            setProperty(key, ok ? "lease-roundtrip-ok" : "lease-check-failed");
            setProperty("ItlwmAWDLPoolProbeBufferCount", count, 32);
            setProperty("ItlwmAWDLPoolProbeLength", length, 32);
        };
        probe(txPool, packetQueues[0], "ItlwmAWDLTxPoolProbe");
        probe(rxPool, packetQueues[2], "ItlwmAWDLRxPoolProbe");
    }


    IOSkywalkEthernetInterface::RegistrationInfo info = {};
    if (!initRegistrationInfo(&info, 1, sizeof(info)))
        return fail(kIOReturnBadArgument);
    // Offsets are from the 25G241 native AWDL registration path. The framework
    // copies this metadata; the name points to a static string in the kext.
    auto put32 = [&info](size_t offset, UInt32 value) {
        memcpy(info.bytes + offset, &value, sizeof(value));
    };
    put32(0x0c, 3);
    put32(0x14, 2);
    put32(0x24, 0x80);
    put32(0x28, 0x80);
    const char *prefix = "awdl";
    memcpy(info.bytes + 0x30, &prefix, sizeof(prefix));
    put32(0x38, 0);
    const UInt16 headroom = 0x40;
    memcpy(info.bytes + 0x3c, &headroom, sizeof(headroom));
    put32(0x40, 4);
    put32(0x48, 1428);
    put32(0x4c, 1428);
    memcpy(info.bytes + 0x108, &address, sizeof(address));
    IOSkywalkPacketQueue *queues[4];
    for (UInt32 i = 0; i < 4; ++i)
        queues[i] = reinterpret_cast<IOSkywalkPacketQueue *>(packetQueues[i]);

    stage("registering");
    registrationAttempted = true;
    IOReturn result = registerEthernetInterface(&info, queues, 4, txPool, rxPool, 0);
    if (result != kIOReturnSuccess) {
        stage("registration-failed");
        return fail(result);
    }
    setRunningState(false);
    noteQueueState("ItlwmAWDLQueueStateAtRegistration");
    int dataTransmit = 0;
    if (PE_parse_boot_argn("-itlawdldatatx", &dataTransmit, sizeof(dataTransmit)) ||
        PE_parse_boot_argn("-itlawdlcoexdata", &dataTransmit, sizeof(dataTransmit))) {
        int requestedQueue = 0;
        if (PE_parse_boot_argn("itlawdltxqueue", &requestedQueue, sizeof(requestedQueue)) &&
            requestedQueue >= int(transmitFrameDefault) && requestedQueue <= int(transmitFrameCapacity))
            transmitFrameCount = unsigned(requestedQueue);
        transmitFrames = static_cast<TransmitFrame *>(IOMalloc(sizeof(TransmitFrame) * transmitFrameCount));
        dataTransmitEnabled = transmitFrames != nullptr;
        setProperty("ItlwmAWDLTxQueueFrames", transmitFrameCount, 32);
    }
    int queueEnable = 0;
    if (PE_parse_boot_argn("-itlawdlqueueenable", &queueEnable, sizeof(queueEnable)) ||
        PE_parse_boot_argn("-itlawdlcoexdata", &queueEnable, sizeof(queueEnable))) {
        for (auto queue : packetQueues)
            queue->enable();
        noteQueueState("ItlwmAWDLQueueStateAfterEnable");
    }
    stage("registered");
    registerService();
    return kIOReturnSuccess;
}
#endif /* ITL_AWDL_NATIVE */
