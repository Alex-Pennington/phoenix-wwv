"""Symbol error rates of the time decoder's input frames, per block of minutes.
Usage: python3 -I symbol_rates.py harness_or_replay_out.txt conv.json [block_minutes]

Reads the decoder's BCDS,FRAME lines: one character per second, as the decoder
holds them ('0' '1' 'P' a pulse that starts within boundary_ms of the second,
'~' a mid-second detection, '.' an erasure, '_' not locked). Truth is frame_bit()
from symbol_truth.py at the UTC minute of the frame's start (out_utc0_unix +
frame_start_ms, rounded to the minute). Second 0 carries no pulse and is skipped.

Per block (default 10 minutes, by the frame's UTC minute) and in total:
  erase   '.' over locked seconds
  mid     '~' over locked seconds that are not erased (the simulator's 'mid')
  0->1    sent 0 read 1, over sent 0 read 0/1/P at the second; 1->0 and P->1 alike
  0->P 1->P P->0  the other confusions, which the simulator does not draw
  res01   0->1 at the 15 reserved seconds alone (the decoder's p01 estimator)
  data01  0->1 at the other sent-0 seconds
Then the same for admitted frames only (the decoder's evidence), and the
spread of the per-frame 0->1 rate: its variance against the binomial variance
of a single rate (an overdispersion ratio well above 1 means the rate moves
between frames)."""
import sys, re, json, math, datetime, collections
fn, conv = sys.argv[1], sys.argv[2]
BLOCK = int(sys.argv[3]) if len(sys.argv) > 3 else 10
U0 = json.load(open(conv))['out_utc0_unix']
exec(open(__file__.replace('symbol_rates.py', 'symbol_truth.py')).read())
RESERVED = {1, 8, 14, 18, 24, 27, 28, 34, 42, 43, 44, 45, 46, 47, 48}

frames = []
for l in open(fn):
    if not l.startswith('TELEM BCDS FRAME,'): continue
    f = l.strip().split('BCDS ', 1)[1].split(',')
    start = float(f[1]); m = round((U0 + start / 1000) / 60) * 60
    frames.append((m, f[3], f[10] == '1'))


def tally(sel):
    c = collections.Counter()
    for m, fr, adm in sel:
        for s in range(1, 60):
            ch = fr[s]
            if ch == '_': continue
            c['locked'] += 1
            if ch == '.': c['erase'] += 1; continue
            if ch == '~': c['mid'] += 1; continue
            tr = frame_bit(m + s)
            c[(tr, ch)] += 1
            if tr == '0':
                k = 'res' if s in RESERVED else 'data'
                c[(k, 'n')] += 1; c[(k, ch)] += 1
    return c


def rate(a, b):
    return f'{a:4d}/{b:<4d} {100.0 * a / b:5.1f}%' if b else f'{a:4d}/{b:<4d}    - '


def line(label, c, nframes):
    n = {t: sum(c[(t, r)] for r in '01P') for t in '01P'}
    loc = c['locked']
    print(f'  {label:13s} fr {nframes:3d}  erase {rate(c["erase"], loc)}  mid {rate(c["mid"], loc - c["erase"])}  '
          f'0->1 {rate(c[("0", "1")], n["0"])}  1->0 {rate(c[("1", "0")], n["1"])}  P->1 {rate(c[("P", "1")], n["P"])}  '
          f'0->P {c[("0", "P")]} 1->P {c[("1", "P")]} P->0 {c[("P", "0")]}  '
          f'res01 {rate(c[("res", "1")], c[("res", "0")] + c[("res", "1")])}  '
          f'data01 {rate(c[("data", "1")], c[("data", "0")] + c[("data", "1")])}')


def report(title, sel):
    print(f'===== SYMBOL RATES {title}: {len(sel)} frames, {BLOCK}-minute blocks (UTC of frame start)')
    blocks = collections.OrderedDict()
    for fr in sel:
        b = fr[0] // (BLOCK * 60) * (BLOCK * 60)
        blocks.setdefault(b, []).append(fr)
    for b, sl in blocks.items():
        t = datetime.datetime.fromtimestamp(b, datetime.timezone.utc).strftime('%H:%M')
        line(t, tally(sl), len(sl))
    line('total', tally(sel), len(sel))


report('all frames', frames)
report('admitted frames', [f for f in frames if f[2]])

# Per-frame 0 -> 1 spread, admitted frames with at least 10 sent 0s read at the second
xs = []
for m, fr, adm in frames:
    if not adm: continue
    c = tally([(m, fr, adm)])
    n0 = c[('0', '0')] + c[('0', '1')]
    if n0 >= 10: xs.append((c[('0', '1')], n0))
k = sum(a for a, _ in xs); n = sum(b for _, b in xs); p = k / n if n else 0.0
if len(xs) > 1 and 0.0 < p < 1.0:
    chi = sum((a - b * p) ** 2 / (b * p * (1 - p)) for a, b in xs)
    print(f'  per-frame 0->1 (admitted, >=10 sent 0s): {len(xs)} frames, pooled {100 * p:.1f}%, '
          f'overdispersion (chi2/dof) {chi / (len(xs) - 1):.2f}')
