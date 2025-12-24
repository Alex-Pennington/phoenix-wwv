# phoenix-wwv

**Version:** v1.0.0  
**Part of:** Phoenix Nest MARS Communications Suite  
**Developer:** Alex Pennington (KY4OLB)

---

## Overview

**Phoenix WWV** is a standalone C library for detecting WWV/WWVH time signals. It provides real-time detection of tick pulses, minute markers, synchronization state tracking, and BCD time code decoding.

Extracted from the waterfall application, this library provides the core WWV detection engine without display or audio I/O dependencies.

---

## Features

- **Tick Detection** — 1000 Hz (WWV) / 1200 Hz (WWVH) 5ms pulse detection
- **Minute Marker Detection** — 800ms marker detection for minute boundaries
- **Sync State Machine** — Multi-stage synchronization with confidence tracking
- **BCD Time Decoder** — 100 Hz subcarrier pulse detection and decoding
- **Tone Tracking** — 500/600 Hz reference tone identification
- **Channel Separation** — Sync/data channel filtering per NTP driver36 architecture
- **No Dependencies** — Pure C, only requires math library (no SDL2, no networking)

---

## Quick Start

### Build

```powershell
# Build library and example
.\build_lib.ps1

# Clean build
.\build_lib.ps1 -Clean

# Release build
.\build_lib.ps1 -Release
```

### Test

```batch
# Run with synthetic WWV signal
.\test_detector.bat
```

### Use in Your Code

```c
#include "phoenix_wwv.h"

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
void tick_callback(const wwv_tick_event_t *event, void *user_data) {
    printf("Tick: %.1f ms, SNR: %.1f dB\n", event->timestamp_ms, event->snr_db);
}

int main(void) {
    wwv_detector_suite_t *suite = wwv_suite_create(50000.0f);
    wwv_suite_set_tick_callback(suite, tick_callback, NULL);
    
    /* Process I/Q samples */
    while (have_samples()) {
        float i, q;
        get_next_sample(&i, &q);
        wwv_suite_process_iq(suite, i, q);
    }
    
    wwv_suite_destroy(suite);
    return 0;
}
```

See [examples/wwv_detect_example.c](examples/wwv_detect_example.c) for a complete working example.

---

## Project Structure

```
phoenix-wwv/
├── include/
│   ├── phoenix_wwv.h          # Public API (use this)
│   ├── wwv_telemetry_stub.h   # No-op telemetry stubs
│   └── *.h                     # Individual detector headers
├── src/
│   ├── phoenix_wwv.c           # Main library implementation
│   ├── kiss_fft.c              # FFT library
│   ├── detectors/              # Detection modules
│   │   ├── tick_detector.c
│   │   ├── marker_detector.c
│   │   ├── sync_detector.c
│   │   ├── bcd_time_detector.c
│   │   ├── bcd_freq_detector.c
│   │   └── bcd_decoder.c
│   ├── correlators/            # Correlation analysis
│   │   ├── tick_correlator.c
│   │   ├── marker_correlator.c
│   │   └── bcd_correlator.c
│   ├── dsp/                    # Signal processing
│   │   ├── channel_filters.c
│   │   └── tick_comb_filter.c
│   └── utils/
│       └── wwv_clock.c         # Clock utilities
├── examples/
│   └── wwv_detect_example.c    # Minimal usage example
├── tools/
│   ├── gen_test_signal.py      # Test signal generator
│   └── diagnostics/            # Diagnostic tools
├── deprecated_waterfall/       # Original waterfall app code
├── docs/
│   ├── DETECTION.md            # Detection algorithms
│   ├── DOC_waterfall.md        # Waterfall signal paths
│   └── DOC_wwv.md              # WWV detection flow
├── build_lib.ps1               # Build script
└── test_detector.bat           # Test script
```

---

## API Reference

### Core Functions

| Function | Description |
|----------|-------------|
| `wwv_suite_create()` | Create detector suite |
| `wwv_suite_destroy()` | Cleanup and free resources |
| `wwv_suite_process_iq()` | Process one I/Q sample pair |
| `wwv_suite_is_locked()` | Check if synchronized |
| `wwv_suite_get_confidence()` | Get sync confidence [0-1] |
| `wwv_suite_get_frame_time()` | Get current frame timing |
| `wwv_suite_get_bcd_time()` | Get decoded BCD time |

### Callbacks

| Callback | Event Type | Description |
|----------|-----------|-------------|
| `wwv_suite_set_tick_callback()` | `wwv_tick_event_t` | Tick pulse detected |
| `wwv_suite_set_marker_callback()` | `wwv_marker_event_t` | Minute marker detected |
| `wwv_suite_set_sync_callback()` | `wwv_frame_time_t` | Sync state changed |
| `wwv_suite_set_bcd_callback()` | `wwv_bcd_time_t` | BCD time decoded |

---

## Building

### Using Build Script (Recommended)

```powershell
# Debug build
.\build_lib.ps1

# Release build  
.\build_lib.ps1 -Release

# Clean
.\build_lib.ps1 -Clean
```

### Manual Build

```bash
gcc -c -O2 -I include src/phoenix_wwv.c src/kiss_fft.c src/detectors/*.c \
    src/correlators/*.c src/dsp/*.c src/utils/*.c
ar rcs libphoenix_wwv.a *.o

# Link with your application
gcc -o myapp myapp.c -L. -lphoenix_wwv -lm
```

### With Example

```bash
gcc -O2 -I include -o wwv_detect examples/wwv_detect_example.c \
    -L. -lphoenix_wwv -lm
```

---

## Testing

```batch
# Run with synthetic signal
.\test_detector.bat

# Or generate custom test
python tools\gen_test_signal.py > test.raw
cmd /c "build\wwv_detect.exe < test.raw"
```

---

## Documentation

- [DETECTION.md](docs/DETECTION.md) — Detection algorithms and theory
- [DOC_wwv.md](docs/DOC_wwv.md) — WWV detection signal flow
- [DOC_waterfall.md](docs/DOC_waterfall.md) — Waterfall app architecture (deprecated)
- [phoenix_wwv.h](include/phoenix_wwv.h) — Complete API reference

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
