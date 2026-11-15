#!/usr/bin/env python3
"""JL (join lifecycle): a join scans the channel it names first; an unacknowledged authentication is retried at once.

Recorded on 2026-11-15: every join swept 41 channels for 3.8 s although the request named the channel, a phone
hotspot on channel 44 was not found in six sweeps, and two joins lost their first authentication frame and waited
out the stack's 5 s timer. All three parts compile the production code: the chanspec helper, the hint block and
channel list of qwz_scan, the completion snippets of the management TX path, and ieee80211_watchdog.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
qwz = (root / 'qwz/qwz.c').read_text()
wcl = (root / 'airport/WCL/WCL.cpp').read_text()
net = (root / 'itl80211/openbsd/net80211/ieee80211.c').read_text()


def between(source, first, last, include_last=False):
    start = source.index(first)
    end = source.index(last, start)
    return source[start:end + (len(last) if include_last else 0)]


def function(source, marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


# The join request's channel reaches the scan: set from the request before the request is wiped.
join = function(wcl, 'IOReturn AirportItlwmSkywalkInterface::startWCLJoin(')
assert join.index('AirportWCL::joinHintChannel(join.channelSpec)') < join.index('explicit_bzero(&join, sizeof(join));\n    wclBssValid')

hint = between(qwz, '#ifdef AIRPORT_WCL\n\t/*\n\t * JL (join lifecycle), debug.qwz.joinhint', '#endif\n', True)
scan = qwz[qwz.index(hint):]  # the channel list of the same function, not an earlier one
channels = between(scan, '\tlastc = &ic->ic_channels[IEEE80211_CHAN_MAX];\n', '#if 0\n\tif (req->flags & NL80211_SCAN_FLAG_RANDOM_ADDR)')
assert channels.count('if (join_hint && ieee80211_mhz2ieee(chan->ic_freq, 0) != join_hint)') == 2

scan_test = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "WCL.hpp"
#define AIRPORT_WCL 1
constexpr int IEEE80211_CHAN_MAX=255,IEEE80211_S_SCAN=1,IEEE80211_S_RUN=4,M_DEVBUF=0,M_NOWAIT=0,M_ZERO=0;
constexpr unsigned IEEE80211_CHAN_PASSIVE=0x200,CHAN_2G=1,CHAN_5G=2;
struct ieee80211_channel {uint16_t ic_freq=0;unsigned ic_flags=0;};
#define IEEE80211_IS_CHAN_2GHZ(c) (((c)->ic_flags&CHAN_2G)!=0)
#define IEEE80211_IS_CHAN_5GHZ(c) (((c)->ic_flags&CHAN_5G)!=0)
static int ieee80211_mhz2ieee(unsigned freq,int){return freq<3000?(freq-2407)/5:(freq-5000)/5;}
struct ieee80211com {
    bool ic_wcl_join_requested=false,ic_wcl_scan_requested=false,ic_wcl_scan_restricted=false;
    uint8_t ic_wcl_join_chan=0,ic_wcl_scan_channels[32]{};int ic_state=IEEE80211_S_SCAN;
    ieee80211_channel ic_channels[IEEE80211_CHAN_MAX+1];
    ieee80211com(){
        for(int c=1;c<=13;++c)ic_channels[c]={uint16_t(2407+5*c),CHAN_2G};
        for(int c:{36,40,44,48,149,153,157,161,165})ic_channels[c]={uint16_t(5000+5*c),CHAN_5G};
        ic_channels[44].ic_flags|=IEEE80211_CHAN_PASSIVE;
    }
};
struct Scan {unsigned dwell_time_active=50,dwell_time_passive=150,num_chan=0;uint32_t *chan_list=nullptr;};
static int qwz_joinhint=1;
template<class... T> static void qwz_log(const char *,T...) {}
struct AnyPointer {void *p;template<class T> operator T *() const {return static_cast<T *>(p);}};
static AnyPointer mallocarray(size_t n,size_t size,int,int){return {calloc(n,size)};}
static int submit(ieee80211com *ic,Scan *arg) {
    int ret=0,num_channels,i,scan_2ghz=1,scan_5ghz=1,join_hint=0;
    ieee80211_channel *chan,*lastc;
''' + hint + channels + r'''
exit:return ret;
}
static unsigned total(const ieee80211com &ic){unsigned n=0;for(const auto &c:ic.ic_channels)n+=c.ic_flags!=0;return n;}
int main() {
    assert(AirportWCL::joinHintChannel(0xd024)==36 && AirportWCL::joinHintChannel(0xd02c)==44 && AirportWCL::joinHintChannel(0x1006)==6);
    // A wider spec names a centre channel; other bands and out-of-range channels are not hints.
    for(uint16_t spec:{0xe02a,0xd826,0x1803,0x100f,0xd023,0xd0b2,0x9024,0x5006,0x0006,0x0000})
        assert(AirportWCL::joinHintChannel(spec)==0);

    ieee80211com ic;const unsigned all=total(ic);assert(all==22);
    {   // The recorded hotspot join: channel 44 alone, long dwell, then the field is spent.
        Scan arg;ic.ic_wcl_join_requested=true;ic.ic_wcl_join_chan=44;
        assert(submit(&ic,&arg)==0 && arg.num_chan==1 && arg.chan_list[0]==5220);
        assert(arg.dwell_time_active==120 && arg.dwell_time_passive==160 && ic.ic_wcl_join_chan==0);
        free(arg.chan_list);
        Scan next;assert(submit(&ic,&next)==0 && next.num_chan==all && next.dwell_time_active==50 && next.dwell_time_passive==150);
        free(next.chan_list); // Not found there: the same join's next scan is the full sweep, as before.
    }
    {   // A scan macOS asked for, or one while associated, is not the join's scan: the hint waits.
        Scan arg;ic.ic_wcl_join_chan=36;ic.ic_wcl_scan_requested=true;
        assert(submit(&ic,&arg)==0 && arg.num_chan==all && ic.ic_wcl_join_chan==36);free(arg.chan_list);
        ic.ic_wcl_scan_requested=false;ic.ic_state=IEEE80211_S_RUN;Scan run;
        assert(submit(&ic,&run)==0 && run.num_chan==all && ic.ic_wcl_join_chan==36);free(run.chan_list);
        ic.ic_state=IEEE80211_S_SCAN;ic.ic_wcl_join_requested=false;Scan idle;
        assert(submit(&ic,&idle)==0 && idle.num_chan==all && ic.ic_wcl_join_chan==36);free(idle.chan_list);
        ic.ic_wcl_join_requested=true;
    }
    {   // Switched off, or a channel the regulatory table does not hold: the sweep, and the hint is spent.
        Scan arg;qwz_joinhint=0;assert(submit(&ic,&arg)==0 && arg.num_chan==all && ic.ic_wcl_join_chan==0);free(arg.chan_list);
        qwz_joinhint=1;ic.ic_wcl_join_chan=52;Scan dfs;
        assert(submit(&ic,&dfs)==0 && dfs.num_chan==all && dfs.dwell_time_active==50 && ic.ic_wcl_join_chan==0);free(dfs.chan_list);
        ic.ic_wcl_join_chan=6;Scan two;assert(submit(&ic,&two)==0 && two.num_chan==1 && two.chan_list[0]==2437);free(two.chan_list);
    }
    puts("actual join request channel -> one-channel first scan -> full sweep fallback: PASS");
}
'''

read = between(qwz, '\tint auth_unacked = 0;\n', '\tbus_dmamap_unload(sc->sc_dmat, tx_data->map);\n\tm_freem(tx_data->m);\n\ttx_data->m = NULL;\n\n\tieee80211_release_node(ic, tx_data->ni);')
act = between(qwz, '\tif (auth_unacked && ic->ic_state == IEEE80211_S_AUTH && ic->ic_mgt_timer > 1) {\n', '\t\tifp->if_timer = 1;\n\t}\n', True)
watchdog = function(net, 'void\nieee80211_watchdog(struct _ifnet *ifp)')
auth_test = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/types.h>
constexpr int IEEE80211_S_SCAN=1,IEEE80211_S_AUTH=2,IEEE80211_S_ASSOC=3,IEEE80211_M_STA=1;
constexpr int IEEE80211_FC0_SUBTYPE_AUTH=0xb0,IFF_DEBUG=4;
constexpr unsigned IEEE80211_F_AUTO_JOIN=1;
#define IEEE80211_ADDR_EQ(a,b) (memcmp((a),(b),6)==0)
#define ISSET(v,f) ((v)&(f))
#define XYLog(...) ((void)0)
struct mbuf {uint8_t data[64];int len;};
#define M_LEN(m) ((m)->len)
#define mtod(m,t) ((t)(m)->data)
struct ieee80211_node {uint8_t ni_macaddr[6]{2,0,0,0,0,1};int ni_fails=0;};
struct _ifnet {int if_flags=0,if_timer=0,if_oerrors=0;const char *if_xname="t";};
struct ieee80211com {
    _ifnet ic_if;int ic_state=IEEE80211_S_AUTH,ic_opmode=IEEE80211_M_STA,ic_mgt_timer=5;unsigned ic_flags=0;
    ieee80211_node node;ieee80211_node *ic_bss=&node;
};
struct Vif {int id;};
struct Softc {Vif sc_vif{0},sc_monvif{1};};
struct TxData {mbuf *m;};
struct Completion {unsigned status,desc_id;};
static int qwz_authfast=1,rejoins,scans,deselects;
template<class... T> static void qwz_log(const char *,T...) {}
static const char *ether_sprintf(const uint8_t *){return "";}
static ieee80211_node *ieee80211_find_node(ieee80211com *ic,const uint8_t *){return ic->ic_bss;}
static void ieee80211_node_join_bss(ieee80211com *ic,ieee80211_node *){++rejoins;ic->ic_mgt_timer=5;}
static void ieee80211_deselect_ess(ieee80211com *){++deselects;}
static void ieee80211_new_state(ieee80211com *ic,int state,int){++scans;ic->ic_state=state;}
''' + watchdog + r'''
static void complete(Softc *sc,ieee80211com *ic,Vif *arvif,TxData *tx_data,Completion *tx_compl_param) {
    _ifnet *ifp=&ic->ic_if;
''' + read + act + r'''
}
static void frame(mbuf &m,uint8_t fc0,const uint8_t *ra){memset(&m,0,sizeof(m));m.len=30;m.data[0]=fc0;memcpy(m.data+4,ra,6);}
int main() {
    Softc sc;const uint8_t other[6]={2,0,0,0,0,9};
    auto run=[&](ieee80211com &ic,uint8_t fc0,const uint8_t *ra,unsigned status,Vif *vif){
        mbuf m;frame(m,fc0,ra);TxData tx{&m};Completion c{status,3};complete(&sc,&ic,vif,&tx,&c);};
    {   // The recorded case: the authentication frame to the BSS came back unacknowledged.
        ieee80211com ic;run(ic,0xb0,ic.node.ni_macaddr,1,&sc.sc_vif);
        assert(ic.ic_mgt_timer==1 && ic.ic_if.if_timer==1);
        // The very next one-second tick is the stack's own retry; nothing was sent from the completion.
        assert(rejoins==0);ieee80211_watchdog(&ic.ic_if);
        assert(rejoins==1 && scans==0 && ic.node.ni_fails==1 && ic.ic_mgt_timer==5 && ic.ic_if.if_timer==1);
        // Bounded by the stack: the third failure of the node gives up and rescans.
        run(ic,0xb0,ic.node.ni_macaddr,1,&sc.sc_vif);ieee80211_watchdog(&ic.ic_if);assert(rejoins==2 && scans==0);
        ic.ic_state=IEEE80211_S_AUTH;run(ic,0xb0,ic.node.ni_macaddr,1,&sc.sc_vif);ieee80211_watchdog(&ic.ic_if);
        assert(rejoins==2 && scans==1 && ic.node.ni_fails==3 && ic.ic_state==IEEE80211_S_SCAN);
    }
    // Everything else leaves the timer alone.
    {ieee80211com ic;run(ic,0xb0,ic.node.ni_macaddr,0,&sc.sc_vif);assert(ic.ic_mgt_timer==5 && ic.ic_if.if_timer==0);}       // acknowledged
    {ieee80211com ic;run(ic,0x00,ic.node.ni_macaddr,1,&sc.sc_vif);assert(ic.ic_mgt_timer==5);}                                 // association request
    {ieee80211com ic;run(ic,0xd0,ic.node.ni_macaddr,1,&sc.sc_vif);assert(ic.ic_mgt_timer==5);}                                 // action frame
    {ieee80211com ic;run(ic,0xb0,other,1,&sc.sc_vif);assert(ic.ic_mgt_timer==5);}                                              // another receiver
    {ieee80211com ic;run(ic,0xb0,ic.node.ni_macaddr,1,&sc.sc_monvif);assert(ic.ic_mgt_timer==5);}                              // the capture vif
    {ieee80211com ic;ic.ic_state=IEEE80211_S_ASSOC;run(ic,0xb0,ic.node.ni_macaddr,1,&sc.sc_vif);assert(ic.ic_mgt_timer==5);}   // already past AUTH
    {ieee80211com ic;ic.ic_mgt_timer=1;run(ic,0xb0,ic.node.ni_macaddr,1,&sc.sc_vif);assert(ic.ic_mgt_timer==1 && ic.ic_if.if_timer==0);}
    {ieee80211com ic;ic.ic_mgt_timer=0;run(ic,0xb0,ic.node.ni_macaddr,1,&sc.sc_vif);assert(ic.ic_mgt_timer==0);}               // no wait running
    {ieee80211com ic;ic.ic_bss=nullptr;run(ic,0xb0,other,1,&sc.sc_vif);assert(ic.ic_mgt_timer==5);}
    {ieee80211com ic;qwz_authfast=0;run(ic,0xb0,ic.node.ni_macaddr,1,&sc.sc_vif);assert(ic.ic_mgt_timer==5);qwz_authfast=1;}
    puts("actual unacknowledged authentication -> one-second retry through ieee80211_watchdog, bounded at three: PASS");
}
'''

with tempfile.TemporaryDirectory(prefix='qwz-join-scan-') as directory:
    flags = ['xcrun', 'clang++', '-std=c++14', '-fsanitize=address,undefined']
    for name, code, extra in (('scan', scan_test, ['-I' + str(root / 'airport/WCL')]), ('auth', auth_test, [])):
        src = Path(directory) / (name + '.cpp')
        exe = Path(directory) / name
        src.write_text(code)
        subprocess.run(flags + extra + [str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
