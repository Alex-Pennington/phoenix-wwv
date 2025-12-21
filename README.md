# phoenix-wwv

**Version:** v0.1.0  
**Part of:** Phoenix Nest MARS Communications Suite  
**Developer:** Alex Pennington (KY4OLB)

---

## Overview

WWV/WWVH time signal detection library for the Phoenix Nest MARS Suite. Provides real-time detection of WWV broadcast signals including tick pulses, minute markers, and BCD time code decoding.

This is the **detection engine** used by phoenix-waterfall and other tools requiring WWV timing.

---

## Features

- **Tick Detection** — 1000 Hz (WWV) / 1200 Hz (WWVH) 5ms pulse detection
- **Minute Marker Detection** — 800ms marker detection for minute boundaries
- **Sync State Machine** — Multi-stage synchronization with confidence tracking
- **BCD Time Decoder** — 100 Hz subcarrier time code extraction
- **Tone Tracking** — 500/600 Hz reference tone identification
- **Channel Separation** — Sync/data channel filtering per NTP driver36 architecture
- **Comb Filtering** — Coherent averaging for weak signal detection

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                          phoenix-wwv                                 │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   I/Q Input (50 kHz)                                               │
│        │                                                            │
│        ▼                                                            │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │              Channel Filters (NTP driver36 style)            │   │
│  │  ┌─────────────────┐        ┌─────────────────┐             │   │
│  │  │  Sync Channel   │        │  Data Channel   │             │   │
│  │  │  BPF 800-1400Hz │        │  LPF 150 Hz     │             │   │
│  │  └────────┬────────┘        └────────┬────────┘             │   │
│  └───────────┼──────────────────────────┼──────────────────────┘   │
│              │                          │                          │
│              ▼                          ▼                          │
│  ┌───────────────────┐      ┌───────────────────┐                 │
│  │   Tick Detector   │      │   BCD Detector    │                 │
│  │   (1000 Hz FFT)   │      │   (100 Hz env)    │                 │
│  └─────────┬─────────┘      └─────────┬─────────┘                 │
│            │                          │                            │
│            ▼                          ▼                            │
│  ┌───────────────────┐      ┌───────────────────┐                 │
│  │ Marker Detector   │      │  BCD Correlator   │                 │
│  │   (800ms pulse)   │      │ (window-based)    │                 │
│  └─────────┬─────────┘      └─────────┬─────────┘                 │
│            │                          │                            │
│            └──────────┬───────────────┘                            │
│                       ▼                                            │
│              ┌───────────────┐                                     │
│              │ Sync Detector │                                     │
│              │ State Machine │                                     │
│              └───────┬───────┘                                     │
│                      │                                              │
│                      ▼                                              │
│               Decoded Time                                          │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Detection Pipeline

### Sync Channel (Tick/Marker Detection)

1. **Bandpass Filter** — 800-1400 Hz isolates tick/marker tones
2. **FFT Detection** — 256-point FFT identifies 1000 Hz energy
3. **Matched Filter** — 5ms template correlation
4. **Comb Filter** — 1-second coherent averaging
5. **Timing Gate** — 0-30ms window per second (protected zone)

### Data Channel (BCD Decoding)

1. **Lowpass Filter** — 150 Hz extracts 100 Hz subcarrier
2. **Envelope Detection** — AM demodulation
3. **Pulse Classification** — 200ms (0), 500ms (1), 800ms (P)
4. **Frame Sync** — Position marker alignment
5. **BCD Decode** — Extract minutes, hours, day of year

---

## Usage

### Basic Example

```c
#include "wwv.h"

// Create detector instances
tick_detector_t *tick = tick_detector_create(50000.0);  // 50 kHz sample rate
marker_detector_t *marker = marker_detector_create(50000.0);
sync_detector_t *sync = sync_detector_create();

// Process I/Q samples
void process_samples(float *i, float *q, size_t count) {
    for (size_t n = 0; n < count; n++) {
        // Compute magnitude
        float mag = sqrtf(i[n]*i[n] + q[n]*q[n]);
        
        // Feed detectors
        tick_detector_process(tick, mag);
        marker_detector_process(marker, mag);
        
        // Check for events
        if (tick_detector_detected(tick)) {
            sync_detector_on_tick(sync, current_time_ms);
        }
        if (marker_detector_detected(marker)) {
            sync_detector_on_marker(sync, current_time_ms);
        }
    }
    
    // Check sync state
    if (sync_detector_is_locked(sync)) {
        printf("Locked: %s\n", sync_detector_get_time_string(sync));
    }
}

// Cleanup
tick_detector_destroy(tick);
marker_detector_destroy(marker);
sync_detector_destroy(sync);
```

---

## Components

### Core Detectors

| File | Description |
|------|-------------|
| `tick_detector.c/h` | 1000 Hz tick pulse detection |
| `marker_detector.c/h` | 800ms minute marker detection |
| `sync_detector.c/h` | Synchronization state machine |
| `tick_correlator.c/h` | Tick correlation analysis |
| `marker_correlator.c/h` | Marker correlation analysis |
| `slow_marker_detector.c/h` | Backup marker detection |
| `tick_comb_filter.c/h` | 1-second comb filter |

### Channel Processing

| File | Description |
|------|-------------|
| `channel_filters.c/h` | Sync/data channel separation |
| `tone_tracker.c/h` | 500/600 Hz tone tracking |

### BCD Decoding

| File | Description |
|------|-------------|
| `bcd_envelope.c/h` | 100 Hz subcarrier envelope |
| `bcd_decoder.c/h` | BCD symbol classification |
| `bcd_time_detector.c/h` | Time-domain BCD detection |
| `bcd_freq_detector.c/h` | Frequency-domain BCD detection |
| `bcd_correlator.c/h` | Window-based BCD integration |

### Utilities

| File | Description |
|------|-------------|
| `kiss_fft.c/h` | FFT processing |
| `wwv_clock.c/h` | Example clock application |

---

## Sync State Machine

```
┌──────────────┐
│  SEARCHING   │◄────────────────────────────┐
└──────┬───────┘                             │
       │ marker detected                      │
       ▼                                      │
┌──────────────┐                             │
│  ACQUIRING   │                             │
└──────┬───────┘                             │
       │ 3 consecutive markers                │
       ▼                                      │
┌──────────────┐         timeout (90s)       │
│   LOCKED     │─────────────────────────────┤
└──────┬───────┘                             │
       │ marker missed                        │
       ▼                                      │
┌──────────────┐         timeout (30s)       │
│  RECOVERING  │─────────────────────────────┘
└──────────────┘
       │ marker detected
       ▼
   Back to LOCKED
```

---

## Building

### As Library

```bash
gcc -c -O2 -I include src/*.c
ar rcs libwwv.a *.o
```

### With Application

```bash
gcc -O2 -I include src/*.c examples/wwv_clock.c -o wwv_clock -lm
```

---

## Related Repositories

| Repository | Description |
|------------|-------------|
| [mars-suite](https://github.com/Alex-Pennington/mars-suite) | Phoenix Nest MARS Suite index |
| [phoenix-sdr-core](https://github.com/Alex-Pennington/phoenix-sdr-core) | SDR hardware interface |
| [phoenix-waterfall](https://github.com/Alex-Pennington/phoenix-waterfall) | Waterfall display (uses this library) |
| [phoenix-reference-library](https://github.com/Alex-Pennington/phoenix-reference-library) | WWV technical documentation |

---

## References

- NIST Special Publication 432 — WWV/WWVH Broadcast Format
- NTP Reference Clock Driver 36 — David Mills' proven architecture
- [phoenix-reference-library](https://github.com/Alex-Pennington/phoenix-reference-library) — Detailed signal specifications

---

## License

AGPL-3.0 — See [LICENSE](LICENSE)

---

*Phoenix Nest MARS Communications Suite*  
*KY4OLB*
