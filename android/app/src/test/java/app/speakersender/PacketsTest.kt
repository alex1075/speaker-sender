package app.speakersender

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

// Mirrors test_alac and test_packets in c/tests/test_unit.c.
class PacketsTest {
    @Test
    fun alacWrapsToGoldenFrameAndBack() {
        assertEquals(1412, Alac.wrappedSize(1408))
        val frame = Alac.wrap(Vectors.V_AUDIO_PCM)
        assertArrayEquals(Vectors.V_ALAC, frame)
        assertArrayEquals(Vectors.V_AUDIO_PCM, Alac.unwrap(frame, 1408))

        val bad = frame.copyOf()
        bad[0] = (bad[0].toInt() xor 0x80).toByte()
        assertNull(Alac.unwrap(bad, 1408))
    }

    @Test
    fun buildsGoldenAudioPacket() {
        val pkt = Packets.buildAudio(
            Vectors.V_AUDIO_KEY,
            counter = 2,
            first = true,
            seq = 0x1234,
            rtpTs = 66150,
            ssrc = 0xdeadbeefL,
            payload = Vectors.V_AUDIO_PCM,
        )
        assertArrayEquals(Vectors.V_AUDIO_PKT3, pkt)
    }

    @Test
    fun buildsGoldenSyncPacket() {
        val sync = Packets.buildSync(
            first = true,
            rtpNowWithoutLatency = 1000,
            ntp = (0xe9c1a2b3L shl 32) or 0x12345678L,
            rtpNow = 67150,
        )
        assertArrayEquals(Vectors.V_SYNC, sync)
    }

    @Test
    fun buildsGoldenTimingReply() {
        val rep = Packets.buildTimingReply(Vectors.V_TIMING_REQ, (0xe9c1a2c0L shl 32) or 0x0a0b0c0dL)
        assertArrayEquals(Vectors.V_TIMING_REP, rep)
        assertNull(Packets.buildTimingReply(Vectors.V_TIMING_REQ.copyOf(10), 0L))
    }
}
