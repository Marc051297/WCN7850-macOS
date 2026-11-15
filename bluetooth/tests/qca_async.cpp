#include "qca_kernel_mock.hpp"
#define QCABT_HOST_TEST 1
#include "../compat/QcaBluetoothHooks.inc"
#include <cstdio>
struct Result {
    int calls=0;
    int status=99;
    uint32_t bytes=0;
    void *parameter=reinterpret_cast<void *>(0x1234);
    static void complete(void *owner,void *parameter,IOReturn status,uint32_t bytes) {
        auto *r=static_cast<Result *>(owner);
        assert(parameter==r->parameter);
        ++r->calls; r->status=status; r->bytes=bytes;
    }
};
static IOService device,provider;
static IOReturn start(const std::vector<uint8_t> &bytes,Result &result) {
    auto *input=IOBufferMemoryDescriptor::withBytes(bytes.data(),bytes.size(),kIODirectionOut);
    StandardUSB::DeviceRequest request; request.wLength=uint16_t(bytes.size());
    IOUSBHostCompletion completion{&result,Result::complete,result.parameter};
    unsigned int length=999;
    auto ret=qcaRequest(&device,&provider,request,nullptr,input,length,&completion,0);
    assert(length==999); // never clobber the asynchronous caller's output reference
    input->release(); // own copy must survive until the native completion
    return ret;
}
static void idle() {
    assert(pending.empty() && liveContexts==0 && liveBuffers==0);
    assert(device.references==1 && provider.references==1);
}
int main() {
    mockPatcher.oldHostDeviceRequest=mockUsb;
    KernelPatcher patcher; qcaInit(patcher);
    Result mask;
    assert(start({1,0x20,8,0x1f,0,0,0,0,0,0,0},mask)==0);
    assert(mask.calls==0 && submissions.back()[3]==0x7f && submissions.back()[4]==0x1a);
    drainOne(); assert(mask.calls==1 && mask.status==0 && mask.bytes==11); idle();
    Result general;
    assert(start({5,0x20,6,1,2,3,4,5,0xc6},general)==0);
    drainOne(); assert(qcaState.haveGeneral); idle();
    std::vector<uint8_t> params(28); params[0]=0x36; params[1]=0x20; params[2]=25; params[3]=3; params[13]=1;
    Result advertising;
    assert(start(params,advertising)==0);
    drainOne(); // original params complete; now the random address is enqueued
    assert(advertising.calls==0 && pending.size()==1);
    assert(submissions.back()[0]==0x35 && submissions.back()[3]==3 && submissions.back()[4]==1);
    drainOne(); assert(advertising.calls==1 && advertising.bytes==28 && (qcaState.addressedSets&8)); idle();
    qcaState.addressedSets=0;
    Result extraFailure;
    assert(start(params,extraFailure)==0); drainOne(); drainOne(kIOReturnError,0);
    assert(extraFailure.calls==1 && extraFailure.status==0 && extraFailure.bytes==28 && !(qcaState.addressedSets&8)); idle();
    rejectOpcode=0x2035;
    Result extraRejected;
    assert(start(params,extraRejected)==0); drainOne();
    assert(extraRejected.calls==1 && extraRejected.status==0 && extraRejected.bytes==28); idle();
    rejectOpcode=0x2036;
    Result originalRejected;
    assert(start(params,originalRejected)==kIOReturnError && originalRejected.calls==0); idle();
    rejectOpcode=0;
    Result originalFailure;
    assert(start(params,originalFailure)==0); drainOne(kIOReturnError,0);
    assert(originalFailure.calls==1 && originalFailure.status==kIOReturnError && originalFailure.bytes==0); idle();
    Result shortOriginal;
    assert(start(params,shortOriginal)==0); drainOne(0,7);
    assert(shortOriginal.calls==1 && shortOriginal.status==0 && shortOriginal.bytes==7); idle();
    IOBufferMemoryDescriptor::failAllocation=true;
    Result noMemory;
    StandardUSB::DeviceRequest allocationRequest; allocationRequest.wLength=28;
    IOUSBHostCompletion allocationCompletion{&noMemory,Result::complete,noMemory.parameter};
    assert(qcaAsyncStart(&device,&provider,allocationRequest,params.data(),params.size(),allocationCompletion,0)==kIOReturnNoMemory);
    assert(noMemory.calls==0); idle();
    qcaState.scanInitialized=false;
    Result scan;
    std::vector<uint8_t> scanCommand={0x0b,0x20,7,0,0,0,0,0,0,0};
    assert(start(scanCommand,scan)==0 && submissions.back()[0]==5);
    drainOne(); assert(scan.calls==0 && submissions.back()[0]==0x0b);
    drainOne(); assert(scan.calls==1 && scan.bytes==10 && qcaState.scanInitialized); idle();
    qcaState.scanInitialized=false;
    Result aborted;
    assert(start(scanCommand,aborted)==0); drainOne(kIOReturnAborted,0);
    assert(aborted.calls==1 && aborted.status==kIOReturnAborted && pending.empty()); idle();
    qcaState.addressedSets=0;
    inlineCompletion=true;
    Result inlineResult;
    assert(start(params,inlineResult)==0);
    assert(inlineResult.calls==1 && inlineResult.bytes==28); idle();
    inlineCompletion=false;
    Result stale;
    qcaState.addressedSets=0;
    assert(start(params,stale)==0); drainOne();
    ++qcaGeneration; qcaState=QcaBluetooth::State{};
    drainOne(); assert(stale.calls==1 && !qcaState.addressedSets); idle();
    delete qcaStateLock;
    std::puts("PASS: actual async hook, owned buffers, callback identity/count, ordering, failures, aborts, inline completion and stale cache");
}
