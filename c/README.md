# airplay-send

A small command-line AirPlay 2 sender for Linux (and macOS), written in plain C. It plays audio on
AirPlay speakers (HomePod, Sonos, Harman Kardon, ...) from anything that can write PCM to a pipe -
no Apple hardware or software involved.

```sh
airplay-send --list                                   # find speakers
airplay-send -t Speaker1 song.wav                      # play a WAV file
arecord -f cd -t raw | airplay-send -t Speaker1        # live capture from a sound card / line-in
ffmpeg -i radio.mp3 -f s16le -ar 44100 -ac 2 - | airplay-send -t Speaker1 -t Speaker2=40
airplay-send --tone 3 -t 192.168.1.50                 # 3 s test tone, speaker by IP
```

`-t` can be repeated to play on several rooms at once; `NAME=40` sets that speaker's volume (1-100).
Names are matched case-insensitively against the mDNS name (unique prefix or substring is enough).

## Get it

**Download a release** (static binaries for Linux x86_64 / aarch64 and macOS arm64, with `SHA256SUMS`):

```sh
gh release download --repo alex1075/airplay-send --pattern 'airplay-send-*-linux-x86_64.tar.gz'
tar xzf airplay-send-*-linux-x86_64.tar.gz
./airplay-send-*/airplay-send --version
```

**Or build from a clone** - needs a C11 compiler, `make` and OpenSSL's libcrypto headers:

```sh
git clone https://github.com/alex1075/airplay-send.git && cd airplay-send
sudo apt install build-essential libssl-dev     # Fedora: sudo dnf install gcc make openssl-devel
make                                            # macOS: brew install openssl, then make
make check                                      # run all tests
sudo make install                               # optional, PREFIX=/usr/local by default
```

`make STATIC=1` links libcrypto statically, `make STATIC=full` builds a fully static executable (use musl, e.g. Alpine).

## Input

* stdin or `-`: raw signed 16-bit little-endian PCM, 44100 Hz, 2 channels (what `arecord -f cd -t raw` writes).
* A `.wav` file path (16-bit stereo 44.1 kHz). Anything else: convert with ffmpeg as shown above.
* `--raw FILE` reads a headerless PCM file, `--tone SECONDS` generates a 440 Hz test tone.

Live sources are paced by the clock; if the source stalls the stream continues with silence, and stale data that
piled up while the speakers were being set up is dropped. At end of input the speaker's buffer is played out (~1.5 s).

## How it works / limits

The sender follows what iOS does for an AirPlay 2 "audio" stream: transient HomeKit pairing (SRP-6a, PIN 3939,
no user interaction), ChaCha20-Poly1305 encrypted RTSP and event channels, a realtime RTP stream
(encrypted, 352 frames per packet) with sync packets and an NTP-style timing responder, and `/feedback` keep-alives.

* Sonos (and anything with `--alac NAME`) is fed ALAC frames; the frames carry the samples uncompressed,
  so no encoder is needed.
* 44.1 kHz / 16-bit / stereo only. No AAC, no resampling.
* Speakers that require a PIN or password (rather than transient pairing) are not supported (`--pin` exists for
  speakers that use a fixed PIN with the same protocol, untested).
* IPv4 only. Discovery is a simple mDNS query; if a speaker does not show up use `-t IP[:PORT]`.
* Playing to several speakers works, but there is no cross-device clock correction beyond the shared sync
  packets, so rooms can drift by tens of milliseconds.

The Python reference this was ported from and the protocol notes are in the
[airplay-sender](https://github.com/alex1075/airplay-sender) repository (`docs/protocol-notes.md`).

## Tests

`make check` runs

* `tests/test_unit` - golden vectors generated from pyatv, srptools and `cryptography` (`tools/gen_vectors.py`
  regenerates `tests/vectors.h`): SRP-6a proofs and session key, HKDF, ChaCha20-Poly1305, HAP framing, TLV8, binary
  plists, the uncompressed-ALAC bit packing, RTP audio, sync and timing packets, mDNS parsing, WAV input.
* `tests/test_mock` - a mock AirPlay 2 receiver written from the receiver side. The real sender pairs with it,
  sets the stream up (PCM and ALAC), plays a known signal and the mock decrypts and verifies every packet and
  checks sync, timing replies, event channel replies, volume, metadata and TEARDOWN.
* `tests/test_cli.sh` - command-line behaviour and an end-to-end pipeline run through the real binary.

GitHub Actions runs these on Linux x86_64, Linux arm64 and macOS, under AddressSanitizer/UBSan, and checks that the
committed vectors still match what the reference libraries produce. Pushing a tag `vX.Y.Z` builds the release binaries.
