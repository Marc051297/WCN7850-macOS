#!/usr/bin/env python3
"""PM and JR (f2v): two join fixes found on an iPhone hotspot in WPA3 transition mode.

PM: the production qwz_sta_offers_mfp and qwz_peer_assoc_h_crypto (qwz.c). The firmware is told a peer uses
management frame protection only when the association agreed it: the AP offers it and this station's own RSN
element offers it too. The element bytes are those of the recorded joins.

JR: the production statements of qwz_newstate_task that decide whether leaving a BSS starts a sweep of every
channel. With no scan and no join asked by macOS the stack goes idle in SCAN, so the next join starts at once.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'qwz/qwz.c').read_text()


def function(prefix, name):
    start = source.index(prefix + name + '(')
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


task = function('void\n', 'qwz_newstate_task')
j0 = task.index('\t\tif (qwz_joinidle != 0 && ostate > IEEE80211_S_SCAN')
idle = task[j0:task.index('goto scan_entered;\n\t\t}', j0) + len('goto scan_entered;\n\t\t}')]
# The idle branch sits before the sweep and lands after it, on the statements that put the station in SCAN.
assert task.index('case IEEE80211_S_SCAN:\n', 0) < j0 < task.index('next_scan:\n\t\terr = qwz_scan(sc);')
after = task[task.index('scan_entered:'):]
assert 'ieee80211_set_link_state(ic, LINK_STATE_DOWN);' in after[:500] and 'ic->ic_state = IEEE80211_S_SCAN;' in after[:700]
# A repeated request to be in SCAN: idle with nothing asked stays idle; anything asked still jumps to the sweep.
r0 = task.index('\t\t\tif (qwz_joinidle != 0 && ic->ic_wcl_scan_idle && !ic->ic_wcl_scan_requested && !ic->ic_wcl_join_requested) {')
repeat = task[r0:task.index('\t\t\t}\n', r0) + len('\t\t\t}\n')]
assert 'return;' in repeat and task.index('if (sc->scan.state != ATH12K_SCAN_IDLE)') < r0 < task.index('goto next_scan;')
airport = (root / 'airport/WCL/WCL.cpp').read_text()
for marker in ('ic->ic_wcl_scan_idle = false;\n        scanSkipCompletion = false;\n        ieee80211_next_scan(&ic->ic_if);',
               'ic->ic_wcl_scan_idle = false;\n        ieee80211_next_scan(&ic->ic_if);'):
    assert marker in airport, 'a scan or join asked from the idle state must clear the idle flag before it restarts the scan'

code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
constexpr uint16_t IEEE80211_RSNCAP_MFPC=0x0080,IEEE80211_RSNCAP_MFPR=0x0040;
constexpr int IEEE80211_F_RSNON=1,IEEE80211_PROTO_WPA=1,IEEE80211_PROTO_RSN=2,IEEE80211_C_MFP=0x100,IEEE80211_ELEMID_RSN=48;
enum ieee80211_state {IEEE80211_S_INIT,IEEE80211_S_SCAN,IEEE80211_S_AUTH,IEEE80211_S_ASSOC,IEEE80211_S_RUN};
struct ifnet {int unused;};
struct ieee80211com {
#ifdef USE_APPLE_SUPPLICANT
    uint8_t *ic_rsn_ie_override;   // the test gives it exactly the bytes of the element, so that an overread is caught
#endif
    int ic_caps=0,ic_flags=IEEE80211_F_RSNON;
    bool ic_wcl_scan_requested=false,ic_wcl_join_requested=false,ic_wcl_scan_idle=false;
};
struct ieee80211_node {uint16_t ni_rsncaps=0;int ni_rsnprotos=IEEE80211_PROTO_RSN;};
struct peer_assoc_params {int need_ptk_4_way=0,need_gtk_2_way=0,is_pmf_enabled=0;};
struct qwz_vif {int unused;};
struct qwz_softc {ieee80211com sc_ic;};
static int qwz_pmfstrict=1,qwz_joinidle=1,logs,resets;
template<class... T> static void qwz_log(const char *,T...) {++logs;}
static void ieee80211_reset_scan(ifnet *) {++resets;}
''' + function('static int\n', 'qwz_sta_offers_mfp') + '\n' + function('void\n', 'qwz_peer_assoc_h_crypto') + r'''
// One association: the AP's RSN capabilities, this station's RSN element (empty: the stack's own), the result.
static int pmf(uint16_t apCaps,std::vector<uint8_t> element,int stackMfp=0,int rsnOn=1) {
    qwz_softc sc;ieee80211_node ni;peer_assoc_params arg;qwz_vif vif;
    std::vector<uint8_t> exact=element.empty()?std::vector<uint8_t>{0,0}:element;   // heap block of exactly the element's size
#ifdef USE_APPLE_SUPPLICANT
    sc.sc_ic.ic_rsn_ie_override=exact.data();
#endif
    sc.sc_ic.ic_caps=stackMfp?IEEE80211_C_MFP:0;sc.sc_ic.ic_flags=rsnOn?IEEE80211_F_RSNON:0;
    ni.ni_rsncaps=apCaps;
    qwz_peer_assoc_h_crypto(&sc,&vif,&ni,&arg);
    assert(arg.need_ptk_4_way==rsnOn && !arg.need_gtk_2_way);
    return arg.is_pmf_enabled;
}
// The task's own statements for a repeated request to be in SCAN: 1 = nothing happens, 0 = the sweep.
struct Refs {int released=0;};
static void refcnt_rele_wake(Refs *r) {++r->released;}
static void splx(int) {}
static int repeatedStaysIdle(bool idle,bool scanAsked,bool joinAsked) {
    struct {Refs task_refs;} softc,*sc=&softc;ieee80211com com;ieee80211com *ic=&com;const int s=0;
    com.ic_wcl_scan_idle=idle;com.ic_wcl_scan_requested=scanAsked;com.ic_wcl_join_requested=joinAsked;
    [&]() {
''' + repeat + r'''
        softc.task_refs.released=-1;   // fell through to the sweep
    }();
    assert(softc.task_refs.released==1 || softc.task_refs.released==-1);
    return softc.task_refs.released==1;
}
// The newstate task's own statements for entering SCAN: 1 = idle without a sweep, 0 = the sweep.
static int entersIdle(ieee80211_state ostate,bool scanAsked,bool joinAsked,bool *idleFlag=nullptr) {
    ieee80211com com;ieee80211com *ic=&com;ifnet interface{};ifnet *ifp=&interface;
    com.ic_wcl_scan_requested=scanAsked;com.ic_wcl_join_requested=joinAsked;
''' + idle + r'''
    if(idleFlag)*idleFlag=com.ic_wcl_scan_idle;
    return 0;
scan_entered:
    if(idleFlag)*idleFlag=com.ic_wcl_scan_idle;
    return 1;
}
#ifndef USE_APPLE_SUPPLICANT
// The build of today (AirportWCN7850): no supplicant-supplied element exists; the stack writes its own, without protection.
int main() {
    const uint16_t transition=0x00ac;
    assert(pmf(transition,{})==0);                       // the fix: the AP's offer alone no longer switches it on
    qwz_pmfstrict=0;assert(pmf(transition,{})==1);qwz_pmfstrict=1;   // the off switch restores f2u's behaviour
    assert(pmf(transition,{},1)==1 && pmf(0x00ec,{},1)==1);          // a stack that offers it (IEEE80211_C_MFP) and an AP that does
    assert(pmf(0x000c,{},1)==0 && pmf(0x0000,{})==0);    // an AP that offers none: never
    assert(pmf(transition,{},1,0)==0 && logs==4);   // a network without RSN is untouched; one evidence line per AP that offers it
    puts("ok: without a supplicant-supplied element the stack's own offer decides");
}
#else
int main() {
    // The iPhone hotspot join of 2026-11-15 15:50: WPA2-PSK, RSN capabilities 0x003c, no protection offered.
    const std::vector<uint8_t> wpa2={0x30,0x14,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x02,0x3c,0x00};
    // The same with protection offered, as macOS built it at 12:23 (capabilities 0x00bc, a PMKID count and the BIP group cipher).
    const std::vector<uint8_t> mfp={0x30,0x1a,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x02,0xbc,0x00,
                                    0x00,0x00,0x00,0x0f,0xac,0x06};
    const uint16_t transition=0x00ac;   // an AP in WPA3 transition mode: protection capable, not required
    assert(pmf(transition,wpa2)==0);            // the fix: the AP's offer alone no longer switches it on
    qwz_pmfstrict=0;assert(pmf(transition,wpa2)==1);qwz_pmfstrict=1;   // the off switch restores f2u's behaviour
    assert(pmf(transition,mfp)==1);             // both sides offered it
    assert(pmf(0x00ec,mfp)==1);                 // required by the AP, offered by this station
    assert(pmf(0x000c,mfp)==0 && pmf(0x0000,wpa2)==0);   // an AP that offers none: never, whatever this station says
    assert(logs==4);                            // one evidence line per association with an AP that offers it
    // No element from macOS: the stack's own, which offers protection only with IEEE80211_C_MFP.
    assert(pmf(transition,{})==0 && pmf(transition,{},1)==1);
    // A network without RSN is untouched.
    assert(pmf(transition,mfp,0,0)==0);
    // Two pairwise suites and two AKM suites before the capabilities.
    assert(pmf(transition,{0x30,0x1c,1,0, 0,0x0f,0xac,4, 2,0, 0,0x0f,0xac,4, 0,0x0f,0xac,2, 2,0, 0,0x0f,0xac,2, 0,0x0f,0xac,8, 0x80,0x00})==1);
    // Malformed or short elements offer nothing and are never read past their end (the element is an exact heap block).
    for(size_t cut=2;cut<mfp.size();++cut) {
        std::vector<uint8_t> t(mfp.begin(),mfp.begin()+cut);t[1]=uint8_t(cut-2);
        assert(pmf(transition,t)==(cut>=22?1:0));
    }
    assert(pmf(transition,{0x30,0x08,1,0, 0,0x0f,0xac,4, 0xff,0xff})==0);                 // a pairwise count that runs past the end
    assert(pmf(transition,{0x30,0x0e,1,0, 0,0x0f,0xac,4, 1,0, 0,0x0f,0xac,4, 0xff,0x7f})==0);   // an AKM count that does
    assert(pmf(transition,{0xdd,0x14,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x02,0xbc,0x00})==0);   // not an RSN element

    // JR: leaving a BSS (from RUN, ASSOC or AUTH) with nothing asked: idle, no sweep.
    bool idleFlag=false;
    for(auto from:{IEEE80211_S_RUN,IEEE80211_S_ASSOC,IEEE80211_S_AUTH}) {
        resets=0;idleFlag=false;
        assert(entersIdle(from,false,false,&idleFlag)==1 && idleFlag && resets==1);
        // A scan or a join macOS has asked for gets its sweep (the join's is the named channel alone, qwz_scan).
        assert(entersIdle(from,true,false,&idleFlag)==0 && !idleFlag);
        assert(entersIdle(from,false,true,&idleFlag)==0 && !idleFlag);
        assert(entersIdle(from,true,true)==0);
    }
    // Entering SCAN from INIT, or scanning again in SCAN, is as before.
    assert(entersIdle(IEEE80211_S_INIT,false,false,&idleFlag)==0 && !idleFlag);
    assert(entersIdle(IEEE80211_S_SCAN,false,false,&idleFlag)==0 && !idleFlag);
    // A second request to be in SCAN while idle with nothing asked changes nothing; a scan or a join macOS asked
    // for (they clear the idle flag first) gets its sweep.
    assert(repeatedStaysIdle(true,false,false)==1);
    assert(repeatedStaysIdle(false,true,false)==0 && repeatedStaysIdle(false,false,true)==0 && repeatedStaysIdle(false,false,false)==0);
    assert(repeatedStaysIdle(true,true,false)==0 && repeatedStaysIdle(true,false,true)==0);
    // The off switch.
    qwz_joinidle=0;assert(entersIdle(IEEE80211_S_RUN,false,false,&idleFlag)==0 && !idleFlag);
    assert(repeatedStaysIdle(true,false,false)==0);qwz_joinidle=1;
    puts("ok: firmware PMF only when the association agreed it; leaving a BSS with nothing asked starts no sweep");
}
#endif
'''
with tempfile.TemporaryDirectory(prefix='qwz-pmf-idle-') as directory:
    path, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
    path.write_text(code)
    for define in ([], ['-DUSE_APPLE_SUPPLICANT']):   # today's build, and the supplicant variant with its element parser
        subprocess.run(['xcrun', 'clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-Wno-unused-function', '-Wno-unused-variable', '-fsanitize=address,undefined'] + define +
                       [str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
