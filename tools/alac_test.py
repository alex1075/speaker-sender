"""Send the test tone to a speaker as AirPlay 2 ALAC (for Sonos, which ignores raw PCM).
usage: python tools/alac_test.py "<speaker name>" [volume 0-100]
"""
import asyncio, os, sys, tempfile, pyatv
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "bridge"))
from stage0_test import make_tone
import alac_patch
from pyatv.storage.memory_storage import MemoryStorage

async def main(name, volume):
    alac_patch.install()
    alac_patch.set_alac(True)
    loop = asyncio.get_running_loop()
    storage = MemoryStorage(); await storage.load()
    conf = [d for d in await pyatv.scan(loop, timeout=6, storage=storage) if d.name == name][0]
    path = os.path.join(tempfile.mkdtemp(), "tone.wav"); make_tone(path, secs=8.0)
    atv = await pyatv.connect(conf, loop, storage=storage)
    try:
        await atv.audio.set_volume(volume)
        await atv.stream.stream_file(path)
        print("stream_file finished OK")
    finally:
        atv.close()

asyncio.run(main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 30))
