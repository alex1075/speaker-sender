package app.airplaysender

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
