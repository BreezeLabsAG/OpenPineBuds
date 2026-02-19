#ifndef SPEECH_FIR_CALIBRATION_H
#define SPEECH_FIR_CALIBRATION_H

#include <stddef.h>
#include "fftfilt.h"

// i.e. only support 4 channels
#define SPEECH_FIR_CALIB_MAX_NUM (3)

typedef struct
{
    float *filter;
    int filter_length;
} CalibChannelConfig;

typedef struct
{
    int bypass;
    int mic_num;
    float delay;    // fraction delay for main mic
    CalibChannelConfig calib[SPEECH_FIR_CALIB_MAX_NUM];
} SpeechFirCalibConfig;

typedef struct SpeechFirCalibState_ SpeechFirCalibState;

#define CONSTRUCT_FUNC_NAME_A(p, c, m)          p ## _ ## c ## _ ## m
#define CONSTRUCT_FUNC_NAME(p, c, m)            CONSTRUCT_FUNC_NAME_A(p, c, m)

#ifndef FIR_CALIB_IMPL
#if defined(VQE_SIMULATE)
#define FIR_CALIB_IMPL fir
#else
#define FIR_CALIB_IMPL hwfir
#endif
#endif

#define speech_fir_calib_init CONSTRUCT_FUNC_NAME(speech, FIR_CALIB_IMPL, calib_init)
#define speech_fir_calib_destroy CONSTRUCT_FUNC_NAME(speech, FIR_CALIB_IMPL, calib_destroy)
#define speech_fir_calib_process CONSTRUCT_FUNC_NAME(speech, FIR_CALIB_IMPL, calib_process)

#ifdef __cplusplus
extern "C" {
#endif

SpeechFirCalibState *speech_fir_calib_init(int32_t sample_rate, int32_t frame_size, const SpeechFirCalibConfig *config);

void speech_fir_calib_destroy(SpeechFirCalibState *st);

void speech_fir_calib_process(SpeechFirCalibState *st, int16_t *buf, int32_t frame_size);

/* Return required MIPS for this instance (may be 0 if not available).
 * Heuristic estimate:
 *  - sample rate: 8000 if MSBC_8K_SAMPLE_RATE defined, otherwise 16000
 *  - ops per tap per sample: ~2 (multiply + add)
 *  - assume default filter length 256 if unknown (conservative)
 * If `st` is NULL returns 0.0f.
 */
static inline float speech_fir_calib_get_required_mips(SpeechFirCalibState *st)
{
    if (st == NULL) return 0.0f;
#if defined(MSBC_8K_SAMPLE_RATE)
    const int fs = 8000;
#else
    const int fs = 16000;
#endif
    const int ops_per_tap = 2; /* multiply + add */
    const int default_len = 256; /* typical FIR calib length seen in configs */
    float mips = (float)fs * (float)ops_per_tap * (float)default_len / 1e6f;
    return mips;
}

#ifdef __cplusplus
}
#endif

#endif