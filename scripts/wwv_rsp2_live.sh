#!/usr/bin/env bash
# Live WWV decode from saga's RSP2: SoapySDR -> wwv_live -> UDP telemetry on port 3005.
#
#   scripts/wwv_rsp2_live.sh [options] [-- extra wwv_live options]
#
#   --secs N            stop after N seconds (default 0: until Ctrl-C)
#   --record PREFIX     also write the raw stream: PREFIX.cs16 (8 MB/s at 2 MS/s),
#                       PREFIX.timelog.npy and PREFIX.json
#   --agc               waterfall.c slow AGC in wwv_live
#   --log FILE          wwv_live air-harness event log (the scoring scripts read it)
#   --timebase FILE     wwv_live anchor as JSON (out_utc0_unix)
#   --build DIR         where wwv_live is (default <repo>/build)
#   --wwv-khz KHZ|auto  WWV frequency: 2500, 5000 (default), 10000, 15000, 20000 or 25000;
#                       auto runs wwv_band_scan.sh first and goes live on its best band
#   --ppm X             RSP2 TCXO error (default 1.09: it read 1.09 ppm slow at 5 MHz on
#                       2026-11-19); the carrier correction is X * f_MHz Hz
#   --carrier-hz HZ     fine carrier correction added to the 100 kHz shift, instead of
#                       the --ppm one (5.45 at 5 MHz)
#   --lna N, --ifgr N   gain for the live band, instead of the band's default
#   --gain KHZ:LNA:IFGR a band's gain instead of wwv_bands.GAIN; repeatable, used by
#                       the scan and the live run
#   --scan-secs N       auto: seconds per band in the scan (default 25)
#   --scan-json FILE    auto: keep the scan's figures as JSON
#
# Receiver settings are those of the scored saga capture: tuner 100 kHz below WWV (WWV
# at +100 kHz), antenna A, AGC off, 2 MS/s. Gain is per band, from scripts/wwv_bands.py:
# LNA state 2 / IF gain reduction 59 dB at 2.5-15 MHz, LNA 1 / IFGR 59 at 20-25 MHz.
# wwv_live scales by 32, the level of the 5 MHz capture.
#
# sdrplay.service is started here, once (auto mode scans and streams under the same
# start), and stopped on every exit path (end, Ctrl-C, error): it stays disabled
# except while recording. The last line printed is the service state after the stop.
# The vendor service can crash on stop; the script then reports it and clears the
# unit's failed state.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(dirname "$HERE")
BUILD=$REPO/build
SECS=0; RECORD=; AGC=; LOG=; TIMEBASE=; CARRIER=; PPM=1.09; LNA=; IFGR=
WWV_KHZ=5000; OFFSET_KHZ=100; RATE=2000000; SCALE=32; SCAN_SECS=25; SCAN_JSON=
GAINS=()
while [ $# -gt 0 ]; do
  case $1 in
    --secs) SECS=$2; shift 2 ;;
    --record) RECORD=$2; shift 2 ;;
    --agc) AGC=1; shift ;;
    --log) LOG=$2; shift 2 ;;
    --timebase) TIMEBASE=$2; shift 2 ;;
    --build) BUILD=$2; shift 2 ;;
    --carrier-hz) CARRIER=$2; shift 2 ;;
    --ppm) PPM=$2; shift 2 ;;
    --wwv-khz) WWV_KHZ=$2; shift 2 ;;
    --lna) LNA=$2; shift 2 ;;
    --ifgr) IFGR=$2; shift 2 ;;
    --gain) GAINS+=(--gain "$2"); shift 2 ;;
    --scan-secs) SCAN_SECS=$2; shift 2 ;;
    --scan-json) SCAN_JSON=$2; shift 2 ;;
    --) shift; break ;;
    -h|--help) sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
EXTRA=("$@")
case $WWV_KHZ in
  auto|2500|5000|10000|15000|20000|25000) ;;
  *) echo "--wwv-khz: want 2500, 5000, 10000, 15000, 20000, 25000 or auto, not $WWV_KHZ" >&2; exit 2 ;;
esac
SDRPLAY_TAG=wwv_rsp2_live
. "$HERE/sdrplay_service.sh"

WWV_LIVE=$BUILD/wwv_live
[ -x "$WWV_LIVE" ] || { echo "wwv_live not found at $WWV_LIVE (cmake -S $REPO -B $BUILD && cmake --build $BUILD)" >&2; exit 2; }

if [ -n "$RECORD" ]; then
  mkdir -p "$(dirname "$RECORD")"
  free=$(df -B1 --output=avail "$(dirname "$RECORD")" | tail -1)
  if [ "$SECS" != 0 ]; then
    need=$(python3 -I -c "print(int($SECS * $RATE * 4 + 2e9))")
    if [ "$free" -lt "$need" ]; then
      echo "not enough disk for --record: need $((need / 1000000000)) GB ($SECS s at 8 MB/s + 2 GB), have $((free / 1000000000)) GB" >&2
      exit 2
    fi
  else
    echo "warning: --record with no --secs: 8 MB/s (29 GB/hour), $((free / 1000000000)) GB free" >&2
  fi
fi

SCPID=; SPID=; WPID=; FIFODIR=
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  if [ -n "$SCPID" ] && kill -0 "$SCPID" 2>/dev/null; then
    kill -TERM "$SCPID" 2>/dev/null || true
  fi
  [ -n "$SCPID" ] && wait "$SCPID" 2>/dev/null || true
  # Streamer first: wwv_live then reads end of input and finishes its report.
  # The API service must not be stopped under an open stream.
  if [ -n "$SPID" ] && kill -0 "$SPID" 2>/dev/null; then
    kill -TERM "$SPID" 2>/dev/null || true
  fi
  [ -n "$SPID" ] && wait "$SPID" 2>/dev/null || true
  if [ -n "$WPID" ]; then
    for _ in $(seq 50); do kill -0 "$WPID" 2>/dev/null || break; sleep 0.1; done
    kill -TERM "$WPID" 2>/dev/null || true
    wait "$WPID" 2>/dev/null || true
  fi
  [ -n "$FIFODIR" ] && /bin/rm -rf "$FIFODIR"
  sdrplay_stop
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

FIFODIR=$(mktemp -d)
sdrplay_start

if [ "$WWV_KHZ" = auto ]; then
  # The scan runs under this script's service start; in the background, so a
  # signal reaches the traps at once.
  SJ=${SCAN_JSON:-$FIFODIR/scan.json}
  echo "[wwv_rsp2_live] --wwv-khz auto: scanning the WWV bands, $SCAN_SECS s each" >&2
  "$HERE/wwv_band_scan.sh" --no-service --secs "$SCAN_SECS" --json "$SJ" "${GAINS[@]}" --ppm "$PPM" >&2 &
  SCPID=$!
  src=0
  wait "$SCPID" || src=$?
  SCPID=
  WWV_KHZ=$(python3 -I -c "import json,sys; b=json.load(open(sys.argv[1])).get('best_khz'); print(b if b else '')" "$SJ" 2>/dev/null || true)
  if [ -z "$WWV_KHZ" ]; then
    echo "[wwv_rsp2_live] the band scan found no band (exit $src); not going live" >&2
    exit 3
  fi
  echo "[wwv_rsp2_live] going live on the scan's best band: $WWV_KHZ kHz" >&2
fi

[ -n "$CARRIER" ] || CARRIER=$(python3 -I -c "print(f'{$PPM * $WWV_KHZ / 1000:.3f}')")
SHIFT=$(python3 -I -c "print(f'{$OFFSET_KHZ * 1000 + $CARRIER:.3f}')")
WL_ARGS=(-f s16 -r "$RATE" -s "$SHIFT" -g "$SCALE")
[ -n "$AGC" ] && WL_ARGS+=(-a)
[ -n "$LOG" ] && WL_ARGS+=(-l "$LOG")
[ -n "$TIMEBASE" ] && WL_ARGS+=(-j "$TIMEBASE")
ST_ARGS=(--wwv-khz "$WWV_KHZ" --offset-khz "$OFFSET_KHZ" --rate "$RATE" --secs "$SECS" "${GAINS[@]}")
[ -n "$LNA" ] && ST_ARGS+=(--lna "$LNA")
[ -n "$IFGR" ] && ST_ARGS+=(--ifgr "$IFGR")
[ -n "$RECORD" ] && ST_ARGS+=(--record "$RECORD")
echo "[wwv_rsp2_live] WWV $WWV_KHZ kHz, tuner $((WWV_KHZ - OFFSET_KHZ)) kHz, carrier correction $CARRIER Hz, shift $SHIFT Hz" >&2

# Two background processes joined by a FIFO, so a signal to this script alone
# (Ctrl-C, kill, systemd) reaches the traps at once: `wait` returns on a signal,
# a foreground pipeline would not.
mkfifo "$FIFODIR/iq"
python3 -I "$HERE/rsp2_stream.py" "${ST_ARGS[@]}" > "$FIFODIR/iq" &
SPID=$!
"$WWV_LIVE" "${WL_ARGS[@]}" "${EXTRA[@]}" < "$FIFODIR/iq" &
WPID=$!
rc=0
wait "$WPID" || rc=$?
WPID=
wait "$SPID" || { src=$?; [ "$rc" = 0 ] && rc=$src; }
SPID=
exit "$rc"
