package app.airplaysender

import java.io.ByteArrayOutputStream

/**
 * Binary plist (bplist00) reader and writer, the subset c/src/bplist.c uses.
 * Values are plain Kotlin: Map<String, Any?>, List<Any?>, String, Long, Double, Boolean, ByteArray, or null.
 */
object Bplist {
    private const val MAX_DEPTH = 64

    /** Returns the root value, or null for malformed input. Never throws. */
    fun parse(buf: ByteArray): Any? = try {
        Reader(buf).root()
    } catch (e: Exception) {
        null
    }

    fun serialize(root: Any?): ByteArray {
        val objs = ArrayList<Obj>()
        val top = add(objs, root)
        val n = objs.size
        val refSize = bytesFor((n - 1).toLong())
        val offsets = LongArray(n)
        var pos = 8L
        for (i in 0 until n) {
            offsets[i] = pos
            pos += objs[i].head.size + objs[i].refs.size * refSize
        }
        val tableOffset = pos
        val offsetSize = bytesFor(tableOffset)
        val out = ByteArrayOutputStream()
        out.write("bplist00".toByteArray(Charsets.US_ASCII))
        for (o in objs) {
            out.write(o.head)
            for (r in o.refs) out.write(beBytes(r.toLong(), refSize))
        }
        for (off in offsets) out.write(beBytes(off, offsetSize))
        out.write(ByteArray(6)) // 5 unused bytes and the sort version
        out.write(offsetSize)
        out.write(refSize)
        out.write(beBytes(n.toLong(), 8))
        out.write(beBytes(top.toLong(), 8))
        out.write(beBytes(tableOffset, 8))
        return out.toByteArray()
    }

    private class Obj(val head: ByteArray, val refs: List<Int>)

    private fun add(objs: MutableList<Obj>, v: Any?): Int {
        val o = when (v) {
            null -> Obj(byteArrayOf(0x00), emptyList())
            is Boolean -> Obj(byteArrayOf((if (v) 0x09 else 0x08).toByte()), emptyList())
            is Int -> Obj(intObj(v.toLong()), emptyList())
            is Long -> Obj(intObj(v), emptyList())
            is Double -> Obj(byteArrayOf(0x23) + beBytes(java.lang.Double.doubleToRawLongBits(v), 8), emptyList())
            is String -> Obj(stringObj(v), emptyList())
            is ByteArray -> Obj(header(0x4, v.size) + v, emptyList())
            is List<*> -> {
                val refs = v.map { add(objs, it) }
                Obj(header(0xA, refs.size), refs)
            }
            is Map<*, *> -> {
                val keys = v.keys.map { add(objs, it) }
                val vals = v.values.map { add(objs, it) }
                Obj(header(0xD, keys.size), keys + vals)
            }
            else -> throw IllegalArgumentException("bplist: unsupported type " + v.javaClass.name)
        }
        objs.add(o)
        return objs.size - 1
    }

    private fun intObj(v: Long): ByteArray = when {
        v in 0..0xFF -> byteArrayOf(0x10) + beBytes(v, 1)
        v in 0..0xFFFF -> byteArrayOf(0x11) + beBytes(v, 2)
        v in 0..0xFFFFFFFFL -> byteArrayOf(0x12) + beBytes(v, 4)
        else -> byteArrayOf(0x13) + beBytes(v, 8)
    }

    private fun header(type: Int, len: Int): ByteArray =
        if (len < 15) byteArrayOf(((type shl 4) or len).toByte())
        else byteArrayOf(((type shl 4) or 0xF).toByte()) + intObj(len.toLong())

    private fun stringObj(s: String): ByteArray =
        if (s.all { it.code < 0x80 }) header(0x5, s.length) + s.toByteArray(Charsets.US_ASCII)
        else header(0x6, s.length) + s.toByteArray(Charsets.UTF_16BE)

    private fun bytesFor(v: Long): Int = when {
        v <= 0xFF -> 1
        v <= 0xFFFF -> 2
        v <= 0xFFFFFFFFL -> 4
        else -> 8
    }

    private class Reader(val buf: ByteArray) {
        val offsetSize: Int
        val refSize: Int
        val count: Int
        val top: Int
        val tableOffset: Int

        init {
            require(buf.size >= 8 + 32 && String(buf, 0, 8, Charsets.US_ASCII) == "bplist00")
            val t = buf.size - 32
            offsetSize = buf[t + 6].toInt() and 0xFF
            refSize = buf[t + 7].toInt() and 0xFF
            count = uint(t + 8, 8).toInt()
            top = uint(t + 16, 8).toInt()
            tableOffset = uint(t + 24, 8).toInt()
            require(offsetSize in 1..8 && refSize in 1..8 && count > 0 && top in 0 until count)
        }

        fun root(): Any? = objectAt(top, 0)

        fun uint(pos: Int, n: Int): Long {
            var v = 0L
            for (i in 0 until n) v = (v shl 8) or (buf[pos + i].toLong() and 0xFF)
            return v
        }

        fun objectAt(ref: Int, depth: Int): Any? {
            require(ref in 0 until count && depth < MAX_DEPTH)
            val off = uint(tableOffset + ref * offsetSize, offsetSize).toInt()
            return value(off, depth)
        }

        fun value(pos: Int, depth: Int): Any? {
            val marker = buf[pos].toInt() and 0xFF
            val info = marker and 0x0F
            return when (marker shr 4) {
                0x0 -> when (info) {
                    0x8 -> false
                    0x9 -> true
                    else -> null
                }
                0x1 -> {
                    require(info <= 3)
                    uint(pos + 1, 1 shl info)
                }
                0x2 -> when (info) {
                    2 -> java.lang.Float.intBitsToFloat(uint(pos + 1, 4).toInt()).toDouble()
                    3 -> java.lang.Double.longBitsToDouble(uint(pos + 1, 8))
                    else -> throw IllegalArgumentException("bplist: real size")
                }
                0x4 -> {
                    val (len, start) = length(pos, info)
                    buf.copyOfRange(start, start + len)
                }
                0x5 -> {
                    val (len, start) = length(pos, info)
                    String(buf, start, len, Charsets.US_ASCII)
                }
                0x6 -> {
                    val (len, start) = length(pos, info)
                    String(buf, start, len * 2, Charsets.UTF_16BE)
                }
                0xA -> {
                    val (len, start) = length(pos, info)
                    List(len) { objectAt(refAt(start + it * refSize), depth + 1) }
                }
                0xD -> {
                    val (len, start) = length(pos, info)
                    val map = LinkedHashMap<String, Any?>()
                    for (i in 0 until len) {
                        val key = objectAt(refAt(start + i * refSize), depth + 1) as? String
                            ?: throw IllegalArgumentException("bplist: non-string key")
                        map[key] = objectAt(refAt(start + (len + i) * refSize), depth + 1)
                    }
                    map
                }
                else -> throw IllegalArgumentException("bplist: unsupported marker")
            }
        }

        fun refAt(pos: Int): Int = uint(pos, refSize).toInt()

        // Returns (length, first data byte). An info nibble of 0xF means an int object follows.
        // The bound stops a corrupt length from allocating a huge list (an Error, which parse() does not catch).
        fun length(pos: Int, info: Int): Pair<Int, Int> {
            val (len, start) = if (info != 0xF) {
                Pair(info, pos + 1)
            } else {
                val m = buf[pos + 1].toInt() and 0xFF
                require(m shr 4 == 0x1 && (m and 0xF) <= 3)
                val n = 1 shl (m and 0xF)
                Pair(uint(pos + 2, n).toInt(), pos + 2 + n)
            }
            require(len in 0..buf.size)
            return Pair(len, start)
        }
    }
}
