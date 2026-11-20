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

## Saga RSP2

`scripts/wwv_rsp2_live.sh` drives the run. Its sequence:

1. It starts `sdrplay.service`.
2. It streams the RSP2 through `scripts/rsp2_stream.py` into `wwv_live`. The settings are those
   of the scored capture: antenna A, tuner 4900 kHz for WWV 5000 kHz, AGC off, LNA state 2,
   IFGR 59, 2 MS/s, scale 32.
3. It stops the service on every exit path. If the vendor service crashed on stop, the script
   also clears the unit's failed state.

`--record PREFIX` also writes the raw stream, at 8 MB/s. Example, 90 minutes:

```
scripts/wwv_rsp2_live.sh --secs 5400 --log run.log --timebase run.json
```

5 MHz on the indoor whip needs propagation. At 17:42 UTC on 2026-11-19 the carrier was not
usable (NOTES.md).
