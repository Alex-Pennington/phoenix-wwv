"""Measure each WWV band on the RSP2 and choose one. Run it through wwv_band_scan.sh, which starts
and stops sdrplay.service.

Usage: python3 -I wwv_band_scan.py [--bands 2500,5000,10000,15000,20000,25000] [--secs 25]
           [--extend-secs 35] [--avoid KHZ ...] [--gain KHZ:LNA:IFGR ...] [--ppm 1.09]
           [--offset-khz 100] [--rate 2000000] [--antenna "Antenna A"] [--json FILE]
           [--replay PREFIX:KHZ ...]

The device is opened once. For each band it is retuned (WWV at +offset, as rsp2_stream.py),
set to the band's gain (wwv_bands.GAIN, or --gain), and read for --secs into memory; the
band is measured before the next one is read. When the choice is close (below), the
contending bands are read again for --extend-secs and scored on both reads together. The
table goes to stdout, the last line is `BEST <kHz>` (or `BEST none`), and --json writes
every figure, the per-second tick contrasts included. --replay measures recordings instead
(PREFIX.cs16 and PREFIX.timelog.npy from rsp2_stream.py --record or rsp_record.py,
PREFIX.json for the gain) and opens no device; an extension reads the next --extend-secs
of the recording.

Measurements, after wwv-capture check_signal.py and saga/scripts/tick_contrast.py:
- Carrier: the RF is mixed by offset + ppm*f, decimated to 25 kHz, and the strongest bin
  within 50 Hz of DC is the carrier (131072-point Hann spectra, averaged). The noise floor is
  the median bin 2000-4500 Hz from the carrier. C/N0 = carrier/noise per bin / bin width, the
  same convention as check_signal.py. The carrier's offset gives the TCXO error it implies.
  Under 30 dB-Hz there is no carrier (noise alone reads 11-14 dB-Hz): no offset is given.
- Sidebands: 1000 Hz (WWV) and 1200 Hz (WWVH) levels over the floor, the larger of +/-.
- Ticks: the 900-1100 Hz energy of the AM envelope, folded by the UTC second in 1 ms bins
  (seconds :00 left out). The fold peak in the first 100 ms is the tick; "fold" is its level
  over the 300-900 ms median. Per-second contrast is the energy in [peak-5, peak+5) ms over
  300-900 ms of the same second, :00/:29/:59 left out; the table gives its median, p10 and
  the share of seconds over 10 dB. 1100-1300 Hz is folded the same way for the WWVH tick.
  Deviation: the envelope |IQ| is used, not check_signal's coherent real part, so a residual
  carrier offset cannot fade the demodulated audio within the measurement.
- Timing: the system time of sample 0 is the least-delayed read (arrival - n/fs), the method
  of the scored saga capture. Host latency makes the fold peak late by a few ms; the tick
  window follows the peak, so the contrast does not depend on it.
- ADC: raw peak in dBFS and the share of I or Q samples at or above 32700. API overload
  events are counted when the overload-logging SoapySDRPlay3 build is loaded (it logs
  WWVCAP_OVERLOAD_*; wwv_band_scan.sh loads it when present); the stock module drops them.

Score: the share of seconds whose tick contrast is over TICK_DB (10 dB), in percent. Where
WWVH may dominate (a WWVH band, 2.5-15 MHz, whose 1200 Hz tick fold is at least 6 dB and
within 3 dB of the 1000 Hz one) the contrast must clear TICK_DB + 6 dB instead. A band with
ADC clipping or an API overload scores -1 and is never chosen. A score of USABLE_PCT or more
is "usable". docs/WWV_LIVE.md gives the evidence for each threshold.

Choice: the lowest-frequency band whose score is above 0 and within MARGIN_PCT of the best, among
the usable bands, or among all measured bands when none is usable (then flagged "not
usable"). A higher band wins only by more than MARGIN_PCT. When every score is 0, the
higher median contrast wins, then the lower band. --avoid drops a band from the choice when
another band is in the running (wwv_rsp2_live.sh avoids the band that failed to lock).

Extension: when two or more bands score above 0 and within MARGIN_PCT of the best, each of
them is read for --extend-secs more and the choice is made again on all their seconds.
--extend-secs 0 turns it off.
"""
import os, sys, json, time, argparse, signal
import numpy as np
from scipy.signal import butter, sosfilt, sosfiltfilt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True   # no __pycache__ in the tree
import wwv_bands

FS = 25000.0                # analysis rate
TICK_DB = 10.0              # a second "has a tick" when its contrast is over this
USABLE_PCT = 50.0           # score at or over this is usable
MARGIN_PCT = 30.0           # a higher band must beat a lower one by more than this
WWVH_MARGIN_DB = 3.0
WWVH_VISIBLE_DB = 6.0       # a 1200 Hz tick fold below this is noise, not WWVH
WWVH_PENALTY_DB = 6.0
CLIP = 32700
CARRIER_DBHZ = 30.0         # below this the "carrier" is the strongest of ~500 noise bins (11-14 dB-Hz)


def log(msg):
    sys.stderr.write(f'[wwv_band_scan] {msg}\n')
    sys.stderr.flush()


def db(x):
    return float(10 * np.log10(x)) if x > 0 else float('-inf')


def measure(x16, n, t0, khz, info, ppm, offset_khz, rate):
    """Measure n samples of interleaved CS16 x16 whose sample 0 is at system time t0.

    Fills and returns info with the carrier, sideband, tick and ADC figures and
    'tick_contrasts_db', the per-second 1000 Hz tick contrasts. The score is set by
    score_row()."""
    dec1 = int(round(rate / 50000))
    x16 = x16[:2 * n]
    ax = np.abs(x16.astype(np.int32))
    pk = int(np.max(ax))
    info['peak_dbfs'] = round(db((pk / 32767.0) ** 2), 1)
    info['clip_ppm'] = round(float(np.count_nonzero(ax >= CLIP)) / len(x16) * 1e6, 2)
    del ax
    # Mix the expected carrier to DC; stage 1 to 50 kHz (IIR, state kept across blocks),
    # stage 2 to 25 kHz (zero phase).
    f_mix = offset_khz * 1e3 + wwv_bands.carrier_hz(khz, ppm)
    sos1 = butter(6, 12e3, fs=rate, output='sos')
    zi = np.zeros((sos1.shape[0], 2), np.complex128)
    blk = dec1 * 50000                    # 1 s blocks, a whole number of decimation steps
    y = []
    for s0 in range(0, n, blk):
        s1 = min(n, s0 + blk)
        iq = x16[2 * s0:2 * s1].astype(np.float32).view(np.complex64).astype(np.complex128) / 32768.0
        ph = (f_mix / rate * np.arange(s0, s1)) % 1.0
        z, zi = sosfilt(sos1, iq * np.exp(-2j * np.pi * ph), zi=zi)
        y.append(z[::dec1])
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
    info['cn0_dbhz'] = round(db(P[ic] / noise / bw), 1)
    info['carrier_seen'] = bool(info['cn0_dbhz'] >= CARRIER_DBHZ)
    if info['carrier_seen']:
        info['carrier_offset_hz'] = round(f_mix - offset_khz * 1e3 + fcar, 2)
        info['tcxo_ppm_implied'] = round(info['carrier_offset_hz'] / (khz / 1000.0), 3)
    else:               # the strongest noise bin: no offset to report
        info['carrier_offset_hz'] = info['tcxo_ppm_implied'] = None
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
        return k, fold, np.array(c)
    k1, fold1, c1 = tick(900, 1100)
    k2, fold2, c2 = tick(1100, 1300)
    info.update(tick_peak_ms=k1, tick_fold_db=round(fold1, 1),
                tick_contrasts_db=[round(float(v), 1) for v in c1],
                wwvh_peak_ms=k2, wwvh_fold_db=round(fold2, 1),
                wwvh_median_db=round(float(np.median(c2)), 1) if len(c2) else None)
    info['wwvh_warning'] = bool(khz in wwv_bands.WWVH_KHZ and fold2 >= WWVH_VISIBLE_DB
                                and fold2 >= fold1 - WWVH_MARGIN_DB)
    info['adc_overload'] = bool(info['clip_ppm'] > 0 or info['overload_events'] > 0
                                or info['overloaded_at_start'])
    return info


def score_row(info):
    """Contrast summary and score from info['tick_contrasts_db'] (all reads of the band)."""
    c = np.array(info.get('tick_contrasts_db') or [], float)
    n = len(c)
    info['tick_seconds'] = int(n)
    info['tick_median_db'] = round(float(np.median(c)), 1) if n else None
    info['tick_p10_db'] = round(float(np.sort(c)[n // 10]), 1) if n else None
    info['tick_over10_pct'] = round(float(np.mean(c > TICK_DB) * 100)) if n else None
    need = TICK_DB + (WWVH_PENALTY_DB if info.get('wwvh_warning') else 0.0)
    if not n:
        info['score_pct'] = None
    elif info.get('adc_overload'):
        info['score_pct'] = -1.0
    else:
        info['score_pct'] = round(float(np.mean(c > need) * 100), 1)
    info['usable'] = bool(info['score_pct'] is not None and info['score_pct'] >= USABLE_PCT)
    return info


def _rank_key(r):
    """Ranking order: score, then median contrast, then the lower band."""
    s = r.get('score_pct')
    m = r.get('tick_median_db')
    return (-(s if s is not None else -999.0), -(m if m is not None else -999.0), r['khz'])


def _measured(rows):
    return [r for r in rows if r.get('score_pct') is not None and r['score_pct'] >= 0]


def contenders(rows):
    """Bands above 0 and within MARGIN_PCT of the best score, when there are two or more."""
    ok = _measured(rows)
    if not ok:
        return []
    top = max(r['score_pct'] for r in ok)
    near = [r for r in ok if r['score_pct'] > 0 and r['score_pct'] >= top - MARGIN_PCT]
    return near if len(near) >= 2 else []


def _pick(pool):
    """(best, leader): the lowest band above 0 and within MARGIN_PCT of the leader, when
    the leader is above 0; else the leader."""
    lead = sorted(pool, key=_rank_key)[0]
    if lead['score_pct'] <= 0:
        return lead, lead
    top = lead['score_pct']
    return min((r for r in pool if r['score_pct'] > 0 and r['score_pct'] >= top - MARGIN_PCT),
               key=lambda r: r['khz']), lead


def choose(rows, avoid=()):
    """(best row or None, reason). See the module docstring."""
    ok = _measured(rows)
    if not ok:
        return None, 'no band measured without ADC overload'
    usable = [r for r in ok if r['usable']]
    pool = [r for r in usable if r['khz'] not in avoid]
    av = ', '.join(f'{k / 1000:g}' for k in sorted(avoid))
    note = ''
    if usable and not pool:
        pool = usable
        note = f'; {av} MHz avoided, but no other band is usable'
    elif avoid and any(r['khz'] in avoid for r in usable):
        note = f'; {av} MHz avoided'
    if not pool:
        pool = [r for r in ok if r['khz'] not in avoid] or ok
        if avoid:
            note = f'; {av} MHz avoided' if len(pool) < len(ok) else f'; {av} MHz avoided, but no other band was measured'
    best, lead = _pick(pool)
    if best is lead:
        why = f'{best["khz"] / 1000:g} MHz scores {best["score_pct"]:g}%'
        lower = sorted((r for r in pool if r['khz'] < best['khz']), key=lambda r: r['khz'])
        if lower:
            why += '; lower bands: ' + ', '.join(f'{r["khz"] / 1000:g} MHz {r["score_pct"]:g}%' for r in lower)
    else:
        why = (f'{best["khz"] / 1000:g} MHz scores {best["score_pct"]:g}%, within {MARGIN_PCT:g} '
               f'points of {lead["khz"] / 1000:g} MHz ({lead["score_pct"]:g}%): the lower band')
    if not best['usable']:
        why = f'no band reaches {USABLE_PCT:g}%; ' + why
    return best, why + note


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bands', default=','.join(str(k) for k in wwv_bands.WWV_KHZ))
    ap.add_argument('--secs', type=float, default=25.0)
    ap.add_argument('--extend-secs', type=float, default=35.0)
    ap.add_argument('--avoid', action='append', default=[], metavar='KHZ')
    ap.add_argument('--gain', action='append', default=[], metavar='KHZ:LNA:IFGR')
    ap.add_argument('--ppm', type=float, default=wwv_bands.TCXO_PPM)
    ap.add_argument('--offset-khz', type=float, default=100.0)
    ap.add_argument('--rate', type=float, default=2e6)
    ap.add_argument('--antenna', default='Antenna A')
    ap.add_argument('--json', default=None)
    ap.add_argument('--replay', action='append', default=[], metavar='PREFIX:KHZ')
    a = ap.parse_args()
    bands = [int(float(x)) for x in a.bands.split(',') if x.strip()]
    avoid = {int(float(x)) for x in a.avoid}
    gains = wwv_bands.parse_gain_overrides(a.gain)
    if int(round(a.rate / 50000)) * 50000 != a.rate:
        raise SystemExit('--rate must be a whole multiple of 50000')

    stop = [False]

    def _stop(sig, frm):
        stop[0] = True
    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    OVL = []           # (time_ns, 'WWVCAP_OVERLOAD_DETECTED' | '..._CORRECTED')
    OVL_PLUGIN = False
    dev = st = None
    mtu = 0
    if not a.replay:
        import SoapySDR
        from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CS16, SOAPY_SDR_OVERFLOW, SOAPY_SDR_TIMEOUT

        def _soapy_log(level, msg):
            if 'WWVCAP_OVERLOAD' in msg:
                OVL.append((time.time_ns(), msg.strip()))
            else:
                log(f'soapy {level}: {msg}')
        SoapySDR.registerLogHandler(_soapy_log)
        OVL_PLUGIN = (bool(os.environ.get('SOAPY_SDR_PLUGIN_PATH'))
                      and os.environ.get('SOAPY_SDR_ROOT') == '/nonexistent')
        for _try in range(20):   # the API service takes a few seconds to come up / release the RSP
            if stop[0]:
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
    nmax = int(round(max(a.secs, a.extend_secs) * a.rate))
    raw = np.empty(2 * nmax, np.int16)
    if dev is not None:
        dev.setSampleRate(SOAPY_SDR_RX, ch, a.rate)
        dev.setAntenna(SOAPY_SDR_RX, ch, a.antenna)
        dev.setGainMode(SOAPY_SDR_RX, ch, False)
        for k, v in (('biasT_ctrl', 'false'), ('rfnotch_ctrl', 'false'), ('extref_ctrl', 'false'),
                     ('iqcorr_ctrl', 'true')):
            dev.writeSetting(k, v)
        st = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [ch])
        mtu = dev.getStreamMTU(st)

    def replay(prefix, khz, secs, skip_s=0.0):
        """Load secs of a recording, from skip_s in, into raw[], as capture() would."""
        nwant = int(round(secs * a.rate))
        s0 = int(round(skip_s * a.rate))
        rec = np.fromfile(prefix + '.cs16', np.int16, count=2 * nwant, offset=4 * s0)
        n = len(rec) // 2
        raw[:2 * n] = rec[:2 * n]
        tl = np.load(prefix + '.timelog.npy')
        tl = tl[(tl[:, 2] > 0) & (tl[:, 0] <= s0 + n) & (tl[:, 0] > s0)]
        t0 = float(np.min(tl[:, 1] / 1e9 - tl[:, 0] / a.rate)) + s0 / a.rate if len(tl) else None
        meta = json.load(open(prefix + '.json')) if os.path.exists(prefix + '.json') else {}
        info = dict(khz=khz, lna=int(meta.get('lna_state', -1)), ifgr=int(meta.get('ifgr', -1)),
                    samples=n, overflows=int(meta.get('overflows', 0)), timeouts=int(meta.get('timeouts', 0)),
                    overload_events=0, overloaded_at_start=False, replay=prefix, replay_skip_s=skip_s)
        return n, t0, info

    def capture(khz, secs):
        """Tune, read secs into raw[]; return (samples, t0 of sample 0 in s, info)."""
        nwant = int(round(secs * a.rate))
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
        while time.time() < t_settle and not stop[0]:
            dev.readStream(st, [tmp], mtu, timeoutUs=1000000)
        t_start_ns = time.time_ns()
        n = 0; overflows = 0; timeouts = 0; t0 = None
        while n < nwant and not stop[0]:
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

    def read_band(prefix, khz, secs, skip_s=0.0):
        """One read of a band, measured; None when it gave under 5 s of samples."""
        if prefix:
            log(f'{khz / 1000:g} MHz: replaying {prefix}' + (f' from {skip_s:g} s' if skip_s else ''))
            n, t0, info = replay(prefix, khz, secs, skip_s)
            tu = time.strftime('%H:%M:%S', time.gmtime(t0)) if t0 else '-'
        else:
            lna, ifgr = wwv_bands.gain_for(khz, gains)
            log(f'{khz / 1000:g} MHz: tuner {(khz - a.offset_khz) / 1000:g} MHz, LNA {lna}, IFGR {ifgr}, '
                f'reading {secs:g} s')
            tu = time.strftime('%H:%M:%S', time.gmtime())
            n, t0, info = capture(khz, secs)
        info['utc'] = tu
        if n < a.rate * 5 or t0 is None:
            log(f'{khz / 1000:g} MHz: only {n} samples, not measured')
            return info, False
        return measure(raw, n, t0, khz, info, a.ppm, a.offset_khz, a.rate), True

    rows = []
    utc_start = time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())
    if not a.replay:
        log(f'scanning {", ".join(f"{k / 1000:g}" for k in bands)} MHz, {a.secs:g} s each; '
            f'API overload events {"logged" if OVL_PLUGIN else "not available (stock SoapySDRPlay3 module)"}')
    jobs = [(it.rsplit(':', 1)[0], int(float(it.rsplit(':', 1)[1]))) for it in a.replay] or [(None, k) for k in bands]
    extended = []
    try:
        for prefix, khz in jobs:
            if stop[0]:
                break
            info, ok = read_band(prefix, khz, a.secs)
            info['prefix'] = prefix
            if not ok:
                info.update(score_pct=None, usable=False, adc_overload=False, wwvh_warning=False)
                rows.append(info)
                continue
            rows.append(score_row(info))
            r = rows[-1]
            log(f'{khz / 1000:g} MHz: C/N0 {r["cn0_dbhz"]} dB-Hz, tick median {r["tick_median_db"]} dB, '
                f'{r["tick_over10_pct"]}% of {r["tick_seconds"]} s over {TICK_DB:g} dB, score {r["score_pct"]}%, '
                f'peak {r["peak_dbfs"]} dBFS, {r["overflows"]} overflows')
        near = contenders(rows) if a.extend_secs > 0 and not stop[0] else []
        if near:
            log(f'close call between {", ".join(f"{r["khz"] / 1000:g}" for r in near)} MHz '
                f'(scores {", ".join(f"{r["score_pct"]:g}%" for r in near)}): reading each '
                f'{a.extend_secs:g} s more')
        for r in near:
            if stop[0]:
                break
            info, ok = read_band(r['prefix'], r['khz'], a.extend_secs, a.secs)
            if not ok:
                continue
            r['tick_contrasts_db'] = r['tick_contrasts_db'] + info['tick_contrasts_db']
            r['extended'] = dict((k, info.get(k)) for k in (
                'utc', 'samples', 'cn0_dbhz', 'tick_fold_db', 'wwvh_fold_db', 'peak_dbfs', 'clip_ppm',
                'overload_events', 'overflows'))
            r['extended']['tick_seconds'] = len(info['tick_contrasts_db'])
            r['adc_overload'] = bool(r['adc_overload'] or info['adc_overload'])
            score_row(r)
            extended.append(r['khz'])
            log(f'{r["khz"] / 1000:g} MHz extended: {r["tick_over10_pct"]}% of {r["tick_seconds"]} s over '
                f'{TICK_DB:g} dB, score {r["score_pct"]}%')
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

    best, why = choose(rows, avoid)
    ranked = sorted(rows, key=_rank_key)
    print(f'WWV band scan {utc_start}, {a.secs:g} s per band, ppm {a.ppm:g}, '
          f'API overload {"logged" if OVL_PLUGIN else "n/a"}')
    print('rank  MHz  UTC       LNA IFGR  C/N0  carrier   ppm  sb1000 sb1200  tick@ms fold  tick_med p10 >10dB  secs  '
          'WWVH@ms fold  peak_dBFS clip_ppm ovl  score  notes')
    for i, r in enumerate(ranked, 1):
        notes = []
        if r is best:
            notes.append('CHOSEN')
        if r.get('adc_overload'):
            notes.append('ADC OVERLOAD')
        if r.get('wwvh_warning'):
            notes.append('WWVH may dominate')
        if r.get('carrier_seen') is False:
            notes.append('no carrier')
        if r.get('overflows'):
            notes.append(f'{r["overflows"]} overflows')
        if r['khz'] in extended:
            notes.append('extended')
        if r['khz'] in avoid:
            notes.append('avoided')
        if r.get('score_pct') is None:
            notes.append('not measured')
        elif not r.get('usable'):
            notes.append('not usable')
        print(f'{i:4d} {r["khz"] / 1000:5g} {r.get("utc", "-"):9s} {r["lna"]:3d} {r["ifgr"]:4d} '
              f'{fmt(r.get("cn0_dbhz"), 5)} {fmt(r.get("carrier_offset_hz"), 7, 2)} {fmt(r.get("tcxo_ppm_implied"), 5, 2)} '
              f'{fmt(r.get("sb1000_db"), 6)} {fmt(r.get("sb1200_db"), 6)} '
              f'{fmt(r.get("tick_peak_ms"), 7)} {fmt(r.get("tick_fold_db"), 5)} {fmt(r.get("tick_median_db"), 8)} '
              f'{fmt(r.get("tick_p10_db"), 4)} {fmt(r.get("tick_over10_pct"), 4)}% {fmt(r.get("tick_seconds"), 5)}  '
              f'{fmt(r.get("wwvh_peak_ms"), 7)} {fmt(r.get("wwvh_fold_db"), 5)} '
              f'{fmt(r.get("peak_dbfs"), 9)} {fmt(r.get("clip_ppm"), 8, 2)} '
              f'{(str(r.get("overload_events")) if OVL_PLUGIN else "n/a"):>3s} '
              f'{fmt(r.get("score_pct"), 5)}%  {", ".join(notes)}')
    log(f'choice: {why}')
    if best is not None and not best['usable']:
        log(f'WARNING: no band has {USABLE_PCT:g}% of its seconds over a {TICK_DB:g} dB tick contrast')
    if a.json:
        json.dump(dict(utc_start=utc_start, secs=a.secs, extend_secs=a.extend_secs, ppm=a.ppm,
                       offset_khz=a.offset_khz, rate=a.rate, overload_logged=OVL_PLUGIN,
                       tick_db=TICK_DB, usable_pct=USABLE_PCT, margin_pct=MARGIN_PCT,
                       avoid=sorted(avoid), extended=extended, rows=ranked,
                       best_khz=best['khz'] if best else None, best_usable=bool(best and best['usable']),
                       choice=why),
                  open(a.json, 'w'), indent=1)
    print(f'BEST {best["khz"] if best else "none"}' + ('' if best is None or best['usable'] else ' (not usable)'))
    sys.stdout.flush()
    sys.exit(0 if best is not None else 3)


if __name__ == '__main__':
    main()
