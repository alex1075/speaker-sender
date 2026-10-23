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

## Speaker results
- Harman Kardon Citation One: works (see stage0-findings.md).
- Sonos Bookshelf ("Kitchen", AirPlay 2, acl=0, transient pairing): accepts the session (200 OK on pair-setup, SETUP x2, RECORD,
  FLUSH; /feedback reports the type-96 stream at 44.1 kHz) but plays nothing - also with the stock pyatv `stream_file` test tone from a
  Mac, at volumes 25-60, and with full metadata (title/artist/album/artwork/duration, tools/meta_test.py). So not a bridge bug and
  not (only) missing metadata. Sonos community reports realtime (non-buffered) AirPlay 2 streams are
  unreliable on Sonos (anecdotal, 2023): https://en.community.sonos.com/speakers-229128/airplay2-realitime-audio-6880588
- Bedroom HomePod: stock test tone accepted; audibility not confirmed.
