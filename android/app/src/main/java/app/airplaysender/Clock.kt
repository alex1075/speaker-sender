package app.airplaysender

import java.time.Instant

/** Monotonic time for pacing, and NTP time (64-bit: seconds since 1900 high, fraction low) for the sync and timing packets. */
object Clock {
    fun nowNs(): Long = System.nanoTime()

    fun ntpNow(): Long {
        val now = Instant.now()
        val sec = now.epochSecond + 0x83AA7E80L // 1900 -> 1970
        val frac = (now.nano.toLong() shl 32) / 1_000_000_000L
        return (sec shl 32) or (frac and 0xFFFFFFFFL)
    }

    fun sleepUntil(deadlineNs: Long) {
        while (true) {
            val d = deadlineNs - System.nanoTime()
            if (d <= 0) return
            Thread.sleep(d / 1_000_000L, (d % 1_000_000L).toInt())
        }
    }
}
