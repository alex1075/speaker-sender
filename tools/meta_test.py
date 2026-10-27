"""Send the test tone with FULL metadata (title/artist/album/artwork/duration) to see if a speaker needs it to render audio.
usage: python tools/meta_test.py "<speaker name>" [volume 0-100]
"""
import asyncio, os, struct, sys, tempfile, zlib, pyatv
sys.path.insert(0, os.path.dirname(__file__))
from stage0_test import make_tone
from pyatv.interface import MediaMetadata
from pyatv.storage.memory_storage import MemoryStorage


def tiny_png(size=64):
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    rows = b"".join(
        b"\x00" + b"".join(bytes([(x * 4) % 256, 80, (y * 4) % 256]) for x in range(size)) for y in range(size)
    )
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


async def main(name, volume):
    loop = asyncio.get_running_loop()
    storage = MemoryStorage(); await storage.load()
    conf = [d for d in await pyatv.scan(loop, timeout=6, storage=storage) if d.name == name][0]
    path = os.path.join(tempfile.mkdtemp(), "tone.wav"); make_tone(path, secs=8.0)
    atv = await pyatv.connect(conf, loop, storage=storage)
    try:
        await atv.audio.set_volume(volume)
        md = MediaMetadata(title="Test Tone", artist="speaker-sender", album="Diagnostics", artwork=tiny_png(), duration=8.0)
        await atv.stream.stream_file(path, metadata=md)
        print("stream_file finished OK")
    finally:
        atv.close()

asyncio.run(main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 30))
