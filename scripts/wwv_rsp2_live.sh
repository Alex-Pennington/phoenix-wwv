#!/usr/bin/env bash
# Live WWV decode from saga's RSP2: SoapySDR -> wwv_live -> UDP telemetry on port 3005.
#
#   scripts/wwv_rsp2_live.sh [options] [-- extra wwv_live options]
#
#   --secs N            stop after N seconds of live run (default 0: until Ctrl-C); a
#                       band switch's re-scan counts toward N
#   --record PREFIX     also write the raw stream: PREFIX.cs16 (8 MB/s at 2 MS/s),
#                       PREFIX.timelog.npy and PREFIX.json
#   --agc               waterfall.c slow AGC in wwv_live
#   --log FILE          wwv_live air-harness event log (the scoring scripts read it)
#   --timebase FILE     wwv_live anchor as JSON (out_utc0_unix)
#   --build DIR         where wwv_live is (default <repo>/build)
#   --wwv-khz KHZ|auto  WWV frequency: 2500, 5000 (default), 10000, 15000, 20000 or 25000;
#                       auto runs wwv_band_scan.sh first and goes live on its band
#   --lock-mins N       auto: when the band has not reached LOCKED N minutes after it
#                       went live, re-scan without it and switch (default 8; 0: never)
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
# A band switch stops the streamer and wwv_live (its sync and decoder start again),
# re-scans and goes live again. Every switch is logged on stderr. Segment 1 writes the
# --log, --timebase, --scan-json, --record and wwv_live --frames names as given; segment
# K writes them with .segK before the extension (run.log -> run.seg2.log; PREFIX ->
# PREFIX.seg2).
# --carrier-hz, --lna and --ifgr hold for the first band only.
#
# sdrplay.service is started here, once (scans and live segments all run under the
# same start), and stopped on every exit path (end, Ctrl-C, error): it stays disabled
# except while recording. The last line printed is the service state after the stop.
# The vendor service can crash on stop; the script then reports it and clears the
# unit's failed state.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(dirname "$HERE")
BUILD=$REPO/build
SECS=0; RECORD=; AGC=; LOG=; TIMEBASE=; CARRIER=; PPM=1.09; LNA=; IFGR=
WWV_KHZ=5000; OFFSET_KHZ=100; RATE=2000000; SCALE=32; SCAN_SECS=25; SCAN_JSON=; LOCK_MINS=8
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
    --lock-mins) LOCK_MINS=$2; shift 2 ;;
    --) shift; break ;;
    -h|--help) sed -n '2,44p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
EXTRA=("$@")
case $WWV_KHZ in
  auto|2500|5000|10000|15000|20000|25000) ;;
  *) echo "--wwv-khz: want 2500, 5000, 10000, 15000, 20000, 25000 or auto, not $WWV_KHZ" >&2; exit 2 ;;
esac
[[ $LOCK_MINS =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo "--lock-mins: want minutes, not $LOCK_MINS" >&2; exit 2; }
AUTO=; [ "$WWV_KHZ" = auto ] && AUTO=1
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

say() { echo "[wwv_rsp2_live] $(date -u +%Y-%m-%dT%H:%M:%SZ) $*" >&2; }

# seg_name NAME K [append]: NAME for segment 1; NAME with .segK before its extension
# (or appended, for a --record prefix or a name without one) for segment K.
seg_name() {
  local name=$1 k=$2 how=${3:-}
  [ -z "$name" ] || [ "$k" = 1 ] && { printf '%s' "$name"; return; }
  local dir base
  dir=$(dirname "$name"); base=$(basename "$name")
  if [ -z "$how" ] && [[ $base == ?*.* ]]; then
    printf '%s/%s.seg%s.%s' "$dir" "${base%.*}" "$k" "${base##*.}"
  else
    printf '%s.seg%s' "$name" "$k"
  fi
}

# seg_extra K: EXTRA with wwv_live's --frames file named for segment K (it is opened "wb").
seg_extra() {
  SEG_EXTRA=()
  local i=0 n=${#EXTRA[@]} x
  while [ "$i" -lt "$n" ]; do
    x=${EXTRA[$i]}
    case $x in
      -F|--frames)
        SEG_EXTRA+=("$x")
        i=$((i + 1))
        [ "$i" -lt "$n" ] && SEG_EXTRA+=("$(seg_name "${EXTRA[$i]}" "$1")") ;;
      --frames=*) SEG_EXTRA+=("--frames=$(seg_name "${x#--frames=}" "$1")") ;;
      *) SEG_EXTRA+=("$x") ;;
    esac
    i=$((i + 1))
  done
}

SCPID=; SPID=; WPID=; RPID=; FIFODIR=
# stop_live: streamer first, so wwv_live reads end of input and finishes its report,
# then wwv_live, then the output filter. The API service must not be stopped under an
# open stream.
stop_live() {
  if [ -n "$SPID" ] && kill -0 "$SPID" 2>/dev/null; then
    kill -TERM "$SPID" 2>/dev/null || true
  fi
  [ -n "$SPID" ] && { wait "$SPID" 2>/dev/null || true; }
  SPID=
  if [ -n "$WPID" ]; then
    for _ in $(seq 50); do kill -0 "$WPID" 2>/dev/null || break; sleep 0.1; done
    kill -TERM "$WPID" 2>/dev/null || true
    wait "$WPID" 2>/dev/null || true
  fi
  WPID=
  if [ -n "$RPID" ]; then
    for _ in $(seq 20); do kill -0 "$RPID" 2>/dev/null || break; sleep 0.1; done
    kill -TERM "$RPID" 2>/dev/null || true
    wait "$RPID" 2>/dev/null || true
  fi
  RPID=
}
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  if [ -n "$SCPID" ] && kill -0 "$SCPID" 2>/dev/null; then
    kill -TERM "$SCPID" 2>/dev/null || true
  fi
  [ -n "$SCPID" ] && { wait "$SCPID" 2>/dev/null || true; }
  stop_live
  [ -n "$FIFODIR" ] && /bin/rm -rf "$FIFODIR"
  sdrplay_stop
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

FIFODIR=$(mktemp -d)
sdrplay_start

SEG=1
HISTORY=()
# scan [--avoid KHZ]: run the band scan under this script's service start, in the
# background so a signal reaches the traps at once. Sets BAND to its choice, or empty.
scan() {
  local sj
  sj=$(seg_name "${SCAN_JSON:-$FIFODIR/scan.json}" "$SEG")
  say "scanning the WWV bands, $SCAN_SECS s each${1:+, avoiding $2 kHz}; figures in $sj"
  "$HERE/wwv_band_scan.sh" --no-service --secs "$SCAN_SECS" --json "$sj" "${GAINS[@]}" --ppm "$PPM" "$@" >&2 &
  SCPID=$!
  local src=0
  wait "$SCPID" || src=$?
  SCPID=
  BAND=$(python3 -I -c "import json,sys; b=json.load(open(sys.argv[1])).get('best_khz'); print(b if b else '')" "$sj" 2>/dev/null || true)
  [ -n "$BAND" ] || say "the band scan found no band (exit $src)"
}

if [ -n "$AUTO" ]; then
  scan
  [ -n "$BAND" ] || { say "not going live"; exit 3; }
  WWV_KHZ=$BAND
  say "going live on the scan's band: $WWV_KHZ kHz"
fi

END=
[ "$SECS" != 0 ] && END=$(python3 -I -c "import time; print(time.time() + $SECS)")
rc=0
while :; do
  REMAIN=0
  if [ -n "$END" ]; then
    REMAIN=$(python3 -I -c "print(max(0.0, round($END - __import__('time').time(), 1)))")
    if [ "$REMAIN" = 0.0 ]; then say "--secs reached during the re-scan"; break; fi
  fi
  [ -n "$CARRIER" ] && [ "$SEG" = 1 ] && C=$CARRIER || C=$(python3 -I -c "print(f'{$PPM * $WWV_KHZ / 1000:.3f}')")
  SHIFT=$(python3 -I -c "print(f'{$OFFSET_KHZ * 1000 + $C:.3f}')")
  WL_ARGS=(-f s16 -r "$RATE" -s "$SHIFT" -g "$SCALE")
  [ -n "$AGC" ] && WL_ARGS+=(-a)
  [ -n "$LOG" ] && WL_ARGS+=(-l "$(seg_name "$LOG" "$SEG")")
  [ -n "$TIMEBASE" ] && WL_ARGS+=(-j "$(seg_name "$TIMEBASE" "$SEG")")
  ST_ARGS=(--wwv-khz "$WWV_KHZ" --offset-khz "$OFFSET_KHZ" --rate "$RATE" --secs "$REMAIN" "${GAINS[@]}")
  if [ "$SEG" = 1 ]; then
    [ -n "$LNA" ] && ST_ARGS+=(--lna "$LNA")
    [ -n "$IFGR" ] && ST_ARGS+=(--ifgr "$IFGR")
  fi
  [ -n "$RECORD" ] && ST_ARGS+=(--record "$(seg_name "$RECORD" "$SEG" append)")
  say "segment $SEG: WWV $WWV_KHZ kHz, tuner $((WWV_KHZ - OFFSET_KHZ)) kHz, carrier correction $C Hz, shift $SHIFT Hz${LOG:+, log $(seg_name "$LOG" "$SEG")}"

  # Background processes joined by FIFOs, so a signal to this script alone (Ctrl-C,
  # kill, systemd) reaches the traps at once: `wait` returns on a signal, a foreground
  # pipeline would not. wwv_live's stdout passes through awk, which marks LOCKED.
  /bin/rm -f "$FIFODIR/iq" "$FIFODIR/out" "$FIFODIR/locked"
  mkfifo "$FIFODIR/iq" "$FIFODIR/out"
  awk -v flag="$FIFODIR/locked" '{ print; fflush() } /^SYNC .* -> LOCKED / { printf "" > flag; close(flag) }' \
    < "$FIFODIR/out" &
  RPID=$!
  python3 -I "$HERE/rsp2_stream.py" "${ST_ARGS[@]}" > "$FIFODIR/iq" &
  SPID=$!
  seg_extra "$SEG"
  "$WWV_LIVE" "${WL_ARGS[@]}" "${SEG_EXTRA[@]}" < "$FIFODIR/iq" > "$FIFODIR/out" &
  WPID=$!

  T_SEG=$SECONDS; LOCKED=; SWITCH=
  while kill -0 "$WPID" 2>/dev/null; do
    if [ -z "$LOCKED" ] && [ -e "$FIFODIR/locked" ]; then
      LOCKED=1
      say "segment $SEG: $WWV_KHZ kHz LOCKED after $((SECONDS - T_SEG)) s"
    fi
    if [ -n "$AUTO" ] && [ -z "$LOCKED" ] && [ "$LOCK_MINS" != 0 ] &&
       python3 -I -c "import sys; sys.exit(0 if $((SECONDS - T_SEG)) >= $LOCK_MINS * 60 else 1)"; then
      SWITCH=1; break
    fi
    sleep 1 & wait $! || true
  done
  [ -z "$LOCKED" ] && [ -e "$FIFODIR/locked" ] && LOCKED=1

  if [ -z "$SWITCH" ]; then
    wait "$WPID" || rc=$?
    WPID=
    wait "$SPID" || { src=$?; [ "$rc" = 0 ] && rc=$src; }
    SPID=
    wait "$RPID" || true
    RPID=
    if [ -n "$LOCKED" ]; then st=LOCKED; else st="never LOCKED"; fi
    HISTORY+=("segment $SEG: $WWV_KHZ kHz, $((SECONDS - T_SEG)) s, $st")
    break
  fi

  HISTORY+=("segment $SEG: $WWV_KHZ kHz, $((SECONDS - T_SEG)) s, never LOCKED, switched")
  say "SWITCH: $WWV_KHZ kHz has not reached LOCKED in $LOCK_MINS min; stopping it to re-scan"
  stop_live
  OLD=$WWV_KHZ
  SEG=$((SEG + 1))
  scan --avoid "$OLD"
  if [ -z "$BAND" ]; then
    say "SWITCH: no band from the re-scan; staying on $OLD kHz"
  elif [ "$BAND" = "$OLD" ]; then
    say "SWITCH: the re-scan keeps $OLD kHz (no other band in the running)"
  else
    WWV_KHZ=$BAND
    say "SWITCH: $OLD kHz -> $WWV_KHZ kHz (segment $SEG)"
  fi
done
for h in "${HISTORY[@]}"; do say "$h"; done
exit "$rc"
