"""Check the BCD symbol second-in-minute index against UTC.
Usage: python3 -I symbol_second_score.py harness_out.txt conv.json
For each SYMCB line (t = window centre, sec = reported second), compare sec with
the UTC second of the window centre; also checks TELEM BCDS SYMBOL lines carry it."""
import sys, re, json, math, collections
L = open(sys.argv[1]).read().splitlines()
U0 = json.load(open(sys.argv[2]))['out_utc0_unix']
d = collections.Counter(); n = 0
for l in L:
    if not l.startswith('SYMCB'): continue
    m = re.search(r'sec=(-?\d+)', l)
    if not m: continue
    t = float(re.search(r' t=([\d.]+)', l).group(1))
    utc_sec = math.floor(U0 + t / 1000) % 60
    diff = (int(m.group(1)) - utc_sec + 30) % 60 - 30
    d[diff] += 1; n += 1
sym = [l for l in L if l.startswith('TELEM BCDS SYMBOL,')]
nf = collections.Counter(len(l.split(',')) for l in sym)
print(f'SYMBOL SECOND: callbacks with sec={n}; reported - UTC second: {dict(sorted(d.items()))}; '
      f'exact={d[0]}/{n}; TELEM SYMBOL lines={len(sym)} field counts {dict(nf)}')
