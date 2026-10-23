# AirPlay 2 sender - what actually worked (Harman Kardon Citation One, 2026-10-23)

Everything below was observed in pyatv 0.18.0 debug logs and read from its source; it is the exact sequence a
non-Apple sender used to play audio on a speaker that needs no PIN. Source paths are inside the `pyatv` package.

## 1. Discovery (mDNS)
- `_raop._tcp` instance name is `<DEVICEID>@<name>` (e.g. `204EF66480F3@office speaker`); TXT has `cn` (codecs), `et` (encryption types), `ft` (features), `sf`, `pk`.
- `_airplay._tcp` advertises the AirPlay 2 `features`. Same port as RAOP on this speaker.

## 2. Control connection (TCP, RTSP/1.0 + HTTP/1.1 on the same socket)
1. `GET /info` (RTSP) -> binary plist with `features`, `statusFlags`, `deviceID`, `model`, ...
2. Transient HAP pairing (no PIN prompt): headers `X-Apple-HKP: 4`, `User-Agent: AirPlay/320.20`
   - `POST /pair-pin-start` (empty body)
   - `POST /pair-setup` M1: TLV8 {Method=0, SeqNo=1, Flags=TransientPairing}
   - M2 reply: {Salt, PublicKey}
   - SRP-6a: user `Pair-Setup`, PIN `3939` (`TRANSIENT_PIN`), 3072-bit group, SHA-512 (`auth/hap_srp.py`)
   - `POST /pair-setup` M3: {SeqNo=3, PublicKey, Proof}; M4 reply = server proof
3. Derive keys: HKDF-SHA512, 32 bytes, `salt`/`info` as ASCII, input = SRP shared key
   - control: salt `Control-Salt`, write info `Control-Write-Encryption-Key`, read info `Control-Read-Encryption-Key`
   - events: salt `Events-Salt`, write `Events-Write-Encryption-Key`, read `Events-Read-Encryption-Key`
4. Encrypt the control socket from here on (`auth/hap_session.py`): frames of up to 1024 bytes,
   `[2-byte LE length][ChaCha20-Poly1305 ciphertext][16-byte tag]`, AAD = the 2 length bytes, counter nonce.
   **Not doing this makes the speaker drop the connection** (seen on pyatv's AirPlay 1 path, which never enables it).

## 3. Session setup (all over the encrypted control socket)
- `SETUP` #1, plist body: `deviceID`, `sessionUUID`, `timingPort` (our NTP-style timing UDP port), `timingProtocol: NTP`,
  `isMultiSelectAirPlay`, `groupContainsGroupLeader: false`, `macAddress`, `model`, `name`, `osName`, `osVersion`, `sourceVersion`, ...
  -> reply `{eventPort, timingPort}`. Open the event channel (TCP to `eventPort`, ChaCha20 with the Events keys).
- `SETUP` #2 (audio stream), plist `streams[0]`: `type 96` (0x60, realtime), `ct 1` (raw PCM), `audioFormat 0x800` (PCM 44100/16/2),
  `spf 352`, `sr 44100`, `controlPort`, `latencyMin 11025`, `latencyMax 88200`, `shk` (32-byte audio key), `streamConnectionID`,
  `isMedia true`, `audioMode default`, `supportsDynamicStreamID false`
  -> reply `{streams:[{dataPort, controlPort, type}]}`. ALAC (`ct 2`, `0x40000`) and AAC-LC (`ct 4`, `0x400000`, spf 1024) were also accepted.
- `RECORD`, then `SET_PARAMETER volume: <dB>` as needed; `FEEDBACK` keep-alives; `TEARDOWN` at the end.

## 4. Audio (UDP to `dataPort`)
- One packet per 352 frames (1408 bytes of 16-bit stereo PCM), paced in real time, ~2 s of latency.
- RTP header (12 bytes: `0x80`, payload type `0x60` or `0xE0` on the first packet, seq, timestamp, ssrc), then
  ChaCha20-Poly1305(`shk`) ciphertext+tag, then the last 8 bytes of the nonce. AAD = `rtp_header[4:12]`. Nonce is an 8-byte
  counter (4 zero bytes + 8 byte counter).
- Sync packets on the control UDP port and an NTP-style timing exchange on the timing port keep the speaker clock aligned.

## 5. Quirks found
- This speaker needs ~10+ s to reply to a rejected `SETUP` (pyatv's default 10 s timeout hid a `400` as a timeout).
- Results from a Mac (which runs its own AirPlay stack) were unreliable; always test from a clean sender.
- Unicast mDNS queries to the speaker return nothing; multicast scans (4 s) find it reliably.
