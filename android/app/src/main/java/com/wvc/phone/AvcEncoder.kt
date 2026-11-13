package com.wvc.phone

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.Surface

/**
 * Hardware H.264 (AVC) encoder with an input Surface: the camera (CameraX Preview) or the screen (VirtualDisplay)
 * draws straight into it, no copies through the CPU. Output: Annex-B access units to [FrameServer.sendH264];
 * SPS/PPS are put in front of every key frame, so WVC can start decoding at any key frame.
 * Baseline profile, 30 fps (60 when WVC asks, 0.13), a key frame every second, bitrate adapts to the network
 * (1080p: 4–8 Mbit/s, 720p: 2–4 Mbit/s; x1.5 at 60 fps).
 */
class AvcEncoder(private val server: FrameServer, private val gate: () -> Boolean) {

    private var codec: MediaCodec? = null
    var surface: Surface? = null
        private set
    @Volatile var rotation = 0  // clockwise x 90°, the picture is sent as it is drawn
    @Volatile private var running = false
    private var thread: Thread? = null
    private var config: ByteArray? = null
    var width = 0
        private set
    var height = 0
        private set
    private var bitrate = 0
    private var minBr = 0
    private var maxBr = 0
    private var lastAdjust = 0L
    private var frameRate = 30

    /** Returns the input surface, or null when the phone has no usable AVC encoder. */
    @Synchronized
    fun start(w: Int, h: Int, fps: Int = 30): Surface? {
        stop()
        val ew = (w + 1) and 1.inv()
        val eh = (h + 1) and 1.inv()
        width = ew
        height = eh
        val px = ew.toLong() * eh
        maxBr = when {
            px >= 1700L * 1000 -> 8_000_000
            px >= 800L * 1000 -> 4_000_000
            else -> 2_500_000
        }
        frameRate = if (fps >= 50) 60 else 30
        if (frameRate == 60) maxBr = maxBr * 3 / 2
        minBr = maxBr / 2
        bitrate = maxBr * 3 / 4
        config = null
        val c = try { MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC) } catch (e: Exception) { return null }
        var ok = false
        for (attempt in 0..1) {
            try {
                c.configure(format(ew, eh, attempt == 0), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                ok = true
                break
            } catch (e: Exception) {
                try { c.reset() } catch (_: Exception) {}
            }
        }
        if (!ok) {
            try { c.release() } catch (_: Exception) {}
            return null
        }
        val s = try {
            c.createInputSurface().also { c.start() }
        } catch (e: Exception) {
            try { c.release() } catch (_: Exception) {}
            return null
        }
        codec = c
        surface = s
        running = true
        lastAdjust = SystemClock.elapsedRealtime()
        thread = Thread({ drain(c) }, "wvc-avc").apply { start() }
        return s
    }

    private fun format(w: Int, h: Int, withProfile: Boolean): MediaFormat {
        val f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h)
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
        f.setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
        f.setInteger(MediaFormat.KEY_FRAME_RATE, frameRate)
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
        f.setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000L)  // a still screen keeps sending frames
        if (withProfile) {
            f.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
            f.setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.AVCLevel41)
            f.setInteger(MediaFormat.KEY_PRIORITY, 0)  // real time
            if (Build.VERSION.SDK_INT >= 30) f.setInteger(MediaFormat.KEY_LATENCY, 1)
        }
        return f
    }

    fun requestKey() {
        val c = codec ?: return
        try {
            val b = Bundle()
            b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            c.setParameters(b)
        } catch (_: Exception) {
        }
    }

    private fun setBitrate(br: Int) {
        val c = codec ?: return
        bitrate = br
        try {
            val b = Bundle()
            b.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, br)
            c.setParameters(b)
        } catch (_: Exception) {
        }
    }

    private fun drain(c: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (running) {
            val idx = try { c.dequeueOutputBuffer(info, 20_000) } catch (e: Exception) { break }
            if (idx >= 0) {
                try {
                    val buf = c.getOutputBuffer(idx)
                    if (buf != null && info.size > 0) {
                        val bytes = ByteArray(info.size)
                        buf.position(info.offset)
                        buf.limit(info.offset + info.size)
                        buf.get(bytes)
                        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                            config = bytes
                        } else if (gate()) {
                            val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                            val cfg = config
                            val data = if (key && cfg != null) cfg + bytes else bytes
                            server.sendH264(data, key, rotation, width, height, info.presentationTimeUs)
                        }
                    }
                } catch (_: Exception) {
                } finally {
                    try { c.releaseOutputBuffer(idx, false) } catch (_: Exception) {}
                }
            } else if (idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                try {
                    val f = c.outputFormat
                    val a = f.getByteBuffer("csd-0")
                    val b = f.getByteBuffer("csd-1")
                    if (a != null) {
                        val x = ByteArray(a.remaining()).also { a.duplicate().get(it) }
                        val y = if (b != null) ByteArray(b.remaining()).also { b.duplicate().get(it) } else ByteArray(0)
                        config = x + y
                    }
                } catch (_: Exception) {
                }
            }
            if (server.takeKeyRequest()) requestKey()
            val now = SystemClock.elapsedRealtime()
            if (now - lastAdjust >= 1000) {
                lastAdjust = now
                when (server.h264Congestion()) {
                    -1 -> if (bitrate > minBr) setBitrate(maxOf(minBr, bitrate * 3 / 4))
                    1 -> if (bitrate < maxBr) setBitrate(minOf(maxBr, bitrate + maxBr / 10))
                }
            }
        }
    }

    @Synchronized
    fun stop() {
        running = false
        val t = thread
        thread = null
        try { t?.join(500) } catch (_: Exception) {}
        val c = codec
        codec = null
        try { c?.stop() } catch (_: Exception) {}
        try { c?.release() } catch (_: Exception) {}
        try { surface?.release() } catch (_: Exception) {}
        surface = null
    }

    val isRunning: Boolean get() = codec != null
}
