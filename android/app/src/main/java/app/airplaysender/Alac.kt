package app.airplaysender

/**
 * "Uncompressed ALAC" framing as in c/src/alac.c: a 23-bit header, the PCM bytes (big endian), then 3 end bits of 111,
 * packed MSB first. Each bit is written at its position in the stream, with no byte alignment.
 */
object Alac {
    private const val HEADER_BITS = 23
    private const val HEADER_VALUE = (1 shl 20) or 1 // tag 001, instance 0, no size, shift 0, uncompressed

    fun wrappedSize(pcmLen: Int): Int = (HEADER_BITS + pcmLen * 8 + 3 + 7) / 8

    fun wrap(pcm: ByteArray): ByteArray {
        val out = ByteArray(wrappedSize(pcm.size))
        var pos = 0
        fun put(value: Int, n: Int) {
            for (i in n - 1 downTo 0) {
                if (((value shr i) and 1) == 1) {
                    out[pos shr 3] = (out[pos shr 3].toInt() or (0x80 ushr (pos and 7))).toByte()
                }
                pos++
            }
        }
        put(HEADER_VALUE, HEADER_BITS)
        for (b in pcm) put(b.toInt() and 0xFF, 8)
        put(7, 3)
        return out
    }

    /** Returns the PCM bytes, or null if the frame is the wrong size, has a bad header, or bad end bits. */
    fun unwrap(frame: ByteArray, pcmLen: Int): ByteArray? {
        if (frame.size != wrappedSize(pcmLen)) return null
        var pos = 0
        fun get(n: Int): Int {
            var v = 0
            repeat(n) {
                v = (v shl 1) or (((frame[pos shr 3].toInt() and 0xFF) shr (7 - (pos and 7))) and 1)
                pos++
            }
            return v
        }
        if (get(HEADER_BITS) != HEADER_VALUE) return null
        val pcm = ByteArray(pcmLen) { get(8).toByte() }
        return if (get(3) == 7) pcm else null
    }
}
