// SPDX-License-Identifier: GPL-3.0-only
// Included by the existing IntelBTPatcher.cpp; uses its one deviceRequest hook.
#ifndef QCABT_HOST_TEST
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/OSAtomic.h>
#endif
// Declare only this export: libkern/version.h conflicts with Lilu's version_major linkage.
extern "C" const char version[];
#include "QcaBluetoothHci.hpp"

static bool qcaContinuityEnabled {};
static const char *qcaOsBuild {};
static IOLock *qcaStateLock {};
static QcaBluetooth::State qcaState {};
static uint64_t qcaOwner {};
static uint32_t qcaGeneration {};
static bool qcaBusy {};
static uint32_t qcaAddresses {}, qcaMasks {}, qcaFailures {}, qcaBypasses {};

static void qcaNote(const char *key, uint32_t value) {
    IORegistryEntry::getRegistryRoot()->setProperty(key, value, 32);
}
// Diagnostic-only counters. Publish the first few events and powers of two,
// avoiding an allocation/property update for every advertising retry.
struct QcaCounter { const char *key; volatile SInt32 value; };
static QcaCounter qcaTargetRequests {"QcaContinuityTargetRequests", 0};
static QcaCounter qcaAsyncRequests {"QcaContinuityAsyncRequests", 0};
static QcaCounter qcaSyncRequests {"QcaContinuitySyncRequests", 0};
static QcaCounter qcaOtherRequests {"QcaContinuityOtherRequests", 0};
static QcaCounter qcaInvalidRequests {"QcaContinuityInvalidRequests", 0};
static QcaCounter qcaMissingBuffers {"QcaContinuityMissingBuffers", 0};
static QcaCounter qcaPrepareFailures {"QcaContinuityPrepareFailures", 0};
static QcaCounter qcaShortReads {"QcaContinuityShortReads", 0};
static QcaCounter qcaMalformedCommands {"QcaContinuityMalformedCommands", 0};
static QcaCounter qcaCommands {"QcaContinuityHciCommands", 0};
static QcaCounter qcaGeneralAddresses {"QcaContinuityGeneralAddressCommands", 0};
static QcaCounter qcaSetAddresses {"QcaContinuitySetAddressCommands", 0};
static QcaCounter qcaParams {"QcaContinuityExtendedParamsCommands", 0};
static QcaCounter qcaEnableCommands {"QcaContinuityEnableCommands", 0};
static QcaCounter qcaMaskCommands {"QcaContinuityMaskCommands", 0};
static QcaCounter qcaScanInitCommands {"QcaContinuityScanInitCommands", 0};
static QcaCounter qcaScanInitFailures {"QcaContinuityScanInitFailures", 0};
static QcaCounter qcaAsyncCompletions {"QcaContinuityAsyncCompletions", 0};
static QcaCounter qcaAsyncFailures {"QcaContinuityAsyncFailures", 0};
static QcaCounter qcaAsyncAddresses {"QcaContinuityAddressSubmissions", 0};
static QcaCounter qcaAsyncMasks {"QcaContinuityMaskSubmissions", 0};
static QcaCounter *qcaCounters[] = {&qcaTargetRequests, &qcaAsyncRequests, &qcaSyncRequests,
    &qcaOtherRequests, &qcaInvalidRequests, &qcaMissingBuffers, &qcaPrepareFailures,
    &qcaShortReads, &qcaMalformedCommands, &qcaCommands, &qcaGeneralAddresses,
    &qcaSetAddresses, &qcaParams, &qcaEnableCommands, &qcaMaskCommands,
    &qcaScanInitCommands, &qcaScanInitFailures, &qcaAsyncCompletions,
    &qcaAsyncFailures, &qcaAsyncAddresses, &qcaAsyncMasks};
static bool qcaCount(QcaCounter &counter) {
    const uint32_t count = uint32_t(OSIncrementAtomic(&counter.value)) + 1;
    if (count <= 4 || (count & (count - 1)) == 0) {
        qcaNote(counter.key, count);
        return true;
    }
    return false;
}
static bool qcaTarget(void *that) {
    auto *device = static_cast<IORegistryEntry *>(that);
    auto *vendor = OSDynamicCast(OSNumber, device->getProperty("idVendor"));
    auto *product = OSDynamicCast(OSNumber, device->getProperty("idProduct"));
    return vendor && product && QcaBluetooth::targetDevice(vendor->unsigned32BitValue(), product->unsigned32BitValue());
}
static void qcaInit(KernelPatcher &patcher) {
    if (!checkKernelArgument("-qcaadv") || checkKernelArgument("-qcaadvoff")) return;
    qcaOsBuild = reinterpret_cast<const char *>(patcher.solveSymbol(KernelPatcher::KernelID, "_osversion"));
    patcher.clearError();
    const auto gate = QcaBluetooth::buildGate(version, qcaOsBuild);
    qcaContinuityEnabled = getKernelVersion() == KernelVersion::Tahoe && QcaBluetooth::allowed(gate);
    auto *root = IORegistryEntry::getRegistryRoot();
    root->setProperty("QcaContinuityHciGate", QcaBluetooth::gateReason(gate));
    root->setProperty("QcaContinuityHciBuildAtInit", !qcaOsBuild ? "<unresolved>" : *qcaOsBuild ? qcaOsBuild : "<empty>");
    if (qcaContinuityEnabled) {
        qcaStateLock = IOLockAlloc();
        qcaContinuityEnabled = qcaStateLock != nullptr;
    }
    qcaNote("QcaContinuityHciEnabled", qcaContinuityEnabled);
    for (auto *counter : qcaCounters) qcaNote(counter->key, 0);
    root->setProperty("QcaContinuityRevision", "1.1.0-experimental.6");
}

static IOReturn qcaSend(void *that, IOService *provider, const StandardUSB::DeviceRequest &original,
                        const uint8_t *bytes, size_t size, unsigned int timeout) {
    auto *buffer = IOBufferMemoryDescriptor::withBytes(bytes, size, kIODirectionOut);
    if (!buffer) return kIOReturnNoMemory;
    auto request = original;
    request.wLength = uint16_t(size);
    unsigned int transferred = 0;
    IOReturn result = buffer->prepare(kIODirectionOut);
    if (result == kIOReturnSuccess) {
        // Bypass our wrapper, never modify the original caller's length or descriptor.
        result = FunctionCast(CIntelBTPatcher::newHostDeviceRequest, callbackIBTPatcher->oldHostDeviceRequest)(
            that, provider, request, nullptr, buffer, transferred, nullptr,
            timeout == 0 || timeout > 1000 ? 1000 : timeout);
        buffer->complete(kIODirectionOut);
        if (result == kIOReturnSuccess && transferred != size) result = kIOReturnUnderrun;
    }
    buffer->release();
    return result;
}

#include "QcaBluetoothAsync.inc"

static IOReturn qcaRequest(void *that, IOService *provider, StandardUSB::DeviceRequest &request,
                            void *data, IOMemoryDescriptor *descriptor, unsigned int &length,
                            IOUSBHostCompletion *completion, unsigned int timeout) {
    auto original = FunctionCast(CIntelBTPatcher::newHostDeviceRequest, callbackIBTPatcher->oldHostDeviceRequest);
    qcaCount(qcaTargetRequests);
    // Recheck when userspace sends commands: a populated, unknown build must never
    // inherit an early decision made while the osversion buffer was empty.
    const auto gate = QcaBluetooth::buildGate(version, qcaOsBuild);
    if (!QcaBluetooth::allowed(gate)) {
        qcaNote("QcaContinuityHciEnabled", 0);
        IORegistryEntry::getRegistryRoot()->setProperty("QcaContinuityHciGate", QcaBluetooth::gateReason(gate));
        return original(that, provider, request, data, descriptor, length, completion, timeout);
    }
    // Inspect synchronous and asynchronous HCI OUT requests alike.
    uint8_t hci[32] {};
    const size_t size = request.wLength;
    if (request.bmRequestType != 0x20 && request.bmRequestType != 0x21) {
        qcaCount(qcaOtherRequests);
        return original(that, provider, request, data, descriptor, length, completion, timeout);
    }
    if (completion) qcaCount(qcaAsyncRequests);
    else qcaCount(qcaSyncRequests);
    if (request.wValue != 0 || size < 3 || size > sizeof(hci)) {
        if (qcaCount(qcaInvalidRequests)) {
            qcaNote("QcaContinuityLastRequestLength", uint32_t(size));
            qcaNote("QcaContinuityLastRequestValue", request.wValue);
        }
        return original(that, provider, request, data, descriptor, length, completion, timeout);
    }
    if (data) {
        lilu_os_memcpy(hci, data, size);
    } else {
        if (!descriptor || descriptor->getLength() < size) {
            qcaCount(qcaMissingBuffers);
            return original(that, provider, request, data, descriptor, length, completion, timeout);
        }
        // Kernel-owned IOBufferMemoryDescriptor storage can be copied directly;
        // generic task descriptors still require our own successful prepare hold.
        auto *kernelBuffer = OSDynamicCast(IOBufferMemoryDescriptor, descriptor);
        if (kernelBuffer && kernelBuffer->getBytesNoCopy(0, size)) {
            lilu_os_memcpy(hci, kernelBuffer->getBytesNoCopy(0, size), size);
        } else {
        const IOReturn prepared = descriptor->prepare(kIODirectionOut);
        if (prepared != kIOReturnSuccess) {
            if (qcaCount(qcaPrepareFailures)) qcaNote("QcaContinuityLastPrepareResult", uint32_t(prepared));
            return original(that, provider, request, data, descriptor, length, completion, timeout);
        }
        const size_t read = descriptor->readBytes(0, hci, size);
        descriptor->complete(kIODirectionOut);
        if (read != size) {
            qcaCount(qcaShortReads);
            return original(that, provider, request, data, descriptor, length, completion, timeout);
        }
    }
    }
    if (!QcaBluetooth::command(hci, size)) {
        qcaCount(qcaMalformedCommands);
        return original(that, provider, request, data, descriptor, length, completion, timeout);
    }
    qcaCount(qcaCommands);
    switch (QcaBluetooth::opcode(hci)) {
    case 0x2005: qcaCount(qcaGeneralAddresses); break;
    case 0x2035: qcaCount(qcaSetAddresses); break;
    case 0x2036:
        if (qcaCount(qcaParams)) {
            qcaNote("QcaContinuityLastParamsLength", uint32_t(size));
            if (size > 13) qcaNote("QcaContinuityLastOwnAddressType", hci[13]);
        }
        break;
    case 0x2039: qcaCount(qcaEnableCommands); break;
    case 0x2001: qcaCount(qcaMaskCommands); break;
    default: break;
    }

    if (completion)
        return qcaAsyncStart(that, provider, request, hci, size, *completion, timeout);

    IOLockLock(qcaStateLock);
    // Avoid holding a lock over USB calls. A concurrent request invalidates the snapshot;
    // it passes through, and the active request discards its cache when it returns.
    if (qcaBusy) {
        ++qcaGeneration;
        const uint32_t bypasses = ++qcaBypasses;
        IOLockUnlock(qcaStateLock);
        qcaNote("QcaContinuityConcurrentBypasses", bypasses);
        return original(that, provider, request, data, descriptor, length, completion, timeout);
    }
    qcaBusy = true;
    const uint64_t owner = static_cast<IORegistryEntry *>(that)->getRegistryEntryID();
    if (qcaOwner != owner) { qcaState = QcaBluetooth::State{}; qcaOwner = owner; }
    const uint32_t generation = qcaGeneration;
    auto state = qcaState;
    IOLockUnlock(qcaStateLock);

    bool failed = false;
    if (QcaBluetooth::needsScanAddress(state, hci, size)) {
        // Preserve IntelBTPatcher's legacy scan workaround; never cache it for advertising.
        const uint8_t fixed[] = {0x05,0x20,0x06,0x94,0x50,0x64,0xd0,0x78,0x6b};
        const auto status = qcaSend(that, provider, request, fixed, sizeof(fixed), timeout);
        qcaCount(qcaScanInitCommands);
        if (status != kIOReturnSuccess) qcaCount(qcaScanInitFailures);
        state.scanInitialized = status == kIOReturnSuccess;
        failed = status != kIOReturnSuccess;
    }
    const bool maskChanged = QcaBluetooth::widenEventMask(hci, size);
    const IOReturn result = maskChanged ? qcaSend(that, provider, request, hci, size, timeout) :
        original(that, provider, request, data, descriptor, length, completion, timeout);
    if (maskChanged && result == kIOReturnSuccess) length = unsigned(size);
    bool addressSent = false;
    if (result == kIOReturnSuccess) {
        const auto action = QcaBluetooth::submitted(state, hci, size);
        if (action.size) {
            addressSent = qcaSend(that, provider, request, action.bytes, action.size, timeout) == kIOReturnSuccess;
            if (addressSent) QcaBluetooth::addressSubmitted(state, action);
            else failed = true; // leave the set unmarked so the next parameters command retries
        }
    }
    IOLockLock(qcaStateLock);
    qcaState = generation == qcaGeneration ? state : QcaBluetooth::State{};
    qcaBusy = false;
    if (addressSent) ++qcaAddresses;
    if (maskChanged && result == kIOReturnSuccess) ++qcaMasks;
    if (failed || result != kIOReturnSuccess) ++qcaFailures;
    const uint32_t addresses = qcaAddresses, masks = qcaMasks, failures = qcaFailures;
    IOLockUnlock(qcaStateLock);
    // These count USB submissions, not confirmed HCI success or on-air advertisements.
    if (addressSent) qcaNote("QcaContinuityAddressSubmissions", addresses);
    if (maskChanged) qcaNote("QcaContinuityMaskSubmissions", masks);
    if (failed || result != kIOReturnSuccess) qcaNote("QcaContinuityUsbFailures", failures);
    return result;
}
