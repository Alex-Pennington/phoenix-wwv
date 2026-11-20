"""Measure each WWV band on the RSP2 and rank them. Run it through wwv_band_scan.sh, which starts
and stops sdrplay.service.

Usage: python3 -I wwv_band_scan.py [--bands 2500,5000,10000,15000,20000,25000] [--secs 25]
           [--gain KHZ:LNA:IFGR ...] [--ppm 1.09] [--offset-khz 100] [--rate 2000000]
           [--antenna "Antenna A"] [--json FILE] [--replay PREFIX:KHZ ...]

The device is opened once. For each band it is retuned (WWV at +offset, as rsp2_stream.py),
set to the band's gain (wwv_bands.GAIN, or --gain), and read for --secs into memory; the
band is measured before the next one is read. The table goes to stdout, the last line is
`BEST <kHz>` (or `BEST none`), and --json writes every figure. --replay measures recordings
instead (PREFIX.cs16 and PREFIX.timelog.npy from rsp2_stream.py --record or rsp_record.py,
PREFIX.json for the gain) and opens no device.

Measurements, after wwv-capture check_signal.py and saga/scripts/tick_contrast.py:
- Carrier: the RF is mixed by offset + ppm*f, decimated to 25 kHz, and the strongest bin
  within 50 Hz of DC is the carrier (131072-point Hann spectra, averaged). The noise floor is
  the median bin 2000-4500 Hz from the carrier. C/N0 = carrier/noise per bin / bin width, the
  same convention as check_signal.py. The carrier's offset gives the TCXO error it implies.
- Sidebands: 1000 Hz (WWV) and 1200 Hz (WWVH) levels over the floor, the larger of +/-.
- Ticks: the 900-1100 Hz energy of the AM envelope, folded by the UTC second in 1 ms bins
  (seconds :00 left out). The fold peak in the first 100 ms is the tick; "fold" is its level
  over the 300-900 ms median. Per-second contrast is the energy in [peak-5, peak+5) ms over
  300-900 ms of the same second, :00/:29/:59 left out; the table gives its median and the
  share of seconds over 10 dB. 1100-1300 Hz is folded the same way for the WWVH tick.
  Deviation: the envelope |IQ| is used, not check_signal's coherent real part, so a residual
  carrier offset cannot fade the demodulated audio within the measurement.
- Timing: the system time of sample 0 is the least-delayed read (arrival - n/fs), the method
  of the scored saga capture. Host latency makes the fold peak late by a few ms; the tick
  window follows the peak, so the contrast does not depend on it.
- ADC: raw peak in dBFS and the share of I or Q samples at or above 32700. API overload
  events are counted when the overload-logging SoapySDRPlay3 build is loaded (it logs
  WWVCAP_OVERLOAD_*; wwv_band_scan.sh loads it when present); the stock module drops them.

Ranking: a band with ADC clipping or an API overload goes last and is never the best. The
rest are ranked by the median per-second tick contrast, less 6 dB where WWVH may dominate:
a WWVH band (2.5-15 MHz) whose 1200 Hz tick fold is at least 6 dB (a visible tick) and
within 3 dB of the 1000 Hz one. A median of 6 dB or more is "usable"; the 5 MHz capture
the chain was scored on had 17.1 dB.
"""
import os, sys, json, time, argparse, signal
import numpy as np
from scipy.signal import butter, sosfilt, sosfiltfilt
import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CS16, SOAPY_SDR_OVERFLOW, SOAPY_SDR_TIMEOUT
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True   # no __pycache__ in the tree
import wwv_bands

ap = argparse.ArgumentParser()
ap.add_argument('--bands', default=','.join(str(k) for k in wwv_bands.WWV_KHZ))
ap.add_argument('--secs', type=float, default=25.0)
ap.add_argument('--gain', action='append', default=[], metavar='KHZ:LNA:IFGR')
ap.add_argument('--ppm', type=float, default=wwv_bands.TCXO_PPM)
ap.add_argument('--offset-khz', type=float, default=100.0)
ap.add_argument('--rate', type=float, default=2e6)
ap.add_argument('--antenna', default='Antenna A')
ap.add_argument('--json', default=None)
ap.add_argument('--replay', action='append', default=[], metavar='PREFIX:KHZ')
a = ap.parse_args()
bands = [int(float(x)) for x in a.bands.split(',') if x.strip()]
gains = wwv_bands.parse_gain_overrides(a.gain)
DEC1 = int(round(a.rate / 50000))          # to 50 kHz
FS = 25000.0                               # analysis rate
if DEC1 * 50000 != a.rate:
    raise SystemExit('--rate must be a whole multiple of 50000')
USABLE_DB = 6.0
WWVH_MARGIN_DB = 3.0
WWVH_VISIBLE_DB = 6.0   # a 1200 Hz tick fold below this is noise, not WWVH
WWVH_PENALTY_DB = 6.0
CLIP = 32700

stop = False
def _stop(sig, frm):
    global stop
    stop = True
signal.signal(signal.SIGINT, _stop)
signal.signal(signal.SIGTERM, _stop)

def log(msg):
    sys.stderr.write(f'[wwv_band_scan] {msg}\n')
    sys.stderr.flush()

OVL = []           # (time_ns, 'WWVCAP_OVERLOAD_DETECTED' | '..._CORRECTED')
OVL_PLUGIN = False
def _soapy_log(level, msg):
    if 'WWVCAP_OVERLOAD' in msg:
        OVL.append((time.time_ns(), msg.strip()))
    else:
        log(f'soapy {level}: {msg}')
SoapySDR.registerLogHandler(_soapy_log)
OVL_PLUGIN = (not a.replay and bool(os.environ.get('SOAPY_SDR_PLUGIN_PATH'))
              and os.environ.get('SOAPY_SDR_ROOT') == '/nonexistent')

dev = None
for _try in (range(0) if a.replay else range(20)):   # the API service takes a few seconds to come up / release the RSP
    if stop:
        sys.exit(1)
    try:
        dev = SoapySDR.Device(dict(driver='sdrplay'))
        break
    except RuntimeError as e:
        log(f'open failed ({e}), retrying')
        time.sleep(1)
if dev is None and not a.replay:
    log('RSP not available')
    sys.exit(1)

ch = 0
nwant = int(round(a.secs * a.rate))
raw = np.empty(2 * nwant, np.int16)
st = None
if dev is not None:
    dev.setSampleRate(SOAPY_SDR_RX, ch, a.rate)
    dev.setAntenna(SOAPY_SDR_RX, ch, a.antenna)
    dev.setGainMode(SOAPY_SDR_RX, ch, False)
    for k, v in (('biasT_ctrl', 'false'), ('rfnotch_ctrl', 'false'), ('extref_ctrl', 'false'),
                 ('iqcorr_ctrl', 'true')):
        dev.writeSetting(k, v)
    st = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [ch])
    mtu = dev.getStreamMTU(st)


def replay(prefix, khz):
    """Load up to --secs of a recording into raw[], as capture() would."""
    rec = np.fromfile(prefix + '.cs16', np.int16, count=2 * nwant)
    n = len(rec) // 2
    raw[:2 * n] = rec[:2 * n]
    tl = np.load(prefix + '.timelog.npy')
    tl = tl[(tl[:, 2] > 0) & (tl[:, 0] <= n)]
    t0 = float(np.min(tl[:, 1] / 1e9 - tl[:, 0] / a.rate))
    meta = json.load(open(prefix + '.json')) if os.path.exists(prefix + '.json') else {}
    info = dict(khz=khz, lna=int(meta.get('lna_state', -1)), ifgr=int(meta.get('ifgr', -1)),
                samples=n, overflows=int(meta.get('overflows', 0)), timeouts=int(meta.get('timeouts', 0)),
                overload_events=0, overloaded_at_start=False, replay=prefix)
    return n, t0, info


def capture(khz):
    """Tune, read --secs into raw[]; return (samples, t0 of sample 0 in s, overflows, timeouts, ovl info)."""
    lna, ifgr = wwv_bands.gain_for(khz, gains)
    fc = (khz - a.offset_khz) * 1e3
    dev.setFrequency(SOAPY_SDR_RX, ch, fc)
    dev.setGain(SOAPY_SDR_RX, ch, 'RFGR', lna)
    dev.setGain(SOAPY_SDR_RX, ch, 'IFGR', ifgr)
    info = dict(khz=khz, lna=lna, ifgr=ifgr, tuner_hz=dev.getFrequency(SOAPY_SDR_RX, ch),
                lna_readback=dev.getGain(SOAPY_SDR_RX, ch, 'RFGR'),
                ifgr_readback=dev.getGain(SOAPY_SDR_RX, ch, 'IFGR'))
    dev.activateStream(st)
    t_settle = time.time() + 1.0          # retune and gain settling
    tmp = np.empty(2 * mtu, np.int16)
    while time.time() < t_settle and not stop:
        dev.readStream(st, [tmp], mtu, timeoutUs=1000000)
    t_start_ns = time.time_ns()
    n = 0; overflows = 0; timeouts = 0; t0 = None
    while n < nwant and not stop:
        want = min(mtu, nwant - n)
        sr = dev.readStream(st, [raw[2 * n:]], want, timeoutUs=1000000)
        tn = time.time_ns()
        if sr.ret > 0:
            n += sr.ret
            t = tn / 1e9 - n / a.rate
            t0 = t if t0 is None else min(t0, t)
        elif sr.ret == SOAPY_SDR_OVERFLOW:
            overflows += 1
        elif sr.ret == SOAPY_SDR_TIMEOUT:
            timeouts += 1
    t_end_ns = time.time_ns()
    dev.deactivateStream(st)
    before = [m for t, m in OVL if t < t_start_ns]
    info.update(samples=n, overflows=overflows, timeouts=timeouts,
                overload_events=sum(1 for t, m in OVL if t_start_ns <= t <= t_end_ns and 'DETECTED' in m),
                overloaded_at_start=bool(before[-1:] == ['WWVCAP_OVERLOAD_DETECTED']))
    return n, t0, info


def db(x):
    return float(10 * np.log10(x)) if x > 0 else float('-inf')


def measure(khz, n, t0, info):
    x16 = raw[:2 * n]
    pk = int(np.max(np.abs(x16.astype(np.int32))))
    info['peak_dbfs'] = round(db((pk / 32767.0) ** 2), 1)
    info['clip_ppm'] = round(float(np.count_nonzero(np.abs(x16.astype(np.int32)) >= CLIP)) / len(x16) * 1e6, 2)
    # Mix the expected carrier to DC; stage 1 to 50 kHz (IIR, state kept across blocks),
    # stage 2 to 25 kHz (zero phase).
    f_mix = a.offset_khz * 1e3 + wwv_bands.carrier_hz(khz, a.ppm)
    sos1 = butter(6, 12e3, fs=a.rate, output='sos')
    zi = np.zeros((sos1.shape[0], 2), np.complex128)
    blk = DEC1 * 50000                    # 1 s blocks, a whole number of decimation steps
    y = []
    for s0 in range(0, n, blk):
        s1 = min(n, s0 + blk)
        iq = x16[2 * s0:2 * s1].astype(np.float32).view(np.complex64).astype(np.complex128) / 32768.0
        ph = (f_mix / a.rate * np.arange(s0, s1)) % 1.0
        z, zi = sosfilt(sos1, iq * np.exp(-2j * np.pi * ph), zi=zi)
        y.append(z[::DEC1])
    y = np.concatenate(y)
    y = sosfiltfilt(butter(8, 9e3, fs=50000, output='sos'), y)[::2]
    N = len(y)
    # Spectrum
    L = 1 << 17
    w = np.hanning(L)
    nseg = max(1, (N - L) // (L // 2) + 1)
    P = np.zeros(L)
    for k in range(nseg):
        seg = y[k * (L // 2):k * (L // 2) + L]
        if len(seg) < L:
            seg = np.pad(seg, (0, L - len(seg)))
        P += np.abs(np.fft.fftshift(np.fft.fft(seg * w))) ** 2
    f = np.fft.fftshift(np.fft.fftfreq(L, 1 / FS))
    bw = FS / L
    near = np.abs(f) < 50
    ic = np.flatnonzero(near)[np.argmax(P[near])]
    fcar = f[ic]
    df = np.abs(f - fcar)
    noise = np.median(P[(df > 2000) & (df < 4500)])
    info['carrier_offset_hz'] = round(f_mix - a.offset_khz * 1e3 + fcar, 2)
    info['tcxo_ppm_implied'] = round(info['carrier_offset_hz'] / (khz / 1000.0), 3)
    info['cn0_dbhz'] = round(db(P[ic] / noise / bw), 1)
    for tone in (1000, 1200):
        lv = []
        for sgn in (+1, -1):
            j = np.argmin(np.abs(f - (fcar + sgn * tone)))
            lv.append(np.max(P[j - 3:j + 4]))
        info[f'sb{tone}_db'] = round(db(max(lv) / noise), 1)
    # Ticks: AM envelope, folded by the UTC second
    env = np.abs(y)
    env = env - np.mean(env)
    t = t0 + np.arange(N) / FS
    ph = t % 1.0
    secs = np.floor(t)
    nz = secs % 60 != 0
    bb = np.minimum((ph * 1000).astype(int), 999)
    def tick(lo, hi):
        e = np.abs(sosfiltfilt(butter(4, [lo, hi], btype='band', fs=FS, output='sos'), env)) ** 2
        mm = np.bincount(bb[nz], e[nz], 1000) / np.maximum(np.bincount(bb[nz], None, 1000), 1)
        k = int(np.argmax(mm[:100]))
        fold = db(mm[k] / np.median(mm[300:900]))
        wl, wh = (k - 5) / 1000, (k + 5) / 1000
        c = []
        for s in np.unique(secs)[1:-1]:
            if s % 60 in (0, 29, 59):
                continue
            m = secs == s
            ps = ph[m]; ee = e[m]
            num = ee[(ps >= wl) & (ps < wh)]; den = ee[(ps > .3) & (ps < .9)]
            if len(num) and len(den):
                c.append(db(np.mean(num) / np.mean(den)))
        c = np.array(c)
        return k, fold, c
    k1, fold1, c1 = tick(900, 1100)
    k2, fold2, c2 = tick(1100, 1300)
    info.update(tick_peak_ms=k1, tick_fold_db=round(fold1, 1),
                tick_median_db=round(float(np.median(c1)), 1) if len(c1) else None,
                tick_p10_db=round(float(np.sort(c1)[len(c1) // 10]), 1) if len(c1) else None,
                tick_over10_pct=round(float(np.mean(c1 > 10) * 100)) if len(c1) else None,
                tick_seconds=int(len(c1)),
                wwvh_peak_ms=k2, wwvh_fold_db=round(fold2, 1),
                wwvh_median_db=round(float(np.median(c2)), 1) if len(c2) else None)
    info['wwvh_warning'] = bool(khz in wwv_bands.WWVH_KHZ and fold2 >= WWVH_VISIBLE_DB
                                and fold2 >= fold1 - WWVH_MARGIN_DB)
    info['adc_overload'] = bool(info['clip_ppm'] > 0 or info['overload_events'] > 0 or info['overloaded_at_start'])
    tm = info['tick_median_db'] if info['tick_median_db'] is not None else -99.0
    info['score_db'] = round(tm - (WWVH_PENALTY_DB if info['wwvh_warning'] else 0.0)
                             - (100.0 if info['adc_overload'] else 0.0), 1)
    info['usable'] = bool(info['score_db'] >= USABLE_DB)
    return info


rows = []
utc_start = time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())
if not a.replay:
    log(f'scanning {", ".join(f"{k / 1000:g}" for k in bands)} MHz, {a.secs:g} s each; '
        f'API overload events {"logged" if OVL_PLUGIN else "not available (stock SoapySDRPlay3 module)"}')
jobs = [(it.rsplit(':', 1)[0], int(float(it.rsplit(':', 1)[1]))) for it in a.replay] or [(None, k) for k in bands]
try:
    for prefix, khz in jobs:
        if stop:
            break
        if prefix:
            log(f'{khz / 1000:g} MHz: replaying {prefix}')
            n, t0, info = replay(prefix, khz)
            tu = time.strftime('%H:%M:%S', time.gmtime(t0))
        else:
            lna, ifgr = wwv_bands.gain_for(khz, gains)
            log(f'{khz / 1000:g} MHz: tuner {(khz - a.offset_khz) / 1000:g} MHz, LNA {lna}, IFGR {ifgr}, reading {a.secs:g} s')
            tu = time.strftime('%H:%M:%S', time.gmtime())
            n, t0, info = capture(khz)
        info['utc'] = tu
        if n < a.rate * 5 or t0 is None:
            log(f'{khz / 1000:g} MHz: only {n} samples, not measured')
            info.update(score_db=-999.0, usable=False, adc_overload=False, wwvh_warning=False)
            rows.append(info)
            continue
        rows.append(measure(khz, n, t0, info))
        r = rows[-1]
        log(f'{khz / 1000:g} MHz: C/N0 {r["cn0_dbhz"]} dB-Hz, tick median {r["tick_median_db"]} dB, '
            f'peak {r["peak_dbfs"]} dBFS, {r["overflows"]} overflows')
finally:
    if dev is not None:
        dev.closeStream(st)
        dev = None

def fmt(v, w, p=1):
    if v is None:
        return '-'.rjust(w)
    if isinstance(v, float):
        return f'{v:{w}.{p}f}'
    return str(v).rjust(w)

ranked = sorted(rows, key=lambda r: r['score_db'], reverse=True)
print(f'WWV band scan {utc_start}, {a.secs:g} s per band, ppm {a.ppm:g}, '
      f'API overload {"logged" if OVL_PLUGIN else "n/a"}')
print('rank  MHz  UTC       LNA IFGR  C/N0  carrier   ppm  sb1000 sb1200  tick@ms fold  tick_med p10 >10dB  '
      'WWVH@ms fold  peak_dBFS clip_ppm ovl  score  notes')
for i, r in enumerate(ranked, 1):
    notes = []
    if r.get('adc_overload'):
        notes.append('ADC OVERLOAD')
    if r.get('wwvh_warning'):
        notes.append('WWVH may dominate')
    if r.get('overflows'):
        notes.append(f'{r["overflows"]} overflows')
    if r.get('score_db', -999) <= -999:
        notes.append('not measured')
    elif not r.get('usable'):
        notes.append('not usable')
    print(f'{i:4d} {r["khz"] / 1000:5g} {r.get("utc", "-"):9s} {r["lna"]:3d} {r["ifgr"]:4d} '
          f'{fmt(r.get("cn0_dbhz"), 5)} {fmt(r.get("carrier_offset_hz"), 7, 2)} {fmt(r.get("tcxo_ppm_implied"), 5, 2)} '
          f'{fmt(r.get("sb1000_db"), 6)} {fmt(r.get("sb1200_db"), 6)} '
          f'{fmt(r.get("tick_peak_ms"), 7)} {fmt(r.get("tick_fold_db"), 5)} {fmt(r.get("tick_median_db"), 8)} '
          f'{fmt(r.get("tick_p10_db"), 4)} {fmt(r.get("tick_over10_pct"), 4)}%  '
          f'{fmt(r.get("wwvh_peak_ms"), 7)} {fmt(r.get("wwvh_fold_db"), 5)} '
          f'{fmt(r.get("peak_dbfs"), 9)} {fmt(r.get("clip_ppm"), 8, 2)} '
          f'{(str(r.get("overload_events")) if OVL_PLUGIN else "n/a"):>3s} {fmt(r.get("score_db"), 6)}  {", ".join(notes)}')
best = ranked[0] if ranked and ranked[0].get('score_db', -999) > -999 and not ranked[0].get('adc_overload') else None
if best is not None and not best['usable']:
    log(f'WARNING: no band reaches a {USABLE_DB:g} dB median tick contrast; the best is '
        f'{best["khz"] / 1000:g} MHz at {best["tick_median_db"]} dB')
if a.json:
    json.dump(dict(utc_start=utc_start, secs=a.secs, ppm=a.ppm, offset_khz=a.offset_khz, rate=a.rate,
                   overload_logged=OVL_PLUGIN, usable_db=USABLE_DB, rows=ranked,
                   best_khz=best['khz'] if best else None, best_usable=bool(best and best['usable'])),
              open(a.json, 'w'), indent=1)
print(f'BEST {best["khz"] if best else "none"}' + ('' if best is None or best['usable'] else ' (not usable)'))
sys.stdout.flush()
sys.exit(0 if best is not None else 3)
