# WWV BCD Time Code Decoder Algorithm

> ⚠️ **PARTIALLY SUPERSEDED** - December 18, 2025
>
> **Superseded content:**
> - Input sample rate is now 50kHz (not 48kHz) - avoids sample drops
> - Goertzel approach replaced by FFT-based detection in bcd_time_detector.c and bcd_freq_detector.c
> - Pulse classification now done via bcd_correlator.c with window-based integration
> - Output sample rate and decimation factors have changed
>
> **Still valuable:** WWV BCD pulse encoding (200/500/800ms), frame structure, time code format.
> The frame structure, frame map and decode (Stages 6 and 7) were corrected to the WWV layout of
> NIST SP 432 Table 3.13; they had shown the WWVB layout.

## Overview

This document describes the algorithm for decoding the Binary Coded Decimal (BCD) time code transmitted by WWV and WWVH on a 100 Hz subcarrier. The time code presents UTC information in serial fashion at a rate of 1 pulse per second, including the current minute, hour, and day of year.

## SDRuno SP2 Reference Parameters

Based on reverse-engineering of SDRuno's AUX SP (SP2) display:

| Parameter | Value |
|-----------|-------|
| Span | 7.3 kHz |
| FFT Size | 2048 points |
| RBW (binwidth) | 5.86 Hz |
| Derived Sample Rate | ~12 kHz (FFT × RBW) |
| Window Function | Sin³ (NENBW ≈ 1.73) |
| Effective RBW | ~10 Hz |

## Input Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Input Sample Rate | 48 kHz | From SDR capture |
| Output Sample Rate | 2.4 kHz | After decimation |
| Target Frequency | 100 Hz | BCD subcarrier |
| Pulse Rate | 1 PPS | Pulse per second |

## WWV BCD Pulse Encoding

| Pulse Width | Duration | Meaning |
|-------------|----------|---------|
| Short | ~200 ms | Binary 0 |
| Medium | ~500 ms | Binary 1 |
| Long | ~800 ms | Position Marker (P) |

## Algorithm Pipeline

```
┌─────────────────────────────────────────────────────────────────┐
│                    WWV BCD DECODER PIPELINE                      │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  48 kHz IQ ──┬──► Decimate ──► 2.5 kHz ──► Goertzel ──► Envelope │
│              │      ÷20                    @100 Hz      Detect   │
│              │                                             │     │
│              │                                             ▼     │
│              │                                      Pulse Width  │
│              │                                       Measure     │
│              │                                             │     │
│              │                                             ▼     │
│              │                                      Symbol       │
│              │                                      Classify     │
│              │                                      (0/1/P)      │
│              │                                             │     │
│              │                                             ▼     │
│              │                                      Frame Sync   │
│              │                                      (find P0)    │
│              │                                             │     │
│              │                                             ▼     │
│              │                                      BCD Decode   │
│              │                                      MM:HH:DDD    │
│              │                                                   │
│              └──► Waterfall Display (parallel path)              │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Stage 1: Decimation (48 kHz → 2.4 kHz)

Two-stage decimation for better filter performance.

```python
# Decimation configuration
DECIM_STAGE1 = 4   # 48000 → 12000 Hz
DECIM_STAGE2 = 5   # 12000 → 2400 Hz
FINAL_FS = 2400    # Final sample rate
```

### Rationale

- Two-stage approach allows for gentler anti-alias filters
- 2400 Hz provides 24× oversampling of 100 Hz target
- Clean integer decimation factors

---

## Stage 2: 100 Hz Extraction (Goertzel Filter)

Goertzel algorithm is optimal for single-frequency detection - O(N) complexity vs O(N log N) for full FFT.

```python
# Goertzel parameters for 100 Hz detection
BLOCK_SIZE = 24          # samples per block (10 ms at 2400 Hz)
BLOCKS_PER_SECOND = 100  # 10 ms resolution for envelope

# Goertzel coefficient for 100 Hz at 2400 Hz sample rate
k = (BLOCK_SIZE * 100) / FINAL_FS  # = 1.0 (bin 1)
omega = (2 * pi * k) / BLOCK_SIZE
coeff = 2 * cos(omega)

def goertzel_magnitude(samples):
    """
    Compute magnitude of 100 Hz component using Goertzel algorithm.

    Args:
        samples: Array of BLOCK_SIZE samples

    Returns:
        Magnitude of 100 Hz component
    """
    s0, s1, s2 = 0, 0, 0
    for sample in samples:
        s0 = sample + coeff * s1 - s2
        s2 = s1
        s1 = s0
    return sqrt(s1*s1 + s2*s2 - coeff*s1*s2)
```

### Why Goertzel?

1. Single frequency of interest (100 Hz)
2. Much more efficient than FFT for single bin
3. Lower latency
4. Simpler implementation

---

## Stage 3: Envelope Detection & Smoothing

```python
# Running envelope with simple low-pass
# Output: 100 values per second (10 ms spacing)

envelope_buffer = RingBuffer(100)  # 1 second history

def update_envelope(goertzel_mag):
    """
    Apply exponential smoothing to Goertzel magnitude.

    Args:
        goertzel_mag: Raw magnitude from Goertzel filter

    Returns:
        Smoothed envelope value
    """
    alpha = 0.3
    smoothed = alpha * goertzel_mag + (1 - alpha) * prev_smoothed
    envelope_buffer.push(smoothed)
    return smoothed
```

---

## Stage 4: Pulse Width Measurement

```python
# Threshold-based edge detection with hysteresis
THRESHOLD_HIGH = noise_floor + 6  # dB - Pulse ON
THRESHOLD_LOW = noise_floor + 3   # dB - Pulse OFF (hysteresis)

# At 100 samples/sec (10 ms resolution):
#   200 ms pulse = ~20 samples
#   500 ms pulse = ~50 samples
#   800 ms pulse = ~80 samples

state = IDLE
pulse_start = 0
pulse_width = 0

def detect_pulse(envelope_sample, sample_idx):
    """
    Detect pulse edges and measure width.

    Args:
        envelope_sample: Current envelope value
        sample_idx: Current sample index (10 ms ticks)

    Returns:
        Classified symbol (BIT_ZERO, BIT_ONE, POSITION_MARK) or None
    """
    global state, pulse_start

    if state == IDLE and envelope_sample > THRESHOLD_HIGH:
        state = IN_PULSE
        pulse_start = sample_idx
    elif state == IN_PULSE and envelope_sample < THRESHOLD_LOW:
        state = IDLE
        pulse_width = sample_idx - pulse_start
        return classify_pulse(pulse_width)
    return None
```

### Hysteresis Thresholding

Using separate high/low thresholds prevents chatter on noisy pulse edges:

```
Signal Level
     │
  +6 dB ─────────────────── THRESHOLD_HIGH (turn ON)
     │         ┌─────┐
     │         │     │
  +3 dB ──────┼─────┼───── THRESHOLD_LOW (turn OFF)
     │        │     │
     │────────┘     └──────
     │
  Noise Floor
     └─────────────────────► Time
```

---

## Stage 5: Symbol Classification

```python
# Pulse width thresholds (in 10ms units)
def classify_pulse(width_samples):
    """
    Classify pulse width into symbol type.

    Args:
        width_samples: Pulse width in 10ms sample units

    Returns:
        Symbol type (BIT_ZERO, BIT_ONE, or POSITION_MARK)
    """
    width_ms = width_samples * 10  # Convert to ms

    if width_ms < 350:
        return BIT_ZERO      # ~200 ms nominal
    elif width_ms < 650:
        return BIT_ONE       # ~500 ms nominal
    else:
        return POSITION_MARK # ~800 ms nominal
```

### Classification Boundaries

```
    0 ms     200 ms    350 ms    500 ms    650 ms    800 ms   1000 ms
    │         │         │         │         │         │         │
    │◄───────────────►│         │         │         │         │
    │    BIT_ZERO     │         │         │         │         │
    │   (nominal)     │◄───────────────────►│        │         │
    │                 │       BIT_ONE       │        │         │
    │                 │      (nominal)      │◄──────────────────►
    │                 │                     │   POSITION_MARK   │
    │                 │                     │     (nominal)     │
```

---

## Stage 6: Frame Synchronization

### WWV Frame Structure (60 seconds)

The WWV and WWVH 100 Hz time code, NIST SP 432 Table 3.13 [1]. BCD fields are sent least
significant bit first. The frame describes the minute in which it is sent. Reserved seconds are
always 0.

```
Second 0:      no pulse (frame reference: the "hole")
Second 1:      reserved
Second 2:      DST indicator (changes 24 h after second 55)
Second 3:      leap second warning (a leap second at the end of this month)
Sec 4-7:       year units (1, 2, 4, 8)
Second 8:      reserved
Second 9:      P1
Sec 10-13:     minute units (1, 2, 4, 8)
Second 14:     reserved
Sec 15-17:     minute tens (10, 20, 40)
Second 18:     reserved
Second 19:     P2
Sec 20-23:     hour units (1, 2, 4, 8)
Second 24:     reserved
Sec 25-26:     hour tens (10, 20)
Sec 27-28:     reserved
Second 29:     P3
Sec 30-33:     day of year units (1, 2, 4, 8)
Second 34:     reserved
Sec 35-38:     day of year tens (10, 20, 40, 80)
Second 39:     P4
Sec 40-41:     day of year hundreds (100, 200)
Sec 42-48:     reserved
Second 49:     P5
Second 50:     UT1 sign (1 = positive)
Sec 51-54:     year tens (10, 20, 40, 80)
Second 55:     DST indicator (changes at 00:00 UTC on the day of a change)
Sec 56-58:     UT1 correction magnitude (0.1, 0.2, 0.4 s)
Second 59:     P0
```

On the day standard time changes to daylight time, second 55 changes from 0 to 1 at 00:00 UTC
and second 2 follows 24 h later; the change back runs the same way [1]. Both are 0 in standard
time and 1 in daylight time.

This is not the WWVB layout (SP 432 Table 2.3 [1]), which sends minutes at seconds 1-8 most
significant bit first and a marker at second 0. A decoder written from the WWVB table decodes a
WWV frame to a confident wrong time.

### Position Marker Pattern

Position markers (P) occur at seconds **9, 19, 29, 39, 49, 59**. Second 0 carries no pulse, so
the minute starts with P0 at second 59 followed by a silent second. The minute itself is marked
by the 800 ms 1000 Hz (WWV) or 1200 Hz (WWVH) tone at second 0, which this repo's sync detector
uses as the anchor. The decoder in `src/decode/wwv_time_decoder.c` takes its frame start from
that anchor and uses the P markers to check the second phase.

```python
# Frame sync state machine
symbol_buffer = RingBuffer(60)
sync_state = SEARCHING

# Expected position marker locations (second 0 has no pulse)
P_POSITIONS = [9, 19, 29, 39, 49, 59]

def process_symbol(symbol, second_tick):
    """
    Process incoming symbol and maintain frame sync.

    Args:
        symbol: Classified symbol (0, 1, or P)
        second_tick: Second boundary indicator

    Returns:
        Decoded UTC time if frame complete, else None
    """
    global sync_state

    symbol_buffer.push(symbol)

    if sync_state == SEARCHING:
        # Look for P markers at expected positions
        if detect_frame_pattern(symbol_buffer):
            sync_state = SYNCED
            frame_offset = calculate_offset()

    elif sync_state == SYNCED:
        if is_frame_complete():
            return decode_frame(symbol_buffer)

    return None

def detect_frame_pattern(buffer):
    """
    Check if buffer contains valid position marker pattern.

    Returns:
        True if valid P marker pattern detected
    """
    # Count P markers and verify spacing
    p_count = 0
    for i, symbol in enumerate(buffer):
        if symbol == POSITION_MARK:
            # Check if this P is at expected offset from previous
            p_count += 1

    # Need at least 3 consecutive P markers at correct spacing
    return p_count >= 3 and verify_spacing(buffer)
```

---

## Stage 7: BCD Decode

WWV has no parity. A single frame decodes as below, but one wrong symbol gives a valid-looking
wrong time. `src/decode/wwv_time_decoder.c` therefore scores every UTC minute against the
symbols of up to 60 consecutive minutes and reports a time as CONFIRMED only when it leads by a
wide margin, and the previous minute agrees (see `include/wwv_time_decoder.h`).

```python
def bcd(symbols, seconds, weights):
    """Sum of weights at seconds whose symbol is 1 (LSB first)."""
    return sum(w for s, w in zip(seconds, weights) if symbols[s] == 1)

def decode_frame(symbols):
    """
    Decode one 60-symbol WWV frame (NIST SP 432 Table 3.13).

    Args:
        symbols: Array of 60 symbols (0, 1, or P); symbols[0] is the silent second

    Returns:
        UTC_Time for the minute in which the frame was sent
    """
    minute = bcd(symbols, [10, 11, 12, 13], [1, 2, 4, 8]) + bcd(symbols, [15, 16, 17], [10, 20, 40])
    hour = bcd(symbols, [20, 21, 22, 23], [1, 2, 4, 8]) + bcd(symbols, [25, 26], [10, 20])
    doy = (bcd(symbols, [30, 31, 32, 33], [1, 2, 4, 8]) +
           bcd(symbols, [35, 36, 37, 38], [10, 20, 40, 80]) +
           bcd(symbols, [40, 41], [100, 200]))
    year = 2000 + bcd(symbols, [4, 5, 6, 7], [1, 2, 4, 8]) + bcd(symbols, [51, 52, 53, 54], [10, 20, 40, 80])
    dut1 = bcd(symbols, [56, 57, 58], [0.1, 0.2, 0.4]) * (1 if symbols[50] == 1 else -1)
    return UTC_Time(year=year, day_of_year=doy, hours=hour, minutes=minute, dut1=dut1,
                    dst=(symbols[55], symbols[2]), leap_second_warning=symbols[3])
```

### BCD Frame Map

```
Sec  | Weight | Field
-----|--------|------------------------------
  0  | -      | No pulse (frame reference)
  1  | 0      | Reserved
  2  | -      | DST indicator (changes 24 h after second 55)
  3  | -      | Leap second warning
  4  | 1      | Year units
  5  | 2      | Year units
  6  | 4      | Year units
  7  | 8      | Year units
  8  | 0      | Reserved
  9  | P      | Position Marker 1
 10  | 1      | Minute units
 11  | 2      | Minute units
 12  | 4      | Minute units
 13  | 8      | Minute units
 14  | 0      | Reserved
 15  | 10     | Minute tens
 16  | 20     | Minute tens
 17  | 40     | Minute tens
 18  | 0      | Reserved
 19  | P      | Position Marker 2
 20  | 1      | Hour units
 21  | 2      | Hour units
 22  | 4      | Hour units
 23  | 8      | Hour units
 24  | 0      | Reserved
 25  | 10     | Hour tens
 26  | 20     | Hour tens
 27  | 0      | Reserved
 28  | 0      | Reserved
 29  | P      | Position Marker 3
 30  | 1      | Day of year units
 31  | 2      | Day of year units
 32  | 4      | Day of year units
 33  | 8      | Day of year units
 34  | 0      | Reserved
 35  | 10     | Day of year tens
 36  | 20     | Day of year tens
 37  | 40     | Day of year tens
 38  | 80     | Day of year tens
 39  | P      | Position Marker 4
 40  | 100    | Day of year hundreds
 41  | 200    | Day of year hundreds
42-48| 0      | Reserved
 49  | P      | Position Marker 5
 50  | -      | UT1 sign (1 = positive)
 51  | 10     | Year tens
 52  | 20     | Year tens
 53  | 40     | Year tens
 54  | 80     | Year tens
 55  | -      | DST indicator (changes at 00:00 UTC)
 56  | 0.1    | UT1 correction (s)
 57  | 0.2    | UT1 correction (s)
 58  | 0.4    | UT1 correction (s)
 59  | P      | Position Marker 0
```

Example from SP 432 [1]: a frame decoding to year 2001, day 173, 21:10 UTC, UT1 +0.3 s is,
seconds 0-59 (`.` for the silent second 0, DST and leap second bits 0):

```
.00010000P000001000P100000100P110001110P100000000P100000110P
```

---

## Key Design Decisions

### 1. Goertzel vs FFT

**Decision:** Use Goertzel algorithm

**Rationale:**
- Single frequency of interest (100 Hz)
- O(N) complexity vs O(N log N) for FFT
- Lower memory footprint
- Lower latency
- Simpler implementation

### 2. Envelope Resolution

**Decision:** 10 ms resolution (100 samples/second)

**Rationale:**
- 20 samples across shortest pulse (200 ms)
- Sufficient margin for width measurement
- Low computational overhead

### 3. Hysteresis Thresholding

**Decision:** 3 dB separation between ON/OFF thresholds

**Rationale:**
- Prevents chatter on noisy edges
- Provides stable state transitions
- Simple to implement and tune

### 4. Parallel Processing Paths

**Decision:** Waterfall display and BCD decoder run independently

**Rationale:**
- Display can run at different update rate
- Decoder failure doesn't affect display
- Easier debugging/testing

---

## Implementation Notes

### Noise Floor Estimation

```python
def estimate_noise_floor(envelope_buffer, percentile=10):
    """
    Estimate noise floor from envelope history.

    Uses lower percentile of recent values to avoid
    including pulse energy in estimate.
    """
    sorted_values = sorted(envelope_buffer)
    idx = int(len(sorted_values) * percentile / 100)
    return sorted_values[idx]
```

### Timing Considerations

- At 2400 Hz sample rate, each sample is 416.67 µs
- Goertzel block (24 samples) = 10 ms
- Minimum pulse (200 ms) = 20 blocks
- Maximum pulse (800 ms) = 80 blocks

### Error Handling

1. **Lost sync:** If 3 consecutive P markers are missed, return to SEARCHING state
2. **Invalid BCD:** If decoded value exceeds valid range (e.g., minutes > 59), the frame holds an
   error. `wwv_time_decoder` does not use the 1s of such a field
3. **Noise burst:** Ignore pulses shorter than 100 ms or longer than 1000 ms

---

## References

[1] M. A. Lombardi, "NIST Time and Frequency Services," National Institute of Standards and
Technology, Gaithersburg, MD, USA, NIST Special Publication 432, 2002 ed. (minor text revisions
April 2003), Table 2.3 "WWVB Time Code Bits" and Table 3.13 "WWV and WWVH Time Code Bits".
[Online]. Available: https://tf.nist.gov/general/pdf/1383.pdf

- WWV/WWVH broadcast format documentation
- SDRuno User Manual v1.42
- SDRplay API documentation

---

## Revision History

| Date | Author | Description |
|------|--------|-------------|
| 2024-12-16 | Claude/Rayven | Initial algorithm design based on SDRuno SP2 analysis |
