"""Stream raw CS16 I/Q from an SDRplay RSP2 (SoapySDR) to stdout, for wwv_live.

Usage: python3 -I rsp2_stream.py [--wwv-khz 5000] [--offset-khz 100] [--rate 2000000]
           [--lna N] [--ifgr N] [--gain KHZ:LNA:IFGR ...] [--antenna "Antenna A"]
           [--secs 0] [--record PREFIX] [--stall-secs 10] [--heartbeat FILE]
           [--report-secs 60] [--hold-secs 3] [--gap-max-secs 10] [--no-fill]
           [--gap-log FILE] [--anchor-out FILE]

The tuner is set to wwv - offset, so WWV sits at +offset Hz, away from DC and LO
leakage (the RSP2's DC removal stays on). AGC is off; the gain is fixed by the LNA
state (RFGR) and IF gain reduction (IFGR). Defaults are the settings of the scored
saga capture (wwv-capture/saga/RESULTS-saga.md): antenna A, 2 MS/s, tuner 4900 kHz
for WWV 5000 kHz. The gain defaults per band come from wwv_bands.GAIN (LNA 2 / IFGR 59
at 5 MHz); --gain overrides a band's entry, and --lna / --ifgr override both.

stdout carries interleaved int16 I/Q (native CS16, full scale 32767) and nothing
else; messages go to stderr. --secs N ends the stream N seconds of wall clock after
it starts, however many samples came; --secs 0 streams until SIGINT/SIGTERM or until
the reader closes the pipe. --record PREFIX also writes PREFIX.cs16 (the same bytes),
PREFIX.timelog.npy (rows: cumulative samples, time.time_ns() after readStream, ret,
flags) and PREFIX.json (settings read back, overflow count), as rsp_record.py did.
Overflows lose samples; each one is reported on stderr.

Gap fill: the stream is gap-exact, so a sample's index is its time since stream
start. SoapySDRPlay3 0.5.3 passes none of the API's own loss signals on (its
rx_callback ignores firstSampleNum and the reset flag, readStream never sets
timeNs or SOAPY_SDR_HAS_TIME, and OVERFLOW means only that its own 8-buffer ring,
about 262 ms, filled), so a loss is read from the arrival times: a block arrives
no earlier than its last sample's time plus the least USB/API latency, so
"lag" = arrival (CLOCK_MONOTONIC) minus stream time has a floor, and samples lost
upstream raise the floor by what was lost, as a step. The floor is a line, not a
constant: on the live runs the delivery delay grew steadily, 50 to 170 ppm (runG:
1 s in 2.6 h), with no sample lost, and filling that ramp put stream time 1 s
ahead of UTC (NOTES.md, "Stream slip"). So the lag's rate is tracked: the least
lag of each 10 s bin (fills included), fitted over the last 300 s (Theil-Sen, once 120 s of bins are in; at most +-1000 ppm). The floor
line is the least of the last 60 s of bins, each carried forward at that rate.
A rate error under about 60 ppm stays under the slow stage's 4.5 ms over those
60 s, so nothing is filled and the fit sees the whole rate; a larger one (the
rate stepping) fills only until the fit catches up.
A steady rate is not a loss and is never filled; this cannot tell a ramp of real
loss under 1000 ppm from a growing delay, and does not try. Two stages, each
filling only the excess over the floor line beyond a margin, so a fill is never
too large:
  fast: the least lag over the last 2 s; over 15 ms margin + 15 ms, the excess
        less the margin is filled, at the block where the lag rose (to within a
        block, 33 ms at 2 MS/s);
  slow: the least lag over the last 30 s; over 2.5 ms margin + 2 ms, the excess
        less the margin is filled ahead of the held blocks (the remainder of a
        fast fill, or a loss under the fast stage's threshold). Until the rate
        is known it fills only within 60 s of a fast fill; at most one fill
        per 30 s, as a fill cannot move a released block's lag.
The margins are the largest window-minimum excesses measured on saga's
10-minute capture (13.4 ms at 2 s, 2.1 ms at 30 s). To place a fill where the
loss was, output is held --hold-secs (3 s) behind the radio. Fills are zeros
(I = Q = 0, which the radio never gives for 0.5 ms); wwv_live takes such a run
out as a gap. Each fill is logged on stderr (GAP: UTC, stream time, size, cause:
timeout or overflow when one came before it, else upstream with no report) and,
with --gap-log, as a JSON line. A loss over --gap-max-secs (or a stall) ends the
stream with exit status 76 (75 for a stall), and wwv_rsp2_live.sh reopens it as a
new segment. --no-fill keeps the old behaviour (no hold, no fill).

--anchor-out FILE: the UTC of sample 0 from the least-delayed block over the
first 10 s, measured on arrival (before the hold), and hold_s, as JSON; written
before the first sample past 10 s leaves, for wwv_live --anchor-in.

Stall: when no sample has come for --stall-secs (the API service died or restarted
under the stream, or the USB transfers stopped), the stream ends with exit status 75
and says so; wwv_rsp2_live.sh then reopens it. Timeouts are logged once when a gap
starts and once when it ends, not one line each.

--heartbeat FILE: its mtime is set at most once a second while samples come, so a
supervisor can see a stream that hangs inside readStream.

Every --report-secs the stream logs how far its sample count (fills included) is
behind the wall clock since streaming began ("behind"), what was filled, how
long its writes to stdout blocked, and the tracked lag rate ("lag rate", ppm; a
steady positive rate makes "behind" grow by that much and is not filled).
Behind growing with little write wait means samples were lost upstream of this
process (USB or the API service) without an overflow report; a long write wait
means the reader (wwv_live) is not keeping up.
"""
import os, sys, json, time, argparse, signal, collections, itertools
import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CS16, SOAPY_SDR_OVERFLOW, SOAPY_SDR_TIMEOUT
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True   # no __pycache__ in the tree
import wwv_bands

ap = argparse.ArgumentParser()
ap.add_argument('--wwv-khz', type=float, default=5000.0)
ap.add_argument('--offset-khz', type=float, default=100.0)
ap.add_argument('--rate', type=float, default=2e6)
ap.add_argument('--lna', type=int, default=None)
ap.add_argument('--ifgr', type=int, default=None)
ap.add_argument('--gain', action='append', default=[], metavar='KHZ:LNA:IFGR')
ap.add_argument('--antenna', default='Antenna A')
ap.add_argument('--secs', type=float, default=0.0)
ap.add_argument('--record', default=None)
ap.add_argument('--stall-secs', type=float, default=10.0)
ap.add_argument('--heartbeat', default=None)
ap.add_argument('--report-secs', type=float, default=60.0)
ap.add_argument('--hold-secs', type=float, default=3.0)
ap.add_argument('--gap-max-secs', type=float, default=10.0)
ap.add_argument('--no-fill', action='store_true')
ap.add_argument('--gap-log', default=None)
ap.add_argument('--anchor-out', default=None)
a = ap.parse_args()
if not a.no_fill and a.hold_secs < 2.5:
    ap.error('--hold-secs must be at least 2.5 (the 2 s gap window and a block), or give --no-fill')
_lna, _ifgr = wwv_bands.gain_for(a.wwv_khz, wwv_bands.parse_gain_overrides(a.gain))
if a.lna is None:
    a.lna = _lna
if a.ifgr is None:
    a.ifgr = _ifgr

stop = False
def _stop(sig, frm):
    global stop
    stop = True
signal.signal(signal.SIGINT, _stop)
signal.signal(signal.SIGTERM, _stop)

def log(msg):
    sys.stderr.write(f'[rsp2_stream] {msg}\n')
    sys.stderr.flush()

SoapySDR.registerLogHandler(lambda level, msg: log(f'soapy {level}: {msg}'))
dev = None
for _try in range(20):   # the API service takes a few seconds to come up / release the RSP
    if stop:
        sys.exit(1)
    if a.heartbeat:
        open(a.heartbeat, 'a').close(); os.utime(a.heartbeat)   # alive while it retries
    try:
        dev = SoapySDR.Device(dict(driver='sdrplay'))
        break
    except RuntimeError as e:
        log(f'open failed ({e}), retrying')
        time.sleep(1)
if dev is None:
    log('RSP not available')
    sys.exit(1)

ch = 0
dev.setSampleRate(SOAPY_SDR_RX, ch, a.rate)
dev.setAntenna(SOAPY_SDR_RX, ch, a.antenna)
dev.setGainMode(SOAPY_SDR_RX, ch, False)
dev.setGain(SOAPY_SDR_RX, ch, 'RFGR', a.lna)
dev.setGain(SOAPY_SDR_RX, ch, 'IFGR', a.ifgr)
for k, v in (('biasT_ctrl', 'false'), ('rfnotch_ctrl', 'false'), ('extref_ctrl', 'false'),
             ('iqcorr_ctrl', 'true')):
    dev.writeSetting(k, v)
fc = (a.wwv_khz - a.offset_khz) * 1e3
dev.setFrequency(SOAPY_SDR_RX, ch, fc)

def readback():
    return dict(
        wwv_hz=a.wwv_khz * 1e3, offset_hz=a.offset_khz * 1e3, tuner_hz_set=fc,
        tuner_hz_readback=dev.getFrequency(SOAPY_SDR_RX, ch),
        rate_set=a.rate, rate_readback=dev.getSampleRate(SOAPY_SDR_RX, ch),
        bandwidth_readback=dev.getBandwidth(SOAPY_SDR_RX, ch),
        antenna=dev.getAntenna(SOAPY_SDR_RX, ch), agc=dev.getGainMode(SOAPY_SDR_RX, ch),
        lna_state=dev.getGain(SOAPY_SDR_RX, ch, 'RFGR'), ifgr=dev.getGain(SOAPY_SDR_RX, ch, 'IFGR'),
        settings={k: dev.readSetting(k) for k in ('biasT_ctrl', 'rfnotch_ctrl', 'extref_ctrl', 'iqcorr_ctrl', 'rfgain_sel')},
        dc_offset_mode=dev.getDCOffsetMode(SOAPY_SDR_RX, ch),
        hw=dict(dev.getHardwareInfo()), driver=dev.getDriverKey(), hardware=dev.getHardwareKey())

rb = readback()
log('settings: ' + json.dumps({k: rb[k] for k in ('wwv_hz', 'tuner_hz_readback', 'rate_readback', 'antenna', 'agc',
                                                  'lna_state', 'ifgr', 'bandwidth_readback')}))

st = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [ch])
mtu = dev.getStreamMTU(st)
buf = np.empty(2 * mtu, np.int16)
nwant = int(round(a.secs * a.rate)) if a.secs > 0 else None
out = sys.stdout.buffer
rec = open(a.record + '.cs16', 'wb') if a.record else None
tlog = []
n = 0; overflows = 0; timeouts = 0      # n: samples from the radio
EX_STALLED = 75

def heartbeat():
    if a.heartbeat:
        try:
            os.utime(a.heartbeat)
        except FileNotFoundError:
            open(a.heartbeat, 'a').close()
heartbeat()
dev.activateStream(st)
t_settle = time.time() + 0.5       # discard ~0.5 s of settling
while time.time() < t_settle and not stop:
    dev.readStream(st, [buf], mtu, timeoutUs=1000000)
t_start_ns = time.time_ns()
t_end_wall = t_start_ns / 1e9 + a.secs if a.secs > 0 else None
log(f'streaming {"until stopped" if nwant is None else f"{a.secs:.0f} s of wall clock"} at {a.rate:.0f} S/s, MTU {mtu}')
reason = 'signal'
rc = 0
t_data = time.time()                 # last read that returned samples
gap_timeouts = 0                     # timeouts since then
t_report = t_data + a.report_secs
behind_last = 0.0
wait_s = 0.0; wait_max = 0.0         # time blocked writing stdout, this report interval
t_beat = 0.0

def behind_ms(tn_s):
    """Samples (fills included) missing against the wall clock since streaming began, as ms."""
    return ((tn_s - t_start_ns / 1e9) * a.rate - idx) / a.rate * 1000.0

# ---- gap fill ----
FAST_W, FAST_B, FAST_T = 2.0, 0.015, 0.015     # window s, margin s, least fill s
SLOW_W, SLOW_B, SLOW_T = 30.0, 0.0025, 0.002
RATE_BIN, RATE_W, RATE_MIN = 10.0, 300.0, 120.0 # lag-rate bins s, fit window s, least span s
RATE_MAX = 1000e-6                              # the largest lag rate tracked, s/s
FLOOR_W = 60.0                                  # the floor: least of the last FLOOR_W s of bins
REMAINDER_S = 60.0                              # before the rate is known, the slow stage fills only this soon after a fast fill
REFINE_S = 10.0                                 # wwv_live's anchor refinement
EX_GAP = 76
hold = 0.0 if a.no_fill else a.hold_secs
held = collections.deque()      # records: [mono_s, wall_s, data or None, n, idx_end, fill_before]
hist = collections.deque()      # the same records, last SLOW_W s (released ones too)
idx = 0                         # true samples so far (fills included)
mono0 = None                    # monotonic time of stream time 0 (any fixed origin)
bins = collections.deque()      # [bin start, t of least lag, least lag], last RATE_W s
rate = 0.0                      # tracked lag rate, s/s (0 until RATE_MIN s of bins)
rate_ready = False
t_fast = None                   # monotonic time of the last fast fill
t_slow = None                   # and of the last slow fill
fills = []                      # (wall UTC, stream s, samples, cause)
filled = 0
events = collections.deque()    # (mono_s, 'timeout' | 'overflow'), last SLOW_W s
anchor = None; anchor_said = False
gap_log = open(a.gap_log, 'a') if a.gap_log else None

def lag(r):
    return r[0] - mono0 - r[4] / a.rate

def track(t, ln):
    """One block's lag on arrival: the bins, and the lag rate when a bin closes"""
    global rate, rate_ready
    if bins and t < bins[-1][0] + RATE_BIN:
        b = bins[-1]
        if ln < b[2]:
            b[1], b[2] = t, ln
        return
    bins.append([t, t, ln])
    while bins[0][0] < t - RATE_W:
        bins.popleft()
    done = list(itertools.islice(bins, 0, len(bins) - 1))   # closed bins
    if len(done) >= 3 and done[-1][1] - done[0][1] >= RATE_MIN:
        x = np.array([b[1] for b in done]); y = np.array([b[2] for b in done])
        i, j = np.triu_indices(len(x), 1)
        dx = x[j] - x[i]
        ok = dx > RATE_BIN / 2
        s = float(np.median((y[j] - y[i])[ok] / dx[ok]))   # Theil-Sen: robust to a step and a stray bin
        rate = max(-RATE_MAX, min(RATE_MAX, s))
        rate_ready = True

def floor_c(t):
    """The floor line's intercept at t: the least lag over the bins of the last FLOOR_W s, each
    carried at the lag rate. The floor at any time u near t is floor_c(t) + rate * u."""
    return min(b[2] - rate * b[1] for b in bins if b[0] >= t - FLOOR_W - RATE_BIN or b is bins[-1])

def floor_at(t):
    return floor_c(t) + rate * t

def cause_between(t0, t1):
    k = [e for t, e in events if t0 - 0.05 <= t <= t1 + 0.05]
    if 'overflow' in k:
        return 'overflow (SOAPY_SDR_OVERFLOW)'
    if 'timeout' in k:
        return f'timeout ({k.count("timeout")} readStream timeouts)'
    return 'upstream, no report (USB or API service)'

def fill(at, nfill, cause, slow=False):
    """nfill zeros before held[at]: every later held record moves on"""
    global idx, filled, t_fast, t_slow
    if slow:
        t_slow = held[-1][0]
    else:
        t_fast = held[-1][0]
    r = held[at]
    r[5] += nfill
    for q in itertools.islice(held, at, None):
        q[4] += nfill
    idx += nfill; filled += nfill
    start = r[4] - r[3] - nfill        # stream index of the first zero
    utc = (anchor if anchor is not None else t_start_ns / 1e9) + start / a.rate
    when = time.strftime('%Y-%m-%dT%H:%M:%S', time.gmtime(utc)) + f'.{int(utc * 1000) % 1000:03d}Z'
    fills.append((utc, start / a.rate, nfill, cause))
    log(f'GAP {when} at stream {start / a.rate:.3f} s (sample {start}): {nfill / a.rate * 1000:.1f} ms '
        f'filled with zeros; cause: {cause}')
    if gap_log:
        gap_log.write(json.dumps(dict(utc=round(utc, 6), stream_s=round(start / a.rate, 6), sample=start,
                                      samples=nfill, ms=round(nfill / a.rate * 1000, 3), cause=cause)) + '\n')
        gap_log.flush()

def check_gaps(now_m):
    """The two stages, on the newest block's arrival. Both measure the lag against the floor
    line (floor_at): a steady lag rate (the delivery delay growing, or the radio's clock off the
    host's) is tracked there, not filled."""
    if len(hist) < 2 or not held:
        return None
    t_new = hist[-1][0]
    c = floor_c(t_new)
    floor = c + rate * t_new
    # fast: the window must be all within the hold, so the fill can still be placed
    if t_new - hist[0][0] >= FAST_W:
        wmin = min(lag(q) for q in hist if q[0] >= t_new - FAST_W)
        ex = wmin - floor
        if ex - FAST_B >= FAST_T:
            d = ex - FAST_B
            k = len(held) - 1
            while k > 0 and lag(held[k - 1]) >= c + rate * held[k - 1][0] + FAST_B + d / 2:
                k -= 1
            prev = held[k - 1][0] if k > 0 else hist[0][0]
            cause = cause_between(prev, held[k][0])
            if d > a.gap_max_secs:
                while len(held) > k:           # after the loss: not this segment's
                    held.pop()
                return f'lost {d:.1f} s, more than --gap-max-secs {a.gap_max_secs:g}; cause: {cause}'
            fill(k, int(round(d * a.rate)), cause)
            return None
    # slow: a step over the floor line the fast stage left (its margin, or a loss under its
    # threshold). Until the rate is known, only a fast fill's remainder: a ramp is not a loss.
    # One fill per window: the window's least lag may be a released block, which a fill does
    # not move, so a second look inside the same window would fill the same excess again.
    if (t_new - hist[0][0] >= SLOW_W - 0.5 and (t_slow is None or t_new - t_slow >= SLOW_W)
            and (rate_ready or (t_fast is not None and t_new - t_fast <= REMAINDER_S))):
        ex = min(lag(q) - rate * q[0] for q in hist) - c
        if ex - SLOW_B >= SLOW_T:
            d = ex - SLOW_B
            fill(0, int(round(d * a.rate)),
                 'remainder over 30 s (a fast fill\'s margin, or a loss under the fast stage\'s threshold)', slow=True)
    return None

def release(r):
    """Write one held block (and the zeros before it)"""
    global wait_s, wait_max, anchor, anchor_said
    nz = r[5]
    if a.anchor_out and not anchor_said and r[4] >= REFINE_S * a.rate:
        with open(a.anchor_out + '.tmp', 'w') as f:
            json.dump(dict(source='rsp2_stream', in_sample0_unix=anchor, hold_s=hold,
                           anchor=f'least-delayed block over the first {REFINE_S:.0f} s, on arrival'), f)
        os.replace(a.anchor_out + '.tmp', a.anchor_out)
        anchor_said = True
    tw = time.monotonic()
    while nz > 0:
        m = min(nz, len(zbuf) // 2)
        out.write(memoryview(zbuf[:2 * m]))
        if rec:
            rec.write(memoryview(zbuf[:2 * m]))
        nz -= m
    out.write(r[2])
    if rec:
        rec.write(r[2])
    dw = time.monotonic() - tw
    wait_s += dw; wait_max = max(wait_max, dw)
    r[2] = None

zbuf = np.zeros(2 * 65536, np.int16)
gap_end = None

try:
    while not stop and (nwant is None or idx < nwant):
        want = mtu if nwant is None else min(mtu, nwant - idx)
        sr = dev.readStream(st, [buf], want, timeoutUs=1000000)
        tn = time.time_ns()
        tm = time.monotonic()
        now = tn / 1e9
        if sr.ret > 0:
            n += sr.ret
            idx += sr.ret
            r = [tm, now, bytes(memoryview(buf[:2 * sr.ret])), sr.ret, idx, 0]
            if mono0 is None:
                mono0 = tm - idx / a.rate
            ln = lag(r)
            fl = floor_at(tm) if bins else ln    # the floor line before this block
            track(tm, ln)
            a0 = now - idx / a.rate
            if idx <= REFINE_S * a.rate or anchor is None:
                anchor = a0 if anchor is None else min(anchor, a0)
            if hold > 0:
                held.append(r); hist.append(r)
                while hist and hist[0][0] < tm - SLOW_W:
                    hist.popleft()
                while events and events[0][0] < tm - SLOW_W:
                    events.popleft()
                if ln - fl > a.gap_max_secs + 0.3:        # past any queueing: certain
                    held.pop()                 # after the loss: not this segment's
                    gap_end = (f'lost about {ln - fl:.1f} s, more than --gap-max-secs {a.gap_max_secs:g}; '
                               f'cause: {cause_between(held[-1][0] if held else tm - ln + fl, tm)}')
                else:
                    gap_end = check_gaps(tm)
                if gap_end:
                    reason = f'gap: {gap_end}'
                    rc = EX_GAP
                    log(f'GAP too large to fill: {gap_end}; ending the stream (exit {EX_GAP})')
                    break
                while held and tm - held[0][0] >= hold:
                    release(held.popleft())
            else:
                release(r)
            if gap_timeouts:
                log(f'samples again after {now - t_data:.1f} s ({gap_timeouts} timeouts)')
                gap_timeouts = 0
            t_data = now
            if now - t_beat >= 1.0:
                heartbeat(); t_beat = now
        elif sr.ret == SOAPY_SDR_OVERFLOW:
            overflows += 1
            events.append((tm, 'overflow'))
            log(f'OVERFLOW #{overflows} after {idx} samples ({idx / a.rate:.1f} s): samples lost')
        elif sr.ret == SOAPY_SDR_TIMEOUT:
            timeouts += 1
            gap_timeouts += 1
            events.append((tm, 'timeout'))
            if gap_timeouts == 1:
                log(f'timeout: no samples for {now - t_data:.1f} s (timeout #{timeouts} in all)')
        if rec:
            tlog.append((n, tn, sr.ret, sr.flags))
        if now >= t_report:
            b = behind_ms(now)
            grew = b - behind_last
            what = ''
            if grew > 100.0:
                what = ('; samples lost upstream (USB or API service), no overflow reported'
                        if wait_s < 0.5 * grew / 1000.0 else '; the reader is not keeping up')
            log(f'behind wall clock {b:+.0f} ms ({grew:+.0f} in {a.report_secs:.0f} s), '
                f'filled {filled / a.rate * 1000:.0f} ms in {len(fills)} gaps, '
                f'write wait {wait_s * 1000:.0f} ms (max {wait_max * 1000:.0f}), '
                f'{overflows} overflows, {timeouts} timeouts, '
                f'lag rate {rate * 1e6:+.0f} ppm{"" if rate_ready else " (not yet known)"}{what}')
            behind_last = b; wait_s = 0.0; wait_max = 0.0
            t_report += a.report_secs
        if t_end_wall is not None and now >= t_end_wall:
            reason = 'duration reached (wall clock)'
            break
        if a.stall_secs > 0 and now - t_data >= a.stall_secs:
            reason = f'stalled: no samples for {now - t_data:.1f} s ({gap_timeouts} timeouts)'
            rc = EX_STALLED
            break
    else:
        reason = 'signal' if stop else 'duration reached'
    while held:                                   # what is held is good: send it
        release(held.popleft())
except BrokenPipeError:
    reason = 'reader closed the pipe'
    os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())   # no flush error at exit
finally:
    t_end_ns = time.time_ns()
    log(f'behind wall clock at the end: {behind_ms(t_end_ns / 1e9):+.0f} ms')
    dev.deactivateStream(st)
    dev.closeStream(st)
    if gap_log:
        gap_log.close()
    if rec:
        rec.close()
        info = dict(readback(), mtu=mtu, samples=idx, samples_radio=n, reads=len(tlog), overflows=overflows,
                    timeouts=timeouts, t_start_ns=t_start_ns, t_end_ns=t_end_ns, stop_reason=reason,
                    hold_s=hold, anchor_sample0_unix=anchor,
                    gaps=[dict(utc=u, stream_s=t, samples=k, cause=c) for u, t, k, c in fills])
        np.save(a.record + '.timelog.npy', np.array(tlog, dtype=np.int64))
        json.dump(info, open(a.record + '.json', 'w'), indent=1)
        log(f'recorded {a.record}.cs16 / .timelog.npy / .json')
    log(f'stopped ({reason}): {idx} samples ({idx / a.rate:.1f} s), {n} from the radio, '
        f'{len(fills)} gaps filled ({filled / a.rate * 1000:.1f} ms), {overflows} overflows, {timeouts} timeouts, '
        f'lag rate {rate * 1e6:+.0f} ppm')
    try:
        out.flush()
    except BrokenPipeError:
        pass
sys.exit(rc)
