package app.speakersender

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream

// Step 8: paced streaming, decrypted audio, sync packets and retransmits against the mock receiver.
class StreamTest {
    private fun pcm(i: Int) = ByteArray(AirPlaySession.PACKET_PCM_BYTES) { ((i * 31 + it * 7) and 0xFF).toByte() }

    /** Little-endian input to the big-endian bytes the speaker should get: swap each sample's two bytes. */
    private fun swapPairs(p: ByteArray) = ByteArray(p.size) { p[it xor 1] }

    private fun waitFor(timeoutMs: Long, cond: () -> Boolean): Boolean {
        val end = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < end) {
            if (cond()) return true
            Thread.sleep(20)
        }
        return cond()
    }

    private fun streamAndCheck(alac: Boolean) {
        MockReceiver().use { mock ->
            AirPlaySession.open(Target("mock", "127.0.0.1", mock.rtspPort, useAlac = alac)).use { session ->
                val inputs = (0 until 200).map { pcm(it) }
                val t0 = System.nanoTime()
                Streamer.run(session, inputs.iterator())
                val elapsed = (System.nanoTime() - t0) / 1e9
                // Last packet is due at 199 * 352 / 44100 = 1.588 s; pacing must not send it all at once.
                assertTrue("elapsed $elapsed", elapsed > 1.5 && elapsed < 3.0)

                assertTrue(waitFor(3000) { mock.audio.size >= 200 })
                assertEquals(0, mock.badPackets)
                val got = mock.audio.toList()
                assertEquals(200, got.size)
                for (i in 0 until 200) {
                    val r = got[i]
                    assertEquals("first flag $i", i == 0, r.first)
                    assertEquals("seq $i", (got[0].seq + i) and 0xFFFF, r.seq)
                    assertEquals("ts $i", AirPlaySession.LATENCY_FRAMES + 352L * i, r.ts)
                    assertArrayEquals("pcm $i", swapPairs(inputs[i]), r.pcm)
                }

                // Sync: the first one has the first-sync marker, and rtp_now sits one latency ahead of the stream time.
                assertTrue("sync", waitFor(3000) { mock.syncPackets.isNotEmpty() })
                val s0 = mock.syncPackets.first()
                assertEquals(0x90.toByte(), s0[0])
                assertEquals(0xD4.toByte(), s0[1])
                assertEquals(AirPlaySession.LATENCY_FRAMES.toLong(), beInt(s0, 16) - beInt(s0, 4))

                // Retransmit: asking for two lost packets gets both back, tagged 0xD6 with their original sequence.
                val lost = got[10].seq
                mock.requestRetransmit(lost, 2)
                assertTrue("retransmit", waitFor(2000) { mock.retransmits.size >= 2 })
                val rt = mock.retransmits.toList().take(2)
                assertEquals(0xD6.toByte(), rt[0][1])
                assertEquals(lost, ((rt[0][2].toInt() and 0xFF) shl 8) or (rt[0][3].toInt() and 0xFF))
                assertEquals(lost + 1, ((rt[1][2].toInt() and 0xFF) shl 8) or (rt[1][3].toInt() and 0xFF))
            }
        }
    }

    private fun beInt(b: ByteArray, pos: Int): Long {
        var v = 0L
        for (i in 0 until 4) v = (v shl 8) or (b[pos + i].toLong() and 0xFF)
        return v
    }

    @Test
    fun streamsPacedPcmWithSyncAndRetransmit() = streamAndCheck(alac = false)

    @Test
    fun streamsPacedAlacWithSyncAndRetransmit() = streamAndCheck(alac = true)

    @Test
    fun readsWholePacketsAndPadsTheLast() {
        val data = ByteArray(AirPlaySession.PACKET_PCM_BYTES * 2 + 100) { (it + 1).toByte() }
        val packets = Streamer.packets(ByteArrayInputStream(data)).asSequence().toList()
        assertEquals(3, packets.size)
        assertTrue(packets.all { it.size == AirPlaySession.PACKET_PCM_BYTES })
        assertArrayEquals(data.copyOfRange(0, 1408), packets[0])
        assertArrayEquals(data.copyOfRange(2816, 2916) + ByteArray(1308), packets[2])
    }
}
