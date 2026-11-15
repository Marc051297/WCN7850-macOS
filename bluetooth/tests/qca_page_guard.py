#!/usr/bin/env python3
"""Compile the real page/verification callbacks against a concurrent host harness."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'compat/QcaPageGuard.inc').read_text()


def function(name):
    import re
    match = re.search(r'^static [^\n]*\b'+name+r'\(', source, re.M)
    start = match.start()
    first = source.index('{', start)
    depth = 1
    pos = first+1
    while depth:
        depth += (source[pos]=='{') - (source[pos]=='}')
        pos += 1
    return source[start:pos]


prelude = r'''
#include "QcaValidatedPages.hpp"
#include "QcaPeerAddress25G342.hpp"
#include "QcaBluetoothHci.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <thread>
#include <map>
using vnode_t=void *;
using memory_object_t=void *;
using memory_object_offset_t=uint64_t;
using UInt32=uint32_t;
using SInt32=int32_t;
using IOLock=std::mutex;
static void IOLockLock(IOLock*p){p->lock();}
static void IOLockUnlock(IOLock*p){p->unlock();}
static void OSIncrementAtomic(volatile SInt32*p){__atomic_add_fetch(p,1,__ATOMIC_RELAXED);}
static void OSBitOrAtomic(UInt32 n,volatile UInt32*p){__atomic_fetch_or(p,n,__ATOMIC_RELAXED);}
#define arrsize(a) (sizeof(a)/sizeof((a)[0]))
#define OSDynamicCast(T,p) static_cast<T*>(p)
struct OSNumber {uint32_t unsigned32BitValue(){return 1;}};
struct IORegistryEntry {
 static IORegistryEntry*getRegistryRoot(){static IORegistryEntry r;return &r;}
 OSNumber*getProperty(const char*){static OSNumber n;return &n;}
};
using QcaNativeValidate=void(*)(vnode_t,void*,uint64_t,const void*,int*,int*,int*);
using QcaNativeVerifier=int(*)(void*);
struct QcaVerifyArguments {void*descriptor;vnode_t vnode;int64_t fileOffset;uint8_t*buffer;
 size_t size;size_t*blockSize;void**context;uint32_t flags;void*vfsContext;void*kind;};
static QcaBluetooth::ValidatedPages<32> qcaPageRecords;
static IOLock pagesLock,verifyLock;
static IOLock *qcaPageLock=&pagesLock,*qcaVerifyLock=&verifyLock;
static uint8_t qcaPageBefore[4096];
static constexpr size_t qcaVerifyCapacity=128*1024;
static uint8_t scratch[qcaVerifyCapacity];
static uint8_t*qcaVerifyCopy=scratch;
static vnode_t qcaGuardVnode=reinterpret_cast<void*>(1);
static QcaNativeVerifier qcaNativeVerifier;
static bool qcaGuardRequested=true;
static const char*qcaOsBuild="25G342";
static const char*version="xnu-12377.161.15.701.48~2/RELEASE_X86_64";
static volatile SInt32 qcaPageRestores,qcaVerifyRestores,qcaVerifyFailures,
 qcaPageRejected,qcaPageRecordsFull,qcaRpaMask;
static uint64_t qcaLastRejectedOffset;
static uint32_t qcaLastRejectedReason,qcaLastRejectedFlags;
static void qcaReportPagesLater(){}
static bool qcaInstallVerifier(vnode_t){return true;}
using Page=std::array<uint8_t,4096>;
static std::map<uint64_t,Page> originals;
static std::atomic<int> nativeChecks{0},fileChecks{0};
static bool rejectPage=false,forceTaint=false;
static int rejectedTaint=1;
static void nativeValidate(vnode_t,void*,uint64_t offset,const void*data,int*v,int*t,int*n){
 ++nativeChecks;
 bool good=originals.count(offset) && !memcmp(data,originals.at(offset).data(),4096) && !rejectPage;
 *v=good;*t=good ? forceTaint : rejectedTaint;*n=0;
}
static void existingPatch(const void*,const char*){}
static std::array<uint8_t,16384> originalBlock;
static int nativeVerify(void*raw){
 auto*a=static_cast<QcaVerifyArguments*>(raw);++fileChecks;
 if(a->flags)return 71;
 if(a->size!=originalBlock.size())return 72;
 return memcmp(a->buffer,originalBlock.data(),a->size)?83:0;
}
'''
main = r'''
int main(){
 assert(qcaPageRecords.bind(1));qcaNativeVerifier=nativeVerify;
 auto validate=[](uint64_t off,uint8_t*data){
  int v=0,t=0,n=0;
  bool handled=qcaGuardValidate(qcaGuardVnode,nullptr,off,data,&v,&t,&n,
    nativeValidate,existingPatch,"/usr/sbin/bluetoothd");
  assert(handled);return v && !t;
 };
 // A caller page arrives before its helper. Native, unmodified pages must not
 // fill the ledger and deprive that later helper page of its reserved slot.
 const auto&legacy=QcaPeerPatches::sites[1];
 Page caller{};memcpy(caller.data()+0x3dd,legacy.find,legacy.size);
 originals[0x20c000]=caller;
 assert(validate(0x20c000,caller.data()));
 assert(!memcmp(caller.data()+0x3dd,legacy.replace,legacy.size));
 for(unsigned i=0;i<500;++i){
  Page unchanged{};uint64_t off=0x400000+uint64_t(i)*4096;
  originals[off]=unchanged;assert(validate(off,unchanged.data()));
 }
 const auto&helpers=QcaPeerPatches::sites[0];
 Page helper{};memcpy(helper.data()+0xba0,helpers.find,helpers.size);
 originals[0xaa2000]=helper;
 assert(validate(0xaa2000,helper.data()));
 assert(!memcmp(helper.data()+0xba0,helpers.replace,helpers.size));
 const Page patched=helper;
 for(int i=0;i<10;++i){assert(validate(0xaa2000,helper.data()));assert(helper==patched);}
 assert(qcaPageRestores>=10 && qcaPageRecordsFull==0);
 // The native filesystem verifier gets the original 16 KiB contents while
 // its shared buffer remains patched. It must see unrelated corruption.
 memcpy(originalBlock.data()+8192,originals.at(0xaa2000).data(),4096);
 auto shared=originalBlock;memcpy(shared.data()+8192,helper.data(),4096);
 const auto intact=shared;
 QcaVerifyArguments args{};args.vnode=qcaGuardVnode;args.fileOffset=0xaa0000;
 args.buffer=shared.data();args.size=shared.size();
 assert(qcaVerifyFileBlock(&args)==0 && shared==intact && args.buffer==shared.data());
 shared[100]^=1;
 assert(qcaVerifyFileBlock(&args)==83 && shared[100]==1);
 shared=intact;
 args.flags=1;assert(qcaVerifyFileBlock(&args)==71);args.flags=0;
 args.vnode=reinterpret_cast<void*>(2);assert(qcaVerifyFileBlock(&args)==83);args.vnode=qcaGuardVnode;
 // Concurrent verification and refaulting of the same shared helper page.
 std::thread validator([&]{for(int i=0;i<100;++i)assert(validate(0xaa2000,shared.data()+8192));});
 std::thread verifier([&]{for(int i=0;i<100;++i){auto a=args;assert(qcaVerifyFileBlock(&a)==0);}});
 validator.join();verifier.join();assert(shared==intact);
 // A corrupt code page must not receive a newly trusted snapshot or patches.
 helper=originals.at(0xaa2000);helper[50]^=1;
 assert(!validate(0xaa2000,helper.data()));assert(helper[50]==1);
 assert(qcaLastRejectedReason==3 && qcaLastRejectedOffset==0xaa2000);
 assert(memcmp(helper.data()+0xba0,helpers.replace,helpers.size));
 // Native authentication failure is propagated even for pristine bytes.
 helper=originals.at(0xaa2000);rejectPage=true;
 assert(!validate(0xaa2000,helper.data()));assert(helper==originals.at(0xaa2000));
 assert(qcaLastRejectedReason==4 && qcaLastRejectedFlags==2);
 rejectedTaint=0;
 assert(!validate(0xaa2000,helper.data()));assert(qcaLastRejectedReason==4 && qcaLastRejectedFlags==0);
 rejectPage=false;forceTaint=true;
 assert(!validate(0xaa2000,helper.data()));assert(qcaLastRejectedReason==5 && qcaLastRejectedFlags==3);
 assert(qcaPageRejected>=2 && qcaVerifyFailures>=1);
 puts("PASS: actual page callbacks, helper reservation, 500 untouched pages, native failures, corruption and concurrent refault/verification");
}
'''

with tempfile.TemporaryDirectory(prefix='qca-guard-test-') as tmp:
    cpp=Path(tmp)/'test.cpp';exe=Path(tmp)/'test'
    cpp.write_text(prelude+'\n'+function('qcaRejectPage')+'\n'+function('qcaVerifyFileBlock')+'\n'+
                   function('qcaApplyPeerPage')+'\n'+function('qcaGuardValidate')+'\n'+main)
    subprocess.run(['xcrun','clang++','-std=c++14','-Wall','-Wextra','-Werror',
                    '-Wno-unused-function','-fsanitize=address,undefined','-pthread',
                    '-I',str(ROOT/'compat'),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
