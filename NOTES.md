# Notes: issues found while fixing the WWV detectors, not fixed on this branch

Measured with `tests/air/run_air.sh` against the KiwiSDR (kb5ag, 5 MHz) and Saga RSP2 (5 MHz)
captures in `~/wwv-capture`. "plain" is carrier at DC with no AGC; "agc" adds the waterfall.c
`normalize()`.

## Detection

- **BCD symbol accuracy is now limited by the BTIME detector, not the anchor or the
  correlator's window logic.** Symbols are 73-83% correct (70/96, 231/294, 257/339, 178/215),
  and every symbol reports the right second-in-minute. `tests/air/trace_all.sh <out-dir>` gives
  every wrong symbol a cause. What remains, on kiwi plain / kiwi agc / saga plain / saga agc:
  - width 10/41/55/27: the window holds the pulse at the second, and the detector measured its
    width wrong. The class medians are right (0: 220-250 ms, 1: 497-532 ms, P: 788-876 ms) and
    the 350/650 ms cuts sit between them, but each class has a wide tail (0 p90 425-502 ms).
    That is detector width noise, not a threshold error.
  - spurious 16/21/25/10: no BTIME event starts near the second. The window classifies a
    mid-second event, which is right about half the time. Emitting NONE for these windows was
    measured at 85001f9 and not adopted (correct fell to 43/38/73/116). The anchor is now right
    (every window on all four runs opens 0-150 ms before its second; minutes anchored by the
    marker detector alone sit -52..+100 ms off :00, tick-path ones -2..+15 ms), so it can be
    re-measured.
  - misaligned 0/1/2/0: all three are duplicate windows (Sync and correlator, below).
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

- **A second is emitted twice when the minute anchor is replaced while its window is open.** The
  correlator indexes windows by whole seconds from the anchor (1b3b401). When the next minute's
  anchor arrives during second 1 or 2, the open window's index changes from 61 to 1. The window
  is closed under the old anchor and a new one opens for the same second. The first copy holds
  the boundary pulse; the second holds only the later mid-second event. 6 seconds at the branch
  tip (kiwi agc 11:40:01, 11:41:01, 11:42:01; saga plain 12:13:02, 12:14:02, 12:16:01), 2 at
  85001f9. Both copies report the right second, so a frame decoder sees two symbols for it. An
  anchor a whole number of minutes from the old one should keep the open window.
- **`src/manager/detector_routing.c` does not wire sync the way the harness does.** It passes
  the tick-path marker's `timestamp_ms` (trailing edge, +0.8 s) to
  `sync_detector_tick_marker()` and never feeds marker-detector events to sync. The sync header
  now says both entry points take the leading edge. waterfall.c (outside this repo) must pass
  `marker_event_t.start_timestamp_ms` to `sync_detector_marker_event()`, as the harness now does.
- `sync_detector_full_reset()` does not clear `last_confirmed_ms`, so a stale anchor survives a
  reset. The re-anchor rule mitigates it.
- The sync detector gets no per-tick events in the waterfall.c wiring, so tick-hole evidence never
  arrives.
- The BCD correlator averages BFREQ's duration with BTIME's when a window holds both. BFREQ fires
  mostly on P-marker clusters, and its durations (164-983 ms over the four runs) do not cluster
  at 200/500/800 ms. No air window is wrong because of it today (freq-avg 0 on all runs). BFREQ-only windows
  give 7 wrong symbols at b2ede79.
- The correlator opens a window only when an event arrives. A second with no BCD event produces
  no callback at all, not a NONE symbol, so a frame decoder cannot tell "no pulse" from "not
  locked".
- `bcd_window_get_second_for_timestamp()` no longer has a caller inside the correlator
  (`bcd_window_get_index()` replaced it). It is kept for its declared interface, and its unused
  `corr` parameter warns.
- The BCD correlator CSV header lists 14 columns and the rows have 10. `bcd_correlator_destroy()`
  still closes the last window through the old static `close_window()` (`SYM`/`CORR` format). Two
  output formats coexist (audit F2).
- `marker_event_t.accumulated_energy` / `peak_energy` carry swapped quantities since 7e7805e. The
  only reader logs them.

## Interfaces changed on this branch

- `data_channel_t` gained `hp[2]`, and `sync_channel_t` is now `bp[4]` (same size). Consumers must
  rebuild.
- `marker_event_t` gained `start_timestamp_ms`, and `duration_ms` now carries the de-stretched
  length. The marker detector no longer reports runs that fail the tone-purity test.
- `sync_detector_marker_event()` and `sync_detector_tick_marker()` take the marker's leading
  edge. `tick_marker_event_t.start_timestamp_ms` comes from the trailing edge for off-length
  runs. `sync_detector_periodic_check()` now confirms a lone pending marker after 3 s. `bcd_symbol_event_t` gained `second`, and the `SYMBOL` telemetry line gained a trailing
  `second` field.

## Documentation

- `docs/wwv_bcd_decoder_algorithm.md:271-288` documents the WWVB bit layout (minutes at seconds
  1-8, MSB first) under a "WWV Frame Structure" heading. WWV's layout differs (NIST SP 432). Not
  fixed here.
- README claims and the audit's F10 items are untouched.
