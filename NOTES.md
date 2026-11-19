# Notes: issues found while fixing the WWV detectors, not fixed on this branch

Measured with `tests/air/run_air.sh` against the KiwiSDR (kb5ag, 5 MHz) and Saga RSP2 (5 MHz)
captures in `~/wwv-capture`. "plain" is carrier at DC with no AGC; "agc" adds the waterfall.c
`normalize()`.

## Detection

- **BCD correlator accuracy is the BCD bottleneck.** BTIME pulses that start within -50..+200 ms
  of a UTC second are width-classified correctly 85% of the time (kiwi plain 102/121, saga plain
  306/360). Correlator symbols are 47-59% correct, whichever source anchored sync, with no peak at
  a +-1 s shift. The window logic in `src/correlation/` loses the accuracy. It is not traced.
- **`bcd_freq` cannot detect per-second pulses.** It thresholds a 1 s sliding sum of 100 Hz
  energy at 3x a baseline that tracks that same sum. The subcarrier is present every second, so
  the sum barely moves. It fires only on the P-marker clusters: saga BFREQ events land at UTC
  :09.7, :19.7, :29.8 and :49.8. There are 3-15 events per 629 s run.
- **The marker detector fires on the :52.5 voice announcement in the AGC wiring.** On kiwi agc
  there are 6 false events, 4 of which start at :52.8-:56.3. The other two, at 11:39:47.7 and
  11:40:19.8, are not identified.
  - Cause, measured on kiwi agc: with the 800-1400 Hz Butterworth sync filter, the voice window's
    p90 marker-bucket sum is 3.45x the tone-period median, over the 3x threshold. With the old
    filter it was 1.95x.
  - A length bound cannot separate voice from weak real markers: real f spans 0.03-0.69, voice
    f spans 0.27-0.94.
  - A tone-purity test (bins 4-6 against bins 7-8) would discriminate. It is not done.
- **Wire `marker_event_t.start_timestamp_ms` into the sync detector.** waterfall.c, and so the
  harness, passes `timestamp_ms`, the end of the window run (+1.3-1.8 s). Minutes anchored on a
  marker-detector event therefore start late, and BCD second indices come out 2 s off: 28/161 on
  saga plain, 54/98 on kiwi agc.
  - Passing `start_timestamp_ms` makes saga plain 156/156 exact.
  - On kiwi agc the voice false markers then anchor at +4 s (BCD correct 50 -> 43).
  - Fix the voice false markers first.
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
- The sync detector gets no per-tick events in the waterfall.c wiring, so tick-hole evidence never
  arrives.
- The BCD correlator CSV header lists 14 columns and the rows have 10. `bcd_correlator_destroy()`
  still closes the last window through the old static `close_window()` (`SYM`/`CORR` format). Two
  output formats coexist (audit F2).
- `marker_event_t.accumulated_energy` / `peak_energy` carry swapped quantities since 7e7805e. The
  only reader logs them.

## Interfaces changed on this branch

- `data_channel_t` gained `hp[2]`, and `sync_channel_t` is now `bp[4]` (same size). Consumers must
  rebuild.
- `marker_event_t` gained `start_timestamp_ms`, and `duration_ms` now carries the de-stretched
  length. `bcd_symbol_event_t` gained `second`, and the `SYMBOL` telemetry line gained a trailing
  `second` field.

## Documentation

- `docs/wwv_bcd_decoder_algorithm.md:271-288` documents the WWVB bit layout (minutes at seconds
  1-8, MSB first) under a "WWV Frame Structure" heading. WWV's layout differs (NIST SP 432). Not
  fixed here.
- README claims and the audit's F10 items are untouched.
