# Notes: issues found while fixing the WWV detectors and building the time decoder, not fixed

Measured with `tests/air/run_air.sh` against the KiwiSDR (kb5ag, 5 MHz) and Saga RSP2 (5 MHz)
captures in `~/wwv-capture`. "plain" is carrier at DC with no AGC; "agc" adds the waterfall.c
`normalize()`.

## Detection

- **BCD symbol accuracy after the BTIME half-amplitude edges.** Symbols are 437/445, 446/450,
  426/463 and 297/324 correct on kiwi plain / kiwi agc / saga plain / saga agc. Of symbols whose
  pulse starts within 250 ms of the second, a sent 0 reads 1 in 0/292, 0/293, 2/262, 6/183 and a
  sent 1 reads 0 in 1/105, 0/108, 7/103, 0/67. `tests/air/trace_all.sh <out-dir>` gives every
  wrong symbol a cause: spurious 3/3/21/18, width 1/0/13/8, freq-avg 4/1/2/0, misaligned
  0/0/1/1. `tests/air/btime_frames` and `tests/air/btime_width_forensics.py` give the per-pulse
  view.
  - **Saga still starts events on the low-level subcarrier.** On saga plain, 8 of the 14 sent 1s
    and Ps read short, and all 4 on saga agc, follow a cooldown after an event that started before
    the second. 9 of those 12 events have a mean of 0.12-0.44 of the pulse level. Saga's pulse/low
    ratio is 2.1-2.7 at p10 (kiwi 6.8-7.8), so in a fade the low level reaches half the tracked
    pulse level. The same events make most of the 21/18 spurious windows.
  - **The detector's energy ripples with the 100 Hz phase.** Its 5.12 ms Hann-windowed FFT
    frame holds half a 100 Hz cycle, and bins 0-2 sum the two sidebands with a phase that drifts
    4.3 degrees per frame, so the plateau ripples with a period of about 214 ms (measured over the
    P plateaus: 210 ms kiwi plain, 218 ms saga plain). Within a pulse the 5th percentile is
    0.62-0.65 of the median. Not changed; the half-amplitude edges clear it.
  - **Leading edges measure at +44..+48 ms (median) for a pulse sent at +30 ms**, and widths at
    184/486/788 ms for 170/470/770 ms of high level (kiwi, p10-p90 within +-5 ms). Filter and
    frame delay; classification is unaffected.
  - **The start threshold lags an instant gain step by a few pulses.** In
    `tests/test_bcd_time_width.c`, a 12 dB step up just before :00 disturbs the next 5 seconds;
    a step down misses 2 seconds (the 2 s release). The test reports the 6 s after each step
    without checking them.
- **`bcd_freq` cannot detect per-second pulses.** It thresholds a 1 s sliding sum of 100 Hz
  energy at 3x a baseline that tracks that same sum. The subcarrier is present every second, so
  the sum barely moves. It fires only on the P-marker clusters: saga BFREQ events land at UTC
  :09.7, :19.7, :29.8 and :49.8. There are 3-15 events per 629 s run.
- **The marker detector's tone-purity test reads bin 7 (1367 Hz), next to WWVH's 1500 Hz minute
  marker, which is sent at the same :00.** On both captures WWVH is weak there (bin 8 / bin 5 at
  :00 is 0.007-0.059) and real markers score 0.01-0.06 against the 0.2 limit. At a site where
  WWVH is as strong as WWV, its marker would add flank excess and could reject WWV's. Not
  measured: no capture has that. Testing bin 3 alone (real 0.02-0.03, voice 0.13-0.30) would
  avoid it with less margin on the voice side.
  - **Correction (time-decoder branch): the premise of the entry above is wrong.** NIST SP 432
    section 3, "Standard Time Intervals": the minute pulse is 800 ms of 1000 Hz at WWV and 1200
    Hz at WWVH; 1500 Hz is the hour pulse, at both stations. So the bin-8 figure measured the
    hour tone, which is present at :00 of one minute in sixty, not WWVH's minute marker. WWVH's
    1200 Hz sits at bin 6.1 (195.3 Hz bins), between the centre (bin 5) and the flank (bin 7).
    Its effect on the purity test is not measured.
- **A purity rejection is printed to stdout only.** It reaches no CSV or telemetry line.
- **A marker-detector run shorter than `min_duration_ms` still takes the 30 s cooldown.** A
  short in-band blip in the seconds before :00 would hide that minute's marker, as the voice
  runs did before the purity test. Not seen on air.
- **The tick-path marker edge is taken from the trailing edge when a run is more than 40 ms off
  a clean marker's length.** That assumes the end is right. On air it is (runs of 614-763 and
  983 ms ended +814..+828 ms after :00). A marker that fades out before 800 ms would get an early
  edge.
- **A false tick-path minute marker at UTC 11:34:20 (983 ms), kiwi agc only,** is present at
  baseline and on main. It is not explained. The sync re-anchor now recovers from it.
- **Tick-path `MARKER_MAX_DURATION_MS` (1000 ms bail-out) contradicts
  `MARKER_MAX_DURATION_MS_CHECK` (1500 ms)** in `include/detection/tick_internal.h:35,52`. A run
  of 1000-1500 ms bails out before the marker check sees it.
- **The breakdown in the `TICK_FILTER_DELAY_MS` comment is stale** (`include/tick_detector.h:71`,
  "0.32ms Butterworth"). The sync filter's group delay at 1000 Hz is now 1.50 ms. The value
  (3.0 ms) was left alone: kiwi plain tick-path marker leading edges measure -1.8..+2.1 ms from
  :00.
- **Saga plain ticks are 42.6% matched** (saga agc 59.9%, kiwi 75-76%). The limit on the weaker
  RSP2 signal is not investigated.
- **Tone tracker window (audit F3)** is still Hann, not the Blackman-Harris it had before 591d6ee.
  Not touched.

## Sync and correlator

- `sync_detector_full_reset()` does not clear `last_confirmed_ms`, so a stale anchor survives a
  reset. The re-anchor rule mitigates it.
- The sync detector gets no per-tick events (`sync_detector_tick_event()`): neither the manager
  nor the harness ever fed them, so tick-hole evidence never arrives. Kept that way so the scored
  chain did not change.
- The BCD correlator averages BFREQ's duration with BTIME's when a window holds both. BFREQ fires
  mostly on P-marker clusters, and its durations (164-983 ms over the four runs) do not cluster
  at 200/500/800 ms. With the BTIME widths now right, the average makes 4/1/2/0 wrong symbols on
  the four runs (freq-avg in `tests/air/trace_all.sh`). BFREQ-only windows give 7 wrong symbols
  at b2ede79.
- **The correlator's P positions include second 0** (`VALID_P_POSITIONS`,
  `src/correlation/bcd_symbol_classifier.c:20`): the WWVB frame reference. WWV sends no pulse at
  second 0, so a long event there is labelled P instead of 1. Neither is a WWV symbol; the time
  decoder rejects a frame with P at second 0. Not seen on air.
- `bcd_window_get_second_for_timestamp()` no longer has a caller inside the correlator
  (`bcd_window_get_index()` replaced it). It is kept for its declared interface, and its unused
  `corr` parameter warns.
- The BCD correlator CSV header lists 14 columns and the rows have 10. `bcd_correlator_destroy()`
  still closes the last window through the old static `close_window()` (`SYM`/`CORR` format). Two
  output formats coexist (audit F2).
- `marker_event_t.accumulated_energy` / `peak_energy` carry swapped quantities since 7e7805e. The
  only reader logs them.

## Time decoder (`src/decode/wwv_time_decoder.c`)

- **The time of day now CONFIRMS on three of the four ten-minute runs, and the date on two.**
  First CONFIRMED right, in seconds after capture start: time of day 467 (kiwi plain), 587 (kiwi
  agc), 630 (saga plain), never (saga agc); date 587, 527, never, never. No false confirm. One
  frame per run decodes alone on kiwi plain, kiwi agc and saga plain, none on saga agc. Saga plain
  DECODES a wrong date once (2024-11-18, 2.5 nats) and saga agc wrong DST bits once (2.5 nats): the
  unverified state.
- **The decoder's p01 prior now overstates the error rate.** Each frame's p01 is
  (reserved ones + 1) / (reserved seconds + 4): a Beta prior of mean `WWV_TD_DEFAULT_P01` 0.25 and
  weight 4 (`src/decode/wwv_time_decoder.c:262-263`), chosen when a sent 0 read 1 in 125/493. It
  now reads 1 in 8/1030, but a frame with 15 clean reserved seconds still gets p01 = 0.05, so an
  observed 1 counts ln(0.95/0.05) = 2.9 nats where the measured rate supports about 4.8. Not
  changed.
- **The history is 60 minutes** (`WWV_TD_MAX_FRAMES`). Date bits change once a day, so a longer
  history would confirm the date sooner at the cost of memory and search time. Each minute
  searches 1440 times of day, and the 36,525 dates once plus once per minute of history (for the
  times of day at which the history would cross 00:00), under both models. The random-chain test
  runs 7200 such minutes in about 19 s.
- **DECODED is wrong sometimes, as designed**: on air, kiwi plain DUT1 -0.3 at 2.8 nats (second 57
  read 1 in both of its two looks). In the random-chain test, DECODED wrong is 981 of 7200
  minutes at and beyond the model's rates. Only CONFIRMED is a verified value.
- **The error model's p10 (sent 1 read as 0) was set on 3 events in 184**, to 0.05 (above the
  95% bound) with CONFIRMED also checked at 0.15. After the BTIME edge fix it is 8 in 383 (2.1%),
  7 of them on saga plain (6.8% there). A live run should re-measure it:
  `tests/air/decode_score.py` prints the confusion.
- **A frame opens at stream time 0.0 when a run ends.** On saga plain the last symbol carries
  `minute_start_ms` 0 and the decoder emits a `BCDS,FRAME` and `BCDS,UTC` line for a frame starting
  at 0.0, which `decode_score.py` scores as 12:10 (all NONE). Not traced.
- `tests/test_chain_synth.c` does not run the decoder: its 125 s signal holds two frames, fewer
  than CONFIRMED needs.
- The commit "Tests: WWV time decoder on clean, corrupted and WWVB-layout frames" gives two
  figures wrong in its message. The 8 false DUT1 confirms at p10 = 10% (before the fixes) were 8
  minutes in 160 chains, not 8 chains. The 61 wrong minutes were with p10 at 10% for half the
  chains and 15% for the other half, and a 0 then counted 2.5 nats, not 2.7. A hook refused the
  rewrite of that unpushed commit, so the correction is recorded here. Two more commit messages
  misread the same table. "BCD correlator: report where a window's first event starts in its
  second" and "WWV time decoder: UTC time and date from the BCD symbol stream" say mid-second-only
  windows read 0 in 141 of 161. 141 and 20 are the counts by sent bit; the readings are as in the
  Detection entry above. The decoder commit's "24-26% at every site and context" holds per site
  (kiwi 49/201, saga 76/292); by preceding symbols it is 0.24-0.30 where the counts exceed 30, and
  0.16-0.50 in the small groups.

## Interfaces changed on this branch

- `data_channel_t` gained `hp[2]`, and `sync_channel_t` is now `bp[4]` (same size). Consumers must
  rebuild.
- `marker_event_t` gained `start_timestamp_ms`, and `duration_ms` now carries the de-stretched
  length. The marker detector no longer reports runs that fail the tone-purity test.
- `sync_detector_marker_event()` and `sync_detector_tick_marker()` take the marker's leading
  edge. `tick_marker_event_t.start_timestamp_ms` comes from the trailing edge for off-length
  runs. `sync_detector_periodic_check()` now confirms a lone pending marker after 3 s. `bcd_symbol_event_t` gained `second`, and the `SYMBOL` telemetry line gained a trailing
  `second` field.
- `bcd_symbol_event_t` gained `minute_start_ms` and `pulse_offset_ms`; the `SYMBOL` line gained a
  trailing `pulse_offset_ms`. New `bcd_correlator_periodic_check()`: with it, a locked second with
  no BCD event is sent as NONE, and windows close 1100 ms after they end. A second is sent once
  when the next minute's anchor replaces the current one during it.
- New module `wwv_time_decoder` (`include/wwv_time_decoder.h`) and telemetry lines `BCDS,FRAME`
  and `BCDS,UTC`.
- `wwv_detector_manager_process_detector_sample()` now takes 50 kHz baseband before the channel
  filters (it owns the filters, the optional AGC, the BCD detectors, correlator and decoder).
  `wwv_detector_config_t` gained `enable_bcd`, `enable_time_decoder`, `enable_agc`, `verbose`;
  `output_dir` NULL writes no CSVs. New: observer struct, component getters, `wwv_chain_log`,
  `iq_frontend`, `telem_set_tap()`, telemetry line `CONS,ANCHOR`.
- `struct bcd_time_detector` (`include/detection/bcd_internal.h`) gained `pulse_energy_sum`,
  `pulse_energy_frames`, `pulse_level` and `last_valid_frame`. BTIME events now start and end at
  half the pulse amplitude (`BCD_TIME_EDGE_FRACTION`, `BCD_TIME_LEVEL_ALPHA`,
  `BCD_TIME_LEVEL_HOLD_MS` in `include/bcd_time_detector.h`), and a segment rejected as too short
  takes no cooldown. The median width of a 0 is 36-67 ms shorter than before (184-189 ms against 220-251 ms).

## Live host (`apps/wwv_live.c`, `scripts/wwv_rsp2_live.sh`)

- **`sdrplay_apiService` 3.15.2 segfaulted while systemd stopped it** (2026-11-19 17:44:59 UTC,
  Result core-dump), after the stream was closed. The unit was stopped but `failed`. An earlier
  stop that day (12:31) was clean. The wrapper now reports it and runs `systemctl reset-failed`.
  The cause is in the vendor binary and is not investigated.
- **At 17:42 UTC the whip saw no usable WWV on 5 MHz**: carrier C/N0 11-14 dB-Hz, against 54-62
  dB-Hz in the 12:09 UTC capture with the same settings. The 165 s smoke test found 0 markers and
  never left ACQUIRING. A live run needs a time of day with propagation (the scored capture was at
  dawn).
- **A raw recording of a 90-minute run does not fit on saga's root filesystem**: 2 MS/s CS16 is
  8 MB/s, 43 GB for 90 minutes, and 39 GB was free. `--record` refuses when `--secs` is given and
  the space is short; without `--secs` it only warns.
- **The live clock has no rate correction.** Detector time counts samples at the nominal rate; the
  RSP2's TCXO read 1.09 ppm slow, 5.9 ms over 90 minutes. The anchor is the least-delayed block in
  the first 10 s, so it is late by the minimum USB/API latency, which nothing measures.
- **The fine carrier correction (5.45 Hz) is a constant measured on one day.** The replay scores
  moved no more with 0 Hz than between AGC wirings, so it is not critical; tracking the carrier
  would remove it.
- **A raw 2 MS/s replay does not reproduce the harness scores exactly**: wwv_live decimates in one
  pass, the converted 50 kHz streams went through a 12 kHz int16 round trip and a GPS-fit
  resampler. Saga plain: ticks 262 vs 255, symbols 244/331 vs 257/336; agc: ticks 351 vs 358,
  symbols 245/286 vs 178/215 (07ffb51 detectors). False confirms 0 in both.
- The tone trackers send no telemetry; wwv_live runs them only for the manager's display path.
- `marker_correlator` and `tick_correlator` are disabled in both hosts; the tick correlator is
  created by the manager when enabled but never fed (it needs tick-detector internals).

## Documentation

- The cited source (NIST SP 432, https://tf.nist.gov/general/pdf/1383.pdf) is not filed in
  `docs/`; the algorithm doc cites it by URL.
- README claims and the audit's F10 items are untouched.

## Commit message corrections

- "Tests: BCD time detector pulse width on a synthetic WWV time code" says that at 07ffb51 the
  steady case's 0s end 72-132 ms after the sent end and its 1s start at +227..+229 ms. It is 9 of
  the 11 sent 0s (the other 2 within 60 ms), and 25 of the 27 late 1s start at +226..+230 ms.
