# Notes: issues found while fixing the WWV detectors and building the time decoder, not fixed

Measured with `tests/air/run_air.sh` against the KiwiSDR (kb5ag, 5 MHz) and Saga RSP2 (5 MHz)
captures in `~/wwv-capture`. "plain" is carrier at DC with no AGC; "agc" adds the waterfall.c
`normalize()`.

## Detection

- **BCD symbol accuracy after the BTIME half-amplitude edges, with BTIME's width taking
  precedence over BFREQ's.** Symbols are 441/445, 447/450, 428/463 and 297/324 correct on kiwi
  plain / kiwi agc / saga plain / saga agc. Of symbols whose pulse starts within 250 ms of the
  second, a sent 0 reads 1 in 0/292, 0/293, 2/262, 6/183 and a sent 1 reads 0 in 0/105, 0/108,
  6/103, 0/67; a sent P reads 1 in 1/44, 0/45, 4/42, 0/26 (decoder frames).
  `tests/air/trace_all.sh <out-dir>` gives every wrong symbol a cause: spurious 3/3/21/18, width
  1/0/13/8, misaligned 0/0/1/1. `tests/air/btime_frames` and `tests/air/btime_width_forensics.py` give the per-pulse
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
  - **Seen on air, and fixed (fix/wwv-marker-rate, 2026-11-20):** kiwi agc 11:43's marker was
    hidden by the cooldown of a 138 ms window run 28 s earlier. A short run now takes no
    cooldown and is printed (`MARKER short run`); kiwi agc finds 10 markers, was 9.
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
- **A third of the live run's seconds are mid-second detections.** Of the locked, non-erased
  seconds the decoder saw, 30.4% (1381/4547) carried a pulse that starts more than 250 ms after
  the second (23.1-35.4% per 10-minute block), against 0.7-12% on the four captures
  (`tests/air/symbol_rates.py`). They carry no evidence, so they slow CONFIRMED as much as
  erasures.
  - **Traced (fix/wwv-mid-second): a chain of BTIME events started on the low-level
    subcarrier.** When the 200 ms cooldown ends, the detector starts an event on the low level.
    That event's end threshold is half its own mean, so it runs through the next second's pulse
    and ends at that pulse's trailing edge. The next event then starts mid-second again. On run90
    / run90c, of 1381 / 559 mid windows, the pulse was inside an event from the previous window in
    922 / 374. The previous cooldown hid the pulse start in 325 / 153, and nothing started by
    +250 ms in 134 / 32. Mid starts cluster at +450 and +750 ms, and 460 of 1381 start within
    10 ms of the cooldown's end. After a P the share is 5% (the cooldown ends inside the next
    pulse). No link to the tone schedule, the tick or the voice.
  - **The correlator now takes such a window's symbol from the trailing edge** (source `EDGE`,
    `EDGE_MIN_END_MS` / `EDGE_LEAD_MS` in `include/correlation/bcd_correlator_internal.h`).
    Mid-second falls from 30.4% to 8.2% on run90 and from 11.8% to 3.3% on run90c, and time of day
    CONFIRMS at 2040 s (was 3059 s) and 1074 s (was 1194 s).
  - **The detector is not changed.** Why the low level clears the start threshold, max(2 x
    noise, 0.5 x pulse level), cannot be shown from the event logs. `wwv_live --frames` now
    records the detector's energy and thresholds per frame for the next live run. A detector-side
    fix (a start threshold set between the tracked low level and the pulse level) needs that
    evidence first.
- **`EDGE` symbols read a 0 as 1 more often than boundary symbols on the RSP2 captures**: 4/33
  saga plain and 6/53 saga agc, against 2/262 and 6/238 at the boundary. Live it is 146/932
  (run90) and 13/400 (run90c), close to the boundary rate. In each traced case the carried event
  started on the low level in the last 350 ms of the previous second, so its mean is low and the
  low level after the pulse stays above half of it, and the event ends late. No start-offset bin
  separates them (86-95% right in each). The decoder weighs `EDGE` and boundary looks alike.
- **A boundary window's width would also read better from its trailing edge on live.** Taking the
  first event end in [+100, +900) ms as the pulse end, run90's boundary 1 -> 0 falls from 15.0%
  to 6.3% and run90c's from 4.1% to 0.3%, but 0 -> P appears (54 / 10, and a P outside a P
  position reads 1). Not changed: it is the boundary path, not the mid-second one.
- **DECODED time of day wrong rose with the `EDGE` symbols**: 0 -> 1 on run90 and 1 -> 3 on run90c
  (decode_score.py). CONFIRMED wrong stays 0. DECODED is the unverified state.
- **Saga plain ticks are 42.6% matched** (saga agc 59.9%, kiwi 75-76%). The limit on the weaker
  RSP2 signal is not investigated.
- **Tone tracker window (audit F3)** is still Hann, not the Blackman-Harris it had before 591d6ee.
  Not touched.
- **The tick detector re-triggers on noise about every 530 ms on a weak night** (run90 console:
  1798 TICK intervals of 512-650 ms). Its threshold, twice a floor that falls
  fast and rises slowly, sits inside the noise; a 5 ms crossing that fails the correlation test
  is dropped without a log line but still takes the 500 ms cooldown. This is what starts the
  minute marker's run late (fix/wwv-marker-rate: 99 of 268 live minutes). Not fixed in the
  detector, where it moves every tick metric; the sync detector now recovers those markers from
  their end (marker candidates). Giving a dropped run no cooldown is the detector-side fix to
  measure next.
- **The tick-path floor reset fires on every late-started marker.** Its run lands in the 50-600 ms
  gap zone, and `tick_state_reset_floor()` takes the marker's mean energy as the new floor (run90:
  66 resets, the floor rising 1.6-13.8 times). Ticks in the seconds after :00 then need that much more
  energy until the floor decays (about 3.5 s for a factor 4). The tick loss it causes is not
  measured.
- **The tick timing gate is never enabled in the chain.** `tick_detector_set_gate_enabled()` has
  no caller; the manager sets only the epoch. All four captures ran with the gate off.
- **Two marker-detector misses on the captures are near misses**: saga agc 12:12 and 12:18 peaked
  at 2.74 and 2.83 times baseline against the 3.0 threshold, with no run at all. A second,
  lower threshold reported only as a candidate (phase-gated by sync, like the short runs) would
  take them. Not done: the live share of no-run minutes is unknown until a run logs the `MARKER
  block peak` lines.

## Sync and correlator

- `sync_detector_full_reset()` does not clear `last_confirmed_ms`, so a stale anchor survives a
  reset. The re-anchor rule mitigates it.
- The sync detector gets no per-tick events (`sync_detector_tick_event()`): neither the manager
  nor the harness ever fed them, so tick-hole evidence never arrives. Kept that way so the scored
  chain did not change.
  - **Measured on the holdover branch, still not fed.** With ticks fed, no tick hole is detected in
    the four captures or the 90-minute live run (a hole needs 20 consecutive 950-1050 ms ticks
    first). The 0.05-per-tick confidence instead locks sync on one marker: kiwi agc locks at
    11:34:44 on the false 11:34:20 tick-path marker and reads 544/584 symbols right against
    447/450. Ticks carry no minute phase; feeding them needs a lock rule that does not count them
    toward LOCKED.
- **`sync_detector_get_frame_time()` takes the current second from `tick_gap.last_tick_ms`**, which
  stays 0 because ticks are not fed. wwv_live's STATUS line prints `sec=1` in all 40 LOCKED lines
  of the 2026-11-20 live run. Display only; the correlator does not use it.
- `recovery_tick_seen` in `recovery_state_t` is written but never read since the holdover change
  (recovery re-locks on a marker on the held phase alone).
- **Stream times are `float` milliseconds** through sync, the correlator and the decoder. The step
  is 0.5 ms at 90 minutes, 8 ms after 18.6 hours and 16 ms after 37 hours. With holdover, a live
  run can stay LOCKED that long; nothing re-bases the clock.
- **A marker within 5 s of a whole number of minutes is accepted and moves the anchor**
  (`confirm_marker()`'s +-5 s interval test), in every state. On air every accepted marker is
  within -45..+61 ms of :00, so it has not mattered. Under holdover a single such marker 1-5 s off
  would move a held, correct phase.
- **BFREQ still sets the width of a window with no BTIME event, and can set its pulse offset.**
  Its durations (164-983 ms over the four runs) do not cluster at 200/500/800 ms; BFREQ-only
  windows gave 7 wrong symbols at b2ede79 (not re-measured). When a window holds both, the
  pulse offset is the earlier of the two first events (`src/correlation/bcd_correlator.c`), so
  a BFREQ event can mark a window as a boundary detection. Not changed.
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
- **The confidence decay is per periodic check, and the check runs every 50 ms, not 100 ms.**
  `CONFIDENCE_DECAY_NORMAL` 0.9999 is commented as 6% per minute; at 50 ms it is 11.3%. LOCKED
  (0.70) therefore needs three markers close together: three consecutive minutes reach 0.73, a
  missed minute before the second 0.71, but a missed minute between the second and the third
  only 0.69, and the fourth marker is needed.
- **Acquisition now takes the first marker plus two minutes in every replay** (run90, run90c,
  smoke, live15: LOCKED 118-120 s after TENTATIVE). A candidate never starts a phase, so the
  minute before the first marker is lost even when it holds a late-started marker on the phase
  the marker then sets (run90 00:31, live15 13:08). Keeping recent candidates and applying them
  when the first marker arrives would save a minute there; not done.

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
- **The pessimistic rates adapt up only.** The pessimistic p01 and p10 are pooled over the
  chain's reserved seconds and P seconds and never go below 0.07 / 0.15, so a clean chain is
  judged as before (the air runs and the measured-rate simulation are unchanged). The nominal
  p01 is still per frame. A fade that raises the 0 -> 1 rate only in data seconds is not seen
  by the reserved seconds; on the live run the two rates agree (14.3% / 14.4%).
- **The pessimistic p10 rests on a proxy.** A sent 1 has no unbiased sample, so the decoder
  takes P seconds read as 1 or 0. Whole runs: 20.9% against 1 -> 0 15.0% live, 9.3% / 5.8% saga
  plain, 0/34 / 1/87 saga agc (the floor covers it). Per 10-minute live block it is below 1 -> 0
  in 2 of 9, by under a point; P -> 1 alone is below in 6 of 9 (01:10: 14.7% / 23.8%), which is
  why P -> 0 counts. The price: the live replay CONFIRMS the time of day at 3059 s, against
  2339 s with P -> 1 alone and 2219 s on the fixed model. A site where a 1 reads short much more
  often than a P would need another estimate: at twice the live rates with the proxy at 2/3 of
  p10 the simulation still gave 0 in 5000, but theory puts the effective margin at 7.6 nats.
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
- **The live run's rates, and what the fixed model did with them.** 2026-11-20 00:30-02:00 UTC,
  replayed with holdover (85 frames; `tests/air/symbol_rates.py`): erase 9.3%, mid-second 30.4%,
  0 -> 1 14.4%, 1 -> 0 15.0%, P -> 1 14.8%, P -> 0 6.1%; per 10-minute block 0 -> 1 10.7-21.4%,
  1 -> 0 7.3-23.8%, erase 4.1-18.5%, mid 23.1-35.4%. With the fixed 0.07 / 0.15 the simulation
  (5000 chains of 60 minutes) confirmed wrong in 0 chains at these rates, 12 at 1.5x and 41 at
  2x, mostly status bits; the replay had 0. With 20% of minutes at twice the live rates it
  confirmed wrong in 2 (LSW, DUT1). With the pessimistic rates raised from the chain's evidence
  it is 0 in all four. The slow ctest now also runs 1x and 2x live.
- **What the adaptive pessimistic rates cost.** Time of day CONFIRMED, median / p90 minutes
  (never, of 5000): live rates with P -> 0, 60-minute chains, 24 / 33 on the fixed model, 28 /
  38 now (27 / 37 with P -> 1 alone as the proxy). The slow gates (30 minutes): x2 air 18 / 24
  (74) to 20 / 28 (256), x3 air 22 / >30 (568) to 28 / >30 (2013), xlive 23 (765) to 27
  (1651). The x2 / x3 cases draw P -> 1 at 1.8 times p10, so the proxy reads p10 high there. At
  the measured air rates nothing moves (11 / 13). The live replay: 2219 s to 3059 s.
- **The simulator draws each symbol's error independently; the live run's are not quite.** A
  sent 0 after a sent P reads 1 in 6.9% (14/203), after a 0 in 14.8%; a sent 1 after a P reads
  0 in 6.4% against 17.5-18.1%. Per second of the minute the 0 -> 1 spread is 1.37 times
  binomial, per frame 1.48. The gate's rates are pooled over contexts, so it is not optimistic on
  that count, but runs of errors on one bit are only as frequent as chance makes them.
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
- `wwv_detector_manager_process_detector_sample()` now takes 50 kHz baseband before the channel
  filters (it owns the filters, the optional AGC, the BCD detectors, correlator and decoder).
  `wwv_detector_config_t` gained `enable_bcd`, `enable_time_decoder`, `enable_agc`, `verbose`;
  `output_dir` NULL writes no CSVs. New: observer struct, component getters, `wwv_chain_log`,
  `iq_frontend`, `telem_set_tap()`, telemetry line `CONS,ANCHOR`.
- The sync detector holds LOCKED for 20 minutes without a marker (was 90 s), and RECOVERING re-locks
  on one marker on the held phase (it never re-locked before). Its STATE telemetry and callback see
  far fewer transitions.
- `bcd_symbol_event_t.source` can be `"EDGE"`: the symbol came from the trailing edge of an event
  that started in an earlier window, and `pulse_offset_ms` is that event's start (before -100 ms).
  `struct bcd_correlator` gained the `edge_*` fields, and there is a new internal
  `bcd_window_note_edge()`. The old static `close_window()` in `bcd_correlator.c` (destroy path)
  does not use the edge.
- `wwv_live` gained `-F` / `--frames FILE` (BTIME detector state per frame, 9 float32).
- `wwv_td_config_t` gained `p01_pessimistic`, `p01_prior_weight` and `confirm_looks`. A caller
  that fills the struct by hand must set them; `wwv_td_default_config()` does. `BCDS,FRAME`'s
  p01 is the nominal model's.
- `p01_pessimistic` and `p10_pessimistic` are now floors and prior means: the pessimistic rates
  rise with the chain's reserved seconds and P seconds, and `p01_prior_weight` weights both
  priors. No field was added.
- `struct bcd_time_detector` (`include/detection/bcd_internal.h`) gained `pulse_energy_sum`,
  `pulse_energy_frames`, `pulse_level` and `last_valid_frame`. BTIME events now start and end at
  half the pulse amplitude (`BCD_TIME_EDGE_FRACTION`, `BCD_TIME_LEVEL_ALPHA`,
  `BCD_TIME_LEVEL_HOLD_MS` in `include/bcd_time_detector.h`), and a segment rejected as too short
  takes no cooldown. The median width of a 0 is 36-67 ms shorter than before (184-189 ms against 220-251 ms).
- **Marker candidates (fix/wwv-marker-rate).** `tick_marker_candidate_t`,
  `tick_detector_set_candidate_callback()` and `TICK_CANDIDATE_MIN_MS` (`include/tick_detector.h`);
  `marker_detector_set_candidate_callback()`; `sync_detector_marker_candidate()`; the observer
  gained `on_tick_marker_candidate` and `on_marker_candidate` (a host that fills the observer
  positionally must add them); chain-log lines `CANDT start= end= lead_s= lead_e=` and `CANDM t=
  dur= lead=`. `replay_events` takes an optional console file and rebuilds the tick-path
  candidates from its `REJECTED: dur=` lines when the log has no `CANDT` lines.
- **Console lines changed**: the tick gap-zone `REJECTED` line gained `start= end= idle=
  prev_end=`; the marker detector prints `MARKER short run` (with flank and `(candidate)`) and,
  every 10 s, `MARKER block peak`; `MINUTE MARKER` gained `peak/baseline`. A marker-detector
  short run takes no cooldown.

## Live host (`apps/wwv_live.c`, `scripts/wwv_rsp2_live.sh`)

- **`sdrplay_apiService` 3.15.2 segfaults while systemd stops it**: both live-host stops on
  2026-11-19 (17:44:59 and 17:54:25 UTC) ended SEGV, Result core-dump, after the stream was
  closed. The unit was stopped but `failed`. The capture session's stop at 12:31 was clean. The wrapper now reports it and runs `systemctl reset-failed`.
  The cause is in the vendor binary and is not investigated.
- **At 17:42 UTC the whip saw no usable WWV on 5 MHz**: carrier C/N0 11-14 dB-Hz, against 54-62
  dB-Hz in the 12:09 UTC capture with the same settings. The 165 s smoke test found 0 markers and
  never left ACQUIRING. A live run needs a time of day with propagation (the scored capture was at
  dawn).
- **The 90-minute live run (2026-11-20 00:30-02:00 UTC) lost lock 9 times before the holdover
  fix.** `tests/air/replay_events` reproduces its SYNC, symbol and frame lines from its
  `--log`, so a sync, correlator or decoder change can be measured on it without IQ. The log is
  in `~/wwv-live/run90/`, outside the repo.
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
  symbols 245/286 vs 178/215 (07ffb51 detectors). With the 21ba9aa BTIME fix: plain symbols
  430/460 vs 426/463, agc 383/409 vs 297/324. False confirms 0 in all.
- The tone trackers send no telemetry; wwv_live runs them only for the manager's display path.
- `marker_correlator` and `tick_correlator` are disabled in both hosts; the tick correlator is
  created by the manager when enabled but never fed (it needs tick-detector internals).
- **A stream timeout loses samples and nothing records it in the chain log.** run90d's RSP2
  timeout (console `[rsp2_stream] timeout #1`, about stream 1077 s) lost about 1.1 s: every marker
  after it reads -1.1 s from UTC. The sync detector followed through the +-5 s interval rule
  (see Sync and correlator). `tests/air/marker_minutes.py --step 1077000:-1115` scores that run.
- **`~/wwv-capture/score.py` matches chain-log lines by the prefix `TMARK`**, so any new line
  starting with it crashes the air scoring. The candidate lines are `CANDT` and `CANDM` for that
  reason.

## Documentation

- The cited source (NIST SP 432, https://tf.nist.gov/general/pdf/1383.pdf) is not filed in
  `docs/`; the algorithm doc cites it by URL.
- README claims and the audit's F10 items are untouched.

## Commit message corrections

- "Tests: BCD time detector pulse width on a synthetic WWV time code" says that at 07ffb51 the
  steady case's 0s end 72-132 ms after the sent end and its 1s start at +227..+229 ms. It is 9 of
  the 11 sent 0s (the other 2 within 60 ms), and 25 of the 27 late 1s start at +226..+230 ms.
- "Time decoder: measured error model, CONFIRMED at ln 10^6" says the status bits "need 8
  frames at ln 10^6". Not measured: the rollover run (from 23:55) leaves its status bits DECODED
  after 12 minutes, 7 frames past 00:00, and CONFIRMS them all by 16 minutes.
