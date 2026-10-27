package app.speakersender

import java.io.Closeable
import java.io.InputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.security.SecureRandom
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * Loopback AirPlay 2 receiver for tests, the Kotlin counterpart of c/tests/test_mock.c. Pairs with transient
 * Pair-Setup (PIN 3939), answers SETUP x2, the event channel, feedback, volume/progress/metadata, RECORD, FLUSH
 * and TEARDOWN, and decrypts every audio packet it gets. Results are public fields for the test to check.
 */
class MockReceiver : Closeable {
    class Received(val first: Boolean, val seq: Int, val ts: Long, val ssrc: Long, val pcm: ByteArray)

    class Request(val method: String, val uri: String, val proto: String, val headers: List<String>, val body: ByteArray) {
        fun header(name: String): String? =
            headers.firstOrNull { it.substringBefore(':').equals(name, ignoreCase = true) }?.substringAfter(':')?.trim()
    }

    /** A reply, plus the HAP keys to switch on after it is written (the C mock enables encryption after M4). */
    private class Answer(val code: Int, val extra: String, val body: ByteArray, val enable: Pair<ByteArray, ByteArray>? = null)

    private val loopback: InetAddress = InetAddress.getLoopbackAddress()
    private val rtsp = ServerSocket(0, 4, loopback)
    private val eventServer = ServerSocket(0, 1, loopback)
    private val audioSock = DatagramSocket(0, loopback)
    private val controlSock = DatagramSocket(0, loopback)
    private val pool: ExecutorService = Executors.newCachedThreadPool { r -> Thread(r).also { it.isDaemon = true } }
    @Volatile private var closed = false

    val rtspPort: Int = rtsp.localPort

    // What the sender did, for assertions.
    @Volatile var pairingOk = false
    @Volatile var volumeDb = Double.NaN
    @Volatile var progress: String? = null
    @Volatile var metadataSeen = false
    @Volatile var recordSeen = false
    @Volatile var flushSeen = false
    @Volatile var teardownSeen = false
    @Volatile var feedbacks = 0
    @Volatile var streamCt = -1L
    @Volatile var streamFormat = -1L
    @Volatile var shkMatches = false
    @Volatile var timingPort = 0
    @Volatile var sessionControlPort = 0
    val audio = ConcurrentLinkedQueue<Received>()
    @Volatile var badPackets = 0
    val syncPackets = ConcurrentLinkedQueue<ByteArray>()
    val retransmits = ConcurrentLinkedQueue<ByteArray>()

    private var srp: Srp.Server? = null
    private var shk = ByteArray(0)
    private var sessionKey = ByteArray(0)
    @Volatile private var eventHap: Crypto.HapSession? = null
    @Volatile private var eventSocket: Socket? = null

    init {
        pool.execute { acceptRtsp() }
        pool.execute { loopPackets(audioSock) { handleAudio(it) } }
        pool.execute { loopPackets(controlSock) { handleControl(it) } }
    }

    /** Sends an event request the way the speaker does, and returns the sender's reply head. */
    fun sendEvent(): String {
        val s = eventSocket ?: error("no event channel yet")
        val hap = eventHap ?: error("no event keys yet")
        val req = "POST /event RTSP/1.0\r\nCSeq: 7\r\nContent-Length: 0\r\n\r\n".toByteArray(Charsets.ISO_8859_1)
        s.getOutputStream().write(hap.encrypt(req))
        s.getOutputStream().flush()
        return Reader(s.getInputStream(), hap).readRaw()
    }

    /** Sends a 32-byte NTP timing request to the sender's timing port and returns the 32-byte reply. */
    fun timingRequest(port: Int): ByteArray {
        val sock = DatagramSocket(0, loopback)
        sock.soTimeout = 2000
        val req = ByteArray(32)
        req[0] = 0x80.toByte()
        req[1] = 0xD2.toByte()
        req[24] = 1
        req[31] = 2
        sock.send(DatagramPacket(req, req.size, loopback, port))
        val reply = DatagramPacket(ByteArray(64), 64)
        sock.receive(reply)
        sock.close()
        return reply.data.copyOf(reply.length)
    }

    /** Asks the sender to resend `count` packets from sequence number `lost` (payload type 0x55). */
    fun requestRetransmit(lost: Int, count: Int) {
        val req = byteArrayOf(
            0x80.toByte(), 0xD5.toByte(), (lost shr 8).toByte(), lost.toByte(),
            (lost shr 8).toByte(), lost.toByte(), (count shr 8).toByte(), count.toByte(),
        )
        controlSock.send(DatagramPacket(req, req.size, loopback, sessionControlPort))
    }

    override fun close() {
        closed = true
        rtsp.close()
        eventServer.close()
        audioSock.close()
        controlSock.close()
        eventSocket?.close()
        pool.shutdownNow()
    }

    private fun acceptRtsp() {
        while (!closed) {
            val sock = try {
                rtsp.accept()
            } catch (e: Exception) {
                return
            }
            serveRtsp(sock)
        }
    }

    private fun serveRtsp(sock: Socket) {
        val hap = Crypto.HapSession()
        val reader = Reader(sock.getInputStream(), hap)
        val out = sock.getOutputStream()
        while (true) {
            val req = try {
                reader.readRequest()
            } catch (e: Exception) {
                break
            }
            val ans = route(req)
            // Set before the reply goes out, so the sender's close() sees it when its TEARDOWN returns.
            if (req.method == "TEARDOWN") teardownSeen = true
            val head = "${req.proto} ${ans.code} OK\r\nCSeq: ${req.header("CSeq") ?: "0"}\r\n" +
                "Content-Length: ${ans.body.size}\r\n${ans.extra}\r\n"
            out.write(hap.encrypt(head.toByteArray(Charsets.ISO_8859_1) + ans.body))
            out.flush()
            ans.enable?.let { hap.enable(it.first, it.second) }
            if (req.method == "TEARDOWN") break
        }
        sock.close()
    }

    private fun route(req: Request): Answer = when {
        req.method == "GET" && req.uri == "/info" -> {
            val features = (1L shl 40) or (1L shl 41) or (1L shl 46) or (1L shl 48)
            Answer(200, "Content-Type: application/x-apple-binary-plist\r\n",
                Bplist.serialize(mapOf("features" to features, "model" to "MockSpeaker1,1")))
        }
        req.uri == "/pair-pin-start" -> Answer(200, "", ByteArray(0))
        req.uri == "/pair-setup" -> pairing(req)
        req.method == "SETUP" -> setup(req)
        req.method == "SET_PARAMETER" -> {
            val text = String(req.body, Charsets.UTF_8)
            when {
                text.startsWith("volume: ") -> volumeDb = text.removePrefix("volume: ").toDouble()
                text.startsWith("progress: ") -> progress = text
                req.header("Content-Type") == "application/x-dmap-tagged" -> metadataSeen = true
            }
            Answer(200, "", ByteArray(0))
        }
        req.method == "RECORD" -> {
            recordSeen = true
            Answer(200, "", ByteArray(0))
        }
        req.method == "FLUSH" -> {
            flushSeen = true
            Answer(200, "", ByteArray(0))
        }
        req.method == "POST" && req.uri == "/feedback" -> {
            feedbacks++
            Answer(200, "", ByteArray(0))
        }
        else -> Answer(200, "", ByteArray(0))
    }

    private fun pairing(req: Request): Answer {
        val state = Tlv8.get(req.body, Tlv8.STATE)?.firstOrNull()?.toInt()
        if (state == 1) {
            val salt = ByteArray(16).also { SecureRandom().nextBytes(it) }
            val bPriv = ByteArray(32).also { SecureRandom().nextBytes(it) }
            val s = Srp.serverStart("Pair-Setup", "3939", salt, bPriv)
            srp = s
            return Answer(200, "Content-Type: application/octet-stream\r\n", Tlv8.encode(
                Tlv8.encodeByte(Tlv8.STATE, 2),
                Tlv8.SALT to salt,
                Tlv8.PUBLIC_KEY to Srp.serverPublic(s),
            ))
        }
        if (state == 3) {
            val s = srp
            val a = Tlv8.get(req.body, Tlv8.PUBLIC_KEY)
            val m1 = Tlv8.get(req.body, Tlv8.PROOF)
            val proof = if (s != null && a != null && m1 != null) Srp.serverVerify(s, a, m1) else null
            if (proof == null) {
                return Answer(200, "", Tlv8.encode(Tlv8.encodeByte(Tlv8.STATE, 4), Tlv8.encodeByte(Tlv8.ERROR, 2)))
            }
            pairingOk = true
            sessionKey = proof.K
            // The sender encrypts with the Control-Write key, so we read with it and write with Control-Read.
            val enable = Pair(
                Crypto.hkdf32("Control-Salt", "Control-Read-Encryption-Key", proof.K),
                Crypto.hkdf32("Control-Salt", "Control-Write-Encryption-Key", proof.K),
            )
            return Answer(200, "Content-Type: application/octet-stream\r\n",
                Tlv8.encode(Tlv8.encodeByte(Tlv8.STATE, 4), Tlv8.PROOF to proof.M2), enable)
        }
        return Answer(200, "", ByteArray(0))
    }

    private fun setup(req: Request): Answer {
        val plist = Bplist.parse(req.body) as? Map<*, *> ?: return Answer(200, "", ByteArray(0))
        val streams = plist["streams"] as? List<*>
        if (streams == null) {
            // SETUP #1: remember the sender's timing port and open the event channel it will connect to.
            timingPort = (plist["timingPort"] as Number).toInt()
            pool.execute { acceptEvents() }
            val reply = Bplist.serialize(mapOf("eventPort" to eventServer.localPort, "timingPort" to 0))
            return Answer(200, "Content-Type: application/x-apple-binary-plist\r\n", reply)
        }
        val s0 = streams[0] as Map<*, *>
        streamCt = (s0["ct"] as Number).toLong()
        streamFormat = (s0["audioFormat"] as Number).toLong()
        shk = s0["shk"] as ByteArray
        shkMatches = shk.contentEquals(Crypto.hkdf32("Events-Salt", "Events-Write-Encryption-Key", sessionKey))
        sessionControlPort = (s0["controlPort"] as Number).toInt()
        val reply = Bplist.serialize(mapOf("streams" to listOf(mapOf(
            "dataPort" to audioSock.localPort,
            "controlPort" to controlSock.localPort,
        ))))
        return Answer(200, "Content-Type: application/x-apple-binary-plist\r\n", reply)
    }

    private fun acceptEvents() {
        val s = try {
            eventServer.accept()
        } catch (e: Exception) {
            return
        }
        // The sender reads with Events-Write and writes with Events-Read, so we do the reverse.
        val hap = Crypto.HapSession()
        hap.enable(
            Crypto.hkdf32("Events-Salt", "Events-Write-Encryption-Key", sessionKey),
            Crypto.hkdf32("Events-Salt", "Events-Read-Encryption-Key", sessionKey),
        )
        eventHap = hap
        eventSocket = s
    }

    private fun loopPackets(sock: DatagramSocket, handle: (ByteArray) -> Unit) {
        val buf = ByteArray(2048)
        while (!closed) {
            val p = DatagramPacket(buf, buf.size)
            try {
                sock.receive(p)
            } catch (e: Exception) {
                return
            }
            handle(buf.copyOf(p.length))
        }
    }

    private fun handleAudio(pkt: ByteArray) {
        val first = (pkt[1].toInt() and 0x80) != 0
        val seq = ((pkt[2].toInt() and 0xFF) shl 8) or (pkt[3].toInt() and 0xFF)
        val ts = beLong(pkt, 4, 4)
        val ssrc = beLong(pkt, 8, 4)
        val nonce = ByteArray(4) + pkt.copyOfRange(pkt.size - 8, pkt.size)
        val plain = Crypto.aeadDecrypt(shk, nonce, pkt.copyOfRange(4, 12), pkt.copyOfRange(12, pkt.size - 8))
        val pcm = if (plain == null) null else if (streamCt == 2L) Alac.unwrap(plain, 1408) else plain
        if (pcm == null) {
            badPackets++
            return
        }
        audio.add(Received(first, seq, ts, ssrc, pcm))
    }

    private fun handleControl(pkt: ByteArray) {
        val type = pkt[1].toInt() and 0x7F
        when {
            pkt.size == 20 && type == 0x54 -> syncPackets.add(pkt)
            type == 0x56 -> retransmits.add(pkt)
        }
    }

    private fun beLong(b: ByteArray, pos: Int, n: Int): Long {
        var v = 0L
        for (i in 0 until n) v = (v shl 8) or (b[pos + i].toLong() and 0xFF)
        return v
    }

    /** Reads whole requests (or raw replies) through a HAP session, which may be disabled. */
    private class Reader(private val input: InputStream, private val hap: Crypto.HapSession) {
        private var pending = ByteArray(0)
        private val crlf2 = "\r\n\r\n".toByteArray(Charsets.ISO_8859_1)

        private fun fill() {
            val tmp = ByteArray(4096)
            val n = input.read(tmp)
            require(n > 0) { "closed" }
            pending += hap.decrypt(tmp.copyOf(n)) ?: error("HAP frame failed authentication")
        }

        private fun endOfHead(): Int {
            for (i in 0..pending.size - 4) if (pending.copyOfRange(i, i + 4).contentEquals(crlf2)) return i
            return -1
        }

        fun readRequest(): Request {
            var end = endOfHead()
            while (end < 0) {
                fill()
                end = endOfHead()
            }
            val lines = String(pending, 0, end, Charsets.ISO_8859_1).split("\r\n")
            val parts = lines[0].split(" ")
            val headers = lines.drop(1)
            val len = headers.firstOrNull { it.substringBefore(':').equals("Content-Length", ignoreCase = true) }
                ?.substringAfter(':')?.trim()?.toInt() ?: 0
            while (pending.size < end + 4 + len) fill()
            val body = pending.copyOfRange(end + 4, end + 4 + len)
            pending = pending.copyOfRange(end + 4 + len, pending.size)
            return Request(parts[0], parts[1], parts[2], headers, body)
        }

        fun readRaw(): String {
            var end = endOfHead()
            while (end < 0) {
                fill()
                end = endOfHead()
            }
            val head = String(pending, 0, end, Charsets.ISO_8859_1)
            pending = pending.copyOfRange(end + 4, pending.size)
            return head
        }
    }
}
