//
//  WCL.cpp
//  AirportItlwm
//
//  Created by laobamac on 2026/10/1.
//  Copyright © 2026 laobamac. All rights reserved.
//

#ifdef AIRPORT_WCL
#include "../AirportItlwmV2.hpp"
#include "../AirportItlwmSkywalkInterface.hpp"
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <net80211/ieee80211_node.h>
#include <net80211/ieee80211_ioctl.h>
#include <net80211/ieee80211_priv.h>
#include <crypto/sha1.h>
#include "qwz_log.h"
#include "qwz_life.h"	/* K1 (W2c): qwz_scanraw */
#include "qwz_mon.h"	/* K5 (L1-C11, C12): qwz_mon_scan_hold, qwz_mon_coex_running_ic, qwz_mon_set, qwz_moncoexjoin */
#define QWZLOG(fmt, ...)	qwz_log("qwz: " fmt "\n", ##__VA_ARGS__)
extern "C" int qwz_mon_holding(struct ieee80211com *);	// qwz/qwz_mon.h (M3 hold)

extern IOCommandGate *_fCommandGate;

struct AirportItlwmSkywalkInterface::CountryUpdate {
    AirportItlwmSkywalkInterface *interface;
    AirportItlwm *controller;
    IOCommandGate *gate;
    IO80211WorkQueue *queue;
    thread_call_t call;
};

void AirportItlwmSkywalkInterface::deferWCLCountryUpdate()
{
    auto queue = instance->getWorkQueue();
    if (wclStopping || !_fCommandGate || !queue)
        return;
    auto update = static_cast<CountryUpdate *>(IOMalloc(sizeof(CountryUpdate)));
    if (!update)
        return;
    update->call = thread_call_allocate(wclCountryUpdateThread, update);
    if (!update->call) {
        IOFree(update, sizeof(*update));
        return;
    }
    update->interface = this;
    update->controller = instance;
    update->gate = _fCommandGate;
    update->queue = queue;
    retain();
    instance->retain();
    update->gate->retain();
    queue->retain();
    thread_call_enter(update->call);
}

void AirportItlwmSkywalkInterface::wclCountryUpdateThread(void *arg, void *)
{
    auto update = static_cast<CountryUpdate *>(arg);
    // Native country notifications synchronously query WCL. Holding the gate
    // is necessary, but executing on the driver's workloop thread is forbidden.
    update->gate->runAction(wclCountryUpdateGated, update);
    thread_call_free(update->call);
    update->interface->release();
    update->controller->release();
    update->gate->release();
    update->queue->release();
    IOFree(update, sizeof(*update));
}

IOReturn AirportItlwmSkywalkInterface::wclCountryUpdateGated(
    OSObject *, void *arg, void *, void *, void *)
{
    auto update = static_cast<CountryUpdate *>(arg);
    if (update->interface->wclStopping)
        return kIOReturnNotReady;
    if (!update->queue->inGate() || update->queue->onThread())
        return kIOReturnNotPermitted;
    update->interface->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, nullptr, 0, false);
    return kIOReturnSuccess;
}

bool AirportItlwmSkywalkInterface::initWCL()
{
    joinTimer = IOTimerEventSource::timerEventSource(this, wclJoinTimeout);
    if (!joinTimer)
        return false;
    return instance->getWorkQueue()->addEventSource(joinTimer) == kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::stopWCL()
{
    // Pending country tasks must observe shutdown under the same gate.
    if (_fCommandGate)
        _fCommandGate->runAction(stopWCLGated, this);
    wclStopping = true;
    if (joinTimer) {
        joinTimer->cancelTimeout();
        if (joinTimer->getWorkLoop())
            joinTimer->getWorkLoop()->removeEventSource(joinTimer);
        OSSafeReleaseNULL(joinTimer);
    }
    if (scanSource)
        scanSource->cancelTimeout();
    scanPending = joinPending = false;
    scanRestartPending = scanSkipCompletion = false;
    joinDeferred = false;
    explicit_bzero(&deferredJoin, sizeof(deferredJoin));
    if (fHalService) {
        ieee80211com *ic = fHalService->get80211Controller();
        ic->ic_wcl_join_requested = false;
        ic->ic_wcl_scan_requested = false;
        explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
    }
}

IOReturn AirportItlwmSkywalkInterface::stopWCLGated(
    OSObject *, void *arg, void *, void *, void *)
{
    static_cast<AirportItlwmSkywalkInterface *>(arg)->wclStopping = true;
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::free()
{
    if (joinTimer)
        stopWCL();
    IO80211InfraProtocol::free();
}

IOReturn AirportItlwmSkywalkInterface::wclCommand(OSObject *, void *a0, void *a1, void *a2, void *a3)
{
    auto self = static_cast<AirportItlwmSkywalkInterface *>(a0);
    if (self->wclStopping)
        return kIOReturnNotReady;
    switch (uintptr_t(a1)) {
        case 13: return self->copyWCLChannels(a2);
        case 14: return self->copyWCLTrafficCounters(a2);
        case 15:
            return self->fHalService->setScanHomeAwayTime(
                AirportWCL::read32(static_cast<const uint8_t *>(a2)));
        case 12:
            self->handleWCLEvent(int(uintptr_t(a2)), a3);
            return kIOReturnSuccess;
        case 11: self->clearWCLPMKSA(); return kIOReturnSuccess;
        case 8: return self->installWCLKey(static_cast<apple80211_key *>(a2));
        case 9: return self->accessWCLRSN(static_cast<apple80211_rsn_ie_data *>(a2), true);
        case 10: return self->accessWCLRSN(static_cast<apple80211_rsn_ie_data *>(a2), false);
        case 0: return self->beginWCLScan(static_cast<apple80211ScanRequest *>(a2));
        case 1: return self->beginWCLJoin(static_cast<apple80211AssocCandidates *>(a2));
        case 2: return self->abortWCLJoin();
        case 3: return self->leaveWCLNetwork();
        case 6: return self->copyWCLExtendedBss(a2);
        case 7:
            if (!a2 || !self->wclBssValid || !self->wclAssociatedBeacons)
                return kIOReturnNotReady;
            memcpy(a2, self->wclBeaconInfo, sizeof(self->wclBeaconInfo));
            return kIOReturnSuccess;
        case 5:
            if (!a2 || !self->wclBssValid)
                return kIOReturnNotReady;
            memcpy(a2, self->wclBssInfo, sizeof(self->wclBssInfo));
            return kIOReturnSuccess;
        case 4:
            self->finishWCLScan(kIOReturnAborted);
            return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_SCAN_REQ(apple80211ScanRequest *request)
{
    return _fCommandGate->runAction(wclCommand, this, nullptr, request);
}

IOReturn AirportItlwmSkywalkInterface::setWCL_SCAN_ABORT(void *)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(4));
}

IOReturn AirportItlwmSkywalkInterface::setWCL_ASSOCIATE(apple80211AssocCandidates *request)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(1), request);
}

#if __IO80211_TARGET == __MAC_15_2
IOReturn AirportItlwmSkywalkInterface::setWCL_JOIN_ABORT(void *)
#else
IOReturn AirportItlwmSkywalkInterface::setWCL_JOIN_ABORT(apple80211_wcl_abort_join *)
#endif
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(2));
}

IOReturn AirportItlwmSkywalkInterface::setWCL_LEAVE_NETWORK(apple80211_leave_network *)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(3));
}

// ---- Scan request diagnostics (S2, docs/research-hotspot-wifi-2026-11-07.md): log only, nothing here changes a scan. ----
// One line per scan request with the outcome beginWCLScan reached, and one per finishWCLScan. The SSID is never printed, only its
// length and a 32-bit FNV-1a hash (the join lines print only ssid_len as well). Lines that carry a list or a primary SSID (a directed
// request), a Busy, a STRANDED or any other non-launch outcome use their own counter and are never rate limited; the plain launched
// lines (no list, no SSID) are limited.
#ifdef QWZ_DEBUG
extern uint32_t qwz_debug;
#define WCL_SCAN_DUMP_MASK	0x00000200	/* own bit of qwz_debug, unused in qwzvar.h (QWZ_D_MGMT 0x80 would also enable the per-frame printf traces): default off even in a debug build */
#endif

static uint64_t wclScanStartMs;	/* when the latest request was latched, for the age in the finish line */
static uint32_t wclScanStartId;

struct WCLScanLogState {
    bool scanPending;
    bool joinPending;
    bool skip;
    bool bgscan;
    int state;
    int idle;
    int prevSsidLength;	/* ic_wcl_scan_ssid_length before this request overwrote it, -1 when not captured */
};

static uint64_t wclScanNowMs(void)
{
    uint64_t t;

    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &t);
    return t / 1000000ULL;
}

static uint32_t wclScanHash(const uint8_t *data, size_t length)
{
    uint32_t h = 2166136261u;

    for (size_t i = 0; i < length; ++i)
        h = (h ^ data[i]) * 16777619u;
    return h;
}

// The conditions of AirportWCL::decodeScan (WCL.hpp) on the raw bytes, for the log line only; same order, 0x1550 as the length.
static bool wclScanDecodeFault(const void *buffer, char *out, size_t outSize)
{
    if (!buffer) {
        snprintf(out, outSize, "request NULL");
        return true;
    }
    const uint8_t *p = static_cast<const uint8_t *>(buffer);
    uint32_t count = AirportWCL::read32(p + 0x54), ssidLength = AirportWCL::read32(p + 0x1c);
    uint32_t type = AirportWCL::read32(p + 0x40);
    if (count > 400 || ssidLength > 32 || type < 1 || type > 4 || 0x1550 < 0x58 + size_t(count) * 12) {
        snprintf(out, outSize, "bad field:%s%s%s", count > 400 || 0x1550 < 0x58 + size_t(count) * 12 ? " channel_count" : "",
                 ssidLength > 32 ? " ssid_len" : "", type < 1 || type > 4 ? " type" : "");
        return true;
    }
    return false;
}

#ifdef QWZ_DEBUG
static void wclScanHex(char *out, size_t outSize, const uint8_t *data, size_t length)
{
    size_t pos = 0;

    for (size_t i = 0; i < length && pos + 3 <= outSize; ++i)
        pos += size_t(snprintf(out + pos, outSize - pos, "%02x", data[i]));
}
#endif

// K1 (W2c): the non-zero 32-bit words of [from, to) of the scan request as 'offset=value' pairs, at most 20 words to a line and 3 lines per region
// (the rest is counted). Used by debug.qwz.scanraw in every build. The words may hold SSID text: redact before sharing.
static void wclScanRawWords(unsigned int seq, const uint8_t *p, size_t from, size_t to)
{
    char line[20 * 16 + 1];
    size_t pos = 0;
    unsigned int inLine = 0, lines = 0, extra = 0, nonzero = 0;

    for (size_t off = from; off + 4 <= to; off += 4) {
        uint32_t w = AirportWCL::read32(p + off);
        if (!w)
            continue;
        ++nonzero;
        if (lines >= 3) {
            ++extra;
            continue;
        }
        pos += size_t(snprintf(line + pos, sizeof(line) - pos, " %zx=%08x", off, w));
        if (++inLine == 20) {
            qwz_log("airport: WCL scan raw #%u: [0x%zx,0x%zx)%s\n", seq, from, to, line);
            pos = 0;
            inLine = 0;
            ++lines;
        }
    }
    if (inLine)
        qwz_log("airport: WCL scan raw #%u: [0x%zx,0x%zx)%s\n", seq, from, to, line);
    qwz_log("airport: WCL scan raw #%u: [0x%zx,0x%zx) %u non-zero words, %u not listed\n", seq, from, to, nonzero, extra);
}

static void logWCLScanRequest(const void *request, const AirportWCL::ScanRequest *scan, const WCLScanLogState &st,
                              const char *outcome, const char *detail)
{
    static unsigned int plain, notable;
    const uint8_t *p = static_cast<const uint8_t *>(request);
    uint32_t list = p ? AirportWCL::read32(p + 0x1318) : 0;	// 0x1550 is the length decodeScan is given; the framework buffer is 0x1598
    bool launched = !strncmp(outcome, "launched", 8);
    unsigned int seq;
    if (list || !launched || (scan && scan->ssidLength))
        seq = __atomic_add_fetch(&notable, 1u, __ATOMIC_RELAXED);
    else if (qwz_log_ratelimit(&plain, 24, 8))
        seq = plain;
    else
        return;
    char chans[48] = "";
    if (scan && p) {
        size_t pos = 0;
        for (uint32_t i = 0; i < scan->channelCount && i < 3 && pos + 16 < sizeof(chans); ++i)
            pos += size_t(snprintf(chans + pos, sizeof(chans) - pos, " %u/0x%x", AirportWCL::read32(p + 0x58 + size_t(i) * 12 + 4),
                                     AirportWCL::read32(p + 0x58 + size_t(i) * 12 + 8)));
    }
    bool zero = true;
    if (p)
        for (int i = 0; i < 6; ++i)
            if (p[0x14 + i])
                zero = false;
    char prev[24] = "";
    if (st.prevSsidLength >= 0)
        snprintf(prev, sizeof(prev), " prev_ssid_len %d", st.prevSsidLength);
    // K2c (C6): the words the family builds into the request (flags +0x44, active dwell +0x48, passive dwell +0x4c, rest time +0x50), on every non-plain line
    char famWords[72] = "";
    if (p && !(launched && !list && !(scan && scan->ssidLength)))
        snprintf(famWords, sizeof(famWords), " family flags 0x%x active %u passive %u rest %u", AirportWCL::read32(p + 0x44), AirportWCL::read32(p + 0x48),
                 AirportWCL::read32(p + 0x4c), AirportWCL::read32(p + 0x50));
    qwz_log("airport: WCL scan req%s #%u: id %u type %u ssid_len %u ssid_hash %08x bssid %s ch %u first%s list %u probes %u | "
            "state %s idle %d scanPending %d joinPending %d bgscan %d skip %d%s | %s (%s)%s\n",
            launched && !list && !(scan && scan->ssidLength) ? "" : "!", seq, p ? AirportWCL::read32(p) : 0, p ? AirportWCL::read32(p + 0x40) : 0,
            p ? AirportWCL::read32(p + 0x1c) : 0, scan ? wclScanHash(scan->ssid, scan->ssidLength) : 0,
            zero ? "zero" : "set", scan ? scan->channelCount : (p ? AirportWCL::read32(p + 0x54) : 0), chans, list, scan ? scan->probeSSIDCount : 0,
            st.state >= 0 && st.state < IEEE80211_S_MAX ? ieee80211_state_name[st.state] : "?", st.idle,
            st.scanPending, st.joinPending, st.bgscan, st.skip, prev, outcome, detail, famWords);
#ifdef QWZ_DEBUG
    static unsigned int dumps;
    // Raw bytes +0x44..+0x57 and the first 0x40 at +0x1318 for the first three requests that carry a list. They may hold SSID text:
    // redact before sharing. Needs bit 0x200 of qwz_debug (WCL_SCAN_DUMP_MASK; boot-arg qwz_debug or debug.qwz.debug, debug builds only).
    if (p && list && (qwz_debug & WCL_SCAN_DUMP_MASK) && __atomic_add_fetch(&dumps, 1u, __ATOMIC_RELAXED) <= 3) {
        char hex[2 * 0x40 + 1] = "";
        wclScanHex(hex, sizeof(hex), p + 0x44, 0x14);
        qwz_log("airport: WCL scan raw #%u: +0x44 %s\n", seq, hex);
        wclScanHex(hex, sizeof(hex), p + 0x1318, 0x40);
        qwz_log("airport: WCL scan raw #%u: +0x1318 %s\n", seq, hex);
    }
#endif
    {
        // K1 (W2c): debug.qwz.scanraw, every build, default 0. 1: the first three non-plain requests (a list, a primary SSID or a non-launch outcome);
        // 2: also the first three plain launched ones. The tail is +0x44..+0x58 (after the type word) and +0x1318..+0x1598 (the list and what follows it).
        static unsigned int rawNonPlain, rawPlain, rawGen;
        int mode = qwz_scanraw;
        // K2c (C6): a write to debug.qwz.scanraw starts the budgets again; a scan answered empty during the monitor hold ("hold") counts as plain
        if (qwz_log_rearmed(&rawGen, &rawNonPlain))
            rawPlain = 0;
        bool plain = (launched || !strncmp(outcome, "hold", 4)) && !list && !(scan && scan->ssidLength);
        if (p && mode >= 1 && (!plain || mode >= 2) &&
            __atomic_add_fetch(plain ? &rawPlain : &rawNonPlain, 1u, __ATOMIC_RELAXED) <= 3) {
            wclScanRawWords(seq, p, 0x44, 0x58);
            wclScanRawWords(seq, p, 0x1318, 0x1598);
        }
    }
}

IOReturn AirportItlwmSkywalkInterface::beginWCLScan(apple80211ScanRequest *request)
{
    AirportWCL::ScanRequest scan;
    // S2: log only. The outcome is logged at each decision point, ahead of the statement that returns there.
    bool scanDecoded = false;
    auto scanLog = [&](const char *outcome, const char *detail, int state = -1, int idle = -1, int prevSsidLength = -1) {
        ieee80211com *lic = fHalService->get80211Controller();
        if (!lic)
            return;
        WCLScanLogState st = {scanPending, joinPending, scanSkipCompletion, bool(lic->ic_flags & IEEE80211_F_BGSCAN), state >= 0 ? state : int(lic->ic_state),
                              idle >= 0 ? idle : int(lic->ic_wcl_scan_idle), prevSsidLength};
        logWCLScanRequest(request, scanDecoded ? &scan : nullptr, st, outcome, detail);
    };
    {
        char fault[48];
        if (wclScanDecodeFault(request, fault, sizeof(fault)))
            scanLog("decode-failure", fault);
    }
    if (!AirportWCL::decodeScan(request, 0x1550, scan)) {
        scanLog("BadArgument", "scan or SSID list validation failed");
        return kIOReturnBadArgument;
    }
    scanDecoded = true;
    ieee80211com *ic = fHalService->get80211Controller();
    // S2: the same gates as the statements below, in the same order; each logs its outcome and only the first one that holds is reached.
    if (!(ic->ic_if.if_flags & IFF_RUNNING))
        scanLog("NotReady", "interface not running");
    else if (scanPending)
        scanLog("Busy", "gate scanPending");
    else if (joinPending)
        scanLog("Busy", "gate joinPending");
    else if (ic->ic_flags & IEEE80211_F_BGSCAN)
        scanLog("Busy", "gate F_BGSCAN");
    else if (ic->ic_state == IEEE80211_S_AUTH || ic->ic_state == IEEE80211_S_ASSOC)
        scanLog("Busy", "gate state AUTH/ASSOC");
    if (!(ic->ic_if.if_flags & IFF_RUNNING))
        return kIOReturnNotReady;
    if (AirportWCL::scanNeedsRadio(scan, request, 0x1550)) {
        const int paused = qwz_mon_native_pause(ic);
        if (paused != 0 && paused != ENOTSUP) {
            scanLog("Busy", "native AWDL pause failed");
            return kIOReturnBusy;
        }
        if (paused == 0)
            scanLog("directed", "native AWDL paused for firmware scan");
    }
    if (qwz_mon_scan_hold(ic) && !qwz_mon_holding(ic)) {
        // K5 (L1-C11): a coex cell shares the radio with the running station (debug.qwz.moncoexscan 1): no firmware scan may take the radio off the
        // cell's channel, so the scan completes at once and the node table keeps the results it has. Same completion as the hold below.
        static unsigned int coexScans;
        if (qwz_log_ratelimit(&coexScans, 24, 8))
            scanLog("coex", "coex: answered from the node table");
        scanPending = true;
        finishWCLScan(kIOReturnSuccess);
        return kIOReturnSuccess;
    }
    if (qwz_mon_holding(ic)) {
        {
            // K1 (W2c): S2 line for the scans answered empty at once during the hold; its own counter, first 24 then every 8th
            static unsigned int holdScans;
            if (qwz_log_ratelimit(&holdScans, 24, 8))
                scanLog("hold", "monhold: answered empty at once");
        }
        // M3 hold: the radio is the AWDL cell; every scan finds nothing.
        scanPending = true;
        finishWCLScan(kIOReturnSuccess);
        return kIOReturnSuccess;
    }
    if (scanPending || joinPending || (ic->ic_flags & IEEE80211_F_BGSCAN))
        return kIOReturnBusy;
    if (ic->ic_state == IEEE80211_S_AUTH || ic->ic_state == IEEE80211_S_ASSOC)
        return kIOReturnBusy;
    if (scan.channelCount) {
        bool usable = false;
        for (unsigned channel = 1; channel <= IEEE80211_CHAN_MAX; ++channel)
            if (ic->ic_channels[channel].ic_flags &&
                AirportWCL::scanChannelSelected(scan.channels, true, channel))
                usable = true;
        if (!usable)
            scanLog("Unsupported", "no usable channel");
        if (!usable)
            return kIOReturnUnsupported;
    }
    const int logState = ic->ic_state, logIdle = ic->ic_wcl_scan_idle, logPrev = ic->ic_wcl_scan_ssid_length;
    wclScanStartMs = wclScanNowMs();
    wclScanStartId = scan.identifier;
    scanPending = true;
    scanCompleted = false;
    scanRestartPending = false;
    // The in-flight firmware sweep was built before this request was latched.
    // Its completion cannot satisfy this request, including a list-bearing
    // hotspot scan whose primary SSID is empty. Restart after its real end.
    scanSkipCompletion = ic->ic_state == IEEE80211_S_SCAN && !ic->ic_wcl_scan_idle;
    ic->ic_wcl_scan_requested = true;
    ic->ic_wcl_scan_active = scan.type != 2;
    ic->ic_wcl_scan_ssid_length = scan.type == 2 ? 0 : scan.ssidLength;
    memcpy(ic->ic_wcl_scan_ssid, scan.ssid, sizeof(scan.ssid));
    static_assert(sizeof(ic->ic_wcl_scan_ssids) / sizeof(ic->ic_wcl_scan_ssids[0]) == AirportWCL::kScanSSIDCapacity,
                  "WCL and firmware scan SSID capacities must agree");
    ic->ic_wcl_scan_ssid_count = scan.type == 2 ? 0 : scan.probeSSIDCount;
    for (unsigned i = 0; i < ic->ic_wcl_scan_ssid_count; ++i) {
        ic->ic_wcl_scan_ssid_lengths[i] = scan.probeSSIDs[i].length;
        memcpy(ic->ic_wcl_scan_ssids[i], scan.probeSSIDs[i].bytes, 32);
    }
    ic->ic_wcl_scan_restricted = scan.channelCount != 0;
    memcpy(ic->ic_wcl_scan_channels, scan.channels, sizeof(scan.channels));
    scanSource->setTimeoutMS(20000);
    if (ic->ic_state == IEEE80211_S_RUN) {
        ieee80211_begin_cache_bgscan(&ic->ic_if);
        if (!(ic->ic_flags & IEEE80211_F_BGSCAN)) {
            scanPending = false;
            ic->ic_wcl_scan_requested = false;
            scanSource->cancelTimeout();
            scanLog("Busy", "bgscan declined in RUN", logState, logIdle, logPrev);
            return kIOReturnBusy;
        }
    } else if (ic->ic_state != IEEE80211_S_SCAN) {
        int error = ic->ic_newstate(ic, IEEE80211_S_SCAN, -1);
        if (error) {
            scanPending = false;
            ic->ic_wcl_scan_requested = false;
            scanSource->cancelTimeout();
            scanLog("Error", "ic_newstate(SCAN) failed", logState, logIdle, logPrev);
            return kIOReturnError;
        }
    } else if (ic->ic_wcl_scan_idle) {
        // The stack waits for us between scans (ieee80211_end_scan).
        ic->ic_wcl_scan_idle = false;
        scanSkipCompletion = false;
        ieee80211_next_scan(&ic->ic_if);
    }
    // The active old sweep owns the radio until its real completion. Its
    // event schedules a deferred restart with the parameters latched above.
    if (logState == IEEE80211_S_RUN)
        scanLog("launched", "bgscan", logState, logIdle, logPrev);
    else if (logState != IEEE80211_S_SCAN)
        scanLog("launched", "newstate", logState, logIdle, logPrev);
    else if (logIdle)
        scanLog("launched", "restart", logState, logIdle, logPrev);
    else
        scanLog("deferred", "waiting for the in-flight scan before restart", logState, logIdle, logPrev);
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::finishWCLScan(IOReturn status)
{
    if (!scanPending)
        return;
    {
        // S2: log only. Non-success (Timeout after a strand, Aborted) always logs, success is rate limited on its own counter.
        static unsigned int done, doneNotable;
        ieee80211com *lic = fHalService->get80211Controller();
        bool notable = status != kIOReturnSuccess;
        if (notable)
            __atomic_add_fetch(&doneNotable, 1u, __ATOMIC_RELAXED);
        if (notable || qwz_log_ratelimit(&done, 24, 8))
            qwz_log("airport: WCL scan done%s #%u: id %u status 0x%x (%s) age %llu ms | completed %d skip %d state %s idle %d\n",
                    notable ? "!" : "", notable ? doneNotable : done, wclScanStartId,
                    unsigned(status), status == kIOReturnSuccess ? "Success" : status == kIOReturnTimeout ? "Timeout" :
                    status == kIOReturnAborted ? "Aborted" : "other", (unsigned long long)(wclScanNowMs() - wclScanStartMs),
                    scanCompleted, scanSkipCompletion, ieee80211_state_name[lic->ic_state], lic->ic_wcl_scan_idle);
    }
    scanPending = false;
    scanCompleted = false;
    scanRestartPending = scanSkipCompletion = false;
    scanSource->cancelTimeout();
    fHalService->get80211Controller()->ic_wcl_scan_requested = false;
    instance->postMessage(this, AirportWCL::ScanDone, &status, sizeof(status), true);
}

void AirportItlwmSkywalkInterface::completeWCLScan()
{
    if (wclStopping || !scanPending)
        return;
    if (scanRestartPending) {
        ieee80211com *ic = fHalService->get80211Controller();
        const uint64_t age = wclScanNowMs() - wclScanStartMs;
        if (age >= 20000) {
            finishWCLScan(kIOReturnTimeout);
            return;
        }
        if (ic->ic_state != IEEE80211_S_SCAN) {
            finishWCLScan(kIOReturnAborted);
            return;
        }
        if (!ic->ic_wcl_scan_idle) {
            // SCAN_DONE is posted before net80211 finishes end_scan. Yield
            // to that owner; never mark its firmware scan idle ourselves.
            scanSource->setTimeoutMS(10);
            return;
        }
        scanRestartPending = false;
        scanCompleted = false;
        ic->ic_wcl_scan_idle = false;
        scanSource->setTimeoutMS(uint32_t(20000 - age));
        qwz_log("airport: WCL deferred scan restarted after %llu ms\n", (unsigned long long)age);
        ieee80211_next_scan(&ic->ic_if);
        return;
    }
    finishWCLScan(scanCompleted ? kIOReturnSuccess : kIOReturnTimeout);
}

void AirportItlwmSkywalkInterface::publishWCLBeacon(const ieee80211_node *node, const uint8_t *ies, size_t length, const uint8_t *frame, size_t frameLength)
{
    if (!node || node->ni_esslen > 32 || !AirportWCL::validIEs(ies, length))
        return;
    ieee80211com *ic = fHalService->get80211Controller();
    if (!node->ni_chan || node->ni_chan == IEEE80211_CHAN_ANYC)
        return;
    unsigned channel = ieee80211_chan2ieee(ic, node->ni_chan);
    if (!channel || channel > 196)
        return;
    size_t size = sizeof(AirportWCL::BeaconMetadata) + length;
    auto metadata = static_cast<AirportWCL::BeaconMetadata *>(IOMalloc(size));
    if (!metadata)
        return;
    *metadata = {};
    metadata->ieLength = uint32_t(length);
    metadata->channelSpec = uint16_t(channel | (IEEE80211_IS_CHAN_5GHZ(node->ni_chan) ? 0xd000 : 0x1000));
    metadata->channel = channel;
    metadata->ssidLength = node->ni_esslen;
    memcpy(metadata->ssid, node->ni_essid, node->ni_esslen);
    memcpy(metadata->bssid, node->ni_bssid, 6);
    metadata->rssi = rssiForNode(node);  /* dBm (qwz stores it as uint8, read as signed); smoothed only for the connected BSS */
    metadata->noise = fHalService->getDriverInfo()->getBSSNoise();  /* -95 dBm */
    metadata->snr = metadata->rssi - metadata->noise;  /* e.g. -48 - (-95) = 47 dB */
    metadata->interval = node->ni_intval;
    metadata->capability = node->ni_capinfo;
    metadata->flags = AirportWCL::SSIDPresent | AirportWCL::RSSIValid;
    if (qwz_beaconmeta) {	/* K3 (S2): noise is -95 and snr = rssi - noise, both already set above */
        metadata->flags |= AirportWCL::OnChannel | AirportWCL::NoiseValid | AirportWCL::SNRValid;
        if (frame && frameLength >= 1 && (frame[0] & 0xfc) == 0x80)	/* beacon, not a probe response */
            metadata->flags |= AirportWCL::FromBeacon;
    }
    {	/* K3: debug.qwz.rssilog, the first 8 publications of the boot; no addresses */
        static unsigned int rssiLogged;
        if (qwz_rssilog && rssiLogged < 8) {
            rssiLogged++;
            qwz_log("airport: K3 publish #%u: ch %u raw ni_rssi byte %u sent rssi %d noise %d snr %d flags 0x%x beaconmeta %d\n",
                rssiLogged, channel, (unsigned)node->ni_rssi, (int)metadata->rssi, (int)metadata->noise,
                (int)metadata->snr, (unsigned)metadata->flags, qwz_beaconmeta);
        }
    }
    memcpy(metadata + 1, ies, length);
    metadata->ieLength = uint32_t(AirportWCL::selectWPA2TransitionMode(
        reinterpret_cast<uint8_t *>(metadata + 1), length));
    size_t messageSize = sizeof(*metadata) + metadata->ieLength;
    if (assocNotified && IEEE80211_ADDR_EQ(node->ni_bssid, joinBSSID)) {
        wclBssValid = messageSize <= sizeof(wclBssInfo);
        if (wclBssValid) {
            bzero(wclBssInfo, sizeof(wclBssInfo));
            memcpy(wclBssInfo, metadata, messageSize);
        }
    }
    instance->postMessage(this, AirportWCL::ScanResult, metadata, messageSize, true);
    IOFree(metadata, size);
}

void AirportItlwmSkywalkInterface::clearWCLPMKSA()
{
    auto ic = fHalService->get80211Controller();
    ieee80211_pmk *pmk;
    while ((pmk = TAILQ_FIRST(&ic->ic_pmksa))) {
        TAILQ_REMOVE(&ic->ic_pmksa, pmk, pmk_next);
        explicit_bzero(pmk, sizeof(*pmk));
        ::free(pmk);
    }
}

IOReturn AirportItlwmSkywalkInterface::setCLEAR_PMKSA_CACHE(void *)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(11));
}

IOReturn AirportItlwmSkywalkInterface::installWCLKey(apple80211_key *key)
{
    static_assert(sizeof(apple80211_key) == 0x94, "WCL cipher key layout");
    if (!key || !AirportWCL::validCipherKey(key->key_cipher_type, key->key_len,
            key->key_flags, key->key_index, key->key_rsc_len))
        return kIOReturnBadArgument;
    auto ic = fHalService->get80211Controller();
    auto ni = ic->ic_bss;
    if (!ic->ic_wcl_enterprise || !ni || ic->ic_state != IEEE80211_S_RUN)
        return kIOReturnNotReady;
    if (key->key_cipher_type == APPLE80211_CIPHER_PMK ||
        key->key_cipher_type == APPLE80211_CIPHER_PMKSA ||
        key->key_cipher_type == APPLE80211_CIPHER_MSK) {
        // WCL's native user-client rejects PTK/GTK, and delivers a PMK/MSK
        // instead. Use the existing net80211 four-way/group-key handshake.
        if (!ieee80211_is_8021x_akm((enum ieee80211_akm)ni->ni_rsnakms))
            return kIOReturnBadArgument;
        uint8_t any = 0;
        for (auto byte : key->key_ea.octet) any |= byte;
        if (any && !IEEE80211_ADDR_EQ(key->key_ea.octet, ni->ni_bssid))
            return kIOReturnBadArgument;
        auto pmk = ieee80211_pmksa_add(ic, (enum ieee80211_akm)ni->ni_rsnakms,
                                     ni->ni_macaddr, key->key, 0);
        IOReturn result = pmk ? kIOReturnSuccess : kIOReturnNoMemory;
        return result;
    }
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmSkywalkInterface::accessWCLRSN(apple80211_rsn_ie_data *data, bool write)
{
    if (!data) return kIOReturnBadArgument;
    auto ic = fHalService->get80211Controller();
    if (write) {
        static unsigned int rsnSets;
        bool valid = AirportWCL::validRSNOverride(data->ie, data->len);
        if (qwz_log_ratelimit(&rsnSets, 24, 64)) {
            char rsn[96];
            AirportWCL::formatRsnSummary(data->ie, data->len <= sizeof(data->ie) ? data->len : sizeof(data->ie),
                                         rsn, sizeof(rsn));
            qwz_log("airport: setRSN_IE #%u: len %u valid %d %s\n", rsnSets, unsigned(data->len), valid, rsn);
        }
        if (!valid)
            return kIOReturnBadArgument;
        // net80211 constructs negotiated IEs for its own key handshake.
        return kIOReturnUnsupported;

    }
    const uint8_t *ie = ic->ic_bss ? ic->ic_bss->ni_rsnie : nullptr;
    if (!ie) return kIOReturnNotReady;
    size_t length = size_t(ie[1]) + 2;
    if (length > sizeof(data->ie)) return kIOReturnBadArgument;
    bzero(data, sizeof(*data));
    data->version = APPLE80211_VERSION;
    data->len = length;
    memcpy(data->ie, ie, length);
    return kIOReturnSuccess;
}

// ---- Join diagnostics (WCL.hpp): lengths, types and flags into the qwz_log ring. ----
// Nothing here ever prints key bytes, passwords, PMKs or a hash of them.

bool AirportWCL::printableKey(const uint8_t *key, size_t length)
{
    if (!key || !length)
        return false;
    for (size_t i = 0; i < length; ++i)
        if (key[i] < 0x20 || key[i] > 0x7e)
            return false;
    return true;
}

size_t AirportWCL::formatRsnSummary(const uint8_t *ie, size_t length, char *out, size_t outSize)
{
    if (!out || outSize < 16)
        return 0;
    out[0] = '\0';
    if (!ie || length < 2) {
        snprintf(out, outSize, "none");
        return 0;
    }
    size_t total = size_t(ie[1]) + 2;
    if (total > length)
        total = length;
    size_t p;
    if (ie[0] == 48) {
        p = 2;
    } else if (ie[0] == 221 && total >= 6 && ie[2] == 0x00 && ie[3] == 0x50 &&
               ie[4] == 0xf2 && ie[5] == 1) {
        p = 6;
    } else {
        snprintf(out, outSize, "ie id %u is not RSN/WPA", ie[0]);
        return 0;
    }
    // version(2) group cipher(4) pairwise count(2) pairwise suites, AKM count(2) AKM suites, caps(2)
    if (p + 8 > total) {
        snprintf(out, outSize, "truncated");
        return 0;
    }
    p += 2 + 4;
    size_t pairwise = read16(ie + p);
    p += 2;
    if (pairwise > (total - p) / 4) {
        snprintf(out, outSize, "truncated");
        return 0;
    }
    p += pairwise * 4;
    if (p + 2 > total) {
        snprintf(out, outSize, "truncated");
        return 0;
    }
    size_t akms = read16(ie + p);
    p += 2;
    size_t pos = size_t(snprintf(out, outSize, "akm="));
    size_t found = 0;
    for (size_t i = 0; i < akms && p + 4 <= total; ++i, p += 4) {
        int n = snprintf(out + pos, outSize - pos, "%s%02x%02x%02x:%u",
                         found ? "," : "", ie[p], ie[p + 1], ie[p + 2], ie[p + 3]);
        if (n < 0 || size_t(n) >= outSize - pos)
            break;
        pos += size_t(n);
        ++found;
    }
    if (!found && pos < outSize)
        pos += size_t(snprintf(out + pos, outSize - pos, "-"));
    if (found == akms && p + 2 <= total && pos < outSize)
        snprintf(out + pos, outSize - pos, " caps=0x%04x", read16(ie + p));
    else if (found != akms && pos < outSize)
        snprintf(out + pos, outSize - pos, " (%u of %u parsed)", unsigned(found), unsigned(akms));
    return found;
}

void AirportWCL::logJoinRaw(const void *buffer, size_t length)
{
    static unsigned int calls;
    if (!qwz_log_ratelimit(&calls, 24, 64))
        return;
    if (!buffer || length < 0x1e0) {
        qwz_log("airport: WCL join raw #%u: buffer %p length 0x%lx too short to read\n",
                calls, buffer, (unsigned long)length);
        return;
    }
    const uint8_t *p = static_cast<const uint8_t *>(buffer);
    uint32_t lower = read32(p + 0x10), upper = read32(p + 0x14);
    uint32_t ssidLength = read32(p + 0x1c);
    uint32_t keyLength = read32(p + 0x44), keyCipher = read32(p + 0x48);
    uint16_t keyFlags = read16(p + 0x4c), keyIndex = read16(p + 0x4e);
    uint16_t rsnLength = read16(p + 0xd4);
    uint32_t count = length >= 0x218 ? read32(p + 0x214) : 0;
    // Same checks, same order as decodeJoin below: which one a rejected join trips.
    const char *verdict = "accepted";
    if (length < 0x218)
        verdict = "REJECT buffer < 0x218";
    else if (!count || count > 10)
        verdict = "REJECT candidate count";
    else if (!ssidLength || ssidLength > 32)
        verdict = "REJECT ssid length";
    else if (keyLength > 32)
        verdict = "REJECT keyLength > 32";
    else if (rsnLength > 257)
        verdict = "REJECT rsnLength > 257";
    else if (length < 0x218 + size_t(count) * 18)
        verdict = "REJECT buffer < candidates";
    else if (p[0x21c] & 1)
        verdict = "REJECT bssid multicast";
    else if (!(p[0x21c] | p[0x21d] | p[0x21e] | p[0x21f] | p[0x220] | p[0x221]))
        verdict = "REJECT bssid zero";
    // isPassphrase() as the join code evaluates it (it is false above 32 bytes);
    // `printable` says whether the whole key, up to the 64 bytes the request can
    // carry, would pass its character test.
    JoinRequest tmp = {};
    tmp.upperAuth = upper;
    tmp.keyLength = keyLength;
    tmp.keyCipher = keyCipher;
    if (keyLength <= sizeof(tmp.key))
        memcpy(tmp.key, p + 0x50, keyLength);
    bool passphrase = isPassphrase(tmp);
    explicit_bzero(&tmp, sizeof(tmp));
    bool printable = keyLength <= 64 && printableKey(p + 0x50, keyLength);
    char rsn[96];
    size_t rsnShown = rsnLength <= 257 ? rsnLength : 257;
    if (0xd6 + rsnShown > length)
        rsnShown = length - 0xd6;
    formatRsnSummary(p + 0xd6, rsnShown, rsn, sizeof(rsn));
    qwz_log("airport: WCL join raw #%u: auth lower %u upper 0x%x ssid_len %u | key len %u "
            "cipher %u%s flags 0x%x index %u isPassphrase %d printable %d | rsn_ie len %u %s | "
            "pmk_len? @1d8 %u @1dc %u | candidates %u | decodeJoin: %s\n",
            calls, lower, upper, ssidLength, keyLength, keyCipher,
            keyCipher == 10 ? " (PWD)" : keyCipher == 6 ? " (PMK)" : "",
            keyFlags, keyIndex, passphrase, printable, rsnLength, rsn,
            read32(p + 0x1d8), read32(p + 0x1dc), count, verdict);
}

IOReturn AirportItlwmSkywalkInterface::beginWCLJoin(apple80211AssocCandidates *request)
{
    AirportWCL::JoinRequest join;
    if (!AirportWCL::decodeJoin(request, 0x2cc, join)) {
        static unsigned int rejected;
        if (qwz_log_ratelimit(&rejected, 24, 64))
            qwz_log("airport: WCL join #%u: decodeJoin rejected the request -> kIOReturnBadArgument (reason: see the 'join raw' line just above)\n", rejected);
        return kIOReturnBadArgument;
    }
    if (qwz_mon_holding(fHalService->get80211Controller())) {
        // airportd retries a refused join; an unthrottled line per retry would flush the log ring.
        static unsigned int holdRefused;
        if (qwz_log_ratelimit(&holdRefused, 8, 64))
            QWZLOG("WCL join refused: the capture cell holds the radio");
        // startWCLJoin zeroes the request on every exit; this early return must too (it can hold a passphrase or PMK).
        explicit_bzero(&join, sizeof(join));
        return kIOReturnNotReady; // M3 hold: no station to join with
    }
    if (qwz_mon_coex_running_ic(fHalService->get80211Controller())) {
        const int paused = qwz_mon_native_pause(fHalService->get80211Controller());
        if (paused == 0)
            return startWCLJoin(join);
        if (paused != ENOTSUP) {
            explicit_bzero(&join, sizeof(join));
            return kIOReturnBusy;
        }
        // K5 (L1-C12): a coex cell shares the radio with the running station.
        if (qwz_moncoexjoin == 0) {
            static unsigned int coexRefused;
            if (qwz_log_ratelimit(&coexRefused, 1, 64))
                QWZLOG("WCL join refused: the AirDrop cell shares the radio (debug.qwz.mon=0 to leave)");
            explicit_bzero(&join, sizeof(join)); // startWCLJoin zeroes the request on every exit; this early return must too
            return kIOReturnBusy;
        }
        // moncoexjoin 1: a real stop of the cell (command gate), then the join proceeds. Blocking gate call from this thread, as AirportItlwm::stop does.
        QWZLOG("WCL join: stopping the AirDrop cell first (debug.qwz.moncoexjoin 1)");
        qwz_mon_set(0);
    }
    return startWCLJoin(join);
}

IOReturn AirportItlwmSkywalkInterface::startWCLJoin(AirportWCL::JoinRequest &join)
{
    auto auth = AirportWCL::classifyLegacyAuth(join);
    XYLog("WCL join ssid_len=%u bssid=%s chanspec=0x%x auth lower=%u upper=0x%x -> %s, key len=%u type=%u%s, rsn_ie len=%u\n",
          join.ssidLength, ether_sprintf(join.bssid), join.channelSpec, join.lowerAuth, join.upperAuth,
          AirportWCL::legacyAuthName(auth), join.keyLength, join.keyCipher,
          AirportWCL::isPassphrase(join) ? " (passphrase)" : "", join.rsnLength);
    // Upstream (M3) replaced the XYLog above (kprintf: serial console only) with this QwzMacLog ring line; the merge keeps both.
    QWZLOG("WCL join ssid_len=%u bssid=%s chanspec=0x%x auth lower=%u upper=0x%x -> %s, key len=%u type=%u%s, rsn_ie len=%u",
          join.ssidLength, ether_sprintf(join.bssid), join.channelSpec, join.lowerAuth, join.upperAuth,
          AirportWCL::legacyAuthName(auth), join.keyLength, join.keyCipher,
          AirportWCL::isPassphrase(join) ? " (passphrase)" : "", join.rsnLength);
    bool personal = auth == AirportWCL::LegacyAuth::Personal;
    bool enterprise = auth == AirportWCL::LegacyAuth::Enterprise;
    bool supported = auth != AirportWCL::LegacyAuth::Invalid;
    ieee80211com *ic = fHalService->get80211Controller();
    {
        static unsigned int started;
        if (qwz_log_ratelimit(&started, 24, 64)) {
            char rsn[96];
            AirportWCL::formatRsnSummary(join.rsn, join.rsnLength, rsn, sizeof(rsn));
            qwz_log("airport: WCL join start #%u: ssid_len %u bssid %s chanspec 0x%x auth lower %u "
                    "upper 0x%x -> %s (supported %d) | key len %u cipher %u isPassphrase %d | "
                    "rsn_ie len %u %s | state %d scanPending %d joinPending %d\n",
                    started, join.ssidLength, ether_sprintf(join.bssid), join.channelSpec,
                    join.lowerAuth, join.upperAuth, AirportWCL::legacyAuthName(auth), supported,
                    join.keyLength, join.keyCipher, AirportWCL::isPassphrase(join),
                    join.rsnLength, rsn, int(ic->ic_state), scanPending, joinPending);
        }
    }
    // Applying a private MAC restarts firmware asynchronously. WCL can issue
    // the association before that restart has completed; keep one request
    // until the HAL reports READY rather than making the user retry it.
    if (supported && !joinPending && ic->ic_wcl_mac_reconfig &&
        (ic->ic_if.if_flags & IFF_UP)) {
        deferredJoin = join;
        memcpy(joinBSSID, join.bssid, sizeof(joinBSSID));
        explicit_bzero(&join, sizeof(join));
        joinDeferred = joinPending = true;
        joinTimer->setTimeoutMS(30000);
        return kIOReturnSuccess;
    }
    if (!supported || !(ic->ic_if.if_flags & IFF_RUNNING) || joinPending || scanPending) {
        static unsigned int refused;
        if (qwz_log_ratelimit(&refused, 24, 64))
            qwz_log("airport: WCL join refused #%u: %s (supported %d running %d joinPending %d scanPending %d)\n",
                    refused, !supported ? "unsupported auth/key combination -> kIOReturnUnsupported" :
                    "radio not running or another join/scan pending -> kIOReturnBusy",
                    supported, !!(ic->ic_if.if_flags & IFF_RUNNING), joinPending, scanPending);
        // Upstream's (M3) variant of the refusal line above (it adds ic_state), rate-limited on its own so a retrying
        // airportd cannot flush the recovery lines out of the log ring.
        static unsigned int refusedState;
        if (qwz_log_ratelimit(&refusedState, 24, 64))
            QWZLOG("WCL join refused: supported=%d running=%d joinPending=%d scanPending=%d state=%d",
                  supported, (ic->ic_if.if_flags & IFF_RUNNING) != 0, joinPending, scanPending, ic->ic_state);
        explicit_bzero(&join, sizeof(join));
        return !supported ? kIOReturnUnsupported : kIOReturnBusy;
    }
    if (ic->ic_state == IEEE80211_S_RUN || ic->ic_state == IEEE80211_S_AUTH || ic->ic_state == IEEE80211_S_ASSOC)
        leaveWCLNetwork();
    ieee80211_disable_rsn(ic);
    ieee80211_disable_wep(ic);
    explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
    clearWCLPMKSA();
    ic->ic_flags &= ~(IEEE80211_F_AUTO_JOIN | IEEE80211_F_PSK);
    ic->ic_wcl_enterprise = enterprise;

    if (personal || enterprise) {
        ieee80211_wpaparams wpa = {};
        wpa.i_enabled = 1;
        if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA_PSK))
            wpa.i_protos |= IEEE80211_WPA_PROTO_WPA1;
        if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA2 | APPLE80211_AUTHTYPE_WPA2_PSK |
                              APPLE80211_AUTHTYPE_SHA256_PSK | APPLE80211_AUTHTYPE_SHA256_8021X))
            wpa.i_protos |= IEEE80211_WPA_PROTO_WPA2;
        if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2_PSK))
            wpa.i_akms |= IEEE80211_WPA_AKM_PSK;
        if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA2))
            wpa.i_akms |= IEEE80211_WPA_AKM_8021X;
        if (join.upperAuth & APPLE80211_AUTHTYPE_SHA256_PSK)
            wpa.i_akms |= IEEE80211_WPA_AKM_SHA256_PSK;
        if (join.upperAuth & APPLE80211_AUTHTYPE_SHA256_8021X)
            wpa.i_akms |= IEEE80211_WPA_AKM_SHA256_8021X;
        if (join.upperAuth & APPLE80211_AUTHTYPE_WPA3_SAE) {
            wpa.i_protos |= IEEE80211_WPA_PROTO_WPA2;
            wpa.i_akms |= IEEE80211_WPA_AKM_PSK;  /* SAE falls back to PSK AKM for now */
        }
        if (join.upperAuth & APPLE80211_AUTHTYPE_WPA3_FT_SAE) {
            wpa.i_protos |= IEEE80211_WPA_PROTO_WPA2;
            wpa.i_akms |= IEEE80211_WPA_AKM_PSK;
        }
        wpa.i_ciphers = IEEE80211_WPA_CIPHER_CCMP | IEEE80211_WPA_CIPHER_TKIP;
        if (join.rsnLength >= 8 && join.rsn[0] == IEEE80211_ELEMID_RSN &&
            join.rsn[4] == 0 && join.rsn[5] == 0x0f && join.rsn[6] == 0xac) {
            if (join.rsn[7] == 2) wpa.i_groupcipher = IEEE80211_WPA_CIPHER_TKIP;
            if (join.rsn[7] == 4) wpa.i_groupcipher = IEEE80211_WPA_CIPHER_CCMP;
        }
        int error = ieee80211_ioctl_setwpaparms(ic, &wpa);
        if (error && error != ENETRESET) {
            explicit_bzero(&join, sizeof(join));
            return kIOReturnBadArgument;
        }
        if (personal) {
            if (AirportWCL::isPassphrase(join)) {
                char passphrase[33] = {};
                memcpy(passphrase, join.key, join.keyLength);
                pbkdf2_sha1(passphrase, join.ssid, join.ssidLength, 4096,
                            ic->ic_psk, sizeof(ic->ic_psk));
                explicit_bzero(passphrase, sizeof(passphrase));
            } else
                memcpy(ic->ic_psk, join.key, sizeof(ic->ic_psk));
            ic->ic_flags |= IEEE80211_F_PSK;
            XYLog("WCL join PSK key check %02x\n",
                  ieee80211_key_check(ic->ic_psk, sizeof(ic->ic_psk)));
        } else if (join.keyLength == IEEE80211_PMK_LEN) {
            bool cached = true;
            if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA2))
                cached = ieee80211_pmksa_add(ic, IEEE80211_AKM_8021X, join.bssid, join.key, 0) != nullptr;
            if (cached && (join.upperAuth & APPLE80211_AUTHTYPE_SHA256_8021X))
                cached = ieee80211_pmksa_add(ic, IEEE80211_AKM_SHA256_8021X, join.bssid, join.key, 0) != nullptr;
            if (!cached) {
                clearWCLPMKSA();
                explicit_bzero(&join, sizeof(join));
                return kIOReturnNoMemory;
            }
        }
    } else if (auth == AirportWCL::LegacyAuth::Wep) {
        ieee80211_nwkey keys = {};
        keys.i_wepon = IEEE80211_NWKEY_WEP;
        keys.i_defkid = join.keyIndex + 1;
        keys.i_key[join.keyIndex].i_keylen = join.keyLength;
        keys.i_key[join.keyIndex].i_keydat = join.key;
        int error = ieee80211_ioctl_setnwkeys(ic, &keys);
        if (error && error != ENETRESET) {
            explicit_bzero(&join, sizeof(join));
            return kIOReturnBadArgument;
        }
    }
    ic->ic_des_esslen = join.ssidLength;
    memcpy(ic->ic_des_essid, join.ssid, sizeof(ic->ic_des_essid));
    memcpy(ic->ic_des_bssid, join.bssid, 6);
    memcpy(joinBSSID, join.bssid, 6);
    ic->ic_flags |= IEEE80211_F_DESBSSID;
    ic->ic_wcl_join_requested = true;
    ic->ic_wcl_scan_requested = false;
    current_authtype_lower = join.lowerAuth;
    current_authtype_upper = join.upperAuth;
    explicit_bzero(&join, sizeof(join));
    wclBssValid = false;
    joinPending = true;
    assocNotified = firstBeaconNotified = portAuthorized = false;
    wclAssociatedBeacons = 0;
    bzero(wclBeaconInfo, sizeof(wclBeaconInfo));
    disassocIsVoluntary = false;
    joinTimer->setTimeoutMS(30000);
    if (ic->ic_state == IEEE80211_S_SCAN && ic->ic_wcl_scan_idle) {
        // The stack waits for us between scans (ieee80211_end_scan).
        ic->ic_wcl_scan_idle = false;
        ieee80211_next_scan(&ic->ic_if);
    }
    if (ic->ic_state != IEEE80211_S_SCAN && ic->ic_newstate(ic, IEEE80211_S_SCAN, -1)) {
        joinPending = false;
        joinTimer->cancelTimeout();
        ic->ic_wcl_join_requested = false;
        clearWCLPMKSA();
        ic->ic_wcl_enterprise = false;
        explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
        return kIOReturnError;
    }
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::resumeWCLJoin()
{
    if (!joinDeferred || wclStopping)
        return;
    AirportWCL::JoinRequest join = deferredJoin;
    explicit_bzero(&deferredJoin, sizeof(deferredJoin));
    joinDeferred = joinPending = false;
    joinTimer->cancelTimeout();
    IOReturn status = startWCLJoin(join);
    explicit_bzero(&join, sizeof(join));
    if (status != kIOReturnSuccess) {
        joinPending = true;
        finishWCLJoin(1, 0, status);
    }
}

void AirportItlwmSkywalkInterface::finishWCLJoin(uint16_t status, uint16_t reason, IOReturn driverStatus)
{
    if (!joinPending)
        return;
    AirportWCL::ConnectionEvent event = {};
    event.status = status;
    event.reason = reason;
    memcpy(event.peers[0].bssid, joinBSSID, 6);
    event.peers[0].driverStatus = driverStatus;
    joinPending = false;
    joinDeferred = false;
    explicit_bzero(&deferredJoin, sizeof(deferredJoin));
    joinTimer->cancelTimeout();
    instance->postMessage(this, AirportWCL::ConnectComplete, &event, sizeof(event), true);
    if (status) {
        ieee80211com *ic = fHalService->get80211Controller();
        ic->ic_wcl_join_requested = false;
        clearWCLPMKSA();
        ic->ic_wcl_enterprise = false;
        explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
    }
}

void AirportItlwmSkywalkInterface::wclJoinTimeout(OSObject *owner, IOTimerEventSource *)
{
    auto self = OSDynamicCast(AirportItlwmSkywalkInterface, owner);
    if (self && !self->wclStopping) {
        self->finishWCLJoin(1, 1004, kIOReturnTimeout);
        self->leaveWCLNetwork();
    }
}

IOReturn AirportItlwmSkywalkInterface::leaveWCLNetwork()
{
    ieee80211com *ic = fHalService->get80211Controller();
    {
        // M0 (C30): who ended the association; ic_state and joinPending are read BEFORE this function changes them.
        static unsigned int leaves;
        if (qwz_log_ratelimit(&leaves, 200, 20))
            qwz_log("airport: wcl: LEAVE_NETWORK (ic_state %s, join pending %d)\n",
                    ieee80211_state_name[ic->ic_state], joinPending);
    }
    joinPending = false;
    joinDeferred = false;
    explicit_bzero(&deferredJoin, sizeof(deferredJoin));
    joinTimer->cancelTimeout();
    ic->ic_wcl_join_requested = false;
    wclAssociatedBeacons = 0;
    wclBssValid = false;
    disassocIsVoluntary = true;
    ic->ic_deauth_reason = IEEE80211_REASON_AUTH_LEAVE;
    if (ic->ic_state > IEEE80211_S_SCAN)
        ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
    ieee80211_disable_rsn(ic);
    ieee80211_disable_wep(ic);
    explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
    clearWCLPMKSA();
    ic->ic_wcl_enterprise = false;
    ic->ic_flags &= ~(IEEE80211_F_PSK | IEEE80211_F_DESBSSID);
    ic->ic_des_esslen = 0;
    bzero(ic->ic_des_essid, sizeof(ic->ic_des_essid));
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::abortWCLJoin()
{
    {
        // M0 (C30)
        static unsigned int aborts;
        ieee80211com *ic = fHalService->get80211Controller();
        if (qwz_log_ratelimit(&aborts, 200, 20))
            qwz_log("airport: wcl: JOIN_ABORT (ic_state %s, join pending %d)\n",
                    ieee80211_state_name[ic->ic_state], joinPending);
    }
    leaveWCLNetwork();
    instance->postMessage(this, AirportWCL::JoinAbortComplete, nullptr, 0, true);
    return kIOReturnSuccess;
}

/*
 * F2 (docs/research-wifi-only-services-2026-11-06.md): tell the kernel the link quality (LQM) of en1.
 * Nothing set it for this Wi-Fi interface: IOEthernetInterface::inputEvent only does so for Ethernet media and
 * IONetworkInterface::attachToDataLinkLayer once at attach (-2 here: valid, not active), and apsd refuses an
 * interface at -2 for its push connection (Find My requests, iMessage, FaceTime, iCloud push).
 * The Wi-Fi call is IO80211InfraInterface::setLQM (vtable slot 376, __ZN21IO80211InfraInterface6setLQMEy, already
 * imported by the vtable fill). It re-enters the main gate (held by every caller of this function), then hands the
 * value to the interface monitor, to IOSkywalkNetworkInterface::reportLinkQuality (ifnet_set_link_quality on the
 * ifnet that prepareBSDInterface stored) and to updateLinkStatus. The argument is the int sign-extended to 64 bits,
 * as Apple's driver passes it (movslq).
 * Two values only: 100 (good) while the link is up, -2 (off) when it goes down; no RSSI tier, as AirportItlwm's
 * pre-Skywalk setLinkQualityMetric(100). 1 to 10 would set the stack's LQM-abort flag for the connections on en1.
 * Sent from handleWCLLink only, which always runs in the main gate: normally on a thread-call worker that may sleep
 * (the setkey task for link up, qwz.c qwz_add_sta_key; the newstate task; the thread running qwz_stop), because the
 * interrupt action only queues those tasks (ic_set_key answers EBUSY, qwz_newstate queues). A link-up verdict can
 * also arrive inline on the receive path, i.e. on the main workloop thread (a retransmitted EAPOL message 3 or a
 * group-key message with the SECURE bit, ieee80211_pae_input.c); there setLQM only re-enters the held gate
 * (runAction is recursive) and takes short kernel locks, as the postMessage calls below do, and does not sleep or
 * wait for the gate itself (read in the disassembly, not run). Not sent (the ring line says why): before the BSD
 * interface exists or after it is gone (getBSDInterface() is NULL), once stop() began, and while a firmware DEAD is
 * pending (the teardown of a DEAD or no-link episode gets no new family call; the join after the restart sends 100 again).
 */
void AirportItlwmSkywalkInterface::reportLqm(int quality, const char *reason)
{
    static unsigned int lines;
    const char *skip = NULL;

    if (ml_at_interrupt_context())
        return;		/* the primary interrupt (QwzIntr::filter only claims): no mutex, no ring line; unreachable today */
    if (wclStopping || instance == NULL || instance->pmStopping)
        skip = "driver stopping";
    else if (getBSDInterface() == NULL)
        skip = "no BSD interface";
    else if (fHalService != NULL && fHalService->faultPending(NULL))
        skip = "firmware DEAD";
    if (skip == NULL)
        setLQM(static_cast<unsigned long long>(static_cast<long long>(quality)));
    if (qwz_log_ratelimit(&lines, 200, 20))
        qwz_log("airport: lqm: %d (%s%s%s)\n", quality, reason, skip != NULL ? ", not sent: " : "", skip != NULL ? skip : "");
}

void AirportItlwmSkywalkInterface::handleWCLLink(bool up)
{
    if (wclStopping)
        return;
    {
        // M0 (C30): the driver's link verdict, before it is applied
        static unsigned int links;
        if (qwz_log_ratelimit(&links, 200, 20))
            qwz_log("airport: wcl: link %s (join pending %d, link indicated %d)\n",
                    up ? "up" : "down", joinPending, wclLinkIndicated);
    }
    portAuthorized = up;
    reportLqm(up ? 100 : -2, up ? "link up" : "link down");
    if (!up && wclLinkIndicated) {
        AirportWCL::LinkStatusEvent event = {};
        memcpy(event.bssid, joinBSSID, sizeof(event.bssid));
        event.reason = 255;
        wclLinkIndicated = false;
        wclBssValid = false;
        instance->postMessage(this, AirportWCL::LinkStatus, &event, sizeof(event), true);
    }
    if (up && joinPending && firstBeaconNotified)
        finishWCLJoin(0, 0, kIOReturnSuccess);
}

void AirportItlwmSkywalkInterface::handleWCLEvent(int code, void *data)
{
    if (wclStopping)
        return;
    // Firmware initialization runs on systq, outside the native PostOffice gate.
    // Serialize only lifecycle notifications; never run synchronous WCL ioctls here.
    if ((code == IEEE80211_EVT_DRIVER_RESET_BEGIN || code == IEEE80211_EVT_DRIVER_READY ||
         code == IEEE80211_EVT_DRIVER_STOPPED || code == IEEE80211_EVT_DRIVER_INIT_FAILED) && !instance->getWorkQueue()->inGate()) {
        _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(12),
                                reinterpret_cast<void *>(uintptr_t(code)), data);
        return;
    }
    ieee80211com *ic = fHalService->get80211Controller();
    if (code == IEEE80211_EVT_DRIVER_RESET_BEGIN || code == IEEE80211_EVT_DRIVER_READY ||
        code == IEEE80211_EVT_DRIVER_STOPPED) {
        // M0 (C30): after the gate hop above, so each event is noted once. After an injected crash this shows whether
        // airportd re-joins by itself (DRIVER_STOPPED wipes the PSK, a join must follow DRIVER_READY without user action).
        static unsigned int events;
        if (qwz_log_ratelimit(&events, 200, 20))
            qwz_log("airport: wcl: event %s (mac_reconfig %d)\n",
                    code == IEEE80211_EVT_DRIVER_RESET_BEGIN ? "DRIVER_RESET_BEGIN" :
                    code == IEEE80211_EVT_DRIVER_READY ? "DRIVER_READY" : "DRIVER_STOPPED",
                    ic->ic_wcl_mac_reconfig ? 1 : 0);
    }
    switch (code) {
        case IEEE80211_EVT_DRIVER_INIT_FAILED:
            instance->requestInterfaceRecovery(int(uintptr_t(data)));
            break;
        case IEEE80211_EVT_COUNTRY_CODE_UPDATE:
            deferWCLCountryUpdate();
            break;
        case IEEE80211_EVT_DRIVER_RESET_BEGIN: {
            if (ic->ic_wcl_mac_reconfig) {
                break;
            }
            uint32_t reset = 0; // Native resetInterfacesBegin.
            instance->postMessage(this, AirportWCL::ResetInterface, &reset, sizeof(reset), true);
            break;
        }
        case IEEE80211_EVT_DRIVER_READY:
        case IEEE80211_EVT_DRIVER_STOPPED: {
            bool ready = code == IEEE80211_EVT_DRIVER_READY;
            if (!ready) {
                finishWCLScan(ic->ic_wcl_mac_reconfig ? kIOReturnAborted : kIOReturnOffline);
                finishWCLJoin(1, 0, kIOReturnOffline);
                portAuthorized = false;
                ic->ic_wcl_join_requested = false;
                clearWCLPMKSA();
                ic->ic_wcl_enterprise = false;
                explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
            }
            // An address application is requested by WCL itself, not a new
            // adapter reset. Replaying firmware-load events here rotates the
            // scan MAC again and recursively restarts the card.
            if (AirportWCL::consumeMacReconfiguration(ic->ic_wcl_mac_reconfig, ready)) {
                if (ready)
                    resumeWCLJoin();
                break;
            }
            if (ready) {
                uint32_t reset = 1; // Native resetInterfacesComplete, only after HAL success.
                instance->postMessage(this, AirportWCL::ResetInterface, &reset, sizeof(reset), true);
            }
            AirportWCL::DriverAvailability event = {};
            event.version = 3;
            event.available = ready;
            event.reason = ready ? 0xe0821803 : 0xe0821804;
            instance->postMessage(this, AirportWCL::DriverAvailable, &event, sizeof(event), true);
            if (ready)
                resumeWCLJoin();
            break;
        }
        case IEEE80211_EVT_BEACON: {
            auto beacon = static_cast<ieee80211_beacon_event *>(data);
            if (!beacon)
                break;
            // Retain the actual associated beacon, including its advancing TSF.
            // WCL polls BEACON_INFO to distinguish fresh reception from a stale BSS.
            if (assocNotified && (joinPending || portAuthorized) &&
                IEEE80211_ADDR_EQ(beacon->node->ni_bssid, joinBSSID) &&
                beacon->frame && beacon->frame_length >= 36 &&
                beacon->frame_length <= 0x800 &&
                (beacon->frame[0] & 0xfc) == IEEE80211_FC0_SUBTYPE_BEACON) {
                uint32_t length = uint32_t(beacon->frame_length);
                bzero(wclBeaconInfo, sizeof(wclBeaconInfo));
                memcpy(wclBeaconInfo, &length, sizeof(length));
                memcpy(wclBeaconInfo + 4, beacon->frame, length);
                ++wclAssociatedBeacons;
                ++wclLqmBeacons;
                // WCL has no legacy LQMData instance at createLQMData().
                // Send the snapshot consumed by WCL instead of legacy 227/229.
                if (portAuthorized && (wclLqmBeacons % 8 == 0)) {
#if __IO80211_TARGET == __MAC_15_2
                    apple80211_rssi_data rssi = {};
                    if (getRSSI(&rssi) == kIOReturnSuccess) {
                        AirportWCL::SequoiaLqmUpdate update = {};
                        update.rssiValid = 1;
                        update.rssi = rssi.aggregate_rssi;
                        instance->postMessage(this, AirportWCL::LqmUpdate,
                            &update, sizeof(update), true);
                    }
#else
                    AirportWCL::LqmBeaconUpdate update;
                    if (AirportWCL::makeBeaconUpdate(
                            wclLqmBeacons - wclLqmReportedBeacons,
                            rssiForNode(ic->ic_bss),	/* dBm, smoothed */
                            fHalService->getDriverInfo()->getBSSNoise(), update)) {
                        instance->postMessage(this, AirportWCL::LqmUpdate,
                            &update, sizeof(update), true);
                        wclLqmReportedBeacons = wclLqmBeacons;
                    }
#endif
                }
            }
            if (scanPending || joinPending || ic->ic_state == IEEE80211_S_SCAN ||
                (portAuthorized && IEEE80211_ADDR_EQ(beacon->node->ni_bssid, joinBSSID)))
                publishWCLBeacon(beacon->node, beacon->ies, beacon->length, beacon->frame, beacon->frame_length);
            if (joinPending && assocNotified && !firstBeaconNotified &&
                IEEE80211_ADDR_EQ(beacon->node->ni_bssid, joinBSSID)) {
                AirportWCL::FirstBeaconEvent event = {};
                memcpy(event.peer.bssid, joinBSSID, 6);
                if (!wclBssValid)
                    break;
                firstBeaconNotified = true;
                AirportWCL::LinkStatusEvent link = {};
                memcpy(link.bssid, joinBSSID, sizeof(link.bssid));
                link.up = 1;
                link.reason = 255;
                wclLinkIndicated = true;
                instance->postMessage(this, AirportWCL::LinkStatus, &link, sizeof(link), true);
                instance->postMessage(this, AirportWCL::FirstBeacon, &event, sizeof(event), true);
                if (portAuthorized)
                    finishWCLJoin(0, 0, kIOReturnSuccess);
            }
            break;
        }
        case IEEE80211_EVT_SCAN_DONE:
            if (scanPending) {
                if (scanSkipCompletion) {
                    scanSkipCompletion = false;
                    scanRestartPending = true;
                    scanSource->setTimeoutMS(0);
                } else if (!scanRestartPending) {
                    scanCompleted = true;
                    scanSource->setTimeoutMS(0);
                }
            }
            break;
        case IEEE80211_EVT_STA_ASSOC_DONE:
            {
                // K1 (W2c): ic_assoc_status at the end of every association attempt (0 = success, otherwise the AP's status code, 0xffff = no answer)
                static unsigned int assocEnds;
                if (qwz_log_ratelimit(&assocEnds, 200, 20))
                    qwz_log("airport: wcl: event STA_ASSOC_DONE ic_assoc_status 0x%x joinPending %d assocNotified %d state %s chan %d\n", unsigned(ic->ic_assoc_status),
                            joinPending, assocNotified, ieee80211_state_name[ic->ic_state],
                            ic->ic_bss && ic->ic_bss->ni_chan ? ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan) : -1);
            }
            if (joinPending && !assocNotified) {
                AirportWCL::AssociationEvent event = {};
                event.status = ic->ic_assoc_status;
                memcpy(event.bssid, joinBSSID, 6);
                event.driverStatus = event.status ? kIOReturnError : kIOReturnSuccess;
                assocNotified = !event.status;
                instance->postMessage(this, AirportWCL::AssocComplete, &event, sizeof(event), true);
                if (event.status)
                    finishWCLJoin(event.status, 0, kIOReturnError);
            }
            break;
        case IEEE80211_EVT_STA_DEAUTH: {
            AirportWCL::DeauthEvent event = {};
            event.reason = ic->ic_deauth_reason;
            if (ic->ic_bss)
                memcpy(event.bssid, ic->ic_bss->ni_bssid, 6);
            {
                // M0 (C30): the reason is ic_deauth_reason, set before the event
                static unsigned int deauths;
                if (qwz_log_ratelimit(&deauths, 200, 20))
                    qwz_log("airport: wcl: event STA_DEAUTH reason %u bss %s\n", unsigned(event.reason), ether_sprintf(event.bssid));
            }
            instance->postMessage(this, AirportWCL::Deauth, &event, sizeof(event), true);
            finishWCLJoin(1, event.reason, kIOReturnOffline);
            break;
        }
        default: break;
    }
}

IOReturn AirportItlwmSkywalkInterface::getWCL_VALID_CHANNEL_COUNT(unsigned long *count)
{
    if (!count)
        return kIOReturnBadArgument;
    *count = 0;
    ieee80211com *ic = fHalService->get80211Controller();
    for (unsigned i = 1; i < IEEE80211_CHAN_MAX; ++i)
        if (isset(ic->ic_chan_active, i))
            ++*count;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_LINK_STATE_UPDATE(apple80211_wcl_update_link_state *data)
{
    if (!data)
        return kIOReturnBadArgument;
    {
        // M0 (C30): what airportd told the driver about the link (byte 6 is the up flag, as the 15.2 branch below reads it)
        static unsigned int updates;
        const uint8_t *b = reinterpret_cast<const uint8_t *>(data);
        if (qwz_log_ratelimit(&updates, 200, 20))
            qwz_log("airport: wcl: LINK_STATE_UPDATE %s (bytes[6..8] %02x %02x %02x)\n", b[6] ? "up" : "down", b[6], b[7], b[8]);
    }
#if __IO80211_TARGET == __MAC_15_2
    const auto bytes = reinterpret_cast<const uint8_t *>(data);
    if (bytes[6]) {
        if (bytes[8])
            setCurrentApAddress(reinterpret_cast<ether_addr *>(data));
        if (bytes[7])
            setLinkState(kIO80211NetworkLinkUp, 0, false, 0);
    } else {
        uint32_t debounce = AirportWCL::read32(bytes + 12);
        setCurrentApAddress(nullptr);
        setLinkState(kIO80211NetworkLinkDown, 0, debounce != 0, debounce);
    }
    return kIOReturnSuccess;
#else
    IOReturn status = IO80211InfraInterface::setWCL_LINK_STATE_UPDATE(data);
    return status;
#endif
}

IOReturn AirportItlwmSkywalkInterface::getWCL_BSS_INFO(apple80211_beacon_msg *data)
{
    if (!data)
        return kIOReturnBadArgument;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(5), data);
}

IOReturn AirportItlwmSkywalkInterface::getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info *data)
{
    if (!data)
        return kIOReturnBadArgument;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(6), data);
}

IOReturn AirportItlwmSkywalkInterface::copyWCLExtendedBss(void *data)
{
    ieee80211com *ic = fHalService->get80211Controller();
    if (!data || !ic->ic_bss || ic->ic_state != IEEE80211_S_RUN)
        return kIOReturnNotReady;
    static_assert(sizeof(apple80211_rate_set_data) == 0xbc, "Extended BSS rates size");
    static_assert(sizeof(apple80211_mcs_index_set_data) == 0x10, "Extended BSS MCS size");
    auto bytes = static_cast<uint8_t *>(data);
#if __IO80211_TARGET == __MAC_15_2
    bzero(bytes, 0x1e0);
#else
    bzero(bytes, 0x214);
#endif
    IOReturn status = getRATE_SET(reinterpret_cast<apple80211_rate_set_data *>(bytes));
    if (status)
        return status;
    status = getMCS_INDEX_SET(reinterpret_cast<apple80211_mcs_index_set_data *>(bytes + 0xbc));
    if (status)
        return status;
    apple80211_vht_mcs_index_set_data vht = {};
    vht.version = APPLE80211_VERSION;
    vht.mcs_map = 0xffff;
    if (ic->ic_bss->ni_flags & IEEE80211_NODE_VHT)
        vht.mcs_map = ic->ic_bss->ni_vht_mcsinfo.tx_mcs_map;
    memcpy(bytes + 0xcc, &vht, sizeof(vht));
    // Native updateMCSSet uses version + a 16-bit HE MCS/NSS map (8 bytes).
    uint32_t heVersion = APPLE80211_VERSION;
    uint16_t heMap = 0xffff;
    if (ic->ic_bss->ni_flags & IEEE80211_NODE_HE) {
        bool wide = ic->ic_bss->ni_chw == IEEE80211_CHAN_WIDTH_160;
        heMap = htole16(ieee80211_he_mcs_intersection(
            le16toh(wide ? ic->ic_he_mcs_nss_supp.rx_mcs_160 : ic->ic_he_mcs_nss_supp.rx_mcs_80),
            le16toh(wide ? ic->ic_bss->ni_he_mcs_nss_supp.tx_mcs_160 : ic->ic_bss->ni_he_mcs_nss_supp.tx_mcs_80)));
    }
    memcpy(bytes + 0xd4, &heVersion, sizeof(heVersion));
    memcpy(bytes + 0xd8, &heMap, sizeof(heMap));
    if (ic->ic_flags & IEEE80211_F_RSNON)
#if __IO80211_TARGET == __MAC_15_2
        ieee80211_add_rsn(bytes + 0xdd, ic, ic->ic_bss);
#else
        ieee80211_add_rsn(bytes + 0x113, ic, ic->ic_bss);
#endif
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_UPDATE_FAST_LANE(apple80211_fastlane *data)
{
    if (!data)
        return kIOReturnBadArgument;
    const auto bytes = reinterpret_cast<const uint8_t *>(data);
    return (bytes[0] || bytes[1]) ? kIOReturnUnsupported : kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::getWCL_LOW_LATENCY_INFO(apple80211_low_latency_info *data)
{
    if (!data)
        return kIOReturnBadArgument;
    // No low-latency or P2P session is implemented by this adapter.
    bzero(data, 4);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::getWCL_CHANNELS_INFO(apple80211ChannelInfo *data)
{
    if (!data)
        return kIOReturnBadArgument;
    auto info = reinterpret_cast<AirportWCL::ChannelInfo *>(data);
    bzero(info, sizeof(*info));
    ieee80211com *ic = fHalService->get80211Controller();
    for (unsigned i = 1; i < IEEE80211_CHAN_MAX && info->count < 400; ++i) {
        if (!isset(ic->ic_chan_active, i))
            continue;
        const ieee80211_channel *channel = &ic->ic_channels[i];
        auto &entry = info->channels[info->count++];
        entry.channel = i;
        entry.channelSpec = uint16_t(i | (IEEE80211_IS_CHAN_5GHZ(channel) ? 0xd000 : 0x1000));
        if (channel->ic_flags & IEEE80211_CHAN_DFS)
            entry.flags |= 2;
        if (channel->ic_flags & IEEE80211_CHAN_PASSIVE)
            entry.flags |= 4;
    }
    apple80211_country_code_data country = {};
    if (instance->getCOUNTRY_CODE(static_cast<OSObject *>(this), &country) == kIOReturnSuccess)
        memcpy(info->country, country.cc, 3);
    return info->count ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn AirportItlwmSkywalkInterface::getCHANNELS_INFO(apple80211_channels_info *data)
{
    if (!data)
        return kIOReturnBadArgument;
    if (!_fCommandGate)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(13), data);
}

IOReturn AirportItlwmSkywalkInterface::copyWCLChannels(void *data)
{
    auto info = static_cast<AirportWCL::NativeChannelInfo *>(data);
    bzero(info, sizeof(*info));
    ieee80211com *ic = fHalService->get80211Controller();
    for (unsigned i = 1; i <= IEEE80211_CHAN_MAX; ++i) {
        const ieee80211_channel *channel = &ic->ic_channels[i];
        if (!isset(ic->ic_chan_active, i) || !channel->ic_freq)
            continue;
        AirportWCL::appendChannel(*info, i, IEEE80211_IS_CHAN_5GHZ(channel),
            channel->ic_flags & IEEE80211_CHAN_DFS,
            channel->ic_flags & IEEE80211_CHAN_PASSIVE,
            IEEE80211_IS_CHAN_HT40(channel) || IEEE80211_IS_CHAN_VHT40(channel),
            IEEE80211_IS_CHAN_VHT80(channel));
    }
    apple80211_country_code_data country = {};
    if (instance->getCOUNTRY_CODE(static_cast<OSObject *>(this), &country) == kIOReturnSuccess)
        memcpy(info->country, country.cc, sizeof(country.cc));
    return info->count ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn AirportItlwmSkywalkInterface::getWCL_TRAFFIC_COUNTERS(apple80211_wcl_traffic_counters *data)
{
    if (!data)
        return kIOReturnBadArgument;
    if (!_fCommandGate)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(14), data);
}

IOReturn AirportItlwmSkywalkInterface::copyWCLTrafficCounters(void *data)
{
    ieee80211com *ic = fHalService->get80211Controller();
    uint64_t timestamp;
    absolutetime_to_nanoseconds(mach_absolute_time(), &timestamp);
    const auto counters = AirportWCL::makeTrafficCounters(
        ic->ic_wcl_tx_packets, ic->ic_wcl_rx_packets, timestamp);
    memcpy(data, &counters, sizeof(counters));
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_SET_SCAN_HOME_AWAY_TIME(scanHomeAndAwayTime *data)
{
    if (!data)
        return kIOReturnBadArgument;
    if (!_fCommandGate)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(15), data);
}
#endif

#ifdef AIRPORT_WCL
IOReturn AirportItlwmSkywalkInterface::getBEACON_INFO(apple80211_beacon_info_t *data)
{
    if (!_fCommandGate || !data)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(7), data);
}
#endif
