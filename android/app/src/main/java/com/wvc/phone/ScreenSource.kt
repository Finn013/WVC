package com.wvc.phone

import android.content.Context
import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.DisplayMetrics
import java.io.ByteArrayOutputStream

/**
 * Phone screen through MediaProjection -> VirtualDisplay -> ImageReader -> JPEG.
 * The projection stays alive until it is stopped (Android 14: one virtual display per consent),
 * frames are only encoded while [active] is true.
 */
class ScreenSource(
    private val context: Context,
    private val server: FrameServer,
    private val onStopped: () -> Unit,
) {
    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    private var projection: MediaProjection? = null
    private var display: VirtualDisplay? = null
    private var reader: ImageReader? = null
    private var curW = 0
    private var curH = 0
    private var longSide = 720

    @Volatile var active = false
    @Volatile private var lastFrameAt = 0L
    private var bitmap: Bitmap? = null
    private val jpegOut = ByteArrayOutputStream(256 * 1024)

    val hasProjection: Boolean get() = projection != null

    private val callback = object : MediaProjection.Callback() {
        override fun onStop() {
            releaseInternal()
            onStopped()
        }
    }

    /** Main thread. Returns false when the projection could not be used. */
    fun start(p: MediaProjection, longSideWanted: Int): Boolean {
        releaseInternal()
        longSide = longSideWanted
        val t = HandlerThread("wvc-screen")
        t.start()
        thread = t
        val h = Handler(t.looper)
        handler = h
        projection = p
        return try {
            p.registerCallback(callback, h)  // Android 14: before createVirtualDisplay
            val (w, hh, dpi) = targetSize()
            val r = newReader(w, hh)
            display = p.createVirtualDisplay("WVC", w, hh, dpi, DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                r.surface, null, h)
            true
        } catch (e: Exception) {
            try { p.stop() } catch (_: Exception) {}
            releaseInternal()
            false
        }
    }

    /** Quality or orientation changed: resize the virtual display (no new consent needed). */
    fun resize(longSideWanted: Int) {
        longSide = longSideWanted
        val d = display ?: return
        val (w, h, dpi) = targetSize()
        if (w == curW && h == curH) return
        try {
            val old = reader
            val r = newReader(w, h)
            d.resize(w, h, dpi)
            d.surface = r.surface
            old?.close()
        } catch (_: Exception) {
        }
    }

    fun stop() {
        val p = projection
        releaseInternal()
        try { p?.stop() } catch (_: Exception) {}
    }

    private fun newReader(w: Int, h: Int): ImageReader {
        val r = ImageReader.newInstance(w, h, PixelFormat.RGBA_8888, 2)
        r.setOnImageAvailableListener({ ir -> onImage(ir) }, handler)
        reader = r
        curW = w
        curH = h
        return r
    }

    private fun onImage(ir: ImageReader) {
        val img = try { ir.acquireLatestImage() } catch (_: Exception) { null } ?: return
        try {
            val now = SystemClock.elapsedRealtime()
            if (!active || now - lastFrameAt < 32 || !server.wantsFrame()) return
            lastFrameAt = now
            val plane = img.planes[0]
            val pixelStride = plane.pixelStride
            val rowStride = plane.rowStride
            val w = img.width
            val h = img.height
            val bw = rowStride / pixelStride
            var b = bitmap
            if (b == null || b.width != bw || b.height != h) {
                b?.recycle()
                b = Bitmap.createBitmap(bw, h, Bitmap.Config.ARGB_8888)
                bitmap = b
            }
            val buf = plane.buffer
            buf.rewind()
            b!!.copyPixelsFromBuffer(buf)
            val src = if (bw != w) Bitmap.createBitmap(b, 0, 0, w, h) else b
            jpegOut.reset()
            src.compress(Bitmap.CompressFormat.JPEG, 80, jpegOut)
            if (src !== b) src.recycle()
            val bytes = jpegOut.toByteArray()
            server.sendFrame(bytes, bytes.size, w, h, 0)
        } catch (_: Exception) {
        } finally {
            img.close()
        }
    }

    private data class Target(val w: Int, val h: Int, val dpi: Int)

    @Suppress("DEPRECATION")
    private fun targetSize(): Target {
        val dm = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
        val m = DisplayMetrics()
        dm.getDisplay(android.view.Display.DEFAULT_DISPLAY)?.getRealMetrics(m)
        var sw = m.widthPixels
        var sh = m.heightPixels
        context.resources.displayMetrics.let { m.densityDpi = it.densityDpi }
        if (sw <= 0 || sh <= 0) {
            sw = 720
            sh = 1280
        }
        val long = maxOf(sw, sh)
        val scale = if (long > longSide) longSide.toDouble() / long else 1.0
        val w = (Math.round(sw * scale).toInt() and 1.inv()).coerceAtLeast(2)
        val h = (Math.round(sh * scale).toInt() and 1.inv()).coerceAtLeast(2)
        val dpi = (m.densityDpi * scale).toInt().coerceAtLeast(120)
        return Target(w, h, dpi)
    }

    private fun releaseInternal() {
        active = false
        try { display?.release() } catch (_: Exception) {}
        display = null
        try { reader?.close() } catch (_: Exception) {}
        reader = null
        try { projection?.unregisterCallback(callback) } catch (_: Exception) {}
        projection = null
        thread?.quitSafely()
        thread = null
        handler = null
        curW = 0
        curH = 0
    }

    fun release() {
        stop()
    }
}
