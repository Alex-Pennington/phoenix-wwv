"""Stub rsp2_stream.py for tests/live: zeros at --rate on stdout, no radio. STUB_MODE:
  ok               stream until --secs (wall clock) or a signal
  hang_after:S     stop writing and stop the heartbeat after S s, stay alive
  stall75_after:S  exit 75 (rsp2_stream's stall status) after S s
  gap76_after:S    exit 76 (rsp2_stream: a loss too large to fill) after S s
  dead             write nothing, keep the heartbeat, until --secs
  crash_after:S    after S s, the vendor service's crash: STUB_ERRNO (default 18) libusb
                   errno=12 lines in $STUB_DIR/journal, then a new MainPID and NRestarts + 1
                   (systemd's restart); the stream goes on until stopped
  errno_after:S    after S s, the errno=12 lines only (no restart), then exit 75
--antenna NAME: recorded, one line per start, in $STUB_DIR/antennas; a name not in
"Antenna A", "Antenna B", "Hi-Z" (the RSP2's list) exits 2, as rsp2_stream.py does.
STUB_ONLY_FIRST=1 applies the mode to the first start only; STUB_DEAF=1 ignores SIGTERM
and --secs. Each start appends one byte to $STUB_DIR/streams."""
import os, sys, time, signal, argparse
ap = argparse.ArgumentParser()
ap.add_argument('--rate', type=float, default=2e6)
ap.add_argument('--secs', type=float, default=0)
ap.add_argument('--heartbeat')
ap.add_argument('--stall-secs', type=float)
ap.add_argument('--antenna', default='Antenna A')
a, _ = ap.parse_known_args()
kind, _, v = os.environ.get('STUB_MODE', 'ok').partition(':')
d = os.environ['STUB_DIR']
with open(os.path.join(d, 'streams'), 'a') as f:
    f.write('x')
nth = len(open(os.path.join(d, 'streams')).read())
if os.environ.get('STUB_ONLY_FIRST') and nth > 1:
    kind = 'ok'
with open(os.path.join(d, 'antennas'), 'a') as f:
    f.write(a.antenna + '\n')
ANTENNAS = ('Antenna A', 'Antenna B', 'Hi-Z')
if a.antenna not in ANTENNAS:
    sys.stderr.write(f'[stub_stream] --antenna {a.antenna!r}: not an antenna of this device; '
                     f'it has {", ".join(map(repr, ANTENNAS))}\n')
    sys.exit(2)
stop = False
crashed = False


def errno_lines():
    ts = time.strftime('%Y-%m-%dT%H:%M:%S+00:00', time.gmtime())
    line = f'{ts} saga sdrplay_apiService[{os.getpid()}]: libusb: error [submit_iso_transfer] submiturb failed, errno=12\n'
    with open(os.path.join(d, 'journal'), 'a') as f:
        f.write(line * int(os.environ.get('STUB_ERRNO', '18')))


def put(name, text):
    with open(os.path.join(d, name + '.new'), 'w') as f:
        f.write(text)
    os.replace(os.path.join(d, name + '.new'), os.path.join(d, name))


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
    if kind == 'gap76_after' and el >= float(v):
        sys.stderr.write('[stub_stream] GAP too large to fill, exit 76\n')
        sys.exit(76)
    if kind == 'crash_after' and el >= float(v) and not crashed:
        crashed = True
        errno_lines()
        time.sleep(0.3)
        nr = int(open(os.path.join(d, 'nr')).read() or 0) + 1
        put('pid', f'{os.getpid() + 7}\n')
        put('nr', f'{nr}\n')
        sys.stderr.write('[stub_stream] the vendor service crashed and was restarted\n')
    if kind == 'errno_after' and el >= float(v):
        errno_lines()
        sys.stderr.write('[stub_stream] errno=12 lines logged, exit 75\n')
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
