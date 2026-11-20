"""frame_bit(): the WWV symbol for the UTC second starting at unix time s, as
~/wwv-capture/score.py builds it (NIST SP 432 Table 3.13; values for 2026-11:
DUT1 -0.1 s, no leap second pending, both DST bits 0). exec()'d by the scorers."""
import datetime
DUT1 = -0.1


def frame_bit(s):
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
