#!/usr/bin/env python3
"""JL (join lifecycle): a 4-way message 1 that overtakes the association response is kept and answered.

Recorded on f2s, 2026-11-15 14:24: message 1 was handled at 205.482 s, the association response at 205.483 s
(data frames and management frames come up through different rings). The message was dropped, the access point
repeated it a second later, and a 0.2 s join took 1.2 s. The production keep block and replay function are
compiled here; the association path's call sites are checked as text.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
net = root / 'itl80211/openbsd/net80211'
pae = (net / 'ieee80211_pae_input.c').read_text()
inp = (net / 'ieee80211_input.c').read_text()
proto = (net / 'ieee80211_proto.c').read_text()


def function(source, marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


start = pae.index('#ifdef AIRPORT_WCL\n        /*\n         * JL (join lifecycle): message 1 overtook the association response.')
keep = pae[start:pae.index('#endif\n', start) + len('#endif\n')]
replay = function(pae, 'void\nieee80211_wcl_replay_early_msg1(struct ieee80211com *ic, struct ieee80211_node *ni)')

# The association response answers the kept message after its own state change, clears it when refused,
# and every return of the supplicant to its initial state discards it.
assoc = function(inp, 'void\nieee80211_recv_assoc_resp(struct ieee80211com *ic, mbuf_t m,')
assert assoc.index('ieee80211_new_state(ic, IEEE80211_S_RUN,') < assoc.index('ieee80211_wcl_replay_early_msg1(ic, ni);')
assert 'if (status != IEEE80211_STATUS_SUCCESS)\n        ic->ic_wcl_early_m1_len = 0;' in assoc
assert proto.count('ni->ni_rsn_supp_state = RSNA_SUPP_INITIALIZE;\n#ifdef AIRPORT_WCL\n\t\tic->ic_wcl_early_m1_len = 0;') == 3

code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/types.h>
#define AIRPORT_WCL 1
enum {IEEE80211_S_SCAN=1,IEEE80211_S_AUTH=2,IEEE80211_S_ASSOC=3,IEEE80211_S_RUN=4,IEEE80211_M_STA=1,IEEE80211_M_HOSTAP=2};
enum {RSNA_SUPP_INITIALIZE=0,RSNA_SUPP_PTKSTART=2};
#define IEEE80211_ADDR_COPY(d,s) memcpy((d),(s),6)
#define IEEE80211_ADDR_EQ(a,b) (memcmp((a),(b),6)==0)
#define BE_READ_2(p) ((u_int16_t)(((p)[0]<<8)|(p)[1]))
struct ieee80211_eapol_key {
    u_int8_t version,type,len[2],desc,info[2],keylen[2],replaycnt[8],nonce[32],iv[16],rsc[8],reserved[8],mic[16],paylen[2];
} __attribute__((packed));
struct ieee80211_node {u_int8_t ni_macaddr[6]{2,0,0,0,0,1};int ni_rsn_supp_state=RSNA_SUPP_INITIALIZE;};
struct ieee80211com {
    int ic_opmode=IEEE80211_M_STA,ic_state=IEEE80211_S_ASSOC;
    ieee80211_node node;ieee80211_node *ic_bss=&node;
    uint16_t ic_wcl_early_m1_len=0;uint8_t ic_wcl_early_m1_bssid[6]{},ic_wcl_early_m1[256]{};
};
template<class... T> static int qwz_log(const char *,T...) {return 0;}
static int handled,dropped;static uint8_t seen[256];static size_t seenLength;
static void ieee80211_recv_4way_msg1(ieee80211com *,ieee80211_eapol_key *key,ieee80211_node *) {
    ++handled;seenLength=4+BE_READ_2(key->len);memcpy(seen,key,seenLength);
}
// The branch of ieee80211_recv_4way_msg1 taken while the supplicant is in its initial state.
static void early(ieee80211com *ic,ieee80211_eapol_key *key,ieee80211_node *ni) {
''' + keep + r'''
    ++dropped;
}
''' + replay + r'''
static size_t message(uint8_t *buffer,size_t keyData) {
    memset(buffer,0,512);
    const size_t body=sizeof(ieee80211_eapol_key)-4+keyData;
    buffer[0]=2;buffer[1]=3;buffer[2]=uint8_t(body>>8);buffer[3]=uint8_t(body);buffer[4]=2;buffer[6]=0x8a;
    for(size_t i=0;i<32;++i)buffer[17+i]=uint8_t(0xa0+i);  // Stands in for the nonce; only copied, never printed.
    for(size_t i=0;i<keyData;++i)buffer[sizeof(ieee80211_eapol_key)+i]=uint8_t(i);
    return body+4;
}
int main() {
    assert(sizeof(ieee80211_eapol_key)==99);
    uint8_t frame[512];
    {   // The recorded race: message 1 during ASSOC, then the response arms the supplicant and answers it.
        ieee80211com ic;const size_t n=message(frame,22);
        early(&ic,reinterpret_cast<ieee80211_eapol_key *>(frame),&ic.node);
        assert(dropped==0 && handled==0 && ic.ic_wcl_early_m1_len==n);
        ieee80211_wcl_replay_early_msg1(&ic,&ic.node);           // Still initial: nothing to answer with, and it is spent.
        assert(handled==0 && ic.ic_wcl_early_m1_len==0);
        early(&ic,reinterpret_cast<ieee80211_eapol_key *>(frame),&ic.node);
        ic.node.ni_rsn_supp_state=RSNA_SUPP_PTKSTART;
        ieee80211_wcl_replay_early_msg1(&ic,&ic.node);
        assert(handled==1 && seenLength==n && !memcmp(seen,frame,n) && ic.ic_wcl_early_m1_len==0);
        ieee80211_wcl_replay_early_msg1(&ic,&ic.node);assert(handled==1);   // Used once.
    }
    handled=dropped=0;
    {   // Only for the BSS being associated, in ASSOC, as a station, and only when it fits.
        ieee80211com ic;ieee80211_node other;other.ni_macaddr[5]=9;const size_t n=message(frame,22);(void)n;
        early(&ic,reinterpret_cast<ieee80211_eapol_key *>(frame),&other);assert(dropped==1 && !ic.ic_wcl_early_m1_len);
        ic.ic_state=IEEE80211_S_AUTH;early(&ic,reinterpret_cast<ieee80211_eapol_key *>(frame),&ic.node);assert(dropped==2);
        ic.ic_state=IEEE80211_S_RUN;early(&ic,reinterpret_cast<ieee80211_eapol_key *>(frame),&ic.node);assert(dropped==3);
        ic.ic_state=IEEE80211_S_ASSOC;ic.ic_opmode=IEEE80211_M_HOSTAP;
        early(&ic,reinterpret_cast<ieee80211_eapol_key *>(frame),&ic.node);assert(dropped==4);
        ic.ic_opmode=IEEE80211_M_STA;message(frame,200);        // 299 bytes: longer than the 256 kept.
        early(&ic,reinterpret_cast<ieee80211_eapol_key *>(frame),&ic.node);assert(dropped==5 && !ic.ic_wcl_early_m1_len);
        message(frame,157);                                      // Exactly 256.
        early(&ic,reinterpret_cast<ieee80211_eapol_key *>(frame),&ic.node);assert(dropped==5 && ic.ic_wcl_early_m1_len==256);
    }
    {   // A kept message is never answered for another BSS.
        ieee80211com ic;message(frame,22);early(&ic,reinterpret_cast<ieee80211_eapol_key *>(frame),&ic.node);
        ieee80211_node other;other.ni_macaddr[5]=9;other.ni_rsn_supp_state=RSNA_SUPP_PTKSTART;
        ieee80211_wcl_replay_early_msg1(&ic,&other);assert(handled==0 && ic.ic_wcl_early_m1_len==0);
        ieee80211_wcl_replay_early_msg1(&ic,nullptr);assert(handled==0);
    }
    puts("actual early 4-way message 1: kept during ASSOC, answered once when the association completes: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='qwz-early-m1-') as directory:
    src = Path(directory) / 'test.cpp'
    exe = Path(directory) / 'test'
    src.write_text(code)
    subprocess.run(['xcrun', 'clang++', '-std=c++14', '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
