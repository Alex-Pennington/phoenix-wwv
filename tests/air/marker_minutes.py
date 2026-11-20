"""Classify every true UTC minute marker by what each marker path did with it.

Usage: python3 -I marker_minutes.py tb.json log [console ...] [-v]

tb.json is a live or capture timebase (out_utc0_unix = UTC of stream time 0). The
log and console files are read together: chain-log lines (TICK, TMARK, MARK,
SYNC) and the detectors' stdout lines ("[   59.8s] REJECTED: ...", "MARKER
rejected: not a tone", "MARKER timed out", "MARKER short run", "MARKER block
peak"). A harness output holds both; a live run has log.txt and console.txt.

For each :00 from the first one 12 s into the stream (the marker detector's
startup) to the last one 2 s before the end, each path gets one outcome:

  tick path (TMARK, run of tick-band energy 600-1500 ms):
    detected      a TMARK whose leading edge is within +-300 ms of :00
    late-start    a run that ends at the marker's end (:00 +700..+1000 ms) but is
                  too short, because it started late. Split by what the detector
                  was doing at :00:
                    cooldown  the run started on the first frame after a 500 ms
                              cooldown (idle= 0-10 ms, logged since this branch),
                              so the energy was already up while the detector
                              was blind; in older logs, a TICK or rejected run
                              ended in the 512 ms before :00 +20 ms
                    weak      the run started after the detector had been idle
                    weak?     older logs, no logged run ended there; the runs the
                              tick detector drops without a log line (a blip
                              failing the correlation test) also take the
                              cooldown, so these are mostly cooldown too
    fragment      a rejected run inside the marker that does not reach its end
    early/late    a TMARK 300 ms-5 s off :00
    no-run        nothing above
  marker detector (MARK, 1 s sliding sum at 3x its baseline):
    detected      a MARK whose leading edge is within +-300 ms of :00
    early/late    a MARK 300 ms-5 s off :00
    purity        a "not a tone" rejection whose run covers :00
    short         a run too short to count, starting near :00
    cooldown      in the 30 s cooldown of an earlier run (a MARK, a timeout, or
                  a short run)
    no-run        the window sum never reached the threshold; with the block
                  peaks logged since this branch, the largest sum/baseline in
                  :00 -0.5..+2.5 s
    unrecorded    nothing logged (builds before this branch did not report
                  short runs or block peaks): no run, a short run, or a
                  cooldown after a short run

-v prints one line per minute. --step MS:OFS shifts every :00 after stream time MS by OFS
ms, for a run whose stream lost samples (run90d: an RSP2 timeout near 1077 s lost
about 1.1 s, so stream time runs 1.1 s behind UTC after it)."""
import sys, re, json, math, datetime

argv = sys.argv[1:]
step = None
if '--step' in argv:
    i = argv.index('--step'); a, b = argv[i + 1].split(':'); step = (float(a), float(b))
    del argv[i:i + 2]
args = [a for a in argv if a != '-v']
verbose = '-v' in argv
U0 = json.load(open(args[0]))['out_utc0_unix']
lines = []
for p in args[1:]:
    lines += open(p, errors='replace').read().splitlines()

def fld(l, k):
    m = re.search(r'(?:^|\s)' + k + r'=([-\d.]+)', l)
    return float(m.group(1)) if m else None

ticks, tmarks, marks, trej, mrej, mpeaks = [], [], [], [], [], []
end_ms = 0.0
for l in lines:
    if l.startswith('TICK '):
        t, d = fld(l, 't'), fld(l, 'dur')
        if t is not None: ticks.append((t, d)); end_ms = max(end_ms, t)
    elif l.startswith('TMARK '):
        tmarks.append((fld(l, 'lead'), fld(l, 'trail'), fld(l, 'dur')))
    elif l.startswith('MARK '):
        marks.append((fld(l, 'lead'), fld(l, 't'), fld(l, 'dur')))
    elif l.startswith('BTIME '):
        a = fld(l, 'at')
        if a: end_ms = max(end_ms, a)
    elif l.startswith('END '):
        s = fld(l, 'samples')
        if s: end_ms = max(end_ms, s / 50.0)
    else:
        m = re.match(r'\[\s*([\d.]+)s\] (.*)', l)
        if not m: continue
        t = float(m.group(1)) * 1000.0
        body = m.group(2)
        if body.startswith('REJECTED: dur='):
            d = float(re.search(r'dur=(\d+)', body).group(1))
            e = re.search(r' end=([-\d.]+)ms idle=(\d+)ms', body)
            if e: t = float(e.group(1))
            trej.append((t, d, 'gap' if 'gap zone' in body else
                         'interval' if 'marker-like' in body else 'other',
                         float(e.group(2)) if e else None))
        elif body.startswith('REJECTED: pulse >1s'):
            trej.append((t, 1000.0, 'long', None))
        elif body.startswith('MARKER rejected'):
            r = re.search(r'window run (\d+)ms, start=([-\d.]+)ms', body)
            mrej.append((t, float(r.group(1)), float(r.group(2)), 'purity'))
        elif body.startswith('MARKER timed out'):
            mrej.append((t, 5000.0, t - 5000.0, 'timeout'))
        elif body.startswith('MARKER short run'):
            r = re.search(r'window run (\d+)ms < \d+ms, start=([-\d.]+)ms', body)
            mrej.append((t, float(r.group(1)), float(r.group(2)),
                         'short-cd' if 'cooldown' in body else 'short'))
        elif body.startswith('MARKER block peak'):
            r = re.search(r'sum/baseline ([\d.]+) at ([-\d.]+)ms', body)
            mpeaks.append((t, float(r.group(1)), float(r.group(2))))

def stream_of_utc(u):
    t = (u - U0) * 1000.0
    if step and t > step[0]: t += step[1]
    return t
first = math.ceil((U0 + 12.0) / 60.0) * 60
minutes = []
u = first
while stream_of_utc(u) < end_ms - 2000.0:
    minutes.append(u); u += 60

def nearest(evts, T0, lim):
    best = None
    for e in evts:
        if e[0] is None: continue
        o = e[0] - T0
        if abs(o) <= lim and (best is None or abs(o) < abs(best[0])): best = (o, e)
    return best

def tick_outcome(T0):
    n = nearest(tmarks, T0, 300.0)
    if n: return 'detected', f'{n[0]:+.0f} ms'
    # rejected runs ending inside the marker window
    rs = [r for r in trej if T0 - 50 <= r[0] <= T0 + 1600]
    for (te, d, kind, idle) in rs:
        start = te - d
        if 700.0 <= te - T0 <= 1000.0 and kind == 'gap':
            if idle is not None:
                # logged since this branch: idle 0 = already above threshold
                # when the cooldown ended
                why = 'cooldown' if idle <= 2 * 5.12 else 'weak'
            else:
                # older logs: a logged TICK or rejected run in the cooldown
                # span; runs dropped without a log line are not seen, so
                # some 'weak' are cooldown too
                pre = [t for (t, dd) in ticks if T0 - 512.0 <= t <= T0 + 20.0]
                pre += [r[0] for r in trej if T0 - 512.0 <= r[0] <= T0 + 20.0]
                why = 'cooldown' if pre else 'weak?'
            return f'late-start/{why}', f'run {d:.0f} ms from {start - T0:+.0f} ms'
    for (te, d, kind, idle) in rs:
        if kind == 'gap' and te - d < T0 + 800.0:
            return 'fragment', f'run {d:.0f} ms from {te - d - T0:+.0f} ms'
        if kind == 'interval': return 'interval', f'run {d:.0f} ms'
        if kind == 'long': return 'too-long', ''
    n = nearest(tmarks, T0, 5000.0)
    if n: return 'early/late', f'{n[0]:+.0f} ms'
    return 'no-run', ''

def marker_outcome(T0):
    n = nearest(marks, T0, 300.0)
    if n: return 'detected', f'{n[0]:+.0f} ms'
    n = nearest(marks, T0, 5000.0)
    if n: return 'early/late', f'{n[0]:+.0f} ms'
    for (te, run, st, kind) in mrej:
        if kind == 'purity' and abs(st - T0) < 1500.0: return 'purity', f'start {st - T0:+.0f} ms'
        if kind == 'timeout' and te - 5000.0 <= T0 <= te: return 'timeout', ''
        if kind.startswith('short') and abs(st - T0) < 1500.0:
            return 'short', f'window run {run:.0f} ms, start {st - T0:+.0f} ms'
    # cooldown of an earlier logged run (MARK end + 30 s)
    for (lead, t, d) in marks:
        if t is not None and t < T0 < t + 30000.0: return 'cooldown', f'MARK ended {(t - T0)/1000:+.1f} s'
    for (te, run, st, kind) in mrej:
        if kind in ('timeout', 'short-cd') and te < T0 < te + 30000.0:
            return 'cooldown', f'{kind} ended {(te - T0)/1000:+.1f} s'
    pk = [p for (t, p, at) in mpeaks if T0 - 500.0 <= at <= T0 + 2500.0]
    if mpeaks:
        return 'no-run', f'peak sum/baseline {max(pk):.2f}' if pk else 'no block peak at :00'
    return 'unrecorded', ''

from collections import Counter
ct, cm, both, either = Counter(), Counter(), 0, 0
for u in minutes:
    T0 = stream_of_utc(u)
    a, ad = tick_outcome(T0)
    b, bd = marker_outcome(T0)
    ct[a] += 1; cm[b] += 1
    if a == 'detected' or b == 'detected': either += 1
    if a == 'detected' and b == 'detected': both += 1
    if verbose:
        hm = datetime.datetime.fromtimestamp(u, datetime.timezone.utc).strftime('%H:%M')
        print(f'  {hm} T0={T0:10.1f}  tick: {a:20s} {ad:28s} marker: {b:12s} {bd}')
N = len(minutes)
print(f'MINUTES {N}: either path {either}, both {both}')
print('  tick path: ' + ', '.join(f'{k} {v}' for k, v in sorted(ct.items(), key=lambda x: -x[1])))
print('  marker det: ' + ', '.join(f'{k} {v}' for k, v in sorted(cm.items(), key=lambda x: -x[1])))
