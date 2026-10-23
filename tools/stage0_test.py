"""Stage 0: can pyatv push an AirPlay 1 (RAOP) stream to a speaker?
usage: python tools/stage0_test.py "<speaker name>" [volume 0-100] [auto|1|1enc]
"""
import asyncio, math, struct, sys, wave, tempfile, os, pyatv
from pyatv.settings import AirPlayVersion
from pyatv.storage.memory_storage import MemoryStorage


def patch_ap1_encrypted():
    """AP1 (ANNOUNCE/SETUP/RECORD) over the HAP-encrypted control channel.
    pyatv's AP1 path does transient pair-verify but never enables encryption,
    which speakers with transient/system pairing (e.g. Harman Kardon) drop."""
    from pyatv.protocols.raop.protocols import airplayv1
    from pyatv.protocols.airplay.auth import verify_connection

    class Shim:
        def __init__(self, creds, conn):
            self.creds, self.conn = creds, conn
        async def verify_credentials(self):
            self.v = await verify_connection(self.creds, self.conn)
            return True

    airplayv1.pair_verify = lambda creds, conn: Shim(creds, conn)

def make_tone(path, secs=2.0, freq=440.0, amp=0.25, rate=44100):
    with wave.open(path, "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
        n = int(secs * rate)
        for i in range(n):
            fade = min(1.0, i / 2000, (n - i) / 2000)
            s = int(32767 * amp * fade * math.sin(2 * math.pi * freq * i / rate))
            w.writeframes(struct.pack("<hh", s, s))

async def main(name, volume, version):
    loop = asyncio.get_running_loop()
    storage = MemoryStorage()
    await storage.load()
    devs = [d for d in await pyatv.scan(loop, timeout=8, storage=storage) if d.name == name]
    if not devs:
        sys.exit(f"{name!r} not found")
    conf = devs[0]
    print("services:", [(s.protocol.name, s.port, s.pairing.name) for s in conf.services])
    st = await storage.get_settings(conf)
    if version == '1enc':
        patch_ap1_encrypted(); version = '1'
    st.protocols.raop.protocol_version = AirPlayVersion(version)
    print('raop protocol_version =', st.protocols.raop.protocol_version.value)
    atv = await pyatv.connect(conf, loop, storage=storage)
    try:
        await atv.audio.set_volume(volume)
        path = os.path.join(tempfile.gettempdir(), "tone.wav")
        make_tone(path)
        await atv.stream.stream_file(path)
        print("stream_file finished OK")
    finally:
        atv.close()

asyncio.run(main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 20.0, sys.argv[3] if len(sys.argv) > 3 else 'auto'))
