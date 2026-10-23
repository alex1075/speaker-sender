"""AirPlay 2 ALAC mode for pyatv, for receivers that ignore raw PCM (Sonos).

pyatv's AirPlay 2 path sets the stream up as raw PCM (ct 1, audioFormat 0x800). Sonos accepts that, shows "playing", and
plays silence (postlund/pyatv#2915). Setting it up as ALAC (ct 2, audioFormat 0x40000) and sending each packet as an
*uncompressed* ALAC frame makes Sonos play. This module patches pyatv's AirPlayV2 accordingly, but only for sessions started
inside `alac_enabled()`, so other speakers keep the stock PCM path.

Uncompressed ALAC frame (bit-packed, big-endian), 352 stereo 16-bit frames:
  3 bits element tag (CPE = 1), 4 bits instance tag (0), 12 bits unused (0), 1 bit has-size (0),
  2 bits unused (0), 1 bit is-not-compressed (1), then L,R samples interleaved big-endian, then end tag (7, 3 bits),
  padded to a byte boundary.  23 header bits mean the samples are not byte aligned, hence the big-integer shifts.
"""
import contextlib
import contextvars

from pyatv.protocols.raop.protocols import airplayv2

_USE_ALAC = contextvars.ContextVar("airplay_alac", default=False)
_installed = False

_HEADER = (0b001 << 20) | 1  # 23 bits: tag 001, instance 0, unused 0, no size, shift 0, not compressed


def alac_wrap(pcm_be: bytes) -> bytes:
    """Wrap big-endian interleaved S16 stereo PCM in one uncompressed ALAC frame."""
    nbits = len(pcm_be) * 8
    value = (_HEADER << nbits) | int.from_bytes(pcm_be, "big")
    value = (value << 3) | 0b111  # end tag
    total = 23 + nbits + 3
    pad = (-total) % 8
    return (value << pad).to_bytes((total + pad) // 8, "big")


def alac_unwrap(frame: bytes, nbytes: int) -> bytes:
    """Inverse of alac_wrap (for tests)."""
    total = len(frame) * 8
    value = int.from_bytes(frame, "big")
    pad = total - (23 + nbytes * 8 + 3)
    value >>= pad
    assert value & 0b111 == 0b111, "missing end tag"
    value >>= 3
    assert value >> (nbytes * 8) == _HEADER, "bad header"
    return (value & ((1 << (nbytes * 8)) - 1)).to_bytes(nbytes, "big")


@contextlib.contextmanager
def alac_enabled(enabled=True):
    token = _USE_ALAC.set(bool(enabled))
    try:
        yield
    finally:
        _USE_ALAC.reset(token)


def set_alac(enabled=True):
    """Enable ALAC for the current asyncio task (use inside the task that calls stream_file)."""
    _USE_ALAC.set(bool(enabled))


def install():
    global _installed
    if _installed:
        return
    _installed = True
    cls = airplayv2.AirPlayV2
    orig_setup = cls.setup_audio_stream
    orig_send = cls.send_audio_packet

    async def setup_audio_stream(self, control_client_port):
        self._alac = _USE_ALAC.get()
        if not self._alac:
            return await orig_setup(self, control_client_port)
        # Same as pyatv's setup_audio_stream but ct=2 / audioFormat=0x40000 (ALAC 44.1 kHz 16-bit stereo)
        real_setup = self.rtsp.setup

        async def setup(body=None, **kw):
            for stream in (body or {}).get("streams", []):
                stream["ct"] = 2
                stream["audioFormat"] = 0x40000
            return await real_setup(body=body, **kw)

        self.rtsp.setup = setup
        try:
            return await orig_setup(self, control_client_port)
        finally:
            self.rtsp.setup = real_setup

    async def send_audio_packet(self, transport, rtp_header, audio):
        if getattr(self, "_alac", False):
            audio = alac_wrap(audio)
        return await orig_send(self, transport, rtp_header, audio)

    cls.setup_audio_stream = setup_audio_stream
    cls.send_audio_packet = send_audio_packet
