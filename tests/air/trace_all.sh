#!/usr/bin/env bash
# Run bcd_window_trace.py on the four outputs of run_air.sh.
#   tests/air/trace_all.sh <out-dir> [capture-root] [--rows]
set -euo pipefail
OUT=${1:?out dir}; ROOT=${2:-$HOME/wwv-capture}; shift; [ $# -gt 0 ] && shift
declare -A CAP=([kiwi]="$ROOT/conv" [saga]="$ROOT/saga/conv")
for c in kiwi saga; do for w in plain agc; do
  echo "== $c/$w"
  python3 -I "$(dirname "$0")/bcd_window_trace.py" "$OUT/${c}_${w}.txt" "${CAP[$c]}/air.json" "$@"
done; done
