package app.airplaysender

/** The low `n` bytes of `v`, big-endian. Longs keep unsigned 32- and 64-bit values from going negative. */
internal fun beBytes(v: Long, n: Int): ByteArray = ByteArray(n) { (v ushr (8 * (n - 1 - it))).toByte() }
