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
 * Frames are never queued: while one frame is being sent, new ones are dropped (latency first).
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

    /** true when a frame would be sent right away (encode only then) */
    fun wantsFrame(): Boolean = synchronized(lock) { client != null && pendingFrame == null && !sending }

    fun sendFrame(jpeg: ByteArray, len: Int, width: Int, height: Int, rotation: Int) {
        synchronized(lock) {
            if (client == null) return
            pendingFrame = jpeg
            pendingLen = len
            pendingW = width
            pendingH = height
            pendingRot = rotation and 3
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
            val sock: Socket?
            synchronized(lock) {
                while (running && (client == null || (pendingFrame == null && pendingTexts.isEmpty()))) {
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
                    o.flush()
                    if (f != null) framesSent++
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
