#!/usr/bin/env python3
"""Compile the real management TX sender; exercise out-of-order completions."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "qwz/qwz.c").read_text()
header = (root / "qwz/qwzvar.h").read_text()


def function(text, name):
    match = re.search(r"(?:static inline )?int\n" + name + r"\(", text)
    assert match, name
    end = text.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


code = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#undef __APPLE__
#define DNPRINTF(...) ((void)0)
#define nitems(a) int(sizeof(a)/sizeof((a)[0]))
#define QWZ_JOINLOG(i) 0
constexpr int QWZ_BSS_TXMGMT_SLOTS=8,QWZ_MGMT_QUEUE_ID=3;
constexpr int BUS_DMA_WRITE=1,BUS_DMA_NOWAIT=2,M_DONTWAIT=0;
constexpr int IEEE80211_FC0_SUBTYPE_MASK=0xf0,IEEE80211_FC0_SUBTYPE_PROBE_REQ=0x40,IEEE80211_FC0_SUBTYPE_ACTION=0xd0;
struct ieee80211_node {};
struct ieee80211_frame {uint8_t i_fc[2]{},i_addr1[6]{};};
struct mbuf {ieee80211_frame frame;unsigned tag;};
struct Map {mbuf *resident=nullptr;};
struct qwz_tx_data {mbuf *m=nullptr;ieee80211_node *ni=nullptr;Map *map=nullptr;};
struct qwz_txmgmt_queue {qwz_tx_data data[64];int cur=0,queued=0;};
struct qwz_vif {qwz_txmgmt_queue txmgmt;unsigned vdev_id=0;};
struct qwz_softc {qwz_vif sc_vif,sc_monvif;int mon_coex=0,sc_dmat=0;unsigned qfullmsk=0;struct {const char *dv_xname="mock";} sc_dev;};
static unsigned maps,unmaps,frees,sends,lastSlot;static int sendError;
static int bus_dmamap_load_mbuf(int,Map *map,mbuf *m,int) {
    assert(map && !map->resident); // occupied slot reuse is a DMA ownership violation
    map->resident=m;++maps;return 0;
}
static void bus_dmamap_unload(int,Map *map) {assert(map && map->resident);map->resident=nullptr;++unmaps;}
static void m_freem(mbuf *) {++frees;}
static int m_defrag(mbuf *,int) {return 0;}
#define mtod(m,type) reinterpret_cast<type>(&(m)->frame)
static const char *ether_sprintf(const void *) {return "mock";}
static void qwz_log(const char *,...) {}
static int qwz_wmi_mgmt_send(qwz_softc *,qwz_vif *,uint8_t,unsigned slot,mbuf *m,qwz_tx_data *td) {
    assert(!td->m && td->map->resident==m);++sends;lastSlot=slot;
    if(sendError) return sendError;
    td->m=m;return 0;
}
'''
code += "\n" + function(header, "qwz_txmgmt_find_free")
code += "\n" + "\n".join(function(source, name) for name in (
    "qwz_txmgmt_nslots", "qwz_txmgmt_base", "qwz_mac_mgmt_tx_wmi"))
code += r'''
static void run(bool station,bool coex) {
    qwz_softc sc;sc.mon_coex=coex;
    qwz_vif &v=station?sc.sc_vif:sc.sc_monvif;
    Map dma[64];for(unsigned i=0;i<64;++i)v.txmgmt.data[i].map=&dma[i];
    int first=station?0:coex?8:0,limit=station?8:64,capacity=limit-first;
    mbuf frames[3000]{};std::vector<unsigned> live;unsigned used=0;
    std::mt19937 rng(0x7850);
    v.txmgmt.cur=-9; // invalid cursor must be normalized before indexing
    for(unsigned n=0;n<2500;++n) {
        if(!live.empty() && (rng()%3)!=0) {
            unsigned at=rng()%live.size(),slot=live[at];auto &td=v.txmgmt.data[slot];
            assert(td.m && td.map->resident==td.m);
            bus_dmamap_unload(0,td.map);td.m=nullptr;td.ni=nullptr;--v.txmgmt.queued;
            live.erase(live.begin()+at);
        }
        mbuf *m=&frames[used++];m->tag=used;
        unsigned before=sends;int previous=v.txmgmt.queued;
        int result=qwz_mac_mgmt_tx_wmi(&sc,&v,0,nullptr,m);
        if(previous==capacity) {
            assert(result==ENOSPC && sends==before && v.txmgmt.queued==previous);
        } else {
            assert(result==0 && lastSlot>=unsigned(first) && lastSlot<unsigned(limit));
            assert(v.txmgmt.data[lastSlot].m==m && v.txmgmt.queued==previous+1);
            for(unsigned slot:live) assert(slot!=lastSlot);
            live.push_back(lastSlot);
        }
        assert(v.txmgmt.queued==int(live.size()));
        for(int slot=0;slot<64;++slot) {
            auto &td=v.txmgmt.data[slot];assert(td.map->resident==td.m);
            if(slot<first || slot>=limit) assert(!td.m);
        }
    }
    for(unsigned slot:live) {auto &td=v.txmgmt.data[slot];bus_dmamap_unload(0,td.map);td.m=nullptr;--v.txmgmt.queued;}
    assert(v.txmgmt.queued==0);
    // All completion holes can be reused; a cursor pointing at a live frame
    // must skip it instead of replacing its mbuf/map/kind bookkeeping.
    v.txmgmt.cur=first;assert(qwz_mac_mgmt_tx_wmi(&sc,&v,0,nullptr,&frames[used++])==0);
    auto *owned=v.txmgmt.data[first].m;v.txmgmt.cur=first;
    int selected=qwz_txmgmt_find_free(&v.txmgmt,first,limit);
    assert(selected==first+1);v.txmgmt.cur=selected;
    assert(qwz_mac_mgmt_tx_wmi(&sc,&v,0,nullptr,&frames[used++])==0 && lastSlot==unsigned(selected));
    assert(v.txmgmt.data[first].m==owned && dma[first].resident==owned);
    for(int slot=first;slot<first+2;++slot) {bus_dmamap_unload(0,&dma[slot]);v.txmgmt.data[slot].m=nullptr;--v.txmgmt.queued;}
    // Submission failure unmaps only this attempt and leaves caller ownership.
    int previous=v.txmgmt.queued;sendError=EIO;unsigned freeBefore=frees;
    assert(qwz_mac_mgmt_tx_wmi(&sc,&v,0,nullptr,&frames[used++])==EIO);
    assert(v.txmgmt.queued==previous && frees==freeBefore);
    for(int slot=0;slot<64;++slot) assert(!v.txmgmt.data[slot].m && !dma[slot].resident);
    sendError=0;
}
int main() {
    run(true,true);run(false,true);run(false,false);
    assert(maps==unmaps && !frees);
    puts("ok: real sender preserves DMA/mbuf ownership through out-of-order completion, wrap, full rings, partition limits and failed submissions");
}
'''
with tempfile.TemporaryDirectory(prefix="qwz-tx-slot-") as directory:
    path, binary = Path(directory) / "test.cpp", Path(directory) / "test"
    path.write_text(code)
    subprocess.run(["xcrun", "clang++", "-std=c++14", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-fsanitize=address,undefined", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
