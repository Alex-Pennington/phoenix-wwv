"""Per-window trace of the BCD correlator against UTC, and a cause for every wrong symbol.
Usage: python3 -I bcd_window_trace.py harness_out.txt conv.json [--rows]

Reads the harness's WEV lines (each BCD event and the window it landed in) and WCLOSE
lines (each window's accumulators at close), plus the SYMCB line that follows each
WCLOSE. Truth is score.py's: the symbol of the UTC second holding the window centre, with
stream time corrected for its slip against the anchor (stream_utc.py).

A "boundary pulse" for UTC second s is a BTIME event that starts within -100..+250 ms
of s (bcd_pulse_score.py's alignment band, widened 50 ms early). Its own class is its
width classified with the correlator's thresholds and position gating at s.

Causes, checked in this order for each wrong symbol (got 0/1/P, truth differs):
  stale-minute  the window's events are a minute (or more) after the window: the anchor
                was stale and the second index wrapped modulo 60 s. The symbol is right
                for the second its events are in; only its timestamp is a minute early.
  sec-index     the reported second-in-minute differs from the UTC second, and position
                gating at the wrong index changed the width class (P <-> 1).
  multi-span    the window holds the boundary pulse and its class is right, but there
                were 2+ BTIME events, and the estimator took last start - first start.
  freq-avg      the window holds the boundary pulse and its class is right, but BFREQ
                was averaged in and changed the class.
  width         the window holds the boundary pulse and its own class is wrong (a
                detector width error: the correlator cannot fix it from this evidence).
  misaligned    the window does not hold the boundary pulse, and the pulse exists: the
                window edge (sync anchor) put it in the neighbouring window.
  spurious      there is no boundary pulse for this second at all; the window classified
                a mid-second BTIME event or a BFREQ event.
  other         none of the above.
Correct symbols are split into right-by-pulse (the boundary pulse is in the window and
its class is right) and right-by-luck (anything else).
"""
import sys, re, json, math, collections, datetime
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); sys.dont_write_bytecode = True
from stream_utc import StreamUTC
fn, conv = sys.argv[1], sys.argv[2]
ROWS = '--rows' in sys.argv
U0 = json.load(open(conv))['out_utc0_unix']
DUT1 = -0.1
B_LO, B_HI = -100.0, 250.0


def frame_bit(s):
    """score.py's frame_bit(): expected symbol for the UTC second starting at unix s."""
    d = datetime.datetime.fromtimestamp(s, datetime.timezone.utc)
    sec, mi, hr = d.second, d.minute, d.hour
    doy, yy = d.timetuple().tm_yday, d.year % 100
    if sec == 0: return None
    if sec % 10 == 9: return 'P'
    f = {}
    def put(st, val, nb):
        for k in range(nb): f[st + k] = (val >> k) & 1
    f[2] = 0; f[3] = 0; f[55] = 0
    put(4, yy % 10, 4); put(51, yy // 10, 4)
    put(10, mi % 10, 4); put(15, mi // 10, 3)
    put(20, hr % 10, 4); put(25, hr // 10, 2)
    put(30, doy % 10, 4); put(35, (doy // 10) % 10, 4); put(40, doy // 100, 2)
    f[50] = 1 if DUT1 >= 0 else 0
    put(56, int(round(abs(DUT1) * 10)), 3)
    return str(f.get(sec, 0))


VALID_P = {0, 9, 19, 29, 39, 49, 59}
def cls(d, sec):
    """bcd_symbol_classify_duration()."""
    if d < 100: return '.'
    if d <= 350: return '0'
    if d <= 650: return '1'
    return 'P' if sec in VALID_P else '1'


def kv(l):
    return dict(re.findall(r'(\w+)=(\S+)', l))


def boundary(t_ms):
    """UTC second whose boundary pulse this event start could be, or None."""
    u = SU.utc(t_ms)
    s = round(u)
    return s if B_LO <= (u - s) * 1000 <= B_HI else None


L = open(fn).read().splitlines()
SU = StreamUTC(L, U0)
bucket = collections.defaultdict(list)
wins = []
bpulse = collections.defaultdict(list)       # UTC second -> boundary BTIME durations
for i, l in enumerate(L):
    if l.startswith('WEV '):
        d = kv(l)
        if d['src'] == 'T':
            s = boundary(float(d['t']))
            if s is not None: bpulse[s].append(float(d['dur']))
        if d['win_sec'] == '-1': continue
        bucket[(d['win_start'], d['win_sec'])].append((d['src'], float(d['t']), float(d['dur'])))
    elif l.startswith('WCLOSE '):
        d = kv(l)
        ev = bucket.pop((d['win_start'], d['win_sec'] if 'win_sec' in d else d['sec']), [])
        sym = next(kv(x) for x in L[i + 1:i + 3] if x.startswith('SYMCB'))
        wins.append((d, ev, sym))

luck = collections.Counter(); st = collections.Counter(); causes = collections.Counter(); rows = []; offs = collections.Counter()
for d, ev, sym in wins:
    ws = float(d['win_start']); sec = int(d['sec'])
    us = SU.second(ws + 500)  # score.py's second, slip-corrected
    truth, got = frame_bit(us), sym['sym']
    off = (SU.utc(ws) - us) * 1000
    offs[int(math.floor(off / 50) * 50)] += 1
    if got not in '01P':
        st['none'] += 1; continue
    st['decoded'] += 1
    # real second of the evidence: undo a stale-anchor wrap (events >= 30 s after window)
    shift = 0
    if ev:
        shift = round(((ev[0][1] - ws) / 1000) / 60) * 60
    real = us + shift
    rtruth = frame_bit(real)
    T = [(t, du, (SU.utc(t) - real) * 1000) for s_, t, du in ev if s_ == 'T']
    F = [(t, du, (SU.utc(t) - real) * 1000) for s_, t, du in ev if s_ == 'F']
    bp = [e for e in T if B_LO <= e[2] <= B_HI]
    bp_ok = bool(bp) and cls(bp[0][1], real % 60) == rtruth
    if got == truth:
        st['correct'] += 1
        if bp_ok and shift == 0: st['right-by-pulse'] += 1
        else:
            st['right-by-luck'] += 1
            luck['bp-wrong-class' if bp else 'bp-in-neighbour' if real in bpulse else 'no-bp'] += 1
            luck['truth=' + str(truth)] += 1
        continue
    st['wrong'] += 1
    secdiff = (sec - real % 60 + 30) % 60 - 30
    dur = float(sym['dur'])
    if shift and got == rtruth:
        c = 'stale-minute'
    elif secdiff and cls(dur, real % 60) == rtruth and cls(dur, sec) != rtruth:
        c = 'sec-index'
    elif bp and bp_ok:
        c = 'multi-span' if len(T) >= 2 else 'freq-avg' if F else 'other'
    elif bp:
        c = 'width'
    elif real in bpulse:
        c = 'misaligned'
    elif not bp:
        c = 'spurious'
    else:
        c = 'other'
    causes[c] += 1
    rows.append((datetime.datetime.fromtimestamp(real, datetime.timezone.utc).strftime('%H:%M:%S'), rtruth, got,
                 round(dur), sec, secdiff, round(off), c,
                 [(round(o), round(du)) for t, du, o in T], [(round(o), round(du)) for t, du, o in F]))

# what a correlator that took exactly the boundary pulse would score, over the same seconds
cov = set(SU.second(float(d['win_start']) + 500) for d, ev, sym in wins)
print(f"WINDOWS: closed={len(wins)} decoded={st['decoded']} correct={st['correct']} wrong={st['wrong']} "
      f"none={st['none']}; correct right-by-pulse={st['right-by-pulse']} right-by-luck={st['right-by-luck']}")
print('  wrong by cause: ' + ', '.join(f'{k}={v}' for k, v in causes.most_common()))
print('  right-by-luck: ' + ', '.join(f'{k}={v}' for k, v in sorted(luck.items())))
ach = sum(1 for s in cov if s in bpulse and cls(bpulse[s][0], s % 60) == frame_bit(s))
print(f'  seconds covered by a window={len(cov)}; with a boundary pulse={sum(1 for s in cov if s in bpulse)}; '
      f'boundary pulse right (achievable from first boundary pulse)={ach}')
print('  window start - UTC second (ms, 50 ms bins): ' + ', '.join(f'{k:+d}:{v}' for k, v in sorted(offs.items())))
if ROWS:
    print('  utc truth got dur sec secdiff win_off cause  BTIME[(start_off,dur)]  BFREQ[...]')
    for r in rows: print('  ', *r)
