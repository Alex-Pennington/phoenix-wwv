"""Stream time to UTC for the air and live scorers, corrected for slip against the anchor.

    from stream_utc import StreamUTC
    su = StreamUTC(log_lines, U0)       # U0: the timebase's out_utc0_unix
    su.utc(t_ms)                        # UTC (unix s) of stream time t_ms
    su.second(t_ms)                     # the UTC second holding it, floor(su.utc(t_ms))

U0 + t is UTC only while the stream keeps its anchor. A live stream slips against it: runG
(2026-11-21) ran 1 s ahead in 2.6 h on spurious fills, and run3h, run90d and run90g seg2
fell seconds behind on unfilled losses (NOTES.md, "Stream slip"). Truth taken as
floor(U0 + t) then names the wrong second once the slip and the event's place in its second
pass 1 s.

The correction is read from the signal's minute boundaries, as the decoder places them: for
each distinct decoder minute start ms (SYMMIN minute_start, WCLOSE anchor), the offset is
U0 + ms minus the nearest UTC minute (so it holds to 30 s). The correction at t is the median
of the offsets of the HALF anchors either side of the anchor nearest t: it follows a slow
slip, switches at a single step (a loss) halfway between anchors, and does not take up one
minute whose sync is wrong. The decoder places its minute start 4-21 ms off the UTC minute,
wandering about 10 ms over a run (runN1, runF seg2, run90), so the run's own baseline (the
median offset of its first 2*HALF+1 anchors, where the stream is on its anchor by
construction) is taken off, and a residual under DEAD (40 ms) counts as no slip: that is
placement noise, not slip (it reaches 35 ms on runF3 seg7, whose BCD pulses hold within 9 ms),
and a run that keeps its anchor scores as floor(U0 + t) did. A real slip that small (run90c
loses about 35 ms at 2950 s) is not corrected; it cannot move a second-level truth. It
does not use the decoder's second labels, so a scorer that checks those
(symbol_second_score.py) still checks something. With no anchors in the log the correction
is 0, which is the old floor(U0 + t)."""
import re, math, bisect

HALF = 5
DEAD = 0.040


class StreamUTC:
    def __init__(self, lines, U0, half=HALF):
        self.U0 = U0
        ms = set()
        for l in lines:
            if l.startswith('SYMMIN '):
                m = re.search(r'minute_start=([-\d.]+)', l)
            elif l.startswith('WCLOSE '):
                m = re.search(r' anchor=([-\d.]+)', l)
            else:
                continue
            if m:
                ms.add(float(m.group(1)))
        self.t = sorted(ms)
        off = [U0 + x / 1000 - round((U0 + x / 1000) / 60) * 60 for x in self.t]
        def med(v):
            v = sorted(v); return v[len(v) // 2]
        base = med(off[:2 * half + 1]) if off else 0.0
        self.corr = []
        for i in range(len(off)):
            c = med(off[max(0, i - half):i + half + 1]) - base
            self.corr.append(c if abs(c) >= DEAD else 0.0)

    def correction(self, t_ms):
        """seconds the stream is ahead of UTC at t_ms (U0 + t - UTC)"""
        if not self.t:
            return 0.0
        i = bisect.bisect_left(self.t, t_ms)
        if i == len(self.t) or (i > 0 and t_ms - self.t[i - 1] <= self.t[i] - t_ms):
            i -= 1
        return self.corr[i]

    def utc(self, t_ms):
        return self.U0 + t_ms / 1000 - self.correction(t_ms)

    def second(self, t_ms):
        return math.floor(self.utc(t_ms))
