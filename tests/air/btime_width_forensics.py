"""Per-pulse width forensics for the BCD time detector.
Usage: python3 btime_width_forensics.py frames.f32 conv.json [label] [--rows N]

frames.f32 is tests/air/btime_frames output. Needs numpy.

Each BTIME event that starts within -100..+250 ms of a UTC second is joined to the
symbol sent in that second (score.py's frame_bit). For each one it reports:
  start_off  event start minus the UTC second (ms)
  dur        the detector's width
  ref_end    where the ripple-free reference (I/Q power over one 100 Hz period) falls
             through the half-way point between the second's high and low subcarrier
             levels, minus the UTC second (ms): the physical end of the high part
  tail       detector end minus ref_end (ms): how long the detector stayed IN_PULSE
             after the subcarrier had dropped
  low/thr    the low-level subcarrier's median detector energy after the pulse, over
             threshold_low: above 1 means the low level alone holds a pulse open
  tail_low   share of the frames between ref_end and the detector end whose energy was
             below threshold_low (the end needs MIN_LOW_FRAMES of them in a row)
For a sent 1 or P read as a shorter class it also reports the detector state at +45 ms,
where the high level has risen: COOLDOWN there means the true leading edge was not seen,
and the event started later.
"""
import sys, json, math, collections
import numpy as np

fn, conv = sys.argv[1], sys.argv[2]
label = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith('--') else fn
NROWS = int(sys.argv[sys.argv.index('--rows') + 1]) if '--rows' in sys.argv else 0
U0 = json.load(open(conv))['out_utc0_unix']
exec(open(__file__.replace('btime_width_forensics.py', 'symbol_truth.py')).read())

R = np.fromfile(fn, dtype=np.float32).reshape(-1, 8).astype(np.float64)
t, en, fl, th, tl, state, ref, pulse = R.T
FR = 256 * 1000.0 / 50000
NOM = {'0': 170.0, '1': 470.0, 'P': 770.0}   # high-level part, starting 30 ms after the second


def at(ms):
    return int(round(ms / FR))


rows = []
for e in np.nonzero(pulse > 0)[0]:
    dur = float(pulse[e])
    s0 = e - int(round(dur / FR)) + 1          # pulse start frame
    tstart = t[s0]
    u = U0 + tstart / 1000
    s = round(u)
    off = (u - s) * 1000
    if not (-100 <= off <= 250): continue
    truth = frame_bit(s)
    if truth not in NOM: continue
    sec_ms = (s - U0) * 1000                   # the UTC second in stream ms
    ra = np.sqrt(np.maximum(ref, 0))           # amplitude
    hi = np.median(ra[at(sec_ms + 60):at(sec_ms + 160)])
    lo = np.median(ra[at(sec_ms + 830):at(sec_ms + 980)])
    half = 0.5 * (hi + lo)
    k0 = at(sec_ms + 60)
    seg = ra[k0:at(sec_ms + 1000)]
    below = np.nonzero(seg < half)[0]
    ref_end = (k0 + below[0]) * FR - sec_ms + FR if len(below) else float('nan')
    det_end = off + dur
    low_e = np.median(en[at(sec_ms + NOM[truth] + 130):at(sec_ms + 980)]) if truth != 'P' else float('nan')
    low_thr = low_e / tl[s0] if truth != 'P' else float('nan')
    # frames between ref_end and the detector end: share of frames with energy below thr_lo
    a, b = at(sec_ms + ref_end), e
    gap = en[a:b] < tl[a:b] if b > a else np.array([])
    rows.append(dict(sec=s, truth=truth, off=off, dur=dur, ref_end=ref_end, det_end=det_end,
                     tail=det_end - ref_end, low_thr=low_thr, hi_lo_db=20 * math.log10(hi / lo) if lo > 0 else float('nan'),
                     gap_low_frac=float(gap.mean()) if len(gap) else float('nan'), e=e, s0=s0, sec_ms=sec_ms))

cls = lambda r: '0' if r['dur'] <= 350 else ('1' if r['dur'] <= 650 else 'P')
print(f'===== BTIME WIDTH {label}: boundary events={len(rows)}')
for tr in '01P':
    g = [r for r in rows if r['truth'] == tr]
    if not g: continue
    c = collections.Counter(cls(r) for r in g)
    d = np.array([r['dur'] for r in g]); re_ = np.array([r['ref_end'] for r in g]); ta = np.array([r['tail'] for r in g])
    so = np.array([r['off'] for r in g])
    print(f'  sent {tr}: n={len(g)} read 0/1/P={c["0"]}/{c["1"]}/{c["P"]}  dur p10/50/90='
          f'{np.percentile(d,10):.0f}/{np.median(d):.0f}/{np.percentile(d,90):.0f}  start_off med={np.median(so):.0f}  '
          f'ref_end med={np.nanmedian(re_):.0f} (p10/p90 {np.nanpercentile(re_,10):.0f}/{np.nanpercentile(re_,90):.0f})  '
          f'tail p10/50/90={np.nanpercentile(ta,10):.0f}/{np.nanmedian(ta):.0f}/{np.nanpercentile(ta,90):.0f}')
for name, g in (('sent 0 read 1', [r for r in rows if r['truth'] == '0' and cls(r) != '0']),
                ('sent 0 read 0', [r for r in rows if r['truth'] == '0' and cls(r) == '0'])):
    if not g: continue
    lt = np.array([r['low_thr'] for r in g]); re_ = np.array([r['ref_end'] for r in g])
    ta = np.array([r['tail'] for r in g]); so = np.array([r['off'] for r in g])
    gl = np.array([r['gap_low_frac'] for r in g])
    print(f'  {name}: n={len(g)}  start_off med={np.median(so):.0f}  ref_end med={np.nanmedian(re_):.0f} '
          f'(>350: {int(np.sum(re_ > 350))})  tail med={np.nanmedian(ta):.0f}  '
          f'low-level/thr_lo med={np.nanmedian(lt):.2f} (>1: {int(np.sum(lt > 1))})  '
          f'frames below thr_lo in the tail med={np.nanmedian(gl):.2f}')
short = collections.Counter()
for r in rows:
    if r['truth'] in '1P' and '01P'.index(cls(r)) < '01P'.index(r['truth']):
        k = at(r['sec_ms'] + 45)
        short[{0: 'idle', 1: 'in-pulse', 2: 'cooldown'}[int(state[k])] + '@+45ms'] += 1
if short:
    print('  sent 1/P read short, detector state at +45 ms: ' + ', '.join(f'{k}={v}' for k, v in sorted(short.items())))
if NROWS:
    print('  sec truth start_off dur ref_end det_end tail low/thr_lo hi/lo_dB')
    for r in [r for r in rows if r['truth'] == '0' and cls(r) != '0'][:NROWS]:
        print(f"   {r['sec']} {r['truth']} {r['off']:.0f} {r['dur']:.0f} {r['ref_end']:.0f} {r['det_end']:.0f} "
              f"{r['tail']:.0f} {r['low_thr']:.2f} {r['hi_lo_db']:.1f}")
