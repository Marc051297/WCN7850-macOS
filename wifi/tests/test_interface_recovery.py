#!/usr/bin/env python3
"""Run the actual init wrapper and recovery engine against lifecycle failures."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
controller=(root/'airport/AirportItlwmV2.cpp').read_text()
qwz=(root/'qwz/qwz.c').read_text()

def body(source,marker):
    start=source.index(marker)
    end=source.index('{',start)+1
    depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]

functions='\n'.join(body(controller,marker) for marker in (
    'bool AirportItlwm::interfaceNeedsRecovery()',
    'void AirportItlwm::requestInterfaceRecovery(',
    'void AirportItlwm::engineRun()',
    'void AirportItlwm::engineNoteFailure(',
    'void AirportItlwm::engineGiveUp('))
wrapper=body(qwz,'int\nqwz_init(struct ifnet *ifp)')
prefix=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <vector>
#include <qwz_life.h>
static uint64_t clockMS=100000;
int qwz_recover=1,qwz_m0_on=1;
static uint64_t pmNowMs(){return clockMS;}
template<class... T> static void qwz_log(const char *,T...) {}
template<class... T> static void qwz_logc(const char *,T...) {}
static void qwz_log_publish() {}
static bool qwz_log_ratelimit(unsigned *,unsigned,unsigned){return true;}
#define QWZ_CRUMB(x) ((void)0)
const char *const qwz_dead_names[]={"none","RDDM","SYS_ERR","LINK","WMI_SILENT","DEADLINE","INJECT","USER","INTERFACE_INIT"};
constexpr unsigned IFF_UP=1,IFF_RUNNING=2;
constexpr unsigned kEngDead=1,kEngRecover=2,kEngInterface=4;
constexpr unsigned kWakeViaDead=6,QWZ_FAIL_FIRST=3,QWZ_FAIL_FINAL=5;
constexpr uint64_t QWZ_COOLDOWN_MS=20000,QWZ_FLAP_MS=60000,QWZ_CHIP_WAIT_MS=10000;
struct MockHAL {
    struct {struct {unsigned if_flags=IFF_UP;} ic_if;bool ic_wcl_mac_reconfig=true;} ic;
    bool fault=false,wake=false,firmware=true;
    unsigned recovered=0,engineUpdates=0;
    auto get80211Controller()->decltype(&ic){return &ic;}
    bool faultPending(uint32_t *r){if(r)*r=QWZ_DEAD_RDDM;return fault;}
    bool wakeNeedsRecovery(){return wake;}
    bool firmwareUp(){return firmware;}
    void noteRecovered(unsigned){++recovered;}
    void setEngineState(const char *,unsigned,bool){++engineUpdates;}
};
struct AirportItlwm {
    MockHAL hal;MockHAL *fHalService=&hal;
    bool engineBusy=false,engGivenUp=false,engDeferred=false,chipBusy=false;
    bool noWakeFix=false,awake=true,downAllowed=true,wakeWorks=true,offDuringDown=false;
    uint32_t engineReq=0,engineReason=0,engReason=0,engAbsorbed=0,engChipBusy=0;
    uint32_t interfaceInitFailures=0,engFailCont=0,engEpisodes=0;
    uint64_t engCooldownMs=0,engLastOkMs=0,engBeginMs=0;
    const char *chipBusyWho="test";
    unsigned downs=0,wakes=0,requests=0,lastError=0;
    std::vector<unsigned> delays;
    bool wakeAllowed(){return awake;}
    void setProperty(const char *name,uint64_t value,unsigned){if(!strcmp(name,"QwzLastInterfaceInitError"))lastError=value;}
    void engineRequest(unsigned bits,unsigned reason){engineReq|=bits;engineReason=reason;++requests;}
    void scheduleEngine(unsigned ms){delays.push_back(ms);}
    bool chipDown(const char *,const char *,uint32_t,uint64_t){
        ++downs;if(!downAllowed)return false;
        hal.wake=true;hal.firmware=false;if(offDuringDown)awake=false;return true;
    }
    void wakeRecover(unsigned){++wakes;if(wakeWorks){hal.wake=false;hal.firmware=true;hal.ic.ic_if.if_flags=IFF_UP|IFF_RUNNING;}}
    bool interfaceNeedsRecovery();
    void requestInterfaceRecovery(int);
    void engineRun();void engineNoteFailure(const char *);void engineGiveUp(const char *);
};
'''
# The actual wrapper must carry a real errno through the event, not claim Ready.
wrapper_prefix=r'''
struct ieee80211com {int ic_state=0;void (*ic_event_handler)(ieee80211com *,int,void *)=nullptr;};
struct qwz_softc {ieee80211com sc_ic;int fw_initialized=1;unsigned sc_fw_epoch=1;};
struct ifnet {void *if_softc;unsigned if_flags=IFF_UP;};
constexpr int IEEE80211_EVT_DRIVER_RESET_BEGIN=8,IEEE80211_EVT_DRIVER_STOPPED=7;
constexpr int IEEE80211_EVT_DRIVER_READY=6,IEEE80211_EVT_DRIVER_INIT_FAILED=9;
static int init_result;
static int qwz_init_impl(ifnet *){return init_result;}
static std::vector<int> events,values;
static void event(ieee80211com *,int code,void *data){events.push_back(code);values.push_back(int(uintptr_t(data)));}
'''
main=r'''
int main(){
    qwz_softc sc;sc.sc_ic.ic_event_handler=event;ifnet ifp{&sc,IFF_UP};
    init_result=55;assert(qwz_init(&ifp)==55);
    assert((events==std::vector<int>{8,7,9}) && values.back()==55);
    events.clear();values.clear();init_result=0;assert(qwz_init(&ifp)==0);
    assert((events==std::vector<int>{8,6}));
    // No declared firmware fault, but an UP interface failed before RUNNING.
    AirportItlwm d;d.requestInterfaceRecovery(55);
    assert(d.engineReq==kEngInterface && d.lastError==55 && !d.hal.fault);
    d.engineRun();
    assert(d.downs==1 && d.wakes==1 && d.hal.recovered==1 && !d.hal.fault);
    assert(!d.hal.ic.ic_wcl_mac_reconfig && !d.engFailCont && d.engReason==QWZ_DEAD_INIT);
    // A manual power-off or successful intervening enable must win.
    AirportItlwm off;off.hal.ic.ic_if.if_flags=0;off.requestInterfaceRecovery(55);
    assert(!off.engineReq);off.engineReq=kEngInterface;off.engineRun();assert(!off.downs);
    AirportItlwm healthy;healthy.requestInterfaceRecovery(55);healthy.hal.ic.ic_if.if_flags|=IFF_RUNNING;
    healthy.engineRun();assert(!healthy.downs);
    AirportItlwm sleep;sleep.awake=false;sleep.requestInterfaceRecovery(55);assert(!sleep.engineReq);
    AirportItlwm owned;owned.chipBusy=true;owned.requestInterfaceRecovery(55);assert(!owned.engineReq);
    // Single flight does not consume the queued request; cooldown and a
    // competing chip transition preserve it for the next scheduled run.
    AirportItlwm wait;wait.requestInterfaceRecovery(55);wait.engineBusy=true;wait.engineRun();
    assert(wait.engineReq==kEngInterface && !wait.downs);wait.engineBusy=false;
    wait.engCooldownMs=clockMS+1000;wait.engineRun();
    assert(wait.engineReq==kEngInterface && wait.delays.back()==1000 && !wait.downs);
    clockMS+=1001;wait.chipBusy=true;wait.engineRun();
    assert(wait.engineReq==kEngInterface && !wait.downs);wait.chipBusy=false;wait.engineRun();assert(wait.downs==1);
    // Failed teardown retains a non-fault request. Failed real bringups use
    // the existing 2s/4s/20s backoff and stop at five attempts.
    AirportItlwm busy;busy.requestInterfaceRecovery(55);busy.downAllowed=false;busy.engineRun();
    assert(busy.engineReq==kEngInterface && busy.engFailCont==1);
    AirportItlwm fail;fail.wakeWorks=false;fail.requestInterfaceRecovery(55);
    for(unsigned n=1;n<=5;++n){fail.engineRun();assert(fail.engFailCont==n);clockMS=fail.engCooldownMs+1;}
    assert(fail.engGivenUp && fail.downs==5 && fail.wakes==5);
    fail.engineReq=kEngInterface;fail.engineRun();assert(fail.downs==5);
    AirportItlwm stopped;stopped.requestInterfaceRecovery(55);stopped.offDuringDown=true;stopped.engineRun();
    assert(stopped.downs==1 && !stopped.wakes && stopped.engDeferred);
    qwz_recover=0;AirportItlwm disabled;disabled.requestInterfaceRecovery(55);assert(!disabled.engineReq);
    puts("actual init errno, alive-firmware recovery, power precedence, request retention and bounded retries: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='qwz-interface-recovery-') as directory:
    src=Path(directory)/'test.cpp';exe=Path(directory)/'test'
    src.write_text(prefix+functions+wrapper_prefix+wrapper+main)
    subprocess.run(['xcrun','clang++','-std=c++14','-I'+str(root/'qwz'),
                    '-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
