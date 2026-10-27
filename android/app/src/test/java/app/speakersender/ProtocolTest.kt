package app.speakersender

import org.junit.Assert.assertEquals
import org.junit.Test

class ProtocolTest {
    @Test
    fun parsesFeatures() {
        assertEquals(0x444F8A00UL, Protocol.parseFeatures("0x444F8A00"))
        assertEquals((0x1C340UL shl 32) or 0x445F8A00UL, Protocol.parseFeatures("0x445F8A00,0x1C340"))
        assertEquals(0UL, Protocol.parseFeatures(""))
        assertEquals(0UL, Protocol.parseFeatures(null))
        assertEquals(0UL, Protocol.parseFeatures("nonsense"))
    }

    // Same values as c/tests/test_ap1.c, taken from the real speakers.
    @Test
    fun detectsAirPlayVersion() {
        assertEquals(1, Protocol.protoFromFeatures(Protocol.parseFeatures("0x444F8A00"))) // legacy receiver
        assertEquals(2, Protocol.protoFromFeatures(Protocol.parseFeatures("0x445F8A00,0x1C340"))) // Harman Kardon
        assertEquals(2, Protocol.protoFromFeatures(Protocol.parseFeatures("0x445F8A00,0x801C340"))) // Sonos
        assertEquals(2, Protocol.protoFromFeatures(Protocol.parseFeatures("0x4A7FCA00,0x3C356BD0"))) // HomePod
        assertEquals(2, Protocol.protoFromFeatures(Protocol.parseFeatures("0x4A7FCFD5,0x38174FDE"))) // Mac
    }

    @Test
    fun mapsVolumeToDb() {
        assertEquals(-144.0, Protocol.volumeDb(0.0), 0.0)
        assertEquals(-18.0, Protocol.volumeDb(40.0), 1e-9)
        assertEquals(-15.0, Protocol.volumeDb(50.0), 1e-9)
        assertEquals(0.0, Protocol.volumeDb(100.0), 1e-9)
    }

    @Test
    fun packetSizeMatchesC() {
        assertEquals(1408, Protocol.PACKET_PCM_BYTES)
    }
}
