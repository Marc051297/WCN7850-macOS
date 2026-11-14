// SPDX-License-Identifier: GPL-3.0-only
// Included by the existing BlueToolFixup validation hook. No second kext/hook
// for cs_validate_page. Native code signing and APFS verification still run.
#include "QcaValidatedPages.hpp"
#include "QcaVerifyLayout.hpp"
#include "QcaPeerAddress25G342.hpp"
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include <sys/vnode.h>

namespace {
using QcaNativeValidate = void (*)(vnode_t, memory_object_t, memory_object_offset_t,
                                   const void *, int *, int *, int *);
using QcaNativeVerifier = int (*)(void *);
// XNU VNOP_VERIFY ABI (bsd/sys/vnode_if.h), independently named to avoid the
// older SDK's missing declaration. Layout is checked for this x86_64 build.
struct QcaVerifyArguments {
    void *descriptor;
    vnode_t vnode;
    int64_t fileOffset;
    uint8_t *buffer;
    size_t size;
    size_t *blockSize;
    void **context;
    uint32_t flags;
    vfs_context_t vfsContext;
    void *kind;
};
static_assert(sizeof(QcaVerifyArguments) == 0x50, "VNOP_VERIFY argument size");
static_assert(offsetof(QcaVerifyArguments, buffer) == 0x18 &&
              offsetof(QcaVerifyArguments, flags) == 0x38, "VNOP_VERIFY layout");
static QcaBluetooth::ValidatedPages<32> qcaPageRecords;
static IOLock *qcaPageLock, *qcaVerifyLock;
static uint8_t qcaPageBefore[4096];
static constexpr size_t qcaVerifyCapacity = 128 * 1024;
static uint8_t *qcaVerifyCopy;
static vnode_t qcaGuardVnode;
static QcaNativeVerifier qcaNativeVerifier;
static mach_vm_address_t qcaVerifyDescriptor;
static uint32_t qcaVnodeVectorOffset;
static bool qcaGuardRequested;
static thread_call_t qcaPageReport;
static volatile SInt32 qcaVerifierState, qcaPageRestores, qcaVerifyRestores,
    qcaVerifyFailures, qcaPageRejected, qcaPageRecordsFull, qcaRpaMask;

static void qcaPublishPages(thread_call_param_t, thread_call_param_t) {
    auto *root = IORegistryEntry::getRegistryRoot();
    root->setProperty("QcaContinuityPageRevision", "experimental.7-peer-address");
    root->setProperty("QcaContinuityVerifierState", uint32_t(qcaVerifierState), 32);
    root->setProperty("QcaContinuityRpaPatchMask", uint32_t(qcaRpaMask), 32);
    root->setProperty("QcaContinuityPageRestores", uint32_t(qcaPageRestores), 32);
    root->setProperty("QcaContinuityVerifyRestores", uint32_t(qcaVerifyRestores), 32);
    root->setProperty("QcaContinuityVerifyFailures", uint32_t(qcaVerifyFailures), 32);
    root->setProperty("QcaContinuityPageRejected", uint32_t(qcaPageRejected), 32);
    root->setProperty("QcaContinuityPageRecordsFull", uint32_t(qcaPageRecordsFull), 32);
}
static void qcaReportPagesLater() {
    if (qcaPageReport) thread_call_enter(qcaPageReport);
}

static int qcaVerifyFileBlock(void *raw) {
    auto *args = static_cast<QcaVerifyArguments *>(raw);
    auto native = __atomic_load_n(&qcaNativeVerifier, __ATOMIC_ACQUIRE);
    const vnode_t known = __atomic_load_n(&qcaGuardVnode, __ATOMIC_ACQUIRE);
    if (!args || !known || args->vnode != known || args->flags != 0 ||
        !args->buffer || !args->size || args->fileOffset < 0 ||
        args->size > qcaVerifyCapacity)
        return native(raw);

    // Lock order is always scratch then pages. Page validation never needs
    // scratch, so a verifier does not wait for scratch while holding pages.
    IOLockLock(qcaVerifyLock);
    IOLockLock(qcaPageLock);
    memcpy(qcaVerifyCopy, args->buffer, args->size);
    const size_t restored = qcaPageRecords.restore(reinterpret_cast<uintptr_t>(known),
        uint64_t(args->fileOffset), qcaVerifyCopy, args->size);
    int result;
    if (restored) {
        IOLockUnlock(qcaPageLock);
        uint8_t *shared = args->buffer;
        args->buffer = qcaVerifyCopy;
        result = native(raw);
        args->buffer = shared;
        OSIncrementAtomic(&qcaVerifyRestores);
    } else {
        // Keep our modifications out of the shared buffer until its native
        // hash completes; no restored private copy exists in this case.
        result = native(raw);
        IOLockUnlock(qcaPageLock);
    }
    IOLockUnlock(qcaVerifyLock);
    if (result) OSIncrementAtomic(&qcaVerifyFailures);
    if (restored || result) qcaReportPagesLater();
    return result;
}

// Called with qcaPageLock. The operation-vector entry, rather than just the
// exported trampoline, also covers callers that inline VNOP_VERIFY.
static bool qcaInstallVerifier(vnode_t vp) {
    if (qcaVerifierState) return qcaVerifierState == 1;
    qcaVerifierState = 2;
    if (!qcaVerifyDescriptor || !qcaVnodeVectorOffset) return false;
    const int index = *reinterpret_cast<const volatile int *>(qcaVerifyDescriptor);
    if (index <= 0 || index >= 1024) { qcaVerifierState = 3; return false; }
    auto fields = reinterpret_cast<void *const *>(reinterpret_cast<uintptr_t>(vp) + qcaVnodeVectorOffset);
    if (!fields[0] || fields[1] != static_cast<void *>(vnode_mount(vp)) ||
        fields[2] != vnode_fsnode(vp)) { qcaVerifierState = 4; return false; }
    auto slot = static_cast<QcaNativeVerifier *>(fields[0]) + index;
    auto old = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (!old || old == qcaVerifyFileBlock || vnode_ref(vp) != 0) {
        qcaVerifierState = 5; return false;
    }
    const bool writable = MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS;
    if (!writable) {
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        vnode_rele(vp);
        qcaVerifierState = 6;
        return false;
    }
    // The plugin is non-unloadable. Keep a vnode reference so saved offsets
    // can never be applied to a recycled vnode belonging to a different file.
    qcaPageRecords.bind(reinterpret_cast<uintptr_t>(vp));
    __atomic_store_n(&qcaNativeVerifier, old, __ATOMIC_RELEASE);
    __atomic_store_n(&qcaGuardVnode, vp, __ATOMIC_RELEASE);
    __atomic_store_n(slot, &qcaVerifyFileBlock, __ATOMIC_RELEASE);
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
    qcaVerifierState = 1;
    return true;
}

static void qcaApplyPeerPage(uint64_t pageOffset, uint8_t *page) {
    // Thin-image file offsets corresponding to the seven checked signatures.
    static const uint32_t offsets[] = {0xaa2ba0,0x20c3dd,0x2234c3,0x729844,0x717012,0x7140d3,0x6d11ea};
    for (size_t n = 0; n < arrsize(QcaPeerPatches::sites); ++n) {
        const auto &site = QcaPeerPatches::sites[n];
        for (size_t i = 0; i + site.size <= 4096; ++i) {
            if (memcmp(page+i, site.find, site.size)) continue;
            if (pageOffset + i < offsets[n]) { OSIncrementAtomic(&qcaPageRejected); return; }
            const uint64_t slice = pageOffset + i - offsets[n];
            // Hold the helper page's ledger slot before redirecting any call.
            // Fat Mach-O slices on this build start on a 4 KiB boundary.
            if (slice % 4096 || slice > UINT64_MAX - 0xaa3000 ||
                !qcaPageRecords.hold(reinterpret_cast<uintptr_t>(qcaGuardVnode), pageOffset) ||
                !qcaPageRecords.hold(reinterpret_cast<uintptr_t>(qcaGuardVnode), slice+0xaa2000)) {
                OSIncrementAtomic(&qcaPageRecordsFull);
                return;
            }
            memcpy(page+i, site.replace, site.size);
            OSBitOrAtomic(uint32_t(1) << n, reinterpret_cast<volatile UInt32 *>(&qcaRpaMask));
            break;
        }
    }
}

static bool qcaGuardValidate(vnode_t vp, memory_object_t pager, memory_object_offset_t offset,
    const void *data, int *validated, int *tainted, int *nx, QcaNativeValidate native,
    void (*patchExisting)(const void *, const char *), const char *path) {
    if (!qcaGuardRequested || !data || !validated || !tainted || !qcaOsBuild ||
        strcmp(qcaOsBuild, "25G342") ||
        !QcaBluetooth::allowed(QcaBluetooth::buildGate(version, qcaOsBuild))) return false;
    auto *hook = OSDynamicCast(OSNumber, IORegistryEntry::getRegistryRoot()->getProperty("QcaContinuityHciHookRouted"));
    if (!hook || hook->unsigned32BitValue() != 1) return false;
    IOLockLock(qcaPageLock);
    if (!qcaInstallVerifier(vp) || vp != qcaGuardVnode) {
        IOLockUnlock(qcaPageLock);
        qcaReportPagesLater();
        return false; // retain the old BlueToolFixup path; no peer patches
    }
    const uintptr_t token = reinterpret_cast<uintptr_t>(vp);
    auto *page = const_cast<uint8_t *>(static_cast<const uint8_t *>(data));
    if (qcaPageRecords.restore(token, offset, page, 4096)) OSIncrementAtomic(&qcaPageRestores);
    // Unmodified executable pages do not consume ledger capacity. The peer
    // patcher holds its caller slot before reserving the helper slot.
    const bool room = qcaPageRecords.reserve(token, offset) >= 0;
    const bool sameOriginal = qcaPageRecords.originalMatches(token, offset, page);
    memcpy(qcaPageBefore, page, 4096);
    native(vp, pager, offset, data, validated, tainted, nx);
    if (room && sameOriginal && *validated && !*tainted) {
        patchExisting(data, path);
        qcaApplyPeerPage(offset, page);
        if (memcmp(qcaPageBefore, page, 4096) &&
            !qcaPageRecords.record(token, offset, qcaPageBefore, page)) {
            memcpy(page, qcaPageBefore, 4096);
            OSIncrementAtomic(&qcaPageRejected);
        }
    } else {
        if (!room) OSIncrementAtomic(&qcaPageRecordsFull);
        else OSIncrementAtomic(&qcaPageRejected);
    }
    IOLockUnlock(qcaPageLock);
    qcaReportPagesLater();
    return true;
}

static void qcaSetupPageGuard(KernelPatcher &patcher) {
    if (!shouldPatchQcaContinuity || checkKernelArgument("-qcarpaoff")) return;
    qcaVerifyDescriptor = patcher.solveSymbol(KernelPatcher::KernelID, "_vnop_verify_desc");
    patcher.clearError();
    const auto fn = patcher.solveSymbol(KernelPatcher::KernelID, "_VNOP_VERIFY");
    patcher.clearError();
    if (fn && qcaVerifyDescriptor)
        qcaVnodeVectorOffset = QcaBluetooth::verifierVectorOffset(
            reinterpret_cast<const uint8_t *>(fn), 256, fn, qcaVerifyDescriptor);
    qcaPageLock = IOLockAlloc();
    qcaVerifyLock = IOLockAlloc();
    qcaVerifyCopy = static_cast<uint8_t *>(IOMalloc(qcaVerifyCapacity));
    qcaPageReport = thread_call_allocate(qcaPublishPages, nullptr);
    qcaGuardRequested = qcaPageLock && qcaVerifyLock && qcaVerifyCopy &&
        qcaPageReport && qcaVnodeVectorOffset && qcaVerifyDescriptor;
    if (!qcaGuardRequested) qcaVerifierState = 2;
    qcaReportPagesLater();
}
}
