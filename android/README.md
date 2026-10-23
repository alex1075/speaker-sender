# Android sender (project set up, handshake not ported yet)

The Gradle project builds in CI (`gradle -p android assembleDebug`, JDK 17, Gradle 8.9) and has JUnit unit tests
(`gradle -p android testDebugUnitTest`). The app starts with a placeholder screen; the sender itself is not written, and
nothing has run on a phone yet (the Pixel hasn't arrived). Protocol details: `../docs/protocol-notes.md`; the working
reference implementation is the C tool in `../c/`.

What exists
- `MainActivity.kt` - placeholder launcher screen.
- `Protocol.kt` - pure helpers (feature parsing, AirPlay 1/2 detection, volume mapping) with `ProtocolTest.kt`, using the same
  values as the C tests.
- `AirPlayDiscovery.kt` - NsdManager browse of `_raop._tcp` (resolve is the older API; migrate to `registerServiceInfoCallback` on API 34+).
- `CaptureService.kt` - foreground service: MediaProjection + AudioPlaybackCapture -> 44.1 kHz stereo PCM in 352-frame chunks.
- `AirPlaySender.kt` - the sender interface, with the handshake as numbered TODOs.

What is missing (the real work)
0. AirPlay 1 (RAOP) for older receivers, as in `c/`, if wanted.
1. HAP transient pairing: SRP-6a (3072-bit, SHA-512), TLV8, HKDF-SHA512, ChaCha20-Poly1305 framing (Bouncy Castle or Tink).
2. Encrypted RTSP client + the two SETUP plists + event channel.
3. RTP/UDP sender with timing + sync packets.
4. UI: pick a speaker, start/stop, volume.

Known risk: apps can opt out of AudioPlaybackCapture; test Apple Music first when the Pixel arrives.
Fallback that works for every app: the Bluetooth-to-AirPlay bridge in `../bridge/`.
