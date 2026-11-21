#!/usr/bin/env bash
# Stub wwv_band_scan.sh for tests/live, installed beside a copy of wwv_rsp2_live.sh. It
# opens no radio. Scan K (counted in $STUB_DIR/scans, one line of arguments each) chooses
# the K-th band of STUB_SCAN_BANDS (comma-separated kHz; "none" for no band) after the
# K-th of STUB_SCAN_SECS seconds (default 1), and writes it to --json as best_khz. The last
# entry of each list repeats. Two band entries stand for a failing API:
#   hang     the scan is stuck in the driver: deaf to SIGTERM (a child sleep, its PID in
#            $STUB_DIR/hang.pids, inherits that), no output, never ends on its own
#   apifail  the scan exits 5, as wwv_band_scan.py does when the sdrplay API is failing
set -euo pipefail
D=${STUB_DIR:?}
JSON=
args=("$@")
while [ $# -gt 0 ]; do
  case $1 in
    --json) JSON=$2; shift 2 ;;
    *) shift ;;
  esac
done
echo "${args[*]}" >> "$D/scans"
k=$(wc -l < "$D/scans")
IFS=, read -r -a bands <<< "${STUB_SCAN_BANDS:-5000}"
i=$((k - 1)); [ "$i" -ge "${#bands[@]}" ] && i=$((${#bands[@]} - 1))
b=${bands[$i]}
echo "[stub_band_scan] scan $k: ${args[*]}" >&2
IFS=, read -r -a secs <<< "${STUB_SCAN_SECS:-1}"
j=$((k - 1)); [ "$j" -ge "${#secs[@]}" ] && j=$((${#secs[@]} - 1))
if [ "$b" = hang ]; then
  trap '' TERM
  echo "[stub_band_scan] scan $k: stuck (deaf to SIGTERM)" >&2
  sleep 600 & echo $! >> "$D/hang.pids"; wait $!
  exit 1
fi
sleep "${secs[$j]}" & wait $!
if [ "$b" = apifail ]; then
  echo '{"best_khz": null, "api_failure": "stub"}' > "$JSON"; echo "BEST none (API failure)"; exit 5
fi
if [ "$b" = none ]; then
  echo '{"best_khz": null}' > "$JSON"; echo "BEST none"; exit 3
fi
echo "{\"best_khz\": $b}" > "$JSON"
echo "BEST $b"
