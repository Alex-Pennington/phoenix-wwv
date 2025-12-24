# Phoenix WWV - Copilot Instructions

## P0 - CRITICAL RULES

1. **NO unauthorized additions.** Do not add features, flags, modes, or files without explicit instruction. ASK FIRST.
2. **Minimal scope.** Fix only what's requested. One change at a time.
3. **Suggest before acting.** Explain your plan, wait for confirmation.
4. **Verify before modifying.** Run `git status`, read files before editing.
5. **Respect file size limits.** Keep all files under 300 lines. If edits would exceed, suggest split strategy first.

---

## Architecture Overview

**Phoenix WWV** is a standalone library for detecting WWV/WWVH time signals. Organized by feature with all files <300 lines for AI-editability.

### Detection Pipeline
```
Audio/I/Q Input (48-50 kHz)
        │
        ▼
┌───────────────────────────────────────┐
│          Channel Filters              │
│  ┌─────────────┐  ┌─────────────┐    │
│  │Sync Channel │  │Data Channel │    │
│  │BPF 800-1400 │  │ LPF 150 Hz  │    │
│  └──────┬──────┘  └──────┬──────┘    │
└─────────┼────────────────┼───────────┘
          │                │
          ▼                ▼
   ┌──────────────┐ ┌──────────────┐
   │tick_detector │ │ bcd_detector  │
   │marker_detect │ │ bcd_envelope  │
   └──────┬───────┘ └──────┬───────┘
          │                │
          └────────┬───────┘
                   ▼
            ┌─────────────┐
            │sync_detector│
            │state machine│
            └──────┬──────┘
                   ▼
             Decoded Time
```

### Key Directories
| Path | Purpose |
|------|---------|
| `src/detection/` | Feature-organized detector modules |
| `src/correlation/` | Event correlation (tick chains, BCD windows) |
| `src/manager/` | Detector lifecycle and routing |
| `src/core/` | Sync, clock, telemetry, filters, FFT |
| `include/*/` | Public headers + *_internal.h implementation headers |
| `DEPRECIATED/` | Deprecated code (not built) |

### Module Organization
- **detection/tick/** — Tick detector (FFT, state machine, API)
- **detection/marker/** — Marker detector (FFT, state machine, API) + slow_marker
- **detection/bcd/** — BCD decoders (time FFT, freq FFT, state machines, decoder)
- **detection/tone/** — Tone tracker (FFT helpers, measurement, API)
- **correlation/** — Event processors (tick chains, marker correlation, BCD windows)
- **manager/** — Coordinator (lifecycle, routing, API)

---

## Build

```powershell
# As library
gcc -c -O2 -I include src/*.c
ar rc1 - File Size Limit
**All source files must be <300 lines.** If modifications would exceed this:
1. Stop and propose split strategy
2. Follow established patterns (detector/correlator/coordinator)
3. Create internal header if needed
4. Extract to separate module file

### P2 - Detector Module Pattern (Sample Processors)
Used in: tick_detector, marker_detector, bcd_detectors, tone_tracker
```c
// Directory structure
src/detection/feature/
  ├── feature_detector.c        // Public API only (~150 lines)
  ├── feature_fft.c              // FFT processing (~200 lines)
  └── feature_state_machine.c   // State logic (~200 lines)

include/detection/feature/
  ├── feature_detector.h         // Public API
  └── feature_detector_internal.h // Shared internals

// Pattern
typedef struct xxx_detector xxx_detector_t;
typedef void (*xxx_callback_fn)(const xxx_event_t *event, void *user_data);

xxx_detector_t *xxx_detector_create(float sample_rate);
void xxx_detector_destroy(xxx_detector_t *det);
void xxx_detector_set_callback(xxx_detector_t *det, xxx_callback_fn cb, void *user_data);
bool xxx_detector_process_sample(xxx_detector_t *det, float sample);  // HOT PATH
```
Each detector owns: own FFT, own sample buffer, own state machine.

### P3 - Correlator Module Pattern (Event Processors)
Used in: tick_correlator, marker_correlator, bcd_correlator
```c
// Directory structure  
src/correlation/
  ├── feature_correlator.c           // Public API (~300 lines)
  ├── feature_window_manager.c       // Window/chain logic (~200 lines)
  └── feature_symbol_classifier.c    // Classification (~100 lines)

include/correlation/
  ├── feature_correlator.h
  └── feature_correlator_internal.h

// Pattern - processes EVENTS not samples
typedef void (*xxx_event_fn)(const xxx_event_t *event, void *user_data);

xxx_correlator_t *xxx_correlator_create(const char *csv_path);
void xxx_correlator_destroy(xxx_correlator_t *corr);
void xxx_correlator_add_event(xxx_correlator_t *corr, ...event_params);  // Event handler
```

### P4 - Coordinator Pattern (Manager/Orchestrator)
Used in: wwv_detector_manager
```c
// Directory structure
src/manager/
  ├── wwv_detector_manager.c    // Public API + hot paths (~200 lines)
  ├── detector_lifecycle.c       // Create/destroy detectors (~100 lines)
  └── detector_routing.c         // Internal callbacks (~100 lines)

include/manager/
  ├── wwv_detector_manager.h
  └── wwv_detector_manager_internal.h

// Pattern - orchestrates multiple components
struct wwv_detector_manager { /* all detector pointers */ };
bool lifecycle_create_all(manager_t *mgr, config_t *config);
void lifecycle_destroy_all(manager_t *mgr);
void routing_on_event(const event_t *ev, void *user_data);
```
// Header pattern
typedef struct xxx_detector xxx_detector_t;           // Opaque type
typedef void (*xxx_callback_fn)(const xxx_event_t *event, void *user_data);

xxx_detector_t *xxx_detector_create(float sample_rate);
void xxx_detector_destroy(xxx_detector_t *det);
void xxx_detector_set_callback(xxx_detector_t *det, xxx_callback_fn cb, void *user_data);
bool xxx_detector_process(xxx_detector_t *det, float sample);  // Returns true on detection
```
Each detector owns: own FFT (if needed), own sample buffer, own state machine.

### P3 - Telemetry Output Pattern
Detectors can output via callback. Callbacks receive event structs:
```c
typedef struct {
    float timestamp_ms;     // When event occurred
    float duration_ms;      // Pulse/event duration
    float confidence;       // Detection confidence 0-1
    // ... detector-specific fields
} xxx_event_t;
```

### P5 - DSP Filter Pattern
Filters use simple inline structs:
```c
// Lowpass (2nd order Butterworth)
typedef struct { float x1,x2,y1,y2,b0,b1,b2,a1,a2; } lowpass_t;

// DC blocker
typedef struct { float x_prev, y_prev; } dc_block_t;
```

### P9 - WWV Broadcast Clock
WWV/WWVH broadcast timing constants:
```c
#define WWV_TICK_FREQ_HZ        1000    // WWV tick tone
#define WWVH_TICK_FREQ_HZ       1200    // WWVH tick tone
#define WWV_TICK_DURATION_MS    5       // Tick pulse width
#define WWV_MARKER_DURATION_MS  800     // Minute marker width
#define WWV_BCD_SUBCARRIER_HZ   100     // BCD time code

// No tick at seconds 29 and 59
// 800ms marker at second 0
```

---

## Sync State Machine

```
SEARCHING → marker detected → ACQUIRING
                                  │
                          3 consecutive markers
                                  ▼
                              LOCKED ←──── marker detected
                                  │              ↑
                          marker missed          │
                                  ▼              │
                            RECOVERING ──────────┘
                                  │
                          timeout (30s)
                                  ▼
                             SEARCHING
```

States defined in `sync_detector.h`:
- `SYNC_SEARCHING` - Looking for first marker
- `SYNC_ACQUIRING` - Building confidence
- `SYNC_LOCKED` - Full synchronization
- `SYNC_RECOVERING` - Lost marker, using predicted timing

---

## Domain Knowledge

- **WWV:** Fort Collins, CO - 1000 Hz tick tone
- **WWVH:** Kauai, HI - 1200 Hz tick tone (female voice)
- **Tick:** 5ms pulse every second (except :29 and :59)
- **Minute marker:** 800ms pulse at :00
- **Hour marker:** 1500 Hz tone, 800ms at minute :00
- **BCD time code:** 100 Hz AM subcarrier, pulse-width encoded
- **Protected zone:** 40ms around each second (no BCD modulation)

---

## BCD Pulse Widths

| Width | Symbol | Meaning |
|-------|--------|---------|
| 200ms | 0 | Binary zero |
| 500ms | 1 | Binary one |
| 800ms | P | Position marker |

Position markers at seconds: 0, 9, 19, 29, 39, 49, 59

---

## Dependencies

| Library | Purpose |
|---------|---------|
| kiss_fft | FFT for frequency detection (included) |
| math.h | Standard math functions |
