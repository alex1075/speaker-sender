package app.speakersender

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.ByteArrayOutputStream

// Mirrors test_tlv8 in c/tests/test_unit.c (no golden vectors exist for TLV8).
class Tlv8Test {
    @Test
    fun fragmentsAndJoinsLongValues() {
        val big = ByteArray(600) { it.toByte() }
        val out = ByteArrayOutputStream()
        Tlv8.put(out, Tlv8.STATE, byteArrayOf(3))
        Tlv8.put(out, Tlv8.PUBLIC_KEY, big)
        Tlv8.put(out, Tlv8.PROOF, "abc".toByteArray())
        val buf = out.toByteArray()
        // 2 + 1, then 255 + 255 + 90 with 3 headers, then 2 + 3
        assertEquals(3 + 600 + 6 + 5, buf.size)
        assertArrayEquals(big, Tlv8.get(buf, Tlv8.PUBLIC_KEY))
        assertArrayEquals("abc".toByteArray(), Tlv8.get(buf, Tlv8.PROOF))
        assertArrayEquals(byteArrayOf(3), Tlv8.get(buf, Tlv8.STATE))
        assertNull(Tlv8.get(buf, Tlv8.SALT))
    }

    @Test
    fun encodesTransientM1ExactlyAsOnTheWire() {
        val m1 = Tlv8.encode(
            Tlv8.encodeByte(Tlv8.METHOD, 0),
            Tlv8.encodeByte(Tlv8.STATE, 1),
            Tlv8.encodeByte(Tlv8.FLAGS, 0x10),
        )
        assertArrayEquals(
            byteArrayOf(0x00, 0x01, 0x00, 0x06, 0x01, 0x01, 0x13, 0x01, 0x10),
            m1,
        )
    }

    @Test
    fun handlesEmptyAndBoundaryLengths() {
        for (n in listOf(0, 1, 254, 255, 256, 510, 511)) {
            val value = ByteArray(n) { (it * 7).toByte() }
            val buf = Tlv8.encode(Tlv8.PUBLIC_KEY to value)
            assertEquals("length $n", 2 * ((n + 254) / 255).coerceAtLeast(1) + n, buf.size)
            assertArrayEquals("length $n", value, Tlv8.get(buf, Tlv8.PUBLIC_KEY))
        }
    }

    @Test
    fun returnsEmptyValueWhenPresentWithLengthZero() {
        val buf = Tlv8.encode(Tlv8.ERROR to ByteArray(0))
        assertArrayEquals(ByteArray(0), Tlv8.get(buf, Tlv8.ERROR))
    }

    @Test
    fun ignoresTruncatedTrailingItem() {
        // Declared length 5, only 2 bytes follow: the item is dropped, not read past the end.
        val buf = byteArrayOf(0x06, 0x01, 0x02, 0x03, 0x05, 0x09)
        assertArrayEquals(byteArrayOf(2), Tlv8.get(buf, Tlv8.STATE))
        assertNull(Tlv8.get(buf, Tlv8.PUBLIC_KEY))
    }
}
