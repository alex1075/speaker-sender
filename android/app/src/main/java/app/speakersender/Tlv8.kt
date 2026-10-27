package app.speakersender

import java.io.ByteArrayOutputStream

/** TLV8 as in c/src/tlv8.c: values longer than 255 bytes go out as 255-byte fragments with the same type. */
object Tlv8 {
    const val METHOD = 0x00
    const val IDENTIFIER = 0x01
    const val SALT = 0x02
    const val PUBLIC_KEY = 0x03
    const val PROOF = 0x04
    const val ENCRYPTED_DATA = 0x05
    const val STATE = 0x06
    const val ERROR = 0x07
    const val FLAGS = 0x13

    fun put(out: ByteArrayOutputStream, type: Int, value: ByteArray) {
        if (value.isEmpty()) {
            out.write(type)
            out.write(0)
            return
        }
        var pos = 0
        while (pos < value.size) {
            val n = minOf(255, value.size - pos)
            out.write(type)
            out.write(n)
            out.write(value, pos, n)
            pos += n
        }
    }

    fun encode(vararg items: Pair<Int, ByteArray>): ByteArray {
        val out = ByteArrayOutputStream()
        for ((type, value) in items) put(out, type, value)
        return out.toByteArray()
    }

    fun encodeByte(type: Int, value: Int): Pair<Int, ByteArray> = type to byteArrayOf(value.toByte())

    /** Returns the concatenated value of `type`, or null if absent. A 255-byte item is continued by the next same-type item. */
    fun get(buf: ByteArray, type: Int): ByteArray? {
        var pos = 0
        var found = false
        val acc = ByteArrayOutputStream()
        while (pos + 2 <= buf.size) {
            val t = buf[pos].toInt() and 0xFF
            val len = buf[pos + 1].toInt() and 0xFF
            if (pos + 2 + len > buf.size) break
            if (t == type) {
                found = true
                acc.write(buf, pos + 2, len)
                if (len != 255) break
            } else if (found) {
                break
            }
            pos += 2 + len
        }
        return if (found) acc.toByteArray() else null
    }
}
