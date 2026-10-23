# airplay-sender

Send audio from a GrapheneOS/Android phone, or any Linux box, to AirPlay speakers without Apple hardware or software.

One repo, two products, one CI:

| Path | What | Status |
|---|---|---|
| `c/` | `airplay-send`, an AirPlay 2 command-line sender in plain C (Linux, macOS) | Working: office Harman Kardon, Kitchen Sonos and Bedroom HomePod verified |
| `android/` | Android app (Kotlin, AudioPlaybackCapture foreground service) | Scaffold, handshake port not done yet |
| `bridge/` | Python bridge (pyatv) used for the Pi/turntable setup | Working |
| `docs/` | Protocol notes | |

## Get the Linux/macOS tool

Download a release (static binaries for Linux x86_64/aarch64 and macOS arm64, plus the debug-signed Android APK and `SHA256SUMS`):

```sh
gh release download --repo alex1075/airplay-sender --pattern 'airplay-send-*-linux-x86_64.tar.gz'
tar xzf airplay-send-*-linux-x86_64.tar.gz && ./airplay-send-*/airplay-send --list
```

Or build from a clone (C11 compiler, `make`, OpenSSL headers):

```sh
git clone https://github.com/alex1075/airplay-sender.git && cd airplay-sender/c
sudo apt install build-essential libssl-dev    # macOS: brew install openssl@3
make && make check
```

Usage and protocol details: [`c/README.md`](c/README.md).

## Get the Android app

Download `airplay-sender-*-android-debug.apk` from the latest release and sideload it, or build it:

```sh
gradle -p android assembleDebug     # needs JDK 17 and Gradle >= 8.9; APK in android/app/build/outputs/apk/debug/
```

## Tested speakers

| Speaker | Model | Format | Result |
|---|---|---|---|
| Office | Harman Kardon Citation One | PCM | Audible. Needs the timing responder running before SETUP |
| Kitchen | Sonos Bookshelf (Sonos Era-class, auto-detected) | ALAC | Audible. Needs volume and progress sent before RECORD/FLUSH |
| Bedroom | Apple HomePod (`AudioAccessory1,1`) | PCM | Audible (6 s tone at 50%) |
| Living Room | Yamaha RX-V679 receiver | n/a | Not tested |

Office, Kitchen and Bedroom also played together from the Python bridge; multi-room sync quality from the C tool is not yet measured.

## CI and releases

`.github/workflows/ci.yml` runs on every push: C tests on Linux x86_64, Linux arm64 and macOS, an ASan/UBSan run, a check that the golden
test vectors still match pyatv/srptools/cryptography, and the Android debug build.
Pushing a tag such as `v0.1.0` (`.github/workflows/release.yml`) builds everything and publishes one release.

## Plan
- **Done** - Stage 0/AirPlay 2 proven from Python and C (transient pairing, encrypted RTSP, ChaCha20-Poly1305, PCM and ALAC).
- **Next** - port the handshake to Kotlin (reuse the golden vectors in JVM tests), then capture test with the Pixel.
- **Fallback** - Bluetooth A2DP into a Raspberry Pi, forwarded to speakers via the Python bridge.

## Known risks
- Apps can opt out of AudioPlaybackCapture (possibly Apple Music, DRM'd apps). Needs testing on the Pixel.
- No cross-room clock correction beyond shared sync packets; rooms can drift by tens of milliseconds.

## References
- https://pyatv.dev/
- https://github.com/mikebrady/shairport-sync
- https://developer.android.com/media/platform/av-capture

## AI Disclosure

airplay-sender is designed, developed, and maintained by Alex Hunt. Anthropic's Claude AI was used as an assistant for parts of the implementation. All design decisions, protocol choices and the overall architecture are the author's own work. All AI generated content was reviewed, edited and tested by the author on real AirPlay speakers (Harman Kardon, Sonos, HomePod) and against reference implementations (pyatv, srptools, cryptography). The author remains responsible for the submitted implementations and their correctness.
