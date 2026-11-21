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
"""
import sys, argparse
import numpy as np

FS = 50000
T0 = 59.5          # true time of sample 0, s past a minute

ap = argparse.ArgumentParser()
ap.add_argument('seconds', type=float)
ap.add_argument('--gap', action='append', default=[])
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
CH = FS                                      # one second per chunk
total = int(round(a.seconds * FS))
for n0 in range(0, total, CH):
    n = np.arange(n0, min(n0 + CH, total), dtype=np.int64)
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
    keep = np.ones(len(n), bool)
    for gs, gl, mode in gaps:
        m = (n >= gs) & (n < gs + gl)
        if mode == 'fill':
            iq[0::2][m] = 0
            iq[1::2][m] = 0
        else:
            keep &= ~m
    iq = iq.reshape(-1, 2)[keep].reshape(-1)
    try:
        out.write(iq.tobytes())
    except BrokenPipeError:
        break
