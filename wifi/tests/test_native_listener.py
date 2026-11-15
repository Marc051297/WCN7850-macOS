#!/usr/bin/env python3
"""Compile the real native listener/ROC functions with a mock firmware backend.

The source bodies are extracted, not copied into a second implementation.
Exercises listener identity, packet admission, channel events, stale scan events,
scan submission failures and the frequency selected for management transmit.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MON = (ROOT / "qwz/qwz_mon.c").read_text()
QWZ = (ROOT / "qwz/qwz.c").read_text()
REG = (ROOT / "qwz/qwzreg.h").read_text()


def block(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def function(source, name):
    match = re.search(r"(?:static )?(?:int|void)\n" + name + r"\(", source)
    assert match, name
    return block(source, match.group())


events = re.search(r"enum wmi_scan_event_type\s*\{.*?\};", REG, re.S)
assert events, "scan event constants"
dispatch = block(QWZ, "if ((sc->mon_dwell_scan_id != 0 && scan_ev.scan_id")
frequency = QWZ[QWZ.index("\tcmd->chanfreq = 0;", QWZ.index("\nqwz_wmi_mgmt_send(")):]
frequency = frequency[:frequency.index("\tcmd->paddr_lo")]

prefix = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define BIT(n) (1U << (n))
#define IEEE80211_ADDR_EQ(a,b) (memcmp((a),(b),6)==0)
#define IEEE80211_ADDR_COPY(a,b) memcpy((a),(b),6)
#define QWZ_MON_DWELL_REQ_ID 2
#define QWZ_CRASHED(sc) ((sc)->crashed)
constexpr int ATH12K_SCAN_IDLE=0, WMI_SCAN_PRIORITY_HIGH=3;
constexpr int M_DEVBUF=0, M_ZERO=1, M_NOWAIT=2;
struct ieee80211_channel { uint32_t ic_freq; };
struct ieee80211_node { ieee80211_channel *ni_chan; };
struct qwz_vif { uint8_t vdev_addr[6]; int is_up, is_started; uint32_t vdev_id; };
struct qwz_softc {
    uint8_t sc_awdl_addr[6], mon_bssid[6];
    int sc_awdl_addr_valid, mon_bssid_set;
    struct { uint8_t ic_myaddr[6]; ieee80211_node *ic_bss; bool ic_wcl_scan_requested; } sc_ic;
    qwz_vif sc_vif, sc_monvif;
    int mon_active, mon_coex, mon_coex_halt, mon_dwell_inflight, mon_dwell_onchannel;
    uint32_t mon_freq, mon_dwell_scan_id;
    uint64_t mon_dwell_arrived_ns, mon_dwell_next_ns;
    struct { uint32_t dwell_lastms, dwell_lastfreq, dwell_wrongfreq,
             dwell_onchan, dwell_fail, dwell_done, ufr_sent, ufr_full, ufr_fail; } mon_stats;
    struct { int state; } scan;
    int sc_awdltest_active, crashed, fw_initialized;
    struct { uint32_t pdev_id; } pdevs[1];
};
constexpr int WLAN_SCAN_CANCEL_SINGLE=1;
struct scan_cancel_param {uint32_t requester,scan_id,req_type,vdev_id,pdev_id;};
struct wmi_scan_event { uint32_t scan_id, scan_req_id, vdev_id, event_type, channel_freq, reason; };
struct scan_req_params {
    uint32_t vdev_id, scan_id, scan_req_id, scan_priority, scan_f_passive,
        scan_f_promisc_mode, scan_f_offchan_mgmt_tx, scan_f_offchan_data_tx,
        dwell_time_passive, dwell_time_active, burst_duration, max_scan_time,
        notify_scan_events, num_chan;
    uint32_t *chan_list;
};
static qwz_softc *qwz_mon_sc;
static void (*qwz_mon_native_rx)();
static int qwz_moncoexdwell=100, qwz_moncoexrest=100, qwz_moncoexdwellflags=7;
static const uint8_t qwz_awdl_bssid[6]={0x00,0x25,0x00,0xff,0x94,0x73};
static uint64_t mock_time=1000000000;
static uint64_t getnsecuptime() { return mock_time; }
static int qwz_mon_fw_unavail(qwz_softc *sc) { return !sc->fw_initialized || sc->crashed; }
static void native_rx() {}
static int sent, send_result, last_native_mode=-1,qwz_nativefmt=0;
static int qwz_mon_tx_bytes(qwz_softc *,const uint8_t *,size_t,uint8_t,int mode=-1) { ++sent;last_native_mode=mode;return send_result; }
static int allocations, fail_alloc, live_alloc;
static void *bsd_malloc(size_t n,int,int) {
    if (++allocations==fail_alloc) return nullptr;
    void *p=calloc(1,n); if(p) ++live_alloc; return p;
}
static void bsd_free(void *p,int,size_t) { if(p) { --live_alloc; free(p); } }
#define malloc(n,p,f) static_cast<scan_req_params *>(bsd_malloc(n,p,f))
#define mallocarray(n,s,p,f) static_cast<uint32_t *>(bsd_malloc((n)*(s),p,f))
#define free(p,t,s) bsd_free(p,t,s)
static scan_req_params last_request;
static uint32_t last_frequency;
static int submit_result;
static int cancels;
static scan_cancel_param last_cancel;
static int qwz_wmi_send_scan_stop_cmd(qwz_softc *,scan_cancel_param *p) {
    ++cancels; last_cancel=*p; return submit_result;
}
static void qwz_wmi_start_scan_init(qwz_softc *,scan_req_params *) {}
static int qwz_wmi_send_scan_start_cmd(qwz_softc *,scan_req_params *p) {
    last_request=*p; last_frequency=p->chan_list[0]; return submit_result;
}
'''

functions = "\n".join(function(MON, name) for name in (
    "qwz_mon_native_identity", "qwz_mon_native_tx_ready", "qwz_mon_tx_raw",
    "qwz_mon_dwell_clamp", "qwz_mon_dwell_event"))
functions += "\n" + function(QWZ, "qwz_mon_dwell_start")
functions += "\n" + function(QWZ, "qwz_mon_dwell_cancel")
functions += "\nstatic int station_events;\nstatic void dispatch(qwz_softc *sc, wmi_scan_event scan_ev) {\n"
functions += dispatch + "\n++station_events;\n}\n"
functions += "\nstatic uint32_t frequency(qwz_softc *sc,qwz_vif *arvif) { struct {uint32_t chanfreq;} result, *cmd=&result;\n"
functions += frequency + "\nreturn result.chanfreq;\n}\n"

main = r'''
int main() {
    qwz_softc sc{}; qwz_mon_sc=&sc;
    ieee80211_channel home{5180}; ieee80211_node bss{&home}; sc.sc_ic.ic_bss=&bss;
    const uint8_t address[6]={0x6a,0xcf,0xc3,0x8f,0xf8,4};
    memset(sc.sc_monvif.vdev_addr,0x86,6);
    uint8_t old[6]; memcpy(old,sc.sc_monvif.vdev_addr,6);
    // The daemon's cell keeps its original identity.
    assert(qwz_mon_native_identity(&sc)==0);
    assert(IEEE80211_ADDR_EQ(sc.sc_monvif.vdev_addr,old));
    qwz_mon_native_rx=native_rx;
    assert(qwz_mon_native_identity(&sc)==EADDRNOTAVAIL);
    memcpy(sc.sc_awdl_addr,address,6); sc.sc_awdl_addr_valid=1;
    memcpy(sc.sc_vif.vdev_addr,address,6);
    assert(qwz_mon_native_identity(&sc)==EADDRINUSE);
    memset(sc.sc_vif.vdev_addr,0x12,6);
    memcpy(sc.sc_ic.ic_myaddr,address,6);
    assert(qwz_mon_native_identity(&sc)==EADDRINUSE);
    memset(sc.sc_ic.ic_myaddr,0x14,6);
    sc.sc_awdl_addr[0]|=1;
    assert(qwz_mon_native_identity(&sc)==EADDRNOTAVAIL);
    memset(sc.sc_awdl_addr,0,6);
    assert(qwz_mon_native_identity(&sc)==EADDRNOTAVAIL);
    memcpy(sc.sc_awdl_addr,address,6);
    assert(qwz_mon_native_identity(&sc)==0);
    assert(IEEE80211_ADDR_EQ(sc.sc_monvif.vdev_addr,address));
    assert(sc.mon_bssid_set && IEEE80211_ADDR_EQ(sc.mon_bssid,qwz_awdl_bssid));
    sc.mon_active=sc.mon_coex=sc.sc_monvif.is_up=sc.fw_initialized=sc.sc_vif.is_started=1;
    sc.mon_freq=5745; sc.sc_vif.vdev_id=0;
    uint8_t frame[32]={0xd0}; memcpy(frame+10,address,6);
    assert(!qwz_mon_native_tx_ready());
    assert(qwz_mon_tx_raw(frame,sizeof(frame))==EAGAIN && sent==0);
    sc.mon_dwell_inflight=1;
    qwz_mon_dwell_event(&sc,WMI_SCAN_EVENT_FOREIGN_CHAN,5180,0);
    assert(!qwz_mon_native_tx_ready() && sc.mon_stats.dwell_wrongfreq==1);
    qwz_mon_dwell_event(&sc,WMI_SCAN_EVENT_FOREIGN_CHAN,5745,0);
    assert(qwz_mon_native_tx_ready() && sc.mon_stats.dwell_onchan==1);
    sc.scan.state=1;
    assert(!qwz_mon_native_tx_ready());
    assert(qwz_mon_tx_raw(frame,sizeof(frame))==EAGAIN && sent==0);
    sc.scan.state=0; sc.sc_ic.ic_wcl_scan_requested=true;
    assert(!qwz_mon_native_tx_ready());
    sc.sc_ic.ic_wcl_scan_requested=false;
    assert(qwz_mon_native_tx_ready());
    qwz_mon_dwell_event(&sc,WMI_SCAN_EVENT_FOREIGN_CHAN,5745,0);
    assert(sc.mon_stats.dwell_onchan==1); // duplicate event
    assert(qwz_mon_tx_raw(frame,sizeof(frame))==0 && sent==1);
    frame[10]^=2;
    assert(qwz_mon_tx_raw(frame,sizeof(frame))==EADDRNOTAVAIL && sent==1);
    frame[10]^=2;
    assert(frequency(&sc,&sc.sc_monvif)==5745);
    assert(frequency(&sc,&sc.sc_vif)==0); // station stays unchanged
    // Native-only rate experiments leave sync, small and multicast frames
    // unchanged; malformed settings do not inherit an injection context.
    uint8_t bulk[1400]={0x08};bulk[4]=2;memcpy(bulk+10,address,6);
    assert(qwz_mon_tx_raw(bulk,sizeof(bulk))==0 && last_native_mode==0);
    qwz_nativefmt=1;assert(qwz_mon_tx_raw(bulk,sizeof(bulk))==0 && last_native_mode==1);
    qwz_nativefmt=2;assert(qwz_mon_tx_raw(bulk,sizeof(bulk))==0 && last_native_mode==2);
    bulk[4]=3;assert(qwz_mon_tx_raw(bulk,sizeof(bulk))==0 && last_native_mode==0);
    bulk[4]=2;assert(qwz_mon_tx_raw(bulk,1199)==0 && last_native_mode==0);
    bulk[0]=0xd0;assert(qwz_mon_tx_raw(bulk,sizeof(bulk))==0 && last_native_mode==0);
    bulk[0]=0x08;qwz_nativefmt=-1;assert(qwz_mon_tx_raw(bulk,sizeof(bulk))==0 && last_native_mode==0);
    mock_time+=100000000;
    qwz_mon_dwell_event(&sc,WMI_SCAN_EVENT_FOREIGN_CHAN_EXIT,5745,0);
    assert(!qwz_mon_native_tx_ready() && sc.mon_stats.dwell_lastms==100);
    assert(frequency(&sc,&sc.sc_monvif)==0);
    qwz_mon_dwell_event(&sc,WMI_SCAN_EVENT_COMPLETED,5180,0);
    assert(!sc.mon_dwell_inflight && sc.mon_stats.dwell_done==1);
    assert(sc.mon_dwell_next_ns==mock_time+100000000);
    qwz_mon_dwell_event(&sc,WMI_SCAN_EVENT_FOREIGN_CHAN,5745,0);
    assert(!qwz_mon_native_tx_ready()); // late event after completion

    // Scan request matches ROC settings; each new visit has its own id.
    assert(qwz_mon_dwell_start(&sc,5745,100)==0 && live_alloc==0);
    const uint32_t old_id=sc.mon_dwell_scan_id;
    assert(last_request.scan_id==old_id && last_request.scan_req_id==2);
    assert(last_frequency==5745 && last_request.burst_duration==100);
    assert(last_request.scan_f_passive && last_request.scan_f_promisc_mode);
    assert(last_request.scan_f_offchan_mgmt_tx && last_request.scan_f_offchan_data_tx);
    assert(last_request.notify_scan_events & WMI_SCAN_EVENT_FOREIGN_CHAN_EXIT);
    assert(qwz_mon_dwell_start(&sc,5745,100)==0 && sc.mon_dwell_scan_id!=old_id);
    sc.mon_dwell_inflight=1;
    wmi_scan_event ev{old_id,2,0,WMI_SCAN_EVENT_FOREIGN_CHAN,5745,0};
    dispatch(&sc,ev);
    assert(!qwz_mon_native_tx_ready() && station_events==0);
    ev.scan_id=sc.mon_dwell_scan_id; ev.vdev_id=99;
    dispatch(&sc,ev); assert(!qwz_mon_native_tx_ready());
    ev.vdev_id=0; ev.scan_req_id=1;
    dispatch(&sc,ev); assert(!qwz_mon_native_tx_ready() && station_events==0);
    ev.scan_req_id=2; dispatch(&sc,ev); assert(qwz_mon_native_tx_ready());
    ev.scan_id=old_id; ev.event_type=WMI_SCAN_EVENT_COMPLETED;
    dispatch(&sc,ev); assert(qwz_mon_native_tx_ready());
    ev.scan_id=sc.mon_dwell_scan_id; dispatch(&sc,ev); assert(!qwz_mon_native_tx_ready());
    ev.scan_id=0xa000; ev.scan_req_id=1;
    dispatch(&sc,ev); assert(station_events==1);
    sc.mon_dwell_scan_id=0xafff;
    assert(qwz_mon_dwell_start(&sc,5745,100)==0 && sc.mon_dwell_scan_id==0xa001);
    fail_alloc=allocations+1;
    assert(qwz_mon_dwell_start(&sc,5745,100)==ENOMEM && live_alloc==0);
    fail_alloc=allocations+2;
    assert(qwz_mon_dwell_start(&sc,5745,100)==ENOMEM && live_alloc==0);
    fail_alloc=0; submit_result=EIO;
    assert(qwz_mon_dwell_start(&sc,5745,100)==EIO && live_alloc==0);
    sc.scan.state=1;
    assert(qwz_mon_dwell_start(&sc,5745,100)==EBUSY && live_alloc==0);
    sc.scan.state=0;
    // Yielding to a tether scan cancels only the outstanding ROC visit.
    sc.mon_dwell_inflight=0;
    assert(qwz_mon_dwell_cancel(&sc)==0 && cancels==0);
    sc.mon_dwell_inflight=1; submit_result=0; sc.pdevs[0].pdev_id=0;
    assert(qwz_mon_dwell_cancel(&sc)==0 && cancels==1);
    assert(last_cancel.scan_id==sc.mon_dwell_scan_id && last_cancel.requester==2);
    assert(last_cancel.req_type==WLAN_SCAN_CANCEL_SINGLE && last_cancel.vdev_id==sc.sc_vif.vdev_id);
    submit_result=EIO; assert(qwz_mon_dwell_cancel(&sc)==EIO && cancels==2);
    submit_result=0;

    // Same-channel mode doesn't require ROC. Stop, a changed address or
    // failed firmware always closes admission.
    sc.mon_freq=5180; assert(qwz_mon_native_tx_ready());
    sc.mon_coex_halt=1; assert(!qwz_mon_native_tx_ready()); sc.mon_coex_halt=0;
    sc.crashed=1; assert(!qwz_mon_native_tx_ready()); sc.crashed=0;
    sc.sc_awdl_addr_valid=0; assert(!qwz_mon_native_tx_ready()); sc.sc_awdl_addr_valid=1;
    sc.sc_awdl_addr[5]^=1; assert(!qwz_mon_native_tx_ready()); sc.sc_awdl_addr[5]^=1;
    sc.mon_active=0;
    sc.mon_dwell_inflight=1; sc.mon_dwell_onchannel=0;
    qwz_mon_dwell_event(&sc,WMI_SCAN_EVENT_FOREIGN_CHAN,5180,0);
    assert(!sc.mon_dwell_onchannel && !qwz_mon_native_tx_ready());
    assert(live_alloc==0);
    puts("ok: native listener identity, transmit admission, ROC events and scan ownership");
}
'''

with tempfile.TemporaryDirectory(prefix="qwz-native-listener-") as directory:
    source = Path(directory) / "test.cpp"
    output = Path(directory) / "test"
    source.write_text(prefix + events.group() + "\n" + functions + main)
    subprocess.run(["xcrun", "clang++", "-std=c++14", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(source), "-o", str(output)], check=True)
    subprocess.run([str(output)], check=True)
