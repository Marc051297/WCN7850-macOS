#!/usr/bin/env python3
"""CC (f2u): the host channel table across a change of country.

The production qwz_init_channels and qwz_close_unnamed_channels (qwz.c), with the flag values of the tree's own
headers, applied the way qwz_reg_chan_list_event applies them: the firmware's world rules first, then the rules of
the country the itlwm_cc boot argument names, at every firmware start. A channel the new country's rules do not
name must be closed; nothing may stay open from the previous table. The table only ever follows the rules the
firmware sent.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'qwz/qwz.c').read_text()
reg = (root / 'qwz/qwzreg.h').read_text()
var = (root / 'itl80211/openbsd/net80211/ieee80211_var.h').read_text()
CHAN40 = (root / 'qwz/qwz_compat.h').read_text()


def function(prefix, name):
    start = source.index(prefix + name + '(')
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def defines(text, pattern):
    found = re.findall(r'^#define[ \t]+(' + pattern + r')[ \t]+(.+?)[ \t]*(?:/\*.*)?$', text, re.M)
    assert found, pattern
    return '\n'.join('#define %s %s' % d for d in found)


# The stack's own channel flag values: the block from IEEE80211_CHAN_CCK to the IS_CHAN macros, and the 40 MHz flag.
chan_flags = var[var.index('#define IEEE80211_CHAN_CCK'):var.index('#define\tIEEE80211_IS_CHAN_A(_c)')]
m40 = re.search(r'^#define[ \t]+IEEE80211_CHAN_40MHZ[ \t]+.*$', CHAN40, re.M)
assert m40, 'IEEE80211_CHAN_40MHZ'
chan_flags += '\n' + m40.group(0) + '\n'

# The handler must mark the channels of an event, close the others only when the country differs, and remember it.
handler = function('int\n', 'qwz_reg_chan_list_event')
assert 'const int other = sc->reg_chan_cc[0] != 0 && memcmp(sc->reg_chan_cc, reg_info->alpha2, 2) != 0;' in handler
assert handler.index('qwz_init_channels(sc, reg_info, seen);') < handler.index('closed = qwz_close_unnamed_channels(sc, seen);')
assert 'memcpy(sc->reg_chan_cc, reg_info->alpha2, 2);' in handler
assert 'memcpy(sc->reg_chan_cc, "00", 3);' in function('void\n', 'qwz_init_channels_world')

code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define BIT(n) (1U<<(n))
#define DPRINTF(...) ((void)0)
''' + defines(reg, r'REGULATORY_CHAN_\w+') + '\n#define IEEE80211_CHAN_MAX 255\n' + chan_flags + r'''
struct ieee80211_channel {uint16_t ic_freq;uint32_t ic_flags;};
struct ieee80211com {ieee80211_channel ic_channels[IEEE80211_CHAN_MAX+1];};
struct qwz_softc {ieee80211com sc_ic;};
struct cur_reg_rule {uint16_t start_freq,end_freq,max_bw;uint32_t flags;};
struct cur_regulatory_info {int num_2ghz_reg_rules,num_5ghz_reg_rules;cur_reg_rule *reg_rules_2ghz_ptr,*reg_rules_5ghz_ptr;};
static unsigned ieee80211_mhz2ieee(unsigned freq,unsigned) {
    if(freq==2484) return 14;
    if(freq<2484) return (freq-2407)/5;
    return (freq-5000)/5;
}
static unsigned ieee80211_ieee2mhz(unsigned chan,unsigned flags) {
    if(flags&IEEE80211_CHAN_2GHZ) return chan==14?2484:2407+chan*5;
    return 5000+chan*5;
}
''' + function('void\n', 'qwz_init_channels') + '\n' + function('static int\n', 'qwz_close_unnamed_channels') + r'''
struct Rules {cur_reg_rule g2[4];int n2;cur_reg_rule g5[6];int n5;};
// The step of qwz_reg_chan_list_event for one accepted event: `other` is "the table holds another country's rules".
static int apply(qwz_softc &sc,Rules &r,bool other) {
    cur_regulatory_info info{r.n2,r.n5,r.g2,r.g5};
    uint8_t seen[(IEEE80211_CHAN_MAX+8)/8];
    memset(seen,0,sizeof(seen));
    qwz_init_channels(&sc,&info,seen);
    return other ? qwz_close_unnamed_channels(&sc,seen) : 0;
}
static bool open(qwz_softc &sc,int c) {return sc.sc_ic.ic_channels[c].ic_flags!=0 && sc.sc_ic.ic_channels[c].ic_freq!=0;}
static bool probes(qwz_softc &sc,int c) {return open(sc,c) && !(sc.sc_ic.ic_channels[c].ic_flags&IEEE80211_CHAN_PASSIVE);}
int main() {
    const uint32_t noir=REGULATORY_CHAN_NO_IR,radar=REGULATORY_CHAN_RADAR;
    // A world domain: 2.4 GHz to channel 13, all of 5 GHz listen-only.
    Rules world{{{2402,2472,20,0},{2457,2482,20,noir}},2,
                {{5170,5250,80,noir},{5250,5330,80,noir|radar},{5490,5730,160,noir|radar},{5735,5835,80,noir}},4};
    // The United States: channels 1 to 11; 36 to 48 and 149 to 165 open, the radar bands listen-first.
    Rules us{{{2402,2472,20,0}},1,
             {{5170,5250,80,0},{5250,5330,80,radar},{5490,5730,160,radar},{5735,5835,80,0}},4};
    // A country with no upper 5 GHz band at all.
    Rules lower{{{2402,2482,20,0}},1,{{5170,5250,80,0},{5250,5330,80,radar},{5490,5710,160,radar}},3};
    // A country whose rules name the upper band but disable it.
    Rules shut{{{2402,2472,20,0}},1,{{5170,5250,80,0},{5735,5835,80,REGULATORY_CHAN_DISABLED}},2};

    qwz_softc sc{};
    assert(apply(sc,world,false)==0);
    for(int c=1;c<=11;++c) assert(probes(sc,c));
    for(int c:{12,13}) assert(open(sc,c) && !probes(sc,c));   // open by the second rule, which says listen-only
    assert(!open(sc,14));
    for(int c:{36,40,44,48,52,64,100,144,149,153,157,161,165}) assert(open(sc,c) && !probes(sc,c));

    // World to US, as at every firmware start: 12 and 13 close, the open bands may be probed, radar bands stay listen-first.
    assert(apply(sc,us,true)==2);
    for(int c=1;c<=11;++c) assert(probes(sc,c));
    for(int c:{12,13,14}) assert(!open(sc,c) && !sc.sc_ic.ic_channels[c].ic_freq);
    for(int c:{36,40,44,48,149,153,157,161,165}) assert(probes(sc,c));
    for(int c:{52,56,60,64,100,120,140,144}) assert(open(sc,c) && !probes(sc,c));
    // Without the closing step the two channels would have stayed open from the world table.
    {qwz_softc old{};apply(old,world,false);apply(old,us,false);assert(open(old,12) && open(old,13));}
    // The same country again changes nothing and closes nothing.
    const ieee80211com before=sc.sc_ic;
    assert(apply(sc,us,false)==0 && !memcmp(&before,&sc.sc_ic,sizeof(before)));
    assert(apply(sc,us,true)==0 && !memcmp(&before,&sc.sc_ic,sizeof(before)));

    // US back to the world domain (a firmware restart): probing permission is withdrawn again, nothing is left over.
    assert(apply(sc,world,true)==0);
    for(int c:{12,13}) assert(open(sc,c) && !probes(sc,c));
    for(int c:{36,44,149,165}) assert(open(sc,c) && !probes(sc,c));

    // US to a country without the upper band: 149 to 165 close; and to one that disables it by rule.
    apply(sc,us,true);
    assert(apply(sc,lower,true)==6);   // 144 (past 5710) and 149 to 165
    for(int c:{144,149,153,157,161,165}) assert(!open(sc,c));
    for(int c:{1,13,36,48,52,140}) assert(open(sc,c));
    apply(sc,us,true);
    assert(apply(sc,shut,true)>0);
    for(int c:{149,153,157,161,165,52,100,12,13}) assert(!open(sc,c));
    for(int c:{1,11,36,48}) assert(probes(sc,c));
    // A rule names only channels whose 20 MHz lie inside it: 2402-2472 ends with channel 11, 5735-5850 with 165.
    {qwz_softc edge{};Rules r{{{2402,2472,20,0}},1,{{5735,5850,80,0}},1};apply(edge,r,false);
     assert(open(edge,11) && !open(edge,12) && !open(edge,13) && open(edge,165) && !open(edge,169));}
    // Channel numbers no rule can name are never opened by any of this.
    for(int c:{0,15,35,34,170,200,255}) assert(!open(sc,c));
    puts("ok: after a change of country the channel table holds exactly what the new rules name; probing follows the rules' flags");
}
'''
with tempfile.TemporaryDirectory(prefix='qwz-country-table-') as directory:
    path, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
    path.write_text(code)
    subprocess.run(['xcrun', 'clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
