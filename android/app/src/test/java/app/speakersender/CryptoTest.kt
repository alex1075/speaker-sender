package app.speakersender

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

// Mirrors test_hkdf, test_aead and test_hap_framing in c/tests/test_unit.c.
class CryptoTest {
    @Test
    fun hkdfMatchesGoldenKeys() {
        assertArrayEquals(
            Vectors.V_HKDF_CONTROL_WRITE,
            Crypto.hkdf32("Control-Salt", "Control-Write-Encryption-Key", Vectors.V_SRP_K),
        )
        assertArrayEquals(
            Vectors.V_HKDF_EVENTS_READ,
            Crypto.hkdf32("Events-Salt", "Events-Read-Encryption-Key", Vectors.V_SRP_K),
        )
    }

    @Test
    fun counterNonceIsZeroPrefixThenLittleEndianCounter() {
        assertArrayEquals(
            byteArrayOf(0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0),
            Crypto.counterNonce(5),
        )
        assertArrayEquals(
            byteArrayOf(0, 0, 0, 0, 2, 1, 0, 0, 0, 0, 0, 0),
            Crypto.counterNonce(0x0102),
        )
    }

    @Test
    fun aeadMatchesGoldenCiphertextAndRejectsTampering() {
        val nonce = Crypto.counterNonce(5)
        val aad = byteArrayOf(0x25, 0x00)
        val ct = Crypto.aeadEncrypt(Vectors.V_AEAD_KEY, nonce, aad, Vectors.V_AEAD_PT)
        assertArrayEquals(Vectors.V_AEAD_CT, ct)
        assertArrayEquals(Vectors.V_AEAD_PT, Crypto.aeadDecrypt(Vectors.V_AEAD_KEY, nonce, aad, ct))

        val bad = Vectors.V_AEAD_CT.copyOf()
        bad[3] = (bad[3].toInt() xor 1).toByte()
        assertNull(Crypto.aeadDecrypt(Vectors.V_AEAD_KEY, nonce, aad, bad))
        assertNull(Crypto.aeadDecrypt(Vectors.V_AEAD_KEY, nonce, aad, ByteArray(15)))
    }

    @Test
    fun hapStreamRoundTripsInAwkwardPieces() {
        val k1 = ByteArray(32) { 0x11 }
        val k2 = ByteArray(32) { 0x22 }
        val a = Crypto.HapSession()
        val b = Crypto.HapSession()
        a.enable(k1, k2) // a writes with k1, reads with k2
        b.enable(k2, k1)
        val msg = ByteArray(3000) { (it * 31).toByte() }
        val enc = a.encrypt(msg)
        // 1024 + 1024 + 952 byte frames, 2 length + 16 tag bytes each
        assertEquals(msg.size + 3 * 18, enc.size)

        val dec = java.io.ByteArrayOutputStream()
        var pos = 0
        var step = 7
        while (pos < enc.size) {
            val n = minOf(step, enc.size - pos)
            val plain = b.decrypt(enc.copyOfRange(pos, pos + n))
            assertNotNull(plain)
            dec.write(plain!!)
            pos += n
            step = step * 3 + 1
        }
        assertArrayEquals(msg, dec.toByteArray())
    }

    @Test
    fun hapStreamRejectsCorruptFrame() {
        val a = Crypto.HapSession()
        val b = Crypto.HapSession()
        a.enable(ByteArray(32) { 0x11 }, ByteArray(32) { 0x22 })
        b.enable(ByteArray(32) { 0x22 }, ByteArray(32) { 0x11 })
        val enc = a.encrypt(ByteArray(100) { (it * 31).toByte() })
        enc[10] = (enc[10].toInt() xor 0x80).toByte()
        assertNull(b.decrypt(enc))
    }
}
