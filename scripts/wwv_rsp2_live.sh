#!/usr/bin/env bash
# Live WWV decode from saga's RSP2: SoapySDR -> wwv_live -> UDP telemetry on port 3005.
#
#   scripts/wwv_rsp2_live.sh [options] [-- extra wwv_live options]
#
#   --secs N            stop after N seconds of live run, by the wall clock, however
#                       many samples came (default 0: until Ctrl-C); a band switch's
#                       re-scan and a reopen count toward N
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
#   --scan-timeout N    auto: stop a band scan that runs past N s of wall clock (default
#                       bands x (--scan-secs + 15) + bands x (35 + 15), the second term
#                       for close-call extensions of 35 s; 240 + 300 s at the defaults)
#   --stall-secs N      reopen the stream when no sample has come for N s (default 10;
#                       0: never)
#   --reopen-max N      end the run after N reopens (default 20)
#   --mem-order N       before starting sdrplay.service and before each reopen, wait for
#                       a free Unmovable or Reclaimable block at order N or above in the
#                       Normal zone (default 9: the pageblock order, 2 MiB; 0-10)
#   --mem-wait-secs N   bound on that wait; past it the wrapper warns and goes ahead
#                       (default 60; 0: no wait)
#   --mem-recompact-secs N  compact memory again every N s while waiting (default 5)
#   --backoff-secs N    wait N s before a reopen caused by a service crash or libusb
#                       errno=12, doubling for each such reopen in a row (default 5; 0: none)
#   --backoff-max N     cap on that wait (default 120)
#   --backoff-reset-secs N  a segment that runs N s starts the doubling over (default 600)
#
# Memory: before every start of sdrplay.service (the first, and the scan's restart) and
# before each reopen, memory is compacted and the wrapper waits as --mem-order says. It
# counts the Normal zone's Unmovable and Reclaimable rows of /proc/pagetypeinfo (sudo -n
# cat), or, when that fails, every type in /proc/buddyinfo ("pagetypeinfo unreadable, using
# buddyinfo"). It logs the buddyinfo line it started on (MEMWAIT). WWV_BUDDYINFO and
# WWV_PAGETYPEINFO name other files to read (for the tests). The wrapper reads sdrplay.service's journal every 3 s and at the
# end of each segment and scan, and logs each burst of libusb errno=12 lines there as one
# ERRNO12 line with a count.
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
# Gaps: rsp2_stream fills samples lost upstream with zeros, so wwv_live's sample
# clock stays on true time, and wwv_live holds the events in a filled span back from
# sync (GAP lines on the console and in the log). rsp2_stream holds its output 3 s to
# place each fill; it passes its anchor, measured before the hold, to wwv_live through
# a file (--anchor-out / --anchor-in).
#
# Scan failure: a band scan that runs past --scan-timeout is stopped (its Python too, killed
# when it is stuck in the driver), and a scan that exits 5 reports the sdrplay API failing
# (wwv_band_scan.py). Either way sdrplay.service is stopped and started again (memory
# compacted and waited for first) and the scan runs once more. When that fails the same way, the run
# ends with exit status 3, "not going live", at the first scan or at a re-scan alike.
#
# Reopen: the stream is stopped and opened again on the same band, as a new segment
# (wwv_live's sample-counter clock cannot carry across a gap it cannot fill), when
#   - rsp2_stream ends because no sample came for --stall-secs (exit status 75);
#   - rsp2_stream ends because it lost more than it fills (over 10 s; exit status 76);
#   - sdrplay.service's MainPID or NRestarts changes (the vendor service crashed and
#     systemd restarted it under the stream);
#   - rsp2_stream's heartbeat file is older than 2 x --stall-secs + 15 s (a hang).
# Each reopen is logged with its reason (REOPEN on stderr, and in the history at the
# end). After --reopen-max reopens the run ends with exit status 4. The lock timer of
# --lock-mins starts again in a reopened segment. A reopen after a service restart, or
# after a segment in which the journal showed errno=12, waits first (BACKOFF): --backoff-secs,
# doubled for each such reopen in a row, at most --backoff-max. The backoff and the memory
# wait count toward --secs.
#
# sdrplay.service is started here, once (scans and live segments all run under the
# same start), and stopped on every exit path (end, Ctrl-C, error): it stays disabled
# except while recording. The last line printed is the service state after the stop.
# The vendor service can crash on stop; the script then reports it and clears the
# unit's failed state. A guard process in its own session covers the exit no trap
# sees (SIGKILL, a session that dies with its shell): when this script is gone and
# its cleanup did not run, the guard stops the processes it started (by the PIDs
# it recorded, never by process group, which the caller may share) and then the
# unit, and appends what it did to this script's stderr.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(dirname "$HERE")
BUILD=$REPO/build
SECS=0; RECORD=; AGC=; LOG=; TIMEBASE=; CARRIER=; PPM=1.09; LNA=; IFGR=
WWV_KHZ=5000; OFFSET_KHZ=100; RATE=2000000; SCALE=32; SCAN_SECS=25; SCAN_JSON=; LOCK_MINS=8
SCAN_TIMEOUT=
STALL_SECS=10; REOPEN_MAX=20
MEM_ORDER=9; MEM_WAIT_SECS=60; MEM_RECOMPACT_SECS=5
BACKOFF_SECS=5; BACKOFF_MAX=120; BACKOFF_RESET_SECS=600
# The streamer; a test may point this at a stub that takes the same options.
RSP2_STREAM=${WWV_RSP2_STREAM:-$HERE/rsp2_stream.py}
# The free-block counts; a test may point these at files it writes.
BUDDYINFO=${WWV_BUDDYINFO:-/proc/buddyinfo}
PAGETYPEINFO=${WWV_PAGETYPEINFO:-/proc/pagetypeinfo}
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
    --scan-timeout) SCAN_TIMEOUT=$2; shift 2 ;;
    --lock-mins) LOCK_MINS=$2; shift 2 ;;
    --stall-secs) STALL_SECS=$2; shift 2 ;;
    --reopen-max) REOPEN_MAX=$2; shift 2 ;;
    --mem-order) MEM_ORDER=$2; shift 2 ;;
    --mem-wait-secs) MEM_WAIT_SECS=$2; shift 2 ;;
    --mem-recompact-secs) MEM_RECOMPACT_SECS=$2; shift 2 ;;
    --backoff-secs) BACKOFF_SECS=$2; shift 2 ;;
    --backoff-max) BACKOFF_MAX=$2; shift 2 ;;
    --backoff-reset-secs) BACKOFF_RESET_SECS=$2; shift 2 ;;
    --) shift; break ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
EXTRA=("$@")
case $WWV_KHZ in
  auto|2500|5000|10000|15000|20000|25000) ;;
  *) echo "--wwv-khz: want 2500, 5000, 10000, 15000, 20000, 25000 or auto, not $WWV_KHZ" >&2; exit 2 ;;
esac
[[ $LOCK_MINS =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo "--lock-mins: want minutes, not $LOCK_MINS" >&2; exit 2; }
[[ $STALL_SECS =~ ^[0-9]+$ ]] || { echo "--stall-secs: want whole seconds, not $STALL_SECS" >&2; exit 2; }
[[ $REOPEN_MAX =~ ^[0-9]+$ ]] || { echo "--reopen-max: want a count, not $REOPEN_MAX" >&2; exit 2; }
[[ $SCAN_SECS =~ ^[0-9]+$ ]] || { echo "--scan-secs: want whole seconds, not $SCAN_SECS" >&2; exit 2; }
[[ $MEM_ORDER =~ ^([0-9]|10)$ ]] || { echo "--mem-order: want an order 0-10, not $MEM_ORDER" >&2; exit 2; }
for _o in MEM_WAIT_SECS:--mem-wait-secs BACKOFF_SECS:--backoff-secs BACKOFF_MAX:--backoff-max \
          BACKOFF_RESET_SECS:--backoff-reset-secs; do
  _v=${_o%%:*}
  [[ ${!_v} =~ ^[0-9]+$ ]] || { echo "${_o#*:}: want whole seconds, not ${!_v}" >&2; exit 2; }
done
[[ $MEM_RECOMPACT_SECS =~ ^[0-9]+$ ]] && [ "$MEM_RECOMPACT_SECS" -gt 0 ] ||
  { echo "--mem-recompact-secs: want whole seconds over 0, not $MEM_RECOMPACT_SECS" >&2; exit 2; }
if [ -z "$SCAN_TIMEOUT" ]; then
  # every band read, plus a close-call extension (wwv_band_scan.py's 35 s) of every band
  NBANDS=$(python3 -I -c "import sys; sys.path.insert(0, sys.argv[1]); import wwv_bands; print(len(wwv_bands.WWV_KHZ))" "$HERE")
  SCAN_TIMEOUT=$((NBANDS * (SCAN_SECS + 15) + NBANDS * (35 + 15)))
fi
[[ $SCAN_TIMEOUT =~ ^[0-9]+$ ]] && [ "$SCAN_TIMEOUT" -gt 0 ] ||
  { echo "--scan-timeout: want whole seconds over 0, not $SCAN_TIMEOUT" >&2; exit 2; }
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

SCPID=; SPID=; WPID=; RPID=; FIFODIR=; GUARD_PID=
# note_pids: record the running children for the guard.
note_pids() {
  [ -n "$FIFODIR" ] && printf '%s\n' $SCPID $SPID $WPID $RPID > "$FIFODIR/pids.tmp" &&
    mv -f "$FIFODIR/pids.tmp" "$FIFODIR/pids"
  return 0
}
# start_guard: a watcher in a new session, so a signal to this script's process
# group does not reach it. It does nothing while this script runs.
start_guard() {
  setsid bash -c '
    me=$1; dir=$2; tag=$3
    while kill -0 "$me" 2>/dev/null; do sleep 1; done
    say() { echo "[$tag guard] $(date -u +%Y-%m-%dT%H:%M:%SZ) $*" >&2; }
    say "wwv_rsp2_live.sh ($me) exited without its cleanup; stopping what it started"
    pids=()
    for p in $(cat "$dir/pids" 2>/dev/null); do
      # only a process of ours: the PID may have been reused
      grep -qaE "rsp2_stream|wwv_live|wwv_band_scan|awk" "/proc/$p/cmdline" 2>/dev/null && pids+=("$p")
    done
    [ ${#pids[@]} -gt 0 ] && kill -TERM "${pids[@]}" 2>/dev/null
    for _ in $(seq 100); do
      alive=0; for p in "${pids[@]}"; do kill -0 "$p" 2>/dev/null && alive=1; done
      [ $alive = 0 ] && break; sleep 0.1
    done
    for p in "${pids[@]}"; do kill -KILL "$p" 2>/dev/null; done
    sudo -n systemctl stop sdrplay.service || say "ERROR: systemctl stop sdrplay.service failed"
    if [ "$(systemctl is-active sdrplay.service)" = failed ]; then sudo -n systemctl reset-failed sdrplay.service; fi
    /bin/rm -rf "$dir"
    say "sdrplay.service: $(systemctl is-active sdrplay.service)"
  ' wwv_rsp2_live_guard "$$" "$FIFODIR" "$SDRPLAY_TAG" < /dev/null >&2 &
  GUARD_PID=$!
}
# gone PID TENTHS: true once PID has exited, polling every 0.1 s for TENTHS tenths.
gone() {
  local i
  for ((i = 0; i < $2; i++)); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done
  ! kill -0 "$1" 2>/dev/null
}
# stop_pid PID NAME TENTHS: TERM, then KILL if it has not exited in TENTHS tenths.
stop_pid() {
  kill -TERM "$1" 2>/dev/null || true
  if ! gone "$1" "$3"; then
    say "$2 did not stop in $(($3 / 10)) s after SIGTERM; killing it"
    kill -KILL "$1" 2>/dev/null || true
  fi
  wait "$1" 2>/dev/null || true
}
# stop_live: streamer first, so wwv_live reads end of input and finishes its report,
# then wwv_live, then the output filter. The API service must not be stopped under an
# open stream. Every wait is bounded: a streamer stuck on a dead API service is killed.
stop_live() {
  [ -n "$SPID" ] && stop_pid "$SPID" rsp2_stream 100
  SPID=
  if [ -n "$WPID" ]; then
    gone "$WPID" 50 || true
    stop_pid "$WPID" wwv_live 50
  fi
  WPID=
  if [ -n "$RPID" ]; then
    gone "$RPID" 20 || true
    stop_pid "$RPID" "the output filter" 20
  fi
  RPID=
}
# past_end: true once --secs of wall clock have run out.
past_end() { [ -n "$END" ] && [ "$EPOCHSECONDS" -ge "$END" ]; }
# svc_ident: sdrplay.service's MainPID and restart count; a change means the vendor
# service died (and systemd started it again) under the stream.
svc_ident() { systemctl show -p MainPID -p NRestarts --value sdrplay.service 2>/dev/null | paste -sd' ' || true; }
# svc_ready: wait up to 30 s for the unit to be active again; start it if it is not.
svc_ready() {
  local i
  for ((i = 0; i < 30; i++)); do [ "$(sdrplay_state)" = active ] && return 0; sleep 1; done
  say "sdrplay.service is $(sdrplay_state) 30 s after the reopen was asked for; starting it"
  svc_start
}
# mem_read: MEM_FREE, the free blocks at order MEM_ORDER and above that the wait counts,
# and MEM_LINE, the rows they were counted from (one per node, joined by " | "). From
# /proc/pagetypeinfo (root only, read with sudo -n): the Normal zone's Unmovable and
# Reclaimable rows together, MEM_SRC "Unmovable + Reclaimable". When that cannot be read,
# from /proc/buddyinfo's Normal zone, every migrate type, MEM_SRC "buddyinfo". MEM_BUDDY is
# buddyinfo's Normal zone either way. MEM_FREE is empty when there is no Normal zone.
mem_read() {
  local bi pt
  bi=$(cat "$BUDDYINFO" 2>/dev/null || true)
  MEM_BUDDY=$(awk '$4 == "Normal" { $1 = $1; printf "%s%s", (n++ ? " | " : ""), $0 }' <<< "$bi")
  pt=$(sudo -n cat "$PAGETYPEINFO" 2>/dev/null || true)
  MEM_LINE=$(awk '$4 == "Normal," && ($6 == "Unmovable" || $6 == "Reclaimable") { $1 = $1; printf "%s%s", (n++ ? " | " : ""), $0 }' <<< "$pt")
  if [ -n "$MEM_LINE" ]; then
    MEM_SRC="Unmovable + Reclaimable"
    MEM_FREE=$(awk -v o="$MEM_ORDER" '$4 == "Normal," && ($6 == "Unmovable" || $6 == "Reclaimable") { for (k = 7 + o; k <= NF; k++) s += $k }
                                      END { print s + 0 }' <<< "$pt")
  else
    MEM_SRC=buddyinfo
    MEM_LINE=$MEM_BUDDY
    MEM_FREE=$(awk -v o="$MEM_ORDER" '$4 == "Normal" { z = 1; for (k = 5 + o; k <= NF; k++) s += $k }
                                      END { if (z) print s + 0 }' <<< "$bi")
  fi
}
# mem_wait WHAT: compact memory, then wait up to --mem-wait-secs for a free block at
# --mem-order or above (mem_read says where it counts), compacting again every
# --mem-recompact-secs. Past the bound, or at --secs, it warns and goes ahead: it never
# blocks the run.
mem_wait() {
  sdrplay_compact
  [ "$MEM_WAIT_SECS" = 0 ] && return 0
  mem_read
  if [ -z "$MEM_FREE" ]; then say "MEMWAIT: no Normal zone in $BUDDYINFO or $PAGETYPEINFO; going ahead"; return 0; fi
  say "MEMWAIT: before $1, Normal zone: $MEM_BUDDY"
  if [ "$MEM_SRC" = buddyinfo ]; then
    say "MEMWAIT: pagetypeinfo unreadable, using buddyinfo (every migrate type)"
  else
    say "MEMWAIT: pagetypeinfo: $MEM_LINE"
  fi
  if [ "$MEM_FREE" -gt 0 ]; then
    say "MEMWAIT: $MEM_FREE free blocks at order >= $MEM_ORDER ($MEM_SRC); going ahead"; return 0
  fi
  say "MEMWAIT: no free block at order >= $MEM_ORDER ($MEM_SRC); waiting up to $MEM_WAIT_SECS s, compacting every $MEM_RECOMPACT_SECS s"
  local t0=$SECONDS tc=$SECONDS n=0
  while :; do
    sleep 1 & wait $! || true
    if past_end; then say "MEMWAIT: --secs reached while waiting; going ahead"; return 0; fi
    if [ $((SECONDS - tc)) -ge "$MEM_RECOMPACT_SECS" ] && [ "${SDRPLAY_COMPACT:-1}" != 0 ]; then
      sdrplay_compact 2> /dev/null; n=$((n + 1)); tc=$SECONDS
    fi
    mem_read
    if [ "${MEM_FREE:-0}" -gt 0 ]; then
      say "MEMWAIT: $MEM_FREE free blocks at order >= $MEM_ORDER ($MEM_SRC) after $((SECONDS - t0)) s ($n re-compactions); going ahead: $MEM_LINE"
      return 0
    fi
    if [ $((SECONDS - t0)) -ge "$MEM_WAIT_SECS" ]; then
      say "MEMWAIT: WARNING: no free block at order >= $MEM_ORDER ($MEM_SRC) after $MEM_WAIT_SECS s ($n re-compactions); going ahead: $MEM_LINE"
      return 0
    fi
  done
}
# svc_start: compact and wait for free memory, then start sdrplay.service (compacted here,
# so not again).
svc_start() {
  mem_wait "starting sdrplay.service"
  SDRPLAY_COMPACT=0 sdrplay_start
}
# errno_poll [flush]: read sdrplay.service's journal from where the last read ended and
# count its libusb errno=12 lines (the vendor service logs them before it crashes). A
# burst is the lines of consecutive reads that found some; it is logged as one ERRNO12
# line when a read finds none, or at once with flush. ERR_SEG counts the segment's lines.
ERR_N=0; ERR_FIRST=; ERR_LAST=; ERR_SEG=0; T_JPOLL=0; JCUR=; J_T0=$EPOCHSECONDS
errno_poll() {
  local out n=0
  T_JPOLL=$SECONDS
  [ -n "$JCUR" ] || return 0
  if [ -s "$JCUR" ]; then
    out=$(journalctl -u sdrplay.service --cursor-file "$JCUR" -o short-iso --utc --no-pager -q 2> /dev/null || true)
  else
    out=$(journalctl -u sdrplay.service --since "@$J_T0" --cursor-file "$JCUR" -o short-iso --utc --no-pager -q 2> /dev/null || true)
  fi
  out=$(grep -F 'errno=12' <<< "$out" || true)
  [ -n "$out" ] && n=$(wc -l <<< "$out")
  if [ "$n" -gt 0 ]; then
    [ -n "$ERR_FIRST" ] || ERR_FIRST=$(head -1 <<< "$out" | cut -c12-19)
    ERR_LAST=$(tail -1 <<< "$out" | cut -c12-19)
    ERR_N=$((ERR_N + n)); ERR_SEG=$((ERR_SEG + n))
  fi
  if [ "$ERR_N" -gt 0 ] && { [ "$n" = 0 ] || [ -n "${1:-}" ]; }; then
    say "ERRNO12: $ERR_N libusb errno=12 lines from sdrplay.service, $ERR_FIRST-$ERR_LAST UTC"
    ERR_N=0; ERR_FIRST=; ERR_LAST=
  fi
  return 0
}
# stop_scan: stop the band scan and its Python. TERM reaches the scan's wrapper, which
# stops the Python; a Python stuck in the driver hears no signal, so whatever of the
# wrapper's children is left after the wrapper is gone is killed.
stop_scan() {
  local kids k
  kids=$(pgrep -P "$SCPID" 2>/dev/null || true)
  stop_pid "$SCPID" wwv_band_scan.sh 100
  for k in $kids; do
    if kill -0 "$k" 2>/dev/null; then
      say "the band scan's process $k outlived its wrapper; killing it"
      kill -KILL "$k" 2>/dev/null || true
    fi
  done
  SCPID=
}
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  [ -n "$SCPID" ] && stop_scan
  stop_live
  [ -n "$FIFODIR" ] && /bin/rm -rf "$FIFODIR"
  sdrplay_stop
  # the guard last: until here, a kill of this cleanup still leaves it to act
  [ -n "$GUARD_PID" ] && { kill "$GUARD_PID" 2>/dev/null || true; }
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

FIFODIR=$(mktemp -d)
start_guard
# past_end is called from the memory wait before the first start, so END is set
# (empty: no end yet) before it; the wall clock of --secs starts with the scan, below.
END=
# the journal cursor: the unit's last entry so far, so errno_poll reads only what follows
JCUR=$FIFODIR/journal.cursor
journalctl -u sdrplay.service -n 1 --cursor-file "$JCUR" -o cat --no-pager -q > /dev/null 2>&1 || true
svc_start

SEG=1
HISTORY=()
# scan_once [--avoid KHZ]: run the band scan under this script's service start, in the
# background so a signal reaches the traps at once. Sets BAND to its choice, or empty,
# and SCAN_FAIL to the reason when it ran past --scan-timeout or reported the API failing.
scan_once() {
  local sj
  sj=$(seg_name "${SCAN_JSON:-$FIFODIR/scan.json}" "$SEG")
  /bin/rm -f "$sj"
  say "scanning the WWV bands, $SCAN_SECS s each${1:+, avoiding $2 kHz}; figures in $sj (bound $SCAN_TIMEOUT s)"
  "$HERE/wwv_band_scan.sh" --no-service --secs "$SCAN_SECS" --json "$sj" "${GAINS[@]}" --ppm "$PPM" "$@" >&2 &
  SCPID=$!
  note_pids
  local src=0 t0=$SECONDS
  SCAN_FAIL=
  while kill -0 "$SCPID" 2>/dev/null; do
    if past_end; then
      say "--secs reached during the re-scan; stopping it"
      stop_scan
      break
    fi
    if [ $((SECONDS - t0)) -ge "$SCAN_TIMEOUT" ]; then
      SCAN_FAIL="the band scan ran past its $SCAN_TIMEOUT s bound"
      say "SCAN: $SCAN_FAIL; stopping it"
      stop_scan
      break
    fi
    sleep 1 & wait $! || true
  done
  if [ -n "$SCPID" ]; then
    wait "$SCPID" 2>/dev/null || src=$?
    SCPID=
  fi
  errno_poll flush
  [ "$src" = 5 ] && SCAN_FAIL="the band scan reports the sdrplay API failing (exit 5)"
  BAND=
  [ -z "$SCAN_FAIL" ] &&
    BAND=$(python3 -I -c "import json,sys; b=json.load(open(sys.argv[1])).get('best_khz'); print(b if b else '')" "$sj" 2>/dev/null || true)
  [ -n "$BAND" ] || [ -n "$SCAN_FAIL" ] || say "the band scan found no band (exit $src)"
  return 0
}
# scan [--avoid KHZ]: scan_once; when it fails (SCAN_FAIL), restart sdrplay.service,
# memory compacted and waited for first, and scan once more. SCAN_FAIL stays set when that
# fails too.
scan() {
  scan_once "$@"
  if [ -n "$SCAN_FAIL" ] && ! past_end; then
    say "SCAN: restarting sdrplay.service and scanning once more"
    sdrplay_stop
    svc_start
    scan_once "$@"
    [ -n "$SCAN_FAIL" ] && say "SCAN: failed again after the restart: $SCAN_FAIL"
  fi
  return 0
}

# END is set (empty, above) before the first scan: scan() calls past_end, and under set -u
# an unset END aborted every --wwv-khz auto run. The wall clock starts with the scan.
[ "$SECS" != 0 ] && END=$(python3 -I -c "import math, time; print(math.ceil(time.time() + $SECS))")

if [ -n "$AUTO" ]; then
  scan
  [ -n "$BAND" ] || { say "not going live"; exit 3; }
  WWV_KHZ=$BAND
  say "going live on the scan's band: $WWV_KHZ kHz"
fi

rc=0
REOPENS=0; BACKOFFS=0
while :; do
  REMAIN=0
  if [ -n "$END" ]; then
    REMAIN=$((END - EPOCHSECONDS))
    if [ "$REMAIN" -le 0 ]; then say "--secs reached before segment $SEG could start"; break; fi
  fi
  [ -n "$CARRIER" ] && [ "$SEG" = 1 ] && C=$CARRIER || C=$(python3 -I -c "print(f'{$PPM * $WWV_KHZ / 1000:.3f}')")
  SHIFT=$(python3 -I -c "print(f'{$OFFSET_KHZ * 1000 + $C:.3f}')")
  WL_ARGS=(-f s16 -r "$RATE" -s "$SHIFT" -g "$SCALE")
  [ -n "$AGC" ] && WL_ARGS+=(-a)
  [ -n "$LOG" ] && WL_ARGS+=(-l "$(seg_name "$LOG" "$SEG")")
  [ -n "$TIMEBASE" ] && WL_ARGS+=(-j "$(seg_name "$TIMEBASE" "$SEG")")
  HB=$FIFODIR/heartbeat
  touch "$HB"
  ANCHOR=$FIFODIR/anchor.json
  /bin/rm -f "$ANCHOR"
  WL_ARGS+=(--anchor-in "$ANCHOR")
  ST_ARGS=(--wwv-khz "$WWV_KHZ" --offset-khz "$OFFSET_KHZ" --rate "$RATE" --secs "$REMAIN" "${GAINS[@]}"
           --stall-secs "$STALL_SECS" --heartbeat "$HB" --anchor-out "$ANCHOR")
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
  SVC0=$(svc_ident)
  python3 -I "$RSP2_STREAM" "${ST_ARGS[@]}" > "$FIFODIR/iq" &
  SPID=$!
  seg_extra "$SEG"
  "$WWV_LIVE" "${WL_ARGS[@]}" "${SEG_EXTRA[@]}" < "$FIFODIR/iq" > "$FIFODIR/out" &
  WPID=$!
  note_pids

  T_SEG=$SECONDS; LOCKED=; SWITCH=; REOPEN=; ENDED=; ERR_SEG=0
  while kill -0 "$WPID" 2>/dev/null; do
    [ $((SECONDS - T_JPOLL)) -ge 3 ] && errno_poll
    if [ -z "$LOCKED" ] && [ -e "$FIFODIR/locked" ]; then
      LOCKED=1
      say "segment $SEG: $WWV_KHZ kHz LOCKED after $((SECONDS - T_SEG)) s"
    fi
    if past_end; then
      ENDED="--secs reached (wall clock)"; break
    fi
    s=$(svc_ident)
    if [ "$s" != "$SVC0" ]; then
      REOPEN="sdrplay.service restarted under the stream (MainPID NRestarts: $SVC0 -> $s)"; break
    fi
    if [ "$STALL_SECS" != 0 ]; then
      age=$((EPOCHSECONDS - $(stat -c %Y "$HB" 2>/dev/null || echo "$EPOCHSECONDS")))
      if [ "$age" -gt $((2 * STALL_SECS + 15)) ]; then
        REOPEN="rsp2_stream heartbeat is $age s old: the stream is hung"; break
      fi
    fi
    if [ -n "$AUTO" ] && [ -z "$LOCKED" ] && [ "$LOCK_MINS" != 0 ] &&
       python3 -I -c "import sys; sys.exit(0 if $((SECONDS - T_SEG)) >= $LOCK_MINS * 60 else 1)"; then
      SWITCH=1; break
    fi
    sleep 1 & wait $! || true
  done
  [ -z "$LOCKED" ] && [ -e "$FIFODIR/locked" ] && LOCKED=1

  if [ -z "$SWITCH" ]; then
    if [ -n "$LOCKED" ]; then st=LOCKED; else st="never LOCKED"; fi
    if [ -n "$ENDED" ] || [ -n "$REOPEN" ]; then
      stop_live
    else
      wait "$WPID" || rc=$?
      WPID=
      src=0; wait "$SPID" || src=$?
      SPID=
      if [ "$src" = 75 ]; then
        REOPEN="rsp2_stream had no samples for $STALL_SECS s"
      elif [ "$src" = 76 ]; then
        REOPEN="rsp2_stream lost more samples than it fills (its GAP line says how much)"
      elif [ "$src" != 0 ] && [ "$rc" = 0 ]; then
        rc=$src
      fi
      wait "$RPID" || true
      RPID=
    fi
    errno_poll flush
    if [ -z "$REOPEN" ]; then
      HISTORY+=("segment $SEG: $WWV_KHZ kHz, $((SECONDS - T_SEG)) s, $st${ENDED:+, $ENDED}")
      [ -n "$ENDED" ] && say "segment $SEG: $ENDED; stopped"
      break
    fi
    HISTORY+=("segment $SEG: $WWV_KHZ kHz, $((SECONDS - T_SEG)) s, $st, reopened: $REOPEN")
    say "REOPEN: segment $SEG: $REOPEN"
    REOPENS=$((REOPENS + 1))
    if [ "$REOPENS" -gt "$REOPEN_MAX" ]; then
      say "REOPEN: $REOPENS reopens, more than --reopen-max $REOPEN_MAX; ending the run"
      rc=4; break
    fi
    # Back off before a reopen the vendor service caused (it crashed and systemd restarted
    # it, or it logged errno=12): those came in clusters, and reopening at once fed them.
    CRASHY=
    [[ $REOPEN == "sdrplay.service restarted"* ]] && CRASHY="the service restarted"
    [ "$ERR_SEG" -gt 0 ] && CRASHY="${CRASHY:+$CRASHY, }$ERR_SEG errno=12 lines in the segment"
    if [ -n "$CRASHY" ] && [ "$BACKOFF_SECS" != 0 ]; then
      if [ $((SECONDS - T_SEG)) -ge "$BACKOFF_RESET_SECS" ]; then BACKOFFS=0; fi
      w=$BACKOFF_SECS
      for ((i = 0; i < BACKOFFS && w < BACKOFF_MAX; i++)); do w=$((w * 2)); done
      [ "$w" -gt "$BACKOFF_MAX" ] && w=$BACKOFF_MAX
      BACKOFFS=$((BACKOFFS + 1))
      say "BACKOFF: waiting $w s before the reopen ($CRASHY; crash reopen $BACKOFFS in a row)"
      t0=$SECONDS
      while [ $((SECONDS - t0)) -lt "$w" ] && ! past_end; do sleep 1 & wait $! || true; done
    fi
    if past_end; then say "--secs reached; not reopening"; break; fi
    svc_ready
    mem_wait "reopen $REOPENS"
    SEG=$((SEG + 1))
    say "REOPEN: opening the stream again on $WWV_KHZ kHz as segment $SEG (reopen $REOPENS of at most $REOPEN_MAX)"
    continue
  fi

  HISTORY+=("segment $SEG: $WWV_KHZ kHz, $((SECONDS - T_SEG)) s, never LOCKED, switched")
  say "SWITCH: $WWV_KHZ kHz has not reached LOCKED in $LOCK_MINS min; stopping it to re-scan"
  stop_live
  OLD=$WWV_KHZ
  SEG=$((SEG + 1))
  scan --avoid "$OLD"
  if [ -n "$SCAN_FAIL" ]; then
    HISTORY+=("re-scan for segment $SEG: $SCAN_FAIL")
    say "not going live"
    rc=3; break
  elif [ -z "$BAND" ]; then
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
