#ifndef SPEECH_IIR_CALIBRATION_H
#define SPEECH_IIR_CALIBRATION_H

#include <stddef.h>
#include "speech_eq.h"

// i.e. only support 4 channels
#define SPEECH_IIR_CALIB_MAX_NUM (3)

typedef struct
{
    int bypass;
    int mic_num;
    EqConfig calib[SPEECH_IIR_CALIB_MAX_NUM];
} SpeechIirCalibConfig;

typedef struct SpeechIirCalibState_ SpeechIirCalibState;

SpeechIirCalibState *speech_iir_calib_init(int32_t sample_rate, int32_t frame_size, const SpeechIirCalibConfig *config);

void speech_iir_calib_destroy(SpeechIirCalibState *st);

void speech_iir_calib_process(SpeechIirCalibState *st, int16_t *buf, int32_t frame_size);

/* Return required MIPS for this instance (may be 0 if not available).
 * Heuristic estimate:
 *  - sample rate: 8000 if MSBC_8K_SAMPLE_RATE defined, otherwise 16000
 *  - ops per biquad stage per sample: ~8 (mult/add/op overhead)
 *  - assume up to SPEECH_IIR_CALIB_MAX_NUM stages active (conservative)
 * If `st` is NULL returns 0.0f.
 */
static inline float speech_iir_calib_get_required_mips(SpeechIirCalibState *st)
{
    if (st == NULL) return 0.0f;
#if defined(MSBC_8K_SAMPLE_RATE)
    const int fs = 8000;
#else
    const int fs = 16000;
#endif
    const int ops_per_stage = 8; /* approx multiplies/adds per sample */
    const int stages = SPEECH_IIR_CALIB_MAX_NUM; /* conservative upper bound */
    /* MIPS = fs(samples/s) * ops_per_sample / 1e6 */
    float mips = (float)fs * (float)ops_per_stage * (float)stages / 1e6f;
    return mips;
}

#endif