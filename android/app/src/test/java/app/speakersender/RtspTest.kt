package app.speakersender

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.InputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

// Loopback mock speaker: records each request head and echoes the body back. Mirrors what the C mock does for rtsp.c.
class RtspTest {
    private class Peer(socket: Socket, private val hap: Crypto.HapSession) {
        private val input: InputStream = socket.getInputStream()
        private val output: OutputStream = socket.getOutputStream()
        private var pending = ByteArray(0)

        private fun fill() {
            val tmp = ByteArray(4096)
            val n = input.read(tmp)
            require(n > 0) { "client closed" }
            pending += hap.decrypt(tmp.copyOf(n))!!
        }

        fun readRequest(): Pair<String, ByteArray> {
            val crlf2 = "\r\n\r\n".toByteArray()
            fun endOfHead(): Int {
                for (i in 0..pending.size - 4) if (pending.copyOfRange(i, i + 4).contentEquals(crlf2)) return i
                return -1
            }
            var end = endOfHead()
            while (end < 0) {
                fill()
                end = endOfHead()
            }
            val head = String(pending, 0, end, Charsets.ISO_8859_1)
            val len = Regex("Content-Length: (\\d+)").find(head)?.groupValues?.get(1)?.toInt() ?: 0
            while (pending.size < end + 4 + len) fill()
            val body = pending.copyOfRange(end + 4, end + 4 + len)
            pending = pending.copyOfRange(end + 4 + len, pending.size)
            return Pair(head, body)
        }

        fun reply(head: String, body: ByteArray) {
            val msg = head.toByteArray(Charsets.ISO_8859_1) + body
            output.write(hap.encrypt(msg))
            output.flush()
        }
    }

    /** Answers `count` requests: 200 with the request body echoed back, CSeq copied from the request. */
    private fun serve(server: ServerSocket, hap: Crypto.HapSession, count: Int, seen: MutableList<String>) {
        val socket = server.accept()
        val peer = Peer(socket, hap)
        repeat(count) {
            val (head, body) = peer.readRequest()
            seen.add(head)
            val cseq = Regex("CSeq: (\\d+)").find(head)?.groupValues?.get(1) ?: "x"
            peer.reply("RTSP/1.0 200 OK\r\nCSeq: $cseq\r\nContent-Length: ${body.size}\r\n\r\n", body)
        }
        socket.close()
    }

    @Test
    fun plainExchangeSendsSessionHeadersAndParsesReply() {
        val server = ServerSocket(0, 1, InetAddress.getLoopbackAddress())
        val seen = mutableListOf<String>()
        val pool = Executors.newSingleThreadExecutor()
        val peer = pool.submit<Unit> { serve(server, Crypto.HapSession(), 2, seen) }
        RtspConnection("127.0.0.1", server.localPort, 5000).use { c ->
            val first = c.exchange("SETUP", contentType = "application/x-apple-binary-plist", body = byteArrayOf(1, 2, 3))
            assertTrue(first.ok)
            assertArrayEquals(byteArrayOf(1, 2, 3), first.body)
            assertEquals("RTSP/1.0 200 OK", first.statusLine)
            assertEquals("0", first.header("cseq"))

            val second = c.exchange("GET_PARAMETER", uri = "rtsp://example/1")
            assertEquals("1", second.header("CSeq"))
            assertTrue(second.body.isEmpty())
        }
        peer.get(10, TimeUnit.SECONDS)
        pool.shutdown()
        server.close()

        assertTrue(seen[0].startsWith("SETUP rtsp://"))
        assertTrue(seen[0].contains("\r\nDACP-ID: "))
        assertTrue(seen[0].contains("\r\nActive-Remote: "))
        assertTrue(seen[0].contains("\r\nClient-Instance: "))
        assertTrue(seen[0].contains("\r\nContent-Length: 3"))
        assertTrue(seen[1].startsWith("GET_PARAMETER rtsp://example/1 RTSP/1.0"))
        assertFalse(seen[1].contains("Content-Length"))
    }

    @Test
    fun nonSuccessStatusIsReturnedNotThrown() {
        val server = ServerSocket(0, 1, InetAddress.getLoopbackAddress())
        val pool = Executors.newSingleThreadExecutor()
        val peer = pool.submit<Unit> {
            val socket = server.accept()
            val p = Peer(socket, Crypto.HapSession())
            p.readRequest()
            p.reply("RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: 0\r\n\r\n", ByteArray(0))
            socket.close()
        }
        RtspConnection("127.0.0.1", server.localPort, 5000).use { c ->
            val r = c.exchange("SETUP")
            assertEquals(455, r.code)
            assertFalse(r.ok)
        }
        peer.get(10, TimeUnit.SECONDS)
        pool.shutdown()
        server.close()
    }

    @Test
    fun encryptedExchangeRoundTripsLargeBody() {
        val k1 = ByteArray(32) { 0x11 }
        val k2 = ByteArray(32) { 0x22 }
        val peerHap = Crypto.HapSession().also { it.enable(k2, k1) }
        val server = ServerSocket(0, 1, InetAddress.getLoopbackAddress())
        val seen = mutableListOf<String>()
        val pool = Executors.newSingleThreadExecutor()
        val peer = pool.submit<Unit> { serve(server, peerHap, 1, seen) }
        RtspConnection("127.0.0.1", server.localPort, 5000).use { c ->
            c.enableEncryption(k1, k2)
            val body = ByteArray(3000) { (it * 7).toByte() } // several 1024-byte HAP frames each way
            val r = c.exchange("SETUP", body = body)
            assertTrue(r.ok)
            assertArrayEquals(body, r.body)
        }
        peer.get(10, TimeUnit.SECONDS)
        pool.shutdown()
        server.close()
    }
}
