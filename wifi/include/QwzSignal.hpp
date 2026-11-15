// Firmware channel measurements, with explicit frequency/epoch/freshness.
// GPL-2.0-or-later. Protocol fields: ath12k WMI_CHAN_INFO_EVENT.
#ifndef QWZ_SIGNAL_MEASUREMENTS_HPP
#define QWZ_SIGNAL_MEASUREMENTS_HPP
#include <stdint.h>
namespace QwzSignal {
constexpr uint64_t maxAgeNS = 30ULL * 1000000000ULL;
struct Reading {
    uint32_t frequency;
    uint32_t epoch;
    uint64_t noiseAt;
    uint64_t busyAt;
    int16_t noise;
    uint8_t busy;
    bool noiseKnown;
    bool busyKnown;
};
inline unsigned record(Reading &r, uint32_t frequency, uint32_t epoch,
                       uint64_t now, uint32_t rawNoise, uint32_t cycles, uint32_t busy) {
    if (r.frequency != frequency || r.epoch != epoch) {
        r = {};
        r.frequency = frequency;
        r.epoch = epoch;
    }
    unsigned accepted = 0;
    // Linux's survey interface consumes this WMI field as s8. Accept both
    // canonical byte and sign-extended forms; reject arbitrary upper bits.
    const int32_t noise = rawNoise <= 0xff ? int32_t(int8_t(rawNoise)) : int32_t(rawNoise);
    // Zero, -1, -128 and positive/out-of-range sentinels are not measurements.
    if (noise >= -127 && noise < -1) {
        r.noise = int16_t(noise);
        r.noiseAt = now;
        r.noiseKnown = true;
        accepted |= 1;
    }
    if (cycles && busy <= cycles) {
        r.busy = uint8_t(uint64_t(busy) * 100 / cycles);
        r.busyAt = now;
        r.busyKnown = true;
        accepted |= 2;
    }
    return accepted;
}
inline bool fresh(const Reading &r, uint32_t frequency, uint32_t epoch,
                  uint64_t now, uint64_t timestamp) {
    return r.frequency == frequency && r.epoch == epoch && now >= timestamp &&
           now - timestamp <= maxAgeNS;
}
inline int noise(const Reading &r, uint32_t frequency, uint32_t epoch, uint64_t now) {
    return r.noiseKnown && fresh(r, frequency, epoch, now, r.noiseAt) ? r.noise : 0;
}
inline int busy(const Reading &r, uint32_t frequency, uint32_t epoch, uint64_t now) {
    return r.busyKnown && fresh(r, frequency, epoch, now, r.busyAt) ? r.busy : -1;
}
}
#endif
