package app.speakersender

import java.io.ByteArrayOutputStream
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.IvParameterSpec
import javax.crypto.spec.SecretKeySpec

/** HKDF-SHA512, ChaCha20-Poly1305 and the HAP frame stream, as in c/src/crypto.c. JDK/Android providers only. */
object Crypto {
    private const val HAP_FRAME = 1024

    /** HKDF-SHA512 extract with `salt`, expand with `info`, first 32 bytes (one block is enough). */
    fun hkdf32(salt: String, info: String, ikm: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA512")
        mac.init(SecretKeySpec(salt.toByteArray(Charsets.US_ASCII), "HmacSHA512"))
        val prk = mac.doFinal(ikm)
        mac.init(SecretKeySpec(prk, "HmacSHA512"))
        return mac.doFinal(info.toByteArray(Charsets.US_ASCII) + byteArrayOf(1)).copyOf(32)
    }

    /** 4 zero bytes, then the 64-bit little-endian counter. */
    fun counterNonce(counter: Long): ByteArray = ByteArray(12) {
        if (it < 4) 0.toByte() else (counter ushr (8 * (it - 4))).toByte()
    }

    /** Returns ciphertext || 16-byte tag. */
    fun aeadEncrypt(key: ByteArray, nonce: ByteArray, aad: ByteArray, plain: ByteArray): ByteArray {
        val c = Cipher.getInstance("ChaCha20-Poly1305")
        c.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "ChaCha20"), IvParameterSpec(nonce))
        if (aad.isNotEmpty()) c.updateAAD(aad)
        return c.doFinal(plain)
    }

    /** `sealed` is ciphertext || tag. Returns null on authentication failure. */
    fun aeadDecrypt(key: ByteArray, nonce: ByteArray, aad: ByteArray, sealed: ByteArray): ByteArray? {
        if (sealed.size < 16) return null
        val c = Cipher.getInstance("ChaCha20-Poly1305")
        c.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "ChaCha20"), IvParameterSpec(nonce))
        if (aad.isNotEmpty()) c.updateAAD(aad)
        return try {
            c.doFinal(sealed)
        } catch (e: AEADBadTagException) {
            null
        }
    }

    /** Encrypted HAP stream: frames of up to 1024 bytes, each prefixed by its 2-byte little-endian length (also the AAD). */
    class HapSession {
        private var enabled = false
        private var outKey = ByteArray(0)
        private var inKey = ByteArray(0)
        private var outCtr = 0L
        private var inCtr = 0L
        private var inbuf = ByteArray(0) // ciphertext not yet a whole frame

        fun enable(outKey: ByteArray, inKey: ByteArray) {
            this.outKey = outKey
            this.inKey = inKey
            outCtr = 0
            inCtr = 0
            enabled = true
        }

        fun encrypt(data: ByteArray): ByteArray {
            if (!enabled) return data
            val out = ByteArrayOutputStream()
            var pos = 0
            while (pos < data.size) {
                val n = minOf(HAP_FRAME, data.size - pos)
                val lenb = byteArrayOf((n and 0xFF).toByte(), (n shr 8).toByte())
                out.write(lenb)
                out.write(aeadEncrypt(outKey, counterNonce(outCtr++), lenb, data.copyOfRange(pos, pos + n)))
                pos += n
            }
            return out.toByteArray()
        }

        /** Returns the plaintext of every complete frame so far (partial frames wait for more input). Null on auth failure. */
        fun decrypt(data: ByteArray): ByteArray? {
            if (!enabled) return data
            inbuf += data
            val out = ByteArrayOutputStream()
            while (inbuf.size >= 2) {
                val n = (inbuf[0].toInt() and 0xFF) or ((inbuf[1].toInt() and 0xFF) shl 8)
                if (n > HAP_FRAME) return null
                if (inbuf.size < 2 + n + 16) break
                val plain = aeadDecrypt(
                    inKey,
                    counterNonce(inCtr),
                    inbuf.copyOfRange(0, 2),
                    inbuf.copyOfRange(2, 2 + n + 16),
                ) ?: return null
                inCtr++
                out.write(plain)
                inbuf = inbuf.copyOfRange(2 + n + 16, inbuf.size)
            }
            return out.toByteArray()
        }
    }
}
