"""Where the time decoder's 1->0 and P->1 come from, on a live run with lost samples.
Usage: python3 -I seg_attr.py replay_out.txt btime_log.txt frames.f32 timebase.json console.txt
                              [--seg MARK] [--before S]

replay_out.txt  tests/air/replay_events output on btime_log.txt (its TELEM BCDS FRAME and
                WCLOSE lines)
btime_log.txt   the run's wwv_live --log, or tests/air/btime_replay's out_log (its BTIME lines)
frames.f32      wwv_live --frames, or btime_replay's out.f32 (9 float32 per frame)
timebase.json   out_utc0_unix (wwv_live -j)
console.txt     the run's console, for the loss; '-' for none (a log already gap-filled, or
                tests/air/shift_loss.py output)
--seg MARK      read the console's STATUS lines only after the last line holding MARK
                (e.g. 'segment 2:'); default the whole console
--before S      the "before the loss" figures use stream times below S seconds (default 1300)
Needs numpy.

Truth is corrected for the loss: UTC second u is at stream time t with
out_utc0_unix + t/1000 + loss(t) = u, where loss(t) is the least-delayed console lag (the A of
STATUS 'lag A..B', sampled every 30 s) less its first value. Without it, every second after
the first loss is scored against the wrong UTC second.

For each decoded 0/1/P at a frame's second (symbol_rates.py's frames and characters), it finds
the held second (the WCLOSE window's start + 100 ms) and the true second, and measures:
  held-true  the phase error; over 100 ms is 'phase off' (lost samples the tracker missed)
  contrast   median detector energy over +80..+180 ms of the true second over -160..-10 ms,
             in dB (the frame record's compressed scale); 'late' is the last 150..50 ms of
             the high part
  boundary   the BTIME events starting -100..+250 ms of the true second
It prints, for sent 1 read 0, sent P read 1 and the right reads: the phase-off count, and on
a right phase, how many had no boundary event or a short one, with their contrast; the phase
error over the run; error rates by contrast bin; and the BTIME lead (start of right-read 1 and
P boundary events against the true second) with the held phase before --before.
"""
import sys, re, json, math, bisect, collections
import numpy as np
if len(sys.argv) < 6:
    sys.exit(__doc__)
rep, btl, frf, tbf, con = sys.argv[1:6]
opt = sys.argv[6:]
seg = opt[opt.index('--seg') + 1] if '--seg' in opt else None
BEFORE_MS = float(opt[opt.index('--before') + 1]) * 1000 if '--before' in opt else 1300e3
U0 = json.load(open(tbf))['out_utc0_unix']
exec(open(__file__.replace('seg_attr.py', 'symbol_truth.py')).read())

# loss curve
L = []
on = seg is None
for l in (open(con, errors='replace') if con != '-' else []):
    if seg and seg in l: on = True; L = []; continue
    if not on or not l.startswith('STATUS'): continue
    m = re.search(r' t=(\d+)s .*lag ([+-]\d+)\.\.', l)
    if m: L.append((float(m.group(1)) * 1000, float(m.group(2))))
L0 = L[0][1] if L else 0.0
LT = [a for a, b in L]; LV = [b - L0 for a, b in L]
def loss(t):
    return float(np.interp(t, LT, LV)) if L else 0.0
def stream_of(u):
    t = (u - U0) * 1000.0
    for _ in range(3): t = (u - U0) * 1000.0 - loss(t)
    return t

R = np.fromfile(frf, dtype=np.float32).reshape(-1, 9).astype(np.float64)
ft, en = R[:, 0], R[:, 1]
FR = ft[1] - ft[0]
def med(a, b):
    i, j = int((a - ft[0]) / FR), int((b - ft[0]) / FR)
    if i < 0 or j >= len(en) or j <= i: return float('nan')
    return float(np.median(en[i:j]))

ev = []
for l in open(btl):
    if l.startswith('BTIME '):
        m = re.search(r't=([\d.]+) dur=([\d.]+)', l); ev.append((float(m.group(1)), float(m.group(2))))
ev.sort(); evt = [e[0] for e in ev]
def events(a, b):
    i = bisect.bisect_left(evt, a); out = []
    while i < len(ev) and ev[i][0] < b: out.append(ev[i]); i += 1
    return out

wins = []
frames = []
for l in open(rep):
    if l.startswith('WCLOSE '):
        m = re.search(r'win_start=([-\d.]+)', l); wins.append(float(m.group(1)) + 100.0)
    elif l.startswith('TELEM BCDS FRAME,'):
        f = l.strip().split('BCDS ', 1)[1].split(',')
        frames.append((float(f[1]), f[3], f[10] == '1'))
wins.sort()

def near_win(t):
    i = bisect.bisect_left(wins, t)
    c = [w for w in wins[max(0, i - 1):i + 1]]
    return min(c, key=lambda w: abs(w - t)) if c else float('nan')

def db(x): return 10 * math.log10(x) if x > 0 and not math.isnan(x) else float('nan')

HI = {'0': 230.0, '1': 530.0, 'P': 830.0}
rows = []
for start, fr, adm in frames:
    mu = round((U0 + start / 1000 + loss(start) / 1000) / 60) * 60
    for s in range(1, 60):
        ch = fr[s]; tr = frame_bit(mu + s)
        if tr is None or ch not in '01P': continue
        T = stream_of(mu + s)
        held = near_win(start + s * 1000.0)
        d = held - T
        pre = med(T - 160, T - 10)
        hi = med(T + 80, T + 180)
        late = med(T + HI[tr] - 150, T + HI[tr] - 50)
        bp = [e for e in events(T - 100, T + 250)]
        rows.append(dict(u=mu + s, tr=tr, ch=ch, d=d, t=T, adm=adm,
                         c_hi=db(hi / pre), c_late=db(late / pre), bp=bp,
                         fe=events(held - 100, held + 900)[:1]))

def summ(name, sel):
    print(f'== {name}: {len(sel)}')
    if not sel: return
    ph = [r for r in sel if abs(r['d']) > 100]
    ok = [r for r in sel if abs(r['d']) <= 100]
    print(f'  phase |held-true| >100 ms: {len(ph)}   <=100 ms: {len(ok)}')
    nob = [r for r in ok if not r['bp']]
    sh = [r for r in ok if r['bp']]
    print(f'  phase ok, no event starting -100..+250 ms of the true second: {len(nob)}; with one: {len(sh)}')
    for nm, g in (('phase ok, no boundary event', nob), ('phase ok, boundary event short', sh), ('phase off', ph)):
        a = np.array([r['c_hi'] for r in g if not math.isnan(r['c_hi'])])
        b = np.array([r['c_late'] for r in g if not math.isnan(r['c_late'])])
        if len(a):
            print(f'    {nm}: n={len(g)} contrast at +80..180 p10/50/90 {np.percentile(a,10):.1f}/{np.median(a):.1f}/{np.percentile(a,90):.1f} dB;'
                  f' late high part p10/50/90 {np.percentile(b,10):.1f}/{np.median(b):.1f}/{np.percentile(b,90):.1f} dB;'
                  f' <3 dB at +80..180: {int((a<3).sum())}')
    if sh:
        so = np.array([r['bp'][0][0] - r['t'] for r in sh]); du = np.array([r['bp'][0][1] for r in sh])
        print(f'    boundary event start offset p10/50/90 {np.percentile(so,10):.0f}/{np.median(so):.0f}/{np.percentile(so,90):.0f} ms, width p50 {np.median(du):.0f};'
              f' start >+100 ms: {int((so>100).sum())}')
    dd = np.array([r['d'] for r in sel])
    print(f'  held-true p10/50/90 {np.percentile(dd,10):.0f}/{np.median(dd):.0f}/{np.percentile(dd,90):.0f} ms')

summ('sent 1 read 0', [r for r in rows if r['tr'] == '1' and r['ch'] == '0'])
summ('sent P read 1', [r for r in rows if r['tr'] == 'P' and r['ch'] == '1'])
summ('sent 1 read 1 (right)', [r for r in rows if r['tr'] == '1' and r['ch'] == '1'])
summ('sent P read P (right)', [r for r in rows if r['tr'] == 'P' and r['ch'] == 'P'])
# all sent-1 and P seconds with phase ok: contrast split
allc = [r for r in rows if r['tr'] in '1P' and abs(r['d']) <= 100]
for th in (3, 5):
    lo = [r for r in allc if r['c_hi'] < th]
    w = sum(1 for r in lo if (r['tr'], r['ch']) in (('1', '0'), ('P', '1'), ('P', '0')))
    hi = [r for r in allc if r['c_hi'] >= th]
    w2 = sum(1 for r in hi if (r['tr'], r['ch']) in (('1', '0'), ('P', '1'), ('P', '0')))
    print(f'sent 1/P read 0/1/P, phase ok: contrast<{th} dB {w}/{len(lo)} shortened; >={th} dB {w2}/{len(hi)} shortened')
# phase error over time
dd = sorted((r['t'], r['d']) for r in rows)
for k in range(0, len(dd), max(1, len(dd)//12)):
    blk = dd[k:k + len(dd)//12]
    a = np.array([x[1] for x in blk])
    print(f'  t={blk[0][0]/1000:6.0f}s  held-true median {np.median(a):+5.0f}  |>100| {int((abs(a)>100).sum())}/{len(a)}')
# error rate by per-second contrast (phase ok)
print('contrast bins (dB at +80..180 over -160..-10), phase ok: sent1 ->0 | sentP ->1 or 0')
B = [(-99, 1), (1, 2), (2, 3), (3, 5), (5, 99)]
for a, b in B:
    s1 = [r for r in allc if r['tr'] == '1' and a <= r['c_hi'] < b]
    sp = [r for r in allc if r['tr'] == 'P' and a <= r['c_hi'] < b]
    e1 = sum(r['ch'] == '0' for r in s1); ep = sum(r['ch'] in '01' for r in sp)
    print(f'  [{a:3d},{b:3d})  1->0 {e1:3d}/{len(s1):3d} {100*e1/max(1,len(s1)):5.1f}%   P->1/0 {ep:3d}/{len(sp):3d} {100*ep/max(1,len(sp)):5.1f}%')
# BTIME lead: start of the boundary event against the true second, right-read 1 and P, phase ok
for th in (-99, 3):
    g = [r['bp'][0][0] - r['t'] for r in allc if r['ch'] == r['tr'] and r['bp'] and r['c_hi'] >= th
         and -60 <= r['bp'][0][0] - r['t'] <= 150]
    pre = [r['bp'][0][0] - r['t'] for r in allc if r['ch'] == r['tr'] and r['bp'] and r['c_hi'] >= th
           and -60 <= r['bp'][0][0] - r['t'] <= 150 and r['t'] < BEFORE_MS]
    if g:
        print(f'BTIME lead vs true second (right 1/P, contrast>={th}): n={len(g)} median {np.median(g):+.0f} ms'
              f' p25/p75 {np.percentile(g,25):+.0f}/{np.percentile(g,75):+.0f}; before {BEFORE_MS/1000:.0f} s n={len(pre)} median {np.median(pre) if pre else float("nan"):+.0f}')
dd = [r['d'] for r in rows if r['t'] < BEFORE_MS]
print(f'held second - true second before {BEFORE_MS/1000:.0f} s: n={len(dd)} median {np.median(dd):+.0f} ms')
