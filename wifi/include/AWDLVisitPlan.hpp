// When to take the radio to the AWDL channel for a transfer we are sending.
//
// The radio visits the AWDL channel in turns (qwz_mon.c). A receiver is on that channel only in some of the
// 16 entries of its own channel sequence, and frames for it are held while it is away (AWDLNativePeerTiming).
// Fixed, unaligned visits meet a receiver's entries by chance; an iPhone in an idle schedule is on the channel
// in 2 to 5 of 16 entries, so most of its time there passed while the radio was on the station's channel.
// This header says when the receiver of the queued frames is next on the channel and for how long, from the
// same schedule the holds already trust. It only computes: no allocation, no firmware command, no clock read.
// Copyright (c) 2026. GPL-2.0-or-later; see LICENSE and the port notices (AWDLPeerSchedule is ForbiddenItlwm's).
#pragma once
#include <AWDLNativePeerTiming.hpp>

namespace ItlAWDL {

constexpr unsigned kVisitPlanLook = 16;        // Queued frames examined for a destination.
constexpr uint64_t kVisitMinRunUS = 25000;     // Less than this left of a run is not worth a channel change.

// From `t`: how long until the peer is on our channel with at least `minRunUS` of that run left, and how long
// the run lasts from then. A run is consecutive sequence entries on our channel. False when the schedule is
// not usable (unknown entry offset, stale, or never on our channel).
inline bool nextPresentRun(const PeerSchedule &s, uint64_t t, uint64_t minRunUS, uint64_t *delayUS, uint64_t *runUS) {
    if (!delayUS || !runUS || !s.known() || !s.fresh(t) || !s.eawUS) return false;
    uint64_t into = 0, delay = 0;
    uint32_t eaw = s.eawAt(t, &into);
    for (unsigned guard = 0; guard < 2 * kPeerSeqEntries + 2; ++guard) {
        if (!s.presentInEntry(eaw)) {
            delay += s.eawUS - into;
            into = 0;
            ++eaw;
            continue;
        }
        uint64_t run = s.eawUS - into;
        uint32_t next = eaw + 1;
        for (unsigned k = 0; k < kPeerSeqEntries && s.presentInEntry(next); ++k, ++next) run += s.eawUS;
        if (run >= minRunUS) {
            *delayUS = delay;
            *runUS = run;
            return true;
        }
        delay += run;  // Too little of this run is left: look past it.
        into = 0;
        eaw = next;
    }
    return false;
}

// The earliest such run among the queued destinations (the first `count` of the transmit queue) whose
// schedule is known on `channel`. Group-addressed and unknown destinations do not plan anything.
inline bool planVisit(NativePeerTiming &timing, const uint8_t (*destinations)[6], unsigned count, uint8_t channel,
                      uint64_t nowUS, uint64_t *delayUS, uint64_t *runUS) {
    if (!destinations || !delayUS || !runUS || !channel) return false;
    bool found = false;
    for (unsigned i = 0; i < count && i < kVisitPlanLook; ++i) {
        auto *peer = timing.find(destinations[i]);
        if (!peer || peer->schedule.ourChannel != channel) continue;
        uint64_t delay = 0, run = 0;
        if (!nextPresentRun(peer->schedule, nowUS, kVisitMinRunUS, &delay, &run)) continue;
        if (!found || delay < *delayUS) {
            *delayUS = delay;
            *runUS = run;
            found = true;
        }
    }
    return found;
}

} // namespace ItlAWDL
