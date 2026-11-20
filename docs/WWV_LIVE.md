# wwv_live: live WWV decode on Linux

`wwv_live` runs the library's whole chain on an I/Q stream: channel filters, tick, marker and
BCD detectors, sync detector, BCD correlator and time decoder, all wired by
`wwv_detector_manager`. It broadcasts the telemetry in `UDP_TELEMETRY_OUTPUT_PROTOCOL.md` on UDP
port 3005 and prints a status line.

## Build

```
cmake -S . -B build && cmake --build build -j
```

The binary is `build/wwv_live` (Linux only).

## Inputs

- **stdin:** interleaved I/Q, `-f s16` (int16, scaled /32768 as waterfall.c did) or `-f f32`, at
  `-r` Hz. The rate must be a whole multiple of 50 kHz, with prime factors no larger than 8.
- **sdr_server TCP:** `-i tcp:HOST:PORT`. The protocol is the one waterfall.c read: a 32-byte
  `PHXI` header (rate, format, centre frequency), then `IQDQ` frames (16-byte header with a
  sequence number) and `META` updates. Sequence gaps are reported. `scripts/iqdq_replay.py`
  serves a file this way for testing.

## Front end (`iq_frontend`)

The input is multiplied by `-g`, then mixed by `-s` Hz, so a carrier at +`-s` Hz lands at DC.
FIR stages of at most /8 decimate it to 50 kHz, keeping 0-7 kHz free of aliases. A 6 kHz lowpass
then gives the detectors the band of the 12 kHz streams that the air runs were scored on, and a
6/25 resampler makes the 12 kHz tone path. At 50 kHz input the detector path is not filtered.
`-a` adds the waterfall.c slow AGC before the channel filters.

## Time

Detector timestamps are milliseconds since detector sample 0. Input sample 0 is anchored to the
system clock as follows.

1. The first block sets the anchor: its arrival time minus its duration.
2. For 10 s the anchor follows the least-delayed block (arrival minus stream time).
3. The anchor is then fixed and logged again.

With `-T UNIX` (a recording), the anchor is the time given. UTC(t) = anchor - group delay +
t/1000. The anchor is printed in `ANCHOR` lines, sent as `CONS,ANCHOR`, and written by `-j FILE`
as `out_utc0_unix`, the field the scoring scripts read. The anchor has no clock-rate correction.

## Output

- `STATUS` shows:
  - the sync state and confidence, and the second of the minute;
  - tick and marker counts;
  - the last 20 symbols: `0 1 P`, `.` for an erasure, `~` for a mid-second 0 or 1, which the
    decoder ignores;
  - the decoded UTC carried forward, with each part's state as `[tod date DUT1 DST LSW]`: `D`
    DECODED, `C` CONFIRMED, `P` PREDICTED, `-` NONE;
  - the arrival lag range (live input only). A rising minimum lag means samples are being lost
    upstream.
- `SYNC`, `MARKER` and `MINUTE` lines appear as events happen. `MINUTE` shows the frame, every
  decoded part with its state and margin, and how far the system clock sits from the decoded
  minute.
- `-l FILE` writes the air harness's event log (`wwv_chain_log`). wwv-capture `score.py` and the
  `tests/air` scorers read it.
- `-F FILE` writes the BCD time detector's state for every 5.12 ms frame: 9 float32 per record,
  in native byte order (`t_ms energy noise_floor threshold_high threshold_low state
  start_threshold pulse_level pulse`; see `write_frame()` in `apps/wwv_live.c`). That is
  7 kB/s, 37 MB for 90 minutes. The event log carries only the events, so the energy around a
  missed or mid-second pulse is in this file alone. The first six fields match
  `tests/air/btime_frames` on the same input. Through the wrapper, pass it after `--`:

  ```
  scripts/wwv_rsp2_live.sh --secs 5400 --log run.log --timebase run.json -- --frames run.frames
  ```

## Saga RSP2

`scripts/wwv_rsp2_live.sh` drives the run. Its sequence:

1. It starts `sdrplay.service`.
2. With `--wwv-khz auto`, it runs the band scan below under that same start, and takes the
   best band.
3. It streams the RSP2 through `scripts/rsp2_stream.py` into `wwv_live`. The settings are those
   of the scored capture, at the chosen band: antenna A, tuner 100 kHz below WWV, AGC off,
   2 MS/s, scale 32, and the band's gain.
4. It stops the service on every exit path. If the vendor service crashed on stop, the script
   also clears the unit's failed state.

`--record PREFIX` also writes the raw stream, at 8 MB/s. Example, 90 minutes at 5 MHz:

```
scripts/wwv_rsp2_live.sh --secs 5400 --log run.log --timebase run.json
```

### Band

WWV transmits on 2.5, 5, 10, 15, 20 and 25 MHz. Which band reaches the indoor whip depends on
the time of day: 5 MHz carried at 12:09 UTC on 2026-11-19 and was not usable at 17:42 UTC, and
10 MHz was weak at dawn. `--wwv-khz` picks the band, and the default is 5000:

```
scripts/wwv_rsp2_live.sh --wwv-khz 10000 --secs 600
scripts/wwv_rsp2_live.sh --wwv-khz auto --secs 5400 --log run.log --timebase run.json
```

The detectors work on the demodulated baseband and do not depend on the band. `iq_frontend`
only mixes the carrier at `-s` Hz to DC, and `wwv_live` uses the PHXI centre frequency for its
`INPUT` line only.

- **Carrier correction.** The RSP2's LO and ADC share one TCXO, which read 1.09 ppm slow at
  5 MHz. That puts the carrier `ppm × f_MHz` Hz above +100 kHz: 2.7 Hz at 2.5 MHz, 5.45 Hz at
  5 MHz, 10.9 Hz at 10 MHz, 27.3 Hz at 25 MHz. The wrapper adds that to the shift. `--ppm`
  sets the error, and `--carrier-hz` sets the correction outright. The scan reports the
  error each band's carrier implies: the saga 30 s recordings give 1.09 ppm at 5 MHz and
  0.99 ppm at 10 MHz.
- **Gain** is per band, in `scripts/wwv_bands.py`. 2.5, 5, 10 and 15 MHz use LNA state 2 and
  IFGR 59. That is the 5 MHz setting of the scored capture, and 10 MHz was tested with it.
  20 and 25 MHz use LNA 1 and IFGR 59, because their band noise and broadcasting are lower.
  The values other than 5 and 10 MHz are starting points; the scan's ADC columns say whether
  they hold. `--gain KHZ:LNA:IFGR` (repeatable) overrides a band in the scan and in the live
  run, and `--lna` / `--ifgr` override the live band.

### Band scan

```
scripts/wwv_band_scan.sh [--secs 25] [--bands 2500,5000,...] [--gain KHZ:LNA:IFGR] [--json FILE]
```

It starts `sdrplay.service` once and stops it on every exit path. It opens the RSP2 once and
reads each band for `--secs`, about 3.5 minutes for all six. It then prints a ranked table, and
the last stdout line is `BEST <kHz>`. `scripts/wwv_band_scan.py --replay PREFIX:KHZ` measures
recordings instead and opens no device.

The method is that of wwv-capture `check_signal.py` and `saga/scripts/tick_contrast.py`:

- **C/N0:** carrier bin over the median bin 2000-4500 Hz away, per Hz.
- **sb1000, sb1200:** the 1000 Hz and 1200 Hz sidebands over the floor.
- **Tick fold, tick contrast:**
  - The 900-1100 Hz energy of the AM envelope is folded by the UTC second, and the peak in the
    first 100 ms is the tick.
  - The tick contrast is measured per second, in a 10 ms window on that peak against 300-900 ms.
    The table gives the median, p10 and the share of seconds over 10 dB.
  - The envelope is used rather than a coherent demodulator, so a residual carrier offset does
    not fade the audio.
- **WWVH:** the same fold at 1100-1300 Hz. "WWVH may dominate" is printed when a WWVH band
  (2.5-15 MHz) shows a 1200 Hz tick fold of at least 6 dB, within 3 dB of the 1000 Hz one.
- **ADC:** raw peak in dBFS, and the share of samples at full scale.
  - API overload events are counted only with the overload-logging SoapySDRPlay3 build
    (`--overload-plugin DIR`). The default is wwv-capture `saga/soapysdrplay3-src/plugin-overloadlog`
    when that exists. The installed module drops the events, so without the build the column
    reads `n/a`.

**Ranking:**
- A band with clipping or an overload goes last and is never chosen.
- The rest are ranked by median tick contrast, less 6 dB where WWVH may dominate.
- 6 dB or more is "usable". The 5 MHz capture the chain was scored on had a 17.1 dB median.
- When no band is usable, the scan warns and `auto` still goes live on the best one.
- Under 30 dB-Hz a row reads "no carrier" and gives no carrier offset. Noise alone reads
  11-14 dB-Hz.

Three scans at dawn on 2026-11-20 (09:17, 09:20 and 09:27 UTC) show how fast the best band
moves:

| UTC   | 5 MHz tick median | 10 MHz tick median | Best    |
|-------|-------------------|--------------------|---------|
| 09:17 | 19.7 dB           | 1.0 dB             | 5 MHz   |
| 09:21 | 7.5 dB            | 8.0 dB             | 10 MHz  |
| 09:27 | 23.6 dB           | 11.5 dB            | 5 MHz   |

In all three scans:
- 2.5 MHz read 46-47 dB-Hz with no ticks and no sidebands. Each time its peak sat in exactly
  the bin the scan mixes to. Whether that is a carrier or an artefact is not checked.
- 15, 20 and 25 MHz had no carrier.
- No band clipped or overloaded the ADC.

On the 10 MHz choice, a 3-minute live run reached TENTATIVE sync only, with no markers. On the
5 MHz choice it locked after two markers, and symbols followed.

`auto` chooses once, at the start. To re-check during a run, the streamer has to stop for a
scan, about 3.5 minutes without input, and `wwv_live` then restarts its sync. Not built.
