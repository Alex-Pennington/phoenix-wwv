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
    - **Traced and changed on fix/wwv-btime-start.** The start threshold was max(2 x noise floor,
      0.5 x pulse level). The noise floor adapts only on idle frames, which are the low level, so
      2 x it is 1.2-1.5 x the low level; and every event started on the low level feeds its mean
      (near the low level) into the pulse level, pulling it to 0.52-0.67 of the measured high
      level at mid starts on saga. 12-20% of low-level frames exceed 1.4 x the low level's
      median at every site. A start now also needs 2.37 x the low level (7.5 dB, half the 15 dB
      step), measured as the median over the second half of each cooldown
      (`BCD_TIME_START_LOW_RATIO`). Mid starts / carried, d46cee4 -> fix: saga plain 178/43 ->
      33/5, saga agc 171/98 -> 59/38; run90f (1649 s replayed from `--frames`) 1443/211 ->
      336/38. `tests/air/btime_replay` re-runs the state machine on a frame record;
      `tests/air/btime_start_forensics.py` gives these counts.
    - **Residual carried events (fix branch).** On saga agc 38 remain: at their starts the
      threshold is 1.99 x the second's low level (median), because the cooldown estimate fell in
      its low tail (it reads the low level at 1.00 median, p10 0.79). Averaging over more than one
      cooldown would narrow it and slow fade tracking. Not done.
    - **When no valid pulse comes for 2 s, both levels drop and the old 2 x noise threshold
      (1.2 x the low level) applies again.** On run90f 115 of the fix's 336 mid starts come
      from that state. Its decoder-view mid rate rises from 9.6% to 18.6% of non-erased
      seconds, and erasures from 6.5% to 28.2% (the pulses it no longer starts have pulse/low
      1.45 at the median). A fallback that keeps the last low level, or a longer hold for it,
      is the next thing to measure. Not done.
      - **Measured (fix/wwv-bcd-corr-cleanup), not changed.** On run90f (frame replay, main)
        the pulse level is 0 at 141 of 377 mid starts, and the mid starts' pulse/low ratio is
        1.51 at the median: no start threshold separates the pulse from the low level there.
        Every fallback tried trades mid starts for erasures and loses right symbols on all
        three frame runs (right/wrong, run90f / run90g2 / run3h): main 664/142, 827/226,
        3471/210; low level kept and halved every 1 s 648/119, 757/219, 3367/203; kept 5 s
        571/105, 651/173, 3177/197; kept with 1.4 x it 647/128, 754/218, 3077/190. With 1.6 or
        1.8 x it run90f locks at 100% erasure: nothing starts, so no cooldown refreshes the
        low level. Run90f time of day DECODED right falls from 18/19 to 16/18 or less.
    - **The "noise floor" is not noise.** It is a low percentile of the 100 Hz low level, so
      `BCD_TIME_THRESHOLD_MULT` x it tracks the low level, not the receiver noise.
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
- **A P carried from before its second was rejected as too long (fixed, fix/wwv-bcd-corr-cleanup).**
  At saga 12:18:09 (both wirings) an event started on a low-level bump at -220 ms and ran
  through the P to +815 ms: about 1035 ms, over `BCD_TIME_PULSE_MAX_MS` (1000). The detector
  rejected it, so the P left no event and no trailing edge (NONE). The cap is now 1100 ms (a
  start up to 250 ms early). Saga plain correct 444 -> 446, saga agc 429 -> 432; run3h 9
  erasures become symbols, 6 right. An event that starts in the last 15 ms of a window and runs
  the full 1100 ms now reports after that window's close latency and is dropped from it (a
  mid-second window); its trailing edge is still noted.
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
  - **Fixed (fix/wwv-tick-marker-cleanup).** It is also sent on `TELEM_MARKERS` as
    `PURITY,<end ms>,<pulse start ms>,<window run ms>,<flank/centre>,<peak/baseline>`, and the
    chain log (`wwv_live --log`) writes that line as `TELEM MARK PURITY,...`. The air harness
    prints TELEM BCDS lines only, so its logs do not carry it; its stdout already has the
    `MARKER rejected` line. A UDP subscriber to the markers channel now sees PURITY lines among
    the marker CSV lines.
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
  - **Fixed (fix/wwv-tick-marker-cleanup): the check now uses `MARKER_MAX_DURATION_MS`**, what the
    bail-out already enforced, and `_CHECK` is gone. Bail-outs: 1 on the captures (saga agc,
    :49 of a minute, not a marker), 0 in the run90, run90c, run90d, run90f, run90g and run3h
    consoles; clean marker runs are 804-835 ms. A run of 1000-1005 ms (one frame past the
    bail-out test) no longer counts as a marker; none is seen. run_air.sh output unchanged.
- **The breakdown in the `TICK_FILTER_DELAY_MS` comment is stale** (`include/tick_detector.h:71`,
  "0.32ms Butterworth"). The sync filter's group delay at 1000 Hz is now 1.50 ms. The value
  (3.0 ms) was left alone: kiwi plain tick-path marker leading edges measure -1.8..+2.1 ms from
  :00.
  - **Fixed (fix/wwv-tick-marker-cleanup):** the comment now says the value is empirical.
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
- **Fixed (fix/wwv-bcd-corr-cleanup): a trailing-edge width is a 0 up to 450 ms**
  (`TRAIL_ZERO_MAX_MS`, `bcd_symbol_classify_trailing()`). Trailing-edge widths of sent 1s
  start at 470-480 ms; sent 0s outnumber them in every 10 ms bin up to 460 ms. EDGE 0/1 errors
  over six live replays and four captures: 202 at the 350 ms cut, 102 at 450. EDGE 0 -> 1 on
  run90 / run90c / run3h: 156 / 11 / 8 -> 70 / 6 / 2; EDGE 1 -> 0 8 / 2 / 0 -> 11 / 5 / 3.
  The entry below is the measurement it rests on.
- **`EDGE` symbols read a 0 as 1 more often than boundary symbols on the RSP2 captures**: 4/33
  saga plain and 6/53 saga agc, against 2/262 and 6/238 at the boundary. Live it is 146/932
  (run90) and 13/400 (run90c), close to the boundary rate. In each traced case the carried event
  started on the low level in the last 350 ms of the previous second, so its mean is low and the
  low level after the pulse stays above half of it, and the event ends late. No start-offset bin
  separates them (86-95% right in each). The decoder weighs `EDGE` and boundary looks alike.
- **Changed (fix/wwv-bcd-corr-cleanup) for one shape: a pulse start hidden by a cooldown.**
  An event on the low level ends shortly before the second; its 200 ms cooldown covers the
  rise; the next event starts when the cooldown ends, inside the pulse, and ends at the
  trailing edge. A window whose first event starts 190-210 ms after the previous BTIME event's
  end, that end less than 170 ms before the second, and the start at +60 ms or later, takes its
  width from the end (source `TRAIL`, the trailing-edge 0 cut). Fixed/broken among those
  windows: run90 84/13, run90c 40/2. Right/wrong: run90 3759/576 -> 3832/503, run90c
  4543/121 -> 4582/83, run90d 4595/230 -> 4654/172; frame replays and captures unchanged.
  The +60 ms floor excludes the second after a P (cooldown ends at +35..+55 ms, at the rise);
  without it run90f read 13:32:20 (a 1, 369 ms) as 0 and lost 9 decodes. The broader rule
  below is not used. Run90c time of day DECODED wrong falls 3 -> 0 (the entry on DECODED wrong
  below); run90 time of day / date first CONFIRMED 2040 / 2040 s -> 1919 / 1259 s.
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
  minute marker's run late (fix/wwv-marker-rate: 99 of 268 live minutes).
  - **Measured on the captures (fix/wwv-tick-marker-cleanup).** Of the ticks main misses, the
    pulse fell in the cooldown after a dropped run in 58/143, 52/149, 174/336 and 87/238 (kiwi
    plain, kiwi agc, saga plain, saga agc); below threshold, idle, in only 1, 1, 17 and 34. The
    gap-zone runs under 100 ms (too short for a marker candidate) do the same: saga agc's
    12:14 marker started late behind a 51 ms one.
  - **Fixed: a run too short for a marker candidate that is not a tick (a tick-length run that
    fails the correlation test, or a gap-zone run under `TICK_CANDIDATE_MIN_MS`) takes no
    cooldown.** The gap-zone floor reset still runs. Ticks matched 76.3/75.1/42.6/59.9% ->
    89.3/87.0/62.9/70.9%; tick-path markers at :00 9/9/6/8 -> 10/10/9/9; late-started markers
    (gap-zone runs ending :00.5-:01.0) 0/0/4/2 -> 0/0/1/1. Off-second ticks 58/70/264/163 ->
    63/68/318/180, and saga plain has 2 ticks at :29 (+44, +62 ms): more noise runs are
    tested, and the off-second rate puts 1.2 in the twenty :29/:59 windows by chance.
    `tests/test_tick_noise_run.c` drives the state machine with both run kinds.
  - **Two downstream changes, traced, not in this code.** kiwi agc: a 128 ms gap-zone noise run
    at 11:41:56.06 gives a CANDT whose start lead is 4 s + 59 ms off the held phase, and the
    sync detector's whole-second rule (within `CANDIDATE_SECOND_TOL_MS`, 60 ms) drops the phase
    for 6 s; time of day decodes 6/6, was 7/7, and symbols 446/445, were 452/451. On main the
    floor was higher there (0.049 against 0.033) and no run started. saga plain: 12:12's anchor
    is now the tick-path marker (149951.5 ms) instead of the marker detector's (149933.4 ms),
    and :01's window sees only a 497 ms event, so a 0 reads 1 (correct 444 -> 443).
  - **Live cannot show this.** The live logs hold detector events, not tick energies, so a replay
    gives the old tick events; only the next live run will.
- **The tick-path floor reset fires on every late-started marker.** Its run lands in the 50-600 ms
  gap zone, and `tick_state_reset_floor()` takes the marker's mean energy as the new floor (run90:
  66 resets, the floor rising 1.6-13.8 times). Ticks in the seconds after :00 then need that much more
  energy until the floor decays (about 3.5 s for a factor 4).
  - **Measured (fix/wwv-tick-marker-cleanup).** Live, a reset on a run ending :00.5-:01.0 came in
    40 / 24 / 5 / 15 minutes of run90 / run90c / run90d / run90f. In those minutes ticks at
    :01-:05 come at 0.24 / 0.36 / 0.36 / 0.23 of the rate at :06-:10 of the same minutes; in the
    other minutes at 0.65 / 0.83 / 0.73 / 0.52. At the other minutes' ratio that is about 45 /
    50 / 8 / 6 ticks lost, 1-2 per such minute. On the captures (main) saga plain gets 3/20
    ticks at :01-:05 after such a reset, 14/30 in the other minutes; saga agc 5/10 and 23/40.
  - **Declined: the reset is not narrowed.** Each narrowing tried loses more ticks than it saves,
    on every capture (ticks matched, kiwi plain / kiwi agc / saga plain / saga agc, against
    89.3 / 87.0 / 62.9 / 70.9% with the reset): no reset on gap-zone runs 60.0 / 71.9 / 35.5 /
    49.5%; none on runs of 100 ms or more (the marker candidates) 86.5 / 81.4 / 60.9 / 66.6%;
    half the run's mean as the floor 85.8 / 80.8 / 55.5 / 61.9%. A floor low in the noise
    lets more noise runs through, and their ticks' cooldowns cost more than the reset does.
    What cuts the loss is fewer late-started markers: the no-cooldown change above takes them
    from 6 to 2 on the captures.
- **The tick timing gate is never enabled in the chain.** `tick_detector_set_gate_enabled()` has
  no caller; the manager sets only the epoch. All four captures ran with the gate off.
  - **Evaluated (fix/wwv-tick-marker-cleanup), not enabled: the call belongs in the manager.**
    A harness copy that, every 50 ms, sets the epoch to the sync detector's `second_start_ms`
    and enables the gate while LOCKED (off otherwise) gives, against the no-cooldown change
    (kiwi plain / kiwi agc / saga plain / saga agc): ticks matched 89.3 / 87.0 / 62.9 / 70.9%
    -> 94.8 / 91.5 / 86.5 / 82.4%, off-second ticks 63 / 68 / 318 / 180 -> 14 / 17 / 101 / 55
    (the rest come before LOCKED), tick-path markers at :00 10 / 10 / 9 / 9 -> 10 / 10 / 10 / 9.
    FALSE CONFIRM 0. Time of day decodes 7 / 7 / 7 / 7 (kiwi agc was 6: the noise run behind its
    phase loss no longer starts).
  - **The gate now opens 20 ms before the epoch (`TICK_GATE_START_MS`).** It opened at the epoch,
    and on kiwi plain the held phase ran 12-20 ms late from 11:43, which shut out every tick run
    start: 11:43-11:44 matched 21/57 and 17/41 gated, 45/57 and 34/41 ungated, and the capture
    85.1% against 94.8% with the early opening. Opening 30 ms early is no better; 50 ms early
    lets more noise in (saga plain 77.9%). The gate is off in the chain, so the captures do not
    change. `tests/test_tick_gate.c` checks a phase 15 ms late.
  - **What stops enabling it: saga agc loses LOCKED for 6 s with the gate on** (time of day 5/5,
    was 7/7). A gated run starts at a tick, so a tick followed by noise makes a 256 ms gap-zone
    run whose start lead is 3 s off the held phase, to within 20 ms, and the sync detector's whole-second
    candidate rule drops the phase. It is the same mechanism as kiwi agc's loss under the
    no-cooldown change: an off-phase CANDT that starts on a tick is noise by construction, not
    evidence of a lost second. Enabling the gate needs that rule changed first.
  - **With that rule changed (fix/wwv-sync-decoder-cleanup, a candidate's whole-second
    disagreement counts only on its end reading) the gate is clean.** This branch plus that
    commit, the gate on while LOCKED in the manager's 50 ms periodic check (epoch = the held
    phase's `second_start_ms`, refreshed when it moves 0.5 ms): ticks matched 94.8 / 91.5 /
    86.5 / 82.4%, off-second 14 / 17 / 101 / 54, no phase loss, time of day 7 / 7 / 7 / 7,
    FALSE CONFIRM 0, ctest 13/13. The call goes in `wwv_detector_manager.c`, not here. The live
    logs hold detector events, so a replay cannot show the gate; the next live run will.
- **Two marker-detector misses on the captures are near misses**: saga agc 12:12 and 12:18 peaked
  at 2.74 and 2.83 times baseline against the 3.0 threshold, with no run at all. A second,
  lower threshold reported only as a candidate (phase-gated by sync, like the short runs) would
  take them. Not done: the live share of no-run minutes is unknown until a run logs the `MARKER
  block peak` lines.
  - **Measured, and declined (fix/wwv-tick-marker-cleanup).** run3h logs the block peaks: of 33
    minutes with no marker-detector MARK, 9 peaked at 2.53-2.91 times baseline at +0.81 s
    (near misses) and 9 at 3.07-3.39 (short runs, already candidates); 3 of 341 other idle
    10 s blocks reached 2.5 (run90g, daytime: 8 of 335). Every one of the 9 live near-miss
    minutes already has a tick-path TMARK or CANDT within 13 ms of :00, and so do saga agc
    12:12 and 12:18. A second threshold at 2.5x, reported as a candidate with the purity test
    and no cooldown, was built and run: on the captures it passes only those two (the 9 other
    weak runs fail purity), their leads read -123 and -207 ms (the second outside sync's
    150 ms candidate tolerance), and no metric moves. It adds a path that finds nothing the
    tick path has not, so it is not kept.

## Sync and correlator

- `sync_detector_full_reset()` does not clear `last_confirmed_ms`, so a stale anchor survives a
  reset. The re-anchor rule mitigates it.
- The sync detector gets no per-tick lock evidence (`sync_detector_tick_event()`): neither the
  manager nor the harness ever fed it, so tick-hole evidence never arrives. Kept that way so the
  scored chain did not change. Ticks and BCD time-pulse leading edges now reach sync through
  `sync_detector_second_edge()`, which only tracks the held sub-second phase while LOCKED
  (fix/wwv-sync-drift); it never adds confidence, starts a lock or chooses the minute.
  - **Measured on the holdover branch, still not fed.** With ticks fed, no tick hole is detected in
    the four captures or the 90-minute live run (a hole needs 20 consecutive 950-1050 ms ticks
    first). The 0.05-per-tick confidence instead locks sync on one marker: kiwi agc locks at
    11:34:44 on the false 11:34:20 tick-path marker and reads 544/584 symbols right against
    447/450. Ticks carry no minute phase; feeding them needs a lock rule that does not count them
    toward LOCKED.
- **`sync_detector_get_frame_time()` reads the second at the last periodic check**
  (fix/wwv-sync-decoder-cleanup). It took it from `tick_gap.last_tick_ms`, which stays 0 because
  ticks are not fed, so it read -anchor mod 60 s: 142 of 162 LOCKED STATUS lines of run3h's console
  say `sec=54`. Display only.
- **Lost samples move the held phase; the tracker follows them only in part on a weak band**
  (fix/wwv-sync-drift). On run90g seg2 (15 MHz) the stream lost about 3 ms/s, up to 5 ms/s, from
  t=1330 s: 5.3 s by the end, read from the console's least-delayed lag (it matches every TMARK
  lead within about 20 ms). Before the tracker the held phase was 0.66 s, 1.39 s and 1.24 s late
  at the re-anchors (1991 s, 2469 s, 2947 s) and 575 of 2662 windows were 500 ms or more off;
  after, 72 of 2381.
  - On seg2, true ticks are 1-8 a minute against about 60 false ones, so BCD time-pulse starts
    carry the slew. Their leading edges spread over about -35..+105 ms around the predicted
    second there, and the coordinator's +-40 ms gate holds only part of that spread. Slews
    take back 344 ms of the 5.3 s; the rest comes from the snaps (5) and the marker
    re-anchors. 498 windows are still more than 140 ms off.
  - The outcome is sensitive to the BCD-only dead band: 15 ms gives time of day 29 right, last
    right at 14:49; 20 ms gives 15/17, last right 14:39. Both give 0 false confirms.
  - The BCD time detector's lead differs by band: +17..+34 ms from the marker lead on the
    5/10 MHz runs, peaking near +50 ms on seg2. `PHASE_LEAD_BTIME_MS` is 30 ms for all.
  - **The loss is known exactly upstream.** wwv_live's lag (arrival minus stream time) rises by
    what was lost, and the SDRplay stream callback carries `firstSampleNum`, so the producer
    can see each gap. Filling the gap in the stream (zeros for the lost span) would keep stream
    time on the wall clock and fix whole seconds too, which no tick can. That is
    `scripts/rsp2_stream.py` / `apps/wwv_live.c`, outside this branch. The run90g console holds
    no `errno` line; where the libusb errno=12 losses were logged was not checked.
- **Ticks cannot see whole seconds.** A loss of n s + d reads as d, and a snap or slew to it
  leaves the phase whole seconds off until a marker or a candidate 1-5 whole seconds off shows
  it. Frames that close in between can pass the decoder's marker check. On run90d the 1.115 s
  loss (about 1077 s) met a marker 7 s later, which moved the phase -1049 ms; the decoder
  dropped its 16-frame chain there. The run's one snap (-102 ms at 666 s) took back a +101 ms
  marker move made just before it. The decoder drops its chain at such a marker
  (900 ms or more) or candidate (`sync_detector_get_phase_resets`). With no marker and no
  candidate nothing shows it (test_sync_phase_track "no candidates": 312 symbols off for 5 min,
  no time of day wrong).
- **The snap assumes edges only move early, and needs the BCD pulse starts to agree.** An early
  tick cluster moves the phase only when the BCD starts since its first tick sit on the
  cluster's phase (4 or more, over twice those on the held phase); BCD starts holding the phase
  make it ignored. Tested with false bursts 100-300 ms early in a tick fade (no shift). A burst
  of false ticks and false BCD starts agreeing on one early phase is not tested; on a band
  where the BCD starts are as sparse as the ticks, a real jump goes to RECOVERING rather than
  snapping.
- **The decoder chains frames less than 900 ms off a whole number of minutes** (was `<=` 1000
  ms, fix/wwv-sync-decoder-cleanup), sync's `PHASE_WRONG_MS`. Residuals of consecutive frame starts
  over the live replays and air captures: 417 of 433 under 100 ms; the largest that chain with
  right values are 704 ms (run90g seg2) and 530 ms (run3h), both lost samples; none in 705-1045 ms.
  Every tolerance in 705-1045 ms gives the same chains there. A phase whole seconds off with no
  marker or candidate to show it still chains (the residual of a right phase that followed a 1 s
  loss and of a wrong one are the same).
- **Date DECODED wrong rose on seg2**: before, 0 right of 3 decoded; after, 9 right of 19, 10
  wrong. All DECODED (unverified); FALSE CONFIRM 0. Not traced per frame.
- `recovery_tick_seen` in `recovery_state_t` is written but never read since the holdover change
  (recovery re-locks on a marker on the held phase alone).
- **Stream times are `float` milliseconds** through the detectors, sync, the correlator and the
  decoder. The step is 1 ms from 2.33 h, 2 ms from 4.66 h, 4 ms from 9.3 h, 8 ms from 18.6 h and 16
  ms from 37 h; with holdover, a live run can stay LOCKED that long and nothing re-bases the clock.
  Not changed (fix/wwv-sync-decoder-cleanup): the longest run is run3h (3 h, step 1 ms against
  sync's 8 ms tick dead band), and the times are float where they are made (tick, marker and BCD
  detector events, the correlator's interface), so sync or the decoder in double would not remove
  the step. Where it bites first: a slew (half the median edge offset, 4-20 ms) added to the held
  anchor rounds to the step, so from 18.6 h a 4-8 ms slew moves the phase 0 or 8 ms. The decoder's
  900 ms chain tolerance and the decay's elapsed-time step tolerate a step of tens of ms. A fix is
  one time type through every module, and the producer re-basing it.
- **A marker within 5 s of a whole number of minutes is accepted and moves the anchor**
  (`confirm_marker()`'s +-5 s interval test), in every state. Not changed
  (fix/wwv-sync-decoder-cleanup): with no gap-fill upstream it is the only correction for whole
  seconds of lost samples. In the live replays, 13 marker moves of 100 ms or more, all on runs that
  lost samples (run3h, run90d, run90g seg2); the two of a second or more (run90d -1049 ms, seg2
  -1153 ms) match the loss (run90d 1.115 s; seg2's stream-to-UTC offset stepped -1153 ms). On the
  runs with no loss (run90, run90c, run90f), no marker moved the phase. Once gaps are filled
  upstream, stream time stays on UTC and a held phase should move only by the drift: a marker more
  than a few hundred ms off would then be false, and the window can be narrowed.
- **Measured (fix/wwv-bcd-corr-cleanup), not changed: BFREQ sets no boundary symbol.** In every
  window holding both, on the four captures and run90 / run90c / run90f / run3h, BTIME starts
  first. The six BFREQ-only windows on live (run90 1, run90c 4, run3h 1; 5 read P as 1) all
  start +250 ms or later, so the decoder holds them as mid seconds with no evidence.
- **BFREQ still sets the width of a window with no BTIME event, and can set its pulse offset.**
  Its durations (164-983 ms over the four runs) do not cluster at 200/500/800 ms; BFREQ-only
  windows gave 7 wrong symbols at b2ede79 (not re-measured). When a window holds both, the
  pulse offset is the earlier of the two first events (`src/correlation/bcd_correlator.c`), so
  a BFREQ event can mark a window as a boundary detection. Not changed.
- **Second 0 is not a P position** (`VALID_P_POSITIONS`, `src/correlation/bcd_symbol_classifier.c`,
  fix/wwv-sync-decoder-cleanup). It was the WWVB frame reference; 13 frames of the live replays
  carried a P there, all on the right phase, and each was refused by the decoder. Without it,
  run90c CONFIRMS the time of day 60 s and the date 120 s sooner; no value turned wrong. The
  decoder still refuses a frame with P at second 0 (the WWVB layout test).
- **Seen on air (fix/wwv-bcd-corr-cleanup), not changed.** A P at second 0 on aligned windows:
  run90 6, run90c 4, run90f 1, run3h 1 (all false; each makes the decoder refuse the frame).
  On run90d the same label caught 2 windows that were UTC :59 Ps with the phase a whole second
  off, which is what the decoder's check (`wwv_time_decoder.c`, `sym[0] != SYM_P`) relies on.
  Dropping 0 from `VALID_P_POSITIONS` removes that check; it is a decoder-side decision.
  *Superseded 2026-11-21 by the entry above (fix/wwv-sync-decoder-cleanup): the two run90d frames
  with P at second 0 (11:36, 12:03) are on the right phase on this branch and on f914843; a frame
  labelled a second off is refused on the 0s at its P seconds.*
- **The correlator's P positions include second 0** (`VALID_P_POSITIONS`,
  `src/correlation/bcd_symbol_classifier.c:20`): the WWVB frame reference. WWV sends no pulse at
  second 0, so a long event there is labelled P instead of 1. Neither is a WWV symbol; the time
  decoder rejects a frame with P at second 0. Not seen on air.
  *Superseded 2026-11-21: second 0 removed from `VALID_P_POSITIONS`; see the entry above.*
- `bcd_window_get_second_for_timestamp()` no longer has a caller inside the correlator
  (`bcd_window_get_index()` replaced it). It is kept for its declared interface, and its unused
  `corr` parameter warns.
- **Fixed (fix/wwv-bcd-corr-cleanup): `bcd_correlator_destroy()` closes the last window through
  `bcd_window_close()`**, marked partial (sent only if it holds an event), with its minute start
  and pulse offset; the old `close_window()` is removed. Before, its symbol had minute_start
  0.0, which made the decoder flush the last partial frame of every run (kiwi plain/agc: one
  extra DECODED from seconds 0-42 of 11:44; the replays' symbol counts included that frame).
- The BCD correlator CSV header lists 14 columns and the rows have 10 (audit F2).
- `marker_event_t.accumulated_energy` / `peak_energy` carry swapped quantities since 7e7805e. The
  only reader logs them.
  - **Fixed (fix/wwv-tick-marker-cleanup):** restored as before 7e7805e and commented in
    `include/marker_detector.h`: `accumulated_energy` is the run's peak window sum (it was the
    sum at the run's end, just under threshold), `peak_energy` the last frame's bucket energy.
    The sync console's `Marker event received ... energy=` now reads the peak (kiwi plain
    1 -> 7, 1 -> 4); nothing else in run_air.sh output changes.
- **Confidence decays on stream time** (fix/wwv-sync-decoder-cleanup): 0.9999 per 100 ms, applied
  for the time since the last periodic check. It was per check, every 50 ms (11.3% a minute), and
  a missed minute between the second and third markers left 0.69, short of LOCKED. In the replays
  run90g seg1 (markers 60 s then 180 s apart) never locked and now locks at 264 s; seg2 locked at
  435 s and now at 373 s. The other runs are unchanged.
- **Acquisition takes the first marker plus two minutes** (run90, run90c, run90d, run90f, run3h:
  LOCKED 119-120 s after TENTATIVE). Locking on two markers a minute apart (threshold 0.62 against
  0.70, measured, not changed) locks 58-180 s sooner in six of the seven replays (seg2 unchanged)
  and brings first CONFIRMED forward on some parts (run3h time of day 727 -> 667 s, date 1087 ->
  1027 s; run90c date 894 -> 774 s; run90d date 1023 -> 963 s), but delays others (run90 leap
  second warning 899 -> 1199 s; run90f, whose only CONFIRMED part is the LSW, 696 -> 756 s), and
  adds wrong DECODED values (run3h and run90c time of day 1 each, seg1 DUT1 3). Why the early
  frames delay a part was not traced. A candidate never starts a phase, so the minute
  before the first marker is lost even when it holds a late-started marker on the phase the
  marker then sets (run90 00:31, live15 13:08).
- **A candidate shows whole seconds by its end reading only** (fix/wwv-sync-decoder-cleanup). A
  tick-path gap-zone run that starts on a tick has its start reading on a whole second by
  construction: on the tick-gate branch, kiwi agc's 128 ms run at 462469 ms (start reading 4 s +
  59 ms off) sent a right phase to RECOVERING and lost a minute (tod 6/6, now 7/7). The run90d-shaped
  drops in test_sync_phase_track still reach RECOVERING through a candidate. A noise run whose
  end lands within 60 ms of a whole second 1-5 s off still does; none did in the replays.

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
- **The correlator's old destroy path sent its last symbol with `minute_start_ms` 0**
  (`bcd_correlator_destroy()` -> the old static `close_window()`). On saga plain and agc it closed
  the open frame early and opened one at 0.0 (`BCDS,FRAME,0.0`, scored as 12:10, all NONE). The
  decoder ignores a symbol with no minute anchor (`minute_start_ms` <= 0)
  (fix/wwv-sync-decoder-cleanup); fix/wwv-bcd-corr-cleanup fixed the destroy path. The run's last
  partial minute is no longer reported (the air captures report 6 minutes, not 7).
- **`BCDS,UTC` carries each part's pessimistic margin** (five trailing fields,
  fix/wwv-sync-decoder-cleanup), negative when the pessimistic model's best value differs.
- **CONFIRMED is margin-bound, with no lag from the state rules.** On every live replay, each part
  is CONFIRMED the minute its nominal and pessimistic margins first both reach ln 10^6, and the
  pessimistic one is the later (run3h: time of day 13.1 then 15.6 nats at frames 9 and 10, date 11.9
  then 14.0 at frames 15 and 16). A decoder-side gain means a different pessimistic model or margin,
  that is, the false-confirm gate itself. On run3h the date margin rises about 1.8 nats a frame under
  the pessimistic model, about the weight of one sent 0 at the floor rates (ln(0.15 / 0.93) =
  -1.82); which bit was not traced.
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
- **fix/wwv-sync-decoder-cleanup.** `sync_detector_set_decay_normal()` / `_recovering()` rates are
  per 100 ms of stream time, not per periodic check. `sync_detector_get_frame_time()` reports the
  second at the last periodic check. `sync_detector_marker_candidate()`'s whole-second rule reads
  `lead_b_ms` (the end reading) only. `wwv_td_result_t` gained `tod_pmargin`, `date_pmargin`,
  `dut1_pmargin`, `dst_pmargin`, `lsw_pmargin`, and `BCDS,UTC` the same five trailing fields
  (`docs/UDP_TELEMETRY_OUTPUT_PROTOCOL.md`). `wwv_time_decoder_symbol()` ignores a symbol with
  `minute_start_ms` <= 0. `VALID_P_POSITIONS` no longer holds second 0.

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

- "BCD time detector: a start must clear 2.37 times the low level, measured after each pulse"
  quotes run90f's "75 of 80 sampled from the noise term" and "threshold 1.40 x the low level"
  from the first 138 s, next to counts from the first 1175 s. Over 1175 s, 1010 of the 1036
  mid starts were set by the noise term, at 1.24 x the low level (median).

- "Tests: BCD time detector pulse width on a synthetic WWV time code" says that at 07ffb51 the
  steady case's 0s end 72-132 ms after the sent end and its 1s start at +227..+229 ms. It is 9 of
  the 11 sent 0s (the other 2 within 60 ms), and 25 of the 27 late 1s start at +226..+230 ms.
- "Time decoder: measured error model, CONFIRMED at ln 10^6" says the status bits "need 8
  frames at ln 10^6". Not measured: the rollover run (from 23:55) leaves its status bits DECODED
  after 12 minutes, 7 frames past 00:00, and CONFIRMS them all by 16 minutes.
