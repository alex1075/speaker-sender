# Android sender (protocol ported, app not wired up yet)

The Gradle project builds in CI (`gradle -p android assembleDebug`, JDK 17, Gradle 8.9) and has JUnit unit tests
(`gradle -p android testDebugUnitTest`). The AirPlay 2 protocol stack has been ported from the C tool in `../c/` and is
tested against the same golden vectors and a mock receiver. The app screen is still a placeholder, and nothing has run on
a phone or against a real speaker yet. Protocol details: `../docs/protocol-notes.md`.

What exists
- `Tlv8.kt`, `Bplist.kt`, `Crypto.kt`, `Srp.kt` - TLV8, binary plists, HKDF-SHA512, ChaCha20-Poly1305 and HAP framing, SRP-6a Pair-Setup.
- `Rtsp.kt` - RTSP client over TCP, plain and HAP-encrypted.
- `AirPlaySession.kt` - AirPlay 2 pairing, both SETUPs, event channel, timing and sync, retransmits, paced packet sending.
- `Alac.kt`, `Packets.kt`, `Streamer.kt` - uncompressed ALAC framing, RTP/sync/timing packets, paced streaming from a PCM stream.
- `Protocol.kt` - feature parsing, AirPlay 1/2 detection, volume mapping.
- `AirPlayDiscovery.kt` - NsdManager browse of `_raop._tcp` (resolve is the older API; migrate to `registerServiceInfoCallback` on API 34+).
- `CaptureService.kt` - foreground service: MediaProjection + AudioPlaybackCapture -> 44.1 kHz stereo PCM in 352-frame chunks.

What is missing
1. UI: pick a speaker, start/stop, volume (`MainActivity.kt` is a placeholder).
2. Connect `CaptureService` to `AirPlaySession`/`Streamer`; `AirPlaySender.kt` is an unused stub interface and can go.
3. AirPlay 1 (RAOP) for older receivers, as in `c/`, if wanted.
4. A test on a phone against real speakers.

Known risk: apps can opt out of AudioPlaybackCapture; test Apple Music first.
Fallback that works for every app: the Bluetooth-to-AirPlay bridge in `../bridge/`.
