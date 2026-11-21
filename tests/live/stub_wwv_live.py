"""Stub wwv_live for tests/live: no decode. It reads its input to the end (as wwv_live does) and
prints SYNC lines, in wwv_live's format, on a schedule counted from its own start:
  STUB_SYNC=S:STATE,S:STATE,...   e.g. 1:LOCKED,6:RECOVERING
Each start (each segment) runs the whole schedule again. With STUB_SYNC_KHZ=KHZ only a start
with --band-khz KHZ runs it; the others print no SYNC line. It ignores its other options and
exits 0 at the end of its input."""
import os, sys, threading, time

t0 = time.monotonic()


def schedule():
    want = os.environ.get('STUB_SYNC_KHZ')
    band = sys.argv[sys.argv.index('--band-khz') + 1] if '--band-khz' in sys.argv[1:-1] else None
    if want and band != want:
        return
    old = 'ACQUIRING'
    for item in filter(None, os.environ.get('STUB_SYNC', '').split(',')):
        at, _, new = item.partition(':')
        time.sleep(max(0.0, t0 + float(at) - time.monotonic()))
        sys.stdout.write(f'SYNC     {time.monotonic() - t0:8.1f} {old} -> {new} conf 0.90\n')
        sys.stdout.flush()
        old = new


threading.Thread(target=schedule, daemon=True).start()
while sys.stdin.buffer.read1(1 << 16):
    pass
sys.exit(0)
