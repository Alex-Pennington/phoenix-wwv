"""Emulate the gap fill on a wwv_live log recorded before it: put the samples lost upstream
back into the event times.
Usage: python3 -I shift_loss.py log.txt console.txt segmark > shifted.txt

log.txt      the run's wwv_live --log, or tests/air/btime_replay's out_log
console.txt  the run's console
segmark      text of the console line that starts the log's segment (e.g. 'segment 2:');
             the STATUS lines after its last occurrence are read

The cumulative loss is the least-delayed console lag (the A of STATUS 'lag A..B', every 30 s)
less its first value, held non-decreasing, and linearly interpolated. It is added to every
stream time (t= at= trail= lead= start= end= lead_s= lead_e=) of the TICK, TMARK, MARK, CANDT,
CANDM, BTIME and BFREQ lines; widths are left alone. Run tests/air/replay_events on the output
and score it with out_utc0_unix as is. The lag is sampled every 30 s, so a loss is placed to
within that, and nothing is removed for the lost span itself.
Needs numpy.
"""
import sys, re
import numpy as np
if len(sys.argv) < 4:
    sys.exit(__doc__)
log, con, seg = sys.argv[1:4]
L = []; on = False
for l in open(con, errors='replace'):
    if seg in l: on = True; L = []; continue
    m = re.search(r' t=(\d+)s .*lag ([+-]\d+)\.\.', l) if on and l.startswith('STATUS') else None
    if m: L.append((float(m.group(1)) * 1000, float(m.group(2))))
LT = np.array([a for a, b in L]); LV = np.array([b for a, b in L]) - L[0][1]
LV = np.maximum.accumulate(np.maximum(LV, 0))   # loss only grows
KEYS = ('t', 'at', 'trail', 'lead', 'start', 'end', 'lead_s', 'lead_e')
pat = re.compile(r'(?<= )(' + '|'.join(KEYS) + r')=([-\d.]+)')
for l in open(log):
    if l.split(' ', 1)[0] in ('TICK', 'TMARK', 'MARK', 'CANDT', 'CANDM', 'BTIME', 'BFREQ'):
        l = pat.sub(lambda m: f'{m.group(1)}={float(m.group(2)) + float(np.interp(float(m.group(2)), LT, LV)):.1f}', l)
    sys.stdout.write(l)
