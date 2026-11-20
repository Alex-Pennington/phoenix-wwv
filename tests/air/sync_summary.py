"""Sync acquisition summary of a harness or replay_events output.
Usage: python3 -I sync_summary.py out.txt [label]

SYNC lines carry no time; each takes the stream time of the last timed line before
it (TICK t, TMARK/MARK lead or t, MARKW t, BTIME at, TDRES at). Prints the stream
time of the first LOCKED, the number of entries into LOCKED (from TENTATIVE or
RECOVERING), the number of losses (LOCKED->RECOVERING) and the decoder frames."""
import sys, re
fn = sys.argv[1]; label = sys.argv[2] if len(sys.argv) > 2 else fn
now = 0.0; first_locked = None; locks = 0; losses = 0; frames = 0; tent = None
for l in open(fn, errors='replace'):
    m = None
    if l.startswith(('TICK ', 'MARK ', 'MARKW ')): m = re.search(r' t=([-\d.]+)', l)
    elif l.startswith('TMARK '): m = re.search(r' trail=([-\d.]+)', l)
    elif l.startswith(('BTIME ', 'BFREQ ', 'TDRES ')): m = re.search(r' at=([-\d.]+)', l)
    if m:
        now = max(now, float(m.group(1)))
        continue
    if l.startswith('SYNC '):
        if '->TENTATIVE' in l and tent is None: tent = now
        if '->LOCKED' in l:
            locks += 1
            if first_locked is None: first_locked = now
        if 'LOCKED->RECOVERING' in l: losses += 1
    elif l.startswith('TELEM BCDS FRAME,'):
        frames += 1
fl = f'{first_locked / 1000:.0f} s' if first_locked is not None else 'never'
ft = f'{tent / 1000:.0f} s' if tent is not None else 'never'
print(f'SYNC {label}: first TENTATIVE {ft}, first LOCKED {fl}, locks {locks}, losses {losses}, frames {frames}')
