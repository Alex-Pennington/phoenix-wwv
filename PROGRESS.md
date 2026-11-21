# Progress

## 2026-11-20 — fix/wwv-decoder-live-rates (worker, phoenix-wwv)

**What changed**

- `src/decode/wwv_time_decoder.c`, `include/wwv_time_decoder.h`: the pessimistic model's p01 and
  p10 rise with the chain's own evidence (reserved seconds read 1; P seconds read 1 or 0), pooled
  over the history and never below 0.07 / 0.15. The nominal model and the CONFIRMED threshold
  (ln 10^6) are unchanged.
- `tests/air/symbol_rates.py`: per-block symbol error rates of the decoder's input frames.
- `tests/sim_wwv_time_decoder.c`: `pP0` (sent P read as 0).
- `tests/CMakeLists.txt`: slow gates `wwv_time_decoder_sim_xlive` and `_xlive2` (1x and 2x the
  live run's rates).
- `NOTES.md`: live rates, the p10 proxy, the cost of the change, live findings not fixed.

**Verified**

- `ctest` (build/): 8/8 passed.
- `ctest -C Slow -R sim_ -j2`: 4/4 passed (x2, x3, xlive, xlive2: 0 CONFIRMED wrong each);
  `ctest -C Slow -E sim_`: 8/8 passed.
- `tests/air/run_air.sh build <out>`: FALSE CONFIRM 0 on all four captures; SYNC, SYMCB, SYMMIN,
  BCDS FRAME and BCDS UTC lines identical to main.
- `build/tests/replay_events ~/wwv-live/run90/log.txt` scored by `tests/air/decode_score.py`:
  FALSE CONFIRM 0; time of day first CONFIRMED at 3059 s (main 2219 s).
- `sim_wwv_time_decoder`, 5000 chains of 60 minutes: 0 CONFIRMED wrong at 1x, 1.5x and 2x live
  (fixed model: 0, 12, 41).

**Next**

- Coordinator review and merge; the second live run (2026-11-21 00:30 UTC) measures the change on
  air.
- Open choice: the p10 proxy counts P read as 0. With P read as 1 alone the live replay confirms
  at 2339 s; NOTES.md gives the evidence for each.
- No issues filed (brief): follow-ups are in NOTES.md.

## 2026-11-21 — fix/wwv-gap-fill (worker, phoenix-wwv)

**What changed**

- `scripts/rsp2_stream.py`: the stream is gap-exact. Losses upstream are read from arrival time
  against stream time on CLOCK_MONOTONIC (SoapySDRPlay3 0.5.3 passes no API loss signal on) and
  filled with zeros: a fast stage (2 s window, 15 ms margin, fills from 15 ms) and a slow one
  (30 s, 2.5 ms). Output is held 3 s to place each fill. GAP lines with UTC, size and cause;
  `--gap-log`, `--anchor-out`, `--hold-secs`, `--gap-max-secs` (exit 76 over it), `--no-fill`.
- `scripts/wwv_rsp2_live.sh`: passes `--anchor-out` / `--anchor-in`; reopens on exit 76.
- `apps/wwv_live.c`: a zero run of 0.5 ms or more is a fill: marked with
  `wwv_detector_manager_mark_gap()` before it is processed; GAP / GAPEND lines in the log and on
  the console; `--anchor-in`; fills and the hold kept out of the lag figures.
- `include/wwv_detector_manager.h`, `include/manager/wwv_detector_manager_internal.h`,
  `src/manager/wwv_detector_manager.c`, `src/manager/detector_routing.c`: `mark_gap` and
  `get_gap_drops`; routing holds events that overlap a filled span back from sync, correlator,
  decoder and callbacks (the observer still sees them).
- `tests/air/replay_events.c`: honours GAP lines; prints GAPDROPS.
- `tests/live/`: `stream_fake_soapy.py` modes `true:` (samples carry their true index, with
  losses) and `timelog:` (replays a recording's arrival times); `check_gapfill.py`;
  `wwv_synth.py` (synthetic WWV at 50 kHz, fills, drops, ramps); cases `gap_chain`, `gap76`,
  `stream_gaps`, `stream_ramp`, `stream_toobig`.
- `NOTES.md`: the missing loss signal, run3h's slow slip, the gap fill's limits.

**Verified** (on the branch merged with main eda145f)

- `ctest` (build/): 14/14 passed, live_wrapper_stubs included (19 stub cases).
- `tests/air/run_air.sh build <out>`: FALSE CONFIRM 0 on all four; every output line identical to
  eda145f's but the wall-clock field of TELEM lines.
- `build/tests/replay_events` on run90, run90c, run90d, run90f, run90g (both segments): output
  identical to eda145f's but that field.
- stream_gaps (0.3 s, 1.05 s, 5 s outage, 30 ms jitter): max |index error| 14.7 ms, 2.6 ms at the
  end. stream_ramp (3 ms/s for 30 s): 25.8 ms, 3.1 ms at the end.
- gap_chain: a filled 1.05 s loss after LOCKED, PHASE breaks=0 epoch=0; unfilled, breaks=1. Held
  back events equal to a recount of the overlap rule, the live GAPEND and the replay GAPDROPS.
- The saga capture's 629 s of recorded arrival times through rsp2_stream (`timelog:` mode):
  0 fills.

**Next**

- A real-radio run with the fill (none was possible: the radio was in use). The checks are in the
  worker's report.
- `docs/WWV_LIVE.md` does not describe GAP / GAPEND, `--anchor-in` or the hold.
- The open items under Live host in `NOTES.md` (no issues filed: the brief forbids it).
