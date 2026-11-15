#!/usr/bin/env python3
"""Replay overlapping native scan requests through production WCL methods."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'airport/WCL/WCL.cpp').read_text()

def function(marker):
    start=source.index(marker);end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

methods='\n'.join(function(marker) for marker in (
    'IOReturn AirportItlwmSkywalkInterface::beginWCLScan(',
    'void AirportItlwmSkywalkInterface::finishWCLScan(',
    'void AirportItlwmSkywalkInterface::completeWCLScan('))
event=source[source.index('        case IEEE80211_EVT_SCAN_DONE:'):source.index('        case IEEE80211_EVT_STA_ASSOC_DONE:')]
prefix=r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "WCL.hpp"
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNotReady=2,kIOReturnBusy=3;
constexpr int kIOReturnUnsupported=4,kIOReturnError=5,kIOReturnAborted=6,kIOReturnTimeout=7;
constexpr int IFF_RUNNING=1,IEEE80211_F_BGSCAN=2,IEEE80211_S_INIT=0,IEEE80211_S_SCAN=1;
constexpr int IEEE80211_S_AUTH=2,IEEE80211_S_ASSOC=3,IEEE80211_S_RUN=4,IEEE80211_CHAN_MAX=255;
constexpr int IEEE80211_EVT_SCAN_DONE=5;
const char *ieee80211_state_name[] = {"INIT","SCAN","AUTH","ASSOC","RUN"};
using apple80211ScanRequest=uint8_t;
struct ieee80211com;
struct ifnet {int if_flags=IFF_RUNNING;ieee80211com *owner;};
struct ieee80211com {
    ifnet ic_if{IFF_RUNNING,this};int ic_state=IEEE80211_S_SCAN,ic_flags=0;
    bool ic_wcl_scan_idle=false,ic_wcl_scan_requested=false,ic_wcl_scan_active=false;
    bool ic_wcl_scan_restricted=false;
    uint8_t ic_wcl_scan_ssid_length=0,ic_wcl_scan_ssid[32]{},ic_wcl_scan_ssid_count=0;
    uint8_t ic_wcl_scan_ssid_lengths[11]{},ic_wcl_scan_ssids[11][32]{},ic_wcl_scan_channels[32]{};
    unsigned ic_des_esslen=0;
    struct {int ic_flags=1;} ic_channels[256];
    int (*ic_newstate)(ieee80211com *,int,int)=nullptr;
};
static uint64_t nowMS=100,wclScanStartMs;
static uint32_t wclScanStartId;
static uint64_t wclScanNowMs(){return nowMS;}
static int qwz_mon_native_pause(ieee80211com *){return ENOTSUP;}
static int qwz_mon_scan_hold(ieee80211com *){return 0;}
static int qwz_mon_holding(ieee80211com *){return 0;}
// SD (f2v): whether a native AirDrop transfer is running, and the switch.
static int transferRunning,busyAsked,qwz_nativescanrest=120;
static int qwz_mon_native_busy(){++busyAsked;return transferRunning;}
static bool wclScanDecodeFault(const void *,char *,size_t){return false;}
struct WCLScanLogState {bool scanPending,joinPending,skip,bgscan;int state,idle,prevSsidLength;};
template<class... T> static void logWCLScanRequest(T...) {}
template<class... T> static void qwz_log(const char *,T...) {}
static bool qwz_log_ratelimit(unsigned *,unsigned,unsigned){return false;}
#define QWZLOG(...) ((void)0)
static unsigned submissions;
static uint8_t sentNames,sentLength,sentSSID[32],sentChannels[32];
static void ieee80211_next_scan(ifnet *ifp) {
    auto ic=ifp->owner;++submissions;sentNames=ic->ic_wcl_scan_ssid_count;
    sentLength=ic->ic_wcl_scan_ssid_lengths[0];
    memcpy(sentSSID,ic->ic_wcl_scan_ssids[0],32);
    memcpy(sentChannels,ic->ic_wcl_scan_channels,32);
}
static bool declineBG;
static void ieee80211_begin_cache_bgscan(ifnet *ifp) {
    if(!declineBG){ifp->owner->ic_flags|=IEEE80211_F_BGSCAN;ieee80211_next_scan(ifp);}
}
static int newstate(ieee80211com *ic,int state,int) {
    ic->ic_state=state;ieee80211_next_scan(&ic->ic_if);return 0;
}
struct Timer {
    std::vector<unsigned> delays;unsigned cancellations=0;
    void setTimeoutMS(unsigned ms){delays.push_back(ms);}
    void cancelTimeout(){++cancellations;}
};
struct HAL {ieee80211com ic;ieee80211com *get80211Controller(){return &ic;}};
struct Driver {
    std::vector<int> results;
    template<class T>void postMessage(T *,unsigned,const void *value,size_t,bool){results.push_back(*static_cast<const int *>(value));}
};
struct AirportItlwmSkywalkInterface {
    HAL hal;HAL *fHalService=&hal;Timer timer;Timer *scanSource=&timer;Driver driver;Driver *instance=&driver;
    bool scanPending=false,scanCompleted=false,scanSkipCompletion=false,scanRestartPending=false;
    bool joinPending=false,wclStopping=false;
    bool wclScanUsedRadio=false;uint64_t wclLastRadioScanMs=0;   // SD (f2v)
    AirportItlwmSkywalkInterface(){hal.ic.ic_newstate=newstate;}
    IOReturn beginWCLScan(apple80211ScanRequest *);
    void finishWCLScan(IOReturn);void completeWCLScan();
    void leaveUnauthorisedRun(){} // JL (join lifecycle): covered by test_join_failure_leave.py
    void event(int code){switch(code){
''' + event + r'''
    default:break;}}
};
'''
main=r'''
static void request(uint8_t *p) {
    memset(p,0,0x1550);p[0]=7;p[0x40]=1;p[0x54]=1;
    p[0x58+4]=44;p[0x58+8]=0x10;
    p[0x1318]=2;p[0x131c]=1;p[0x1320]=7;memcpy(p+0x1324,"example",7);
}
int main() {
    uint8_t raw[0x1550];request(raw);
    AirportItlwmSkywalkInterface d;auto &ic=d.hal.ic;
    submissions=1; // One older firmware scan is already running.
    assert(d.beginWCLScan(raw)==kIOReturnSuccess);
    assert(submissions==1 && d.scanPending && d.scanSkipCompletion && !d.scanRestartPending);
    assert(ic.ic_wcl_scan_ssid_length==0 && ic.ic_wcl_scan_ssid_count==2);
    // A second request is refused without replacing this accepted request.
    raw[0x1324]='x';assert(d.beginWCLScan(raw)==kIOReturnBusy);
    assert(ic.ic_wcl_scan_ssids[0][0]=='e');raw[0x1324]='e';
    nowMS+=700;d.event(IEEE80211_EVT_SCAN_DONE);
    assert(d.scanRestartPending && d.driver.results.empty() && !d.scanCompleted);
    d.completeWCLScan(); // Callback before old end_scan has released ownership.
    assert(submissions==1 && !ic.ic_wcl_scan_idle && d.timer.delays.back()==10);
    ic.ic_wcl_scan_idle=true;d.completeWCLScan();
    assert(submissions==2 && !d.scanRestartPending && !ic.ic_wcl_scan_idle);
    assert(sentNames==2 && sentLength==7 && !memcmp(sentSSID,"example",7));
    assert(sentChannels[44/8]&(1<<(44%8)));
    assert(d.timer.delays.back()==19300 && d.driver.results.empty());
    d.event(IEEE80211_EVT_SCAN_DONE);ic.ic_wcl_scan_idle=true;d.completeWCLScan();
    assert(d.driver.results.size()==1 && d.driver.results[0]==kIOReturnSuccess);
    assert(!d.scanPending && !ic.ic_wcl_scan_requested);
    d.completeWCLScan();assert(d.driver.results.size()==1);
    // No invented completion or new submission if the old sweep never ends.
    AirportItlwmSkywalkInterface timeout;submissions=0;timeout.beginWCLScan(raw);
    nowMS+=20000;timeout.completeWCLScan();
    assert(submissions==0 && timeout.driver.results[0]==kIOReturnTimeout);
    // Delayed net80211 release obeys the original request deadline too.
    AirportItlwmSkywalkInterface slow;slow.beginWCLScan(raw);slow.event(IEEE80211_EVT_SCAN_DONE);
    nowMS+=20000;slow.completeWCLScan();assert(slow.driver.results[0]==kIOReturnTimeout);
    // An intervening state change cancels instead of starting a stale scan.
    AirportItlwmSkywalkInterface moved;moved.beginWCLScan(raw);moved.event(IEEE80211_EVT_SCAN_DONE);
    moved.hal.ic.ic_state=IEEE80211_S_AUTH;moved.completeWCLScan();
    assert(moved.driver.results[0]==kIOReturnAborted);
    // Abort clears deferred ownership. Late events cannot resurrect work.
    AirportItlwmSkywalkInterface aborted;aborted.beginWCLScan(raw);aborted.event(IEEE80211_EVT_SCAN_DONE);
    unsigned before=submissions;aborted.finishWCLScan(kIOReturnAborted);
    aborted.hal.ic.ic_wcl_scan_idle=true;aborted.event(IEEE80211_EVT_SCAN_DONE);aborted.completeWCLScan();
    assert(submissions==before && !aborted.scanRestartPending && aborted.driver.results.size()==1);
    // Normal idle, INIT and associated background paths launch immediately.
    for(int state:{IEEE80211_S_SCAN,IEEE80211_S_INIT,IEEE80211_S_RUN}) {
        AirportItlwmSkywalkInterface normal;normal.hal.ic.ic_state=state;normal.hal.ic.ic_wcl_scan_idle=true;
        before=submissions;assert(normal.beginWCLScan(raw)==kIOReturnSuccess);
        assert(submissions==before+1 && !normal.scanSkipCompletion && !normal.scanRestartPending);
        normal.event(IEEE80211_EVT_SCAN_DONE);normal.completeWCLScan();
        assert(normal.driver.results.size()==1 && normal.driver.results[0]==kIOReturnSuccess);
    }
    // SD (f2v): while a native AirDrop transfer runs, an undirected background scan of the associated station is
    // answered from the node table; the radio is taken at most once in debug.qwz.nativescanrest seconds.
    {
        uint8_t plain[0x1550];memset(plain,0,sizeof(plain));plain[0]=9;plain[0x40]=1;   // no SSID, no list, no channels, no BSSID
        AirportItlwmSkywalkInterface t;auto &tc=t.hal.ic;tc.ic_state=IEEE80211_S_RUN;
        auto radioScan=[&](uint8_t *request) {   // one scan that takes the radio and completes
            const unsigned before=submissions;
            assert(t.beginWCLScan(request)==kIOReturnSuccess && submissions==before+1 && t.scanPending && t.wclScanUsedRadio);
            t.event(IEEE80211_EVT_SCAN_DONE);tc.ic_flags&=~IEEE80211_F_BGSCAN;t.completeWCLScan();
            assert(!t.scanPending && !t.wclScanUsedRadio && t.driver.results.back()==kIOReturnSuccess);
        };
        auto answered=[&](uint8_t *request) {    // answered at once, the radio untouched
            const unsigned before=submissions;const size_t results=t.driver.results.size();
            assert(t.beginWCLScan(request)==kIOReturnSuccess && submissions==before);
            assert(!t.scanPending && !tc.ic_wcl_scan_requested && !(tc.ic_flags&IEEE80211_F_BGSCAN));
            assert(t.driver.results.size()==results+1 && t.driver.results.back()==kIOReturnSuccess);
        };
        // No radio scan has completed yet: the first one runs even during a transfer (the table would be empty).
        transferRunning=1;radioScan(plain);
        const uint64_t fresh=t.wclLastRadioScanMs;assert(fresh==nowMS);
        // During the transfer the next ones are answered from the table, and the table's age is not renewed by them.
        nowMS+=1000;answered(plain);nowMS+=60000;answered(plain);assert(t.wclLastRadioScanMs==fresh);
        // Once the last radio scan is nativescanrest old, one real scan runs again.
        nowMS+=59000;radioScan(plain);assert(t.wclLastRadioScanMs==nowMS);
        nowMS+=5000;answered(plain);
        // No transfer: every scan takes the radio, as before.
        transferRunning=0;radioScan(plain);
        // Switched off: as up to f2u, transfer or not.
        transferRunning=1;qwz_nativescanrest=0;nowMS+=1000;radioScan(plain);qwz_nativescanrest=120;
        // Never answered this way: a scan for an SSID, a hotspot list, one BSSID, or a scan while not associated.
        nowMS+=1000;
        {uint8_t r[0x1550];memcpy(r,plain,sizeof(r));r[0x1c]=4;memcpy(r+0x20,"abcd",4);radioScan(r);}
        nowMS+=1000;{uint8_t r[0x1550];request(r);radioScan(r);}
        nowMS+=1000;{uint8_t r[0x1550];memcpy(r,plain,sizeof(r));
            AirportWCL::ScanRequest probe;assert(AirportWCL::decodeScan(r,0x1550,probe));
            // the BSSID field of the request: find it by decoding, so the test follows WCL.hpp's layout
            for(unsigned off=4;off<0x58;++off){memcpy(r,plain,sizeof(r));r[off]=0x5a;
                AirportWCL::ScanRequest q;if(AirportWCL::decodeScan(r,0x1550,q)&&q.bssid[0]==0x5a){radioScan(r);break;}
                assert(off<0x57);}}
        nowMS+=1000;tc.ic_state=IEEE80211_S_SCAN;tc.ic_wcl_scan_idle=true;
        {const unsigned before=submissions;assert(t.beginWCLScan(plain)==kIOReturnSuccess && submissions==before+1 && t.wclScanUsedRadio);
         t.finishWCLScan(kIOReturnAborted);assert(!t.wclScanUsedRadio);}
        // A scan the radio refused or that did not complete does not count as a fresh table.
        tc.ic_state=IEEE80211_S_RUN;tc.ic_wcl_scan_idle=false;transferRunning=0;
        const uint64_t last=t.wclLastRadioScanMs;nowMS+=1000;
        declineBG=true;assert(t.beginWCLScan(plain)==kIOReturnBusy && !t.wclScanUsedRadio && t.wclLastRadioScanMs==last);declineBG=false;
        assert(t.beginWCLScan(plain)==kIOReturnSuccess);nowMS+=20000;t.completeWCLScan();
        assert(t.driver.results.back()==kIOReturnTimeout && t.wclLastRadioScanMs==last && !t.wclScanUsedRadio);
        assert(busyAsked>0);
    }
    puts("actual WCL list-only overlap, real completion, deferred restart, deadline and abort: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='qwz-scan-restart-') as directory:
    src=Path(directory)/'test.cpp';exe=Path(directory)/'test'
    src.write_text(prefix+methods+main)
    subprocess.run(['xcrun','clang++','-std=c++14','-I'+str(root/'airport/WCL'),'-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
