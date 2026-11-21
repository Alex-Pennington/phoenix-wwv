"""Synthetic WWV for wwv_live (tests/live): s16 I/Q at 50 kHz on stdout, carrier at DC.

    python3 -I wwv_synth.py SECONDS [--gap T:LEN[:fill|drop] ...] [--seed N] > iq.s16
    wwv_live -r 50000 -p 0 --no-tones < iq.s16

The model is tests/test_chain_synth.c's: a carrier plus 0.5 x audio, where the audio
is a 5 ms 1000 Hz tick each second except :29 and :59, an 800 ms 1000 Hz marker at
:00, and the 100 Hz BCD subcarrier high for 200 / 500 / 800 ms (0 / 1 / P, P at
:x9) from +30 ms of each second, low otherwise; plus Gaussian noise. Sample 0 is
0.5 s before a minute.

--gap T:LEN:fill replaces LEN s of the stream from true time T with zeros, as
rsp2_stream.py fills samples lost upstream: every later sample keeps its true index.
--gap T:LEN:drop removes them, as an unfilled loss did: everything later is LEN s
early on the sample clock. The true time of every sample is never changed.
--ramp T1:T2:R loses R s per s from T1 to T2, a little every 32.8 ms block, and fills
as rsp2_stream.py's fast stage does: when the loss not yet filled reaches 30 ms,
zeros for all but 15 ms of it go in at that point. (The slow stage is left out: the
residual stays up to 30 ms, the worst case.)
"""
import sys, argparse
import numpy as np

FS = 50000
T0 = 59.5          # true time of sample 0, s past a minute

ap = argparse.ArgumentParser()
ap.add_argument('seconds', type=float)
ap.add_argument('--gap', action='append', default=[])
ap.add_argument('--ramp', default=None)
ap.add_argument('--seed', type=int, default=12345)
ap.add_argument('--noise', type=float, default=0.05)
a = ap.parse_args()

rng = np.random.default_rng(a.seed)
nsec = int(np.ceil(a.seconds)) + 2
truth = rng.integers(0, 2, nsec + 61)          # 0/1 per second index
gaps = []
for g in a.gap:
    t, ln, *mode = g.split(':')
    gaps.append((int(round(float(t) * FS)), int(round(float(ln) * FS)), mode[0] if mode else 'fill'))

out = sys.stdout.buffer
total = int(round(a.seconds * FS))

# The output as runs: ('t', first true index, count) or ('z', count)
cuts = []                                    # (true index, lost samples, zeros before the next)
for gs, gl, mode in gaps:
    cuts.append((gs, gl, gl if mode == 'fill' else 0))
if a.ramp:
    t1, t2, r = (float(v) for v in a.ramp.split(':'))
    B = int(0.0328 * FS); acc = 0.0; owed = 0
    for b0 in range(int(t1 * FS), int(t2 * FS), B):
        acc += r * B
        lost = int(acc); acc -= lost; owed += lost
        z = 0
        if owed >= 0.030 * FS:
            z = owed - int(0.015 * FS); owed -= z
        cuts.append((b0, lost, z))
cuts.sort()
runs = []; k = 0
for at, lost, z in cuts:
    if at > k:
        runs.append(('t', k, at - k)); k = at
    if z:
        runs.append(('z', z))
    k += lost
if k < total:
    runs.append(('t', k, total - k))

def synth(n):
    t = T0 + n / FS
    s = np.floor(t).astype(np.int64)
    frac = t - s
    sec = s % 60
    tone = np.sin(2 * np.pi * 1000 * t)
    aud = np.where(sec == 0, np.where(frac < 0.800, tone, 0.0),
                   np.where((sec != 29) & (sec != 59) & (frac < 0.005), tone, 0.0))
    sym = np.where(sec == 0, -1, np.where(sec % 10 == 9, 2, truth[s % len(truth)]))
    hi = np.choose(sym + 1, [0.0, 200.0, 500.0, 800.0])
    level = np.where((sym >= 0) & (frac >= 0.030) & (frac * 1000 < hi), 1.0, 0.15)
    aud = aud + 0.5 * level * np.sin(2 * np.pi * 100 * t)
    i = 1.0 + 0.5 * aud + rng.normal(0, a.noise, len(n))
    q = rng.normal(0, a.noise, len(n))
    iq = np.empty(2 * len(n), np.int16)
    iq[0::2] = np.clip(np.round(i * 8000), -32767, 32767)
    iq[1::2] = np.clip(np.round(q * 8000), -32767, 32767)
    iq[(iq == 0)] = 1                         # the source never gives an exact zero
    return iq

try:
    for run in runs:
        if run[0] == 'z':
            out.write(np.zeros(2 * run[1], np.int16).tobytes())
            continue
        _, k0, cnt = run
        for c0 in range(k0, k0 + cnt, FS):
            out.write(synth(np.arange(c0, min(c0 + FS, k0 + cnt), dtype=np.int64)).tobytes())
except BrokenPipeError:
    pass
