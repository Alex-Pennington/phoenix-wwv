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
#   --carrier-hz HZ     fine carrier correction added to the 100 kHz shift (default 5.45:
#                       the RSP2's TCXO read 1.09 ppm slow at 5 MHz on 2026-11-19)
#
# Receiver settings are rsp2_stream.py's defaults, those of the scored saga capture:
# WWV 5000 kHz, tuner 4900 kHz (WWV at +100 kHz), antenna A, AGC off, LNA state 2,
# IF gain reduction 59 dB, 2 MS/s. wwv_live scales by 32, the level of that capture.
#
# sdrplay.service is started here and stopped on every exit path (end, Ctrl-C,
# error): it stays disabled except while recording. The last line printed is the
# service state after the stop.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(dirname "$HERE")
BUILD=$REPO/build
SECS=0; RECORD=; AGC=; LOG=; TIMEBASE=; CARRIER=5.45
WWV_KHZ=5000; OFFSET_KHZ=100; RATE=2000000; SCALE=32
while [ $# -gt 0 ]; do
  case $1 in
    --secs) SECS=$2; shift 2 ;;
    --record) RECORD=$2; shift 2 ;;
    --agc) AGC=1; shift ;;
    --log) LOG=$2; shift 2 ;;
    --timebase) TIMEBASE=$2; shift 2 ;;
    --build) BUILD=$2; shift 2 ;;
    --carrier-hz) CARRIER=$2; shift 2 ;;
    --) shift; break ;;
    -h|--help) sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
EXTRA=("$@")

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

SHIFT=$(python3 -I -c "print(f'{$OFFSET_KHZ * 1000 + $CARRIER:.3f}')")
WL_ARGS=(-f s16 -r "$RATE" -s "$SHIFT" -g "$SCALE")
[ -n "$AGC" ] && WL_ARGS+=(-a)
[ -n "$LOG" ] && WL_ARGS+=(-l "$LOG")
[ -n "$TIMEBASE" ] && WL_ARGS+=(-j "$TIMEBASE")
ST_ARGS=(--wwv-khz "$WWV_KHZ" --offset-khz "$OFFSET_KHZ" --rate "$RATE" --secs "$SECS")
[ -n "$RECORD" ] && ST_ARGS+=(--record "$RECORD")

STARTED=; SPID=; WPID=; FIFODIR=
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
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
  if [ -n "$STARTED" ]; then
    echo "[wwv_rsp2_live] stopping sdrplay.service" >&2
    sudo systemctl stop sdrplay.service || echo "[wwv_rsp2_live] ERROR: systemctl stop sdrplay.service failed" >&2
  fi
  echo "[wwv_rsp2_live] sdrplay.service: $(systemctl is-active sdrplay.service || true)" >&2
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "[wwv_rsp2_live] starting sdrplay.service" >&2
STARTED=1
sudo systemctl start sdrplay.service
echo "[wwv_rsp2_live] sdrplay.service: $(systemctl is-active sdrplay.service || true)" >&2

# Two background processes joined by a FIFO, so a signal to this script alone
# (Ctrl-C, kill, systemd) reaches the traps at once: `wait` returns on a signal,
# a foreground pipeline would not.
FIFODIR=$(mktemp -d)
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
