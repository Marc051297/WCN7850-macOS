#ifndef QWZ_SIGNAL_H
#define QWZ_SIGNAL_H
#include <stdint.h>
#include <stddef.h>
struct qwz_softc;
void qwz_signal_record(struct qwz_softc *, uint32_t frequency, uint32_t rawNoise,
                       uint32_t cycles, uint32_t busy);
int16_t qwz_signal_noise(struct qwz_softc *, uint32_t frequency);
int qwz_signal_busy(struct qwz_softc *, uint32_t frequency);
void qwz_signal_log(struct qwz_softc *);
#endif
