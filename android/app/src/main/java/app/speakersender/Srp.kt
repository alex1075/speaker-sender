package app.speakersender

import java.math.BigInteger
import java.security.MessageDigest

/**
 * SRP-6a client for HAP Pair-Setup, as in the client half of c/src/srp.c.
 * RFC 5054 / RFC 3526 3072-bit group, generator 5, SHA-512. Hashing conventions match the C code:
 * PAD() is 384 bytes, "min" is big-endian without leading zero bytes, and H(N) xor H(g) uses g as one byte.
 */
object Srp {
    const val N_BYTES = 384
    const val HASH_BYTES = 64

    private const val N_HEX = "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF"
    private val N = BigInteger(N_HEX, 16)
    private val G = BigInteger.valueOf(5)
    private val MULT = BigInteger(1, sha512(pad(N), pad(G))) // k = H(N | PAD(g))

    /** Session key K and client proof M1 from the client side. M2 is what the server must send back. */
    data class Proof(val M1: ByteArray, val K: ByteArray, val M2: ByteArray)

    /** A = g^a mod N, padded to 384 bytes. `aPriv` is 32 random bytes. */
    fun clientPublic(aPriv: ByteArray): ByteArray = pad(G.modPow(BigInteger(1, aPriv), N))

    /** Server state for the mock receiver: b, B = k*v + g^b, v = g^x. */
    class Server(val user: String, val salt: ByteArray, val bSecret: BigInteger, val bPub: BigInteger, val v: BigInteger)

    /** Server half, for tests and the mock receiver. `bPriv` is 32 random bytes. */
    fun serverStart(user: String, pass: String, salt: ByteArray, bPriv: ByteArray): Server {
        val b = BigInteger(1, bPriv)
        val v = G.modPow(xOf(user, pass, salt), N)
        val bigB = MULT.multiply(v).add(G.modPow(b, N)).mod(N)
        return Server(user, salt, b, bigB, v)
    }

    fun serverPublic(s: Server): ByteArray = pad(s.bPub)

    /** Checks the client's A and M1. Returns K and M2 when M1 is right, otherwise null. */
    fun serverVerify(s: Server, A: ByteArray, M1: ByteArray): Proof? {
        if (A.isEmpty() || A.size > N_BYTES) return null
        val aBig = BigInteger(1, A)
        if (aBig.mod(N).signum() == 0) return null
        val u = BigInteger(1, sha512(pad(aBig), pad(s.bPub)))
        val S = s.v.modPow(u, N).multiply(aBig).mod(N).modPow(s.bSecret, N)
        val k = sha512(minBytes(S))
        val expect = m1(s.user, s.salt, aBig, s.bPub, k)
        if (!MessageDigest.isEqual(expect, M1)) return null
        return Proof(expect, k, sha512(minBytes(aBig), expect, k))
    }

    /** x = H(salt | H(user ":" pass)) */
    private fun xOf(user: String, pass: String, salt: ByteArray): BigInteger =
        BigInteger(1, sha512(salt, sha512(user.toByteArray(), ":".toByteArray(), pass.toByteArray())))

    /** Returns null if B is 0 mod N, which SRP requires the client to refuse. */
    fun clientProof(user: String, pass: String, salt: ByteArray, aPriv: ByteArray, B: ByteArray): Proof? {
        if (B.isEmpty() || B.size > N_BYTES) return null
        val bn = BigInteger(1, B)
        if (bn.mod(N).signum() == 0) return null
        val a = BigInteger(1, aPriv)
        val aPub = G.modPow(a, N)
        val x = xOf(user, pass, salt)
        val u = BigInteger(1, sha512(pad(aPub), pad(bn)))
        val v = G.modPow(x, N)
        val t = bn.subtract(MULT.multiply(v).mod(N)).mod(N)
        val s = t.modPow(u.multiply(x).add(a), N)
        val k = sha512(minBytes(s))
        val m1 = m1(user, salt, aPub, bn, k)
        return Proof(m1, k, sha512(minBytes(aPub), m1, k))
    }

    private fun m1(user: String, salt: ByteArray, aPub: BigInteger, bPub: BigInteger, k: ByteArray): ByteArray {
        val hn = sha512(pad(N))
        val hg = sha512(byteArrayOf(5))
        val xor = ByteArray(HASH_BYTES) { (hn[it].toInt() xor hg[it].toInt()).toByte() }
        return sha512(
            stripZeros(xor),
            stripZeros(sha512(user.toByteArray())),
            salt,
            minBytes(aPub),
            minBytes(bPub),
            k,
        )
    }

    private fun sha512(vararg parts: ByteArray): ByteArray {
        val md = MessageDigest.getInstance("SHA-512")
        for (p in parts) md.update(p)
        return md.digest()
    }

    private fun pad(v: BigInteger): ByteArray {
        val raw = minBytes(v)
        require(raw.size <= N_BYTES)
        return ByteArray(N_BYTES - raw.size) + raw
    }

    /** Big-endian without leading zero bytes; zero is one zero byte. */
    private fun minBytes(v: BigInteger): ByteArray {
        val b = v.toByteArray()
        return if (b.size > 1 && b[0] == 0.toByte()) b.copyOfRange(1, b.size) else b
    }

    /** Strips leading zero bytes but keeps at least one, as strip_zeros in c/src/srp.c. */
    private fun stripZeros(b: ByteArray): ByteArray {
        var i = 0
        while (i + 1 < b.size && b[i] == 0.toByte()) i++
        return b.copyOfRange(i, b.size)
    }
}
