#!/usr/bin/env bash
# Rank the WWV bands on saga's RSP2: carrier C/N0, 1 kHz tick contrast, ADC overload.
#
#   scripts/wwv_band_scan.sh [options]
#
#   --secs N               seconds per band (default 25)
#   --bands LIST           kHz, comma-separated (default 2500,5000,10000,15000,20000,25000)
#   --gain KHZ:LNA:IFGR    a band's gain instead of wwv_bands.GAIN; repeatable
#   --ppm X                TCXO error used to place the carrier (default 1.09)
#   --json FILE            write every figure as JSON
#   --overload-plugin DIR  SoapySDRPlay3 build that logs the API's ADC overload events
#                          (default $WWV_OVERLOAD_PLUGIN, else wwv-capture's
#                          saga/soapysdrplay3-src/plugin-overloadlog when present)
#   --no-overload-plugin   use the installed module: no API overload count, clipping only
#   --no-service           do not start or stop sdrplay.service (the caller holds it)
#
# Prints the ranked table and a last stdout line `BEST <kHz>` (`BEST none` when no band
# could be measured without overload). Exit 0 with a best band, 3 without one.
#
# sdrplay.service is started once, here, and stopped on every exit path (end, Ctrl-C,
# error). The last stderr line is the service state after the stop.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SDRPLAY_TAG=wwv_band_scan
. "$HERE/sdrplay_service.sh"

PLUGIN=${WWV_OVERLOAD_PLUGIN:-$HOME/wwv-capture/saga/soapysdrplay3-src/plugin-overloadlog}
SERVICE=1
PY_ARGS=()
while [ $# -gt 0 ]; do
  case $1 in
    --secs|--bands|--gain|--ppm|--json) PY_ARGS+=("$1" "$2"); shift 2 ;;
    --overload-plugin) PLUGIN=$2; shift 2 ;;
    --no-overload-plugin) PLUGIN=; shift ;;
    --no-service) SERVICE=; shift ;;
    -h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

ENV=()
if [ -n "$PLUGIN" ]; then
  if [ -f "$PLUGIN/libsdrPlaySupport.so" ]; then
    ENV=(SOAPY_SDR_ROOT=/nonexistent "SOAPY_SDR_PLUGIN_PATH=$PLUGIN")
  else
    echo "[wwv_band_scan] no overload-logging module at $PLUGIN: ADC clipping only" >&2
  fi
fi

PID=
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
    kill -TERM "$PID" 2>/dev/null || true
  fi
  [ -n "$PID" ] && wait "$PID" 2>/dev/null || true
  [ -n "$SERVICE" ] && sdrplay_stop
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

[ -n "$SERVICE" ] && sdrplay_start

# In the background, so a signal reaches the traps at once.
env "${ENV[@]}" python3 -I "$HERE/wwv_band_scan.py" "${PY_ARGS[@]}" &
PID=$!
rc=0
wait "$PID" || rc=$?
PID=
exit "$rc"
