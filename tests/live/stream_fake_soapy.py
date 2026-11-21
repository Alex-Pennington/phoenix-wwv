"""tests/live: run scripts/rsp2_stream.py or wwv_band_scan.py against a fake SoapySDR module
(no radio). Reads return seeded noise at the real-time rate. argv: SCRIPT MODE -- script args.
MODE: stall_after:S  data for S s, then timeouts forever
      lossy:F        data at F x real rate
      dead           timeouts from the start
      ok             data at the real rate
      true:SPEC      data at the real rate whose samples carry their true index k
                     (I = k & 0x7fff, Q = (k >> 15 & 0x3fff) + 1, never 0), with losses
                     upstream of the API as SPEC, ';'-separated (times are true stream s):
                       loss@T:D        D s of samples lost at T, nothing reported
                       outage@T:D      no samples for D s from T (readStream times out),
                                       and those samples are lost
                       ramp@T1-T2:R    from T1 to T2, R s lost per s, a little before each block
                       jitter:J        each block arrives 0..J ms late (uniform, seeded)
                     FAKE_MTU sets the block (default 4096).
      timelog:PATH   the blocks of a recording's PATH.timelog.npy (rsp2_stream / rsp_record
                     --record: cumulative samples, arrival ns, ret, flags), each at its
                     recorded arrival time from the first, carrying its true index as true:
                     does; the rate from PATH.json. Timeouts and overflows are replayed.
"""
import sys, time, types, runpy
import numpy as np
script, mode = sys.argv[1], sys.argv[2]
args = sys.argv[sys.argv.index('--') + 1:]
kind, _, val = mode.partition(':')
S = types.ModuleType('SoapySDR')
S.SOAPY_SDR_RX = 0; S.SOAPY_SDR_CS16 = 'CS16'; S.SOAPY_SDR_OVERFLOW = -4; S.SOAPY_SDR_TIMEOUT = -1
class SR:
    def __init__(s, ret): s.ret = ret; s.flags = 0
SPEC = []
JIT = 0.0
if kind == 'true':
    for item in filter(None, val.split(';')):
        what, _, arg = item.partition('@') if '@' in item else item.partition(':')
        if what == 'jitter':
            JIT = float(arg) / 1000.0
        elif what in ('loss', 'outage'):
            t, d = arg.split(':'); SPEC.append([what, float(t), float(d)])
        elif what == 'ramp':
            span, r = arg.split(':'); t1, t2 = span.split('-'); SPEC.append(['ramp', float(t1), float(t2), float(r)])
        else:
            raise SystemExit(f'bad true: item {item}')
import os, json as _json
MTU = int(os.environ.get('FAKE_MTU', '4096'))
TL = None
if kind == 'timelog':
    TL = np.load(val + '.timelog.npy')
    TL_RATE = _json.load(open(val + '.json')).get('rate_readback', 2e6)
    TL_I = 0
    MTU = int(max(TL[:, 2].max(), 1))
class Dev:
    def __init__(s, *a): s.rate = 2e6; s.t0 = None; s.sent = 0; s.k = 0; s.last = 0.0; s.ramp_acc = 0.0
    def setSampleRate(s, d, c, r): s.rate = r
    def getSampleRate(s, *a): return s.rate
    def __getattr__(s, k): return lambda *a, **kw: 0
    def getHardwareInfo(s): return {}
    def setupStream(s, *a): return 1
    def getStreamMTU(s, st): return MTU
    def read_true(s, bufs, n, timeoutUs):
        now = time.time()
        if s.t0 is None: s.t0 = now
        for e in SPEC:
            if e[0] in ('loss', 'outage') and e[1] * s.rate <= s.k:
                s.k += int(round(e[2] * s.rate)); e[0] = 'done'
            elif e[0] == 'ramp' and e[1] * s.rate <= s.k < e[2] * s.rate:
                s.ramp_acc += e[3] * n
                lost = int(s.ramp_acc); s.ramp_acc -= lost; s.k += lost
        due = s.t0 + (s.k + n) / s.rate + RNG.uniform(0, JIT)
        due = max(due, s.last)
        if due - now > timeoutUs / 1e6:
            time.sleep(timeoutUs / 1e6)
            return SR(S.SOAPY_SDR_TIMEOUT)
        if due > now:
            time.sleep(due - now)
        s.last = due
        k = np.arange(s.k, s.k + n, dtype=np.int64)
        bufs[0][0:2 * n:2] = (k & 0x7fff).astype(np.int16)
        bufs[0][1:2 * n:2] = ((k >> 15) & 0x3fff).astype(np.int16) + 1
        s.k += n
        return SR(n)
    def read_timelog(s, bufs, n, timeoutUs):
        global TL_I
        now = time.time()
        if s.t0 is None: s.t0 = now; s.tl0 = TL[0, 1] / 1e9
        if TL_I >= len(TL):
            time.sleep(timeoutUs / 1e6)
            return SR(S.SOAPY_SDR_TIMEOUT)
        cum, tn, ret, fl = (int(v) for v in TL[TL_I])
        due = s.t0 + tn / 1e9 - s.tl0
        if due > now:
            time.sleep(due - now)
        TL_I += 1
        if ret <= 0:
            return SR(ret)
        k = np.arange(cum - ret, cum, dtype=np.int64)
        bufs[0][0:2 * ret:2] = (k & 0x7fff).astype(np.int16)
        bufs[0][1:2 * ret:2] = ((k >> 15) & 0x3fff).astype(np.int16) + 1
        return SR(ret)
    def readStream(s, st, bufs, n, timeoutUs=0):
        if kind == 'timelog':
            return s.read_timelog(bufs, n, timeoutUs)
        if kind == 'true':
            return s.read_true(bufs, n, timeoutUs)
        now = time.time()
        if s.t0 is None: s.t0 = now
        el = now - s.t0
        if kind == 'dead' or (kind == 'stall_after' and el > float(val)):
            time.sleep(timeoutUs / 1e6 / 5)   # a timeout, 5x faster than the API's
            return SR(S.SOAPY_SDR_TIMEOUT)
        f = float(val) if kind == 'lossy' else 1.0
        due = el * s.rate * f
        if s.sent + n > due:
            time.sleep((s.sent + n - due) / (s.rate * f))
        bufs[0][:2 * n] = RNG.integers(-300, 300, 2 * n, dtype=np.int16)   # noise, seeded
        s.sent += n
        return SR(n)
RNG = np.random.default_rng(1)
S.Device = Dev
S.registerLogHandler = lambda f: None
sys.modules['SoapySDR'] = S
sys.argv = [script] + args
t = time.time()
code = 0
try:
    runpy.run_path(script, run_name='__main__')
except SystemExit as e:
    code = e.code or 0
sys.stderr.write(f'[harness] exit {code} after {time.time() - t:.1f} s\n')
