"""Stream raw CS16 I/Q from an SDRplay RSP2 (SoapySDR) to stdout, for wwv_live.

Usage: python3 -I rsp2_stream.py [--wwv-khz 5000] [--offset-khz 100] [--rate 2000000]
           [--lna N] [--ifgr N] [--gain KHZ:LNA:IFGR ...] [--antenna "Antenna A"]
           [--secs 0] [--record PREFIX]

The tuner is set to wwv - offset, so WWV sits at +offset Hz, away from DC and LO
leakage (the RSP2's DC removal stays on). AGC is off; the gain is fixed by the LNA
state (RFGR) and IF gain reduction (IFGR). Defaults are the settings of the scored
saga capture (wwv-capture/saga/RESULTS-saga.md): antenna A, 2 MS/s, tuner 4900 kHz
for WWV 5000 kHz. The gain defaults per band come from wwv_bands.GAIN (LNA 2 / IFGR 59
at 5 MHz); --gain overrides a band's entry, and --lna / --ifgr override both.

stdout carries interleaved int16 I/Q (native CS16, full scale 32767) and nothing
else; messages go to stderr. --secs 0 streams until SIGINT/SIGTERM or until the
reader closes the pipe. --record PREFIX also writes PREFIX.cs16 (the same bytes),
PREFIX.timelog.npy (rows: cumulative samples, time.time_ns() after readStream, ret,
flags) and PREFIX.json (settings read back, overflow count), as rsp_record.py did.
Overflows lose samples, which makes wwv_live's sample-counter clock run behind:
each one is reported on stderr.
"""
import os, sys, json, time, argparse, signal
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
a = ap.parse_args()
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
n = 0; overflows = 0; timeouts = 0
dev.activateStream(st)
t_settle = time.time() + 0.5       # discard ~0.5 s of settling
while time.time() < t_settle and not stop:
    dev.readStream(st, [buf], mtu, timeoutUs=1000000)
t_start_ns = time.time_ns()
log(f'streaming {"until stopped" if nwant is None else f"{a.secs:.0f} s"} at {a.rate:.0f} S/s, MTU {mtu}')
reason = 'signal'
try:
    while not stop and (nwant is None or n < nwant):
        want = mtu if nwant is None else min(mtu, nwant - n)
        sr = dev.readStream(st, [buf], want, timeoutUs=1000000)
        tn = time.time_ns()
        if sr.ret > 0:
            data = memoryview(buf[:2 * sr.ret])
            out.write(data)
            if rec:
                rec.write(data)
            n += sr.ret
        elif sr.ret == SOAPY_SDR_OVERFLOW:
            overflows += 1
            log(f'OVERFLOW #{overflows} after {n} samples ({n / a.rate:.1f} s): samples lost')
        elif sr.ret == SOAPY_SDR_TIMEOUT:
            timeouts += 1
            log(f'timeout #{timeouts}')
        if rec:
            tlog.append((n, tn, sr.ret, sr.flags))
    else:
        reason = 'signal' if stop else 'duration reached'
except BrokenPipeError:
    reason = 'reader closed the pipe'
    os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())   # no flush error at exit
finally:
    t_end_ns = time.time_ns()
    dev.deactivateStream(st)
    dev.closeStream(st)
    if rec:
        rec.close()
        info = dict(readback(), mtu=mtu, samples=n, reads=len(tlog), overflows=overflows, timeouts=timeouts,
                    t_start_ns=t_start_ns, t_end_ns=t_end_ns, stop_reason=reason)
        np.save(a.record + '.timelog.npy', np.array(tlog, dtype=np.int64))
        json.dump(info, open(a.record + '.json', 'w'), indent=1)
        log(f'recorded {a.record}.cs16 / .timelog.npy / .json')
    log(f'stopped ({reason}): {n} samples ({n / a.rate:.1f} s), {overflows} overflows, {timeouts} timeouts')
    try:
        out.flush()
    except BrokenPipeError:
        pass
