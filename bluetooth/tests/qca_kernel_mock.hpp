#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <deque>
#include <vector>
#include <string>
using SInt32 = int32_t;
using UInt32 = uint32_t;
using IOReturn = int;
using mach_vm_address_t = uintptr_t;
constexpr int kIOReturnSuccess=0, kIOReturnNoMemory=-1, kIOReturnUnderrun=-2,
    kIOReturnBadArgument=-3, kIOReturnAborted=-4, kIOReturnNotOpen=-5,
    kIOReturnNoDevice=-6, kIOReturnError=-7, kIODirectionOut=1;
inline SInt32 OSIncrementAtomic(volatile SInt32 *p) { return __sync_fetch_and_add(p,1); }
inline SInt32 OSDecrementAtomic(volatile SInt32 *p) { return __sync_fetch_and_sub(p,1); }
inline bool OSCompareAndSwap(UInt32 old,UInt32 value,volatile UInt32 *p) { return __sync_bool_compare_and_swap(p,old,value); }
using IOLock=std::mutex;
inline IOLock *IOLockAlloc() { return new IOLock; }
inline void IOLockLock(IOLock *p) { p->lock(); }
inline void IOLockUnlock(IOLock *p) { p->unlock(); }
static int liveContexts=0, liveBuffers=0;
inline void *IOMalloc(size_t n) { ++liveContexts; return std::malloc(n); }
inline void IOFree(void *p,size_t) { --liveContexts; std::free(p); }
inline void lilu_os_memcpy(void *to,const void *from,size_t n) { std::memcpy(to,from,n); }
struct OSObject {
    int references=1;
    virtual ~OSObject() = default;
    void retain() { ++references; }
    void release() { if (--references==0) delete this; }
};
struct OSNumber: OSObject {
    uint32_t value;
    explicit OSNumber(uint32_t n):value(n) {}
    uint32_t unsigned32BitValue() const { return value; }
};
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
struct IORegistryEntry: OSObject {
    OSNumber vendor{0x0489},product{0xe10a};
    uint64_t identifier=1;
    static IORegistryEntry *getRegistryRoot() { static IORegistryEntry root; return &root; }
    OSObject *getProperty(const char *name) {
        if (!std::strcmp(name,"idVendor")) return &vendor;
        if (!std::strcmp(name,"idProduct")) return &product;
        return nullptr;
    }
    void setProperty(const char *,uint32_t,int) {}
    void setProperty(const char *,const char *) {}
    uint64_t getRegistryEntryID() const { return identifier; }
};
struct IOService: IORegistryEntry {
    bool inactive=false;
    bool isInactive() const { return inactive; }
};
struct IOMemoryDescriptor: OSObject {
    std::vector<uint8_t> memory;
    int prepares=0;
    IOReturn prepareResult=0;
    IOMemoryDescriptor(const void *p,size_t n):memory(static_cast<const uint8_t *>(p),static_cast<const uint8_t *>(p)+n) { ++liveBuffers; }
    ~IOMemoryDescriptor() override { assert(prepares==0); --liveBuffers; }
    size_t getLength() const { return memory.size(); }
    IOReturn prepare(int) { if (prepareResult) return prepareResult; ++prepares; return 0; }
    void complete(int=kIODirectionOut) { assert(prepares>0); --prepares; }
    size_t readBytes(size_t offset,void *to,size_t n) { assert(prepares>0 && offset+n<=memory.size()); std::memcpy(to,memory.data()+offset,n); return n; }
};
struct IOBufferMemoryDescriptor: IOMemoryDescriptor {
    using IOMemoryDescriptor::IOMemoryDescriptor;
    static bool failAllocation;
    static IOBufferMemoryDescriptor *withBytes(const void *p,size_t n,int) {
        if (failAllocation) { failAllocation=false; return nullptr; }
        return new IOBufferMemoryDescriptor(p,n);
    }
    void *getBytesNoCopy(size_t offset=0,size_t n=0) { assert(offset+n<=memory.size()); return memory.data()+offset; }
};
bool IOBufferMemoryDescriptor::failAllocation=false;
namespace StandardUSB { struct DeviceRequest {
    uint8_t bmRequestType=0x20,bRequest=0;
    uint16_t wValue=0,wIndex=0,wLength=0;
}; }
struct IOUSBHostCompletion {
    void *owner;
    void (*action)(void *,void *,IOReturn,uint32_t);
    void *parameter;
};
using OriginalRequest=IOReturn(*)(void *,IOService *,StandardUSB::DeviceRequest &,void *,IOMemoryDescriptor *,unsigned int &,IOUSBHostCompletion *,unsigned int);
struct CIntelBTPatcher { OriginalRequest oldHostDeviceRequest; };
static CIntelBTPatcher mockPatcher;
static CIntelBTPatcher *callbackIBTPatcher=&mockPatcher;
#define FunctionCast(function, address) (address)
enum class KernelVersion { Tahoe=25 };
inline KernelVersion getKernelVersion() { return KernelVersion::Tahoe; }
inline bool checkKernelArgument(const char *arg) { return !std::strcmp(arg,"-qcaadv"); }
struct KernelPatcher {
    static constexpr size_t KernelID=0;
    uintptr_t solveSymbol(size_t,const char *) { return reinterpret_cast<uintptr_t>("25G342"); }
    void clearError() {}
};
extern "C" const char version[] = "Darwin Kernel Version 25.6.0: date; root:xnu-12377.161.15.701.48~2/RELEASE_X86_64";

struct MockTransfer { IOMemoryDescriptor *buffer; IOUSBHostCompletion completion; };
static std::deque<MockTransfer> pending;
static std::vector<std::vector<uint8_t>> submissions;
static uint16_t rejectOpcode=0;
static bool inlineCompletion=false;
static void drainOne(IOReturn status=0,int byteOverride=-1) {
    assert(!pending.empty());
    auto transfer=pending.front(); pending.pop_front();
    auto bytes=byteOverride<0 ? uint32_t(transfer.buffer->getLength()) : uint32_t(byteOverride);
    transfer.completion.action(transfer.completion.owner,transfer.completion.parameter,status,bytes);
    transfer.buffer->release();
}
static IOReturn mockUsb(void *,IOService *,StandardUSB::DeviceRequest &request,void *raw,
                         IOMemoryDescriptor *descriptor,unsigned int &length,
                         IOUSBHostCompletion *completion,unsigned int) {
    std::vector<uint8_t> bytes;
    if (raw) bytes.assign(static_cast<uint8_t *>(raw),static_cast<uint8_t *>(raw)+request.wLength);
    else { assert(descriptor && descriptor->prepares>0); bytes=descriptor->memory; }
    assert(bytes.size()==request.wLength);
    const uint16_t op=uint16_t(bytes[0])|(uint16_t(bytes[1])<<8);
    if (rejectOpcode==op) return kIOReturnError;
    submissions.push_back(bytes);
    if (completion) {
        descriptor->retain();
        pending.push_back({descriptor,*completion});
        if (inlineCompletion) drainOne();
    } else length=unsigned(bytes.size());
    return 0;
}
