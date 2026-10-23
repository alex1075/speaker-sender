package app.airplaysender

import java.io.Closeable
import java.io.IOException
import java.math.BigInteger
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.Socket
import java.security.SecureRandom
import java.util.Locale
import java.util.UUID

/** Where to send, as ap_target in c/src/airplay.h. Volume is 0..100 (0 means leave the speaker's volume alone). */
data class Target(
    val name: String,
    val host: String,
    val port: Int = 7000,
    val useAlac: Boolean = false,
    val volumePct: Double = 0.0,
    val pin: String = "3939",
    val title: String = "airplay-send",
    val artist: String = "airplay-send",
    val album: String = "airplay-send",
    val durationS: Double = 0.0,
)

/**
 * AirPlay 2 sender session: transient pairing, SETUP x2, event channel, volume/progress/metadata, RECORD, FLUSH,
 * then encrypted RTP audio with sync packets and timing replies. Ports ap_session_open and the threads in c/src/airplay.c.
 * Call open(), then sendPacket() once per 352-frame packet (pacing is the caller's job), then close().
 */
class AirPlaySession private constructor(private val t: Target) : Closeable {
    private val lock = Any()
    private val timing = DatagramSocket(0)
    private val ctrl = DatagramSocket(0)
    private val events = Crypto.HapSession()
    private val backlog = arrayOfNulls<Stored>(BACKLOG)
    private val threads = ArrayList<Thread>()
    private lateinit var conn: RtspConnection
    private var eventSocket: Socket? = null
    private var audio: DatagramSocket? = null
    private lateinit var remote: java.net.InetAddress
    private var remoteCtrlPort = 0
    private var shk = ByteArray(0)
    private var seq = SecureRandom().nextInt(0x10000)
    private var framesSent = 0L
    private var aeadCtr = 0L
    private var ntpStart = Clock.ntpNow()
    private var firstPacket = true
    private var firstSync = true
    @Volatile private var syncReady = false
    @Volatile private var stop = false
    @Volatile private var dead = false
    private var closed = false

    private class Stored(val seq: Int, val bytes: ByteArray)

    /** False once the speaker stops answering feedback. */
    val alive: Boolean get() = !dead

    companion object {
        const val SAMPLE_RATE = 44100
        const val FRAMES_PER_PACKET = 352
        const val PACKET_PCM_BYTES = FRAMES_PER_PACKET * 4
        const val LATENCY_FRAMES = 22050 + SAMPLE_RATE
        private const val BACKLOG = 1000
        private const val CONTROL_SALT = "Control-Salt"
        private const val CONTROL_WRITE = "Control-Write-Encryption-Key"
        private const val CONTROL_READ = "Control-Read-Encryption-Key"
        private const val EVENTS_SALT = "Events-Salt"
        private const val EVENTS_WRITE = "Events-Write-Encryption-Key"
        private const val EVENTS_READ = "Events-Read-Encryption-Key"
        private const val FAKE_MAC = "AA:BB:CC:DD:EE:FF"
        private const val SETUP_CT = "application/x-apple-binary-plist"

        /** Connects and sets the stream up. Throws IOException with the reason on failure. */
        fun open(t: Target): AirPlaySession {
            val s = AirPlaySession(t)
            try {
                s.start()
            } catch (e: Exception) {
                s.close()
                throw e
            }
            return s
        }
    }

    private fun start() {
        // The timing responder must answer before SETUP #1, or some speakers never reply to SETUP #2.
        startThread("timing") { timingLoop() }
        startThread("control") { controlLoop() }
        startThread("sync") { syncLoop() }

        conn = RtspConnection(t.host, t.port, 10000)
        remote = conn.remoteAddress

        val info = conn.exchange("GET", "/info")
        val features = if (info.ok) (Bplist.parse(info.body) as? Map<*, *>)?.get("features") as? Number else null
        val proto = if (features != null) Protocol.protoFromFeatures(features.toLong().toULong()) else 2
        if (proto == 1) throw IOException("${t.name}: AirPlay 1 (RAOP) is not ported yet")

        val k = pairSetup()
        conn.enableEncryption(
            Crypto.hkdf32(CONTROL_SALT, CONTROL_WRITE, k),
            Crypto.hkdf32(CONTROL_SALT, CONTROL_READ, k),
        )

        val eventPort = setupOne(k)
        openEvents(eventPort, k)
        setupTwo()

        if (t.volumePct > 0) {
            val vol = String.format(Locale.US, "volume: %.6f", Protocol.volumeDb(t.volumePct))
            conn.exchange("SET_PARAMETER", contentType = "text/parameters", body = vol.toByteArray())
        }
        val duration = if (t.durationS > 0) t.durationS else 12.0 * 3600.0
        val end = LATENCY_FRAMES + (duration * SAMPLE_RATE).toLong()
        conn.exchange("SET_PARAMETER", contentType = "text/parameters", body = "progress: $LATENCY_FRAMES/$LATENCY_FRAMES/$end".toByteArray())

        val pl = dmapString("minm", t.title) + dmapString("asal", t.album) + dmapString("asar", t.artist)
        val md = "mlit".toByteArray(Charsets.US_ASCII) + beBytes(pl.size.toLong(), 4) + pl
        conn.exchange("SET_PARAMETER", contentType = "application/x-dmap-tagged", extraHeaders = listOf("Session: 0", rtpInfo()), body = md)

        startThread("keep") { keepLoop() }

        val record = conn.exchange("RECORD")
        if (!record.ok) throw IOException("${t.name}: RECORD failed (${record.statusLine})")
        val flush = conn.exchange("FLUSH", extraHeaders = listOf("Range: npt=0-", "Session: 0", rtpInfo()))
        if (!flush.ok) throw IOException("${t.name}: FLUSH failed (${flush.statusLine})")
        log("${t.name}: ready")
    }

    private fun rtpInfo(): String = "RTP-Info: seq=${seq and 0xFFFF};rtptime=$LATENCY_FRAMES"

    /** Pair-Setup M1 to M4 with PIN and transient flag. Returns the 64-byte session key K. */
    private fun pairSetup(): ByteArray {
        val hdrs = listOf("User-Agent: AirPlay/320.20", "Connection: keep-alive", "X-Apple-HKP: 4", "Content-Type: application/octet-stream")
        conn.request("POST", "/pair-pin-start", "HTTP/1.1", null, hdrs, ByteArray(0))

        val m1 = Tlv8.encode(Tlv8.encodeByte(Tlv8.METHOD, 0), Tlv8.encodeByte(Tlv8.STATE, 1), Tlv8.encodeByte(Tlv8.FLAGS, 0x10))
        val r1 = conn.request("POST", "/pair-setup", "HTTP/1.1", null, hdrs, m1)
        if (r1.code != 200 || r1.body.isEmpty()) throw IOException("${t.name}: pair-setup M1 rejected (HTTP ${r1.code})")
        Tlv8.get(r1.body, Tlv8.ERROR)?.let {
            throw IOException("${t.name}: pair-setup M2 error 0x%02x (may need a PIN or password)".format(it.firstOrNull()?.toInt() ?: 0))
        }
        val salt = Tlv8.get(r1.body, Tlv8.SALT) ?: throw IOException("${t.name}: pair-setup M2 missing salt")
        val bPub = Tlv8.get(r1.body, Tlv8.PUBLIC_KEY) ?: throw IOException("${t.name}: pair-setup M2 missing public key")

        val aPriv = ByteArray(32).also { SecureRandom().nextBytes(it) }
        val proof = Srp.clientProof("Pair-Setup", t.pin, salt, aPriv, bPub) ?: throw IOException("${t.name}: SRP failed")
        val m3 = Tlv8.encode(
            Tlv8.encodeByte(Tlv8.STATE, 3),
            Tlv8.PUBLIC_KEY to Srp.clientPublic(aPriv),
            Tlv8.PROOF to proof.M1,
        )
        val r3 = conn.request("POST", "/pair-setup", "HTTP/1.1", null, hdrs, m3)
        if (r3.code != 200) throw IOException("${t.name}: pair-setup M3 rejected (HTTP ${r3.code})")
        Tlv8.get(r3.body, Tlv8.ERROR)?.let { throw IOException("${t.name}: pair-setup M4 error (wrong PIN?)") }
        // As in C, a server proof that differs from ours is logged and ignored (same as pyatv).
        Tlv8.get(r3.body, Tlv8.PROOF)?.let { if (!it.contentEquals(proof.M2)) log("${t.name}: server proof differs (ignored)") }
        return proof.K
    }

    /** SETUP #1 (timing port in, event port out). */
    private fun setupOne(k: ByteArray): Int {
        val body = Bplist.serialize(
            mapOf(
                "deviceID" to FAKE_MAC,
                "sessionUUID" to UUID.randomUUID().toString().uppercase(Locale.US),
                "timingPort" to timing.localPort,
                "timingProtocol" to "NTP",
                "isMultiSelectAirPlay" to true,
                "groupContainsGroupLeader" to false,
                "macAddress" to FAKE_MAC,
                "model" to "iPhone14,3",
                "name" to "airplay-send",
                "osBuildVersion" to "20F66",
                "osName" to "iPhone OS",
                "osVersion" to "16.5",
                "senderSupportsRelay" to false,
                "sourceVersion" to "690.7.1",
                "statsCollectionEnabled" to false,
            ),
        )
        val r = conn.exchange("SETUP", contentType = SETUP_CT, body = body)
        if (!r.ok) throw IOException("${t.name}: SETUP failed (${r.statusLine})")
        val port = (Bplist.parse(r.body) as? Map<*, *>)?.get("eventPort") as? Number
        if (port == null || port.toInt() <= 0) throw IOException("${t.name}: SETUP reply had no eventPort")
        return port.toInt()
    }

    /** Opens the event channel (retrying: some speakers listen a moment after replying) and turns on its HAP framing. */
    private fun openEvents(port: Int, k: ByteArray) {
        val evOut = Crypto.hkdf32(EVENTS_SALT, EVENTS_READ, k)
        val evIn = Crypto.hkdf32(EVENTS_SALT, EVENTS_WRITE, k)
        shk = evIn // the audio key is the first 32 bytes of the events write key
        var last: IOException? = null
        repeat(5) {
            try {
                val s = Socket()
                s.connect(InetSocketAddress(remote, port), 3000)
                s.tcpNoDelay = true
                s.soTimeout = 100
                eventSocket = s
                events.enable(evOut, evIn)
                return
            } catch (e: IOException) {
                last = e
                Thread.sleep(1000)
            }
        }
        throw IOException("${t.name}: could not open the event channel", last)
    }

    /** SETUP #2 (audio stream). Returns nothing; sets up the UDP audio and control destinations. */
    private fun setupTwo() {
        val stream = mapOf(
            "audioFormat" to (if (t.useAlac) 0x40000 else 0x800),
            "audioMode" to "default",
            "controlPort" to ctrl.localPort,
            "ct" to (if (t.useAlac) 2 else 1),
            "isMedia" to true,
            "latencyMax" to 88200,
            "latencyMin" to 11025,
            "shk" to shk,
            "spf" to FRAMES_PER_PACKET,
            "sr" to SAMPLE_RATE,
            "type" to 0x60,
            "supportsDynamicStreamID" to false,
            "streamConnectionID" to conn.sessionId,
        )
        val r = conn.exchange("SETUP", contentType = SETUP_CT, body = Bplist.serialize(mapOf("streams" to listOf(stream))))
        if (!r.ok) throw IOException("${t.name}: audio SETUP failed (${r.statusLine})")
        val s0 = ((Bplist.parse(r.body) as? Map<*, *>)?.get("streams") as? List<*>)?.firstOrNull() as? Map<*, *>
        val dataPort = (s0?.get("dataPort") as? Number)?.toInt() ?: 0
        remoteCtrlPort = (s0?.get("controlPort") as? Number)?.toInt() ?: 0
        if (dataPort <= 0 || remoteCtrlPort <= 0) throw IOException("${t.name}: audio SETUP reply had no dataPort/controlPort")
        audio = DatagramSocket().also { it.connect(InetSocketAddress(remote, dataPort)) }
    }

    /**
     * Starts the audio clock (ap_session_begin). Call right before the first packet. Sync packets start only
     * here, so none goes out before the anchor they describe is set.
     */
    fun beginClock() {
        synchronized(lock) {
            ntpStart = Clock.ntpNow()
            framesSent = 0
            firstSync = true
        }
        syncReady = true
    }

    /** One packet: PACKET_PCM_BYTES of interleaved 16-bit little-endian stereo. Swapped to big endian as AirPlay wants. */
    fun sendPacket(pcmLe: ByteArray) {
        check(!dead) { "${t.name}: session is dead" }
        require(pcmLe.size == PACKET_PCM_BYTES) { "packet must be $PACKET_PCM_BYTES bytes" }
        val be = ByteArray(PACKET_PCM_BYTES) { pcmLe[it xor 1] }
        val payload = if (t.useAlac) Alac.wrap(be) else be
        synchronized(lock) {
            val ts = (framesSent + LATENCY_FRAMES) and 0xFFFFFFFFL
            val pkt = Packets.buildAudio(shk, aeadCtr++, firstPacket, seq, ts, conn.sessionId, payload)
            backlog[seq % BACKLOG] = Stored(seq, pkt)
            audio!!.send(DatagramPacket(pkt, pkt.size))
            seq = (seq + 1) and 0xFFFF
            framesSent += FRAMES_PER_PACKET
            firstPacket = false
        }
    }

    override fun close() {
        if (closed) return
        closed = true
        if (::conn.isInitialized && !dead) {
            runCatching { conn.exchange("TEARDOWN", extraHeaders = listOf("Session: 0")) }
        }
        stop = true
        audio?.close()
        ctrl.close()
        timing.close()
        eventSocket?.close()
        if (::conn.isInitialized) conn.close()
        threads.forEach { it.join(1000) }
    }

    private fun startThread(name: String, body: () -> Unit) {
        val th = Thread(body, "airplay-$name")
        th.isDaemon = true
        threads.add(th)
        th.start()
    }

    /** Answers NTP timing requests (32 bytes) on the timing port. */
    private fun timingLoop() {
        val buf = ByteArray(64)
        while (!stop) {
            val p = DatagramPacket(buf, buf.size)
            try {
                timing.receive(p)
            } catch (e: IOException) {
                return
            }
            val reply = Packets.buildTimingReply(buf.copyOf(p.length), Clock.ntpNow()) ?: continue
            runCatching { timing.send(DatagramPacket(reply, reply.size, p.socketAddress)) }
        }
    }

    /** Retransmit requests (payload type 0x55) on the control port: resend from the backlog. */
    private fun controlLoop() {
        val buf = ByteArray(512)
        while (!stop) {
            val p = DatagramPacket(buf, buf.size)
            try {
                ctrl.receive(p)
            } catch (e: IOException) {
                return
            }
            if (p.length < 8 || (buf[1].toInt() and 0x7F) != 0x55) continue
            val lost = ((buf[4].toInt() and 0xFF) shl 8) or (buf[5].toInt() and 0xFF)
            val count = ((buf[6].toInt() and 0xFF) shl 8) or (buf[7].toInt() and 0xFF)
            for (i in 0 until minOf(count, 64)) {
                val q = (lost + i) and 0xFFFF
                val b = synchronized(lock) { backlog[q % BACKLOG] } ?: continue
                if (b.seq != q) continue
                val resp = byteArrayOf(0x80.toByte(), 0xD6.toByte(), b.bytes[2], b.bytes[3]) + b.bytes
                runCatching { ctrl.send(DatagramPacket(resp, resp.size, p.socketAddress)) }
            }
        }
    }

    /** Sync packet on the control port every second, once the clock anchor is set. */
    private fun syncLoop() {
        var next = Clock.nowNs()
        while (!stop) {
            if (!syncReady) {
                Thread.sleep(5)
                next = Clock.nowNs()
                continue
            }
            Clock.sleepUntil(next)
            if (stop) return
            val (sent, ntp, first) = synchronized(lock) {
                val f = firstSync
                firstSync = false
                Triple(framesSent, ntpStart + ((BigInteger.valueOf(framesSent).shiftLeft(32)).divide(BigInteger.valueOf(SAMPLE_RATE.toLong())).toLong()), f)
            }
            val sp = Packets.buildSync(first, sent, ntp, sent + LATENCY_FRAMES)
            runCatching { ctrl.send(DatagramPacket(sp, sp.size, InetSocketAddress(remote, remoteCtrlPort))) }
            next += 1_000_000_000L
            if (next < Clock.nowNs()) next = Clock.nowNs() + 1_000_000_000L
        }
    }

    /** Feedback every 2 s, and event requests from the speaker (answered with 200 OK). */
    private fun keepLoop() {
        var nextFeedback = Clock.nowNs() + 2_000_000_000L
        var failures = 0
        while (!stop) {
            val es = eventSocket
            if (es != null) readEvents(es) else Thread.sleep(100)
            if (Clock.nowNs() >= nextFeedback) {
                val ok = try {
                    conn.exchange("POST", "/feedback").ok
                } catch (e: IOException) {
                    false
                }
                failures = if (ok) 0 else failures + 1
                if (failures >= 3) {
                    log("${t.name}: connection lost (feedback failed)")
                    dead = true
                    return
                }
                nextFeedback = Clock.nowNs() + 2_000_000_000L
            }
        }
    }

    private var eventBytes = ByteArray(0)

    private fun readEvents(es: Socket) {
        val tmp = ByteArray(4096)
        val n = try {
            es.getInputStream().read(tmp)
        } catch (e: java.net.SocketTimeoutException) {
            return
        } catch (e: IOException) {
            return
        }
        if (n < 0) return
        eventBytes += events.decrypt(tmp.copyOf(n)) ?: return
        while (true) {
            val end = indexOf(eventBytes, "\r\n\r\n")
            if (end < 0) return
            val head = String(eventBytes, 0, end, Charsets.ISO_8859_1)
            val len = Regex("(?i)Content-Length:\\s*(\\d+)").find(head)?.groupValues?.get(1)?.toInt() ?: 0
            if (eventBytes.size < end + 4 + len) return
            val firstLine = head.substringBefore("\r\n")
            val proto = firstLine.substringAfterLast(' ', "RTSP/1.0")
            val cseq = Regex("(?i)CSeq:[^\r\n]*").find(head)?.value
            val reply = buildString {
                append("$proto 200 OK\r\nContent-Length: 0\r\nAudio-Latency: 0")
                if (cseq != null) append("\r\n").append(cseq)
                append("\r\n\r\n")
            }
            val out = events.encrypt(reply.toByteArray(Charsets.ISO_8859_1))
            runCatching { es.getOutputStream().write(out) }
            eventBytes = eventBytes.copyOfRange(end + 4 + len, eventBytes.size)
        }
    }

    private fun dmapString(tag: String, value: String): ByteArray {
        val v = value.toByteArray(Charsets.UTF_8)
        return tag.toByteArray(Charsets.US_ASCII) + beBytes(v.size.toLong(), 4) + v
    }

    private fun indexOf(buf: ByteArray, pat: String): Int {
        val p = pat.toByteArray(Charsets.ISO_8859_1)
        for (i in 0..buf.size - p.size) {
            var j = 0
            while (j < p.size && buf[i + j] == p[j]) j++
            if (j == p.size) return i
        }
        return -1
    }

    private fun log(msg: String) = System.err.println("airplay: $msg")
}
