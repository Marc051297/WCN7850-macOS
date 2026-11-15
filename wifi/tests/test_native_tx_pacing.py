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
using IOReturn=int;
constexpr int kIOReturnSuccess=0, kIOReturnNotReady=1;
struct Packet { uint8_t byte; };
using mbuf_t=Packet *;
static int credits=2, reserve=1, ready=1, sent, freed, dequeued, roomChecks;
static int closeAfterSend, syncResult, syncCalls;
static bool peerHeld=false;
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
static int qwz_mon_tx_raw(const uint8_t *,size_t) {
    if(credits<=reserve) return ENOSPC;
    --credits; ++sent;
    if(closeAfterSend) ready=0;
    return 0;
}
struct Stats {
    uint64_t dataSent=0,dataTxErrors=0,syncSent=0,syncTxErrors=0,syncBuildFailures=0;
    uint64_t txSubmittedBytes=0;
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
        bool hold(const uint8_t *,uint8_t,uint64_t) {return peerHeld;}
        void clearPeers() {}
    } awdlNativePeerTiming;
    bool copyAWDLTransmitDestination(uint8_t *a) {a[0]=2;return queued>0;}
    bool serviceAWDLTransmit() {++services;return queued>0;}
    unsigned queuedAWDLTransmit() {return queued;}
    IOReturn buildAWDLTransmit(mbuf_t *p) {
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
    puts("ok: radio pump preserves queued packets when credits/window close; sync diagnostics recover");
}
'''
with tempfile.TemporaryDirectory(prefix="qwz-native-tx-") as directory:
    path = Path(directory) / "test.cpp"
    output = Path(directory) / "test"
    path.write_text(code)
    subprocess.run(["xcrun", "clang++", "-std=c++14", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path), "-o", str(output)], check=True)
    subprocess.run([str(output)], check=True)
