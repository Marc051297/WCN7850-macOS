#!/usr/bin/env python3
"""RT (f2u): the AirDrop cell's send rate, debug.qwz.monrate.

The production qwz_mon_rate_index, qwz_mon_cell_rate and qwz_mon_cell_params (qwz_mon.c) against a recording
firmware: which vdev parameter goes out, with which rate code, when, and how often.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
mon = (root / 'qwz/qwz_mon.c').read_text()
reg = (root / 'qwz/qwzreg.h').read_text()


def function(name):
    m = re.search(r'static (?:int|void)\n' + name + r'\(', mon)
    assert m, name
    end = mon.index('{', m.end()) + 1
    depth = 1
    while depth:
        depth += (mon[end] == '{') - (mon[end] == '}')
        end += 1
    return mon[m.start():end]


# The tree's own rate numbering and rate-code layout.
rates = reg[reg.index('\tATH12K_HW_RATE_OFDM_48M = 0,'):]
rates = 'enum {\n' + rates[:rates.index('};') + 2]
rc = reg[reg.index('#define ATH12K_RC_VERSION_OFFSET'):reg.index('/* Preamble types to be used with VDEV fixed rate configuration */')]
# The cell applies the rate where its start already set the K1 parameters, and the visit task carries a later change.
start = mon[mon.index('qwz_mon_vdev_start(struct qwz_softc *sc, int chan, uint32_t freq)\n{'):]
assert 'qwz_mon_cell_params(sc);' in start[:6000]
task = mon[mon.index('qwz_mon_dwell_task_fn(void *arg)\n{'):]
assert 'if (sc->mon_rate_applied != qwz_monrate)' in task[:1500] and 'qwz_mon_cell_rate(sc);' in task[:1500]
assert 'sc->mon_rate_applied = -1;\t/* RT (f2u) */' in mon

code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
''' + rates + '\n' + rc + r'''
enum {WMI_RATE_PREAMBLE_OFDM,WMI_RATE_PREAMBLE_CCK};
constexpr uint32_t WMI_VDEV_PARAM_MGMT_RATE=77,WMI_PDEV_PARAM_MGMT_RETRY_LIMIT=78,WMI_VDEV_PARAM_NON_AGG_SW_RETRY_TH=79;
struct qwz_vif {uint32_t vdev_id=1;int is_up=1;};
struct qwz_softc {
    qwz_vif sc_monvif;
    struct {uint8_t pdev_id=0;} pdevs[1];
    int mon_active=0,mon_chan=149,mon_rate_applied=-1,dead=0;
};
static int qwz_monrate=12,qwz_monparams=0,sendResult=0;
struct Set {uint32_t vdev,param,value;};
static std::vector<Set> sets;
static int pdevSets;
static int qwz_mon_fw_unavail(qwz_softc *sc) {return sc->dead;}
static int qwz_wmi_vdev_set_param_cmd(qwz_softc *,uint32_t vdev,uint8_t,uint32_t param,uint32_t value) {
    sets.push_back({vdev,param,value});return sendResult;
}
static int qwz_wmi_pdev_set_param(qwz_softc *,uint32_t,uint32_t,uint8_t) {++pdevSets;return 0;}
''' + '\n'.join(function(n) for n in ('qwz_mon_rate_index', 'qwz_mon_cell_rate', 'qwz_mon_cell_params')) + r'''
static uint32_t code(int index) {return (1U<<28)|uint32_t(index);}   // version 1, OFDM preamble 0, one stream
int main() {
    assert(ATH12K_HW_RATE_CODE(ATH12K_HW_RATE_OFDM_12M,0,WMI_RATE_PREAMBLE_OFDM)==0x10000002U);   // the value f2t's firmware took
    struct {int mbit,index;} table[]={{6,ATH12K_HW_RATE_OFDM_6M},{9,ATH12K_HW_RATE_OFDM_9M},{12,ATH12K_HW_RATE_OFDM_12M},
        {18,ATH12K_HW_RATE_OFDM_18M},{24,ATH12K_HW_RATE_OFDM_24M},{36,ATH12K_HW_RATE_OFDM_36M},{48,ATH12K_HW_RATE_OFDM_48M},
        {54,ATH12K_HW_RATE_OFDM_54M}};
    for(auto &t:table) assert(qwz_mon_rate_index(t.mbit)==t.index);
    for(int bad:{-1,1,2,5,11,13,60,65,108,1000}) assert(qwz_mon_rate_index(bad)==-1);

    // At the cell's start (the cell is not marked active yet there): the default 12 Mbit/s, once.
    qwz_softc sc;
    qwz_mon_cell_params(&sc);
    assert(sets.size()==1 && sets[0].vdev==1 && sets[0].param==WMI_VDEV_PARAM_MGMT_RATE && sets[0].value==0x10000002U);
    assert(sc.mon_rate_applied==12 && !pdevSets);
    // A value changed while the cell runs: each of the eight rates, the code of the tree's own numbering.
    sc.mon_active=1;
    for(auto &t:table) {
        qwz_monrate=t.mbit;sets.clear();qwz_mon_cell_rate(&sc);
        assert(sets.size()==1 && sets[0].param==WMI_VDEV_PARAM_MGMT_RATE && sets[0].value==code(t.index) && sc.mon_rate_applied==t.mbit);
    }
    // No OFDM rate: nothing is sent, and not asked again until the value changes.
    qwz_monrate=7;sets.clear();qwz_mon_cell_rate(&sc);assert(sets.empty() && sc.mon_rate_applied==7);
    // 0 after a rate was given: back to the firmware's 6 Mbit/s on 5 GHz; a 2.4 GHz cell is left alone.
    qwz_monrate=24;qwz_mon_cell_rate(&sc);sets.clear();
    qwz_monrate=0;qwz_mon_cell_rate(&sc);
    assert(sets.size()==1 && sets[0].value==code(ATH12K_HW_RATE_OFDM_6M) && sc.mon_rate_applied==0);
    sc.mon_rate_applied=24;sc.mon_chan=6;sets.clear();qwz_mon_cell_rate(&sc);assert(sets.empty());sc.mon_chan=149;
    // 0 on a fresh cell: the firmware's own rate stays, nothing is sent.
    {qwz_softc fresh;qwz_monrate=0;sets.clear();qwz_mon_cell_params(&fresh);assert(sets.empty() && fresh.mon_rate_applied==0);}
    // The vdev down or the firmware gone: skipped, never a command into a dead firmware.
    {qwz_softc down;down.sc_monvif.is_up=0;qwz_monrate=12;sets.clear();qwz_mon_cell_params(&down);assert(sets.empty());}
    {qwz_softc dead;dead.dead=1;qwz_monrate=12;sets.clear();qwz_mon_cell_params(&dead);assert(sets.empty());}
    // A refused command is logged and not repeated.
    {qwz_softc r;qwz_monrate=36;sendResult=5;sets.clear();qwz_mon_cell_params(&r);assert(sets.size()==1 && r.mon_rate_applied==36);sendResult=0;}
    // debug.qwz.monparams above 0 keeps its own behaviour (K1): fixed 12 Mbit/s, and with 2 the two retry parameters.
    {qwz_softc k;qwz_monparams=1;qwz_monrate=54;sets.clear();qwz_mon_cell_params(&k);
     assert(sets.size()==1 && sets[0].value==0x10000002U && k.mon_rate_applied==54 && !pdevSets);}
    {qwz_softc k;qwz_monparams=2;sets.clear();qwz_mon_cell_params(&k);
     assert(sets.size()==2 && sets[1].param==WMI_VDEV_PARAM_NON_AGG_SW_RETRY_TH && pdevSets==1);}
    puts("ok: the cell's rate follows debug.qwz.monrate at its start and when changed, with the tree's own rate codes; invalid values and a dead firmware send nothing");
}
'''
with tempfile.TemporaryDirectory(prefix='qwz-cell-rate-') as directory:
    path, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
    path.write_text(code)
    subprocess.run(['xcrun', 'clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
