"""Score marker-detector leading edges against UTC :00.
Usage: python3 -I marker_lead_score.py harness_out.txt conv.json
Reads MARK lines (lead= field, the detector's leading-edge estimate) and prints
each event's offset from the nearest UTC minute, and the count within +-100 ms."""
import sys, re, json, datetime
L = open(sys.argv[1]).read().splitlines()
U0 = json.load(open(sys.argv[2]))['out_utc0_unix']
offs = []
for l in L:
    if not l.startswith('MARK '): continue
    m = re.search(r'lead=([-\d.]+)', l)
    if not m: continue
    u = U0 + float(m.group(1)) / 1000; mm = round(u / 60) * 60
    d = float(re.search(r'dur=([-\d.]+)', l).group(1))
    offs.append(1000 * (u - mm))
    print(f'  marker-det lead {datetime.datetime.fromtimestamp(u, datetime.timezone.utc):%H:%M:%S.%f} '
          f'= {1000*(u-mm):+.1f} ms from {datetime.datetime.fromtimestamp(mm, datetime.timezone.utc):%H:%M}:00, dur {d:.0f} ms')
ok = [o for o in offs if abs(o) <= 100]
near = [o for o in offs if abs(o) <= 500]
print(f'MARKER LEAD: events={len(offs)} within +-100 ms of :00={len(ok)} within +-500 ms={len(near)} '
      f'false(>500 ms off)={len(offs)-len(near)}' + (f' median={sorted(near)[len(near)//2]:+.1f} ms' if near else ''))
