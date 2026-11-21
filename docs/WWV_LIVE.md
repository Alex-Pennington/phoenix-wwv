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

With `--anchor-in FILE`, the anchor taken at the end of the 10 s is the one rsp2_stream measured
before its output hold (see *Lost samples: the gap fill*). If the file holds no anchor, `wwv_live`
keeps its own and says so on stderr: it is late by the hold.

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
- `LAG` (live input only, console only) is printed each time the minimum arrival lag has grown
  250 ms since the last `LAG` line, once the anchor is fixed:

  ```
  LAG      14:52:10Z stream time is +1250 ms behind the system clock (+250 since the last LAG line): UTC from the sample counter is late by that
  ```

  Samples are missing from the counter, or the input is backed up, so UTC read off the sample
  counter is late by about that much. rsp2_stream's `behind wall clock` line says which (see
  *Run length, stalls and reopen*).
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

1. It starts `sdrplay.service`, once for the whole run.
2. With `--wwv-khz auto`, it runs the band scan below under that same start, bounded by
   `--scan-timeout`, and takes the band the scan chooses (see *Band scan bound and retry*).
3. It streams the RSP2 through `scripts/rsp2_stream.py` into `wwv_live`. The settings are those
   of the scored capture, at the chosen band: antenna A, tuner 100 kHz below WWV, AGC off,
   2 MS/s, scale 32, and the band's gain.
4. With `auto`, a band that has not reached LOCKED `--lock-mins` after it went live is
   switched (see *Band switch* below).
5. A stream that stalls, hangs, or loses the vendor service under it is reopened on the same
   band (see *Run length, stalls and reopen*).
6. It ends at `--secs` by the wall clock, and stops the service on every exit path. If the
   vendor service crashed on stop, the script also clears the unit's failed state. A guard
   process covers the exits no trap sees.

`--record PREFIX` also writes the raw stream, at 8 MB/s. Example, 90 minutes at 5 MHz:

```
scripts/wwv_rsp2_live.sh --secs 5400 --log run.log --timebase run.json
```

### Run length, stalls and reopen

```
scripts/wwv_rsp2_live.sh [--secs N] [--stall-secs 10] [--reopen-max 20] ...
```

- **`--secs N` is wall clock.** The run ends N seconds after it starts, however many samples
  came. With `--wwv-khz auto` the clock starts with the first scan, and band-switch re-scans
  and reopens count toward it. A re-scan still running at `--secs` is stopped. rsp2_stream
  is given the time left as its own `--secs`, so it usually ends a moment before the wrapper
  checks. `--secs 0` (the default) runs until Ctrl-C.
- **`--stall-secs N`** (default 10; 0: never). rsp2_stream ends with exit status 75 when no
  sample has come for N seconds. That happens when the API service died or restarted under
  the stream, or the USB transfers stopped. It logs a timeout once when a gap starts and once
  when it ends.
- **Reopen.** The wrapper stops the stream and `wwv_live` and opens them again on the same
  band as a new segment. `wwv_live`'s sample-counter clock cannot carry across a gap, so its
  sync and decoder start again. The wrapper reopens when:
  - rsp2_stream exits 75;
  - rsp2_stream exits 76 (a loss over `--gap-max-secs`; see *Lost samples: the gap fill*);
  - `sdrplay.service`'s MainPID or NRestarts changes (systemd restarted the vendor service
    under the stream);
  - rsp2_stream's heartbeat file is older than 2 × `--stall-secs` + 15 s (a stream hung
    inside `readStream`).

  Each reopen is logged on stderr, and again in the history at the end:

  ```
  [wwv_rsp2_live] 2026-11-20T22:29:26Z REOPEN: segment 1: sdrplay.service restarted under the stream (MainPID NRestarts: 19552 0 -> 49898 1)
  [wwv_rsp2_live] 2026-11-20T22:29:26Z REOPEN: opening the stream again on 10000 kHz as segment 2 (reopen 1 of at most 5)
  ```

  The reasons read `rsp2_stream had no samples for N s`, `rsp2_stream lost more samples than it
  fills (its GAP line says how much)` (exit status 76) and `rsp2_stream heartbeat is N s old:
  the stream is hung`. The `--lock-mins` timer starts again in a reopened segment. A reopened
  segment's files take the `.segK` names, as after a band switch.
- **`--reopen-max N`** (default 20). The reopen that would be number N + 1 ends the run with exit status 4
  (`REOPEN: N+1 reopens, more than --reopen-max N; ending the run`). The vendor service can crash several times within seconds (three reopens at 2026-11-21 02:55 UTC), so 5 was too few.
- **Bounded stops.** Every stop is SIGTERM, then SIGKILL if the process has not exited in 10 s
  (rsp2_stream, the band scan), 5 s (`wwv_live`) or 2 s (the output filter). A kill is
  logged: `rsp2_stream did not stop in 10 s after SIGTERM; killing it`.
- **`behind wall clock`.** Every `--report-secs` (60 s) rsp2_stream logs how far its sample
  count (fills included) is behind the wall clock since streaming began, how much it filled,
  and how long its writes to stdout blocked:

  ```
  [rsp2_stream] behind wall clock +1250 ms (+300 in 60 s), filled 0 ms in 0 gaps, write wait 4 ms (max 1), 0 overflows, 0 timeouts; samples lost upstream (USB or API service), no overflow reported
  ```

  When "behind" grows by more than 100 ms in an interval, the line names the cause. With
  little write wait, it reads `samples lost upstream (USB or API service), no overflow
  reported`. With a long write wait, it reads `the reader is not keeping up`.
  The last line gives the figure at the end. Read it with `wwv_live`'s `LAG` lines: a `LAG`
  with upstream loss here means UTC from the sample counter is late by the samples lost.
- **Kill guard.** At start the wrapper starts a guard process in its own session, so a signal
  to the wrapper's process group does not reach it. It does nothing while the wrapper runs.
  If the wrapper exits without its cleanup (SIGKILL, a session that dies with its shell):
  1. The guard sends SIGTERM to the processes the wrapper recorded, by PID, never by process
     group, which the caller may share. A PID counts only if its command line is still
     rsp2_stream, `wwv_live`, the band scan or the output filter.
  2. It sends SIGKILL to any still running after 10 s.
  3. It stops `sdrplay.service` and clears a failed state.

  It appends what it did to the wrapper's stderr, as `[wwv_rsp2_live guard]` lines ending
  with the unit's state. The wrapper's own cleanup stops the guard last.

### Memory compaction

Before it starts `sdrplay.service`, and before each reopen, the wrapper asks the kernel to
compact memory (`echo 1 > /proc/sys/vm/compact_memory` under sudo). On saga the API service
crashed on libusb `errno=12` (ENOMEM) during isochronous transfer submission, with
`usbfs_memory_mb` already 0, while free high-order pages were scarce. Four crashes in
01:37-05:38 UTC on 2026-11-21. It logs `compacted memory before opening the RSP2`, or
`memory compaction failed (continuing)`. `SDRPLAY_COMPACT=0` skips it.

### Lost samples: the gap fill

SoapySDRPlay3 0.5.3 passes on no sign of lost samples (it drops the API's
`firstSampleNum` and reset flag and never sets `timeNs`), so without help a loss
upstream shortens the stream and every later sample is early by what was lost. rsp2_stream
detects losses from arrival time: arrival (CLOCK_MONOTONIC) minus stream time has a floor,
and a loss raises it by exactly the samples lost. It fills each loss with zeros, so a
sample's index stays its true time since the start. Details and margins are in
`scripts/rsp2_stream.py`'s header.

- **Hold (`--hold-secs`, default 3).** rsp2_stream's output runs that many seconds behind the
  radio, so a fill lands where the loss was. The least is 2.5 s: a smaller value is refused
  unless `--no-fill` is given. The wrapper passes `--anchor-out FILE` to rsp2_stream and
  `--anchor-in FILE` to `wwv_live` on every segment. rsp2_stream writes the file (JSON:
  `source`, `in_sample0_unix`, `hold_s`, `anchor`) before the first sample past 10 s leaves. `wwv_live` reads it when
  its own 10 s refinement ends, and takes the anchor measured before the hold. Its arrival lag
  then leaves the hold out:

  ```
  ANCHOR   input sample 0 = 2026-11-21T01:37:10.400733Z: rsp2_stream: least-delayed block over the first 10 s, before its 3.000 s hold line
  ```

- **Fills.** Each one is logged by rsp2_stream, then by `wwv_live`, which takes a run of
  exact zeros (0.5 ms or more) out as a gap. The manager holds every detector event that
  overlaps the span back from sync, the correlator and the decoder, so those seconds become
  erasures. rsp2_stream logs on stderr:

  ```
  [rsp2_stream] GAP <UTC> at stream S s (sample N): X ms filled with zeros; cause: ...
  ```

  The cause is `overflow (SOAPY_SDR_OVERFLOW)` or `timeout (K readStream timeouts)` when one came
  with the loss, else `upstream, no report (USB or API service)`, or `remainder over 30 s ...` for
  the slow stage. `--gap-log FILE` appends each fill to FILE as one JSON line (`utc`, `stream_s`,
  `sample`, `samples`, `ms`, `cause`).

  `wwv_live` prints on the console:

  ```
  GAP      <UTC> stream S s: X ms of samples lost upstream (filled); events there are not evidence
  GAPEND   stream S s, X ms: held back tick N, tick-marker N, marker N, candidate N, BTIME N, BFREQ N
  ```

  `GAPEND` comes 6.5 s of stream time after the span's end (`WWV_GAP_SETTLE_MS` 1.5 s plus 5 s for the
  slowest event report), or earlier when the span table is full or the input ends. The counts are
  the events held back since the last `GAPEND`, so spans that overlap in time share them. With
  `-l`, the chain log carries the same two events in its own form:

  ```
  GAP start=<ms> len=<ms>
  GAPEND start=<ms> len=<ms> tick= tmark= mark= cand= btime= bfreq=
  ```

  At the end of the run `wwv_live` closes every open span and, when there was a fill, prints
  `GAPS     N filled gaps, X ms in all`. `start` is a detector time in ms.
  `tests/air/replay_events` honours them (`GAPDROPS` at the end equals the sum of the `GAPEND`
  counts).
- **What normal looks like.** No fills with cause `upstream, no report`. `behind wall
  clock` stays within about ±30 ms with `filled 0 ms in 0 gaps`. A few-ms `remainder over
  30 s` fill now and then is the radio's clock against the host's. On 2026-11-21 01:37 UTC
  (5 MHz) the first 6 minutes showed 0 fills and `behind` between −20 and +1 ms.
- **Too big (`--gap-max-secs`, default 10).** A loss over that ends the stream with exit
  status 76, and the wrapper reopens it as a new segment (`REOPEN: ... rsp2_stream lost more
  samples than it fills`). rsp2_stream logs `GAP too large to fill: ...; ending the stream
  (exit 76)`. What it holds from after the loss is not sent; what it holds from before is.
- **`--no-fill`** (rsp2_stream) keeps the old behaviour: no hold and no fill. The wrapper has
  no option for it.
- With `--record PREFIX`, `PREFIX.json` lists the fills (`gaps`), `hold_s`, and `samples`
  against `samples_radio`. `tests/live/stream_fake_soapy.py` replays a recording's arrival
  times through the fill offline.

Exit status: 0 at the end of the run, 2 for bad options or a missing `wwv_live`, 3 when the
first `auto` scan finds no band or a scan (first or re-scan) fails again after the restart, 4
after `--reopen-max`, 130/143 on SIGINT/SIGTERM, else `wwv_live`'s or rsp2_stream's own.

`tests/live/run_stub_tests.sh` runs the wrapper and rsp2_stream.py against stubs, with no
radio and no `sdrplay.service`: the ctest test `live_wrapper_stubs` (`ctest -L live`, about
TIMEX). The stubs in `tests/live/fakebin` stand in for `systemctl` and `sudo`, and the
script refuses to run unless the PATH reaches them. Its cases cover each reopen reason
(including the 76 exit), `--reopen-max`, a stream deaf to SIGTERM, the kill guard, the `auto`
scan going live and ending at `--secs`, a band switch with `--avoid`, `--secs` during a re-scan,
a scan that finds no band, a stuck scan and a failing API (the bound, the restart and the
retry), rsp2_stream's stall exit and `behind wall clock` line, its gap fill (losses, an
outage, a slow ramp, a loss too large), `wwv_live`'s handling of a fill on synthetic WWV, and
the scan's `--keep-raw` round trip.

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
  15 MHz was checked on WWV on 2026-11-20 at 13:03-13:06 UTC:

  | LNA | C/N0       | peak       | score |
  |-----|------------|------------|-------|
  | 0   | 64.1 dB-Hz | -10.6 dBFS | 100%  |
  | 2   | 64-65 dB-Hz | -24 dBFS  | 100%  |
  | 4   | 52.4 dB-Hz | -32.6 dBFS | 8%    |

  So LNA 2 loses nothing against LNA 0 and keeps 14 dB more headroom, while LNA 4 is
  receiver-noise limited. 2.5, 20 and 25 MHz are still starting points, because no WWV signal
  was seen on them. 20 MHz peaks at -6 to -10 dBFS at LNA 1 with no carrier, the most of any
  band, and has not clipped. The scan's ADC columns say whether the values hold. `--gain KHZ:LNA:IFGR` (repeatable) overrides a band in the scan and in the live
  run, and `--lna` / `--ifgr` override the live band.

### Band scan bound and retry

With `--wwv-khz auto` the wrapper runs `wwv_band_scan.sh --no-service` in the background, so a
signal reaches its traps at once. Options (all `auto` only):

- **`--scan-secs N`** (default 25): seconds per band.
- **`--scan-json FILE`**: keep the scan's figures (default: a temporary file).
- **`--scan-timeout N`**: stop a scan that runs past N seconds of wall clock. The default is
  bands × (`--scan-secs` + 15) + bands × (35 + 15), the second term for a 35 s extension of
  every band: 240 + 300 = 540 s at the defaults.

A scan fails when it runs past the bound (`SCAN: the band scan ran past its N s bound; stopping
it`) or exits 5 (the sdrplay API failing, from `wwv_band_scan.py`). Past the bound, the wrapper
stops the scan (SIGTERM, then SIGKILL after 10 s) and kills (SIGKILL) any of its child processes
still running. On either failure it stops `sdrplay.service`, starts it again (memory compacted
first), and scans once more (`SCAN: restarting sdrplay.service and scanning once more`). If that fails too, the run logs `not going
live` and ends with exit status 3. That holds for the first scan and for a band switch's re-scan
(the history then reads `re-scan for segment K: <reason>`). A scan that ends without failing but
finds no band logs `the band scan found no band (exit N)`. At the first scan that is exit status
3 too. At a re-scan the run stays on its band. The retry is not made once `--secs` has run out.

### Band scan

```
scripts/wwv_band_scan.sh [--secs 25] [--extend-secs 35] [--avoid KHZ] [--bands 2500,5000,...]
                         [--gain KHZ:LNA:IFGR] [--json FILE] [--keep-raw DIR]
```

It starts `sdrplay.service` once and stops it on every exit path. It opens the RSP2 once and
reads each band for `--secs`, about 3.5 minutes for all six, plus `--extend-secs` for each band
in a close call. It then prints a ranked table, logs the reason for its choice on stderr, and
the last stdout line is `BEST <kHz>`. The JSON keeps every per-second tick and BCD contrast.
`scripts/wwv_band_scan.py --replay PREFIX:KHZ` measures recordings instead and opens no device;
an extension reads the next seconds of the recording.

`--keep-raw DIR` writes each band's raw I/Q, extension included, as
`DIR/scan_<UTC>_<kHz>.cs16`, `.timelog.npy` and `.json`: the recording format `--replay`
reads. That is 8 MB/s, 200 MB per band at 25 s, about 1.2 GB for a six-band scan.
`--replay DIR/scan_<UTC>_<kHz>:<kHz>` with the same `--secs` measures them again and gives
the same figures (the `scan_keep_raw` stub test checks that). Scans without it keep only the
JSON, which carries no 100 Hz figures from before this score existed.

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
- **BCD:** the 80-120 Hz energy of the same envelope, the 100 Hz time-code subcarrier.
  - Every second but :00 sends the pulse at full level from 30 ms to 230, 530 or 830 ms, then
    the low level.
  - The BCD contrast of a second is the mean energy in 60-200 ms (inside every pulse, past
    the filter's rise) over 850-980 ms (the low level in every second), in dB.
  - The windows follow the tick fold peak when that fold is 6 dB or more; host latency puts
    it 13-16 ms late.
  - `bcd_fold` is the same ratio over all seconds. The table gives the per-second median, p10
    and count.
- **WWVH:** the same fold at 1100-1300 Hz. "WWVH may dominate" is printed when a WWVH band
  (2.5-15 MHz) shows a 1200 Hz tick fold of at least 6 dB, within 3 dB of the 1000 Hz one.
  - The sidebands do not show WWVH. In the saga capture (25 windows, 12:09-12:20 UTC),
    sb1200 reads 17-24 dB in odd minutes and about 4 dB in even ones. sb1000 reaches 31-35 dB
    in the even windows.
  - That follows WWV's own 600 Hz (odd minutes) and 500 Hz (even) tones, at twice their
    frequency. So `sb1200 >= sb1000` gives the minute's parity, and the scan does not use it.
  - The WWVH tick fold also takes the WWV tick's spectrum: on that capture it reads
    5.5-15.7 dB beside WWV tick folds of 15-28 dB. Only a 1200 Hz fold close to the
    1000 Hz one counts.
- **ADC:** raw peak in dBFS, and the share of samples at full scale.
  - API overload events are counted only with the overload-logging SoapySDRPlay3 build
    (`--overload-plugin DIR`). The default is wwv-capture `saga/soapysdrplay3-src/plugin-overloadlog`
    when that exists. The installed module drops the events, so without the build the column
    reads `n/a`.

**Scores.**
- **Tick score:** the share of seconds whose tick contrast is over 10 dB, in percent: the
  `score` column. Where WWVH may dominate, a second must clear 16 dB instead.
- **BCD score:** the share of seconds whose BCD contrast is over 10 dB: the `BCD` column.
- A band with clipping or an overload scores -1 on both and is never chosen.
- Under 30 dB-Hz a row reads "no carrier" and gives no carrier offset; noise alone reads
  11-14 dB-Hz.

**Choice.**
- A tick score of 40% or more is tick-usable. A BCD score of 50% or more is BCD-usable: the
  median second clears 10 dB. A band that is both is usable.
- The scan takes the first of these tiers that has a band in it:
  1. the usable bands, ranked by BCD score;
  2. the tick-usable bands, ranked by BCD score (noted "BCD not usable");
  3. every measured band, ranked by tick score, then median tick contrast ("not usable").
- Within the tier it takes the lowest band whose score is above 0 and within the margin of
  the best: 20 points of BCD score in tiers 1-2, 30 points of tick score in tier 3. A higher
  band wins only by more than the margin.
- When the tier's best score is 0, the ranking order decides.
- When two or more bands of the tier score above 0 within its margin of the best, each is
  read `--extend-secs` (35 s) more, and the choice is made again on all their seconds,
  about 57 per band.
- When no band is usable, the scan warns and `auto` still goes live on its choice.
- `--avoid KHZ` drops a band while another band of the tier is in the running. The band
  switch uses it.

**Why the tick thresholds.** Lock and the 25 s score, from the 2026-11-19 and 2026-11-20 runs:

| Recording or scan                       | Band   | Score, 25 s windows        | Live or replayed outcome                   |
|-----------------------------------------|--------|----------------------------|--------------------------------------------|
| scored capture, 11-19 12:09-12:20        | 5 MHz  | 57-100%, mean 86%, SD 13   | LOCKED at 2.5 min, held to the end, 10 markers |
| smoke recordings, 11-19 17:42 and 17:51 | 5 MHz  | 0% in all 12 windows       | 165 s each, never left ACQUIRING           |
| scan 11-20 09:17, then run90c           | 5 MHz  | 100%                       | run90c LOCKED until its 09:17 end          |
| scan 11-20 09:21, smoke                 | 10 MHz | 13% (5 MHz: 29%)           | 3 min: TENTATIVE, no marker                |
| scan 11-20 09:27, smoke2                | 5 MHz  | 100% (10 MHz: 57%)         | LOCKED at 2.8 min                          |
| scan 11-20 11:08, run90d                | 5 MHz  | 43% (10 MHz: 9%)           | LOCKED at 2.1 min, held                    |

- **10 dB:** the column the scan already printed. It splits the bands that locked (43% or
  more) from the ones that did not (13% or less), where the median did not: 7.5 dB against
  8.0 dB at 09:21.
- **40% usable:** under the lowest score that went on to lock (43%, 11:08), and above the
  highest that did not (13%). No band between 13% and 43% has a recorded outcome yet, so
  where inside that gap lock stops is not measured.
- **30-point margin:** on that steady band, one 25 s window differs from the next by 18
  points SD (13 × √2), and by up to 35. A margin smaller than that would choose on noise.
- **Extension:** 25 s gives about 23 seconds per band, so a score's sampling error is up to
  10 points. Pooling 60 s brings it under 7, and only close calls pay for it.

Under the median ranking, 09:21 chose 10 MHz. On the same 25 s figures the tick-score ranking
chooses 5 MHz (29% against 13%). It would first read both bands 35 s more, and that read was
not taken.

The same three dawn scans show how fast the best band moves:

| UTC   | 5 MHz median, score | 10 MHz median, score | Median ranking | Tick-score ranking |
|-------|---------------------|----------------------|------------|------------|
| 09:17 | 19.7 dB, 100%       | 1.0 dB, 0%           | 5 MHz      | 5 MHz      |
| 09:21 | 7.5 dB, 29%         | 8.0 dB, 13%          | 10 MHz     | 5 MHz      |
| 09:27 | 23.6 dB, 100%       | 11.5 dB, 57%         | 5 MHz      | 5 MHz      |

In all three scans:
- 2.5 MHz read 46-47 dB-Hz with no ticks and no sidebands. Each time its peak sat in exactly
  the bin the scan mixes to. Whether that is a carrier or an artefact is not checked.
- 15, 20 and 25 MHz had no carrier.
- No band clipped or overloaded the ADC.

**Why the BCD thresholds.** The time of day is decoded from the BCD pulses. On 2026-11-20
the tick ranking put 10 MHz, then 15 MHz, first (run90g). 15 MHz locked, but its symbols
were far worse than 5 MHz's.

Two measures of the 100 Hz contrast are on record:
- **Raw I/Q, the scan's own measure.** The saga recordings, re-measured in 25 s windows with
  the scan's code.
- **`wwv_live --frames`.** The BTIME detector's energy every 5.12 ms, folded by UTC second
  with the same `bcd_contrasts()`, from the live runs that kept frames.

The detector's energy is a 50 Hz bucket of a 256-point FFT at 50 kHz (195 Hz bins), so it
admits more noise and its contrast is compressed. On the saga capture, replayed through
`wwv_live --frames`, the per-second median is 7.4 dB in the frames and 21.4 dB with the
scan's measure.

The frames are put on the scan's scale with one model: contrast = (x+1)/(x/R+1), where x is
the pulse-to-noise ratio and R the transmitted pulse-to-low ratio. The frames see x lower by
a fixed factor, 15.5 dB at R = 30 dB, fitted on the saga medians. R between 26 and 40 dB
moves the mapped window medians by up to 1.5 dB.

The model holds for medians, not for the tails. Mapped saga frames give a p10-p90 of
17.7-23.2 dB, where the scan measures 12.6-25.8 dB. So the per-25 s-window median is the
figure compared.

| Source (UTC)                            | Band   | Tick score | BCD, scan scale, window median | Usable windows at 10 dB / 50% | Outcome |
|-----------------------------------------|--------|------------|--------------------------------|-------------------------------|---------|
| scored capture, raw, 11-19 12:09-12:20   | 5 MHz  | 56-100%    | 14.5-25.4 dB; BCD score 71-100%, mean 94% | 25 of 25 | LOCKED at 2.5 min, decoded |
| chk30, raw, 11-19 12:07                  | 5 MHz  | 65%        | 16.8 dB; BCD score 88%         | 1 of 1                        | (scan only) |
| chk30, raw, 11-19 12:08                  | 10 MHz | 0%         | -0.2 dB; BCD score 0%          | 0 of 1                        | (scan only) |
| smoke, raw, 11-19 17:42 and 17:51        | 5 MHz  | 0%         | -0.5 to 0.6 dB; BCD score 0%   | 0 of 12                       | never left ACQUIRING |
| run90f frames, 11-20 13:18-13:48         | 5 MHz  | 50% (scan) | p10 9.3, median 14.1 dB        | 60 of 71 (85%)                | 1->0 27.5%, P->1 13.2%, mid 10.2%, erase 6.6% |
| run90g seg 1 frames, 13:51-13:59         | 10 MHz | 0% (scan)  | p10 6.2, median 8.3 dB         | 7 of 19 (37%)                 | never LOCKED in 8 min |
| run90g seg 2 frames, 14:02-14:54         | 15 MHz | 30% (scan) | median 5.4, p90 14.1 dB        | 39 of 124 (31%)               | 1->0 48.6%, P->1 35.0%, mid 37.0%, erase 27.7%; date never right |

The symbol rates are from `tests/air/symbol_rates.py` on each run's log.

Noise alone (smoke, the 12:08 10 MHz window: 306 seconds) gives per-second BCD contrasts
with a median of -0.2 dB, a p99 of 5.6 dB and a maximum of 8.0 dB. None is over 10 dB.

- **10 dB per second, 50% usable.** Only the median transfers between the two measures, so
  "usable" is set where the median second clears the threshold. At 10 dB:
  - 85% of run90f's windows pass, against 31% of 15 MHz's and 37% of 10 MHz's;
  - every window of the scored capture passes;
  - no noise second reaches it.

  The other thresholds, as the share of windows that pass:

  | Threshold   | run90f 5 MHz | 10 MHz seg 1 | 15 MHz seg 2 |
  |-------------|--------------|--------------|--------------|
  | 8 dB        | 97%          | 58%          | 40%          |
  | 12 dB       | 62%          | 11%          | 19%          |

  8 dB lets more than half of the 10 MHz windows through. 12 dB drops 38% of run90f's
  windows. 10 dB is also the tick threshold.
- **20-point margin.** On the scored capture, one 25 s window's BCD score differs from the
  next by 8.8 points SD, and by up to 21. At a score near 50% over 23 seconds, the binomial
  SD is 10 points, or 15 points for the difference of two bands. A margin of 20 points
  covers the largest step measured. The tick margin stays 30 points, where its own steps
  were larger.
- **What a 25 s window cannot show.** 15 MHz in run90g faded in and out: 31% of its windows
  would pass. A scan that reads such a band in a good window still chooses it. The band
  switch's `--lock-mins` is the backstop. No raw I/Q of run90g's scan windows was kept: the
  scan JSONs from before this change carry no 100 Hz figure, so its 14:01 window cannot be
  scored. `--keep-raw` keeps that evidence from now on.
- **run90f's own date.** Live, run90f decoded the date wrong: 10 minutes read 2026-05-04
  and one read 2026-11-20. It stands as the better band here on its symbol rates against
  15 MHz's. run90c and run90d (5 MHz, date CONFIRMED in 69 and 61 minutes) kept neither
  frames nor raw I/Q, so their BCD contrast is not measured.

### Band switch

With `--wwv-khz auto`, the wrapper watches `wwv_live`'s `SYNC` lines. When the band has not
reached LOCKED `--lock-mins` minutes (default 8; 0 turns it off) after it went live, the wrapper:

1. stops the streamer and `wwv_live`, whose sync and decoder state start again;
2. re-scans with `--avoid` on that band, under the same service start;
3. goes live again on the scan's choice, as a new segment. If no other band is in the
   running, it stays on the same band and restarts the timer.
   If the re-scan fails (see *Band scan bound and retry*), the run ends with exit status 3.

Once a band has locked it is kept, even if it loses lock later.

Every switch is logged on stderr with the UTC time, and the run ends with one line per
segment: band, length, and whether it locked. A fixed `--wwv-khz` never switches.

Segment 1 writes `--log`, `--timebase`, `--scan-json`, `--record` and `wwv_live --frames` under
the names given. Segment K writes them with `.segK` before the extension: `run.log` becomes
`run.seg2.log`, and `PREFIX` becomes `PREFIX.seg2`. `--secs` counts from the start of the
first scan, so the scans count toward it. `--carrier-hz`, `--lna` and `--ifgr` hold for the first band only.

**On the radio, 2026-11-20 12:53-13:03 UTC** (`--lock-mins 3 --secs 300`): the scan chose
5 MHz at 48%. It reached TENTATIVE with one marker, and at 3 minutes the wrapper logged
`SWITCH`. The re-scan, avoiding 5 MHz, found 15 MHz at 100% (it had no carrier at 12:55)
and logged `SWITCH: 5000 kHz -> 15000 kHz`. `--secs` ran out during that re-scan.

**Why 8 minutes.** Time to the first LOCKED on 5 MHz:

| Run                | Start (UTC) | First LOCKED |
|--------------------|-------------|--------------|
| scored capture     | 11-19 12:09 | 2.5 min      |
| smoke2             | 11-20 09:30 | 2.8 min      |
| run90 (night)      | 11-20 00:30 | 5.0 min      |
| run90c (dawn)      | 11-20 07:47 | 6.9 min      |

run90c then held lock for 83 minutes. A 5-minute limit would have switched away from it.
Lock needs two minute markers, so it comes just after a minute boundary; 8 minutes covers
the slowest observed first lock, with one more marker to spare. run90's re-acquisitions after
losing lock took 2.5-15.5 min, but the switch does not act once a band has locked.

On 2026-11-20, 5-minute live runs did not lock on a good band either:
- 5 MHz from 12:48, scoring 70%;
- 15 MHz from 13:07, scoring 100%.

Each reached TENTATIVE and caught two markers 2-3 minutes apart, missing the ones between.
A tick score shows the band carries ticks, but lock waits on the minute markers. That is why
the limit is not shorter.
