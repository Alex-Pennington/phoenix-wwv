# Phoenix WWV - Copilot Instructions

## P0 - CRITICAL RULES

1. **NO unauthorized additions.** Do not add features, flags, modes, or files without explicit instruction. ASK FIRST.
2. **Minimal scope.** Fix only what's requested. One change at a time.
3. **Suggest before acting.** Explain your plan, wait for confirmation.
4. **Verify before modifying.** Run `git status`, read files before editing.

---

## Architecture Overview

**Phoenix WWV** is a standalone library for detecting WWV/WWVH time signals. It provides tick detection, marker detection, sync state machine, and BCD time code decoding.

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
   │tick_detector │ │ bcd_decoder  │
   │marker_detect │ │ bcd_envelope │
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
| `src/` | Detection modules |
| `include/` | Public headers, `wwv.h` is unified API |
| `examples/` | Example applications |
| `docs/` | Documentation |

---

## Build

```powershell
# As library
gcc -c -O2 -I include src/*.c
ar rcs libwwv.a *.o

# With example
gcc -O2 -I include src/*.c examples/wwv_clock.c -o wwv_clock -lm
```

---

## Critical Patterns

### P2 - Detector Module Pattern
All detectors follow the same structure:
```c
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
