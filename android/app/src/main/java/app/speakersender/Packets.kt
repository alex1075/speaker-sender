package app.speakersender

/** RTP audio, sync and timing packets, as the builders in c/src/airplay.c. Long carries unsigned 32/64-bit values. */
object Packets {
    /** Encrypted RTP audio: 12-byte header, ChaCha20-Poly1305 payload + tag, then the last 8 nonce bytes. */
    fun buildAudio(
        key: ByteArray,
        counter: Long,
        first: Boolean,
        seq: Int,
        rtpTs: Long,
        ssrc: Long,
        payload: ByteArray,
    ): ByteArray {
        val head = byteArrayOf(0x80.toByte(), (if (first) 0xE0 else 0x60).toByte()) +
            beBytes(seq.toLong(), 2) + beBytes(rtpTs, 4) + beBytes(ssrc, 4)
        val nonce = Crypto.counterNonce(counter)
        val sealed = Crypto.aeadEncrypt(key, nonce, head.copyOfRange(4, 12), payload)
        return head + sealed + nonce.copyOfRange(4, 12)
    }

    /** 20-byte sync packet for the control port. */
    fun buildSync(first: Boolean, rtpNowWithoutLatency: Long, ntp: Long, rtpNow: Long): ByteArray =
        byteArrayOf((if (first) 0x90 else 0x80).toByte(), 0xD4.toByte()) + beBytes(7, 2) +
            beBytes(rtpNowWithoutLatency, 4) + beBytes(ntp ushr 32, 4) + beBytes(ntp, 4) + beBytes(rtpNow, 4)

    /** Reply to a 32-byte NTP timing request; the request's send time becomes the reference time. Null if too short. */
    fun buildTimingReply(req: ByteArray, ntpNow: Long): ByteArray? {
        if (req.size < 32) return null
        return byteArrayOf(req[0], 0xD3.toByte()) + beBytes(7, 2) + ByteArray(4) + req.copyOfRange(24, 32) +
            beBytes(ntpNow ushr 32, 4) + beBytes(ntpNow, 4) + beBytes(ntpNow ushr 32, 4) + beBytes(ntpNow, 4)
    }
}
