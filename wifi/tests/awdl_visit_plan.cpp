// AL (aligned visits): when the receiver of queued frames is next on our channel, from its own schedule.
#include <AWDLVisitPlan.hpp>
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace ItlAWDL;

constexpr uint64_t E = 65536;  // One sequence entry: an extended AW of 4 x 16 TU.

// A schedule anchored so that sequence entry 0 starts at `startUS` (offset 0 assumed, as the holds do).
static PeerSchedule schedule(const uint8_t *seq, uint64_t startUS, bool assume = true) {
    PeerSchedule s{};
    s.assumeZero = assume;
    PeerSyncInfo info{};
    info.awPeriodTU = 16;
    info.awCounter = 16 * 4 * 7;  // Extended AW 112: entry 0.
    info.downCounterTU = 64;      // All of it still to come.
    memcpy(info.channels, seq, sizeof(info.channels));
    s.note(info, startUS, 149);
    return s;
}

int main() {
    const uint64_t t0 = 50000000;
    uint64_t delay = 0, run = 0;
    {   // An idle phone: on our channel in entries 2 and 9 only (the recorded sparse sequence).
        const uint8_t seq[16] = {44, 44, 149, 0, 0, 0, 0, 0, 6, 149, 44, 0, 0, 0, 0, 0};
        const auto s = schedule(seq, t0);
        assert(nextPresentRun(s, t0 + 100, kVisitMinRunUS, &delay, &run) && delay == 2 * E - 100 && run == E);
        assert(nextPresentRun(s, t0 + 2 * E, kVisitMinRunUS, &delay, &run) && delay == 0 && run == E);
        assert(nextPresentRun(s, t0 + 3 * E - 40000, kVisitMinRunUS, &delay, &run) && delay == 0 && run == 40000);
        // Ten milliseconds left of a run is not worth a channel change: the next run is the plan.
        assert(nextPresentRun(s, t0 + 3 * E - 10000, kVisitMinRunUS, &delay, &run) && delay == 10000 + 6 * E && run == E);
        // Past the last entry on our channel the plan wraps into the next sequence.
        assert(nextPresentRun(s, t0 + 10 * E, kVisitMinRunUS, &delay, &run) && delay == 8 * E && run == E);
        // No sync frame for two seconds: the schedule is stale and plans nothing.
        assert(!nextPresentRun(s, t0 + kPeerScheduleStaleUS + 1, kVisitMinRunUS, &delay, &run));
        // The entry offset is neither calibrated nor assumed: nothing is planned on a guess.
        const auto unknown = schedule(seq, t0, false);
        assert(!unknown.known() && !nextPresentRun(unknown, t0 + 100, kVisitMinRunUS, &delay, &run));
    }
    {   // The recorded awake sequence: entries 2, 9 and the run 11 to 13.
        const uint8_t seq[16] = {44, 44, 149, 44, 44, 44, 36, 36, 6, 149, 44, 149, 149, 149, 36, 36};
        const auto s = schedule(seq, t0);
        assert(nextPresentRun(s, t0 + 10 * E, kVisitMinRunUS, &delay, &run) && delay == E && run == 3 * E);
        assert(nextPresentRun(s, t0 + 12 * E + 1000, kVisitMinRunUS, &delay, &run) && delay == 0 && run == 2 * E - 1000);
    }
    {   // Always there, and never there.
        uint8_t all[16], none[16] = {};
        memset(all, 149, sizeof(all));
        const auto here = schedule(all, t0);
        assert(nextPresentRun(here, t0 + 500, kVisitMinRunUS, &delay, &run) && delay == 0 && run >= 16 * E);
        const auto away = schedule(none, t0);
        assert(!nextPresentRun(away, t0 + 500, kVisitMinRunUS, &delay, &run));
    }
    {   // Several destinations queued: the earliest known receiver decides; the others plan nothing.
        NativePeerTiming timing{};
        const uint8_t sparse[16] = {44, 44, 149, 0, 0, 0, 0, 0, 6, 149, 44, 0, 0, 0, 0, 0};
        const uint8_t late[16] = {44, 44, 44, 44, 44, 149, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const uint8_t a[6] = {2, 0, 0, 0, 0, 1}, b[6] = {2, 0, 0, 0, 0, 2}, stranger[6] = {2, 0, 0, 0, 0, 9};
        const uint8_t group[6] = {0x33, 0x33, 0, 0, 0, 0xfb};
        timing.admit(a)->schedule = schedule(late, t0);
        timing.admit(b)->schedule = schedule(sparse, t0);
        uint8_t queue[4][6];
        memcpy(queue[0], group, 6); memcpy(queue[1], stranger, 6); memcpy(queue[2], a, 6); memcpy(queue[3], b, 6);
        assert(planVisit(timing, queue, 4, 149, t0 + 100, &delay, &run) && delay == 2 * E - 100 && run == E);  // b, entry 2
        assert(planVisit(timing, queue, 3, 149, t0 + 100, &delay, &run) && delay == 5 * E - 100 && run == E);  // a alone
        assert(!planVisit(timing, queue, 2, 149, t0 + 100, &delay, &run));   // Only group and unknown addresses.
        assert(!planVisit(timing, queue, 4, 44, t0 + 100, &delay, &run));    // Schedules are for another cell channel.
        assert(!planVisit(timing, queue, 0, 149, t0 + 100, &delay, &run) && !planVisit(timing, nullptr, 4, 149, t0, &delay, &run));
    }
    {   // The idle phone's longest absence, 8 entries (524 ms), is held through without a forced release:
        // the hold ends when the receiver is back, not 74 ms before (the ceiling was 450 ms).
        const uint8_t seq[16] = {44, 44, 149, 0, 0, 0, 0, 0, 6, 149, 44, 0, 0, 0, 0, 0};
        const auto s = schedule(seq, t0);
        const uint64_t away = t0 + 10 * E + 5000;                       // Just after entry 9 ends.
        assert(s.shouldPause(away));
        const uint64_t limit = s.pauseLimitUS(away);
        assert(limit > 8 * E - 5000 && limit < kPeerPauseCeilingUS);   // Set by the run, not cut by the ceiling.
        PeerPauseState pause{};
        assert(pause.step(true, away, limit) == 1);
        for (uint64_t t = away; t < t0 + 18 * E - 2000; t += 1000)     // Entry 2 of the next sequence starts at 18 E.
            assert(pause.step(s.shouldPause(t), t, s.pauseLimitUS(t)) == 0 && pause.paused);
        assert(pause.step(s.shouldPause(t0 + 18 * E + 2000), t0 + 18 * E + 2000, 0) == -1 && pause.forcedResumes == 0);
    }
    puts("receiver schedule -> next run on our channel -> visit plan: PASS");
}
