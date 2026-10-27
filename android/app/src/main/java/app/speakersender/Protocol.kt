package app.speakersender

/** Pure protocol helpers shared in spirit with c/src/airplay.c (same rules, same test vectors). */
object Protocol {
    const val SAMPLE_RATE = 44100
    const val FRAMES_PER_PACKET = 352
    const val FRAME_BYTES = 4
    const val PACKET_PCM_BYTES = FRAMES_PER_PACKET * FRAME_BYTES // 1408

    /** Feature bits that mark an AirPlay 2 receiver: buffered audio, PTP clock, HomeKit pairing, transient pairing. */
    private val AIRPLAY2_BITS: ULong = (1UL shl 40) or (1UL shl 41) or (1UL shl 46) or (1UL shl 48)

    /** Parses the mDNS/`GET /info` features value: "0x445F8A00,0x1C340" is low word, high word. 0 if empty or invalid. */
    fun parseFeatures(text: String?): ULong {
        if (text.isNullOrBlank()) return 0UL
        val parts = text.trim().split(",")
        fun word(s: String): ULong? = s.trim().removePrefix("0x").removePrefix("0X").toULongOrNull(16)
        val low = word(parts[0]) ?: return 0UL
        val high = if (parts.size > 1) word(parts[1]) ?: 0UL else 0UL
        return (high shl 32) or (low and 0xFFFFFFFFUL)
    }

    /** 2 for an AirPlay 2 receiver, otherwise 1 (legacy RAOP). */
    fun protoFromFeatures(features: ULong): Int = if (features and AIRPLAY2_BITS != 0UL) 2 else 1

    /** Volume percent (0..100) to the dB value sent in SET_PARAMETER: 0 is mute (-144), else -30..0 dB. */
    fun volumeDb(percent: Double): Double = when {
        percent <= 0.0 -> -144.0
        percent > 100.0 -> 0.0
        else -> -30.0 + percent / 100.0 * 30.0
    }
}
