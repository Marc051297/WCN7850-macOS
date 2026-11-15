//
//  QwzAWDLRadio.cpp
//  AirportWCN7850
//
//  The radio side of the native AWDL port, slice F2-1 (docs/forbiddenitlwm-port-map-2026-11-13.md, R1-R7). The radio is
//  the K5/K6 cell beside the station (qwz/qwz_mon.c, debug.qwz.moncoex=1, debug.qwz.mon=<the station's channel>); this
//  file only connects it to the Apple half (QwzAWDLController.cpp):
//    receive:  every frame the capture path sees (qwz_mon_set_native_rx) is classified here, in the gate: AWDL action
//              frames go to the peer manager through receiveAWDLActionFrame (channel and RSSI attached), AWDL data frames
//              to awdl0 through observeAWDLDataFrame;
//    transmit: a timer on the controller's work loop (the gate) sends our sync frame (buildAWDLSyncProbe: Apple's MI or
//              a PSF) every 45 ms idle / 100 ms while data moves, pumps awdl0's packets (buildAWDLTransmit) through the
//              cell while it has room, and completes them (serviceAWDLTransmit). Every 2 ms while data moves, else 20 ms.
//  Nothing here starts or stops the cell; with no cell the timer idles and frames are not sent. What Intel's listener
//  context does on its own (promiscuous RX, a station per peer, aggregation) is the cell's business (K6 WBM tap, TDLS
//  peers) and later slices.
//
//  Copyright (c) 2026. GPL-2.0-or-later (see LICENSE).
//
#include "AirportItlwmV2.hpp"
#if defined(ITL_AWDL_NATIVE)
#include "QwzAWDLInterface.hpp"
#include <AWDLFrameParser.hpp>
#include <AWDLProbePolicy.hpp>
#include "qwz_mon.h"
#include <AWDLVisitPlan.hpp>
#include "qwz_log.h"
#include <sys/mbuf.h>

extern IO80211WorkQueue *_fWorkloop;

/* The channel our sync frames name: the cell's while it runs, else 149 (the frames are not sent then anyway). */
unsigned itlAWDLFixedChannel()
{
	const int cell = qwz_mon_status();
	return cell > 0 ? unsigned(cell) : 149;
}

/* 128 would claim 80 MHz on 149; the cell is HT20, so 0. */
unsigned itlAWDLChannelClass()
{
	return 0;
}

namespace {
constexpr uint64_t kDataActiveUS = 1000000;		/* data moved in the last second: fast pacing */
constexpr uint64_t kSyncDataIntervalUS = 100000;	/* sync frame every 100 ms while data moves */
constexpr unsigned kPumpBudget = 16;			/* awdl0 packets per tick at most */
constexpr unsigned kScratchBytes = 2400 + 4;		/* QWZ_MONTX_MAXFRAME plus the four bytes the hand-off expects */

uint64_t nowUS()
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}
}

void AirportItlwm::awdlNativeRadioStart()
{
	int value = 0;

	if (awdlNativeTimer != nullptr)
		return;
	awdlNativeRssiFallback = -60;
	if (PE_parse_boot_argn("itlawdlrssi", &value, sizeof(value)) && value <= -20 && value >= -95)
		awdlNativeRssiFallback = value;
	memset(&awdlNativeStats, 0, sizeof(awdlNativeStats));
	awdlNativeLastSyncUS = awdlNativeLastDataUS = awdlNativeLastPublishUS = 0;
	awdlNativeLastRxBytes = awdlNativeLastAckedBytes = 0;
	awdlNativePeerTiming.reset();
	awdlNativeTimingCell = 0;
	int timingOff = 0;
	awdlNativePeerTimingEnabled = !PE_parse_boot_argn("-qwzawdlpeerpauseoff", &timingOff, sizeof(timingOff));
	if (awdlNativeScratch == nullptr)
		awdlNativeScratch = static_cast<uint8_t *>(IOMalloc(kScratchBytes));
	awdlNativeTimer = IOTimerEventSource::timerEventSource(this, awdlNativeTimerFired);
	if (awdlNativeTimer == nullptr || getWorkLoop()->addEventSource(awdlNativeTimer) != kIOReturnSuccess) {
		OSSafeReleaseNULL(awdlNativeTimer);
		qwz_log("airport: awdl0: native radio: no timer; nothing will be sent\n");
		qwz_log_publish();
		return;
	}
	qwz_mon_set_native_rx(awdlNativeRadioRx, this);
	qwz_mon_set_native_plan(awdlNativeRadioPlan, this);	/* AL (aligned visits) */
	if (auto *channel = OSDynamicCast(OSNumber, getProperty("QwzAWDLPreferredChannel"))) {
		const int configured = int(channel->unsigned32BitValue());
		const int error = qwz_mon_native_set_preferred_channel(configured);
		setProperty("QwzAWDLConfiguredChannelError", uint64_t(error), 32);
	}
	int manual = 0;
	qwz_mon_native_auto_enable(!PE_parse_boot_argn("-qwzawdlmanual", &manual, sizeof(manual)));
	awdlNativeTimer->setTimeoutMS(50);
	setProperty("QwzAWDLRadio", "started");
	setProperty("QwzAWDLRadioBuild", "f2t-20261115");
	setProperty("QwzAWDLPeerTimingEnabled", awdlNativePeerTimingEnabled);
	qwz_log("airport: awdl0: native radio started (cell %d, rssi fallback %d)\n", qwz_mon_status(), awdlNativeRssiFallback);
	qwz_log_publish();
}

void AirportItlwm::awdlNativeRadioStop()
{
	awdlNativePeerTiming.clearPeers();
	qwz_mon_set_native_plan(nullptr, nullptr);	/* AL */
	qwz_mon_set_native_rx(nullptr, nullptr);
	if (fHalService != nullptr)
		static_cast<ItlQwz *>(fHalService)->setAwdlAddress(nullptr);
	if (awdlNativeTimer != nullptr) {
		awdlNativeTimer->cancelTimeout();
		getWorkLoop()->removeEventSource(awdlNativeTimer);
		OSSafeReleaseNULL(awdlNativeTimer);
	}
	if (awdlNativeScratch != nullptr) {
		IOFree(awdlNativeScratch, kScratchBytes);
		awdlNativeScratch = nullptr;
	}
	setProperty("QwzAWDLRadio", "stopped");
}

/* The capture path's second consumer (qwz_mon.h): the gate is held by every emitter, which the hand-offs require. */
void AirportItlwm::awdlNativeRadioRx(void *arg, const uint8_t *frame, size_t len, uint16_t flags, uint8_t chan, int8_t rssi)
{
	static_cast<AirportItlwm *>(arg)->awdlNativeRadioInput(frame, len, flags, chan, rssi);
}

void AirportItlwm::awdlNativeRadioInput(const uint8_t *frame, size_t len, uint16_t flags, uint8_t chan, int8_t rssi)
{
	auto &st = awdlNativeStats;

	if (frame == nullptr || len < 24 || !awdlInterfaceTest || fAWDLInterface == nullptr || !awdlManagementInputEnabled)
		return;
	if (_fWorkloop == nullptr || !_fWorkloop->inGate()) {
		++st.rxNotInGate;
		return;
	}
	const bool fcs = (flags & QWZ_MONCAP_F_FCS) != 0;
	const size_t body = fcs && len >= 28 ? len - 4 : len;
	const uint8_t type = (frame[0] >> 2) & 3;
	if (type == 0) {
		if (frame[0] != 0xd0)	/* only action frames go to the peer manager */
			return;
		++st.rxAction;
		size_t frameLength = fcs ? body : ItlAWDL::trimZeroPadding(frame, body);
		if (frameLength != body)
			++st.rxActionTrimmed;
		if (awdlNativePeerTimingEnabled)
			awdlNativePeerTiming.noteAction(frame, frameLength, chan, nowUS());
		if (rssi == 0) {
			rssi = int8_t(awdlNativeRssiFallback);
			++st.rssiFallbacks;
		}
		if (frameLength + 4 > 2048) {
			++st.rxActionTooLong;
			return;
		}
		mbuf_t m = nullptr;
		if (mbuf_getpacket(MBUF_DONTWAIT, &m) || m == nullptr) {
			++st.rxNoMbuf;
			return;
		}
		if (mbuf_maxlen(m) < frameLength + 4) {
			mbuf_freem(m);
			++st.rxActionTooLong;
			return;
		}
		memcpy(mbuf_data(m), frame, frameLength);
		memset(static_cast<uint8_t *>(mbuf_data(m)) + frameLength, 0, 4);
		mbuf_setlen(m, frameLength + 4);
		mbuf_pkthdr_setlen(m, frameLength + 4);
		const IOReturn r = receiveAWDLActionFrame(m, frameLength, chan, rssi);
		mbuf_freem(m);
		if (r == kIOReturnSuccess)
			++st.rxActionHanded;
		else {
			++st.rxActionRefused;
			st.rxActionLastError = uint32_t(r);
		}
		return;
	}
	if (type == 2) {
		if (awdlNativePeerTimingEnabled)
			awdlNativePeerTiming.noteData(frame, len, fcs, chan, nowUS());
		++st.rxData;
		const uint8_t *buf = frame;
		size_t n = len;
		if (!fcs) {	/* the hand-off parses with the FCS included: give it four bytes */
			if (awdlNativeScratch == nullptr || len + 4 > kScratchBytes) {
				++st.rxDataTooLong;
				return;
			}
			memcpy(awdlNativeScratch, frame, len);
			memset(awdlNativeScratch + len, 0, 4);
			buf = awdlNativeScratch;
			n = len + 4;
		}
		observeAWDLDataFrame(buf, n);
		++st.rxDataHanded;
		st.rxDataBytes += body;
		awdlNativeLastDataUS = nowUS();
	}
}

/*
 * AL (aligned visits): the capture cell's timer asks, in the gate, when the receiver of the frames waiting on awdl0 is next
 * on the cell's channel (qwz_mon.h). The answer comes from the same peer schedules the per-destination holds use, for the
 * first few queued frames. Only when the holds are on and a schedule is known; otherwise 0, and the visit is the fixed one.
 */
int AirportItlwm::awdlNativeRadioPlan(void *arg, uint32_t *delayUS, uint32_t *runUS)
{
	return arg != nullptr ? static_cast<AirportItlwm *>(arg)->awdlNativePlanVisit(delayUS, runUS) : 0;
}

int AirportItlwm::awdlNativePlanVisit(uint32_t *delayUS, uint32_t *runUS)
{
	if (delayUS == nullptr || runUS == nullptr || awdlNativeTimer == nullptr || !awdlInterfaceTest || fAWDLInterface == nullptr ||
	    !awdlManagementInputEnabled || !awdlNativePeerTimingEnabled || !qwz_nativepeerpause || _fWorkloop == nullptr || !_fWorkloop->inGate())
		return 0;
	const int cell = qwz_mon_status();
	if (cell <= 0)
		return 0;
	auto *interface = static_cast<QwzAWDLInterface *>(fAWDLInterface);
	uint8_t destinations[ItlAWDL::kVisitPlanLook][6];
	unsigned count = 0;
	interface->retain();	/* as buildAWDLTransmit holds it across its queue access */
	while (count < ItlAWDL::kVisitPlanLook && interface->copyQueuedDestination(count, destinations[count]))
		++count;
	interface->release();
	uint64_t delay = 0, run = 0;
	if (count == 0 || !ItlAWDL::planVisit(awdlNativePeerTiming, destinations, count, uint8_t(cell), nowUS(), &delay, &run))
		return 0;
	*delayUS = delay > 0xffffffffULL ? 0xffffffffU : uint32_t(delay);
	*runUS = run > 0xffffffffULL ? 0xffffffffU : uint32_t(run);
	return 1;
}

void AirportItlwm::awdlNativeTimerFired(OSObject *owner, IOTimerEventSource *)
{
	auto *self = OSDynamicCast(AirportItlwm, owner);
	if (self == nullptr || self->awdlNativeTimer == nullptr)
		return;
	auto &st = self->awdlNativeStats;
	++st.timerTicks;
	const bool ready = self->awdlInterfaceTest && self->fAWDLInterface != nullptr && self->awdlManagementInputEnabled;
	if (ready)
		qwz_mon_native_auto_tick();
	const int cell = ready ? qwz_mon_status() : 0;
	if (cell != self->awdlNativeTimingCell) {
		self->awdlNativePeerTiming.clearPeers();
		self->awdlNativeTimingCell = cell;
	}
	const uint64_t now = nowUS();
	bool pending = false;
	if (cell > 0)
		st.cellTimerTicks++;
	if (cell > 0 && qwz_mon_native_tx_ready()) {
		const bool dataActive = self->awdlNativeLastDataUS != 0 && now - self->awdlNativeLastDataUS < kDataActiveUS;
		const uint64_t sinceSync = now - self->awdlNativeLastSyncUS;
		if (qwz_mon_tx_room() > 0 && (self->awdlNativeLastSyncUS == 0 || sinceSync >= (dataActive ? kSyncDataIntervalUS : ItlAWDL::kSyncIdleIntervalUS))) {
			mbuf_t m = nullptr;
			const IOReturn built = self->buildAWDLSyncProbe(&m, now);
			st.syncLastBuild = uint32_t(built);
			if (built == kIOReturnSuccess && m != nullptr) {
				const int err = qwz_mon_tx_raw(static_cast<const uint8_t *>(mbuf_data(m)), mbuf_len(m));
				st.syncLastTxError = uint32_t(err);
				if (err == 0) {
					++st.syncSent;
					self->awdlNativeLastSyncUS = now;
				} else {
					++st.syncTxErrors;
				}
			} else {
				++st.syncBuildFailures;
			}
			if (m != nullptr)
				mbuf_freem(m);
		}
		unsigned budget = kPumpBudget;
		// A send spends a WMI credit. Recheck before dequeuing each packet;
		// the slot count alone can outlive the available transport credit.
		while (budget > 0 && qwz_mon_native_tx_ready() && qwz_mon_tx_room() > 0) {
			mbuf_t m = nullptr;
			if (self->buildAWDLTransmit(&m, nullptr, false, nowUS()) != kIOReturnSuccess || m == nullptr)
				break;
			const int err = qwz_mon_tx_raw(static_cast<const uint8_t *>(mbuf_data(m)), mbuf_len(m));
			st.dataLastTxError = uint32_t(err);
			if (err == 0)
				st.txSubmittedBytes += mbuf_len(m);
			mbuf_freem(m);
			--budget;
			if (err == 0) {
				++st.dataSent;
				self->awdlNativeLastDataUS = now;
			} else {
				++st.dataTxErrors;
				if (err == ENOSPC || err == EAGAIN)
					break;
			}
		}
	}
	// Return completed Skywalk packets even while waiting for a radio window.
	if (ready) {
		pending = self->serviceAWDLTransmit();
		st.queued = self->queuedAWDLTransmit();
	}
	if (now - self->awdlNativeLastPublishUS >= 1000000) {
		const uint64_t intervalUS = self->awdlNativeLastPublishUS ? now - self->awdlNativeLastPublishUS : 0;
		qwz_mon_native_tx_stats tx = {};
		qwz_mon_native_get_tx_stats(&tx);
		const uint64_t txRate = intervalUS && tx.acked_bytes >= self->awdlNativeLastAckedBytes ?
		    (tx.acked_bytes - self->awdlNativeLastAckedBytes) * 1000000 / intervalUS : 0;
		const uint64_t rxRate = intervalUS ?
		    (st.rxDataBytes - self->awdlNativeLastRxBytes) * 1000000 / intervalUS : 0;
		self->awdlNativeLastAckedBytes = tx.acked_bytes;
		self->awdlNativeLastRxBytes = st.rxDataBytes;
		self->awdlNativeLastPublishUS = now;
		OSDictionary *d = OSDictionary::withCapacity(24);
		if (d != nullptr) {
			auto put = [d](const char *k, uint64_t v) {
				if (OSNumber *n = OSNumber::withNumber(v, 64)) { d->setObject(k, n); n->release(); }
			};
			put("cell", uint64_t(cell)); put("ticks", st.timerTicks); put("cellTicks", st.cellTimerTicks);
			put("preferredCell", qwz_mon_native_preferred_channel());
			put("txReady", qwz_mon_native_tx_ready());
			put("rxAction", st.rxAction); put("rxActionHanded", st.rxActionHanded); put("rxActionRefused", st.rxActionRefused);
			put("rxActionLastError", st.rxActionLastError); put("rxActionTrimmed", st.rxActionTrimmed); put("rxActionTooLong", st.rxActionTooLong);
			put("rxData", st.rxData); put("rxDataHanded", st.rxDataHanded); put("rxDataTooLong", st.rxDataTooLong);
			put("rxNotInGate", st.rxNotInGate); put("rxNoMbuf", st.rxNoMbuf); put("rssiFallbacks", st.rssiFallbacks);
			put("syncSent", st.syncSent); put("syncBuildFailures", st.syncBuildFailures); put("syncLastBuild", st.syncLastBuild);
			put("syncTxErrors", st.syncTxErrors); put("syncLastTxError", st.syncLastTxError);
			put("dataSent", st.dataSent); put("dataTxErrors", st.dataTxErrors); put("dataLastTxError", st.dataLastTxError);
			put("queued", st.queued);
			put("txSubmittedBytes", st.txSubmittedBytes); put("rxDataBytes", st.rxDataBytes);
			put("txDataAckedBytes", tx.acked_bytes); put("txDataAcked", tx.acked); put("txDataFailed", tx.failed);
			put("txDataNoAck", tx.noack); put("txInflight", tx.inflight);
			put("txAckedBytesPerSecond", txRate); put("rxBytesPerSecond", rxRate);
			put("timedPeers", self->awdlNativePeerTiming.known(now));
			put("peerHoldChecks", self->awdlNativePeerTiming.holdChecks);
			put("peerForcedReleases", self->awdlNativePeerTiming.forcedReleases);
			put("peerTimingResets", self->awdlNativePeerTiming.resets);
			put("peerBypasses", self->awdlNativePeerTiming.bypasses);
			put("peerTimingEnabled", self->awdlNativePeerTimingEnabled && qwz_nativepeerpause);
			put("nativeFormat", qwz_nativefmt);
			self->setProperty("QwzAWDLRadioStats", d);
			self->setProperty("QwzAWDLPeerTimingEnabled", self->awdlNativePeerTimingEnabled && qwz_nativepeerpause);
			self->setProperty("QwzAWDLRadioAuto", qwz_mon_native_auto_state());
			d->release();
		}
	}
	const bool dataActive = self->awdlNativeLastDataUS != 0 && now - self->awdlNativeLastDataUS < kDataActiveUS;
	self->awdlNativeTimer->setTimeoutMS(cell <= 0 ? (pending ? 20 : 200) : (dataActive || pending) ? 1 : 20);
}
#else
unsigned itlAWDLFixedChannel() { return 149; }
unsigned itlAWDLChannelClass() { return 0; }
#endif /* ITL_AWDL_NATIVE */
