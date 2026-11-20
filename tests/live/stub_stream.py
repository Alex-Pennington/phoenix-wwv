"""Stub rsp2_stream.py for tests/live: zeros at --rate on stdout, no radio. STUB_MODE:
  ok               stream until --secs (wall clock) or a signal
  hang_after:S     stop writing and stop the heartbeat after S s, stay alive
  stall75_after:S  exit 75 (rsp2_stream's stall status) after S s
  dead             write nothing, keep the heartbeat, until --secs
STUB_ONLY_FIRST=1 applies the mode to the first start only; STUB_DEAF=1 ignores SIGTERM
and --secs. Each start appends one byte to $STUB_DIR/streams."""
import os, sys, time, signal, argparse
ap = argparse.ArgumentParser()
ap.add_argument('--rate', type=float, default=2e6)
ap.add_argument('--secs', type=float, default=0)
ap.add_argument('--heartbeat')
ap.add_argument('--stall-secs', type=float)
a, _ = ap.parse_known_args()
kind, _, v = os.environ.get('STUB_MODE', 'ok').partition(':')
d = os.environ['STUB_DIR']
with open(os.path.join(d, 'streams'), 'a') as f:
    f.write('x')
nth = len(open(os.path.join(d, 'streams')).read())
if os.environ.get('STUB_ONLY_FIRST') and nth > 1:
    kind = 'ok'
stop = False


def h(*_):
    global stop
    stop = True


deaf = bool(os.environ.get('STUB_DEAF'))
signal.signal(signal.SIGTERM, signal.SIG_IGN if deaf else h)
signal.signal(signal.SIGINT, h)
if deaf:
    a.secs = 0
t0 = time.time()
blk = bytes(4 * 50000)
sent = 0
sys.stderr.write(f'[stub_stream] start #{nth} mode {kind}:{v} secs {a.secs}\n')
while not stop:
    el = time.time() - t0
    if a.secs and el >= a.secs:
        break
    if kind == 'stall75_after' and el >= float(v):
        sys.stderr.write('[stub_stream] stalled, exit 75\n')
        sys.exit(75)
    if kind == 'hang_after' and el >= float(v):
        time.sleep(0.2)
        continue
    if kind == 'dead':
        os.utime(a.heartbeat)
        time.sleep(0.2)
        continue
    due = el * a.rate
    if sent + 50000 > due:
        time.sleep((sent + 50000 - due) / a.rate)
    try:
        sys.stdout.buffer.write(blk)
        sys.stdout.buffer.flush()
    except BrokenPipeError:
        break
    sent += 50000
    os.utime(a.heartbeat)
sys.stderr.write(f'[stub_stream] end #{nth} after {time.time() - t0:.1f} s\n')
