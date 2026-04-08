# DSP Bypass for Microphone Path

## Goal

Bypass noise-removal filters on the mic path so breathing is clearly audible. The headset connects via HFP profile to a custom recording app that streams mic audio back for capture. The filter bypass will be toggleable via a gesture/button on the headset.

## Audio Pipeline Overview

### Hardware IIR (runs on codec hardware, before software chain)
- Configured via `adc_iir_cfg` in `services/bt_app/app_bt_stream.cpp` using `HW_CODEC_IIR_CFG_F` (raw biquad float coefficients: `{b0, b1, b2} / {a0, a1, a2}`)
- Created at SCO stream open: `hw_filter_codec_iir_create()` at `app_bt_stream.cpp:6358`
- Writes coefficients directly to hardware registers via `hw_codec_iir_set_coefs()`
- Enabled by build flag `HW_DC_FILTER_WITH_IIR`
- **FINDING: This HW IIR does NOT affect the SCO ADC capture path. Tested with 2kHz LPF coefficients — no effect on mic audio. Not usable for our purpose.**

### Software Speech TX Chain (`apps/audioplayers/bt_sco_chain.c`, `_speech_tx_process_pre`)
Processing order (only enabled filters listed):
1. **SPEECH_TX_DC_FILTER** — DC bias removal (essential, always keep)
2. **SPEECH_TX_COMPEXP** — compressor/expander (kills quiet sounds like breathing)
3. **SPEECH_TX_EQ** — software biquad EQ (supports LPF, HPF, peaking, shelving, etc.)

All other filters disabled in `config/open_source/target.mk`: AEC, NS, multi-mic denoise, noise gate, AGC, post gain.

---

## Prior Attempts (branch `ph_dsp_bypass` initial changes)

### Change 1: `services/bt_app/app_bt_stream.cpp`
- Force-defined `#define HW_DC_FILTER_WITH_IIR 1` (overriding target.mk's `0`)
- Replaced `adc_iir_cfg` biquad coefficients with: `b={1,0,0}, a={0,0,0}`

**Why it failed:** `a={0,0,0}` is mathematically invalid (denominator of all zeros = division by zero). A pass-through filter needs `a={1,0,0}`. The original high-pass at 20Hz with 4x gain was commented out.

### Change 2: `config/open_source/tgt_hardware.c`
- Changed `audio_eq_hw_adc_iir_adc_cfg` from `= audio_eq_sw_iir_cfg` to a custom `IIR_CFG_T` with `IIR_TYPE_LOW_PASS` at 1000Hz

**Why it failed:** This config uses the `IIR_CFG_T` struct format (high-level: type, gain, freq, Q) and feeds into `audio_eq_hw_adc_iir_cfg_list[]` which is for **DAC EQ**, NOT the ADC capture path. The HW ADC IIR filter in `app_bt_stream.cpp` uses `HW_CODEC_IIR_CFG_F` (raw biquad float coefficients) — a completely different struct that doesn't connect to `tgt_hardware.c`.

### Change 3: `config/open_source/target.mk` and `config/common.mk`
- Added debug `$(info ...)` prints
- `HW_DC_FILTER_WITH_IIR` remained `0` in target.mk (overridden by force-define in .cpp)

**Result:** No effect whatsoever on audio output.

---

## Milestone 1: Wire 2kHz Low-Pass Filter (Flash Test) — FAILED

### Approach: Hardware IIR on ADC
Attempted to use `HW_DC_FILTER_WITH_IIR` with correct 2kHz Butterworth biquad coefficients in `app_bt_stream.cpp`.

### Biquad Coefficients (2kHz Butterworth LPF @ 16kHz sample rate)
```
b = {0.09763f, 0.19526f, 0.09763f}
a = {1.00000f, -0.94280f, 0.33333f}
```

### Changes Made
1. `config/open_source/target.mk`: `HW_DC_FILTER_WITH_IIR ?= 1`, `SPEECH_TX_COMPEXP ?= 0`
2. `services/bt_app/app_bt_stream.cpp`: Replaced broken `adc_iir_cfg` with correct 2kHz LPF coefficients, removed force-define and commented code
3. `config/open_source/tgt_hardware.c`: Reverted to `= audio_eq_sw_iir_cfg`

### Test Result
- **No LPF effect** — full bandwidth (max 8kHz = Nyquist of 16kHz mSBC) with no damping
- **Unwanted sidetone loopback** — heard own voice through speakers. This was caused by `HW_DC_FILTER_WITH_IIR = 1`: the `hw_codec_iir_open(HW_CODEC_IIR_ADC)` call opens a hardware ADC → IIR → DAC sidetone path, NOT a filter on the SCO capture. The app does not produce loopback — this was entirely a headset-side hardware routing issue.
- **Conclusion: The HW codec IIR (`hw_filter_codec_iir_create`) creates a sidetone path, not a capture filter.** It must stay disabled (`HW_DC_FILTER_WITH_IIR = 0`) to avoid unwanted loopback.

---

## Milestone 1.1: Software EQ Approach (Current)

Since the HW IIR doesn't work for SCO mic capture, we use the **software speech TX EQ** (`SPEECH_TX_EQ`) which processes every mic frame in `_speech_tx_process_pre()`. This uses the built-in `IIR_BIQUARD_LPF` filter type with automatic coefficient design.

### Changes

#### 1. `config/open_source/target.mk`
- `HW_DC_FILTER_WITH_IIR ?= 1` → `HW_DC_FILTER_WITH_IIR ?= 0` (reverted — HW IIR doesn't work for SCO)
- `SPEECH_TX_COMPEXP ?= 1` → `SPEECH_TX_COMPEXP ?= 0` (disable compressor, kept from Milestone 1)
- `SPEECH_TX_EQ ?= 0` → `SPEECH_TX_EQ ?= 1` (enable software TX EQ)

#### 2. `apps/audioplayers/bt_sco_chain_cfg_default.c` (tx_eq section, ~line 584)
- Changed from `IIR_BIQUARD_HPF` at 60Hz to `IIR_BIQUARD_LPF` at 2000Hz with Q=0.707 (Butterworth)
- 4 cascaded stages for steep -48dB/octave rolloff
- The EQ framework calls `iirfilt_design()` which computes biquad coefficients automatically from `{f0, gain, q}` parameters

#### 3. `services/bt_app/app_bt_stream.cpp`
- Kept the corrected 2kHz LPF biquad coefficients in `adc_iir_cfg` (harmless, doesn't affect SCO)

#### 4. `config/open_source/tgt_hardware.c`
- Already reverted to `= audio_eq_sw_iir_cfg` (from Milestone 1)

### How to test
1. **IMPORTANT: Must do a clean build** (`make clean T=open_source && make ...`). Incremental builds don't recompile when only Makefile flags change.
2. Flash firmware to earbuds
3. Connect headset to custom recording app via HFP
4. Record mic audio (speak + breathe near mic)
5. Open recording in Audacity, check spectrum analysis:
   - **Steep rolloff above 2kHz** = filter is working
   - **Breathing visible/audible** = bypass concept validated
   - **Full bandwidth / sounds normal** = software EQ also not applied, deeper investigation needed

### Result
- **SUCCESS** — steep rolloff above 2kHz confirmed in Audacity spectrum analysis
- Initial "no effect" was due to flashing old binary without rebuilding (Makefile flag changes require clean build)

---

## Milestone 2: Runtime Toggle via Gesture

Quad-tap (paired mode) toggles between normal audio and 2kHz LPF bypass mode at runtime.

### Changes

#### 1. `config/open_source/target.mk`
- `SPEECH_TX_COMPEXP ?= 0` → `SPEECH_TX_COMPEXP ?= 1` (re-enabled — now bypassed at runtime instead of compile-time)
- `SPEECH_TX_EQ ?= 1` (kept enabled — starts in passthrough mode, LPF activated by toggle)

#### 2. `apps/audioplayers/bt_sco_chain.c`
- Added `dsp_bypass_enabled` state flag (starts at 0 = normal mode)
- Added two static `EqConfig` structs:
  - `dsp_bypass_eq_cfg`: 4-stage 2kHz Butterworth LPF (steep rolloff)
  - `dsp_normal_eq_cfg`: bypass=1, passthrough
- Added `speech_dsp_bypass_toggle()`: flips state, hot-swaps EQ config via `eq_set_config()`
- Added `speech_dsp_bypass_is_enabled()`: getter for state
- Guarded `compexp_process()` with `if (!dsp_bypass_enabled)` — compressor skipped in bypass mode

#### 3. `apps/audioplayers/bt_sco_chain.h`
- Declared `speech_dsp_bypass_toggle()` and `speech_dsp_bypass_is_enabled()`

#### 4. `apps/audioplayers/bt_sco_chain_cfg_default.c`
- Default `tx_eq` changed to passthrough (`.bypass = 1, .num = 0`) — EQ starts inactive, LPF only when toggled

#### 5. `services/app_ibrt/inc/app_ibrt_keyboard.h`
- Added `#define IBRT_ACTION_DSP_BYPASS_TOGGLE 0x10`

#### 6. `services/app_ibrt/src/app_ibrt_keyboard.cpp`
- Added `#include "bt_sco_chain.h"`
- Added case for `IBRT_ACTION_DSP_BYPASS_TOGGLE` calling `speech_dsp_bypass_toggle()`

#### 7. `apps/main/key_handler.cpp`
- Added `send_dsp_bypass_toggle()` helper
- `app_key_quad_tap()`: in paired mode, calls `send_dsp_bypass_toggle()` (single-pod mode still does vol down)
- Updated gesture documentation comment

### Gesture mapping
| Gesture | Single pod | Paired Right | Paired Left |
|---------|-----------|--------------|-------------|
| Single tap | Play/Pause | Play/Pause | Play/Pause |
| Double tap | Next track | Next track | Previous track |
| Triple tap | Volume Up | Volume Up | Volume Down |
| **Quad tap** | **DSP bypass toggle** | **DSP bypass toggle** | **DSP bypass toggle** |
| Long press | Previous track | ANC on/off | ANC on/off |

Note: Quad-tap was previously volume down in single-pod mode. Changed to DSP bypass toggle in all modes for consistency and testability (only one earbud needed for testing due to limited flash cycles).

### How to test
1. Clean build and flash (`make clean T=open_source && make -j$(nproc) T=open_source DEBUG=1`)
2. Connect to custom recording app via HFP, start recording
3. Quad-tap → should toggle between:
   - **Normal**: full bandwidth, compressor active
   - **Bypass**: 2kHz LPF, compressor off, breathing audible
4. Quad-tap again to toggle back
5. Compare recordings in Audacity

### Result
- **SUCCESS** — toggle works on single earbud

---

## Milestone 3: Audio Feedback on Toggle

Voice prompts "bypass on" / "bypass off" played through the earbud speaker when toggling.

### Changes

#### 1. `config/_default_cfg_src_/res/en/SOUND_BYPASS_ON.opus` (new file)
- Generated via flite TTS → ffmpeg opus encode (48kHz mono, 48kbps, matching existing prompts)

#### 2. `config/_default_cfg_src_/res/en/SOUND_BYPASS_OFF.opus` (new file)
- Same as above

#### 3. `services/resources/resources.h`
- Added `AUDIO_ID_BT_BYPASS_ON` and `AUDIO_ID_BT_BYPASS_OFF` to `AUD_ID_ENUM`

#### 4. `apps/main/app_status_ind.h`
- Added `APP_STATUS_INDICATION_BYPASS_ON` and `APP_STATUS_INDICATION_BYPASS_OFF`

#### 5. `services/bt_app/res_audio_data.h`
- Declared `SOUND_BYPASS_ON[]`, `SOUND_BYPASS_ON_len`, `SOUND_BYPASS_OFF[]`, `SOUND_BYPASS_OFF_len`

#### 6. `apps/main/apps.cpp` (in `app_voice_report_handler`)
- Mapped `APP_STATUS_INDICATION_BYPASS_ON` → `AUDIO_ID_BT_BYPASS_ON`
- Mapped `APP_STATUS_INDICATION_BYPASS_OFF` → `AUDIO_ID_BT_BYPASS_OFF`

#### 7. `services/bt_app/app_media_player.cpp` (in `media_runtime_audio_prompt_update`)
- Mapped `AUDIO_ID_BT_BYPASS_ON` → `SOUND_BYPASS_ON` data/len
- Mapped `AUDIO_ID_BT_BYPASS_OFF` → `SOUND_BYPASS_OFF` data/len

#### 8. `apps/audioplayers/bt_sco_chain.c`
- Added `#include "app_status_ind.h"` and `#include "apps.h"`
- `speech_dsp_bypass_toggle()` now calls `app_voice_report()` after toggling

### How to test
1. Clean build and flash
2. Quad-tap → should hear audio feedback and see 2kHz LPF in spectrum
3. Quad-tap again → should hear audio feedback and see full bandwidth

### Result
- **PARTIAL** — toggle produces a double-beep sound instead of spoken "bypass on"/"bypass off". The TTS opus files are in the build but the media player may be falling back to a default tone. The beep is sufficient as acoustic feedback for now.

---

## Milestone 4: Three-Mode Cycle with Voice Prompts

Replaced 2-state on/off toggle with a 3-state cycle: normal → breathing → passthrough → normal.

### Modes

| Mode | EQ | Compexp | Purpose |
|------|-----|---------|---------|
| **Normal** | Bypassed | ON | Stock voice-optimized processing |
| **Breathing** | Bandpass 250–2000 Hz + boost 700–1500 Hz | OFF | Breathing-enhanced for ML training |
| **Passthrough** | Bypassed | OFF | Raw unfiltered audio (DC filter still active) |

### Breathing mode filter design rationale
- **HPF at 250 Hz**: cuts wind noise (dominant 20–400 Hz), speech F0 (85–255 Hz), footstep rumble
- **LPF at 2000 Hz x2**: steep cut of speech sibilants (4–8 kHz) and HF noise
- **Peaking EQ +6dB at 1000 Hz, Q=0.8**: boosts the breathing sweet spot (700–1500 Hz) where external mouth/nose breathing has spectral peaks

Research basis (external mic, NOT stethoscope/chest-wall):
- Breathing bandwidth via headset mic: 150–2000 Hz (Nam et al., JBHI 2016)
- Nasal breathing spectral correlation at 2–4 kHz (ResearchGate, Frequency Spectra of Normal Expiratory Nasal Sound)
- In-ear mic breathing: 150–2000 Hz with energy peak in mid-range (MDPI Sensors, Classification of Breathing Phase)
- Wind noise dominant below 400 Hz (Lyons et al., Acoustics Today 2021)

### Changes

#### 1. `config/_default_cfg_src_/res/en/SOUND_MODE_NORMAL.opus` (new)
#### 2. `config/_default_cfg_src_/res/en/SOUND_MODE_BREATHING.opus` (new)
#### 3. `config/_default_cfg_src_/res/en/SOUND_MODE_PASSTHROUGH.opus` (new)
- Generated via flite TTS → ffmpeg opus encode (48kHz mono, 48kbps)

#### 4. `services/resources/resources.h`
- Added `AUDIO_ID_BT_MODE_NORMAL`, `AUDIO_ID_BT_MODE_BREATHING`, `AUDIO_ID_BT_MODE_PASSTHROUGH`

#### 5. `apps/main/app_status_ind.h`
- Added `APP_STATUS_INDICATION_MODE_NORMAL`, `MODE_BREATHING`, `MODE_PASSTHROUGH`

#### 6. `services/bt_app/res_audio_data.h`
- Declared `SOUND_MODE_NORMAL`, `SOUND_MODE_BREATHING`, `SOUND_MODE_PASSTHROUGH` arrays and lengths

#### 7. `apps/main/apps.cpp`
- Mapped 3 new status indications → audio IDs

#### 8. `services/bt_app/app_media_player.cpp`
- Mapped 3 new audio IDs → sound data

#### 9. `apps/audioplayers/bt_sco_chain.c`
- Replaced `dsp_bypass_enabled` (bool) with `dsp_mode` (0/1/2 cycle)
- Added 3 static `EqConfig` structs: `dsp_eq_normal`, `dsp_eq_breathing`, `dsp_eq_passthrough`
- `speech_dsp_bypass_toggle()` now cycles through modes and announces via voice report
- `speech_dsp_bypass_is_enabled()` returns true for modes 1 and 2 (compexp skipped)
- Compexp guard: `if (dsp_mode == DSP_MODE_NORMAL)` instead of bool check

### How to test
1. Clean build and flash
2. Quad-tap → cycles: normal → breathing → passthrough → normal
3. Each mode should announce its name ("normal", "breathing", "passthrough")
4. Record in each mode, compare spectra in Audacity
