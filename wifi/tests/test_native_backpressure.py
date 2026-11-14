#!/usr/bin/env python3
"""Exercise real native FIFO ownership against short/zero Skywalk dequeues."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "airport/awdlnative/QwzAWDLInterface.cpp").read_text()


def function(name):
    match = re.search(r"(?:UInt32|bool) QwzAWDLInterface::" + name + r"\(", source)
    assert match, name
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


code = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <AWDLObservation.hpp>
#include <AWDLPeerProbe.hpp>
using UInt32=unsigned;
using IOReturn=int;
using std::min;
constexpr int kIOReturnSuccess=0;
struct OSData {
    static OSData *withBytes(const void *,size_t) { return nullptr; }
    void release() {}
};
struct IOSkywalkPacket {
    uint8_t bytes[64]{};
    unsigned length=54,offset=8;
    unsigned getDataLength() const { return length; }
    unsigned getPacketBufferCount() const { return 1; }
    uintptr_t getDataVirtualAddress() const { return uintptr_t(bytes); }
};
using ItlSkywalkPacketView=IOSkywalkPacket;
static unsigned short itlPacketGetDataOffset(const IOSkywalkPacket *p) { return p->offset; }
struct QwzAWDLInterface;
struct MockQueue {
    QwzAWDLInterface *owner=nullptr;
    std::vector<IOSkywalkPacket *> waiting,completed;
    bool requested=false,synchronous=false;
    int requestResult=0,completeResult=0;
    unsigned requests=0,callbacks=0;
    void dequeue();
};
static IOReturn itlTxComplete(void *q,void *const *packets,unsigned count,unsigned flags) {
    assert(!flags && count && count<=32);
    auto &queue=*static_cast<MockQueue *>(q);
    if(queue.completeResult) return queue.completeResult;
    for(unsigned i=0;i<count;++i) {
        auto *p=static_cast<IOSkywalkPacket *>(packets[i]);
        assert(std::find(queue.completed.begin(),queue.completed.end(),p)==queue.completed.end());
        queue.completed.push_back(p);
    }
    return 0;
}
static IOReturn itlTxRequestDequeue(void *q,void *context,unsigned flags) {
    assert(!context && flags==1);
    auto &queue=*static_cast<MockQueue *>(q);
    ++queue.requests;
    if(queue.requestResult) return queue.requestResult;
    queue.requested=true;
    if(queue.synchronous) queue.dequeue();
    return 0;
}
struct QwzAWDLInterface {
    static constexpr unsigned transmitFrameCount=4;
    struct TransmitFrame { uint16_t length; uint8_t bytes[1514]; } frames[4]{};
    TransmitFrame *transmitFrames=frames;
    unsigned transmitDoneCount=0,transmitQueued=0,transmitHead=0;
    unsigned transmitAccepted=0,transmitCompleted=0,transmitDropped=0,transmitLastPublished=0;
    unsigned transmitBackpressureEvents=0,transmitResumeRequests=0;
    unsigned transmitPopped=0,transmitHeldMark=0,transmitHeldShort=0;
    bool transmitBackpressured=false,dataTransmitEnabled=true;
    unsigned addedQueues=4;
    void *packetQueues[4]{};
    IOSkywalkPacket *transmitDone[4]{};
    uint32_t transmitDepthBins[9]{};
    uint16_t transmitSequence=0;
    uint8_t localFrameAddress[6]={2,3,4,5,6,7};
    template<class... T> void setProperty(T...) {}
    UInt32 acceptTransmit(IOSkywalkPacket *const *,UInt32);
    bool serviceTransmit();
    bool popTransmit(uint8_t *,size_t,size_t *,const uint8_t * = nullptr,bool = false);
};
void MockQueue::dequeue() {
    requested=false;
    while(!waiting.empty()) {
        assert(++callbacks<1000); // zero acceptance must stop this dequeue pass
        unsigned n=owner->acceptTransmit(waiting.data(),unsigned(waiting.size()));
        assert(n<=waiting.size());
        waiting.erase(waiting.begin(),waiting.begin()+n);
        if(!n) break;
    }
}
static bool pop(QwzAWDLInterface &i,unsigned expected) {
    uint8_t out[1600]{}; size_t length=0;
    if(!i.popTransmit(out,sizeof(out),&length)) return false;
    assert(length==80 && out[24+16+7]==expected); // IPv6 hop limit tags order
    return true;
}
'''
code += "\n" + "\n".join(function(n) for n in ("acceptTransmit", "popTransmit", "serviceTransmit"))
code += r'''
int main() {
    QwzAWDLInterface i;
    MockQueue q; q.owner=&i; i.packetQueues[0]=i.packetQueues[1]=&q;
    IOSkywalkPacket packets[12];
    for(unsigned n=0;n<12;++n) {
        auto *eth=packets[n].bytes+packets[n].offset;
        eth[0]=4; memcpy(eth+6,i.localFrameAddress,6);
        eth[12]=0x86; eth[13]=0xdd; eth[14]=0x60; eth[21]=uint8_t(n);
        q.waiting.push_back(&packets[n]);
    }
    q.dequeue();
    assert(i.transmitQueued==4 && i.transmitAccepted==4 && q.waiting.size()==8);
    assert(i.transmitBackpressured && !i.transmitDropped);
    const unsigned callbacks=q.callbacks;
    i.serviceTransmit(); // completion space alone cannot resume a full FIFO
    assert(q.completed.size()==4 && q.requests==0 && q.callbacks==callbacks);
    assert(pop(i,0) && pop(i,1));
    i.serviceTransmit();
    assert(q.requested && q.requests==1 && !i.transmitBackpressured);
    q.dequeue();
    assert(i.transmitQueued==4 && i.transmitAccepted==6 && q.waiting.size()==6);
    assert(i.transmitBackpressured && !i.transmitDropped);
    // A synchronous resume can backpressure again; service must preserve that.
    q.synchronous=true;
    assert(pop(i,2)); i.serviceTransmit();
    assert(i.transmitBackpressured && i.transmitQueued==4 && q.waiting.size()==5);
    for(unsigned n=3;n<12;++n) {
        assert(pop(i,n)); i.serviceTransmit();
    }
    assert(!pop(i,12) && !i.serviceTransmit());
    assert(q.waiting.empty() && q.completed.size()==12 && i.transmitAccepted==12);
    assert(i.transmitCompleted==12 && !i.transmitDropped && !i.transmitBackpressured);
    for(unsigned n=0;n<12;++n) assert(q.completed[n]==&packets[n]);
    // Completion storage can fill even after all frame copies have been sent.
    QwzAWDLInterface j; MockQueue r; r.owner=&j; j.packetQueues[0]=j.packetQueues[1]=&r;
    for(unsigned n=0;n<4;++n) r.waiting.push_back(&packets[n]);
    r.dequeue(); for(unsigned n=0;n<4;++n) assert(pop(j,n));
    r.waiting.push_back(&packets[4]); r.dequeue();
    assert(j.transmitBackpressured && j.transmitDoneCount==4 && j.transmitAccepted==4);
    r.completeResult=5; j.serviceTransmit();
    assert(r.requests==0 && j.transmitDoneCount==4 && j.transmitBackpressured);
    r.completeResult=0; r.requestResult=7; j.serviceTransmit();
    assert(j.transmitBackpressured && r.requests==1 && r.waiting.size()==1);
    r.requestResult=0; j.serviceTransmit(); r.dequeue();
    assert(j.transmitAccepted==5 && pop(j,4)); j.serviceTransmit();
    assert(r.completed.size()==5 && !j.transmitDropped);
    // Invalid frames are completed once and dropped, never held indefinitely.
    packets[5].length=10; r.waiting.push_back(&packets[5]); r.dequeue(); j.serviceTransmit();
    assert(j.transmitDropped==1 && r.completed.back()==&packets[5]);
    j.transmitBackpressured=true; j.dataTransmitEnabled=false;
    unsigned requests=r.requests; j.serviceTransmit(); assert(r.requests==requests);
    puts("ok: full FIFO keeps ownership in Skywalk, resumes without loss/reordering/double completion");
}
'''
with tempfile.TemporaryDirectory(prefix="qwz-backpressure-") as directory:
    path = Path(directory) / "test.cpp"
    output = Path(directory) / "test"
    path.write_text(code)
    subprocess.run(["xcrun", "clang++", "-std=c++14", "-D__IO80211_TARGET=__MAC_26_0",
                    "-I" + str(root / "include"), "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path), "-o", str(output)], check=True)
    subprocess.run([str(output)], check=True)
