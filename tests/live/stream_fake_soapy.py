"""tests/live: run scripts/rsp2_stream.py or wwv_band_scan.py against a fake SoapySDR module
(no radio). Reads return seeded noise at the real-time rate. argv: SCRIPT MODE -- script args.
MODE: stall_after:S  data for S s, then timeouts forever
      lossy:F        data at F x real rate
      dead           timeouts from the start
      ok             data at the real rate
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
class Dev:
    def __init__(s, *a): s.rate = 2e6; s.t0 = None; s.sent = 0
    def setSampleRate(s, d, c, r): s.rate = r
    def getSampleRate(s, *a): return s.rate
    def __getattr__(s, k): return lambda *a, **kw: 0
    def getHardwareInfo(s): return {}
    def setupStream(s, *a): return 1
    def getStreamMTU(s, st): return 4096
    def readStream(s, st, bufs, n, timeoutUs=0):
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
