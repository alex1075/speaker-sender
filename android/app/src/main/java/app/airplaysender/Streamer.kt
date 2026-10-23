package app.airplaysender

import java.io.InputStream

/**
 * Sends packets at real time on an absolute clock, as the loop in c/src/stream.c. A packet is due at
 * start + frames / 44100 s, so timing does not drift. A stall of over a second re-anchors the clock
 * instead of sending a burst.
 */
object Streamer {
    fun run(session: AirPlaySession, packets: Iterator<ByteArray>) {
        session.beginClock()
        var t0 = Clock.nowNs()
        var frames = 0L
        while (packets.hasNext()) {
            val due = t0 + frames * 1_000_000_000L / AirPlaySession.SAMPLE_RATE
            val now = Clock.nowNs()
            if (now < due) {
                Clock.sleepUntil(due)
            } else if (now - due > 1_000_000_000L) {
                t0 += now - due
            }
            session.sendPacket(packets.next())
            frames += AirPlaySession.FRAMES_PER_PACKET
        }
    }

    /**
     * Whole packets from 16-bit stereo PCM. Each read fills a full packet, so no 4-byte frame is split.
     * The last packet is padded with silence.
     */
    fun packets(input: InputStream): Iterator<ByteArray> = object : Iterator<ByteArray> {
        private var ahead: ByteArray? = read()

        private fun read(): ByteArray? {
            val buf = ByteArray(AirPlaySession.PACKET_PCM_BYTES)
            var n = 0
            while (n < buf.size) {
                val r = input.read(buf, n, buf.size - n)
                if (r < 0) break
                n += r
            }
            return if (n == 0) null else buf
        }

        override fun hasNext(): Boolean = ahead != null

        override fun next(): ByteArray {
            val cur = ahead ?: throw NoSuchElementException()
            ahead = read()
            return cur
        }
    }
}
