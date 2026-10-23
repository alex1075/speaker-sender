# airplay-sender

Send audio from a GrapheneOS/Android phone to AirPlay speakers, without Apple hardware or Google services.

## Plan
- **Stage 0** - prove the speakers accept AirPlay 1 (RAOP) streams (pyatv from a laptop).
- **Stage 1** - Android AirPlay 1 sender: NsdManager discovery of `_raop._tcp`, AudioPlaybackCapture in a foreground service, RTSP handshake, RTP/ALAC stream, RTCP sync.
- **Stage 2** - AirPlay 2: HAP pairing, ChaCha20-Poly1305, buffered audio, PTP timing.
- **Fallback** - Bluetooth A2DP into a Raspberry Pi, forwarded to speakers via PipeWire `module-raop-discover`.

## Known risks
- Apps can opt out of AudioPlaybackCapture (possibly Apple Music, DRM'd apps). Needs testing.
- AirPlay 2-only speakers may reject legacy streams.

## References
- https://pyatv.dev/
- https://github.com/mikebrady/shairport-sync
- https://developer.android.com/media/platform/av-capture
