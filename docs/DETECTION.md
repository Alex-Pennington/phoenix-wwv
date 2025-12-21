# WWV Signal Detection

## Overview

This library implements WWV/WWVH time signal detection based on the proven NTP driver36 architecture by David Mills.

---

## Signal Structure

### Tick Pulses

- **WWV:** 1000 Hz, 5ms duration
- **WWVH:** 1200 Hz, 5ms duration
- Occurs at each second except :29 and :59
- 100% modulation depth

### Minute Markers

- 800ms duration at second :00
- Same frequency as ticks (1000/1200 Hz)
- Used for frame synchronization

### Hour Markers

- 1500 Hz tone (both stations)
- 800ms duration at minute :00

### Protected Zone

NIST suppresses all modulation for 40ms around each second marker:
- 10ms silence before tick
- 5ms tick pulse
- 25ms silence after tick

This is the key to tick/BCD separation — the 100 Hz subcarrier is guaranteed absent during this window.

---

## BCD Time Code

### Subcarrier

- 100 Hz amplitude-modulated subcarrier
- Pulse-width modulation: 200ms (0), 500ms (1), 800ms (P)
- 1 bit per second, 60-bit frame

### Frame Structure

Position markers (P) at seconds: 0, 9, 19, 29, 39, 49, 59

| Seconds | Content |
|---------|---------|
| 1-8 | Minutes (BCD) |
| 10-18 | Hours (BCD) |
| 20-28 | Day of year hundreds/tens |
| 30-38 | Day units + DUT1 sign |
| 40-48 | DUT1 magnitude + misc |
| 50-58 | Year + leap second warning |

---

## Detection Pipeline

### Phase 1: Channel Separation

Split input into sync and data channels using parallel filter banks:

**Sync Channel (tick/marker):**
- 4th-order Butterworth bandpass 800-1400 Hz
- Passes tick tones, rejects BCD subcarrier harmonics

**Data Channel (BCD):**
- 4th-order Butterworth lowpass 150 Hz
- Extracts 100 Hz subcarrier envelope

### Phase 2: Tick Detection

1. FFT-based energy detection at 1000 Hz
2. Timing gate (0-30ms after predicted second)
3. 5ms matched filter correlation
4. 1-second comb filter for coherent averaging

### Phase 3: Marker Detection

1. Extended tick detection (>700ms duration)
2. Validate at predicted minute boundary
3. Update sync epoch on confirmed marker

### Phase 4: Sync State Machine

States:
- **SEARCHING:** Looking for first marker
- **ACQUIRING:** Building confidence (need 3 consecutive markers)
- **LOCKED:** Full synchronization achieved
- **RECOVERING:** Lost marker, using predicted timing

### Phase 5: BCD Decoding

1. Envelope detection of 100 Hz subcarrier
2. Pulse width measurement
3. Symbol classification (0/1/P)
4. Frame alignment using position markers
5. BCD to decimal conversion

---

## Timing Accuracy

| Parameter | Typical Value |
|-----------|---------------|
| Tick detection | ±1 ms |
| Minute boundary | ±5 ms |
| BCD symbol | ±10 ms |
| Absolute time | ±20 ms (propagation limited) |

---

## Signal Requirements

| Parameter | Minimum | Recommended |
|-----------|---------|-------------|
| SNR (tick) | -10 dB | +6 dB |
| SNR (BCD) | +3 dB | +10 dB |
| Sample rate | 8 kHz | 48-50 kHz |

---

## References

- NIST Special Publication 432
- NTP Reference Clock Driver 36 (David Mills)
- phoenix-reference-library signal_characteristics.md
