# Stage 0 findings (2026-10-23)

## CORRECTION - re-run from a clean sender (Pi Zero W, no AirPlay stack)
The Mac results below were **tainted** (the Mac runs its own AirPlay stack). Re-running from the Pi Zero W
(pyatv 0.18.0 via piwheels, Python 3.13, armv6l):
- `tools/sweep_setup.py`: the audio-stream SETUP was **accepted for every variant** (PCM ct=1, ALAC ct=2, AAC-LC ct=4,
  PCM without latency keys, PCM with minimal keys) - including the plain PCM baseline that got `400 Bad Request` from the Mac.
- `tools/stage0_test.py "office speaker" 20 auto` streamed a 2 s tone with no errors (11:15 BST; audible result to be confirmed by Alex).
- So the speaker is fine with pyatv's stock AirPlay 2 realtime path (transient HAP pairing + encrypted control channel + PCM).
  The "needs ALAC/AAC" and "AirPlay 1 can't work" conclusions below should be treated as unproven.
- Pi note: pyatv installs from piwheels but takes ~15 min on the Zero; scp does not work (no sftp-server), use `tar | ssh`.

## Original Mac-run notes (tainted, kept for reference)

Tooling: pyatv 0.18.0 on Python 3.14 (`atvremote` CLI is broken on 3.14 - `get_event_loop`; use the scripts in `tools/`).

## Speaker tested: "office speaker" (Harman Kardon Citation One, fw p20.0.0.2710, AirTunes/366.0)
- mDNS RAOP: `cn=0,1` (PCM, ALAC) `et=0,4` `ft=0x445F8A00,0x1C340` `sf=0x4`; also advertises AirPlay 2 (HK pairing/system pairing, buffered audio, PTP).
- `GET /info` works. HAP **transient** pair-setup (`POST /pair-pin-start`, `/pair-setup` M1-M4) succeeds with no PIN.

## Results
| Path | Outcome |
|---|---|
| pyatv AP1 (RAOP ANNOUNCE) as shipped | Speaker drops the connection at ANNOUNCE. pyatv's AP1 path runs transient pair-verify but never enables HAP encryption on the RTSP channel. |
| AP1 with encryption enabled (`1enc` in `tools/stage0_test.py`) | ANNOUNCE -> 200 OK. Classic RAOP `SETUP` (Transport header) then gets no reply / connection lost. |
| pyatv AP2 realtime (auto) | Base SETUP OK (eventPort/timingPort returned). Audio-stream SETUP (`type 0x60`, `ct=1` PCM, `audioFormat 0x800`) -> **`400 Bad Request`**, returned slowly (>10 s; pyatv's default 10 s timeout hides it as a TimeoutError). |

## Takeaways
- This speaker wants an AirPlay 2 style session: transient HAP pairing, encrypted control channel, plist SETUP.
- PCM (`ct=1`) realtime streams are rejected; next candidates are ALAC (`ct=2`, `audioFormat 0x40000`) or AAC, and/or tweaking latencyMin/Max, `shk`, `streamConnectionID`, or buffered audio (type 103) with PTP timing.
- So the "start with AP1, add AP2 later" plan does not hold for this speaker: the sender needs transient pairing + encryption from day one (AirPlay 1 audio payload, AirPlay 2-style session).
- Not yet tested: a HomePod (Bedroom) as a control, and non-Harman speakers.
