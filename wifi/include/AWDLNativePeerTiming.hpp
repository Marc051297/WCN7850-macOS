// Qualcomm native FIFO timing, using the licensed AWDLPeerSchedule helper.
// PSFs may be heard outside data windows: they refresh the anchor but never
// count as evidence of data availability. MIFs and validated data do count.
// No offset-zero assumption, sleeping, allocation or firmware command here.
// Copyright (c) 2026. GPL-2.0-or-later; see LICENSE and existing port notices.
#pragma once
#include <AWDLPeerSchedule.hpp>
#include <AWDLFrameParser.hpp>
#include <AWDLDataFrame.hpp>

namespace ItlAWDL {
struct NativePeerTiming {
    static constexpr unsigned kPeers = 8;
    struct Peer {
        uint8_t address[6];
        bool occupied, haveMifWindow, haveContradiction;
        uint32_t mifWindows, lastMifWindow, contradictionWindow;
        uint64_t firstMifUS, lastSeenUS;
        PeerSchedule schedule;
        PeerPauseState pause;
    } peers[kPeers];
    uint64_t actionObservations, dataObservations, holdChecks, forcedReleases, resets, bypasses;

    void reset() { memset(this, 0, sizeof(*this)); }
    void clearPeers() { memset(peers, 0, sizeof(peers)); ++resets; }
    static bool unicast(const uint8_t *a) {
        const uint8_t zero[6] = {};
        return a && !(a[0] & 1) && memcmp(a, zero, 6);
    }
    Peer *find(const uint8_t *a) {
        if (!unicast(a)) return nullptr;
        for (auto &p : peers)
            if (p.occupied && !memcmp(p.address, a, 6)) return &p;
        return nullptr;
    }
    Peer *admit(const uint8_t *a) {
        if (!unicast(a)) return nullptr;
        if (auto *p = find(a)) return p;
        Peer *oldest = &peers[0];
        for (auto &p : peers) {
            if (!p.occupied) { oldest = &p; break; }
            if (p.lastSeenUS < oldest->lastSeenUS) oldest = &p;
        }
        memset(oldest, 0, sizeof(*oldest));
        memcpy(oldest->address, a, 6);
        oldest->occupied = true;
        return oldest;
    }
    static void clearEvidence(Peer &p) {
        p.haveMifWindow = p.haveContradiction = false;
        p.mifWindows = 0;
        p.firstMifUS = 0;
    }
    // Contradictory data/MIF observations in two separate windows revoke the
    // low-sample native calibration. A single delayed RX burst cannot do so.
    static void evidence(Peer &p, uint64_t nowUS) {
        auto &s = p.schedule;
        uint64_t into = 0;
        const uint32_t window = s.eawAt(nowUS, &into);
        if (s.calibrated && into >= kPeerEdgeUS && s.eawUS - into >= kPeerEdgeUS &&
            !s.presentInEntry(window)) {
            if (p.haveContradiction && p.contradictionWindow != window) {
                s.invalidate();
                clearEvidence(p);
                return;
            }
            p.haveContradiction = true;
            p.contradictionWindow = window;
        }
        s.notePeerFrame(nowUS);
    }
    bool noteAction(const uint8_t *f, size_t n, uint8_t channel, uint64_t nowUS) {
        ActionFrame action{};
        PeerSyncInfo sync{};
        if (!channel || parseAction(f, n, &action) != ParseResult::Valid ||
            !peerSyncFromAction(f, n, &sync)) return false;
        Peer *p = admit(f + 10);
        if (!p) return false;
        auto &s = p->schedule;
        const bool changed = !s.fresh(nowUS) || s.ourChannel != channel ||
            s.eawUS != uint64_t(sync.awPeriodTU) * 4096 ||
            memcmp(s.channels, sync.channels, sizeof(s.channels));
        const uint32_t oldDecalibrations = s.decalibrations;
        s.note(sync, nowUS, channel);
        if (changed || oldDecalibrations != s.decalibrations) clearEvidence(*p);
        p->lastSeenUS = nowUS;
        ++actionObservations;
        if (action.subtype == 3) {
            const uint32_t window = sync.awCounter / 4;
            if (!p->haveMifWindow || p->lastMifWindow != window) {
                if (!p->mifWindows) p->firstMifUS = nowUS;
                ++p->mifWindows;
                p->lastMifWindow = window;
                p->haveMifWindow = true;
                evidence(*p, nowUS);
                // At least eight distinct MIF windows spanning a full sequence,
                // a unique fit, <=3% contradictions and >=25% separation from
                // the next candidate. Data's original 300-frame route remains.
                if (s.valid && p->mifWindows >= 8 && nowUS >= p->firstMifUS &&
                    nowUS - p->firstMifUS >= s.eawUS * kPeerSeqEntries && !s.calibrated)
                    s.calibrate(8, 25);
            }
        }
        return true;
    }
    bool noteData(const uint8_t *f, size_t n, bool includesFCS, uint8_t channel, uint64_t nowUS) {
        DataFrameView frame{};
        if (parseDataFrame(f, n, includesFCS, &frame) != ParseResult::Valid) return false;
        Peer *p = find(frame.source);
        if (!p || p->schedule.ourChannel != channel || !p->schedule.fresh(nowUS)) return false;
        p->lastSeenUS = nowUS;
        evidence(*p, nowUS);
        ++dataObservations;
        return true;
    }
    bool hold(const uint8_t *destination, uint8_t channel, uint64_t nowUS) {
        Peer *p = find(destination);
        if (!p) return false;
        auto &s = p->schedule;
        const bool want = s.ourChannel == channel && s.shouldPause(nowUS);
        const uint32_t forced = p->pause.forcedResumes;
        p->pause.step(want, nowUS, s.pauseLimitUS(nowUS));
        forcedReleases += p->pause.forcedResumes - forced;
        if (p->pause.paused) ++holdChecks;
        return p->pause.paused;
    }
    unsigned known(uint64_t nowUS) const {
        unsigned count = 0;
        for (const auto &p : peers) count += p.occupied && p.schedule.known() && p.schedule.fresh(nowUS);
        return count;
    }
};
} // namespace ItlAWDL
