package app.airplaysender

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

// Mirrors the setup checks of run_scenario in c/tests/test_mock.c (ap_session_open against the mock receiver).
class SessionTest {
    private fun waitFor(timeoutMs: Long, cond: () -> Boolean): Boolean {
        val end = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < end) {
            if (cond()) return true
            Thread.sleep(20)
        }
        return cond()
    }

    private fun openAndCheck(alac: Boolean) {
        MockReceiver().use { mock ->
            AirPlaySession.open(Target("mock", "127.0.0.1", mock.rtspPort, useAlac = alac, volumePct = 40.0)).use { session ->
                assertTrue(mock.pairingOk)
                assertEquals(if (alac) 2L else 1L, mock.streamCt)
                assertEquals(if (alac) 0x40000L else 0x800L, mock.streamFormat)
                assertTrue(mock.shkMatches)
                assertEquals(-18.0, mock.volumeDb, 0.01) // 40% is -18 dB
                assertTrue(mock.progress!!.startsWith("progress: 66150/66150/"))
                assertTrue(mock.metadataSeen)
                assertTrue(mock.recordSeen)
                assertTrue(mock.flushSeen)

                // Event channel: the speaker sends a request and the sender answers 200 with the same CSeq.
                val reply = mock.sendEvent()
                assertTrue(reply, reply.startsWith("RTSP/1.0 200 OK"))
                assertTrue(reply, reply.contains("CSeq: 7"))

                // Timing responder: reference time is the request's send time (bytes 24..31).
                val req = ByteArray(32).also { it[24] = 1; it[31] = 2 }
                val rep = mock.timingRequest(mock.timingPort)
                assertEquals(32, rep.size)
                assertEquals(0xD3.toByte(), rep[1])
                assertArrayEquals(req.copyOfRange(24, 32), rep.copyOfRange(8, 16))

                // Keep-alive: feedback goes out about every 2 s.
                assertTrue("feedback", waitFor(5000) { mock.feedbacks >= 1 })
                assertTrue(session.alive)
            }
            assertTrue(mock.teardownSeen)
        }
    }

    @Test
    fun opensPairsSetsUpAndTearsDownPcm() = openAndCheck(alac = false)

    @Test
    fun opensPairsSetsUpAndTearsDownAlac() = openAndCheck(alac = true)
}
