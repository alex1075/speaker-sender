#!/usr/bin/env python3
"""Bluetooth (A2DP sink via BlueALSA) -> AirPlay bridge, built on pyatv.

A phone/turntable pairs with this Pi as a Bluetooth speaker. When audio starts, the bridge connects to the
chosen AirPlay speaker and streams the live PCM; when the audio has been silent for --idle seconds it
disconnects again and waits for the next session.

  python bt_airplay_bridge.py --target "office speaker" --volume 30

Notes
- pyatv ends a stream once its source returns empty frames, so LiveSource always returns a full packet
  (silence when nothing arrived) and only returns empty to finish the session.
- Latency is ~2 s (AirPlay buffering) plus whatever was buffered while connecting: fine for music, not for video.
"""
import argparse
import asyncio
import logging
import sys
import time

import pyatv
import pyatv.protocols.raop as raop
from pyatv.interface import MediaMetadata
from pyatv.protocols.raop.audio_source import AudioSource
from pyatv.storage.memory_storage import MemoryStorage

RATE, CHANNELS, SAMPLE_BYTES = 44100, 2, 2
FRAME_BYTES = CHANNELS * SAMPLE_BYTES
CAPTURE_CMD = ["arecord", "-q", "-D", "plug:bluealsa", "-t", "raw", "-f", "S16_LE", "-r", str(RATE), "-c", str(CHANNELS)]

log = logging.getLogger("bridge")


class LiveSource(AudioSource):
    """Raw PCM from a running capture process, shaped for pyatv's AirPlay sender."""

    def __init__(self, proc, first_chunk, max_buffer_s, idle_s):
        self._proc = proc
        self._buf = bytearray(first_chunk)
        self._max = int(RATE * max_buffer_s) * FRAME_BYTES
        self._idle_s = idle_s
        self._last_active = time.monotonic()
        self._eof = False
        self._task = asyncio.create_task(self._pump())

    async def _pump(self):
        try:
            while True:
                chunk = await self._proc.stdout.read(4096)
                if not chunk:
                    break
                if any(chunk):  # vinyl/BT noise is never all-zero; digital silence is
                    self._last_active = time.monotonic()
                self._buf += chunk
                if len(self._buf) > self._max:  # keep latency bounded: drop oldest, stay frame-aligned
                    drop = len(self._buf) - self._max
                    del self._buf[: drop - drop % FRAME_BYTES]
        finally:
            self._eof = True

    async def readframes(self, nframes):
        if self._eof and not self._buf:
            return self.NO_FRAMES
        if time.monotonic() - self._last_active > self._idle_s:
            return self.NO_FRAMES  # idle: end the session
        need = nframes * FRAME_BYTES
        data = bytes(self._buf[:need])
        del self._buf[:need]
        if len(data) < need:
            data += b"\0" * (need - len(data))  # underrun -> silence keeps the stream alive
        return data

    async def get_metadata(self):
        return MediaMetadata(title="Bluetooth audio", artist="BT bridge", album="", duration=0.0)

    async def close(self):
        self._task.cancel()
        if self._proc.returncode is None:
            self._proc.kill()
        await self._proc.wait()

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
        try:
            while True:
                chunk = await proc.stdout.read(4096)
                if not chunk:
                    break  # capture exited (no Bluetooth source connected yet)
                if any(chunk):
                    log.info("audio detected")
                    return proc, chunk
        except BaseException:
            proc.kill()
            raise
        await proc.wait()
        await asyncio.sleep(2.0)


async def stream_session(args, proc, first_chunk):
    loop = asyncio.get_running_loop()
    src = LiveSource(proc, first_chunk, args.max_buffer, args.idle)
    storage = MemoryStorage()
    await storage.load()
    try:
        confs = []
        for scan_s in (4, 8):  # 4 s finds everything on this network (measured); 8 s is the fallback
            confs = [d for d in await pyatv.scan(loop, timeout=scan_s, storage=storage) if d.name == args.target]
            if confs:
                break
        if not confs:
            log.warning("speaker %r not found on the network", args.target)
            return
        atv = await pyatv.connect(confs[0], loop, storage=storage)
        try:
            await atv.audio.set_volume(args.volume)
            log.info("streaming to %s", args.target)
            raop.open_source = lambda *_a, **_k: _return(src)  # hand pyatv our live source
            await atv.stream.stream_file("live", metadata=await src.get_metadata())
            log.info("session finished (idle or Bluetooth disconnected)")
        finally:
            atv.close()
    finally:
        await src.close()


async def _return(x):
    return x


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
    ap.add_argument("--target", required=True, help="AirPlay speaker name, exactly as shown by tools/scan.py")
    ap.add_argument("--volume", type=float, default=30.0, help="initial speaker volume 0-100 (default 30)")
    ap.add_argument("--idle", type=float, default=60.0, help="seconds of digital silence before disconnecting")
    ap.add_argument("--max-buffer", type=float, default=4.0, help="max seconds of captured audio held; covers the ~5-6 s AirPlay connect time (latency cap)")
    ap.add_argument("--capture-cmd", nargs="+", default=CAPTURE_CMD, help="command that writes S16_LE/44.1k/stereo raw PCM")
    ap.add_argument("--once", action="store_true", help="exit after one session (for testing)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO, format="%(asctime)s %(name)s: %(message)s")
    try:
        asyncio.run(main(a))
    except KeyboardInterrupt:
        sys.exit(0)
