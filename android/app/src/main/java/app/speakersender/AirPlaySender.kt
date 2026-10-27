package app.speakersender

/**
 * The part that has to be written. Follow docs/protocol-notes.md; each step was verified against a real speaker with pyatv.
 */
interface AirPlaySender {
    /** Steps 1-4: GET /info, transient pairing (SRP PIN 3939), HKDF control keys, encrypt the control socket. */
    suspend fun connect(speaker: Speaker)

    /** SET_PARAMETER volume (dB, -144 = mute .. 0). */
    suspend fun setVolume(percent: Float)

    /** One 352-frame (1408 byte) PCM chunk; encrypt with ChaCha20-Poly1305(shk) and send over UDP in real time. */
    suspend fun sendPcm(chunk: ByteArray)

    /** TEARDOWN and close all sockets. */
    suspend fun close()
}

class AirPlay2Sender : AirPlaySender {
    override suspend fun connect(speaker: Speaker) {
        TODO(
            "1) GET /info  2) POST /pair-pin-start + /pair-setup M1-M4 (TLV8, SRP-6a 3072/SHA-512, user 'Pair-Setup', PIN 3939)  " +
                "3) HKDF-SHA512 control+events keys  4) enable HAP framing on the RTSP socket  5) SETUP #1 (plist) -> eventPort  " +
                "6) open event channel  7) SETUP #2 (stream: type 96, ct 1, audioFormat 0x800, shk) -> dataPort  8) RECORD  " +
                "9) start timing + sync UDP responders"
        )
    }

    override suspend fun setVolume(percent: Float) = TODO("RTSP SET_PARAMETER volume")
    override suspend fun sendPcm(chunk: ByteArray) = TODO("RTP header + ChaCha20-Poly1305 + 8-byte nonce trailer")
    override suspend fun close() = TODO("TEARDOWN")
}
