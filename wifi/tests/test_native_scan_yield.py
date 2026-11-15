#!/usr/bin/env python3
"""Execute actual AWDL scan admission and visit worker with firmware interleavings."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
mon = (root / 'qwz/qwz_mon.c').read_text()
qwz = (root / 'qwz/qwz.c').read_text()


def function(name):
    m = re.search(r'(?:static )?(?:int|void)\n' + name + r'\(', mon)
    assert m, name
    end = mon.index('{', m.end()) + 1
    depth = 1
    while depth:
        depth += (mon[end] == '{') - (mon[end] == '}')
        end += 1
    return mon[m.start():end]


prefix = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#define MSEC_TO_NSEC(n) (uint64_t(n)*1000000)
constexpr int ATH12K_SCAN_IDLE=0,ATH12K_SCAN_STARTING=1,ATH12K_SCAN_RUNNING=2;
constexpr uint8_t IEEE80211_FC0_VERSION_MASK=3,IEEE80211_FC0_TYPE_MASK=12,IEEE80211_FC0_TYPE_MGT=0;
constexpr uint8_t IEEE80211_FC0_SUBTYPE_MASK=0xf0,IEEE80211_FC0_SUBTYPE_BEACON=0x80,IEEE80211_FC0_SUBTYPE_PROBE_RESP=0x50;
struct ieee80211com {struct {void *if_softc;} ic_if; bool ic_wcl_scan_requested=false;};
struct qwz_softc {
    ieee80211com sc_ic;
    int mon_active=1,mon_coex=1,mon_native_owned=1,mon_coex_halt=0;
    int mon_dwell_inflight=0,mon_dwell_onchannel=0,mon_dwell_submitting=0;
    uint32_t sc_fw_epoch=1,mon_freq=5745;
    int mon_chan=149,dead=0,hold=0;
    uint64_t mon_dwell_next_ns=0;
    int mon_dwell_plan_ms=0; // AL: 0 = the fixed visit
    int mon_rate_applied=12; // RT: the rate the cell was last given
    struct {int state=ATH12K_SCAN_STARTING;} scan;
    struct {unsigned dwell_fail=0,dwell_start=0,coex_st_mgmt=0,coex_cell_mgmt=0,coex_drop=0,coex_scan_mgmt=0;
            int coex_scan_rssi_min=0,coex_scan_rssi_max=0;} mon_stats;
};
static qwz_softc *qwz_mon_sc;
static int qwz_moncoexscan=1,qwz_moncoexdwell=100,qwz_moncoexrest=100;
static uint64_t now;
static uint64_t getnsecuptime(){return now;}
static int qwz_mon_fw_unavail(qwz_softc *sc){return sc->dead;}
static int qwz_mon_coex_running(qwz_softc *sc){return sc && sc->mon_active && sc->mon_coex && !sc->dead;}
static int qwz_mon_holding(ieee80211com *ic){return ic && static_cast<qwz_softc*>(ic->ic_if.if_softc)->hold;}
static int sleeps,sleep_error,submits,submit_mode;
static void (*on_sleep)();
static int tsleep_nsec(void *,int,const char *,uint64_t ns) {
    now+=ns; ++sleeps;
    if(on_sleep) on_sleep();
    return sleep_error ? sleep_error : EWOULDBLOCK;
}
static int qwz_monrate=12,rate_sets,rate_mode;
static void qwz_mon_cell_rate(qwz_softc *sc) { // RT: may sleep, like the visit; mode 1 = the firmware restarted meanwhile
    ++rate_sets; assert(sc->mon_dwell_submitting); sc->mon_rate_applied=qwz_monrate;
    if(rate_mode==1) ++sc->sc_fw_epoch;
}
static int qwz_mon_dwell_start(qwz_softc *sc,uint32_t,int) {
    ++submits; assert(sc->mon_dwell_submitting);
    if(submit_mode==1) ++sc->sc_fw_epoch;
    return submit_mode==2 ? ENOBUFS : 0;
}
'''
body = '\n'.join(function(n) for n in (
    'qwz_mon_scan_hold', 'qwz_mon_is_scan_frame', 'qwz_mon_dwell_clamp', 'qwz_mon_dwell_task_fn', 'qwz_mon_scan_drain'))
# Compile the real scan reservation/rollback statements too. Parameters and
# firmware submission after admission are replaced with failure injection.
start = qwz.index('\nqwz_scan(struct qwz_softc *sc)')
admit = qwz[qwz.index('\tswitch (sc->scan.state)', start):qwz.index('\targ = malloc(', start)]
cleanup = qwz[qwz.index('exit:\n', start):qwz.index('\tif (arg) {', qwz.index('exit:\n', start))]
prefix = prefix.replace('int mon_chan=149,dead=0,hold=0;',
                        'int mon_chan=149,dead=0,hold=0,sc_awdltest_active=0;')
prefix = prefix.replace('struct {int state=ATH12K_SCAN_STARTING;} scan;',
                        'struct {int state=ATH12K_SCAN_STARTING,started=0,completed=0,is_roc=0,vdev_id=0;} scan;')
prefix = prefix.replace('ATH12K_SCAN_RUNNING=2;', 'ATH12K_SCAN_RUNNING=2,ATH12K_SCAN_ABORTING=3;')
body += r'''
static int admission(qwz_softc *sc,int failure,bool reset=false) {
    bool owns_scan=false;
    const uint32_t scan_epoch=sc->sc_fw_epoch;
    struct {int vdev_id=0;} vif,*arvif=&vif;
    int ret=0;
''' + admit + r'''
    if(reset) ++sc->sc_fw_epoch;
    ret=failure; // allocation/command result
''' + cleanup + '\nreturn ret;\n}\n'
rx_start = qwz.index('\tif (sc->mon_active && sc->mon_coex)', qwz.index('rxi.rxi_rssi = rx_ev.snr'))
rx_branch = qwz[rx_start:qwz.index('\t/* Monitor-mode capture', rx_start)]
body += r'''
#define M_LEN(m) ((m)->length)
#define mtod(m,t) static_cast<t>((m)->bytes)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
struct mbuf {size_t length=36;uint8_t bytes[36]{};bool freed=false;};
static unsigned captured;
static void m_freem(mbuf *m){assert(!m->freed);m->freed=true;}
static void qwz_mon_capture_mgmt_mbuf(qwz_softc *,mbuf *m,int,int){assert(!m->freed);++captured;}
// Classify the other two existing routes without changing their precedence.
static bool qwz_mon_is_station_frame(qwz_softc *,const uint8_t *f,size_t){return f[4]==1;}
static bool qwz_mon_claims_mgmt(qwz_softc *,const uint8_t *f,size_t,uint32_t){return f[4]==2;}
static int receive(qwz_softc *sc,mbuf *m,int sample) {
    struct {int rxi_rssi;} rxi{sample};
    struct {uint32_t chan_freq=5180;int rate=6;} rx_ev;
''' + rx_branch + r'''
k5_coex_station_mgmt:
    assert(!m->freed);
    return sample; // unchanged value enters normal net80211 validation
exit:
    assert(m->freed);
    return -4096;
}
'''
main = r'''
int main() {
    qwz_softc sc; qwz_mon_sc=&sc; sc.sc_ic.ic_if.if_softc=&sc;
    // Fresh beacon/probe responses take the normal scanner path, with their
    // own RSSI. Action/data/invalid/truncated frames cannot enter that path.
    uint8_t frame[36]{};
    for(unsigned state=0;state<4;++state) for(unsigned fc=0;fc<256;++fc) {
        sc.scan.state=state; frame[0]=fc;
        bool expected=(state==1 || state==2) && (fc==0x80 || fc==0x50);
        assert(bool(qwz_mon_is_scan_frame(&sc,frame,sizeof(frame)))==expected);
        assert(!qwz_mon_is_scan_frame(&sc,frame,35));
    }
    sc.scan.state=ATH12K_SCAN_STARTING; frame[0]=0x80;
    sc.mon_native_owned=0; assert(!qwz_mon_is_scan_frame(&sc,frame,sizeof(frame)));
    sc.mon_native_owned=1;
    mbuf beacon; beacon.bytes[0]=0x80; beacon.bytes[4]=3;
    assert(receive(&sc,&beacon,-88)==-88 && !beacon.freed && captured==0);
    assert(sc.mon_stats.coex_scan_mgmt==1 && sc.mon_stats.coex_scan_rssi_min==-88);
    mbuf probe;probe.bytes[0]=0x50;probe.bytes[4]=3;
    assert(receive(&sc,&probe,-73)==-73 && sc.mon_stats.coex_scan_rssi_max==-73);
    mbuf station;station.bytes[4]=1;
    assert(receive(&sc,&station,-35)==-35 && sc.mon_stats.coex_st_mgmt==1);
    mbuf action;action.bytes[0]=0xd0;action.bytes[4]=2;
    assert(receive(&sc,&action,-50)==-4096 && captured==1 && sc.mon_stats.coex_cell_mgmt==1);
    mbuf foreignAction;foreignAction.bytes[0]=0xd0;foreignAction.bytes[4]=3;
    assert(receive(&sc,&foreignAction,-60)==-4096 && captured==1);
    sc.scan.state=ATH12K_SCAN_IDLE;
    mbuf idleBeacon;idleBeacon.bytes[0]=0x80;idleBeacon.bytes[4]=3;
    assert(receive(&sc,&idleBeacon,-88)==-4096 && sc.mon_stats.coex_scan_mgmt==2);
    sc.scan.state=ATH12K_SCAN_STARTING;
    assert(!qwz_mon_scan_hold(&sc.sc_ic)); // native scans must reach firmware
    sc.mon_native_owned=0; assert(qwz_mon_scan_hold(&sc.sc_ic));
    sc.mon_native_owned=1; sc.hold=1; assert(qwz_mon_scan_hold(&sc.sc_ic)); sc.hold=0;
    assert(qwz_mon_scan_drain(&sc)==0 && sleeps==0);
    // Pending but unsent visit withdraws; no WMI command goes out.
    sc.mon_dwell_inflight=1;
    on_sleep=[](){qwz_mon_dwell_task_fn(qwz_mon_sc);};
    assert(qwz_mon_scan_drain(&sc)==0 && submits==0 && sleeps==1);
    // A real visit remains owned until completion, including a sender still
    // returning after an early firmware event cleared inflight.
    sc.mon_dwell_inflight=sc.mon_dwell_submitting=1;
    sleeps=0;
    on_sleep=[](){if(sleeps==4)qwz_mon_sc->mon_dwell_inflight=0;
                 if(sleeps==8)qwz_mon_sc->mon_dwell_submitting=0;};
    assert(qwz_mon_scan_drain(&sc)==0 && sleeps==8);
    // Missing completion cannot be invented to admit a competing scan.
    sc.mon_dwell_inflight=1; sleeps=0; on_sleep=nullptr;
    assert(qwz_mon_scan_drain(&sc)==EBUSY && sleeps==150 && sc.mon_dwell_inflight);
    // Firmware restart and no-sleep contexts abort the wait without ownership loss.
    sleeps=0; on_sleep=[](){++qwz_mon_sc->sc_fw_epoch;};
    assert(qwz_mon_scan_drain(&sc)==ESHUTDOWN && sc.mon_dwell_inflight);
    on_sleep=nullptr; sleep_error=EDEADLK;
    assert(qwz_mon_scan_drain(&sc)==EDEADLK && sc.mon_dwell_inflight);
    sleep_error=0; sc.dead=1;
    assert(qwz_mon_scan_drain(&sc)==ESHUTDOWN); sc.dead=0;
    sc.mon_dwell_inflight=0; sc.scan.state=2;
    assert(qwz_mon_scan_drain(&sc)==EBUSY);
    // WCL can request a scan before reserving STARTING; queued visits also
    // withdraw in that interval, then resume after the real scan finishes.
    sc.scan.state=ATH12K_SCAN_IDLE; sc.sc_ic.ic_wcl_scan_requested=true;
    sc.mon_dwell_inflight=1; qwz_mon_dwell_task_fn(&sc);
    assert(submits==0 && !sc.mon_dwell_inflight);
    sc.sc_ic.ic_wcl_scan_requested=false; sc.mon_dwell_inflight=1;
    qwz_mon_dwell_task_fn(&sc);
    assert(submits==1 && sc.mon_dwell_inflight && sc.mon_stats.dwell_start==1);
    submit_mode=2; qwz_mon_dwell_task_fn(&sc);
    assert(submits==2 && !sc.mon_dwell_inflight && sc.mon_stats.dwell_fail==1);
    // Old sender returning across reset must not edit new epoch ownership.
    submit_mode=1; sc.mon_dwell_inflight=1; qwz_mon_dwell_task_fn(&sc);
    assert(submits==3 && sc.mon_dwell_inflight && sc.mon_stats.dwell_start==1);
    assert(!sc.mon_dwell_submitting && sc.mon_active && sc.mon_native_owned);
    // RT: a changed debug.qwz.monrate reaches the cell from the visit task, once, before the visit goes out.
    submit_mode=0; sc.mon_dwell_inflight=1; qwz_monrate=24;
    qwz_mon_dwell_task_fn(&sc);
    assert(rate_sets==1 && sc.mon_rate_applied==24 && submits==4);
    sc.mon_dwell_inflight=1; qwz_mon_dwell_task_fn(&sc);
    assert(rate_sets==1 && submits==5);
    // The firmware restarted while the rate command slept: no visit goes into the new epoch, its state is left alone.
    rate_mode=1; qwz_monrate=36; sc.mon_dwell_inflight=1; qwz_mon_dwell_task_fn(&sc);
    assert(rate_sets==2 && submits==5 && sc.mon_dwell_inflight && !sc.mon_dwell_submitting);
    rate_mode=0;
    // Allocation/drain failure frees only this scan's reservation. A busy
    // scan or newer firmware epoch retains its own state.
    submit_mode=0; sc.mon_dwell_inflight=0; sc.scan.state=ATH12K_SCAN_IDLE;
    assert(admission(&sc,ENOMEM)==ENOMEM && sc.scan.state==ATH12K_SCAN_IDLE);
    sc.scan.state=ATH12K_SCAN_RUNNING;
    assert(admission(&sc,ENOMEM)==EBUSY && sc.scan.state==ATH12K_SCAN_RUNNING);
    sc.scan.state=ATH12K_SCAN_IDLE;
    assert(admission(&sc,ENOMEM,true)==ENOMEM && sc.scan.state==ATH12K_SCAN_STARTING);
    sc.scan.state=ATH12K_SCAN_IDLE; sc.sc_awdltest_active=1;
    assert(admission(&sc,0)==EBUSY && sc.scan.state==ATH12K_SCAN_IDLE);
    sc.sc_awdltest_active=0; sc.mon_dwell_inflight=1;
    assert(admission(&sc,0)==EBUSY && sc.scan.state==ATH12K_SCAN_IDLE && sc.mon_dwell_inflight);
    sc.mon_dwell_inflight=0;
    assert(admission(&sc,0)==0 && sc.scan.state==ATH12K_SCAN_STARTING);
    puts("native scan yield: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='qwz-scan-yield-') as temp:
    src = Path(temp) / 'test.cpp'
    exe = Path(temp) / 'test'
    src.write_text(prefix + body + main)
    subprocess.run(['xcrun', 'clang++', '-std=c++17', '-O1', '-g',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
