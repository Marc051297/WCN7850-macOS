// SPDX-License-Identifier: GPL-3.0-only
// Native asynchronous USB requests, with copied OUT buffers and exactly one
// original completion. Never issue a synchronous USB request from a completion.
enum class QcaAsyncPhase { ScanAddress, Original, SetAddress };
struct QcaAsyncRequest {
    volatile SInt32 references;
    volatile UInt32 finished;
    IOService *device;
    IOService *provider;
    IOUSBHostCompletion original;
    StandardUSB::DeviceRequest request;
    IOBufferMemoryDescriptor *mainBuffer;
    IOBufferMemoryDescriptor *extraBuffer;
    uint8_t command[32];
    size_t size;
    unsigned int timeout;
    QcaAsyncPhase phase;
    IOReturn originalStatus;
    uint32_t originalBytes;
    uint32_t generation;
    uint64_t owner;
    QcaBluetooth::Action address;
    bool maskChanged;
};
static void qcaAsyncFailure(QcaAsyncRequest *context, IOReturn status, uint32_t bytes) {
    if (qcaCount(qcaAsyncFailures)) {
        qcaNote("QcaContinuityLastAsyncPhase", uint32_t(context->phase));
        qcaNote("QcaContinuityLastAsyncStatus", uint32_t(status));
        qcaNote("QcaContinuityLastAsyncBytes", bytes);
        qcaNote("QcaContinuityLastAsyncExpected", uint32_t(context->phase == QcaAsyncPhase::Original ?
            context->size : context->phase == QcaAsyncPhase::ScanAddress ? 9 : context->address.size));
    }
}
static void qcaAsyncRelease(QcaAsyncRequest *context) {
    if (OSDecrementAtomic(&context->references) != 1) return;
    if (context->mainBuffer) {
        context->mainBuffer->complete(kIODirectionOut);
        context->mainBuffer->release();
    }
    if (context->extraBuffer) {
        context->extraBuffer->complete(kIODirectionOut);
        context->extraBuffer->release();
    }
    context->provider->release();
    context->device->release();
    IOFree(context, sizeof(*context));
}
static void qcaAsyncFinish(QcaAsyncRequest *context, IOReturn status, uint32_t bytes) {
    if (!OSCompareAndSwap(0, 1, &context->finished)) return;
    const auto completion = context->original;
    if (completion.action)
        completion.action(completion.owner, completion.parameter, status, bytes);
}
static void qcaAsyncComplete(void *owner, void *, IOReturn status, uint32_t bytes);

static IOReturn qcaAsyncIssue(QcaAsyncRequest *context, QcaAsyncPhase phase,
                              const uint8_t *extra = nullptr, size_t extraSize = 0) {
    context->phase = phase;
    auto request = context->request;
    auto *buffer = context->mainBuffer;
    unsigned int timeout = context->timeout;
    if (phase != QcaAsyncPhase::Original) {
        if (context->extraBuffer) {
            context->extraBuffer->complete(kIODirectionOut);
            context->extraBuffer->release();
            context->extraBuffer = nullptr;
        }
        buffer = IOBufferMemoryDescriptor::withBytes(extra, extraSize, kIODirectionOut);
        if (!buffer) return kIOReturnNoMemory;
        const auto prepared = buffer->prepare(kIODirectionOut);
        if (prepared != kIOReturnSuccess) { buffer->release(); return prepared; }
        context->extraBuffer = buffer;
        request.wLength = uint16_t(extraSize);
        if (!timeout || timeout > 1000) timeout = 1000;
    }
    IOUSBHostCompletion completion {context, qcaAsyncComplete, nullptr};
    unsigned int transferred = 0;
    // The API copies request and completion. Hold one reference for that callback,
    // plus the invoking caller's reference in case completion races this return.
    OSIncrementAtomic(&context->references);
    const auto result = FunctionCast(CIntelBTPatcher::newHostDeviceRequest,
                                     callbackIBTPatcher->oldHostDeviceRequest)(
        context->device, context->provider, request, nullptr, buffer,
        transferred, &completion, timeout);
    if (result != kIOReturnSuccess) {
        qcaAsyncFailure(context, result, 0);
        qcaAsyncRelease(context); // no callback promised
    }
    return result;
}

static void qcaAsyncComplete(void *owner, void *, IOReturn status, uint32_t bytes) {
    auto *context = static_cast<QcaAsyncRequest *>(owner);
    qcaCount(qcaAsyncCompletions);
    const auto phase = context->phase;
    if (phase == QcaAsyncPhase::ScanAddress) {
        const bool sent = status == kIOReturnSuccess && bytes == 9;
        qcaCount(qcaScanInitCommands);
        if (!sent) qcaCount(qcaScanInitFailures);
        IOLockLock(qcaStateLock);
        if (qcaOwner == context->owner && qcaGeneration == context->generation)
            qcaState.scanInitialized = sent;
        IOLockUnlock(qcaStateLock);
        // An abort must not enqueue new I/O into a closing session.
        if (status == kIOReturnAborted || status == kIOReturnNotOpen || status == kIOReturnNoDevice) {
            qcaAsyncFinish(context, status, 0);
        } else {
            const auto result = qcaAsyncIssue(context, QcaAsyncPhase::Original);
            if (result != kIOReturnSuccess) qcaAsyncFinish(context, result, 0);
        }
    } else if (phase == QcaAsyncPhase::Original) {
        context->originalStatus = status;
        context->originalBytes = bytes;
        QcaBluetooth::Action action {};
        if (status == kIOReturnSuccess && bytes == context->size) {
            if (context->maskChanged) qcaCount(qcaAsyncMasks);
            IOLockLock(qcaStateLock);
            if (qcaOwner == context->owner) {
                const uint16_t op = QcaBluetooth::opcode(context->command);
                if (op == 0x0c03 || op == 0x203c || op == 0x203d) ++qcaGeneration;
                action = QcaBluetooth::submitted(qcaState, context->command, context->size);
                context->generation = qcaGeneration;
            }
            IOLockUnlock(qcaStateLock);
        } else qcaAsyncFailure(context, status, bytes);
        context->address = action;
        if (action.size && !context->device->isInactive() && !context->provider->isInactive()) {
            const auto result = qcaAsyncIssue(context, QcaAsyncPhase::SetAddress, action.bytes, action.size);
            if (result != kIOReturnSuccess) {
                qcaAsyncFinish(context, status, bytes);
            }
        } else qcaAsyncFinish(context, status, bytes);
    } else {
        if (status == kIOReturnSuccess && bytes == context->address.size) {
            qcaCount(qcaAsyncAddresses);
            IOLockLock(qcaStateLock);
            if (qcaOwner == context->owner && qcaGeneration == context->generation)
                QcaBluetooth::addressSubmitted(qcaState, context->address);
            IOLockUnlock(qcaStateLock);
        } else qcaAsyncFailure(context, status, bytes);
        // The extra command's completion must never replace the original transfer's
        // status or byte count. Its failure is reported only through diagnostics.
        qcaAsyncFinish(context, context->originalStatus, context->originalBytes);
    }
    qcaAsyncRelease(context);
}

static IOReturn qcaAsyncStart(void *that, IOService *provider,
                              const StandardUSB::DeviceRequest &request,
                              const uint8_t *hci, size_t size,
                              const IOUSBHostCompletion &completion, unsigned int timeout) {
    if (!provider) return kIOReturnBadArgument;
    auto *context = static_cast<QcaAsyncRequest *>(IOMalloc(sizeof(QcaAsyncRequest)));
    if (!context) return kIOReturnNoMemory;
    bzero(context, sizeof(*context));
    context->references = 1;
    context->device = static_cast<IOService *>(that);
    context->provider = provider;
    context->device->retain();
    provider->retain();
    context->original = completion;
    context->request = request;
    context->size = size;
    context->timeout = timeout;
    lilu_os_memcpy(context->command, hci, size);
    uint8_t outgoing[32] {};
    lilu_os_memcpy(outgoing, hci, size);
    context->maskChanged = QcaBluetooth::widenEventMask(outgoing, size);
    auto *buffer = IOBufferMemoryDescriptor::withBytes(outgoing, size, kIODirectionOut);
    if (!buffer) { qcaAsyncRelease(context); return kIOReturnNoMemory; }
    const auto prepared = buffer->prepare(kIODirectionOut);
    if (prepared != kIOReturnSuccess) {
        buffer->release(); qcaAsyncRelease(context); return prepared;
    }
    context->mainBuffer = buffer;
    context->owner = context->device->getRegistryEntryID();
    IOLockLock(qcaStateLock);
    if (qcaOwner != context->owner) {
        qcaState = QcaBluetooth::State{};
        qcaOwner = context->owner;
        ++qcaGeneration;
    }
    context->generation = qcaGeneration;
    const bool scan = QcaBluetooth::needsScanAddress(qcaState, hci, size);
    IOLockUnlock(qcaStateLock);
    const uint8_t fixed[] = {0x05,0x20,0x06,0x94,0x50,0x64,0xd0,0x78,0x6b};
    auto result = scan ? qcaAsyncIssue(context, QcaAsyncPhase::ScanAddress, fixed, sizeof(fixed)) :
                         qcaAsyncIssue(context, QcaAsyncPhase::Original);
    // If the preliminary shim could not even be submitted, preserve the caller's
    // original request. An accepted request always completes through our callback.
    if (scan && result != kIOReturnSuccess) {
        qcaCount(qcaScanInitFailures);
        result = qcaAsyncIssue(context, QcaAsyncPhase::Original);
    }
    qcaAsyncRelease(context);
    return result;
}
