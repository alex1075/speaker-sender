# speaker-sender

Not affiliated with or endorsed by Apple Inc. AirPlay is a trademark of Apple Inc.; it is used here only to describe compatibility. This project plays audio only and does not handle FairPlay-protected content.

Send audio from a GrapheneOS/Android phone, or any Linux box, to AirPlay speakers without Apple hardware or software.

One repo, two products, one CI:

| Path | What | Status |
|---|---|---|
| `c/` | `speaker-send`, an AirPlay 1 and 2 command-line sender in plain C (Linux, macOS) | Working: verified on Harman Kardon Citation One, Sonos Bookshelf and HomePod |
| `android/` | Android app (Kotlin, AudioPlaybackCapture foreground service) | Scaffold, handshake port not done yet |
| `bridge/` | Bluetooth A2DP to AirPlay bridge (Pi receiver + pyatv sender), see below | Working: turntable to three speakers |
| `docs/` | Protocol notes | |

## Get the Linux/macOS tool

Download a release (static binaries for Linux x86_64/aarch64 and macOS arm64, plus the debug-signed Android APK and `SHA256SUMS`):

```sh
gh release download --repo alex1075/speaker-sender --pattern 'speaker-send-*-linux-x86_64.tar.gz'
tar xzf speaker-send-*-linux-x86_64.tar.gz && ./speaker-send-*/speaker-send --list
```

Or build from a clone (C11 compiler, `make`, OpenSSL headers):

```sh
git clone https://github.com/alex1075/speaker-sender.git && cd speaker-sender/c
sudo apt install build-essential libssl-dev    # macOS: brew install openssl@3
make && make check
```

Usage and protocol details: [`c/README.md`](c/README.md).

## Bluetooth bridge (`bridge/`)

For sources that cannot speak AirPlay at all, such as a Bluetooth turntable. Audio goes
source -> Bluetooth A2DP -> Raspberry Pi Zero W -> AirPlay speakers:

1. The Pi is an A2DP sink (BlueALSA) with an open pairing window only when asked (`bt-pair-mode`).
2. A second machine (a 4-core box; the Pi Zero is too slow for Python AirPlay) pulls the raw PCM over SSH with a capture-only key.
3. `bt_airplay_bridge.py` (Python, pyatv) streams it live to one or more speakers: `--target "Speaker1=30" --target "Speaker2=40"`.
   Sonos gets ALAC frames (`alac_patch.py`), the others raw PCM. Start of playback is triggered by real signal level, not idle hiss.

Verified with a Sony PS-LX310BT turntable playing on Harman Kardon, Sonos and HomePod at once. Setup, systemd units and the
byte-order, frame-alignment and Sonos findings are in [`docs/bridge-findings.md`](docs/bridge-findings.md). The C tool in `c/`
replaces the Python sender where a pipe of PCM is enough; the bridge is the Bluetooth-to-PCM part.

## Get the Android app

Download `speaker-sender-*-android-debug.apk` from the latest release and sideload it, or build it:

```sh
gradle -p android assembleDebug     # needs JDK 17 and Gradle >= 8.9; APK in android/app/build/outputs/apk/debug/
```

## Tested on

| Model | Format | Result |
|:--|:--:|:--|
| Harman Kardon Citation One | PCM | ✅ Works |
| Sonos Bookshelf | ALAC | ✅ Works |
| HomePod | PCM | ✅ Works |
| Yamaha RX-V679 | ALAC (AirPlay 1) | ✅ Works |

Quirks: the Harman Kardon needs the timing responder running before SETUP; Sonos needs volume and progress sent before RECORD/FLUSH.
Playing on all three at once was in sync by ear (click-track test); not yet measured with instruments.

## CI and releases

`.github/workflows/ci.yml` runs on every push: C tests on Linux x86_64, Linux arm64 and macOS, an ASan/UBSan run, a check that the golden
test vectors still match pyatv/srptools/cryptography, and the Android debug build.
Pushing a tag such as `v0.1.0` (`.github/workflows/release.yml`) builds everything and publishes one release.

## Plan
- **Done** - Stage 0/AirPlay 2 proven from Python and C (transient pairing, encrypted RTSP, ChaCha20-Poly1305, PCM and ALAC).
- **Done** - AirPlay 1 (RAOP) sender in `c/` with automatic AirPlay 1/2 detection (Yamaha RX-V679 verified).
- **Next** - port the handshake to Kotlin (reuse the golden vectors in JVM tests), then capture test with the Pixel.
- **Fallback** - Bluetooth A2DP into a Raspberry Pi, forwarded to speakers via the Python bridge.

## Known risks
- Apps can opt out of AudioPlaybackCapture (possibly Apple Music, DRM'd apps). Needs testing on the Pixel.
- No cross-room clock correction beyond shared sync packets; rooms can drift by tens of milliseconds.

## References
- https://pyatv.dev/ (MIT): the protocol sequence was worked out from its debug logs and source; golden test vectors are generated with it
- https://github.com/mikebrady/shairport-sync
- https://developer.android.com/media/platform/av-capture

## License

MIT, see [LICENSE](LICENSE).

## AI Disclosure

speaker-sender is designed, developed, and maintained by Alex Hunt. Anthropic's Claude AI was used as an assistant for parts of the implementation. All AI generated content was reviewed, edited and tested by the author on real AirPlay speakers (Harman Kardon, Sonos, HomePod) and against reference implementations (pyatv, srptools, cryptography). The author remains responsible for the submitted implementations and their correctness.
