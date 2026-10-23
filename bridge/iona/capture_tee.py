#!/usr/bin/env python3
"""Capture command for the Bluetooth bridge that also feeds an AirPlay 1 receiver.

Runs capture-from-pi.sh and copies its raw S16_LE/44.1k/stereo output to stdout (for bt_airplay_bridge.py, which
sends it to the AirPlay 2 speakers through pyatv) and to `airplay-send` (the C tool, which detects AirPlay 1),
for speakers that pyatv's AirPlay 2 path cannot use. The copy never blocks the bridge: if airplay-send is slow or
dies, whole 4096-byte blocks (a multiple of the 4-byte stereo frame) are dropped for it and the bridge carries on.
Usage: capture_tee.py "Living Room=30" ["Other=40" ...]   (extra targets use airplay-send's -t syntax)
"""
import os
import signal
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BLOCK = 4096  # <= PIPE_BUF, so a non-blocking pipe write is all or nothing; multiple of 4 keeps frames aligned
MAX_PENDING = 64 * BLOCK
targets = sys.argv[1:]
procs = []


def stop(*_):
    for p in procs:
        if p.poll() is None:
            p.terminate()
    sys.exit(0)


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)

cap = subprocess.Popen([os.path.join(HERE, "capture-from-pi.sh")], stdout=subprocess.PIPE)
procs.append(cap)
sender = None
sfd = -1
if targets:
    cmd = [os.path.join(HERE, "airplay-send"), "--scan", "6000"]
    for t in targets:
        cmd += ["-t", t]
    log = open("/tmp/airplay-send-extra.log", "ab")
    sender = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=log, stderr=log)
    procs.append(sender)
    sfd = sender.stdin.fileno()
    os.set_blocking(sfd, False)

pending = b""
try:
    while True:
        chunk = os.read(cap.stdout.fileno(), 65536)
        if not chunk:
            break
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        if sender is None:
            continue
        if sender.poll() is not None:
            sender = None
            continue
        pending += chunk
        while len(pending) >= BLOCK:
            try:
                os.write(sfd, pending[:BLOCK])
            except BlockingIOError:
                break
            except OSError:
                sender = None
                break
            pending = pending[BLOCK:]
        if len(pending) > MAX_PENDING:
            # airplay-send is not keeping up: drop the oldest audio, starting on a stereo frame boundary
            start = (len(pending) - MAX_PENDING // 2 + 3) // 4 * 4
            pending = pending[start:]
finally:
    stop()
