# Android sender (scaffold - NOT built or tested)

Written without an Android toolchain or a phone to run it on (the Pixel hasn't arrived). Treat it as a starting
point: nothing here has been compiled. Protocol details: `../docs/protocol-notes.md`.

What exists
- `AirPlayDiscovery.kt` - NsdManager browse of `_raop._tcp` (resolve is the older API; migrate to `registerServiceInfoCallback` on API 34+).
- `CaptureService.kt` - foreground service: MediaProjection + AudioPlaybackCapture -> 44.1 kHz stereo PCM in 352-frame chunks.
- `AirPlaySender.kt` - the sender interface, with the handshake as numbered TODOs.

What is missing (the real work)
1. HAP transient pairing: SRP-6a (3072-bit, SHA-512), TLV8, HKDF-SHA512, ChaCha20-Poly1305 framing (Bouncy Castle or Tink).
2. Encrypted RTSP client + the two SETUP plists + event channel.
3. RTP/UDP sender with timing + sync packets.
4. UI: pick a speaker, start/stop, volume.

Known risk: apps can opt out of AudioPlaybackCapture; test Apple Music first when the Pixel arrives.
Fallback that works for every app: the Bluetooth-to-AirPlay bridge in `../bridge/`.
