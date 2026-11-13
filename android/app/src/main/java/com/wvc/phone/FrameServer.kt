package com.wvc.phone

import android.os.SystemClock
import java.io.BufferedOutputStream
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket

/**
 * TCP server for WVC (port 47800) + UDP discovery (port 47801). See docs/PHONE-PROTOCOL.md.
 * One client at a time: a new connection replaces the old one.
 * Listens on all interfaces, loopback included: over a USB cable WVC comes through `adb forward` from 127.0.0.1.
 * One-frame queue: a frame waits while the previous one is being sent; a newer frame replaces it (latency first).
 * JPEG quality adapts: when frames have to be replaced often (slow Wi-Fi), the encoders are asked for less.
 * H.264 (after WVC sends "CODEC h264"): every packet is sent in order; when the queue grows too long it is dropped up to
 * the next key frame (one is requested at once) and the encoder is asked for a lower bitrate.
 */
class FrameServer(private val listener: Listener, private val deviceName: String) {

    interface Listener {
        fun onClientConnected(address: String)
        fun onClientDisconnected()
        fun onCommand(line: String)
    }

    companion object {
        const val TCP_PORT = 47800
        const val UDP_PORT = 47801
    }

    @Volatile private var running = false
    private var serverSocket: ServerSocket? = null
    private var udpSocket: DatagramSocket? = null
    private val lock = Object()

    // guarded by lock
    private var client: Socket? = null
    private var out: OutputStream? = null
    private var pendingFrame: ByteArray? = null
    private var pendingLen = 0
    private var pendingW = 0
    private var pendingH = 0
    private var pendingRot = 0
    private val pendingTexts = ArrayList<String>()
    private var sending = false

    // H.264 (guarded by lock)
    private class Pkt(val data: ByteArray, val flags: Int, val rot: Int, val w: Int, val h: Int, val pts: Long)
    private val h264q = ArrayDeque<Pkt>()
    private var h264Bytes = 0
    private var waitKey = true
    private var keyRequested = false
    private var overflows = 0
    private var maxQueue = 0

    /** WVC asked for H.264 on this connection ("CODEC h264"); reset for every new connection. */
    @Volatile var h264 = false
        private set

    // adaptive JPEG quality (guarded by lock)
    private var produced = 0
    private var dropped = 0
    private var qualityOffset = 0  // 0 .. -35
    private var qualityCheckedAt = 0L

    @Volatile private var writeStartedAt = 0L
    @Volatile var framesSent = 0
        private set

    val isConnected: Boolean
        get() = synchronized(lock) { client != null }

    fun start() {
        if (running) return
        running = true
        Thread({ acceptLoop() }, "wvc-accept").start()
        Thread({ sendLoop() }, "wvc-send").start()
        Thread({ discoveryLoop() }, "wvc-discovery").start()
    }

    fun stop() {
        running = false
        try { serverSocket?.close() } catch (_: Exception) {}
        try { udpSocket?.close() } catch (_: Exception) {}
        synchronized(lock) {
            closeClientLocked()
            lock.notifyAll()
        }
    }

    /** true when a frame would not have to wait behind another one (it may be encoded while one is sent) */
    fun wantsFrame(): Boolean = synchronized(lock) { client != null && pendingFrame == null }

    fun hasClient(): Boolean = synchronized(lock) { client != null }

    /** JPEG quality to use now: [base] lowered while the network cannot keep up, raised back when it can. */
    fun jpegQuality(base: Int): Int = synchronized(lock) {
        val now = SystemClock.elapsedRealtime()
        if (now - qualityCheckedAt >= 1000) {
            if (produced >= 5) {
                val ratio = dropped.toDouble() / produced
                if (ratio > 0.25) qualityOffset = maxOf(-35, qualityOffset - 10)
                else if (ratio < 0.05) qualityOffset = minOf(0, qualityOffset + 5)
            }
            produced = 0
            dropped = 0
            qualityCheckedAt = now
        }
        (base + qualityOffset).coerceIn(40, 95)
    }

    fun sendFrame(jpeg: ByteArray, len: Int, width: Int, height: Int, rotation: Int) {
        synchronized(lock) {
            if (client == null) return
            produced++
            if (pendingFrame != null) dropped++  // the network is behind: the older frame is replaced
            pendingFrame = jpeg
            pendingLen = len
            pendingW = width
            pendingH = height
            pendingRot = rotation and 3
            lock.notifyAll()
        }
    }

    /** Switches the connection to H.264 / back to JPEG (main thread, from the CODEC command). */
    fun useH264(on: Boolean) {
        synchronized(lock) {
            h264 = on
            h264q.clear()
            h264Bytes = 0
            waitKey = true
            keyRequested = true
        }
    }

    fun requestKeyFrame() {
        synchronized(lock) { keyRequested = true }
    }

    /** The encoder asks whether it should make a key frame now. */
    fun takeKeyRequest(): Boolean = synchronized(lock) {
        val k = keyRequested
        keyRequested = false
        k
    }

    /** Once a second: -1 = the network is behind (lower the bitrate), 1 = it keeps up easily, 0 = leave it. */
    fun h264Congestion(): Int = synchronized(lock) {
        val r = when {
            overflows > 0 || maxQueue > 5 -> -1
            maxQueue <= 2 -> 1
            else -> 0
        }
        overflows = 0
        maxQueue = 0
        r
    }

    /** One H.264 access unit (Annex-B). [key] = IDR (SPS/PPS in front). */
    fun sendH264(data: ByteArray, key: Boolean, rotation: Int, width: Int, height: Int, ptsUs: Long) {
        synchronized(lock) {
            if (client == null || !h264) return
            if (waitKey && !key) {
                keyRequested = true
                return
            }
            if (key) waitKey = false
            if (h264q.size >= 15 || h264Bytes > 4 * 1024 * 1024) {  // ~0.25–0.5 s behind: old frames are useless
                // the network is far behind: drop what waits, start again at a key frame
                h264q.clear()
                h264Bytes = 0
                overflows++
                if (!key) {
                    waitKey = true
                    keyRequested = true
                    return
                }
            }
            h264q.addLast(Pkt(data, if (key) 3 else 0, rotation and 3, width, height, ptsUs))
            h264Bytes += data.size
            if (h264q.size > maxQueue) maxQueue = h264q.size
            lock.notifyAll()
        }
    }

    fun sendText(text: String) {
        synchronized(lock) {
            if (client == null) return
            if (pendingTexts.size < 32) pendingTexts.add(text)
            lock.notifyAll()
        }
    }

    /** Called every couple of seconds: a write that hangs means the PC is gone. */
    fun checkStall() {
        val t = writeStartedAt
        if (t != 0L && SystemClock.elapsedRealtime() - t > 6000) {
            synchronized(lock) { closeClientLocked() }
        }
    }

    private fun closeClientLocked() {
        val c = client ?: return
        client = null
        out = null
        pendingFrame = null
        pendingTexts.clear()
        h264q.clear()
        h264Bytes = 0
        h264 = false
        try { c.close() } catch (_: Exception) {}
    }

    private fun acceptLoop() {
        try {
            val ss = ServerSocket()
            ss.reuseAddress = true
            ss.bind(InetSocketAddress(TCP_PORT))
            serverSocket = ss
            while (running) {
                val s = try { ss.accept() } catch (e: Exception) { if (!running) break else continue }
                try {
                    s.tcpNoDelay = true
                    s.keepAlive = true
                    s.sendBufferSize = 256 * 1024
                } catch (_: Exception) {}
                val stream = BufferedOutputStream(s.getOutputStream(), 64 * 1024)
                synchronized(lock) {
                    closeClientLocked()  // the newest WVC wins
                    client = s
                    out = stream
                    qualityOffset = 0
                    h264 = false
                    waitKey = true
                    keyRequested = true
                    lock.notifyAll()
                }
                val addr = s.inetAddress?.hostAddress ?: "?"
                listener.onClientConnected(addr)
                Thread({ readLoop(s) }, "wvc-read").start()
            }
        } catch (_: Exception) {
        }
    }

    private fun readLoop(s: Socket) {
        try {
            val r = BufferedReader(InputStreamReader(s.getInputStream(), Charsets.UTF_8))
            while (running) {
                val line = r.readLine() ?: break
                val t = line.trim()
                if (t.isNotEmpty()) listener.onCommand(t)
            }
        } catch (_: Exception) {
        }
        val wasCurrent: Boolean
        synchronized(lock) {
            wasCurrent = client === s
            if (wasCurrent) closeClientLocked()
        }
        if (wasCurrent) listener.onClientDisconnected()
    }

    private fun sendLoop() {
        while (running) {
            val stream: OutputStream?
            val frame: ByteArray?
            val len: Int
            val w: Int
            val h: Int
            val rot: Int
            val texts: List<String>
            val pkts: List<Pkt>
            val sock: Socket?
            synchronized(lock) {
                while (running && (client == null || (pendingFrame == null && pendingTexts.isEmpty() && h264q.isEmpty()))) {
                    try { lock.wait(1000) } catch (_: InterruptedException) {}
                }
                if (!running) return
                stream = out
                sock = client
                frame = pendingFrame
                len = pendingLen
                w = pendingW
                h = pendingH
                rot = pendingRot
                texts = ArrayList(pendingTexts)
                pendingFrame = null
                pendingTexts.clear()
                pkts = ArrayList(h264q)
                h264q.clear()
                h264Bytes = 0
                sending = true
            }
            val o = stream
            try {
                if (o != null) {
                    writeStartedAt = SystemClock.elapsedRealtime()
                    for (t in texts) {
                        val b = t.toByteArray(Charsets.UTF_8)
                        o.write(header("WVCS", b.size))
                        o.write(b)
                    }
                    val f = frame
                    if (f != null) {
                        val hdr = header("WVCF", len)
                        o.write(hdr)
                        o.write(byteArrayOf(
                            (w and 0xFF).toByte(), ((w shr 8) and 0xFF).toByte(),
                            (h and 0xFF).toByte(), ((h shr 8) and 0xFF).toByte(),
                            rot.toByte()))
                        o.write(f, 0, len)
                    }
                    for (p in pkts) {
                        o.write(header("WVCH", p.data.size))
                        val ext = ByteArray(14)
                        ext[0] = p.flags.toByte()
                        ext[1] = p.rot.toByte()
                        ext[2] = (p.w and 0xFF).toByte()
                        ext[3] = ((p.w shr 8) and 0xFF).toByte()
                        ext[4] = (p.h and 0xFF).toByte()
                        ext[5] = ((p.h shr 8) and 0xFF).toByte()
                        for (i in 0 until 8) ext[6 + i] = ((p.pts shr (8 * i)) and 0xFFL).toByte()
                        o.write(ext)
                        o.write(p.data)
                    }
                    o.flush()
                    if (f != null) framesSent++
                    framesSent += pkts.size
                }
            } catch (e: Exception) {
                val wasCurrent: Boolean
                synchronized(lock) {
                    wasCurrent = client === sock
                    if (wasCurrent) closeClientLocked()
                }
                if (wasCurrent) listener.onClientDisconnected()
            } finally {
                writeStartedAt = 0L
                synchronized(lock) { sending = false }
            }
        }
    }

    private fun header(magic: String, len: Int): ByteArray {
        val m = magic.toByteArray(Charsets.US_ASCII)
        return byteArrayOf(
            m[0], m[1], m[2], m[3],
            (len and 0xFF).toByte(), ((len shr 8) and 0xFF).toByte(),
            ((len shr 16) and 0xFF).toByte(), ((len shr 24) and 0xFF).toByte())
    }

    private fun discoveryLoop() {
        try {
            val u = DatagramSocket(null)
            u.reuseAddress = true
            u.broadcast = true
            u.bind(InetSocketAddress(UDP_PORT))
            udpSocket = u
            val buf = ByteArray(256)
            while (running) {
                val p = DatagramPacket(buf, buf.size)
                try {
                    u.receive(p)
                } catch (e: Exception) {
                    if (!running) break else continue
                }
                val msg = String(p.data, 0, p.length, Charsets.UTF_8)
                if (msg.startsWith("WVCP?")) {
                    val reply = ("WVCP!$deviceName").toByteArray(Charsets.UTF_8)
                    try {
                        u.send(DatagramPacket(reply, reply.size, p.socketAddress))
                    } catch (_: Exception) {}
                }
            }
        } catch (_: Exception) {
        }
    }
}
