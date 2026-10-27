package app.speakersender

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

// Mirrors test_bplist in c/tests/test_unit.c.
class BplistTest {
    @Test
    fun parsesGoldenPlist() {
        val root = Bplist.parse(Vectors.V_BPLIST) as Map<*, *>
        val s0 = (root["streams"] as List<*>)[0] as Map<*, *>
        assertEquals(0x800L, s0["audioFormat"])
        assertEquals(3735928559L, s0["streamConnectionID"])
        assertEquals(true, s0["isMedia"])
        assertEquals(false, s0["supportsDynamicStreamID"])
        assertArrayEquals(Vectors.V_AUDIO_KEY, s0["shk"] as ByteArray)
        assertEquals(4294967296L, root["big"])
        assertEquals(1.5, root["ratio"])
        assertEquals("café ♫", root["name"]) // UTF-16 string in the golden file
    }

    @Test
    fun roundTripsEveryTypeItWrites() {
        val blob = ByteArray(300) { 0xAB.toByte() }
        val arr = (0 until 20).map { it * 1000L }
        val d = linkedMapOf<String, Any?>(
            "n" to -5L,
            "small" to 7,
            "big" to (1L shl 40),
            "f" to 0.25,
            "t" to true,
            "fl" to false,
            "s" to "héllo",
            "long" to "0123456789abcdef0123456789",
            "blob" to blob,
            "arr" to arr,
            "nul" to null,
        )
        val ser = Bplist.serialize(d)
        assertArrayEquals("bplist00".toByteArray(), ser.copyOf(8))
        val p = Bplist.parse(ser) as Map<*, *>
        assertNotNull(p)
        assertEquals(-5L, p["n"])
        assertEquals(7L, p["small"])
        assertEquals(1L shl 40, p["big"])
        assertEquals(0.25, p["f"])
        assertEquals(true, p["t"])
        assertEquals(false, p["fl"])
        assertEquals("héllo", p["s"])
        assertEquals("0123456789abcdef0123456789", p["long"])
        assertArrayEquals(blob, p["blob"] as ByteArray)
        assertEquals(arr, p["arr"])
        assertTrue(p.containsKey("nul"))
    }

    @Test
    fun rejectsGarbageWithoutThrowing() {
        assertNull(Bplist.parse("bplist00xxxx".toByteArray()))
        for (cut in 0 until Vectors.V_BPLIST.size step 17) Bplist.parse(Vectors.V_BPLIST.copyOf(cut))
        val corrupt = Vectors.V_BPLIST.copyOf()
        for (i in 8 until corrupt.size - 33 step 5) {
            corrupt[i] = (corrupt[i].toInt() xor 0xFF).toByte()
            Bplist.parse(corrupt)
            corrupt[i] = (corrupt[i].toInt() xor 0xFF).toByte()
        }
    }
}
