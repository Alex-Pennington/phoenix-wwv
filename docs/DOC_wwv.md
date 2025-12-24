# DOC_wwv: WWV Detection Signal Path

## Purpose
WWV detection subsystem extracts precise time information from WWV/WWVH time signal broadcasts. The system uses parallel channel filtering and multi-stage detection to decode tick pulses, markers, and BCD time code.

---

## WWV Signal Overview

### Broadcast Format
- **WWV** (Fort Collins, CO): 1000 Hz tick tone
- **WWVH** (Kauai, HI): 1200 Hz tick tone (female voice)
- **Tick Duration**: 5ms pulse every second (except :29 and :59)
- **Minute Marker**: 800ms pulse at second :00
- **BCD Time Code**: 100 Hz AM subcarrier, pulse-width encoded

### Protected Zones
- 40ms around each second boundary (no BCD modulation during tick/marker pulses)
- Seconds 29 and 59: No tick ("tick holes") - silence before markers at :30 and :00

---

## Signal Input to WWV Detectors

### Source
Detectors receive samples from the **DETECTOR PATH** in waterfall.c:
```
Raw I/Q (2 MHz) → Lowpass 5kHz → Decimate 40:1 → 50 kHz
                                                    ↓
                                              Normalizer (slow AGC)
                                                    ↓
                                           det_i, det_q @ 50kHz
```

### Channel Splitting (Parallel Architecture)
After normalization, signal splits into two independent channels:

```c
// Sync Channel: BPF 800-1400 Hz (4th order Butterworth)
float sync_i = sync_channel_process(&g_sync_channel_i, det_i);
float sync_q = sync_channel_process(&g_sync_channel_q, det_q);

// Data Channel: LPF 0-150 Hz (4th order Butterworth)  
float data_i = data_channel_process(&g_data_channel_i, det_i);
float data_q = data_channel_process(&g_data_channel_q, det_q);
```

**Purpose**: Isolate tick/marker energy (1000 Hz) from BCD subcarrier (100 Hz).

---

## Channel Filter Design

### Sync Channel (800-1400 Hz Bandpass)
```
Input: det_i, det_q @ 50 kHz
    ↓
Highpass 800 Hz (2x 2nd order cascade = 4th order)
    ↓
Lowpass 1400 Hz (2x 2nd order cascade = 4th order)
    ↓
Output: sync_i, sync_q
```

**Targets:**
- WWV tick tone (1000 Hz)
- WWVH tick tone (1200 Hz)
- Minute markers (1000 Hz carrier, 800ms duration)

**Rejects:**
- BCD 100 Hz subcarrier
- Voice announcements
- Noise outside tick band

### Data Channel (0-150 Hz Lowpass)
```
Input: det_i, det_q @ 50 kHz
    ↓
Lowpass 150 Hz (2x 2nd order cascade = 4th order)
    ↓
Output: data_i, data_q
```

**Targets:**
- BCD 100 Hz subcarrier and sidebands (100 Hz ± 10 Hz for 10 Hz keying rate)
- 500/600 Hz tones (WWV subcarrier tones on some minutes)

**Rejects:**
- 1000 Hz tick tone
- Voice content
- High frequency noise

---

## Detector Processing Chain

### 1. Tick Detector (Sync Channel Input)

```
sync_i, sync_q @ 50 kHz
    ↓
256-pt FFT (5.12ms window, matches 5ms tick pulse)
    ↓
Bin monitoring: 1000 Hz ± 100 Hz
    ↓
Correlation with 5ms template (240 samples @ 50kHz)
    ↓
Threshold detection: peak_energy / noise_floor > threshold_mult
    ↓
Emit: tick_event_t
```

**Outputs:**
- Tick detections every ~1 second
- Duration measurements (nominal 5ms)
- Correlation ratio (quality metric)
- Noise floor tracking (adaptive threshold)

**Special Detection:**
- Minute markers detected by **duration** (>100ms pulse = marker, not tick)
- Emits separate `tick_marker_event_t` for 800ms markers

**File:** `tick_detector.c` / `tick_detector.h`

---

### 2. Marker Detector (Sync Channel Input)

```
sync_i, sync_q @ 50 kHz
    ↓
Dedicated FFT for marker detection
    ↓
Tracks 800ms pulse energy
    ↓
Confirms minute boundary timing
    ↓
Emit: marker_event_t
```

**Purpose:**
- Independent verification of minute markers
- Cross-checks tick detector's duration-based marker detection
- Provides fallback if tick detector misses marker

**File:** `marker_detector.c` / `marker_detector.h`

---

### 3. Sync Detector (Evidence Fusion)

```
Evidence Sources:
    ├─ tick_detector → tick events
    ├─ tick_detector → marker events (duration-based)
    ├─ marker_detector → marker events
    ├─ bcd_decoder → P-marker events (position markers at BCD seconds 0,9,19,29,39,49,59)
    └─ tick_detector → tick hole detection (absence at :29/:59)
    ↓
Confidence Weighted Fusion
    ↓
State Machine: ACQUIRING → TENTATIVE → LOCKED ↔ RECOVERING
    ↓
Output: frame_time_t (authoritative current_second within minute)
```

**State Progression:**
- **ACQUIRING**: No prior state, building initial reference
- **TENTATIVE**: Some evidence accumulated, gaining confidence
- **LOCKED**: High confidence (>locked_threshold), tracking in real-time
- **RECOVERING**: Lost signal, coasting on predicted timing

**Confidence Decay:**
- Normal operation: slow decay (e.g., -0.01 per second)
- Recovering state: fast decay (e.g., -0.05 per second)
- Evidence hits: confidence boost based on timing accuracy

**Output:**
```c
typedef struct {
    int current_second;      // 0-59, authoritative position
    float second_start_ms;   // When this second began
    float confidence;        // 0.0 - 1.0
    uint32_t evidence_mask;  // Which signals contributed
    sync_state_t state;      // ACQUIRING/TENTATIVE/LOCKED/RECOVERING
} frame_time_t;
```

**File:** `sync_detector.c` / `sync_detector.h`

---

### 4. BCD Time Detector (Data Channel Input)

```
data_i, data_q @ 50 kHz
    ↓
100 Hz subcarrier demodulation
    ↓
Pulse width measurement
    ↓
Symbol classification: 0 (200ms), 1 (500ms), P (800ms)
    ↓
BCD frame decode (minutes, hours, day-of-year)
    ↓
Emit: bcd_time_event_t
```

**BCD Encoding:**
- **200ms pulse**: Binary 0
- **500ms pulse**: Binary 1  
- **800ms pulse**: Position marker (P) at seconds 0, 9, 19, 29, 39, 49, 59

**Frame Structure:**
- Minutes: BCD digits at seconds 1-8
- Hours: BCD digits at seconds 12-18
- Day-of-year: BCD digits at seconds 22-33
- DUT1 correction: Seconds 36-38, 50-51, 56

**Protected Zones:**
- BCD amplitude reduced to zero during 40ms around each second (no collision with tick pulses)

**File:** `bcd_time_detector.c` / `bcd_time_detector.h`

---

### 5. BCD Frequency Detector (Data Channel Input)

```
data_i, data_q @ 50 kHz
    ↓
Tone detector FFT
    ↓
Detect 500 Hz or 600 Hz tones
    ↓
Match against expected schedule
    ↓
Emit: tone detection events
```

**WWV Schedule:**
- **500 Hz minutes**: 4, 6, 12, 14, 16, 20, 22, 24, 26, 28, 32, 34, 36, 38, 40, 42, 52, 54, 56, 58
- **600 Hz minutes**: 1, 3, 5, 7, 11, 13, 15, 17, 19, 21, 23, 25, 27, 31, 33, 35, 37, 39, 41, 53, 55, 57
- **No tone**: Minutes 0, 2, 8, 9, 10, 29, 30, 43-51, 59

**Purpose:**
- Verify minute position (cross-check with BCD decode)
- Signal quality assessment

**File:** `bcd_freq_detector.c` / `bcd_freq_detector.h`

---

### 6. Correlators (Epoch Tracking)

#### Tick Correlator
```
Tick events → Pattern matching across seconds
    ↓
Builds confidence in tick epoch (exact second timing)
    ↓
Provides timing reference for sync detector
```

**File:** `tick_correlator.c` / `tick_correlator.h`

#### Marker Correlator
```
Marker events → Minute boundary tracking
    ↓
Confirms 60-second frame structure
    ↓
Feeds sync detector with marker evidence
```

**File:** `marker_correlator.c` / `marker_correlator.h`

#### BCD Correlator
```
BCD P-markers → Position verification
    ↓
Confirms second position within minute (0,9,19,29,39,49,59)
    ↓
Strong evidence for sync detector
```

**File:** `bcd_correlator.c` / `bcd_correlator.h`

---

## Complete WWV Detection Signal Flow

```
┌─────────────────────────────────────────────────────────┐
│ WATERFALL DETECTOR PATH (50 kHz, normalized)           │
│ det_i, det_q                                            │
└──────────────────┬──────────────────────────────────────┘
                   │
                   ├──────────────────────┬─────────────────────┐
                   ↓                      ↓                     ↓
            ┌─────────────┐      ┌──────────────┐    ┌──────────────┐
            │SYNC CHANNEL │      │DATA CHANNEL  │    │  (future)    │
            │BPF 800-1400 │      │LPF 0-150 Hz  │    │              │
            └──────┬──────┘      └──────┬───────┘    └──────────────┘
                   │                    │
        ┌──────────┼────────┐           ├────────────┬─────────────┐
        ↓          ↓        ↓           ↓            ↓             ↓
   ┌────────┐ ┌────────┐ ┌────────┐ ┌──────┐  ┌──────────┐  ┌──────────┐
   │  TICK  │ │ MARKER │ │ SLOW   │ │ BCD  │  │   BCD    │  │   TONE   │
   │DETECTOR│ │DETECTOR│ │ MARKER │ │ TIME │  │  FREQ    │  │ TRACKER  │
   │        │ │        │ │        │ │DETECT│  │  DETECT  │  │ (500/600)│
   └───┬────┘ └───┬────┘ └───┬────┘ └──┬───┘  └────┬─────┘  └────┬─────┘
       │          │          │         │           │             │
       │ tick     │ marker   │         │ symbols   │ 500/600     │
       │ events   │ events   │         │           │ Hz          │
       │          │          │         │           │             │
       └──────┬───┴──────┬───┴─────────┴─────┬─────┴─────────────┘
              │          │                   │
              ↓          ↓                   ↓
        ┌──────────┐ ┌────────────┐   ┌──────────┐
        │  TICK    │ │  MARKER    │   │   BCD    │
        │CORRELATOR│ │ CORRELATOR │   │CORRELATOR│
        └────┬─────┘ └─────┬──────┘   └────┬─────┘
             │             │                │
             └─────────┬───┴────────────────┘
                       ↓
                ┌──────────────┐
                │     SYNC     │
                │   DETECTOR   │
                │ (Evidence    │
                │  Fusion)     │
                └──────┬───────┘
                       ↓
              ┌────────────────┐
              │  frame_time_t  │
              │                │
              │ current_second │
              │ confidence     │
              │ state          │
              └────────────────┘
                       ↓
              Authoritative Time Reference
```

---

## Evidence Fusion Weighting

Sync detector assigns confidence weights to evidence sources:

| Evidence Type | Default Weight | Tolerance (ms) | Purpose |
|--------------|----------------|----------------|---------|
| Tick | 0.15 | ±10 | Regular 1-second cadence |
| Marker | 0.45 | ±30 | Minute boundary (highest weight) |
| P-Marker | 0.40 | ±30 | BCD position markers (very reliable) |
| Tick Hole | 0.20 | ±10 | Absence at :29/:59 |
| Combined Hole+Marker | 0.65 | ±30 | Hole followed by marker (strongest evidence) |

**Tunable via UDP commands** (see RUNTIME_PARAMETER_TUNING.md)

---

## Timing Calibration

### Filter Group Delay
Total detector path delay from raw I/Q to detection:
- Decimation filter delay: ~0.13 ms
- Butterworth channel filter: ~0.32 ms
- Hann window (FFT): ~2.55 ms
- **Total**: ~3.0 ms (`TICK_FILTER_DELAY_MS`)

### Timestamp Correction
Detectors report **trailing edge** timestamps (when energy drops below threshold).

To get **on-time marker**:
```c
start_timestamp_ms = timestamp_ms - duration_ms - TICK_FILTER_DELAY_MS
```

---

## Output Events

### Tick Events
```c
typedef struct {
    int tick_number;
    float timestamp_ms;
    float interval_ms;      // Since previous tick
    float duration_ms;      // Pulse width
    float peak_energy;
    float noise_floor;
    float corr_peak;        // Template correlation
    float corr_ratio;       // peak / avg
} tick_event_t;
```

### Marker Events (from tick detector)
```c
typedef struct {
    int marker_number;
    float timestamp_ms;        // Trailing edge
    float start_timestamp_ms;  // Leading edge (corrected)
    float duration_ms;         // ~800ms
    float corr_ratio;
    float interval_ms;         // Since previous marker
} tick_marker_event_t;
```

### Sync Events
```c
typedef struct {
    int current_second;         // 0-59
    float second_start_ms;
    float confidence;           // 0.0-1.0
    uint32_t evidence_mask;
    sync_state_t state;         // ACQUIRING/TENTATIVE/LOCKED/RECOVERING
} frame_time_t;
```

### BCD Time Events
```c
typedef struct {
    int minute;
    int hour;
    int day_of_year;
    int dut1;                   // UT1-UTC correction
    float timestamp_ms;
    float confidence;
} bcd_time_event_t;
```

---

## CSV Logging & UDP Telemetry

All detectors support dual output:

### CSV Files (Optional)
- Passed to `_create(csv_path)` 
- `fprintf()` + `fflush()` for immediate write
- Headers include version, metadata

### UDP Telemetry (Always Available)
- Port 3005 broadcast
- `telem_sendf(TELEM_CHANNEL, "format", ...)`
- Channels: TICK, MARK, SYNC, BCDS, CARR, T500, T600, CONS, RESP

---

## Summary: WWV Detection Architecture

### Key Principles
1. **Parallel Channel Filtering**: Isolate tick/marker energy (800-1400 Hz) from BCD subcarrier (0-150 Hz)
2. **Independent Detectors**: Each with own FFT, buffer, state machine
3. **Evidence Fusion**: Sync detector combines all sources with confidence weighting
4. **Timing Precision**: 50 kHz sample rate → ~20 µs resolution, adequate for 5ms pulse detection
5. **Adaptive Thresholds**: Noise floor tracking, slow AGC normalization
6. **Graceful Degradation**: State machine handles signal loss/recovery

### Design Philosophy
WWV detection is optimized for **timing accuracy** over **frequency resolution**. The detector path runs at 50 kHz (not 12 kHz like display) to preserve pulse edge timing. Channel filtering prevents BCD 100 Hz from interfering with tick detection, and vice versa.
