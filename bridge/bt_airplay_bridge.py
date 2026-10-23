#!/usr/bin/env python3
"""Bluetooth (A2DP sink via BlueALSA) -> AirPlay bridge, built on pyatv.

A phone/turntable pairs with this Pi as a Bluetooth speaker. When audio starts, the bridge connects to the
chosen AirPlay speaker and streams the live PCM; when the audio has been silent for --idle seconds it
disconnects again and waits for the next session.

  python bt_airplay_bridge.py --target "Speaker1" --volume 30
  python bt_airplay_bridge.py --target "Speaker2=25" --target "Speaker1=30"   # several speakers

Notes
- pyatv ends a stream once its source returns empty frames, so LiveSource always returns a full packet
  (silence when nothing arrived) and only returns empty to finish the session.
- Latency is ~2 s (AirPlay buffering) plus whatever was buffered while connecting: fine for music, not for video.
"""
import argparse
import array
import asyncio
import logging
import signal
import sys
import time

import pyatv
import pyatv.protocols.raop as raop
from pyatv.const import Protocol
from pyatv.interface import MediaMetadata
from pyatv.protocols.raop.audio_source import AudioSource
from pyatv.storage.memory_storage import MemoryStorage

import alac_patch

RATE, CHANNELS, SAMPLE_BYTES = 44100, 2, 2
FRAME_BYTES = CHANNELS * SAMPLE_BYTES
CAPTURE_CMD = ["arecord", "-q", "-D", "plug:bluealsa", "-t", "raw", "-f", "S16_LE", "-r", str(RATE), "-c", str(CHANNELS)]

log = logging.getLogger("bridge")

ACTIVE_PEAK = 150  # idle Bluetooth hiss peaks around 80 (of 32767); music is far above this


def is_active(chunk):
    """True if the chunk holds real audio rather than digital silence or idle hiss."""
    n = len(chunk) - len(chunk) % 2
    if not n:
        return False
    samples = array.array("h")
    samples.frombytes(bytes(chunk[:n]))
    return max(max(samples), -min(samples)) > ACTIVE_PEAK


class LiveSource(AudioSource):
    """Raw PCM fed from the shared capture, shaped for pyatv's AirPlay sender (one instance per speaker)."""

    def __init__(self, first_chunk, max_buffer_s, idle_s, join_s):
        self._buf = bytearray(first_chunk)
        self._max = int(RATE * max_buffer_s) * FRAME_BYTES
        self._join = int(RATE * join_s) * FRAME_BYTES
        self.started = asyncio.Event()  # set on the first read, i.e. once pyatv has finished its setup
        self._idle_s = idle_s
        self._last_active = time.monotonic()
        self._eof = False

    def feed(self, chunk):
        if is_active(chunk):
            self._last_active = time.monotonic()
        self._buf += chunk
        if len(self._buf) > self._max:  # keep latency bounded: drop oldest, stay frame-aligned
            drop = len(self._buf) - self._max
            del self._buf[: drop - drop % FRAME_BYTES]

    def finish(self):
        self._eof = True

    async def readframes(self, nframes):
        if not self.started.is_set():
            # Join the live stream: drop the backlog built up while connecting so that every speaker
            # plays the same moment of audio (otherwise a speaker that connects later lags by seconds).
            excess = len(self._buf) - self._join
            if excess > 0:
                del self._buf[: excess - excess % FRAME_BYTES]
            self.started.set()
        if self._eof and not self._buf:
            return self.NO_FRAMES
        if time.monotonic() - self._last_active > self._idle_s:
            return self.NO_FRAMES  # idle: end the session
        need = nframes * FRAME_BYTES
        # Only hand out whole frames. A partial frame left over after an underrun would otherwise be
        # padded with zeros, shifting every later sample by a few bytes (permanent static).
        avail = len(self._buf) - len(self._buf) % FRAME_BYTES
        take = min(need, avail)
        data = bytes(self._buf[:take])
        del self._buf[:take]
        if len(data) < need:
            data += b"\0" * (need - len(data))  # underrun -> silence keeps the stream alive
        # AirPlay raw PCM is big-endian (network order); arecord gives little-endian. pyatv's own
        # file source byteswaps for the same reason. Skipping this plays as loud static.
        samples = array.array("h")
        samples.frombytes(data)
        if sys.byteorder == "little":
            samples.byteswap()
        return samples.tobytes()

    async def get_metadata(self):
        return MediaMetadata(title="Bluetooth audio", artist="BT bridge", album="", duration=0.0)

    async def close(self):
        pass

    @property
    def sample_rate(self):
        return RATE

    @property
    def channels(self):
        return CHANNELS

    @property
    def sample_size(self):
        return SAMPLE_BYTES

    @property
    def duration(self):
        return 0


async def wait_for_audio(args):
    """Run the capture command until real (non-zero) audio arrives; returns (proc, first_chunk)."""
    while True:
        proc = await asyncio.create_subprocess_exec(
            *args.capture_cmd, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL
        )
        skipped = 0  # bytes read and thrown away; reads can end mid-frame
        try:
            while True:
                chunk = await proc.stdout.read(4096)
                if not chunk:
                    break  # capture exited (no Bluetooth source connected yet)
                if is_active(chunk):
                    # Resume on a frame boundary, otherwise every sample is shifted a byte (loud static).
                    chunk = chunk[(-skipped) % FRAME_BYTES:]
                    log.info("audio detected")
                    return proc, chunk
                skipped += len(chunk)
        except BaseException:
            proc.kill()
            raise
        await proc.wait()
        await asyncio.sleep(2.0)


async def tee(proc, sources):
    """Read the capture once and give every speaker its own copy."""
    try:
        while True:
            chunk = await proc.stdout.read(4096)
            if not chunk:
                break
            for src in sources:
                src.feed(chunk)
    finally:
        for src in sources:
            src.finish()


async def find_speakers(loop, storage, names):
    found = {}
    for scan_s in (4, 8):  # 4 s finds everything on this network (measured); 8 s is the fallback
        for d in await pyatv.scan(loop, timeout=scan_s, storage=storage):
            if d.name in names:
                found[d.name] = d
        if len(found) == len(names):
            break
    for name in names:
        if name not in found:
            log.warning("speaker %r not found on the network", name)
    return found


async def stream_session(args, proc, first_chunk):
    loop = asyncio.get_running_loop()
    sources = {name: LiveSource(first_chunk, args.max_buffer, args.idle, args.join_buffer) for name, _vol in args.targets}
    by_file = {f"live:{name}": src for name, src in sources.items()}
    tee_task = asyncio.create_task(tee(proc, list(sources.values())))  # start draining the capture at once

    async def open_source(file, rate, channels, sample_size, *_a, **_k):
        if (rate, channels, sample_size) != (RATE, CHANNELS, SAMPLE_BYTES):
            log.warning("%s wants %s Hz/%s ch/%s B, not %s/%s/%s: audio will be wrong",
                        file, rate, channels, sample_size, RATE, CHANNELS, SAMPLE_BYTES)
        return by_file[file]

    setup_lock = asyncio.Lock()  # SRP pairing is heavy for a Pi Zero: connect one speaker at a time

    async def run_one(name, volume, conf, storage):
        async with setup_lock:
            task = asyncio.create_task(stream_one(name, volume, conf, storage))
            started = asyncio.create_task(sources[name].started.wait())
            await asyncio.wait({task, started}, timeout=45, return_when=asyncio.FIRST_COMPLETED)
            started.cancel()
        await task

    async def stream_one(name, volume, conf, storage):
        try:
            svc = conf.get_service(Protocol.AirPlay)
            maker = (svc.properties.get("manufacturer") or "") if svc else ""
            use_alac = name in args.alac or maker.lower() == "sonos"  # Sonos plays silence for raw PCM (pyatv#2915)
            alac_patch.set_alac(use_alac)  # per task; read when this session's stream is set up
            if use_alac:
                log.info("%s: using ALAC (%s)", name, maker or "forced")
            atv = await pyatv.connect(conf, loop, storage=storage)
            try:
                await atv.audio.set_volume(volume)
                log.info("streaming to %s (volume %g)", name, volume)
                await atv.stream.stream_file(f"live:{name}", metadata=await sources[name].get_metadata())
                log.info("%s: session finished (idle or Bluetooth disconnected)", name)
            finally:
                atv.close()
        except Exception as exc:  # one speaker failing must not stop the others
            log.error("%s: %s: %s", name, type(exc).__name__, exc)

    alac_patch.install()
    storage = MemoryStorage()
    await storage.load()
    try:
        found = await find_speakers(loop, storage, [n for n, _v in args.targets])
        if not found:
            return
        raop.open_source = open_source  # hand pyatv our live sources
        await asyncio.gather(*(run_one(n, v, found[n], storage) for n, v in args.targets if n in found))
    finally:
        tee_task.cancel()
        if proc.returncode is None:
            proc.kill()
        await proc.wait()


async def main(args):
    while True:
        proc, first = await wait_for_audio(args)
        try:
            await stream_session(args, proc, first)
        except Exception as exc:  # keep the service alive across network hiccups
            log.error("session failed: %s: %s", type(exc).__name__, exc)
            await asyncio.sleep(5.0)
        if args.once:
            return


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", required=True, action="append",
                    help='AirPlay speaker name, exactly as shown by tools/scan.py; repeat for several speakers; '
                         'append =N for a per-speaker volume, e.g. --target "Speaker2=25"')
    ap.add_argument("--alac", action="append", default=[], metavar="NAME",
                    help="force AirPlay 2 ALAC for this speaker (repeatable); Sonos speakers use it automatically")
    ap.add_argument("--volume", type=float, default=30.0, help="default speaker volume 0-100 (default 30)")
    ap.add_argument("--idle", type=float, default=60.0, help="seconds of digital silence before disconnecting")
    ap.add_argument("--max-buffer", type=float, default=4.0, help="max seconds of captured audio held; covers the ~5-6 s AirPlay connect time (latency cap)")
    ap.add_argument("--join-buffer", type=float, default=1.5, help="seconds of audio kept when a speaker starts playing; every speaker joins the live stream with this cushion (default 1.5)")
    ap.add_argument("--capture-cmd", nargs="+", default=CAPTURE_CMD, help="command that writes S16_LE/44.1k/stereo raw PCM")
    ap.add_argument("--once", action="store_true", help="exit after one session (for testing)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    a.targets = []
    for t in a.target:
        name, _, vol = t.rpartition("=")
        try:
            a.targets.append((name, float(vol)) if name else (t, a.volume))
        except ValueError:  # the "=" belongs to the name
            a.targets.append((t, a.volume))
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO, format="%(asctime)s %(name)s: %(message)s")
    async def runner():
        # SIGTERM/SIGINT cancel the main task so the finally blocks kill the capture child; otherwise an orphaned
        # capture (e.g. the ssh to the Pi) keeps the audio channel busy and the restarted bridge hears nothing.
        loop = asyncio.get_running_loop()
        task = asyncio.current_task()
        for sig in (signal.SIGTERM, signal.SIGINT):
            loop.add_signal_handler(sig, task.cancel)
        await main(a)

    try:
        asyncio.run(runner())
    except (KeyboardInterrupt, asyncio.CancelledError):
        sys.exit(0)
