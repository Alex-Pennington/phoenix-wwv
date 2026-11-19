# Notes: issues found while fixing the WWV detectors and building the time decoder, not fixed

Measured with `tests/air/run_air.sh` against the KiwiSDR (kb5ag, 5 MHz) and Saga RSP2 (5 MHz)
captures in `~/wwv-capture`. "plain" is carrier at DC with no AGC; "agc" adds the waterfall.c
`normalize()`.

## Detection

- **BCD symbol accuracy is now limited by the BTIME detector, not the anchor or the
  correlator's window logic.** Symbols are 73-79% correct (70/96, 229/291, 257/336, 178/215),
  and every symbol reports the right second-in-minute. `tests/air/trace_all.sh <out-dir>` gives
  every wrong symbol a cause. What remains, on kiwi plain / kiwi agc / saga plain / saga agc:
  - width 10/41/55/27: the window holds the pulse at the second, and the detector measured its
    width wrong. The class medians are right (0: 220-250 ms, 1: 497-532 ms, P: 788-876 ms) and
    the 350/650 ms cuts sit between them, but each class has a wide tail (0 p90 425-502 ms).
    That is detector width noise, not a threshold error.
  - spurious 16/21/24/10: no BTIME event starts near the second. The window classifies a
    mid-second event, and its reading barely depends on the bit: a sent 0 reads 0 in 100 of 141
    such windows, a sent 1 in 13 of 20 (all four runs). Re-measured on the
    corrected anchor: NONE for these windows takes correct to 55/193/219/153 and wrong to
    9/41/55/27, removing exactly the right-by-luck and spurious symbols. Not adopted in the
    correlator; the symbol event now carries `pulse_offset_ms` and the time decoder treats these
    windows as erasures.
  - misaligned 0/0/0/0.
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
  at 200/500/800 ms. No air window is wrong because of it today (freq-avg 0 on all runs). BFREQ-only windows
  give 7 wrong symbols at b2ede79.
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

- **The ten-minute captures do not hold enough evidence to confirm a time.** A sent 0 is read as
  1 in 125 of 493 symbols at the second (24-26% at both sites), so each bit needs several looks.
  On air the time of day reaches DECODED on kiwi agc and saga plain (margins 2.7-2.9 nats) and
  the date at most 1.2 nats, against 9.2 for CONFIRMED. Simulated at the measured rates with
  half the seconds usable (saga plain and kiwi agc give 27 of 53 per frame), the time of day
  CONFIRMS after a median 27 minutes (90% by 39) and the date after 38 (90% by 56); with 43%
  usable (saga agc), 30/45 and 45 minutes, 15% not within 60; with 13% (kiwi plain), 186 of 200
  chains never within 60. No simulated chain CONFIRMED wrong.
- **The history is 60 minutes** (`WWV_TD_MAX_FRAMES`). Date bits change once a day, so a longer
  history would confirm the date sooner at the cost of memory and search time. Each minute
  searches 1440 times of day, and the 36,525 dates once plus once per minute of history (for the
  times of day at which the history would cross 00:00), under both models. The random-chain test
  runs 7200 such minutes in about 19 s.
- **DECODED is wrong sometimes, as designed**: on air, kiwi plain DUT1 -0.3 at 2.8 nats (second 57
  read 1 in both of its two looks). In the random-chain test, DECODED wrong is 981 of 7200
  minutes at and beyond the model's rates. Only CONFIRMED is a verified value.
- **The error model's p10 (sent 1 read as 0) rests on 3 events in 184**, set to 0.05 (above the
  95% bound) with CONFIRMED also checked at 0.15. A live run should re-measure it:
  `tests/air/decode_score.py` prints the confusion.
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
