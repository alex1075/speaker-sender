package app.airplaysender

import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.security.SecureRandom

/** A parsed RTSP/HTTP response. Any status code is a Response; only I/O and framing errors throw. */
class Response(val code: Int, val statusLine: String, val headers: List<Pair<String, String>>, val body: ByteArray) {
    val ok: Boolean get() = code in 200..299

    fun header(name: String): String? = headers.firstOrNull { it.first.equals(name, ignoreCase = true) }?.second
}

/**
 * RTSP/HTTP client over one TCP socket, as c/src/rtsp.c. Optionally HAP-encrypted after pairing.
 * exchange() adds CSeq, DACP-ID, Active-Remote and Client-Instance, and is serialised.
 */
class RtspConnection(host: String, port: Int, timeoutMs: Int) : Closeable {
    private val socket: Socket
    private val input: InputStream
    private val output: OutputStream
    private val hap = Crypto.HapSession()
    private val lock = Any()
    private var pending = ByteArray(0) // decrypted bytes not yet consumed
    private var cseq = 0
    val dacpId: String
    val activeRemote: Long
    val sessionId: Long
    val localIp: String
    val uri: String

    init {
        val addr = InetAddress.getAllByName(host).firstOrNull { it is Inet4Address }
            ?: throw IOException("cannot resolve $host")
        socket = Socket()
        socket.connect(InetSocketAddress(addr, port), timeoutMs)
        socket.tcpNoDelay = true
        socket.soTimeout = timeoutMs
        input = socket.getInputStream()
        output = socket.getOutputStream()
        localIp = (socket.localAddress as Inet4Address).hostAddress ?: ""
        val rnd = ByteArray(16).also { SecureRandom().nextBytes(it) }
        sessionId = ByteBuffer.wrap(rnd, 0, 4).int.toLong() and 0xFFFFFFFFL
        activeRemote = ByteBuffer.wrap(rnd, 4, 4).int.toLong() and 0xFFFFFFFFL
        dacpId = rnd.copyOfRange(8, 16).joinToString("") { "%02X".format(it.toInt() and 0xFF) }
        uri = "rtsp://$localIp/$sessionId"
    }

    /** Turns on HAP framing for everything sent and received from now on (after pair-verify). */
    fun enableEncryption(outKey: ByteArray, inKey: ByteArray) = hap.enable(outKey, inKey)

    /** Sends "method uri protocol" and reads one response. For the HTTP-style pairing calls. */
    fun request(
        method: String,
        uri: String,
        protocol: String,
        contentType: String?,
        headers: List<String>,
        body: ByteArray,
    ): Response {
        val head = StringBuilder("$method $uri $protocol")
        if (headers.none { it.startsWith("User-Agent:", ignoreCase = true) }) head.append("\r\nUser-Agent: $USER_AGENT")
        if (contentType != null) head.append("\r\nContent-Type: $contentType")
        if (body.isNotEmpty()) head.append("\r\nContent-Length: ${body.size}")
        for (h in headers) head.append("\r\n").append(h)
        head.append("\r\n\r\n")
        val msg = head.toString().toByteArray(Charsets.ISO_8859_1) + body
        output.write(hap.encrypt(msg))
        output.flush()
        return readResponse()
    }

    /** RTSP exchange on the session URI (or `uri`). Returns the response for any status code. */
    fun exchange(
        method: String,
        uri: String? = null,
        contentType: String? = null,
        extraHeaders: List<String> = emptyList(),
        body: ByteArray = ByteArray(0),
    ): Response = synchronized(lock) {
        require(extraHeaders.size <= 11) { "at most 11 extra headers, as in rtsp_exchange" }
        val headers = listOf(
            "CSeq: ${cseq++}",
            "DACP-ID: $dacpId",
            "Active-Remote: $activeRemote",
            "Client-Instance: $dacpId",
        ) + extraHeaders
        request(method, uri ?: this.uri, "RTSP/1.0", contentType, headers, body)
    }

    override fun close() {
        socket.close()
    }

    private fun readMore() {
        val tmp = ByteArray(4096)
        val n = input.read(tmp)
        if (n < 0) throw IOException("RTSP connection closed by the speaker")
        pending += hap.decrypt(tmp.copyOf(n)) ?: throw IOException("HAP frame failed authentication")
    }

    private fun readResponse(): Response {
        var end = indexOf(pending, CRLF2)
        while (end < 0) {
            if (pending.size > MAX_HEAD) throw IOException("RTSP header too large")
            readMore()
            end = indexOf(pending, CRLF2)
        }
        val head = parseHead(String(pending, 0, end, Charsets.ISO_8859_1))
        val length = head.header("Content-Length")?.trim()?.toLongOrNull() ?: 0L
        if (length < 0 || length > MAX_BODY) throw IOException("RTSP body too large")
        val bodyStart = end + 4
        val total = bodyStart + length.toInt()
        while (pending.size < total) readMore()
        val body = pending.copyOfRange(bodyStart, total)
        pending = pending.copyOfRange(total, pending.size)
        return Response(head.code, head.statusLine, head.headers, body)
    }

    private fun parseHead(text: String): Response {
        val lines = text.split("\n").map { it.removeSuffix("\r") }
        val statusLine = lines[0]
        val code = statusLine.substringAfter(' ', "").takeWhile { it.isDigit() }.toIntOrNull() ?: 0
        val headers = ArrayList<Pair<String, String>>()
        for (line in lines.drop(1)) {
            if (line.isEmpty()) break
            val colon = line.indexOf(':')
            if (colon < 0) continue
            headers.add(line.substring(0, colon) to line.substring(colon + 1).trimStart(' ', '\t'))
        }
        return Response(code, statusLine, headers, ByteArray(0))
    }

    private companion object {
        const val USER_AGENT = "AirPlay/550.10"
        const val MAX_HEAD = 1 shl 20
        const val MAX_BODY = 16L shl 20
        val CRLF2 = "\r\n\r\n".toByteArray(Charsets.ISO_8859_1)

        fun indexOf(buf: ByteArray, pat: ByteArray): Int {
            for (i in 0..buf.size - pat.size) {
                var j = 0
                while (j < pat.size && buf[i + j] == pat[j]) j++
                if (j == pat.size) return i
            }
            return -1
        }
    }
}
