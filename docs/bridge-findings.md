# Bluetooth -> AirPlay bridge: findings (2026-10-23)

Sony PS-LX310BT turntable -> Raspberry Pi Zero W (BlueALSA A2DP sink) -> pyatv -> Harman Kardon Citation One
("office speaker"). Confirmed by ear: clear music end to end.

## Bugs that produced "static" (all three were mine)
1. **Byte order.** AirPlay raw PCM is big-endian. `arecord` gives little-endian. pyatv's own file source calls
   `array.byteswap()` (`_to_audio_samples` in `pyatv/protocols/raop/audio_source.py`) although the `readframes` docstring says
   "little endian". A custom `AudioSource` must do the same swap, otherwise every sample is byte-reversed: loud static, and a
   test sine sounds like "buzzing". Fix: byteswap in `LiveSource.readframes`.
2. **Frame alignment after discarding startup bytes.** Pipe reads return arbitrary lengths; bytes thrown away while waiting for
   audio must be tracked and the first real chunk trimmed to a 4-byte (stereo S16) boundary.
3. **Frame alignment after an underrun.** Handing out a partial frame and zero-padding it shifts every later sample.
   Only hand out whole frames.
Also: start streaming on real amplitude (peak > 150), not on "any non-zero byte" - idle turntable hiss peaks around 80/32767.

## Pi / Bluetooth
- Turntable is transmitter-only. Sony's procedure: have the receiver discoverable first, then hold BLUETOOTH/PAIRING ~2 s
  (LED flashes blue). The turntable initiates; pairing from the Pi side fails with Authentication Failed.
- The Pi must advertise an audio class. Set `Class = 0x240414` in `/etc/bluetooth/main.conf` (persistent). `hciconfig ... class` is
  overwritten by bluetoothd; `btmgmt class` hangs without a tty. Without it the Pi shows as 0x00440000 and the turntable never lists it.
- After a stale bond (Pi rebooted/removed): `bluetoothctl remove <mac>`, reopen the window, re-pair.
- Pi Zero W (1 core): ~60-90 s Python start-up; one stream uses ~20-35 % CPU. Two simultaneous 3072-bit SRP pairings time out
  (SETUP, ~24 s), so multi-target connects are serialised. A Pi Zero 2 W would make multi-room comfortable.
- One unexplained reboot during testing; `vcgencmd get_throttled` was 0x0 afterwards.

## Multi-speaker
`--target` is repeatable (`--target "Kitchen=25" --target "office speaker=30"`). One capture is fanned out to one pyatv session per
speaker; each joins the live stream with a 1.5 s cushion so speakers connecting at different times stay in the same place in the
audio. Separate sessions are not clock-synced (expect small offsets, so fine for different rooms, not for one room).

## Sonos fix (found via postlund/pyatv#2915)
Sonos accepts pyatv's AirPlay 2 raw-PCM setup (ct 1, audioFormat 0x800), shows "playing" and plays silence. Setting the stream up as
ALAC (ct 2, audioFormat 0x40000) and wrapping each packet in an uncompressed ALAC frame before encryption makes it play (confirmed
by ear on a Sonos Bookshelf). `bridge/alac_patch.py` does this by patching pyatv's `AirPlayV2`; the bridge enables it per session for
Sonos (manufacturer property) or `--alac NAME`. The Harman Kardon keeps the stock PCM path.

## Speaker results
- Harman Kardon Citation One: works (see stage0-findings.md).
- Sonos Bookshelf ("Kitchen", AirPlay 2, acl=0, transient pairing): raw PCM (stock pyatv) - accepts the session (200 OK on pair-setup, SETUP x2, RECORD,
  FLUSH; /feedback reports the type-96 stream at 44.1 kHz) but plays nothing - also with the stock pyatv `stream_file` test tone from a
  Mac, at volumes 25-60, and with full metadata (title/artist/album/artwork/duration, tools/meta_test.py). So not a bridge bug and
  not (only) missing metadata. Sonos community reports realtime (non-buffered) AirPlay 2 streams are
  unreliable on Sonos (anecdotal, 2023): https://en.community.sonos.com/speakers-229128/airplay2-realitime-audio-6880588
- Bedroom HomePod: stock test tone accepted; audibility not confirmed.

## Split setup: Pi = Bluetooth receiver, iona = AirPlay sender (2026-10-23)
Measured on the Pi Zero W: `import pyatv` 24 s, 3072-bit modexp 0.18-0.37 s, ChaCha20-Poly1305 176 us/packet (~2 % of a core per
stream), byteswap 79 us/packet (~1 %). So the crypto is cheap and a C rewrite would not help; interpreter start-up and a single slow
core are the limits. ALAC wrapping is 8 us/packet on iona (0.1 % of a core). Moving the AirPlay work to a 4-core x86 box fixes it.

- Pi: Bluetooth sink only. iona pulls the raw capture with `bridge/iona/capture-from-pi.sh` over SSH using a capture-only key
  (Pi `authorized_keys`: `restrict,command="arecord ..."`; no shell, tty or forwarding).
- iona: `bridge/iona/run-bridge.sh` (Office, Kitchen/Sonos via ALAC, Bedroom HomePod): all three play the turntable, no "too slow".
- Gotcha: killing the bridge must also kill its capture ssh, otherwise the orphan keeps the Pi's audio channel busy and the next
  bridge hears nothing. The bridge now handles SIGTERM/SIGINT by cancelling its main task (finally blocks kill the capture).
- Service on iona: `bridge/iona/airplay-bridge.service` (user unit, install by hand into ~/.config/systemd/user). Lingering is
  off, so an administrator must run `loginctl enable-linger <user>` once for it to start at boot without a login.
