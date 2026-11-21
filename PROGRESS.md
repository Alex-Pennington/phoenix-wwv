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
  (Done 2026-11-21 in 8ec94eb.)
- The open items under Live host in `NOTES.md` (no issues filed: the brief forbids it).

## 2026-11-21 — ymir-phoenix-wwv (coordinator, phoenix-wwv)

**What was merged** (`git log --merges 0c146d0..424ad81`)

- 8ec94eb: `docs/WWV_LIVE.md` gap fill, scan bound, reopen-max; live_wrapper_stubs 437 s of 600.
- 83a20f8: 15 MHz BCD quality (NOTES only): 1->0 errors match 5 MHz bin for bin at equal contrast,
  lost samples cause most wrong dates; BTIME lead +55/+62 ms on both bands. 93678df: its scripts
  (`seg_attr.py`, `shift_loss.py`, `confusion_causes.py`) in `tests/air`, outputs byte-identical.
- 4fbcbf6: one stream time type, `wwv_time_ms_t` (double ms), detector to decoder; float ms stepped
  8 ms by 18.6 h. 35 replays 0 FALSE CONFIRM, same first-CONFIRMED times. b3553e1: detector test at
  0 h and 40 h, 267 stamps exact (float: 255/267 off, up to 7.69 ms).
- aef316f: wrapper MEM lines at start, reopen and errno=12 burst; crash backoff 5 s doubling to
  120 s; `--mem-wait-secs` opt-in, default 0. Why: runG ran with 0 errno=12 while Unmovable +
  Reclaimable had 0 free blocks at order 9 or above, so the order-9 premise is falsified.
- c48234e: pessimistic p10 floor 0.15 -> 0.10. Time of day CONFIRMS up to 8 min sooner (1-2 on
  most runs). Independent review: no blocker; per-bit risk about 10x the old floor (1.1e-6 at a
  12.4% 1->0 rate).
- fcd29c0: stream slip. runG's slow stage filled 1054.6 ms never lost (delivery delay growing
  50-170 ppm); the fill now tracks the lag rate (Theil-Sen, 300 s); a runG-like drift fills 0.0 ms
  (was 1185.8). ffba5d4: shared stream-UTC truth (`tests/air/stream_utc.py`) for the air scorers.
- 7d63b2e: full sync reset clears the anchor; marker and BCD-freq startup guards count from
  start_frame. 36 live replays byte-identical.
- 21d7bc2: `--antenna NAME` through the wrapper and band scan, checked against listAntennas().
- b6abcc7: CLOCK line, host clock error against confirmed WWV time; synthetic anchor errors
  recovered within +1.4..+3.6 ms at sigma 3 ms; clean saga segments -4..-11 ms.
- 2a93047: BCD TIME step fit at contrast 1.3-3: wrong 1669 -> 833, right 26587 -> 27602, holds out
  of sample (fit on either half, tested on the other). Ceiling kept at 3. Cost: saga agc DST no
  longer CONFIRMS (630 s on main).
- 2174441: `--band-cache` with locked_utc refreshed while LOCKED; skips the ~7.5 min scan.
- bfcf6fa: mawk buffered the FIFO, so LOCKED reached the filter minutes late (runH1); the wrapper
  picks gawk, else `mawk -W interactive`, else busybox awk.
- 424ad81: live stub suite split into five ctest entries; 538 s -> 171-175 s wall at `-j4`.

**runG** (saga, 2026-11-21 09:58-12:38 UTC, 5 MHz; crash and memory figures from the
coordinator's log reads)

- 0 errno=12 lines and 0 `sdrplay_apiService` core dumps.
- Time of day and date CONFIRMED right 146/146; FALSE CONFIRM 0 (NOTES.md, p10 floor item).
- Order-9/10 free blocks were 0 in about half the minutes from 10:12 UTC, before the copy and
  stick-write confounders began. So the `vm.min_free_kbytes` result is consistent with no crashes,
  and fragmentation is not shown to be the trigger.

**gangleri** (coordinator's setup and log reads)

- The RSP2 moved from saga to gangleri: Zotac ZBOX CI320 nano, headless, Mint live USB, WireGuard
  10.77.77.23, Hi-Z fan dipole, built from `gangleri-linux-install` (local on saga).
- Runs 24/7 as user unit `wwv-live.service` on bfcf6fa, one run directory per start under
  `~/wwv-live`; saga pulls the logs into `~/wwv-gangleri`.
- runH1/H2/H3, 10 and 15 MHz: Hi-Z C/N0 62-80 dB-Hz on 5-20 MHz; CLOCK steady at about -19 ms,
  sigma 3 ms; delivery lag +4 to +105 ppm; 0 fills.

**Open**

- PHASE_LEAD_* against the measured edge leads (NOTES.md, Sync and correlator).
- TENTATIVE has no timeout.
- Pipe buffering in the other pipeline tools (grep, sed, python) is unchecked.
- `sdrplay_apiService` dumps core on SIGTERM.
- ALE (greybeard-ale) is to take over gangleri's RSP2 later and keep WWV clock sync; a decoder
  state save and restore is requested.
- fix/wwv-stream-activate-hang: stopped by Alex, left as is.
