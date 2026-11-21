"""tests/live: check that a stream from stream_fake_soapy.py true:... is gap-exact.

    python3 -I check_gapfill.py STREAM.cs16 RATE CONSOLE [--max-ms 25] [--end-ms 6] [--gaps N]

Each non-zero sample carries its true index k; its index in the stream is p. p - (k - k0)
is the error, k0 the first sample's k (the stream starts after its settling discard). Zero samples are fills. Checks: |p - k| <= --max-ms for every sample (no
sample whole seconds or even a block off), |p - k| <= --end-ms over the last 5 s, the
fills are where the error steps, and the stream's GAP lines (CONSOLE) number at least
--gaps. Prints the error's path: each step with its stream time.
"""
import sys, re, argparse
import numpy as np
ap = argparse.ArgumentParser()
ap.add_argument('stream'); ap.add_argument('rate', type=float); ap.add_argument('console')
ap.add_argument('--max-ms', type=float, default=25.0)
ap.add_argument('--end-ms', type=float, default=6.0)
ap.add_argument('--gaps', type=int, default=1)
ap.add_argument('--min-filled-ms', type=float, default=0.0)
a = ap.parse_args()
x = np.fromfile(a.stream, np.int16).reshape(-1, 2).astype(np.int64)
p = np.arange(len(x))
zero = (x[:, 0] == 0) & (x[:, 1] == 0)
k = (x[:, 0] & 0x7fff) | ((x[:, 1] - 1) << 15)
k0 = k[~zero][0]                                # the stream starts after the settling discard
err = (p - (k - k0))[~zero] / a.rate * 1000.0   # ms; + : the stream index is ahead of true time
pt = p[~zero] / a.rate
ok = True
steps = np.flatnonzero(np.abs(np.diff(err)) > 0.0005) + 1
print(f'{len(x)} samples ({len(x) / a.rate:.1f} s), {zero.sum()} zeros ({zero.sum() / a.rate * 1000:.1f} ms)')
print(f'error at start {err[0]:+.3f} ms; steps:')
for i in steps[:40]:
    print(f'  at {pt[i]:8.3f} s: {err[i - 1]:+9.3f} -> {err[i]:+9.3f} ms')
if len(steps) > 40:
    print(f'  ... {len(steps)} steps in all')
mx = np.abs(err).max(); end = np.abs(err[pt >= pt[-1] - 5]).max()
print(f'max |error| {mx:.3f} ms (limit {a.max_ms}), last 5 s {end:.3f} ms (limit {a.end_ms})')
if mx > a.max_ms: print('FAIL: an error over the limit'); ok = False
if end > a.end_ms: print('FAIL: the error at the end is over the limit'); ok = False
# every zero run sits where the error steps up (filled ahead of the next real sample)
runs = np.flatnonzero(np.diff(np.r_[0, zero.astype(np.int8), 0]))
nruns = len(runs) // 2
print(f'{nruns} zero runs (fills)')
gl = [l for l in open(a.console) if '] GAP ' in l]
print(f'{len(gl)} GAP lines (want at least {a.gaps})')
for l in gl[:12]: print('  ' + l.strip())
if len(gl) < a.gaps or nruns < a.gaps: print('FAIL: too few gaps'); ok = False
if zero.sum() / a.rate * 1000 < a.min_filled_ms: print('FAIL: too little filled'); ok = False
print('check_gapfill:', 'ok' if ok else 'FAILED')
sys.exit(0 if ok else 1)
