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

- **`src/manager/detector_routing.c` does not wire sync the way the harness does.** It passes
  the tick-path marker's `timestamp_ms` (trailing edge, +0.8 s) to
  `sync_detector_tick_marker()` and never feeds marker-detector events to sync. The sync header
  now says both entry points take the leading edge. waterfall.c (outside this repo) must pass
  `marker_event_t.start_timestamp_ms` to `sync_detector_marker_event()`, as the harness now does.
  Neither the manager nor waterfall.c calls `bcd_correlator_periodic_check()` or creates a
  `wwv_time_decoder`; without the periodic call there are no NONE erasures. The harness is the
  only host that runs the decoder.
- `sync_detector_full_reset()` does not clear `last_confirmed_ms`, so a stale anchor survives a
  reset. The re-anchor rule mitigates it.
- The sync detector gets no per-tick events in the waterfall.c wiring, so tick-hole evidence never
  arrives.
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

- **CONFIRMED now needs 10-12 minutes on a measured-rate signal, and the ten-minute captures
  no longer CONFIRM the time of day or date.** The error model is the measured one (nominal
  p01 0.01, p10 0.03; pessimistic 0.07 / 0.15; prior weight 50) and the CONFIRMED margin is
  ln 10^6 (`include/wwv_time_decoder.h`). In `tests/sim_wwv_time_decoder.c` the old gate (ln
  10^4) confirmed wrong in 7 of 5000 chains at three times the worst run's rates, and in 1 of
  5000 at twice; the ctest Monte Carlo's 160 chains never showed it. ln 10^6 gives 0 in 15000
  chains at 3x, and at the measured rates time of day CONFIRMS at median 11 / p90 13 minutes
  (old: 8 / 10). On air (7-9 frames per run) only the leap second warning CONFIRMS (kiwi, 630
  s); the time of day reaches 24.5 nats nominal on kiwi plain but not ln 10^6 under the
  pessimistic model. A longer live run is the measurement that matters.
- **Wrong DECODED values on air, both from one look**: saga plain DECODES 2024-11-18 at 12:15
  (second 5, year units bit 2, sent 1 read 0 in its only boundary look) and saga agc DST bits
  10 at 12:13 (second 55 sent 0 read 1, its only look), at 3.5 nats (2.5 before the model
  change). DECODED is the unverified state; on air, kiwi plain DUT1 -0.3 at 2.8 nats was
  another such case before the BTIME fix. In the random-chain test, DECODED wrong is 609 of 7200
  minutes at the worst run's rates and three times them.
- **The pessimistic p01 does not adapt down.** A frame's p01 is the posterior mean of its
  reserved bits under a Beta prior (weight 50), and the pessimistic model never goes below its
  prior mean 0.07. On air the reserved bits read 1 once in 37 frames, so the estimate barely
  moves; a fade that raises the 0 -> 1 rate only in data seconds is not seen by it.
- **`confirm_looks` (the margin on K consecutive minutes) is kept at 1.** K = 2 and 3 cut the
  3x false confirms at ln 10^4 from 11 to 7 and 4, at ln 10^5 to 1, for about a minute each. The
  remaining cases are one bit read wrong in most of its looks (a 1 read 0 in 9 of 11 at a 20%
  rate), which persists over minutes, so consecutive looks do not remove them.
- **The history is 60 minutes** (`WWV_TD_MAX_FRAMES`). Date bits change once a day, so a longer
  history would confirm the date sooner at the cost of memory and search time. Each minute
  searches 1440 times of day, and the 36,525 dates once plus once per minute of history (for the
  times of day at which the history would cross 00:00), under both models. The random-chain test
  runs 7200 such minutes in about 19 s.
- **DECODED is wrong sometimes, as designed**: on air, kiwi plain DUT1 -0.3 at 2.8 nats (second 57
  read 1 in both of its two looks). In the random-chain test, DECODED wrong is 981 of 7200
  minutes at and beyond the model's rates. Only CONFIRMED is a verified value.
- **The error model rests on four ten-minute captures.** 0 -> 1 is 8/1028 pooled and 6/183
  on saga agc; 1 -> 0 is 8/383 pooled and 7/103 on saga plain; P -> 1 is 10/157 pooled and 5/42
  on saga plain. A live run should re-measure them: `tests/air/decode_score.py` prints the 0/1
  confusion (not P -> 1). The slow ctest (`ctest -C Slow`) holds the gate at 2x and 3x the
  worst run.
- **A frame opens at stream time 0.0 when a run ends.** On saga plain the last symbol carries
  `minute_start_ms` 0 and the decoder emits a `BCDS,FRAME` and `BCDS,UTC` line for a frame starting
  at 0.0, which `decode_score.py` scores as 12:10 (all NONE). Not traced.
- **The pessimistic margin is not reported.** `BCDS,UTC` carries the nominal margin only, so a
  part at 24 nats nominal that does not CONFIRM gives no reason on the line.
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
- `wwv_td_config_t` gained `p01_pessimistic`, `p01_prior_weight` and `confirm_looks`. A caller
  that fills the struct by hand must set them; `wwv_td_default_config()` does. `BCDS,FRAME`'s
  p01 is the nominal model's.
- `struct bcd_time_detector` (`include/detection/bcd_internal.h`) gained `pulse_energy_sum`,
  `pulse_energy_frames`, `pulse_level` and `last_valid_frame`. BTIME events now start and end at
  half the pulse amplitude (`BCD_TIME_EDGE_FRACTION`, `BCD_TIME_LEVEL_ALPHA`,
  `BCD_TIME_LEVEL_HOLD_MS` in `include/bcd_time_detector.h`), and a segment rejected as too short
  takes no cooldown. The median width of a 0 is 36-67 ms shorter than before (184-189 ms against 220-251 ms).

## Documentation

- The cited source (NIST SP 432, https://tf.nist.gov/general/pdf/1383.pdf) is not filed in
  `docs/`; the algorithm doc cites it by URL.
- README claims and the audit's F10 items are untouched.

## Commit message corrections

- "Tests: BCD time detector pulse width on a synthetic WWV time code" says that at 07ffb51 the
  steady case's 0s end 72-132 ms after the sent end and its 1s start at +227..+229 ms. It is 9 of
  the 11 sent 0s (the other 2 within 60 ms), and 25 of the 27 late 1s start at +226..+230 ms.
