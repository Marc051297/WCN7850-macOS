#!/usr/bin/env python3
"""Compile real native automatic-start code with a deferred task/firmware mock."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "qwz/qwz_mon.c").read_text()


def function(name):
    # Definitions have their return type on the preceding line.
    match = re.search(r"(?:static )?(?:int|void|const char \*|IOReturn)\n" + name + r"\(", source)
    assert match, name
    start = match.start()
    end = source.index("{", match.end()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


prefix = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define isset(a,n) ((a)[(n)/8] & (1U<<((n)%8)))
constexpr unsigned IEEE80211_CHAN_PASSIVE=0x200,IEEE80211_CHAN_DFS=0x80000,IEEE80211_CHAN_NOADHOC=0x200000;
constexpr int IEEE80211_S_RUN=4,ATH12K_SCAN_IDLE=0,RW_WRITE=1,RW_NOSLEEP=0x40;
constexpr unsigned QWZ_MON_F_OFFSETS=1,QWZ_MON_F_RXMON=2,QWZ_MON_F_KEEP_FILTER=8,QWZ_MON_F_REO=256,QWZ_MON_F_AP=1024;
struct task {int adds=0;};
struct rwlock {int held=0;};
struct ieee80211_channel {uint32_t ic_freq=0,ic_flags=0;};
struct ieee80211_node {ieee80211_channel *ni_chan=nullptr;};
struct ieee80211com {
    int ic_state=0; ieee80211_node *ic_bss=nullptr;
    bool ic_wcl_join_requested=false,ic_wcl_scan_requested=false;
    ieee80211_channel ic_channels[256]; uint8_t ic_chan_active[32]{};
};
struct qwz_softc {
    ieee80211com sc_ic;
    struct {int is_up=0,is_started=0;} sc_vif;
    int mon_native_auto=0,mon_native_manual=0,mon_native_owned=0;
    int sc_awdl_addr_valid=0,attached=0,mon_active=0,mon_coex=0,mon_coex_setup=0,mon_coex_task_why=0;
    uint64_t mon_native_next_ns=0,mon_native_pause_until_ns=0;
    struct {int state=0;} scan;
    task mon_coex_task; rwlock ioctl_rwl;
    int unavailable=0;
};
using qwz_mon_native_rx_fn=void(*)();
static qwz_softc *qwz_mon_sc;
static qwz_mon_native_rx_fn qwz_mon_native_rx;
static void *qwz_mon_native_rx_arg;
static const char *qwz_mon_native_state="disabled";
static int qwz_moncoex,qwz_moncoexmcc,qwz_moncoexjoin,qwz_moncoexquota,qwz_moncoexfilter,qwz_moncoexdwell,qwz_moncoexrest,qwz_moncoexdwellflags;
static uint32_t qwz_mon_flags;
static uint64_t now=10000000000ULL;
static uint64_t getnsecuptime() {return now;}
static int qwz_mon_fw_unavail(qwz_softc *sc) {return sc->unavailable;}
static void *systq;
static void task_add(void *,task *t) {++t->adds;}
static int rw_enter(rwlock *l,int flags) {
    assert(flags==(RW_WRITE|RW_NOSLEEP));
    if(l->held) return EBUSY; l->held=1; return 0;
}
static void rw_exit(rwlock *l) {assert(l->held); l->held=0;}
static int starts,stops,start_result,cancel_during_start,chosen;
static int qwz_mon_start(qwz_softc *sc,int channel) {
    assert(sc->ioctl_rwl.held); ++starts; chosen=channel;
    if(cancel_during_start) {
        qwz_mon_native_rx=nullptr; sc->mon_native_auto=0;
    }
    sc->mon_active=sc->mon_coex=start_result==0;
    return start_result;
}
static void qwz_mon_stop(qwz_softc *sc) {++stops; sc->mon_active=sc->mon_coex=0;sc->mon_native_owned=0;}
static void rx() {}
using OSObject=void;
using IOReturn=int;
constexpr int kIOReturnSuccess=0;
static int rw_enter_write(rwlock *l) {if(l->held)return EBUSY;l->held=1;return 0;}
'''

names = ["qwz_mon_set_native_rx", "qwz_mon_native_auto_enable", "qwz_mon_native_auto_state",
         "qwz_mon_native_auto_allowed", "qwz_mon_native_auto_channel", "qwz_mon_native_auto_tick",
         "qwz_mon_native_auto_start", "qwz_mon_gated", "qwz_mon_native_pause"]
main = r'''
int main() {
    qwz_softc sc; qwz_mon_sc=&sc;
    qwz_mon_set_native_rx(rx,&sc);
    qwz_mon_native_auto_enable(1);
    qwz_mon_native_auto_tick(); assert(sc.mon_coex_task.adds==0 && starts==0);
    sc.attached=sc.sc_vif.is_up=sc.sc_vif.is_started=sc.sc_awdl_addr_valid=1;
    sc.sc_ic.ic_state=IEEE80211_S_RUN;
    ieee80211_channel home{5180,0}; ieee80211_node bss{&home}; sc.sc_ic.ic_bss=&bss;
    sc.sc_ic.ic_channels[149]={5745,0}; sc.sc_ic.ic_chan_active[149/8]|=1U<<(149%8);
    sc.sc_ic.ic_channels[6]={2437,0}; sc.sc_ic.ic_chan_active[6/8]|=1U<<(6%8);
    qwz_mon_native_auto_tick();
    assert(starts==0 && sc.mon_coex_task.adds==1 && sc.mon_coex_task_why==8);
    qwz_mon_native_auto_tick(); assert(sc.mon_coex_task.adds==1); // coalesce/retry throttle
    sc.scan.state=1;
    qwz_mon_native_auto_start(&sc); assert(starts==0); // recheck in worker
    sc.scan.state=0;
    sc.ioctl_rwl.held=1;
    qwz_mon_native_auto_start(&sc); assert(starts==0); // never wait behind an ioctl
    sc.ioctl_rwl.held=0;
    qwz_mon_native_auto_start(&sc);
    assert(starts==1 && chosen==149 && sc.mon_native_owned && sc.mon_active);
    assert(qwz_mon_flags==1291 && qwz_moncoex && qwz_moncoexmcc);
    assert(qwz_moncoexquota==0 && qwz_moncoexfilter==0);
    assert(qwz_moncoexdwell==100 && qwz_moncoexrest==100 && qwz_moncoexdwellflags==7);
    qwz_mon_native_auto_tick(); assert(starts==1 && sc.mon_coex_task.adds==1);
    // Hotspot scanning releases the radio without disabling automatic mode.
    assert(qwz_mon_native_pause(&sc.sc_ic)==0);
    assert(!sc.mon_active && sc.mon_native_auto && !sc.mon_native_manual);
    qwz_mon_native_auto_tick(); assert(sc.mon_coex_task.adds==1);
    qwz_mon_native_auto_start(&sc); assert(starts==1); // queued old start cannot violate pause
    now+=13000000000ULL;
    sc.sc_ic.ic_wcl_join_requested=true;
    qwz_mon_native_auto_start(&sc); assert(starts==1);
    sc.sc_ic.ic_wcl_join_requested=false;
    sc.sc_ic.ic_wcl_scan_requested=true;
    qwz_mon_native_auto_start(&sc); assert(starts==1);
    sc.sc_ic.ic_wcl_scan_requested=false;
    qwz_mon_native_auto_start(&sc); assert(starts==2 && sc.mon_active);
    // Explicit sysctl stop opts out. A later timer must not restart the cell.
    int result=-1;
    qwz_mon_gated(nullptr,nullptr,&result,(void*)1,nullptr);
    assert(result==0 && !sc.mon_active && sc.mon_native_manual);
    now+=6000000000ULL;
    qwz_mon_native_auto_tick(); assert(sc.mon_coex_task.adds==1);
    // New native registration/reboot re-arms automatic operation.
    qwz_mon_native_auto_enable(1);
    sc.sc_ic.ic_channels[149].ic_flags=IEEE80211_CHAN_PASSIVE;
    qwz_mon_native_auto_start(&sc); assert(chosen==6 && sc.mon_native_owned);
    qwz_mon_set_native_rx(nullptr,nullptr);
    assert(!sc.mon_native_auto && (sc.mon_coex_task_why&1)); // teardown requests stop
    qwz_mon_stop(&sc);
    qwz_mon_set_native_rx(rx,&sc); qwz_mon_native_auto_enable(1);
    sc.sc_ic.ic_channels[6].ic_flags=IEEE80211_CHAN_DFS;
    const int before=starts;
    qwz_mon_native_auto_start(&sc); assert(starts==before && !sc.ioctl_rwl.held);
    assert(strcmp(qwz_mon_native_auto_state(),"no-active-channel")==0);
    sc.sc_ic.ic_channels[149].ic_flags=0;
    start_result=EIO;
    qwz_mon_native_auto_start(&sc); assert(!sc.mon_native_owned && !sc.ioctl_rwl.held);
    start_result=0;
    sc.unavailable=1;
    qwz_mon_native_auto_start(&sc); assert(starts==before+1);
    sc.unavailable=0; cancel_during_start=1;
    qwz_mon_native_auto_start(&sc); assert(!sc.mon_active && !sc.mon_native_owned && !sc.ioctl_rwl.held);
    // Native teardown after scheduling cancels the queued start too.
    cancel_during_start=0; qwz_mon_native_auto_enable(1);
    qwz_mon_set_native_rx(nullptr,nullptr);
    const int canceled=starts;
    qwz_mon_native_auto_start(&sc); assert(starts==canceled);
    qwz_mon_set_native_rx(rx,&sc); qwz_mon_native_auto_enable(0);
    qwz_mon_native_auto_tick(); assert(starts==canceled);
    puts("ok: automatic start waits for Wi-Fi, uses deferred work, respects manual stop and channel availability, cancels safely");
}
'''
with tempfile.TemporaryDirectory(prefix="qwz-native-auto-") as directory:
    path=Path(directory)/"test.cpp"; output=Path(directory)/"test"
    path.write_text(prefix+"\n"+"\n".join(function(name) for name in names)+main)
    subprocess.run(["xcrun","clang++","-std=c++14","-Wall","-Wextra","-Werror",
                    "-fsanitize=address,undefined",str(path),"-o",str(output)],check=True)
    subprocess.run([str(output)],check=True)
