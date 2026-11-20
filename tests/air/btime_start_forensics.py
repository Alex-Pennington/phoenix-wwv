"""Where BTIME events start, and what the start threshold was cleared by.
Usage: python3 -I btime_start_forensics.py frames.f32 timebase.json [label] [--cols 9|8]

frames.f32 is a wwv_live --frames record or tests/air/btime_replay output (9 float32
per frame: t_ms energy noise thr_hi thr_lo state start_thr pulse_level pulse).
timebase.json gives out_utc0_unix (wwv_live -j, or a capture's conv json). Needs numpy.

Every BTIME event (valid or not is not recorded, so only events that ended with a
width: the pulse field) is placed by its start's offset in the UTC second:
  boundary  starts within -100..+250 ms of a second (a pulse rise: sent at +30 ms,
            seen at about +45 ms)
  mid       any other start: the event began between pulses
  carried   a mid event still running at +60 ms of the next second, so it holds that
            second's pulse
For each second the script measures, from the energy alone:
  hi   median energy over +80..+180 ms (inside every symbol's high part)
  lo   median energy over the second's low part (after the high part ends + 80 ms,
       to +1000 ms); a P second's low part is too short and is not used
At each event start it reports:
  E/lo    start energy over the low level of the second the start lies in
  E/hi    start energy over the next pulse's high level (mid) or this one's (boundary)
  rise    start energy over the median of the frames 15-40 ms before the start
  thr     what set the start threshold: 'noise' (2 x noise floor) or 'level' (half the
          tracked pulse level), and the threshold over lo (thr_lo)
  hilo    hi over lo: the pulse/low ratio around the start
  Lhi     the tracked pulse level over hi
  after_cd  the start is within two frames of a cooldown's end (the last cooldown
          frame records IDLE)
"""
import sys, json, math, collections
import numpy as np

fn, tb = sys.argv[1], sys.argv[2]
label = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith('--') else fn
U0 = json.load(open(tb))['out_utc0_unix']
exec(open(__file__.replace('btime_start_forensics.py', 'symbol_truth.py')).read())
R = np.fromfile(fn, dtype=np.float32).reshape(-1, 9).astype(np.float64)
t, en, nf, th, tl, st, sthr, lvl, pulse = R.T
FR = 256 * 1000.0 / 50000
HIEND = {'0': 200.0, '1': 500.0, 'P': 800.0}
N = len(t)


def fi(ms):
    """frame index holding stream time ms"""
    return int(math.floor(ms / FR))


def sec_ms(s):
    return (s - U0) * 1000.0


_hl = {}


def hilo(s):
    if s in _hl: return _hl[s]
    tr = frame_bit(s)
    a = sec_ms(s)
    hi = lo = float('nan')
    if tr is not None and fi(a + 1020) < N and fi(a) >= 0:
        hi = float(np.median(en[fi(a + 80):fi(a + 180)]))
        if tr != 'P':
            lo = float(np.median(en[fi(a + HIEND[tr] + 80):fi(a + 1000)]))
    _hl[s] = (tr, hi, lo)
    return _hl[s]


rows = []
for e in np.nonzero(pulse > 0)[0]:
    s0 = e - int(round(pulse[e] / FR)) + 1
    if s0 < 10: continue
    u = U0 + t[s0] / 1000.0
    s = math.floor(u)
    o = (u - s) * 1000.0
    if o >= 900: s, o = s + 1, o - 1000.0
    kind = 'boundary' if o < 250 else 'mid'
    end_u = U0 + (t[e] + FR) / 1000.0
    carried = kind == 'mid' and end_u > s + 1 + 0.060
    # the low level the start lies in: this second's (mid) or the previous one's (boundary)
    tr_s, hi_s, lo_s = hilo(s)
    tr_n, hi_n, lo_n = hilo(s + 1)
    tr_p, hi_p, lo_p = hilo(s - 1)
    lo = lo_s if kind == 'mid' else lo_p
    hi = hi_n if kind == 'mid' else hi_s
    pre = en[max(0, s0 - 8):s0 - 2]
    rise = en[s0] / np.median(pre) if len(pre) else float('nan')
    thr_kind = 'level' if 0.5 * lvl[s0] > th[s0] else 'noise'
    rows.append(dict(s=s, o=o, kind=kind, carried=carried, E=en[s0], lo=lo, hi=hi,
                     Elo=en[s0] / lo if lo > 0 else float('nan'),
                     Ehi=en[s0] / hi if hi > 0 else float('nan'),
                     rise=rise, thr=thr_kind, thr_lo=sthr[s0] / lo if lo > 0 else float('nan'),
                     lvl0=lvl[s0] == 0, after_cd=bool(np.any(st[max(0, s0 - 3):s0 - 1] == 2)),
                     Lhi=lvl[s0] / hi if hi > 0 else float('nan'),
                     hilo=hi / lo if lo > 0 else float('nan'), dur=pulse[e]))


def pct(a, q):
    a = np.array([x for x in a if not math.isnan(x)])
    return np.percentile(a, q) if len(a) else float('nan')


def summ(name, g):
    if not g:
        print(f'  {name}: n=0'); return
    c = collections.Counter(r['thr'] for r in g)
    print(f'  {name}: n={len(g)}  thr noise/level={c["noise"]}/{c["level"]}  level dropped(0)={sum(r["lvl0"] for r in g)}  '
          f'after cooldown={sum(r["after_cd"] for r in g)}')
    for k in ('Elo', 'Ehi', 'rise', 'thr_lo', 'hilo', 'Lhi'):
        v = [r[k] for r in g]
        print(f'      {k:6s} p10/25/50/75/90 = ' + '/'.join(f'{pct(v, q):.2f}' for q in (10, 25, 50, 75, 90)))


n_sec = int((t[-1] - t[0]) / 1000)
B = [r for r in rows if r['kind'] == 'boundary']
M = [r for r in rows if r['kind'] == 'mid']
C = [r for r in M if r['carried']]
print(f'===== BTIME STARTS {label}: {len(rows)} events over {n_sec} s; boundary {len(B)}, mid {len(M)} '
      f'(carried through the next pulse {len(C)})')
summ('boundary', B)
summ('mid', M)
summ('carried', C)
# mid starts by offset bin, and whether they came right off a cooldown
h = collections.Counter(int(r['o'] // 100) * 100 for r in M)
print('  mid start offset (ms, 100 ms bins): ' + ' '.join(f'{k}:{h[k]}' for k in sorted(h)))
# separation: how many boundary / mid starts each rule keeps
print('  rule                         keeps boundary   keeps mid')
for name, f in (('rise >= 1.5', lambda r: r['rise'] >= 1.5), ('rise >= 2.0', lambda r: r['rise'] >= 2.0),
                ('rise >= 2.5', lambda r: r['rise'] >= 2.5),
                ('E/lo >= 2.0', lambda r: r['Elo'] >= 2.0), ('E/lo >= 2.4', lambda r: r['Elo'] >= 2.4),
                ('E/lo >= 3.0', lambda r: r['Elo'] >= 3.0)):
    kb = sum(1 for r in B if f(r)); km = sum(1 for r in M if f(r))
    print(f'  {name:28s} {kb:5d}/{len(B):<5d}      {km:5d}/{len(M):<5d}')
