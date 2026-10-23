"""Probe which AirPlay 2 realtime audio-stream SETUP bodies a speaker accepts.
Aborts right after the SETUP reply, so no audio is ever sent.
usage: python tools/sweep_setup.py "<speaker name>"
"""
import asyncio, os, sys, tempfile, wave
import pyatv, pyatv.support.http as h
from pyatv.protocols.raop.protocols import airplayv2 as a2
from pyatv.settings import AirPlayVersion
from pyatv.storage.memory_storage import MemoryStorage

# the speaker answers a rejected SETUP slowly (>10 s), so widen pyatv's timeout
_orig = h.HttpConnection.send_and_receive
async def _patched(self, *a, **k):
    k["timeout"] = 40
    return await _orig(self, *a, **k)
h.HttpConnection.send_and_receive = _patched

class Done(Exception):
    pass

VARIANTS = {
    "baseline PCM ct=1 0x800":      ({}, []),
    "ALAC ct=2 0x40000":            ({"ct": 2, "audioFormat": 0x40000}, []),
    "AAC-LC ct=4 0x400000 spf1024": ({"ct": 4, "audioFormat": 0x400000, "spf": 1024}, []),
    "PCM, no latencyMin/Max":       ({}, ["latencyMin", "latencyMax"]),
    "PCM, minimal keys":            ({}, ["isMedia", "audioMode", "supportsDynamicStreamID", "latencyMin", "latencyMax"]),
}
CURRENT = {}

async def setup_audio_stream(self, control_client_port):
    out_key, _ = self._verifier.encryption_keys(a2.EVENTS_SALT, a2.EVENTS_WRITE_INFO, a2.EVENTS_READ_INFO)
    s = {"audioFormat": 0x800, "audioMode": "default", "controlPort": control_client_port, "ct": 1,
         "isMedia": True, "latencyMax": 88200, "latencyMin": 11025, "shk": out_key[0:32], "spf": 352,
         "sr": 44100, "type": 0x60, "supportsDynamicStreamID": False,
         "streamConnectionID": self.rtsp.session_id}
    over, drop = CURRENT["v"]
    s.update(over)
    for k in drop:
        s.pop(k, None)
    try:
        r = await self.rtsp.setup(body={"streams": [s]})
        print("   ACCEPTED:", a2.decode_bplist_from_body(r))
    except Exception as e:
        print("   rejected:", type(e).__name__, str(e)[:100])
    raise Done()
a2.AirPlayV2.setup_audio_stream = setup_audio_stream

async def main(name):
    loop = asyncio.get_running_loop()
    path = os.path.join(tempfile.gettempdir(), "silence.wav")
    with wave.open(path, "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
        w.writeframes(b"\0" * 4 * 4410)
    for label, v in VARIANTS.items():
        print("variant:", label, flush=True)
        CURRENT["v"] = v
        storage = MemoryStorage(); await storage.load()
        devs = [d for d in await pyatv.scan(loop, timeout=8, storage=storage) if d.name == name]
        if not devs:
            sys.exit(f"{name!r} not found")
        st = await storage.get_settings(devs[0])
        st.protocols.raop.protocol_version = AirPlayVersion.Auto
        atv = await pyatv.connect(devs[0], loop, storage=storage)
        try:
            await atv.stream.stream_file(path)
        except Done:
            pass
        except Exception as e:
            print("   (post-setup)", type(e).__name__, str(e)[:80])
        finally:
            atv.close()
        await asyncio.sleep(2)

asyncio.run(main(sys.argv[1]))
