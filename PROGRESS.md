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
