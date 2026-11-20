"""Serve a raw I/Q file over the sdr_server TCP protocol (PHXI header, IQDQ frames),
as waterfall.c 1ec38d1 read it, for testing `wwv_live -i tcp:HOST:PORT`.

Usage: python3 -I iqdq_replay.py FILE --rate HZ --format s16|f32 [--port 4536]
           [--centre HZ] [--frame N] [--realtime] [--meta-every N]

Accepts one client, sends the 32-byte PHXI header, then the file in IQDQ frames of
N samples (16-byte header: magic, sequence, num_samples, flags), and closes. With
--meta-every N it also sends a META frame (same rate/format) every N data frames.
"""
import argparse, socket, struct, time

ap = argparse.ArgumentParser()
ap.add_argument('file')
ap.add_argument('--rate', type=int, required=True)
ap.add_argument('--format', choices=('s16', 'f32'), required=True)
ap.add_argument('--port', type=int, default=4536)
ap.add_argument('--centre', type=int, default=5000000)
ap.add_argument('--frame', type=int, default=8192)
ap.add_argument('--realtime', action='store_true')
ap.add_argument('--meta-every', type=int, default=0)
a = ap.parse_args()

PHXI, IQDQ, META = 0x50485849, 0x49514451, 0x4D455441
fmt = 1 if a.format == 's16' else 2
bps = 4 if a.format == 's16' else 8
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', a.port))
srv.listen(1)
print(f'listening on 127.0.0.1:{a.port}', flush=True)
conn, _ = srv.accept()
lo, hi = a.centre & 0xFFFFFFFF, a.centre >> 32
conn.sendall(struct.pack('<8I', PHXI, 1, a.rate, fmt, lo, hi, 0, 0))
seq = 0
t0 = time.monotonic()
sent = 0
with open(a.file, 'rb') as f:
    while True:
        buf = f.read(a.frame * bps)
        n = len(buf) // bps
        if n == 0:
            break
        if a.meta_every and seq and seq % a.meta_every == 0:
            conn.sendall(struct.pack('<8I', META, a.rate, fmt, lo, hi, 59, 2, 0))
        conn.sendall(struct.pack('<4I', IQDQ, seq, n, 0) + buf[:n * bps])
        seq += 1
        sent += n
        if a.realtime:
            ahead = sent / a.rate - (time.monotonic() - t0)
            if ahead > 0:
                time.sleep(ahead)
conn.close()
print(f'sent {sent} samples in {seq} frames', flush=True)
