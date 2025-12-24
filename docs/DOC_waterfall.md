# DOC_waterfall: Waterfall Application Signal Path

## Purpose
Waterfall.exe is an SDL2-based spectrum analyzer and WWV detector application. It receives I/Q samples via TCP or stdin and processes them through two independent signal paths for different purposes.

---

## Signal Source

### Input Options
1. **TCP Mode (default)**: Receives I/Q stream from `sdr_server` on port 4536
   - Protocol: PHXI header + IQDQ data frames + META updates
   - Sample formats: S16 (16-bit signed), F32 (float32), U8 (unsigned 8-bit)
   - Typical rate: 2 MHz from SDRplay RSP2 Pro

2. **stdin Mode**: Reads 16-bit signed PCM mono audio from stdin
   - Legacy mode for piping from simple_am_receiver

3. **Test Pattern Mode**: Generates synthetic 1000 Hz tone internally
   - Used for testing detector pipeline without hardware

### Raw I/Q Samples
```
i_raw = (float)sample_i / 32768.0f;  // Normalized to [-1, 1]
q_raw = (float)sample_q / 32768.0f;
```

---

## Dual Signal Path Architecture

The waterfall application splits the incoming I/Q stream into **TWO INDEPENDENT PATHS** immediately after normalization:

### Path Divergence Point (waterfall.c line ~2005)
```c
/* Input: i_raw, q_raw from TCP/stdin */

/* DETECTOR PATH - optimized for pulse timing */
float det_i = lowpass_process(&g_detector_lowpass_i, i_raw);
float det_q = lowpass_process(&g_detector_lowpass_q, q_raw);
// → Decimated to 50 kHz → WWV detectors

/* DISPLAY PATH - optimized for spectrum visualization */
float disp_i = lowpass_process(&g_display_lowpass_i, i_raw);
float disp_q = lowpass_process(&g_display_lowpass_q, q_raw);
// → Decimated to 12 kHz → FFT waterfall
```

**CRITICAL RULE**: These paths NEVER share filters, buffers, or processing state.

---

## DETECTOR PATH (50 kHz)

### Purpose
Feed WWV detectors with samples optimized for precise pulse timing detection.

### Signal Chain
```
i_raw, q_raw (2 MHz)
    ↓
Lowpass Filter (5 kHz cutoff, Butterworth)
    ↓
Decimate 40:1 → 50 kHz
    ↓
Normalizer (slow AGC for gain independence)
    ↓
Channel Filters (parallel processing)
    ├─→ Sync Channel (BPF 800-1400 Hz) → tick_detector, marker_detector
    └─→ Data Channel (LPF 150 Hz) → bcd_time_detector, bcd_freq_detector
```

### Configuration
- **Sample Rate**: 50 kHz (exactly 2MHz ÷ 40)
- **Decimation**: 40:1 from TCP stream
- **Anti-alias Filter**: 5 kHz cutoff (2nd order Butterworth lowpass)
- **Buffer**: `g_detector_buffer` (not used for FFT)

### Components Fed
1. **Tick Detector** (sync channel) - Detects 1000 Hz / 1200 Hz tick pulses
2. **Marker Detector** (sync channel) - Detects 800ms markers
3. **BCD Time Detector** (data channel) - Decodes 100 Hz BCD time code
4. **BCD Freq Detector** (data channel) - Detects 500/600 Hz tones
5. **Sync Detector** - State machine for frame synchronization

### Normalizer (Slow AGC)
```c
// Tracks signal level over ~1 second
// Adjusts gain to maintain consistent detector thresholds
// Independent of SDR gain settings
float norm_factor = normalize(&g_normalizer, det_i, det_q);
det_i *= norm_factor;
det_q *= norm_factor;
```

### Periodic Sync Check
Every 100ms (5000 samples @ 50kHz), `sync_detector_periodic_check()` is called to verify frame lock.

---

## DISPLAY PATH (12 kHz)

### Purpose
Generate high-resolution waterfall display matching SDRuno quality.

### Signal Chain
```
i_raw, q_raw (2 MHz)
    ↓
Lowpass Filter (5 kHz cutoff, Butterworth)
    ↓
Decimate 166:1 → 12 kHz
    ↓
Buffer (2048 samples)
    ↓
2048-pt FFT with Blackman-Harris window
    ↓
50% overlap (1024 samples)
    ↓
SDL2 Waterfall Texture
```

### Configuration
- **Sample Rate**: 12 kHz (2MHz ÷ 166.67 ≈ 12000)
- **Decimation**: 166:1 from TCP stream (varies with actual rate)
- **Anti-alias Filter**: 5 kHz cutoff (1 kHz Nyquist guard band)
- **FFT Size**: 2048 points
- **Overlap**: 50% (1024 samples)

### Display Characteristics
- **Frequency Resolution**: 5.86 Hz/bin (12000 Hz ÷ 2048)
- **Frame Duration**: 170.7 ms per FFT (2048 samples @ 12 kHz)
- **Effective Update Rate**: 85.3 ms (with 50% overlap)
- **RBW**: ~11 Hz (Blackman-Harris NENBW ≈ 1.9 bins)
- **Bandwidth**: ±6 kHz displayed (sufficient for WWV ±2 kHz content)

### Window Function
Blackman-Harris 4-term window for clean spectral lines and low sidelobes.

### Components Fed
1. **FFT Waterfall** - Main spectrum display
2. **Tone Tracker** - Visual tracking of 500/600 Hz tones (independent FFT)

---

## Path Isolation Rules

### Independent Filter Instances
```c
/* DETECTOR PATH filters */
static lowpass_t g_detector_lowpass_i;
static lowpass_t g_detector_lowpass_q;

/* DISPLAY PATH filters (completely separate) */
static lowpass_t g_display_lowpass_i;
static lowpass_t g_display_lowpass_q;
```

### Independent Buffers
```c
/* DETECTOR PATH */
static iq_sample_t *g_detector_buffer;
static int g_detector_buffer_idx;

/* DISPLAY PATH */
static iq_sample_t *g_display_buffer;
static int g_display_buffer_idx;
```

### Independent Decimation Counters
```c
static int g_detector_decim_counter = 0;
static int g_display_decim_counter = 0;
```

**Why?** Prevents any crosstalk between timing-critical detector processing and visualization. Detector path optimized for 5ms tick pulse timing (~50 sample accuracy @ 50kHz). Display path optimized for spectral clarity.

---

## Audio Output Path (Independent)

Waterfall also supports audio monitoring via `waterfall_audio.c`:

```
i_raw, q_raw
    ↓
waterfall_audio module (separate DSP chain)
    ↓
AM demodulation
    ↓
Audio filtering
    ↓
SDL2 Audio Queue
```

**Isolation**: Audio path has its own `g_audio_dsp` state, completely separate from detector/display paths.

---

## Telemetry Output

All detector events are broadcast via UDP telemetry (port 3005):
- **TELEM_TICKS**: Tick detections
- **TELEM_MARKERS**: Marker detections  
- **TELEM_SYNC**: Sync state changes
- **TELEM_BCDS**: BCD symbol decode
- **TELEM_CARR**: Carrier tracking
- **TELEM_T500** / **TELEM_T600**: Tone detections
- **TELEM_CONSOLE**: General messages
- **TELEM_RESP**: Command responses

See `waterfall_telemetry.h` for protocol details.

---

## Summary: Path Comparison

| Aspect | DETECTOR PATH | DISPLAY PATH |
|--------|---------------|--------------|
| Sample Rate | 50 kHz | 12 kHz |
| Decimation | 40:1 | 166:1 |
| Filter Cutoff | 5 kHz | 5 kHz |
| Purpose | Pulse timing | Spectrum visualization |
| Processing | Channel filters → Detectors | FFT → Waterfall |
| Time Resolution | ~20 µs per sample | 85 ms effective frame rate |
| Frequency Resolution | N/A (time domain) | 5.86 Hz/bin |
| Critical For | WWV time decoding | User feedback |

**Design Philosophy**: Detector path prioritizes timing accuracy. Display path prioritizes visual clarity. No compromises, no shared state.
