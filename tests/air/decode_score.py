"""Score the WWV time decoder's BCDS,UTC lines against GPS-timed truth.
Usage: python3 -I decode_score.py harness_out.txt conv.json [label]

Truth: the UTC minute whose start is the frame's start (out_utc0_unix + frame_start_ms,
rounded to the minute), and for 2026-11 the status bits score.py uses: DUT1 = -0.1 s
(IERS Bulletin A Vol. XXXIX No. 046, `DUT1 = -0.1 seconds beginning 29 October 2026`),
no leap second pending (LSW 0), DST ended 2026-11-01 (bits 55 and 2 both 0).

Each part (time of day, date, DUT1, DST, LSW) is counted by state, right or wrong.
"time" is time of day and date both reported (not NONE). A CONFIRMED part with a
wrong value is a false confirm; the gate is that there are none. Time to first is
seconds from the start of the capture to the stream time the result was emitted.
Also prints the symbol confusion the decoder's error model rests on: for symbols
whose pulse starts within 250 ms of the second, a sent 0 read as 1 and a sent 1
read as 0. A symbol's true second is the decoder's own label (SYMMIN sec) in the
UTC minute nearest its minute start (out_utc0_unix + minute_start), not the UTC
second its stream time falls in: stream time can slip against the anchor (runG,
2026-11-21: 1 s in 2.6 h, NOTES.md "Stream slip"), and the old floor(U0 + t)
took the next second once the slip and the symbol's place in its second passed
1 s. The 250 ms test keeps only symbols whose pulse sits on the decoder's
second, so the label is the pulse's second; the minute rounding holds to 30 s."""
import sys, re, json, math, datetime, collections
fn, conv = sys.argv[1], sys.argv[2]
label = sys.argv[3] if len(sys.argv) > 3 else fn
U0 = json.load(open(conv))['out_utc0_unix']
TRUE_DUT1, TRUE_DST, TRUE_LSW = '-0.1', '00', '0'
L = open(fn).read().splitlines()
at = {}
for l in L:
    if l.startswith('TDRES'):
        d = dict(re.findall(r'(\w+)=(\S+)', l)); at[d['start']] = float(d['at'])
rows = [l.split('BCDS ', 1)[1].split(',') for l in L if l.startswith('TELEM BCDS UTC,')]
frames = [l.split('BCDS ', 1)[1].split(',') for l in L if l.startswith('TELEM BCDS FRAME,')]
st = collections.Counter(); first = {}; false_conf = []
parts = ('tod', 'date', 'dut1', 'dst', 'lsw', 'time')
table = []
for r in rows:
    start = float(r[1]); m = round((U0 + start / 1000) / 60) * 60
    d = datetime.datetime.fromtimestamp(m, datetime.timezone.utc)
    truth = {'tod': d.strftime('%H:%M'), 'date': d.strftime('%Y-%m-%d') + '/' + d.strftime('%j'),
             'dut1': TRUE_DUT1, 'dst': TRUE_DST, 'lsw': TRUE_LSW}
    got = {'tod': (r[2], r[3], r[4]), 'date': (r[5], r[6] + '/' + r[7], r[8]), 'dut1': (r[9], r[10], r[11]),
           'dst': (r[12], r[13], r[14]), 'lsw': (r[15], r[16], r[17])}
    t_emit = at.get(r[1], start + 61000.0) / 1000.0
    row = [d.strftime('%H:%M')]
    for p in parts[:-1]:
        state, val, mg = got[p]
        if state == 'NONE': row.append(f'{p}=- ({mg})'); continue
        ok = val == truth[p]
        st[(p, state, ok)] += 1
        row.append(f"{p}={val} {state[:4]}{'' if ok else ' WRONG'} ({mg})")
        if ok and (p, 'any') not in first: first[(p, 'any')] = t_emit
        if ok and state == 'CONFIRMED' and (p, 'conf') not in first: first[(p, 'conf')] = t_emit
        if state == 'CONFIRMED' and not ok: false_conf.append((d.strftime('%H:%M'), p, val, truth[p]))
    if got['tod'][0] != 'NONE' and got['date'][0] != 'NONE':
        ok = got['tod'][1] == truth['tod'] and got['date'][1] == truth['date']
        state = 'CONFIRMED' if got['tod'][0] == got['date'][0] == 'CONFIRMED' else 'PARTIAL'
        st[('time', state, ok)] += 1
        if ok and ('time', 'any') not in first: first[('time', 'any')] = t_emit
    table.append(row)
print(f'===== DECODE {label}: frames closed={len(rows)}')
for row in table: print('  ', ' | '.join(row))
for p in parts:
    s = ' '.join(f'{state.lower()} {st[(p, state, True)]}/{st[(p, state, True)] + st[(p, state, False)]}'
                 for state in ('DECODED', 'CONFIRMED', 'PREDICTED', 'PARTIAL')
                 if st[(p, state, True)] + st[(p, state, False)])
    wrong = sum(v for (q, s_, ok), v in st.items() if q == p and not ok)
    f1 = first.get((p, 'any')); fc = first.get((p, 'conf'))
    print(f'  {p:5s} right/reported by state: {s or "none"}; wrong={wrong}; first right at '
          f'{f1:.0f} s' if f1 else f'  {p:5s} right/reported by state: {s or "none"}; wrong={wrong}; first right: never', end='')
    print(f'; first CONFIRMED right at {fc:.0f} s' if fc else '')
print(f'  FALSE CONFIRM: {len(false_conf)} {false_conf}')
adm = sum(1 for f in frames if f[10] == '1'); hv = sum(1 for f in frames if f[11] == '1')
# symbol confusion behind the error model: pulses that start within 250 ms of the second
exec(open(__file__.replace('decode_score.py', 'symbol_truth.py')).read())
conf = collections.Counter()
for i, l in enumerate(L):
    if l.startswith('SYMCB') and i + 1 < len(L) and L[i + 1].startswith('SYMMIN'):
        sym = re.search(r'sym=(.)', l).group(1)
        off = float(re.search(r'pulse_offset=(\S+)', L[i + 1]).group(1))
        if sym not in '01' or not (off <= 250): continue
        sec = int(re.search(r'sec=(\d+)', L[i + 1]).group(1))
        ms = float(re.search(r'minute_start=([-\d.]+)', L[i + 1]).group(1))
        tr = frame_bit(round((U0 + ms / 1000) / 60) * 60 + sec)
        if tr in ('0', '1'): conf[(tr, sym)] += 1
n0 = conf[('0', '0')] + conf[('0', '1')]; n1 = conf[('1', '0')] + conf[('1', '1')]
print(f'  frames admitted={adm}/{len(frames)} decodable alone (hard_valid)={hv}; conflicts={rows[-1][20] if rows else 0}')
print(f'  symbols at the second: sent 0 read as 1 {conf[("0", "1")]}/{n0}, sent 1 read as 0 {conf[("1", "0")]}/{n1}')
