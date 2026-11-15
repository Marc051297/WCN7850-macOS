#!/usr/bin/env python3
"""Exercise the real radio pump/sync branches against a limited-credit sender."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "airport/awdlnative/QwzAWDLRadio.cpp").read_text()


def block(marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


pump = block("while (budget > 0")
sync = block("if (qwz_mon_tx_room() > 0")
prefix = r'''
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
using IOReturn=int;
constexpr int kIOReturnSuccess=0, kIOReturnNotReady=1;
struct Packet { uint8_t byte; };
using mbuf_t=Packet *;
static int credits=2, reserve=1, ready=1, sent, freed, dequeued, roomChecks;
static int closeAfterSend, syncResult, syncCalls;
static bool peerHeld=false;
// RS (f2u): the cell's list of frames waiting for another try, in miniature. `dest` is the receiver (first address
// byte); `result` is what the next send of that frame answers.
static int qwz_nativepeerpause=1;
struct Waiting { uint8_t dest; int result; };
static std::vector<Waiting> waitingFrames;
static std::vector<uint8_t> resentTo;
static uint8_t heldDest=0xff;
static unsigned resendCalls=0;
static int cell=149,autoTicks,services;
static uint64_t mockNow=1000000;
static uint64_t nowUS() {return mockNow;}
static void qwz_mon_native_auto_tick() {++autoTicks;}
static int qwz_mon_status() {return cell;}
static bool qwz_mon_native_tx_ready() { return ready; }
static int qwz_mon_tx_room() { ++roomChecks; return credits>reserve ? 60 : 0; }
static void *mbuf_data(mbuf_t p) { return p; }
static size_t mbuf_len(mbuf_t) { return 1; }
static void mbuf_freem(mbuf_t p) { ++freed; delete p; }
static int qwz_mon_native_resend_first(unsigned from,uint8_t *dest) {
    for(unsigned i=from;i<waitingFrames.size();++i) {
        bool earlier=false;
        for(unsigned k=0;k<i;++k) earlier|=waitingFrames[k].dest==waitingFrames[i].dest;
        if(earlier) continue;
        memset(dest,0,6);dest[0]=waitingFrames[i].dest;
        return int(i);
    }
    return -1;
}
static int qwz_mon_native_resend_send(unsigned index) {
    assert(index<waitingFrames.size() && ++resendCalls<1000); // every pass must make progress
    const int result=waitingFrames[index].result;
    if(result==EAGAIN || result==ENOSPC) return result;        // the frame keeps waiting
    if(result==0) {
        if(credits<=reserve) return ENOSPC;
        --credits;resentTo.push_back(waitingFrames[index].dest);
    }
    waitingFrames.erase(waitingFrames.begin()+index);          // sent again, or given up
    return result;
}
static int qwz_mon_tx_raw(const uint8_t *,size_t) {
    if(credits<=reserve) return ENOSPC;
    --credits; ++sent;
    if(closeAfterSend) ready=0;
    return 0;
}
struct Stats {
    uint64_t dataSent=0,dataTxErrors=0,syncSent=0,syncTxErrors=0,syncBuildFailures=0;
    uint64_t txSubmittedBytes=0,dataResent=0;
    uint64_t cellTimerTicks=0,queued=0;
    uint32_t dataLastTxError=28,syncLastTxError=28,syncLastBuild=1;
};
struct MockSelf {
    Stats awdlNativeStats;
    uint64_t awdlNativeLastDataUS=0,awdlNativeLastSyncUS=0;
    int queued=40;
    bool awdlInterfaceTest=true,awdlManagementInputEnabled=true;
    void *fAWDLInterface=this;
    bool awdlNativePeerTimingEnabled=true;
    int awdlNativeTimingCell=0;
    struct Timing {
        bool hold(const uint8_t *d,uint8_t,uint64_t) {return peerHeld || d[0]==heldDest;}
        void clearPeers() {}
    } awdlNativePeerTiming;
    bool copyAWDLTransmitDestination(uint8_t *a) {a[0]=2;return queued>0;}
    bool serviceAWDLTransmit() {++services;return queued>0;}
    unsigned queuedAWDLTransmit() {return queued;}
    IOReturn buildAWDLTransmit(mbuf_t *p,const uint8_t * = nullptr,bool = false,uint64_t = 0) {
        if(peerHeld && awdlNativePeerTimingEnabled) return kIOReturnNotReady;
        if(!queued) return kIOReturnNotReady;
        --queued; ++dequeued; *p=new Packet{}; return kIOReturnSuccess;
    }
    IOReturn buildAWDLSyncProbe(mbuf_t *p,uint64_t) {
        ++syncCalls;
        if(syncResult==0) *p=new Packet{};
        return syncResult;
    }
};
namespace ItlAWDL { constexpr uint64_t kSyncIdleIntervalUS=45000; }
constexpr uint64_t kSyncDataIntervalUS=100000;
constexpr uint64_t kDataActiveUS=1000000;
constexpr unsigned kPumpBudget=16;
'''
code = prefix + "\nstatic void pump(MockSelf *self,uint64_t now) { auto &st=self->awdlNativeStats; unsigned budget=16;\n" + pump + "\n}\n"
code += "static void syncFrame(MockSelf *self,uint64_t now) { auto &st=self->awdlNativeStats; bool dataActive=false; uint64_t sinceSync=now-self->awdlNativeLastSyncUS;\n" + sync + "\n}\n"
tick_start = source.index("const bool ready = self->awdlInterfaceTest")
tick_end = source.index("\n\tif (now - self->awdlNativeLastPublishUS", tick_start)
code += "static bool tick(MockSelf *self) { auto &st=self->awdlNativeStats;\n" + source[tick_start:tick_end] + "\nreturn pending;\n}\n"
code += r'''
int main() {
    MockSelf self;
    // Wi-Fi join/hotspot pause removes the cell but must not strand native
    // packet completions or dequeue resumption behind the radio condition.
    cell=0;
    assert(tick(&self) && services==1 && autoTicks==1 && !dequeued && !syncCalls);
    self.awdlManagementInputEnabled=false;
    assert(!tick(&self) && services==1 && autoTicks==1);
    self.awdlManagementInputEnabled=true;cell=149;
    // One available credit with many free slots: dequeue only one packet.
    pump(&self,100);
    assert(sent==1 && dequeued==1 && freed==1 && self.queued==39);
    assert(!self.awdlNativeStats.dataTxErrors && credits==reserve);
    assert(self.awdlNativeStats.dataLastTxError==0);
    pump(&self,200);
    assert(dequeued==1 && self.queued==39); // no dequeue while waiting
    credits=3;
    pump(&self,300);
    assert(sent==3 && dequeued==3 && freed==3 && self.queued==37);
    credits=100;
    pump(&self,400);
    assert(sent==19 && self.queued==21); // per-tick budget still applies
    closeAfterSend=1;
    pump(&self,500);
    assert(sent==20 && self.queued==20 && !ready);
    pump(&self,600);
    assert(sent==20 && self.queued==20);
    assert(!self.awdlNativeStats.dataTxErrors && dequeued==freed);
    assert(self.awdlNativeStats.txSubmittedBytes==self.awdlNativeStats.dataSent);
    // Sync's credit is spent before the data pump checks room.
    ready=1; closeAfterSend=0; credits=reserve;
    syncFrame(&self,100000);
    assert(!syncCalls && !self.awdlNativeLastSyncUS);
    ready=1; closeAfterSend=0; credits=2;
    syncFrame(&self,100000);
    const int before=dequeued;
    pump(&self,100000);
    assert(dequeued==before && credits==reserve);
    assert(self.awdlNativeStats.syncLastBuild==0 && self.awdlNativeStats.syncLastTxError==0);
    syncResult=kIOReturnNotReady; credits=2;
    syncFrame(&self,200000);
    assert(self.awdlNativeStats.syncLastBuild==kIOReturnNotReady);
    syncResult=0;
    syncFrame(&self,300000);
    assert(self.awdlNativeStats.syncLastBuild==0);
    assert(self.awdlNativeStats.syncBuildFailures==1); // historical counter
    assert(!self.awdlNativeStats.dataTxErrors);
    assert(roomChecks>0);
    // Holding a calibrated peer leaves FIFO ownership intact. Completion
    // service runs even when no packet may be removed for that peer.
    credits=100;peerHeld=true;
    const int queued=self.queued,wasDequeued=dequeued,wasServiced=services;
    self.awdlNativeLastSyncUS=mockNow;
    assert(tick(&self) && dequeued==wasDequeued && self.queued==queued);
    assert(services==wasServiced+1);
    self.awdlNativePeerTimingEnabled=false;
    pump(&self,mockNow);
    assert(dequeued>wasDequeued); // boot-argument opt-out keeps original path
    // RS (f2u): frames waiting for another try go before new ones, oldest first for each receiver.
    auto fresh=[&](MockSelf &m) {
        m=MockSelf{};m.awdlNativeLastSyncUS=mockNow;m.awdlNativeTimingCell=149;
        credits=100;ready=1;closeAfterSend=0;peerHeld=false;cell=149;heldDest=0xff;
        waitingFrames.clear();resentTo.clear();resendCalls=0;
    };
    MockSelf r;
    fresh(r);waitingFrames={{4,0},{4,0},{6,0}};
    int was=dequeued;tick(&r);
    assert(resentTo==std::vector<uint8_t>({4,4,6}) && waitingFrames.empty());
    assert(r.awdlNativeStats.dataResent==3 && dequeued-was==13);   // the tick's budget covers both kinds
    // A receiver that is away keeps its frames waiting; another receiver's frame still goes.
    fresh(r);waitingFrames={{4,0},{4,0},{6,0}};heldDest=4;tick(&r);
    assert(resentTo==std::vector<uint8_t>({6}) && waitingFrames.size()==2 && waitingFrames[0].dest==4);
    // The holds switched off: every waiting frame goes at once.
    fresh(r);waitingFrames={{4,0},{6,0}};heldDest=4;r.awdlNativePeerTimingEnabled=false;tick(&r);
    assert(resentTo==std::vector<uint8_t>({4,6}));
    fresh(r);waitingFrames={{4,0},{6,0}};heldDest=4;qwz_nativepeerpause=0;tick(&r);
    assert(resentTo==std::vector<uint8_t>({4,6}));qwz_nativepeerpause=1;
    // A frame that is not due yet keeps the same receiver's newer frame behind it.
    fresh(r);waitingFrames={{4,EAGAIN},{4,0},{6,0}};tick(&r);
    assert(resentTo==std::vector<uint8_t>({6}) && waitingFrames.size()==2 && waitingFrames[1].result==0);
    // A frame past its bounds is dropped by the cell and the next one for that receiver goes.
    fresh(r);waitingFrames={{4,ETIMEDOUT},{4,0}};tick(&r);
    assert(resentTo==std::vector<uint8_t>({4}) && waitingFrames.empty() && r.awdlNativeStats.dataResent==1);
    // No slot or credit: the pass stops, nothing is lost.
    fresh(r);waitingFrames={{4,ENOSPC},{6,0}};tick(&r);
    assert(resentTo.empty() && waitingFrames.size()==2);
    fresh(r);waitingFrames={{4,0},{6,0}};credits=reserve;tick(&r);
    assert(resentTo.empty() && waitingFrames.size()==2);
    // More waiting than one tick's budget: 16 go, the rest next tick, and no new frame is taken meanwhile.
    fresh(r);for(int n=0;n<20;++n) waitingFrames.push_back({uint8_t(n%2?4:6),0});
    was=dequeued;tick(&r);
    assert(resentTo.size()==16 && waitingFrames.size()==4 && dequeued==was);
    tick(&r);assert(resentTo.size()==20 && waitingFrames.empty());
    puts("ok: radio pump preserves queued packets when credits/window close; sync diagnostics recover; waiting frames go again first, in order");
}
'''
with tempfile.TemporaryDirectory(prefix="qwz-native-tx-") as directory:
    path = Path(directory) / "test.cpp"
    output = Path(directory) / "test"
    path.write_text(code)
    subprocess.run(["xcrun", "clang++", "-std=c++14", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path), "-o", str(output)], check=True)
    subprocess.run([str(output)], check=True)
