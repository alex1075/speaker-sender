"""Stand-in for the Bluetooth capture: raw S16_LE/44.1k/stereo sine at real-time pace. usage: fake_capture.py [seconds]"""
import math, struct, sys, time
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
rate, chunk = 44100, 1024
out = sys.stdout.buffer
n = 0
t0 = time.monotonic()
while n < secs * rate:
    out.write(b"".join(struct.pack("<hh", s, s) for s in (int(6500 * math.sin(2 * math.pi * 330 * (n + i) / rate)) for i in range(chunk))))
    out.flush()
    n += chunk
    delay = t0 + n / rate - time.monotonic()
    if delay > 0:
        time.sleep(delay)
