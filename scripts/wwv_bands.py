"""WWV bands and the RSP2 settings for each, shared by rsp2_stream.py and wwv_band_scan.py.

WWV transmits on 2.5, 5, 10, 15, 20 and 25 MHz; WWVH (Kauai) on 2.5, 5, 10 and 15 MHz, on the
same carriers, with 1200 Hz ticks and tones where WWV uses 1000 Hz.

Gain: (LNA state, IF gain reduction dB) per band, AGC off. 5 MHz is LNA 2 / IFGR 59, the setting
of the scored saga capture (wwv-capture/saga/RESULTS-saga.md): at 5 MHz on the indoor whip the
floor stays antenna-noise-limited down to LNA 2-3, and no ADC overload was seen at LNA 0 / IFGR 59
with a broadcaster 25-30 dB over WWV in the passband. 10 MHz was tested at LNA 2 / IFGR 59 with no
overload. 2.5 and 15 MHz keep LNA 2 (tropical-band and 19 m broadcasters in the passband).
20 and 25 MHz take LNA 1: their band noise is lower and their passbands carry less broadcasting.
The 2.5, 15, 20 and 25 MHz values are starting points; wwv_band_scan.py reports the ADC overload
and clipping for each band, and --gain KHZ:LNA:IFGR overrides any of them.

TCXO: the RSP2's LO and ADC share one TCXO, read 1.09 ppm slow at 5 MHz on 2026-11-19 (carrier at
+5.448 Hz). A fractional clock error e puts a carrier at f a further f*e Hz above where it is
expected, so the carrier correction is ppm * f_MHz Hz: 5.45 Hz at 5 MHz, 27.3 Hz at 25 MHz.
"""

WWV_KHZ = (2500, 5000, 10000, 15000, 20000, 25000)
WWVH_KHZ = (2500, 5000, 10000, 15000)
TCXO_PPM = 1.09

GAIN = {        # kHz: (LNA state, IFGR dB)
    2500: (2, 59),
    5000: (2, 59),
    10000: (2, 59),
    15000: (2, 59),
    20000: (1, 59),
    25000: (1, 59),
}


def parse_gain_overrides(items):
    """['15000:3:59', ...] -> {15000: (3, 59)}."""
    out = {}
    for it in items or ():
        try:
            k, lna, ifgr = it.split(':')
            out[int(float(k))] = (int(lna), int(ifgr))
        except ValueError:
            raise SystemExit(f'bad --gain {it!r}: want KHZ:LNA:IFGR, e.g. 15000:3:59')
    return out


def gain_for(khz, overrides=None):
    """(LNA state, IFGR) for a band: an override, else the table, else the 5 MHz setting."""
    k = int(round(khz))
    if overrides and k in overrides:
        return overrides[k]
    return GAIN.get(k, GAIN[5000])


def carrier_hz(khz, ppm=TCXO_PPM):
    """Carrier offset from the TCXO error, Hz, at WWV frequency khz."""
    return ppm * khz / 1000.0
