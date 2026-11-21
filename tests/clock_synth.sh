#!/usr/bin/env bash
# The CLOCK line through the whole chain, on synthetic WWV with a real time code and
# an anchor whose error is known.
#
#   tests/clock_synth.sh BUILD_DIR WORK_DIR
#
# tests/live/wwv_synth.py sends 15 minutes of WWV from 2026-11-21 12:00:00 UTC (sample 0
# 0.5 s before it). wwv_live gets it through --anchor-in, as from rsp2_stream.py, with
# in_sample0_unix off by ERR s, so utc_minus_anchor_s must come out as ERR. Checks, for
# ERR = +2.500 and -0.750:
#   - no CLOCK line before the time of day is CONFIRMED, and one per CONFIRMED minute;
#   - each line within 2 sigma of ERR, and the median error within 1 sigma;
#   - utc_minus_anchor_s = UTC(minute) - (in_sample0_unix + frame_stream_ms / 1000);
#   - band_khz and source as given; the same lines in --log;
#   - tests/air/replay_events on the log (its ANCHOR line) and with --anchor on the
#     --timebase file gives the same lines, t_unix apart, to 0.2 ms (the log keeps
#     event times to 0.1 ms).
# The synthetic stream has no radio path and no delivery delay, so ERR is all there is.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD=${1:?usage: clock_synth.sh BUILD_DIR WORK_DIR}
WORK=${2:?usage: clock_synth.sh BUILD_DIR WORK_DIR}
mkdir -p "$WORK"
U=1795262400                      # 2026-11-21T12:00:00Z
FAIL=0
python3 -I "$HERE/live/wwv_synth.py" 1 --selftest || FAIL=1
for ERR in 2.500 -0.750; do
  D=$WORK/err$ERR; mkdir -p "$D"
  python3 -I -c "import json, sys; json.dump({'source': 'rsp2_stream', 'in_sample0_unix': $U - 0.5 - $ERR, 'hold_s': 0.0}, open(sys.argv[1], 'w'))" "$D/anchor.json"
  python3 -I "$HERE/live/wwv_synth.py" 900 --utc "$U" |
    "$BUILD/wwv_live" -r 50000 -p 0 --no-tones -S 60 --anchor-in "$D/anchor.json" --band-khz 5000 \
      --log "$D/log.txt" -j "$D/tb.json" > "$D/con.txt" 2>&1 || { echo "FAIL: wwv_live, ERR $ERR"; FAIL=1; continue; }
  "$BUILD/tests/replay_events" "$D/log.txt" > "$D/rep.txt" 2>&1 || { echo "FAIL: replay, ERR $ERR"; FAIL=1; }
  "$BUILD/tests/replay_events" --anchor "$D/tb.json" --band-khz 5000 "$D/log.txt" > "$D/rep_tb.txt" 2>&1 ||
    { echo "FAIL: replay --anchor, ERR $ERR"; FAIL=1; }
  python3 -I - "$D" "$ERR" "$U" <<'PY' || FAIL=1
import json, sys, statistics, calendar
d, err, u0 = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
con = open(f'{d}/con.txt').read().splitlines()
bad = []
anchor = json.load(open(f'{d}/anchor.json'))['in_sample0_unix']
confirmed = False; lines = []; tod = None
for l in con:
    if l.startswith('         tod '):
        f = l.split()
        confirmed = f[2] == 'CONFIRMED'; tod = f[1]
    if l.startswith('CLOCK '):
        if not confirmed:
            bad.append(f'a CLOCK line before CONFIRMED: {l}')
        c = json.loads(l[6:]); c['tod'] = tod; lines.append(c)
nconf = sum(1 for l in con if l.startswith('         tod ') and l.split()[2] == 'CONFIRMED')
if not lines: bad.append('no CLOCK line')
if len(lines) != nconf: bad.append(f'{len(lines)} CLOCK lines for {nconf} CONFIRMED minutes')
errs = []
for c in lines:
    h, m = (int(x) for x in c['tod'].split(':'))
    utc = calendar.timegm((2026, 11, 21, h, m, 0))
    want = utc - (anchor + c['frame_stream_ms'] / 1000)
    if abs(c['utc_minus_anchor_s'] - want) > 2e-6: bad.append(f'utc_minus_anchor_s {c["utc_minus_anchor_s"]} != {want:.6f}')
    e = (c['utc_minus_anchor_s'] - err) * 1000; errs.append((e, c['sigma_ms']))
    if abs(e) > 2 * c['sigma_ms']: bad.append(f'{c["tod"]}: off {e:+.1f} ms, sigma {c["sigma_ms"]} ms')
    if c['band_khz'] != 5000 or c['source'] != 'rsp2_stream' or c['confirmed'] is not True: bad.append(f'fields: {c}')
if errs:
    me = statistics.median(e for e, _ in errs); ms = statistics.median(s for _, s in errs)
    if abs(me) > ms: bad.append(f'median error {me:+.1f} ms over median sigma {ms:.1f} ms')
    print(f'clock_synth ERR {err:+.3f} s: {len(lines)} CLOCK lines, errors ' + ' '.join(f'{e:+.1f}' for e, _ in errs) +
          f' ms, median {me:+.1f}, sigma {min(s for _, s in errs):.1f}..{max(s for _, s in errs):.1f} ms')
def strip(p):
    out = []
    for l in open(p):
        if l.startswith('CLOCK '):
            c = json.loads(l[6:]); c.pop('t_unix'); out.append(c)
    return out
def same(a, b, tol):
    """the same lines; numbers within tol ms (the log keeps event times to 0.1 ms)"""
    if len(a) != len(b): return False
    for x, y in zip(a, b):
        if x.keys() != y.keys(): return False
        for k in x:
            if isinstance(x[k], float) and not isinstance(x[k], bool):
                scale = 1000 if k == 'utc_minus_anchor_s' else 1
                if abs(x[k] - y[k]) * scale > tol: return False
            elif x[k] != y[k]: return False
    return True
live = strip(f'{d}/con.txt')
if strip(f'{d}/log.txt') != live: bad.append('log.txt: CLOCK lines differ from the console')
for name in ('rep.txt', 'rep_tb.txt'):
    if not same(strip(f'{d}/{name}'), live, 0.2): bad.append(f'{name}: CLOCK lines differ from the console')
for b in bad: print('  FAIL:', b)
sys.exit(1 if bad else 0)
PY
done
[ "$FAIL" = 0 ] && echo "clock_synth: ok" || echo "clock_synth: FAILED"
exit "$FAIL"
