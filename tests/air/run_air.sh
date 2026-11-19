#!/usr/bin/env bash
# Run the detector chain on the recorded WWV captures and score against UTC.
#
#   tests/air/run_air.sh <build-dir> <out-dir> [capture-root]
#
# capture-root defaults to ~/wwv-capture. It must hold the converted 50 kHz
# streams (conv/air_50k.f32 for the KiwiSDR capture, saga/conv/air_50k.f32 for the
# RSP2 capture), their air.json timebases, and score.py / bcd_pulse_score.py.
# Each capture runs in two wirings: carrier at DC with no AGC, and with the
# waterfall.c slow AGC (-n). Outputs: <out-dir>/<capture>_<wiring>.txt and
# score_<capture>_<wiring>.txt.
set -euo pipefail
BUILD=${1:?build dir}; OUT=${2:?out dir}; ROOT=${3:-$HOME/wwv-capture}
H="$BUILD/tests/harness_iq"
mkdir -p "$OUT"
declare -A CAP=([kiwi]="$ROOT/conv" [saga]="$ROOT/saga/conv")
pids=()
for c in kiwi saga; do
  for w in plain agc; do
    flag=(); [ "$w" = agc ] && flag=(-n)
    ( "$H" "${flag[@]}" "${CAP[$c]}/air_50k.f32" > "$OUT/${c}_${w}.txt" 2>&1
      python3 -I "$ROOT/score.py" "$OUT/${c}_${w}.txt" "${CAP[$c]}/air.json" "$c/$w" > "$OUT/score_${c}_${w}.txt"
      python3 -I "$ROOT/bcd_pulse_score.py" "$OUT/${c}_${w}.txt" "${CAP[$c]}/air.json" >> "$OUT/score_${c}_${w}.txt"
      python3 -I "$(dirname "$0")/marker_lead_score.py" "$OUT/${c}_${w}.txt" "${CAP[$c]}/air.json" >> "$OUT/score_${c}_${w}.txt"
      python3 -I "$(dirname "$0")/symbol_second_score.py" "$OUT/${c}_${w}.txt" "${CAP[$c]}/air.json" >> "$OUT/score_${c}_${w}.txt" ) &
    pids+=($!)
  done
done
rc=0; for p in "${pids[@]}"; do wait "$p" || rc=1; done
for c in kiwi saga; do for w in plain agc; do
  f="$OUT/score_${c}_${w}.txt"
  printf '%-11s ' "$c/$w"
  grep -o 'matched=[0-9]* ([0-9.]*%)' "$f" | head -1 | tr '\n' ' '
  grep -o 'false@:29/:59=[0-9]*' "$f" | tr '\n' ' '
  grep -o 'off-second=[0-9]*' "$f" | tr '\n' ' '
  printf 'tmark@:00(±200ms)=%s ' "$(grep 'tick-path TMARK' "$f" | grep -cE '= [+-]1?[0-9]?[0-9]\.[0-9] ms from' || true)"
  printf 'marks=%s ' "$(grep -c 'marker-det MARK' "$f" || true)"
  grep -oE '^MARKER LEAD: .*false\(>500 ms off\)=[0-9]+' "$f" | sed -E 's/MARKER LEAD: events=[0-9]+ within \+-100 ms of :00=([0-9]+) within \+-500 ms=([0-9]+) false\(>500 ms off\)=([0-9]+)/lead@:00 ±100=\1 ±500=\2 false=\3/' | tr '\n' ' ' 
  grep -o 'decoded(0/1/P)=[0-9]* correct=[0-9]*' "$f" | tr '\n' ' '
  grep -o '^BTIME: events=[0-9]*' "$f" | tr '\n' ' '
  grep -o '^BFREQ: events=[0-9]*' "$f" | tr '\n' ' '
  echo
done; done
exit $rc
