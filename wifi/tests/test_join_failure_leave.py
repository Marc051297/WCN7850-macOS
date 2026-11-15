#!/usr/bin/env python3
"""JL (join lifecycle): a failed join or an AP deauthentication must leave the BSS; replayed through the production code.

The recorded failure (2026-11-15, hotspot): the AP deauthenticated during a join, the WCL glue wiped the key
but kept the desired network, net80211 re-authenticated by itself, the key handshake could never complete,
and RUN without a valid port refused every scan until Wi-Fi was switched off and on.

Part 1 compiles the actual WCL methods and event cases. Part 2 compiles the actual ieee80211_recv_deauth,
once as the WCL product and once without AIRPORT_WCL (the other product keeps OpenBSD's behaviour).
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
wcl = (root / 'airport/WCL/WCL.cpp').read_text()
net = (root / 'itl80211/openbsd/net80211/ieee80211_input.c').read_text()


def function(source, marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


methods = '\n'.join(function(wcl, marker) for marker in (
    'void AirportItlwmSkywalkInterface::finishWCLJoin(',
    'IOReturn AirportItlwmSkywalkInterface::leaveWCLNetwork(',
    'void AirportItlwmSkywalkInterface::forgetWCLNetwork(',
    'void AirportItlwmSkywalkInterface::leaveUnauthorisedRun('))
events = wcl[wcl.index('        case IEEE80211_EVT_STA_ASSOC_DONE:'):wcl.index('        default: break;')]

# The production call sites this test cannot execute are checked as text.
scan = function(wcl, 'IOReturn AirportItlwmSkywalkInterface::beginWCLScan(')
declined = scan.index('"bgscan declined in RUN"')
assert 'leaveUnauthorisedRun();' in scan[declined:scan.index('return kIOReturnBusy;', declined)], \
    'the refused-scan branch must call the guard'
join = function(wcl, 'IOReturn AirportItlwmSkywalkInterface::startWCLJoin(')
assert 'wclUnauthSinceMs = 0;' in join, 'a new join must restart the guard'

part1 = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <utility>
#include <vector>
#include "WCL.hpp"
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnError=5,kIOReturnTimeout=7,kIOReturnOffline=8;
constexpr int IEEE80211_S_INIT=0,IEEE80211_S_SCAN=1,IEEE80211_S_AUTH=2,IEEE80211_S_ASSOC=3,IEEE80211_S_RUN=4;
constexpr unsigned IEEE80211_F_PSK=1,IEEE80211_F_DESBSSID=2,IEEE80211_F_RSNON=4,IEEE80211_F_WEPON=8;
constexpr int IEEE80211_REASON_AUTH_LEAVE=3;
constexpr int IEEE80211_EVT_STA_ASSOC_DONE=1,IEEE80211_EVT_STA_DEAUTH=2;
const char *ieee80211_state_name[]={"INIT","SCAN","AUTH","ASSOC","RUN"};
struct ieee80211_channel {};
struct ieee80211_node {uint8_t ni_bssid[6]{};int ni_port_valid=0;ieee80211_channel *ni_chan=nullptr;};
struct ieee80211com {
    int ic_state=IEEE80211_S_SCAN;unsigned ic_flags=0;
    bool ic_wcl_join_requested=false,ic_wcl_enterprise=false;
    uint8_t ic_psk[32]{};unsigned ic_des_esslen=0;uint8_t ic_des_essid[32]{};
    uint16_t ic_deauth_reason=0,ic_assoc_status=0;
    ieee80211_node node;ieee80211_node *ic_bss=&node;
};
static std::vector<std::pair<int,int>> transitions;
static void ieee80211_new_state(ieee80211com *ic,int state,int arg){transitions.push_back({state,arg});ic->ic_state=state;}
static void ieee80211_disable_rsn(ieee80211com *ic){ic->ic_flags&=~(IEEE80211_F_PSK|IEEE80211_F_RSNON);memset(ic->ic_psk,0,sizeof(ic->ic_psk));}
static void ieee80211_disable_wep(ieee80211com *ic){ic->ic_flags&=~IEEE80211_F_WEPON;}
static int ieee80211_chan2ieee(ieee80211com *,ieee80211_channel *){return 44;}
static const char *ether_sprintf(const uint8_t *){return "bssid";}
static void explicit_bzero(void *p,size_t n){memset(p,0,n);}
template<class... T> static void qwz_log(const char *,T...) {}
static bool qwz_log_ratelimit(unsigned *,unsigned,unsigned){return false;}
static uint64_t nowMS=1000;
static uint64_t wclScanNowMs(){return nowMS;}
struct Timer {unsigned cancels=0;void cancelTimeout(){++cancels;}void setTimeoutMS(unsigned){}};
struct HAL {ieee80211com ic;ieee80211com *get80211Controller(){return &ic;}};
struct Message {unsigned id;int status;};
struct Driver {
    std::vector<Message> sent;
    template<class T>void postMessage(T *,unsigned id,const void *value,size_t,bool){
        int status=-1;
        if(id==unsigned(AirportWCL::ConnectComplete))status=static_cast<const AirportWCL::ConnectionEvent *>(value)->status;
        sent.push_back({id,status});
    }
};
struct AirportItlwmSkywalkInterface {
    HAL hal;HAL *fHalService=&hal;Timer timer;Timer *joinTimer=&timer;Driver driver;Driver *instance=&driver;
    bool joinPending=false,joinDeferred=false,assocNotified=false,wclBssValid=false,disassocIsVoluntary=false;
    AirportWCL::JoinRequest deferredJoin={};uint8_t joinBSSID[6]{};uint32_t wclAssociatedBeacons=0;unsigned pmksaClears=0;
    uint64_t wclUnauthSinceMs=0,wclUnauthLastMs=0;
    void clearWCLPMKSA(){++pmksaClears;}
    void finishWCLJoin(uint16_t,uint16_t,IOReturn);
    IOReturn leaveWCLNetwork();
    void forgetWCLNetwork(const char *);
    void leaveUnauthorisedRun();
    void event(int code){ieee80211com *ic=&hal.ic;switch(code){
''' + events + r'''
        default: break;}}
};
''' + methods + r'''
static void associate(AirportItlwmSkywalkInterface &d,int state,bool pending,bool portValid) {
    auto &ic=d.hal.ic;
    ic.ic_state=state;ic.ic_flags=IEEE80211_F_PSK|IEEE80211_F_DESBSSID|IEEE80211_F_RSNON;
    ic.ic_des_esslen=15;memset(ic.ic_des_essid,'n',15);memset(ic.ic_psk,0xaa,sizeof(ic.ic_psk));
    ic.ic_wcl_join_requested=pending;ic.node.ni_port_valid=portValid;
    d.joinPending=pending;d.assocNotified=state>=IEEE80211_S_RUN;
    transitions.clear();
}
static bool forgotten(AirportItlwmSkywalkInterface &d) {
    auto &ic=d.hal.ic;
    for(uint8_t byte:ic.ic_psk)if(byte)return false;
    return !(ic.ic_flags&(IEEE80211_F_PSK|IEEE80211_F_DESBSSID|IEEE80211_F_RSNON)) && ic.ic_des_esslen==0 &&
        !ic.ic_des_essid[0] && !ic.ic_wcl_join_requested && d.pmksaClears>0;
}
int main() {
    {   // The recorded failure: the AP deauthenticates during a pending join.
        AirportItlwmSkywalkInterface d;associate(d,IEEE80211_S_RUN,true,false);d.hal.ic.ic_deauth_reason=2;
        d.event(IEEE80211_EVT_STA_DEAUTH);
        assert(d.driver.sent.size()==2 && d.driver.sent[0].id==unsigned(AirportWCL::Deauth));
        assert(d.driver.sent[1].id==unsigned(AirportWCL::ConnectComplete) && d.driver.sent[1].status==1);
        assert(!d.joinPending && d.timer.cancels==1 && forgotten(d));
        assert(transitions.empty()); // The state change belongs to the stack, after the handler returns.
    }
    {   // An authorised association ended by the AP: macOS is told once and the network is forgotten too.
        AirportItlwmSkywalkInterface d;associate(d,IEEE80211_S_RUN,false,true);
        d.event(IEEE80211_EVT_STA_DEAUTH);
        assert(d.driver.sent.size()==1 && d.driver.sent[0].id==unsigned(AirportWCL::Deauth));
        assert(forgotten(d) && transitions.empty());
    }
    {   // The AP refuses the association: both completions carry the status and nothing stays configured.
        AirportItlwmSkywalkInterface d;associate(d,IEEE80211_S_ASSOC,true,false);d.hal.ic.ic_assoc_status=17;
        d.event(IEEE80211_EVT_STA_ASSOC_DONE);
        assert(d.driver.sent.size()==2 && d.driver.sent[0].id==unsigned(AirportWCL::AssocComplete));
        assert(d.driver.sent[1].id==unsigned(AirportWCL::ConnectComplete) && d.driver.sent[1].status==17);
        assert(!d.assocNotified && !d.joinPending && forgotten(d) && transitions.empty());
    }
    {   // A successful association keeps everything for the key handshake.
        AirportItlwmSkywalkInterface d;associate(d,IEEE80211_S_ASSOC,true,false);d.hal.ic.ic_assoc_status=0;
        d.event(IEEE80211_EVT_STA_ASSOC_DONE);
        assert(d.driver.sent.size()==1 && d.assocNotified && d.joinPending && !forgotten(d));
        assert(d.hal.ic.ic_psk[0]==0xaa && d.hal.ic.ic_des_esslen==15);
    }
    {   // The guard: refused scans in an unauthorised RUN that nobody owns end it after 8 s, not before.
        AirportItlwmSkywalkInterface d;associate(d,IEEE80211_S_RUN,false,false);
        nowMS=1000;d.leaveUnauthorisedRun();nowMS=5000;d.leaveUnauthorisedRun();nowMS=8900;d.leaveUnauthorisedRun();
        assert(transitions.empty() && !forgotten(d));
        nowMS=9000;d.leaveUnauthorisedRun();
        assert(transitions.size()==1 && transitions[0].first==IEEE80211_S_SCAN && transitions[0].second==-1);
        assert(forgotten(d) && d.wclUnauthSinceMs==0 && d.hal.ic.ic_deauth_reason==IEEE80211_REASON_AUTH_LEAVE);
        d.leaveUnauthorisedRun();assert(transitions.size()==1); // Now in SCAN: nothing more to leave.
    }
    {   // A valid port, a pending join or another state is never touched, and each ends the count.
        AirportItlwmSkywalkInterface d;associate(d,IEEE80211_S_RUN,false,false);
        nowMS=1000;d.leaveUnauthorisedRun();assert(d.wclUnauthSinceMs==1000);
        d.hal.ic.node.ni_port_valid=1;nowMS=20000;d.leaveUnauthorisedRun();
        assert(transitions.empty() && d.wclUnauthSinceMs==0 && !forgotten(d));
        d.hal.ic.node.ni_port_valid=0;d.joinPending=true;nowMS=40000;d.leaveUnauthorisedRun();
        nowMS=60000;d.leaveUnauthorisedRun();assert(transitions.empty() && d.wclUnauthSinceMs==0);
        d.joinPending=false;d.hal.ic.ic_state=IEEE80211_S_ASSOC;d.leaveUnauthorisedRun();assert(transitions.empty());
        d.hal.ic.ic_state=IEEE80211_S_RUN;d.hal.ic.ic_flags&=~IEEE80211_F_RSNON;nowMS=90000;d.leaveUnauthorisedRun();
        nowMS=99000;d.leaveUnauthorisedRun();assert(transitions.empty()); // An open network has no port to wait for.
    }
    {   // Refusals more than 15 s apart are separate episodes: an old transient is not added to a new one.
        AirportItlwmSkywalkInterface d;associate(d,IEEE80211_S_RUN,false,false);
        nowMS=1000;d.leaveUnauthorisedRun();nowMS=21000;d.leaveUnauthorisedRun();
        assert(transitions.empty() && d.wclUnauthSinceMs==21000);
        nowMS=28999;d.leaveUnauthorisedRun();assert(transitions.empty());
        nowMS=29000;d.leaveUnauthorisedRun();assert(transitions.size()==1 && forgotten(d));
    }
    puts("actual WCL deauthentication, refused association and unauthorised-RUN guard: PASS");
}
'''

deauth = function(net, 'void\nieee80211_recv_deauth(struct ieee80211com *ic, mbuf_t m,')
part2 = r'''
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <sys/types.h>
#include <utility>
#include <vector>
#define IEEE80211_STA_ONLY 1
struct mbuf {size_t len;unsigned char *data;};
typedef mbuf *mbuf_t;
#define mbuf_len(m) ((m)->len)
#define mtod(m,t) ((t)(m)->data)
#define LE_READ_2(p) ((u_int16_t)((p)[0]|((p)[1]<<8)))
#define XYLog(...) ((void)0)
#define DPRINTF(x) ((void)0)
struct ieee80211_frame {u_int8_t i_fc[2],i_dur[2],i_addr1[6],i_addr2[6],i_addr3[6],i_seq[2];};
enum {IEEE80211_S_INIT,IEEE80211_S_SCAN,IEEE80211_S_AUTH,IEEE80211_S_ASSOC,IEEE80211_S_RUN};
enum {IEEE80211_M_STA=1};
enum {IEEE80211_F_BGSCAN=1,IEEE80211_F_DISABLE_BG_AUTO_CONNECT=2,IEEE80211_F_STAYAUTH=4};
enum {IEEE80211_FC0_SUBTYPE_DEAUTH=0xc0,IEEE80211_EVT_STA_DEAUTH=2};
struct ieee80211_node {};
struct ieee80211com {
    u_int16_t ic_deauth_reason=0;int ic_opmode=IEEE80211_M_STA,ic_state=IEEE80211_S_RUN;
    unsigned ic_flags=0,ic_userflags=0;struct {unsigned is_rx_deauth=0;} ic_stats;
    void (*ic_event_handler)(ieee80211com *,int,void *)=nullptr;
};
static std::vector<std::pair<int,int>> transitions;
static int handlerCalls,stateAtHandler,transitionsAtHandler;
static void handler(ieee80211com *ic,int code,void *){
    assert(code==IEEE80211_EVT_STA_DEAUTH);++handlerCalls;stateAtHandler=ic->ic_state;transitionsAtHandler=int(transitions.size());
}
static void ieee80211_new_state(ieee80211com *ic,int state,int arg){transitions.push_back({state,arg});ic->ic_state=state;}
template<class... T> static int qwz_log(const char *,T...) {return 0;}
static bool qwz_log_ratelimit(unsigned *,unsigned,unsigned){return true;}
''' + deauth + r'''
static void deliver(ieee80211com &ic,int state,unsigned flags=0) {
    unsigned char frame[26]={0xc0,0};frame[24]=2;frame[25]=0;
    mbuf m={sizeof(frame),frame};ieee80211_node node;
    ic.ic_state=state;ic.ic_flags=flags;ic.ic_event_handler=handler;transitions.clear();handlerCalls=0;
    ieee80211_recv_deauth(&ic,&m,&node);
    assert(handlerCalls==1 && stateAtHandler==state && transitionsAtHandler==0 && ic.ic_deauth_reason==2);
}
int main() {
    ieee80211com ic;
#ifdef AIRPORT_WCL
    for(int state:{IEEE80211_S_RUN,IEEE80211_S_ASSOC,IEEE80211_S_AUTH}) {
        deliver(ic,state);
        assert(transitions.size()==1 && transitions[0].first==IEEE80211_S_SCAN && transitions[0].second==-1);
    }
    // Also during a background scan, with or without the roaming flag: the AP has ended the association.
    deliver(ic,IEEE80211_S_RUN,IEEE80211_F_BGSCAN);assert(transitions.size()==1 && transitions[0].first==IEEE80211_S_SCAN);
    deliver(ic,IEEE80211_S_RUN,IEEE80211_F_BGSCAN|IEEE80211_F_DISABLE_BG_AUTO_CONNECT);assert(transitions[0].first==IEEE80211_S_SCAN);
    // A stray frame while scanning or down changes nothing (and never starts an authentication).
    deliver(ic,IEEE80211_S_SCAN);assert(transitions.empty());
    deliver(ic,IEEE80211_S_INIT);assert(transitions.empty());
    puts("actual ieee80211_recv_deauth as AIRPORT_WCL: reported first, then the scan state, never re-authentication: PASS");
#else
    deliver(ic,IEEE80211_S_RUN);
    assert(transitions.size()==1 && transitions[0].first==IEEE80211_S_AUTH && transitions[0].second==IEEE80211_FC0_SUBTYPE_DEAUTH);
    deliver(ic,IEEE80211_S_RUN,IEEE80211_F_BGSCAN);assert(transitions.empty()); // OpenBSD ignores it during a roaming scan.
    puts("actual ieee80211_recv_deauth without AIRPORT_WCL: OpenBSD behaviour unchanged: PASS");
#endif
}
'''

with tempfile.TemporaryDirectory(prefix='qwz-join-leave-') as directory:
    flags = ['xcrun', 'clang++', '-std=c++14', '-fsanitize=address,undefined']
    src = Path(directory) / 'wcl.cpp'
    exe = Path(directory) / 'wcl'
    src.write_text(part1)
    subprocess.run(flags + ['-I' + str(root / 'airport/WCL'), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    src = Path(directory) / 'deauth.cpp'
    src.write_text(part2)
    for name, define in (('deauth-wcl', ['-DAIRPORT_WCL']), ('deauth-plain', [])):
        exe = Path(directory) / name
        subprocess.run(flags + define + [str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
