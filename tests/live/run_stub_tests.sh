#!/usr/bin/env bash
# wwv_rsp2_live.sh and rsp2_stream.py against stubs: no radio, no sdrplay.service.
#
#   tests/live/run_stub_tests.sh BUILD_DIR WORK_DIR [CASE ...]
#
# BUILD_DIR holds wwv_live. WORK_DIR gets a copy of the wrapper with a stub band scan
# beside it, and one directory per case (console.txt, the fake unit's calls). The stubs
# in tests/live/fakebin stand in for systemctl, sudo and journalctl: the fake sdrplay.service
# and its journal live in the case directory, and every check fails before a case runs if
# the PATH would reach the real ones. The stub sudo logs the memory compaction instead of
# running it. Each case's buddyinfo is a file in its directory (WWV_BUDDYINFO), with free
# blocks at order 9 and 10 unless the case writes another. rsp2_stream.py runs against a
# fake SoapySDR module.
#
# Cases (all by default):
#   fixed_end     fixed band, ends at --secs by the wall clock
#   stall75       the stream exits 75 once: REOPEN, segment 2, end at --secs
#   svc_restart   the unit's MainPID changes under the stream: REOPEN
#   hang          the stream stops writing and its heartbeat: REOPEN on the heartbeat
#   reopen_max    every stream exits 75: the run ends with status 4 after --reopen-max
#   deaf          a stream deaf to SIGTERM and --secs: killed 10 s after the TERM
#   kill9         the wrapper is SIGKILLed: the guard stops the stream and the unit
#   auto_end      auto: scan, go live on its band, end at --secs
#   auto_switch   auto: no LOCKED within --lock-mins, re-scan with --avoid, switch, end
#   auto_rescan_end  auto: --secs runs out during the re-scan, which is stopped
#   auto_none     auto: the scan finds no band: exit 3, not live
#   scan_hang     auto: the scan is stuck past --scan-timeout and deaf to SIGTERM: killed,
#                 the unit restarted, the retry is stuck too: exit 3, not live, within the bound
#   scan_apifail  auto: every scan exits 5 (API failing): restart, retry once, exit 3, not live
#   scan_apifail_retry  auto: the first scan exits 5, the retry after the restart finds a band
#   scan_py_api   wwv_band_scan.py on a failing API (fake module): activateStream fails and a
#                 read would block for an hour; the same with a 0 return; ServiceNotResponding;
#                 reads stuck in the driver; reads that only time out. Each exits 5 within its
#                 bound and writes best_khz null
#   stream_stall  rsp2_stream.py: no samples for --stall-secs: exit 75
#   stream_lag    rsp2_stream.py: samples at half rate: "behind wall clock" names upstream loss
#   scan_keep_raw wwv_band_scan.py --keep-raw: a --replay of the kept windows gives the same figures
#   mem_now       buddyinfo has free order 9-10 blocks: the memory wait goes ahead at once,
#                 after one compaction
#   mem_delay     no free block at order >= 9 until 4 s in: the wait re-compacts every 1 s,
#                 goes ahead when the blocks appear, and only then starts the unit
#   mem_bound     no free block ever: the wait warns after its 3 s bound and goes live
#   backoff_seq   the vendor service crashes (18 errno=12 lines, a restart) in every segment:
#                 one ERRNO12 line per burst, backoffs of 1, 2, 4, 4 s (capped), exit 4
#   errno_burst   12 errno=12 lines and a stall, no restart: one ERRNO12 line, a 1 s backoff,
#                 segment 2
#   gap76         the stream exits 76 once (a loss too large to fill): REOPEN, segment 2
#   stream_gaps   rsp2_stream.py: losses of 0.3 s and 1.05 s and a 5 s outage (timeouts), 30 ms
#                 arrival jitter: every sample's stream index stays within 40 ms of its true
#                 time, within 6 ms at the end; each fill logged (GAP) with its cause
#   stream_ramp   rsp2_stream.py: 3 ms/s lost for 30 s as a little before every block: the same
#   stream_toobig rsp2_stream.py, --stall-secs 20: a 12 s outage, over --gap-max-secs 10: exit 76,
#                 and no sample from after the loss is sent
#   gap_chain     wwv_live on synthetic WWV with a 1.05 s loss after LOCKED: filled (zeros), the
#                 held phase stays (no break, no re-anchor) and every event overlapping the span is
#                 held back from sync (recounted from the log); dropped (unfilled), it breaks.
#                 The replay of the filled log honours its GAP line and holds back the same events.
#                 A 3 ms/s loss for 3 minutes, filled as rsp2_stream fills it: no break either.
# Every wrapper case also checks that the fake unit is inactive at the end.
# Exit 0 when every case passes, 1 otherwise.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BUILD=${1:?usage: run_stub_tests.sh BUILD_DIR WORK_DIR [CASE ...]}
WORK=${2:?usage: run_stub_tests.sh BUILD_DIR WORK_DIR [CASE ...]}
shift 2
BUILD=$(cd "$BUILD" && pwd)
[ -x "$BUILD/wwv_live" ] || { echo "no wwv_live in $BUILD" >&2; exit 1; }
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)

# The wrapper and its helpers, with the stub scan as wwv_band_scan.sh.
SCR=$WORK/scripts
/bin/rm -rf "$SCR"; mkdir -p "$SCR"
/bin/cp -f "$REPO/scripts/wwv_rsp2_live.sh" "$REPO/scripts/sdrplay_service.sh" "$REPO/scripts/wwv_bands.py" "$SCR/"
/bin/cp -f "$HERE/stub_band_scan.sh" "$SCR/wwv_band_scan.sh"
chmod +x "$SCR/wwv_rsp2_live.sh" "$SCR/wwv_band_scan.sh"
mkdir -p "$WORK/tmp"

STUB_PATH=$HERE/fakebin:$PATH
# Refuse to run unless systemctl and sudo resolve to the stubs: a real stop would end
# whatever is using the radio.
for c in systemctl sudo journalctl; do
  got=$(PATH=$STUB_PATH command -v "$c")
  [ "$got" = "$HERE/fakebin/$c" ] || { echo "FATAL: $c resolves to $got, not the stub" >&2; exit 1; }
done

FAIL=0; PASS=0; D=; RC=0
fail() { echo "  FAIL: $*"; CASE_OK=; }
has() { grep -qF -- "$1" "$D/console.txt" || fail "no line with: $1"; }
hasnt() { ! grep -qF -- "$1" "$D/console.txt" || fail "unexpected line with: $1"; }
rc_is() { [ "$RC" = "$1" ] || fail "exit $RC, want $1"; }
streams_is() { local n; n=$(cat "$D/streams" 2>/dev/null | wc -c); [ "$n" = "$1" ] || fail "$n stream starts, want $1"; }
# ended_in K BAND: the run's history ends with segment K on BAND kHz, not reopened or
# switched. (The streamer gets --secs as the time left and usually ends a moment before
# the wrapper's own check, so a clean end need not say "--secs reached".)
ended_in() {
  grep -qE "\] [0-9TZ:-]+ segment $1: $2 kHz, [0-9]+ s, (never LOCKED|LOCKED)(, --secs reached \(wall clock\))?\$" "$D/console.txt" ||
    fail "no closing history line for segment $1 on $2 kHz"
  grep -qE "segment $(($1 + 1)): WWV" "$D/console.txt" && fail "a segment $(($1 + 1)) started"
  return 0
}
unit_off() { [ "$(cat "$D/state" 2>/dev/null)" = inactive ] || fail "fake sdrplay.service is $(cat "$D/state" 2>/dev/null)"; }
# buddy FREE9 FREE10: write the case's buddyinfo, saga's three zones (2026-11-21), with
# FREE9 and FREE10 free blocks at order 9 and 10 in the Normal zone.
buddy() {
  printf '%s\n' 'Node 0, zone      DMA      0      0      0      0      0      0      0      0      1      2      2 ' \
    'Node 0, zone    DMA32   2792   1643   1082   1072    904    734    525    347    197    135    284 ' \
    "Node 0, zone   Normal   3989  16137  11122   5590   3206    995    222     54     53 $1 $2 " > "$D/buddyinfo.new"
  mv -f "$D/buddyinfo.new" "$D/buddyinfo"
}
begin() {
  D=$WORK/$1; /bin/rm -rf "$D"; mkdir -p "$D"; CASE_OK=1; T0=$SECONDS
  buddy 96 22
  echo '2026-11-21T00:00:00+00:00 saga systemd[1]: Started sdrplay.service' > "$D/journal"
  echo "== $1"
}
compactions() { cat "$D/compactions" 2>/dev/null | wc -l; }
end_case() {
  if [ -n "$CASE_OK" ]; then PASS=$((PASS + 1)); echo "  ok ($((SECONDS - T0)) s)"; else
    FAIL=$((FAIL + 1))
    echo "  --- $D/console.txt (last 25 lines)"; tail -25 "$D/console.txt" | sed 's/^/  | /'; fi
}
# live [ENV=V ...] -- wrapper args: run the wrapper copy under the stubs; sets RC.
live() {
  local envs=()
  while [ "$1" != -- ]; do envs+=("$1"); shift; done; shift
  env STUB_DIR="$D" PATH="$STUB_PATH" TMPDIR="$WORK/tmp" WWV_RSP2_STREAM="$HERE/stub_stream.py" \
      WWV_BUDDYINFO="$D/buddyinfo" SOAPY_SDR_ROOT=/nonexistent SOAPY_SDR_PLUGIN_PATH=/nonexistent "${envs[@]}" \
      "$SCR/wwv_rsp2_live.sh" --build "$BUILD" --log "$D/run.log" "$@" -- -p 0 > "$D/console.txt" 2>&1
  RC=$?
}
want() { [ $# -eq 0 ] || [[ " ${CASES[*]} " == *" $1 "* ]]; }
CASES=("$@")
[ ${#CASES[@]} -eq 0 ] && CASES=(fixed_end stall75 svc_restart hang reopen_max deaf kill9 auto_end auto_switch
                                 auto_rescan_end auto_none scan_hang scan_apifail scan_apifail_retry scan_py_api
                                 stream_stall stream_lag scan_keep_raw gap_chain gap76
                                 mem_now mem_delay mem_bound backoff_seq errno_burst
                                 stream_gaps stream_ramp stream_toobig)

for c in "${CASES[@]}"; do
  case $c in
  fixed_end)
    begin "$c"
    live STUB_MODE=ok -- --wwv-khz 10000 --secs 8
    rc_is 0; ended_in 1 10000; hasnt REOPEN; streams_is 1; unit_off
    end_case ;;
  stall75)
    begin "$c"
    live STUB_MODE=stall75_after:2 STUB_ONLY_FIRST=1 -- --wwv-khz 10000 --secs 10
    rc_is 0; has 'REOPEN: segment 1: rsp2_stream had no samples for 10 s'
    has 'REOPEN: opening the stream again on 10000 kHz as segment 2 (reopen 1 of at most 20)'
    ended_in 2 10000; streams_is 2; hasnt BACKOFF; unit_off
    end_case ;;
  svc_restart)
    begin "$c"
    ( sleep 4; echo 4242 > "$D/pid.new"; mv -f "$D/pid.new" "$D/pid"; echo 1 > "$D/nr" ) &
    live STUB_MODE=ok -- --wwv-khz 5000 --secs 10 --backoff-secs 1
    wait
    rc_is 0; has 'REOPEN: segment 1: sdrplay.service restarted under the stream'
    has 'BACKOFF: waiting 1 s before the reopen (the service restarted; crash reopen 1 in a row)'
    has 'MEMWAIT: before reopen 1, Normal zone: '
    ended_in 2 5000; streams_is 2; unit_off
    end_case ;;
  hang)
    begin "$c"
    live STUB_MODE=hang_after:2 STUB_ONLY_FIRST=1 -- --wwv-khz 5000 --secs 26 --stall-secs 1
    rc_is 0; has 'REOPEN: segment 1: rsp2_stream heartbeat is'; has 'the stream is hung'
    ended_in 2 5000; streams_is 2; unit_off
    end_case ;;
  reopen_max)
    begin "$c"
    live STUB_MODE=stall75_after:1 -- --wwv-khz 5000 --secs 60 --reopen-max 2
    rc_is 4; has 'REOPEN: 3 reopens, more than --reopen-max 2; ending the run'; streams_is 3; unit_off
    end_case ;;
  deaf)
    begin "$c"
    live STUB_MODE=ok STUB_DEAF=1 -- --wwv-khz 5000 --secs 5
    rc_is 0; has 'segment 1: --secs reached (wall clock); stopped'
    has 'rsp2_stream did not stop in 10 s after SIGTERM; killing it'; unit_off
    end_case ;;
  kill9)
    begin "$c"
    env STUB_DIR="$D" PATH="$STUB_PATH" TMPDIR="$WORK/tmp" WWV_RSP2_STREAM="$HERE/stub_stream.py" STUB_MODE=ok \
        WWV_BUDDYINFO="$D/buddyinfo" SOAPY_SDR_ROOT=/nonexistent SOAPY_SDR_PLUGIN_PATH=/nonexistent \
        "$SCR/wwv_rsp2_live.sh" --build "$BUILD" --wwv-khz 5000 --secs 60 -- -p 0 > "$D/console.txt" 2>&1 &
    W=$!
    sleep 4
    kill -KILL "$W"; wait "$W" 2>/dev/null
    for _ in $(seq 150); do grep -q 'guard\] .*sdrplay.service:' "$D/console.txt" && break; sleep 0.1; done
    has 'exited without its cleanup; stopping what it started'; has 'guard]'; unit_off
    pgrep -f "$HERE/stub_stream.py" > /dev/null && fail 'stub_stream.py is still running'
    pgrep -f "$BUILD/wwv_live" > /dev/null && fail 'wwv_live is still running'
    end_case ;;
  auto_end)
    begin "$c"
    live STUB_MODE=ok STUB_SCAN_BANDS=5000 -- --wwv-khz auto --lock-mins 0 --secs 8 --scan-json "$D/scan.json"
    rc_is 0; has "going live on the scan's band: 5000 kHz"; ended_in 1 5000
    hasnt 'unbound variable'; hasnt SWITCH
    [ "$(cat "$D/scans" 2>/dev/null | wc -l)" = 1 ] || fail "$(cat "$D/scans" 2>/dev/null | wc -l) scans, want 1"
    [ -f "$D/scan.json" ] || fail 'no --scan-json file'
    streams_is 1; unit_off
    end_case ;;
  auto_switch)
    begin "$c"
    # lock limit 6 s: scan 1 s, 10 MHz 6 s, re-scan 1 s, then 15 MHz until --secs at 13 s
    live STUB_MODE=ok STUB_SCAN_BANDS=10000,15000 -- --wwv-khz auto --lock-mins 0.1 --secs 13 --scan-json "$D/scan.json"
    rc_is 0; has "going live on the scan's band: 10000 kHz"
    has 'SWITCH: 10000 kHz has not reached LOCKED in 0.1 min; stopping it to re-scan'
    has 'SWITCH: 10000 kHz -> 15000 kHz (segment 2)'
    has 'segment 2: WWV 15000 kHz'
    ended_in 2 15000
    hasnt 'unbound variable'
    sed -n 2p "$D/scans" | grep -qF -- '--avoid 10000' || fail 'the re-scan did not avoid 10000 kHz'
    [ -f "$D/scan.seg2.json" ] || fail 'no scan.seg2.json'
    [ -f "$D/run.seg2.log" ] || fail 'no run.seg2.log'
    streams_is 2; unit_off
    end_case ;;
  auto_rescan_end)
    begin "$c"
    live STUB_MODE=ok STUB_SCAN_BANDS=10000 STUB_SCAN_SECS=1,30 -- --wwv-khz auto --lock-mins 0.1 --secs 12
    rc_is 0; has 'SWITCH: 10000 kHz has not reached LOCKED in 0.1 min; stopping it to re-scan'
    has '--secs reached during the re-scan; stopping it'
    has '--secs reached before segment 2 could start'
    [ $((SECONDS - T0)) -lt 25 ] || fail "took $((SECONDS - T0)) s: the re-scan was not stopped at --secs"
    streams_is 1; unit_off
    end_case ;;
  auto_none)
    begin "$c"
    live STUB_SCAN_BANDS=none -- --wwv-khz auto --secs 30
    rc_is 3; has 'the band scan found no band'; has 'not going live'; streams_is 0; unit_off
    end_case ;;
  scan_hang)
    begin "$c"
    live STUB_SCAN_BANDS=hang -- --wwv-khz auto --secs 300 --scan-timeout 3
    rc_is 3; has 'SCAN: the band scan ran past its 3 s bound; stopping it'
    has 'wwv_band_scan.sh did not stop in 10 s after SIGTERM; killing it'
    has 'outlived its wrapper; killing it'
    has 'SCAN: restarting sdrplay.service and scanning once more'
    has 'SCAN: failed again after the restart: the band scan ran past its 3 s bound'
    has 'not going live'; streams_is 0; unit_off
    [ "$(cat "$D/scans" 2>/dev/null | wc -l)" = 2 ] || fail "$(cat "$D/scans" 2>/dev/null | wc -l) scans, want 2"
    # the unit was stopped and started again between the two scans
    [ "$(grep -cE 'systemctl (start|stop) sdrplay' "$D/calls")" -ge 4 ] || fail 'no restart of the unit between the scans'
    [ $((SECONDS - T0)) -lt 60 ] || fail "took $((SECONDS - T0)) s: the stuck scans were not ended within the bound"
    for p in $(cat "$D/hang.pids" 2>/dev/null); do kill -0 "$p" 2>/dev/null && { fail "stuck scan process $p still running"; kill -KILL "$p"; }; done
    end_case ;;
  scan_apifail)
    begin "$c"
    live STUB_SCAN_BANDS=apifail -- --wwv-khz auto --secs 300
    rc_is 3; has 'SCAN: restarting sdrplay.service and scanning once more'
    has 'SCAN: failed again after the restart: the band scan reports the sdrplay API failing (exit 5)'
    has 'not going live'; hasnt 'the band scan found no band'; streams_is 0; unit_off
    [ "$(cat "$D/scans" 2>/dev/null | wc -l)" = 2 ] || fail "$(cat "$D/scans" 2>/dev/null | wc -l) scans, want 2"
    [ $((SECONDS - T0)) -lt 30 ] || fail "took $((SECONDS - T0)) s"
    end_case ;;
  scan_apifail_retry)
    begin "$c"
    live STUB_MODE=ok STUB_SCAN_BANDS=apifail,5000 -- --wwv-khz auto --lock-mins 0 --secs 8
    rc_is 0; has 'SCAN: restarting sdrplay.service and scanning once more'
    hasnt 'failed again'; has "going live on the scan's band: 5000 kHz"; ended_in 1 5000
    streams_is 1; unit_off
    end_case ;;
  scan_py_api)
    begin "$c"
    : > "$D/console.txt"
    # mode, bound s: the bands are 3 reads of 2 s; a read is bounded at 1 + 2 + 10 s, and a
    # read stuck in the driver at 10 s past that
    for spec in 'actfail:sdrplay_api_Fail 8' 'actfail0:sdrplay_api_Fail 8' 'actfail:sdrplay_api_ServiceNotResponding 8' \
                'stuck 30' 'dead 35'; do
      set -- $spec
      t=$SECONDS
      env SOAPY_SDR_ROOT=/nonexistent python3 -I "$HERE/stream_fake_soapy.py" "$REPO/scripts/wwv_band_scan.py" "$1" -- \
          --bands 2500,5000,10000 --secs 2 --json "$D/api.json" > "$D/api.out" 2>> "$D/console.txt"
      r=$?
      dt=$((SECONDS - t))
      echo "[scan_py_api] $1: exit $r after $dt s" >> "$D/console.txt"
      [ "$r" = 5 ] || fail "$1: exit $r, want 5"
      [ "$dt" -le "$2" ] || fail "$1: took $dt s, bound $2 s"
      grep -qF 'BEST none (API failure)' "$D/api.out" || fail "$1: no 'BEST none (API failure)'"
      python3 -I -c "import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if d['best_khz'] is None and d['api_failure'] else 1)" \
          "$D/api.json" || fail "$1: --json lacks best_khz null and api_failure"
      /bin/rm -f "$D/api.json"
    done
    has 'activateStream failed (returned -1): band not read'
    has 'activateStream failed (returned 0): band not read'
    has 'API FAILURE: the module logged sdrplay_api_ServiceNotResponding'
    has 'read stuck in the driver 10 s past its wall-clock bound'
    has 'read ended at its wall-clock bound (13 s) with 0 of 4000000 samples'
    end_case ;;
  stream_stall)
    begin "$c"
    env SOAPY_SDR_ROOT=/nonexistent python3 -I "$HERE/stream_fake_soapy.py" "$REPO/scripts/rsp2_stream.py" stall_after:2 -- \
        --secs 20 --stall-secs 2 --heartbeat "$D/hb" > /dev/null 2> "$D/console.txt"
    has '[harness] exit 75'; has 'stalled: no samples for'
    end_case ;;
  stream_lag)
    begin "$c"
    env SOAPY_SDR_ROOT=/nonexistent python3 -I "$HERE/stream_fake_soapy.py" "$REPO/scripts/rsp2_stream.py" lossy:0.5 -- \
        --secs 4 --report-secs 1 --stall-secs 0 --no-fill > /dev/null 2> "$D/console.txt"
    has '[harness] exit 0'; has 'behind wall clock'; has 'samples lost upstream'
    end_case ;;
  scan_keep_raw)
    begin "$c"
    env SOAPY_SDR_ROOT=/nonexistent python3 -I "$HERE/stream_fake_soapy.py" "$REPO/scripts/wwv_band_scan.py" ok -- \
        --bands 5000,10000 --secs 6 --extend-secs 0 --keep-raw "$D/raw" --json "$D/live.json" > "$D/console.txt" 2>&1
    has '[harness] exit 0'; has 'BEST '
    ls "$D"/raw/scan_*_5000.cs16 "$D"/raw/scan_*_10000.timelog.npy "$D"/raw/scan_*_10000.json > /dev/null 2>&1 ||
      fail 'no kept raw windows'
    p5=$(ls "$D"/raw/scan_*_5000.cs16 2>/dev/null); p10=$(ls "$D"/raw/scan_*_10000.cs16 2>/dev/null)
    python3 -I "$REPO/scripts/wwv_band_scan.py" --replay "${p5%.cs16}:5000" --replay "${p10%.cs16}:10000" \
        --secs 6 --extend-secs 0 --json "$D/replay.json" >> "$D/console.txt" 2>&1 || fail 'the replay failed'
    python3 -I - "$D/live.json" "$D/replay.json" >> "$D/console.txt" 2>&1 <<'PY' || fail 'the replay of the kept windows differs from the scan'
import json, sys
a, b = ({r['khz']: r for r in json.load(open(f))['rows']} for f in sys.argv[1:3])
bad = [(k, f) for k in a for f in ('tick_contrasts_db', 'bcd_contrasts_db', 'cn0_dbhz', 'score_pct', 'bcd_pct')
       if a[k].get(f) != b.get(k, {}).get(f)]
print('replay check:', 'same' if not bad else f'differs: {bad}')
sys.exit(1 if bad or not a else 0)
PY
    end_case ;;
  mem_now)
    begin "$c"
    live STUB_MODE=ok -- --wwv-khz 5000 --secs 3
    rc_is 0; ended_in 1 5000
    has 'MEMWAIT: before starting sdrplay.service, Normal zone: Node 0, zone Normal 3989 16137 11122 5590 3206 995 222 54 53 96 22'
    has 'MEMWAIT: 118 free blocks at order >= 9; going ahead'; hasnt 'waiting up to'
    [ "$(compactions)" = 1 ] || fail "$(compactions) compactions, want 1 (compacted once, not again by sdrplay_start)"
    unit_off
    end_case ;;
  mem_delay)
    begin "$c"
    buddy 0 0
    ( sleep 4; buddy 3 0 ) &
    live STUB_MODE=ok -- --wwv-khz 5000 --secs 3 --mem-wait-secs 30 --mem-recompact-secs 1
    wait
    rc_is 0; ended_in 1 5000
    has 'MEMWAIT: before starting sdrplay.service, Normal zone: Node 0, zone Normal 3989 16137 11122 5590 3206 995 222 54 53 0 0'
    has 'MEMWAIT: no free block at order >= 9; waiting up to 30 s, compacting every 1 s'
    t=$(grep -oE 'MEMWAIT: 3 free blocks at order >= 9 after [0-9]+ s' "$D/console.txt" | grep -oE '[0-9]+ s$' | cut -d' ' -f1)
    [ -n "$t" ] && [ "$t" -ge 3 ] && [ "$t" -le 8 ] || fail "the wait did not end 3-8 s in when the blocks appeared (got '${t}')"
    [ "$(compactions)" -ge 3 ] || fail "$(compactions) compactions, want 3 or more (one each second of the wait)"
    a=$(grep -n 'MEMWAIT: 3 free blocks' "$D/console.txt" | cut -d: -f1); b=$(grep -n 'starting sdrplay.service$' "$D/console.txt" | head -1 | cut -d: -f1)
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ] || fail 'the unit was started before the wait ended'
    unit_off
    end_case ;;
  mem_bound)
    begin "$c"
    buddy 0 0
    live STUB_MODE=ok -- --wwv-khz 5000 --secs 3 --mem-wait-secs 3 --mem-recompact-secs 1
    rc_is 0; ended_in 1 5000
    has 'MEMWAIT: WARNING: no free block at order >= 9 after 3 s'; has 'going ahead: Node 0, zone Normal'
    [ "$(compactions)" -ge 3 ] || fail "$(compactions) compactions, want 3 or more"
    [ $((SECONDS - T0)) -lt 20 ] || fail "took $((SECONDS - T0)) s: the wait was not bounded"
    unit_off
    end_case ;;
  backoff_seq)
    begin "$c"
    live STUB_MODE=crash_after:1 -- --wwv-khz 5000 --secs 120 --reopen-max 4 --backoff-secs 1 --backoff-max 4
    rc_is 4; has 'REOPEN: 5 reopens, more than --reopen-max 4; ending the run'; streams_is 5
    seq=$(grep -oE 'BACKOFF: waiting [0-9]+ s' "$D/console.txt" | grep -oE '[0-9]+' | paste -sd' ')
    [ "$seq" = '1 2 4 4' ] || fail "backoffs '$seq', want '1 2 4 4'"
    has 'BACKOFF: waiting 4 s before the reopen (the service restarted, 18 errno=12 lines in the segment; crash reopen 4 in a row)'
    n=$(grep -c 'ERRNO12: ' "$D/console.txt"); [ "$n" = 5 ] || fail "$n ERRNO12 lines, want 5 (one per burst)"
    [ "$(grep -c 'ERRNO12: 18 libusb errno=12 lines from sdrplay.service' "$D/console.txt")" = 5 ] || fail 'an ERRNO12 line without the count 18'
    [ "$(grep -c 'MEMWAIT: before reopen' "$D/console.txt")" = 4 ] || fail 'no memory wait before every reopen'
    unit_off
    end_case ;;
  errno_burst)
    begin "$c"
    live STUB_MODE=errno_after:2 STUB_ONLY_FIRST=1 STUB_ERRNO=12 -- --wwv-khz 5000 --secs 12 --backoff-secs 1
    rc_is 0; has 'REOPEN: segment 1: rsp2_stream had no samples for 10 s'
    n=$(grep -c 'ERRNO12: ' "$D/console.txt"); [ "$n" = 1 ] || fail "$n ERRNO12 lines, want 1"
    has 'ERRNO12: 12 libusb errno=12 lines from sdrplay.service, '
    has 'BACKOFF: waiting 1 s before the reopen (12 errno=12 lines in the segment; crash reopen 1 in a row)'
    ended_in 2 5000; streams_is 2; unit_off
    end_case ;;
  gap76)
    begin "$c"
    live STUB_MODE=gap76_after:2 STUB_ONLY_FIRST=1 -- --wwv-khz 10000 --secs 10
    rc_is 0; has 'REOPEN: segment 1: rsp2_stream lost more samples than it fills'
    ended_in 2 10000; streams_is 2; unit_off
    end_case ;;
  stream_gaps|stream_ramp)
    begin "$c"
    if [ "$c" = stream_gaps ]; then
      spec='true:jitter:30;loss@6:0.3;loss@14:1.05;outage@24:5'; secs=72; ngaps=3
    else
      spec='true:jitter:30;ramp@8-38:0.003'; secs=74; ngaps=5
    fi
    env FAKE_MTU=16384 SOAPY_SDR_ROOT=/nonexistent python3 -I "$HERE/stream_fake_soapy.py" "$REPO/scripts/rsp2_stream.py" \
        "$spec" -- --rate 500000 --secs "$secs" --stall-secs 10 --report-secs 30 --gap-log "$D/gaps.jsonl" \
        --anchor-out "$D/anchor.json" > "$D/stream.cs16" 2> "$D/console.txt"
    has '[harness] exit 0'; has '] GAP '
    [ "$c" = stream_gaps ] && has 'cause: timeout'
    python3 -I "$HERE/check_gapfill.py" "$D/stream.cs16" 500000 "$D/console.txt" --gaps "$ngaps" --max-ms 40 \
        >> "$D/console.txt" 2>&1 || fail 'the stream is not gap-exact (check_gapfill)'
    grep -q '"hold_s": 3.0' "$D/anchor.json" 2>/dev/null || fail 'no --anchor-out file'
    [ "$(wc -l < "$D/gaps.jsonl")" -ge "$ngaps" ] || fail 'too few --gap-log lines'
    /bin/rm -f "$D/stream.cs16"
    end_case ;;
  stream_toobig)
    begin "$c"
    env FAKE_MTU=16384 SOAPY_SDR_ROOT=/nonexistent python3 -I "$HERE/stream_fake_soapy.py" "$REPO/scripts/rsp2_stream.py" \
        'true:outage@5:12' -- --rate 500000 --secs 40 --stall-secs 20 > "$D/stream.cs16" 2> "$D/console.txt"
    has '[harness] exit 76'; has 'GAP too large to fill: lost about'
    python3 -I - "$D/stream.cs16" >> "$D/console.txt" 2>&1 <<'PY' || fail 'samples after the loss were sent'
import sys, numpy as np
x = np.fromfile(sys.argv[1], np.int16).reshape(-1, 2).astype(np.int64)
k = (x[:, 0] & 0x7fff) | ((x[:, 1] - 1) << 15)
d = np.diff(k)
print('stream_toobig: samples', len(k), 'steps in k', int((d != 1).sum()))
sys.exit(0 if len(k) and (d == 1).all() else 1)
PY
    /bin/rm -f "$D/stream.cs16"
    end_case ;;
  gap_chain)
    begin "$c"
    for m in fill drop; do
      python3 -I "$HERE/wwv_synth.py" 330 --gap "250.3:1.05:$m" |
        "$BUILD/wwv_live" -r 50000 -p 0 --no-tones -S 60 -T 1795000000 --log "$D/$m.log" > "$D/$m.con" 2>&1 ||
        fail "wwv_live on the $m stream failed"
      "$BUILD/tests/replay_events" "$D/$m.log" > "$D/$m.rep" 2>&1 || fail "replay of $m failed"
    done
    cat "$D/fill.con" "$D/fill.rep" > "$D/console.txt"
    has 'GAP      ' ; has 'GAPEND   stream 250.3 s, 1050 ms'
    grep -q 'TENTATIVE -> LOCKED' "$D/fill.con" || fail 'never LOCKED before the gap'
    grep -qE '^PHASE .* breaks=0 epoch=0$' "$D/fill.rep" || fail "filled: the phase broke: $(grep ^PHASE "$D/fill.rep")"
    grep -qE '^PHASE .* breaks=[1-9]' "$D/drop.rep" || fail "dropped: no break, so the test shows nothing: $(grep ^PHASE "$D/drop.rep")"
    grep -q '^GAP ' "$D/drop.log" && fail 'a GAP line in the unfilled stream'
    # 3 ms/s lost for 3 minutes after LOCKED, filled as rsp2_stream's fast stage fills it
    python3 -I "$HERE/wwv_synth.py" 330 --ramp 130:310:0.003 |
      "$BUILD/wwv_live" -r 50000 -p 0 --no-tones -S 60 -T 1795000000 --log "$D/ramp.log" > "$D/ramp.con" 2>&1 ||
      fail 'wwv_live on the ramp failed'
    "$BUILD/tests/replay_events" "$D/ramp.log" > "$D/ramp.rep" 2>&1 || fail 'replay of the ramp failed'
    grep -E '^(GAPS|PHASE)' "$D/ramp.con" "$D/ramp.rep" >> "$D/console.txt"
    grep -qE '^GAPS     [1-9][0-9]* filled gaps' "$D/ramp.con" || fail 'ramp: no fills'
    grep -qE '^PHASE .* breaks=0 epoch=0$' "$D/ramp.rep" || fail "ramp: the phase broke: $(grep ^PHASE "$D/ramp.rep")"
    python3 -I - "$D/fill.log" "$D/fill.rep" "$REPO/include/wwv_detector_manager.h" >> "$D/console.txt" 2>&1 <<'PY' || fail 'held-back events differ from the overlap rule'
import re, sys
log, rep, hdr = sys.argv[1:4]
k = {m.group(1): float(m.group(2)) for m in re.finditer(r'#define (WWV_GAP_\w+)\s+([\d.]+)', open(hdr).read())}
gaps, ends, ev = [], [], []
order_ok = True
for l in open(log):
    f = dict(re.findall(r'(\w+)=([-\d.]+)', l))
    w = l.split(' ', 1)[0]
    if w == 'GAP': gaps.append((float(f['start']), float(f['len'])))
    elif w == 'GAPEND': ends.append({x: int(f[x]) for x in ('tick', 'tmark', 'mark', 'cand', 'btime', 'bfreq')})
    elif w in ('TICK', 'TMARK', 'MARK', 'CANDT', 'CANDM', 'BTIME', 'BFREQ'):
        g = {k2: float(v) for k2, v in f.items()}
        lo, hi = {'TICK': (g.get('t', 0) - g.get('dur', 0) - 20, g.get('t', 0) + 20),
                  'TMARK': (g.get('lead'), g.get('trail')), 'MARK': (g.get('lead'), g.get('t')),
                  'CANDT': (g.get('start'), g.get('end')), 'CANDM': (g.get('lead'), g.get('t')),
                  'BTIME': (g.get('t', 0) - 20, g.get('t', 0) + g.get('dur', 0) + 20),
                  'BFREQ': (g.get('t', 0) - 20, g.get('t', 0) + g.get('dur', 0) + 20)}[w]
        lo, hi = min(lo, hi), max(lo, hi)
        ev.append((w, lo, hi, len(gaps)))
key = {'TICK': 'tick', 'TMARK': 'tmark', 'MARK': 'mark', 'CANDT': 'cand', 'CANDM': 'cand', 'BTIME': 'btime', 'BFREQ': 'bfreq'}
want = dict.fromkeys(key.values(), 0); inside = 0
for w, lo, hi, ngap in ev:
    for s, ln in gaps[:ngap]:      # only spans marked before the event's line
        settle = min(k['WWV_GAP_SETTLE_MS'], k['WWV_GAP_SETTLE_MIN_MS'] + ln)
        if hi >= s - k['WWV_GAP_GUARD_MS'] and lo <= s + ln + settle:
            want[key[w]] += 1; break
    for i, (s, ln) in enumerate(gaps):
        if lo >= s and hi <= s + ln and i >= ngap:
            inside += 1           # an event inside a span, logged before the span's GAP line
got = {x: sum(e[x] for e in ends) for x in want}
rd = dict(re.findall(r'(\w+)=(\d+)', next((l for l in open(rep) if l.startswith('GAPDROPS')), '')))
rd = {x: int(rd.get(x, -1)) for x in want}
print('gap_chain: gaps', gaps, 'overlap rule', want, 'live GAPEND', got, 'replay', rd, 'inside before GAP', inside)
sys.exit(0 if gaps and want == got == rd and sum(want.values()) > 0 and inside == 0 else 1)
PY
    end_case ;;
  *) echo "unknown case: $c" >&2; FAIL=$((FAIL + 1)) ;;
  esac
done
echo "stub tests: $PASS cases passed, $FAIL failed"
[ "$FAIL" = 0 ]
