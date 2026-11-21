"""1->0, P->1 and P->0 of the BCD correlator's windows, by cause and window offset.
Usage: python3 -I bcd_window_trace.py run_out.txt timebase.json --rows > rows.txt
       python3 -I confusion_causes.py rows.txt

rows.txt is bcd_window_trace.py's --rows output (one line per wrong window: UTC, truth, got,
width, second, second difference, window offset, cause, then its BTIME and BFREQ events). For
each confusion it counts the windows by cause and by how far the window start is from its
nominal place, 100 ms before the UTC second (phase<=40, <=140, >140 ms). For the 'width'
cause it gives the boundary event's start offset and width, and for 'spurious' how many
windows held no BTIME event. Last, for sent 1 and P, how many decoded windows held a boundary
event (one starting -100..+250 ms of the second) at all.

These are window-level symbols, before the decoder's boundary test, so they outnumber
symbol_rates.py's. bcd_window_trace.py takes truth from out_utc0_unix + stream time, so on a
run with lost samples every window after the first loss is scored against the wrong second:
use it there only before the loss, or on a log gap-filled by shift_loss.py.
"""
import sys, collections, re
if len(sys.argv) < 2:
    sys.exit(__doc__)
rows = []
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) < 8 or not re.match(r'\d\d:\d\d:\d\d', p[0]): continue
    utc, truth, got, dur, sec, sd, off, cause = p[:8]
    ev = re.findall(r'\((-?\d+), (\d+)\)', l.split(cause, 1)[1].split('] [')[0])
    rows.append((utc, truth, got, float(dur), int(off), cause, [(int(a), int(b)) for a, b in ev]))
def ph(o):  # window start nominal is second - 100 ms
    d = o + 100
    return 'phase<=40' if abs(d) <= 40 else ('phase<=140' if abs(d) <= 140 else 'phase>140')
for t, g in (('1', '0'), ('P', '1'), ('P', '0')):
    sel = [r for r in rows if r[1] == t and r[2] == g]
    print(f'== sent {t} read {g}: {len(sel)}')
    c = collections.Counter((r[5], ph(r[4])) for r in sel)
    for k, v in sorted(c.items(), key=lambda x: -x[1]): print('  ', k, v)
    # boundary-pulse start offsets for width cause
    w = [r for r in sel if r[5] == 'width']
    bp = [e for r in w for e in r[6] if -100 <= e[0] <= 250][:]
    if bp:
        so = sorted(e[0] for e in bp); du = sorted(e[1] for e in bp)
        n = len(so)
        print(f'   width: boundary start_off median {so[n//2]} p10 {so[n//10]} p90 {so[9*n//10]}; dur median {du[n//2]}')
        late = sum(1 for e in bp if e[0] > 100)
        print(f'   width: starts later than +100 ms {late}/{n}; end (start+dur) median {sorted(e[0]+e[1] for e in bp)[n//2]}')
    s = [r for r in sel if r[5] == 'spurious']
    print(f'   spurious with no BTIME at all {sum(1 for r in s if not r[6])}/{len(s)}')
# all sent-1 windows: how many had a boundary pulse at all
for t in ('1', 'P'):
    sel = [r for r in rows if r[1] == t and r[2] in '01P' ]
    hasbp = sum(1 for r in sel if any(-100 <= e[0] <= 250 for e in r[6]))
    print(f'sent {t} decoded windows {len(sel)}: with boundary pulse {hasbp}')
