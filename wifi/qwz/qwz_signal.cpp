/* Firmware measurements only; no commands, radio transitions or guessed data.
 * Copyright (c) 2026. GPL-2.0-or-later. */
#include "qwz_compat.h"
#include "qwzreg.h"
#include "qwzvar.h"
#include "qwz_signal.h"

static int
qwz_signal_index(struct qwz_softc *sc, uint32_t frequency)
{
	if (!sc || !frequency)
		return -1;
	const int index = ieee80211_mhz2ieee(frequency, 0);
	if (index <= 0 || index >= (int)nitems(sc->sc_signal) ||
	    !sc->sc_ic.ic_channels[index].ic_flags ||
	    sc->sc_ic.ic_channels[index].ic_freq != frequency)
		return -1;
	return index;
}

void
qwz_signal_record(struct qwz_softc *sc, uint32_t frequency, uint32_t rawNoise,
    uint32_t cycles, uint32_t busy)
{
	if (!sc) return;
	++sc->sc_signal_events;
	sc->sc_signal_last_freq = frequency;
	sc->sc_signal_last_raw_noise = rawNoise;
	if (!sc->fw_initialized || QWZ_CRASHED(sc)) {
		++sc->sc_signal_rejected;
		return;
	}
	const int index = qwz_signal_index(sc, frequency);
	if (index < 0) {
		++sc->sc_signal_rejected;
		return;
	}
	const unsigned accepted = QwzSignal::record(sc->sc_signal[index], frequency,
	    sc->sc_fw_epoch, getnsecuptime(), rawNoise, cycles, busy);
	if (accepted & 1) ++sc->sc_signal_noise_samples;
	if (accepted & 2) ++sc->sc_signal_busy_samples;
	if (!accepted) ++sc->sc_signal_rejected;
}

int16_t
qwz_signal_noise(struct qwz_softc *sc, uint32_t frequency)
{
	const int index = qwz_signal_index(sc, frequency);
	return index < 0 || !sc->fw_initialized || QWZ_CRASHED(sc) ? 0 :
	    int16_t(QwzSignal::noise(sc->sc_signal[index], frequency, sc->sc_fw_epoch, getnsecuptime()));
}

int
qwz_signal_busy(struct qwz_softc *sc, uint32_t frequency)
{
	const int index = qwz_signal_index(sc, frequency);
	return index < 0 || !sc->fw_initialized || QWZ_CRASHED(sc) ? -1 :
	    QwzSignal::busy(sc->sc_signal[index], frequency, sc->sc_fw_epoch, getnsecuptime());
}

void
qwz_signal_log(struct qwz_softc *sc)
{
	if (!sc) return;
	struct ieee80211com *ic = &sc->sc_ic;
	const uint32_t frequency = ic->ic_state == IEEE80211_S_RUN && ic->ic_bss &&
	    ic->ic_bss->ni_chan && ic->ic_bss->ni_chan != IEEE80211_CHAN_ANYC ? ic->ic_bss->ni_chan->ic_freq : 0;
	qwz_log("qwz: signal: freq %u noise %d valid %d busy %d; events %u noise_samples %u busy_samples %u rejected %u last_freq %u raw_noise %d bss_rssi_samples %u\n",
	    frequency, qwz_signal_noise(sc, frequency), qwz_signal_noise(sc, frequency) < 0,
	    qwz_signal_busy(sc, frequency), sc->sc_signal_events, sc->sc_signal_noise_samples,
	    sc->sc_signal_busy_samples, sc->sc_signal_rejected, sc->sc_signal_last_freq,
	    int32_t(sc->sc_signal_last_raw_noise), sc->sc_bss_rssi_nsamples);
	qwz_log("qwz: signal: wmi_events %u last_flags %u last_error %u bss_survey88 %d pdev_survey127 %d db2dbm178 %d\n",
	    sc->sc_signal_wmi_events, sc->sc_signal_last_flags, sc->sc_signal_last_error,
	    isset(sc->wmi.svc_map, WMI_TLV_SERVICE_BSS_CHANNEL_INFO_64) != 0,
	    isset(sc->wmi.svc_map, WMI_TLV_SERVICE_PDEV_BSS_CHANNEL_INFO_64) != 0,
	    isset(sc->wmi.svc_map, WMI_TLV_SERVICE_HW_DB2DBM_CONVERSION_SUPPORT) != 0);
}
