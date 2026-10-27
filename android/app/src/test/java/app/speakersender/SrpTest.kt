package app.speakersender

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

// Mirrors test_srp_reference in c/tests/test_unit.c; vectors come from srptools (c/tools/gen_vectors.py).
class SrpTest {
    @Test
    fun clientPublicMatchesReference() {
        assertEquals(Srp.N_BYTES, Vectors.V_SRP_A_MIN.size)
        assertArrayEquals(Vectors.V_SRP_A_MIN, Srp.clientPublic(Vectors.V_SRP_A_PRIV))
    }

    @Test
    fun clientProofMatchesReference() {
        val proof = Srp.clientProof("Pair-Setup", "3939", Vectors.V_SRP_SALT, Vectors.V_SRP_A_PRIV, Vectors.V_SRP_B)
        assertArrayEquals(Vectors.V_SRP_K, proof!!.K)
        assertArrayEquals(Vectors.V_SRP_M1, proof.M1)
    }

    // No golden vector covers the server side or M2, so this checks the two halves against each other.
    @Test
    fun serverAndClientAgreeAndWrongPinIsRefused() {
        val salt = ByteArray(16) { it.toByte() }
        val aPriv = ByteArray(32) { (it + 1).toByte() }
        val server = Srp.serverStart("Pair-Setup", "3939", salt, ByteArray(32) { (it * 3 + 5).toByte() })
        val b = Srp.serverPublic(server)
        val a = Srp.clientPublic(aPriv)
        val client = Srp.clientProof("Pair-Setup", "3939", salt, aPriv, b)!!
        val side = Srp.serverVerify(server, a, client.M1)!!
        assertArrayEquals(client.K, side.K)
        assertArrayEquals(client.M2, side.M2)
        val wrong = Srp.clientProof("Pair-Setup", "1234", salt, aPriv, b)!!
        assertNull(Srp.serverVerify(server, a, wrong.M1))
    }

    @Test
    fun refusesZeroModN() {
        val zero = ByteArray(Srp.N_BYTES)
        assertNull(Srp.clientProof("Pair-Setup", "3939", Vectors.V_SRP_SALT, Vectors.V_SRP_A_PRIV, zero))
    }

    @Test
    fun refusesEmptyOrOversizedB() {
        assertNull(Srp.clientProof("Pair-Setup", "3939", Vectors.V_SRP_SALT, Vectors.V_SRP_A_PRIV, ByteArray(0)))
        assertNull(Srp.clientProof("Pair-Setup", "3939", Vectors.V_SRP_SALT, Vectors.V_SRP_A_PRIV, ByteArray(385) { 1 }))
    }
}
