#!/usr/bin/env python3
"""AL (aligned visits): the production visit planner, driven over recorded receiver schedules.

The actual qwz_mon_dwell_plan from qwz_mon.c decides, through the actual AWDLVisitPlan.hpp and peer schedule,
when each visit to the AWDL channel starts and how long it lasts. A small clock model (10 ms timer tick, 6 ms from
command to channel, the configured rest) then measures how much of the receiver's time on the channel the radio
shared, against the fixed 100 ms on / 100 ms off visits, and how long the station was left.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
mon = (root / 'qwz/qwz_mon.c').read_text()
start = mon.index('#define QWZ_MON_ALIGN_LEAD_US')
end = mon.index('\nstatic void\nqwz_mon_reap(void *arg)', start)
planner = mon[start:end]
tick = mon[mon.index('\tif (sc->mon_coex && qwz_moncoexdwell > 0 &&\n\t    sc->scan.state == ATH12K_SCAN_IDLE'):]
assert 'now >= sc->mon_dwell_next_ns && qwz_mon_dwell_plan(sc, now)' in tick[:1600], 'the timer must ask the planner'
task = mon[mon.index('qwz_mon_dwell_task_fn(void *arg)\n{'):]
assert 'sc->mon_dwell_plan_ms > 0 ? sc->mon_dwell_plan_ms' in task[:1500], 'the visit must use the planned length'
event = mon[mon.index('qwz_mon_dwell_event(struct qwz_softc *sc, uint32_t type, uint32_t freq, uint32_t reason)\n{'):]
assert 'sc->mon_dwell_ended_ns = now;' in event[:2500], 'the end of a visit must be recorded'

code = r'''
#include <AWDLVisitPlan.hpp>
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace ItlAWDL;
struct qwz_softc {
    int mon_dwell_plan_ms=0;uint64_t mon_dwell_ended_ns=0,mon_dwell_next_ns=0;
    struct {uint32_t dwell_aligned=0,dwell_deferred=0;} mon_stats;
};
typedef int (*qwz_mon_native_plan_fn)(void *,uint32_t *,uint32_t *);
static qwz_mon_native_plan_fn qwz_mon_native_plan;
static void *qwz_mon_native_plan_arg;
static int qwz_moncoexalign=130;
''' + planner + r'''
constexpr uint64_t E=65536,MS=1000000;
static uint64_t clockNS;                      // The model's time; the planner's own clock is microseconds.
static PeerSchedule peer;
static bool queued=true;
static int fixedDelay=-1,fixedRun;            // A canned answer for the decision cases.
static int plan(void *,uint32_t *delay,uint32_t *run) {
    if(fixedDelay>=0){*delay=uint32_t(fixedDelay);*run=uint32_t(fixedRun);return 1;}
    uint64_t d=0,r=0;
    if(!queued||!nextPresentRun(peer,clockNS/1000,kVisitMinRunUS,&d,&r))return 0;
    *delay=uint32_t(d);*run=uint32_t(r);return 1;
}
static PeerSchedule schedule(const uint8_t *seq,uint64_t startUS) {
    PeerSchedule s{};s.assumeZero=true;PeerSyncInfo info{};
    info.awPeriodTU=16;info.awCounter=16*4*7;info.downCounterTU=64;memcpy(info.channels,seq,16);
    s.note(info,startUS,149);return s;
}
static bool present(uint64_t ns){uint64_t into=0;return peer.presentInEntry(peer.eawAt(ns/1000,&into));}
struct Result {double shared,away;uint64_t longestGapMS;unsigned visits;};
// The timer, task and completion of qwz_mon.c in miniature: tick every 10 ms, 6 ms to reach the channel,
// the visit as long as planned (or 100 ms), then 100 ms of rest. The sync frames keep the schedule fresh.
static Result simulate(const uint8_t *seq,int align,uint64_t seconds) {
    qwz_softc sc;qwz_moncoexalign=align;fixedDelay=-1;
    const uint64_t t0=40*MS*1000/1000;clockNS=t0;peer=schedule(seq,t0/1000);
    uint64_t sharedNS=0,awayNS=0,presentNS=0,lastEnd=t0,longest=0;unsigned visits=0;
    const uint64_t endNS=t0+seconds*1000*MS;
    while(clockNS<endNS) {
        if(clockNS>=sc.mon_dwell_next_ns && qwz_mon_dwell_plan(&sc,clockNS)) {
            const uint64_t on=clockNS+6*MS,off=on+uint64_t(sc.mon_dwell_plan_ms>0?sc.mon_dwell_plan_ms:100)*MS;
            if(on-lastEnd>longest)longest=on-lastEnd;
            for(uint64_t t=clockNS;t<off;t+=MS/4){awayNS+=MS/4;if(t>=on&&present(t))sharedNS+=MS/4;}
            for(uint64_t t=clockNS;t<off;t+=MS/4)if(present(t))presentNS+=MS/4;
            ++visits;clockNS=off;sc.mon_dwell_ended_ns=off;sc.mon_dwell_next_ns=off+100*MS;lastEnd=off;
            peer.heardUS=clockNS/1000;  // A sync frame heard on each visit.
            continue;
        }
        for(uint64_t t=clockNS;t<clockNS+10*MS;t+=MS/4)if(present(t))presentNS+=MS/4;
        clockNS+=10*MS;
        if((clockNS/MS)%500==0)peer.heardUS=clockNS/1000;
    }
    return {double(sharedNS)/double(presentNS),double(awayNS)/double(endNS-t0),longest/MS,visits};
}
int main() {
    qwz_softc sc;const uint64_t now=900*MS;
    // No planner, the switch off, or nothing queued for a known receiver: the ordinary visit, at once.
    assert(qwz_mon_dwell_plan(&sc,now)==1 && sc.mon_dwell_plan_ms==0);
    qwz_mon_native_plan=plan;queued=false;peer=PeerSchedule{};
    assert(qwz_mon_dwell_plan(&sc,now)==1 && sc.mon_dwell_plan_ms==0);
    qwz_moncoexalign=0;fixedDelay=0;fixedRun=65000;assert(qwz_mon_dwell_plan(&sc,now)==1 && sc.mon_dwell_plan_ms==0);
    qwz_moncoexalign=130;
    // The receiver is there, or arrives within the lead: start now, for its run, within 30 ms and the limit.
    fixedDelay=0;fixedRun=65000;assert(qwz_mon_dwell_plan(&sc,now)==1 && sc.mon_dwell_plan_ms==65);
    fixedDelay=5000;assert(qwz_mon_dwell_plan(&sc,now)==1 && sc.mon_dwell_plan_ms==70);
    fixedRun=300000;assert(qwz_mon_dwell_plan(&sc,now)==1 && sc.mon_dwell_plan_ms==130);
    fixedDelay=0;fixedRun=10000;assert(qwz_mon_dwell_plan(&sc,now)==1 && sc.mon_dwell_plan_ms==30);
    qwz_moncoexalign=9999;fixedRun=900000;assert(qwz_mon_dwell_plan(&sc,now)==1 && sc.mon_dwell_plan_ms==500);
    qwz_moncoexalign=130;assert(sc.mon_stats.dwell_aligned==5);
    // It arrives later: wait for it, 12 ms early, but never more than 350 ms after the last visit ended.
    fixedDelay=200000;fixedRun=65000;sc.mon_dwell_ended_ns=now-100*MS;
    assert(qwz_mon_dwell_plan(&sc,now)==0 && sc.mon_dwell_next_ns==now+188*MS && sc.mon_stats.dwell_deferred==1);
    fixedDelay=300000;sc.mon_dwell_next_ns=0;
    assert(qwz_mon_dwell_plan(&sc,now)==1 && sc.mon_dwell_plan_ms==0 && sc.mon_dwell_next_ns==0);  // Too far: an ordinary visit first.
    fixedDelay=200000;sc.mon_dwell_ended_ns=0;
    assert(qwz_mon_dwell_plan(&sc,now)==1 && sc.mon_dwell_plan_ms==0);                              // No visit yet: never wait.
    sc.mon_dwell_ended_ns=now+5*MS;assert(qwz_mon_dwell_plan(&sc,now)==1);                          // A clock that went back: never wait.
    assert(sc.mon_stats.dwell_deferred==1);

    // The two recorded schedules, 20 s each, aligned against fixed visits.
    const uint8_t idle[16]={44,44,149,0,0,0,0,0,6,149,44,0,0,0,0,0};
    const uint8_t awake[16]={44,44,149,44,44,44,36,36,6,149,44,149,149,149,36,36};
    for(const uint8_t *seq:{idle,awake}) {
        queued=true;
        const Result fixed=simulate(seq,0,20),aligned=simulate(seq,130,20);
        printf("  %s receiver: on-channel time shared %.0f%% -> %.0f%%, radio away from the station %.0f%% -> %.0f%%, longest gap %llu -> %llu ms, visits %u -> %u\n",
            seq==idle?"idle":"awake",100*fixed.shared,100*aligned.shared,100*fixed.away,100*aligned.away,
            (unsigned long long)fixed.longestGapMS,(unsigned long long)aligned.longestGapMS,fixed.visits,aligned.visits);
        assert(aligned.shared>=0.70);                         // Most of the receiver's time on the channel is met.
        assert(aligned.shared>=fixed.shared*1.3);             // Clearly more than chance gave.
        assert(aligned.away<=fixed.away+0.01);                // The station is not left for longer than before.
        assert(aligned.longestGapMS<=350+100+20);             // Discovery and other peers still get a visit in time.
        // With nothing queued the planner changes nothing at all.
        queued=false;const Result quiet=simulate(seq,130,20);
        assert(quiet.visits==fixed.visits && quiet.shared==fixed.shared && quiet.away==fixed.away);
    }
    puts("actual visit planner over recorded receiver schedules: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='qwz-visit-align-') as directory:
    src = Path(directory) / 'test.cpp'
    exe = Path(directory) / 'test'
    src.write_text(code)
    subprocess.run(['xcrun', 'clang++', '-std=c++14', '-I' + str(root / 'include'), '-fsanitize=address,undefined',
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
