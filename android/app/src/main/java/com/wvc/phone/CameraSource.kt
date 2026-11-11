package com.wvc.phone

import android.content.Context
import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.os.SystemClock
import android.util.Size
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleOwner
import java.io.ByteArrayOutputStream
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * Back / front camera through CameraX ImageAnalysis: YUV_420_888 -> NV21 -> JPEG.
 * The picture is sent as the sensor gives it plus a rotation hint (clockwise, x 90°), WVC rotates it upright.
 */
class CameraSource(
    private val context: Context,
    private val owner: LifecycleOwner,
    private val server: FrameServer,
    private val onError: (String) -> Unit,
) {
    private val executor: ExecutorService = Executors.newSingleThreadExecutor()
    private var provider: ProcessCameraProvider? = null
    private var analysis: ImageAnalysis? = null
    private var wantFront: Boolean? = null  // null = stopped
    private var wantLongSide = 720
    @Volatile private var targetRotation = 0  // Surface.ROTATION_*
    @Volatile private var lastFrameAt = 0L

    private val jpegOut = ByteArrayOutputStream(256 * 1024)
    private var nv21 = ByteArray(0)
    private var rowA = ByteArray(0)
    private var rowB = ByteArray(0)

    val isRunning: Boolean get() = wantFront != null

    /** Main thread. */
    fun start(front: Boolean, longSide: Int) {
        wantFront = front
        wantLongSide = longSide
        val p = provider
        if (p != null) {
            bind(p)
            return
        }
        val f = ProcessCameraProvider.getInstance(context)
        f.addListener({
            try {
                val got = f.get()
                provider = got
                if (wantFront != null) bind(got)
            } catch (e: Exception) {
                onError("camera_error")
            }
        }, ContextCompat.getMainExecutor(context))
    }

    /** Main thread. */
    fun stop() {
        wantFront = null
        try { provider?.unbindAll() } catch (_: Exception) {}
        analysis = null
    }

    /** Device orientation changed (Surface.ROTATION_*). */
    fun setRotation(rotation: Int) {
        targetRotation = rotation
        analysis?.targetRotation = rotation
    }

    fun release() {
        stop()
        executor.shutdown()
    }

    private fun bind(p: ProcessCameraProvider) {
        val front = wantFront ?: return
        try {
            p.unbindAll()
            val size = if (wantLongSide >= 1080) Size(1920, 1080) else Size(1280, 720)
            val selector = ResolutionSelector.Builder()
                .setResolutionStrategy(
                    ResolutionStrategy(size, ResolutionStrategy.FALLBACK_RULE_CLOSEST_HIGHER_THEN_LOWER))
                .build()
            val a = ImageAnalysis.Builder()
                .setResolutionSelector(selector)
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_YUV_420_888)
                .setTargetRotation(targetRotation)
                .build()
            a.setAnalyzer(executor) { img -> analyze(img) }
            val cam = if (front) CameraSelector.DEFAULT_FRONT_CAMERA else CameraSelector.DEFAULT_BACK_CAMERA
            if (!p.hasCamera(cam)) {
                onError("camera_error")
                return
            }
            p.bindToLifecycle(owner, cam, a)
            analysis = a
        } catch (e: Exception) {
            onError("camera_error")
        }
    }

    private fun analyze(img: ImageProxy) {
        try {
            val now = SystemClock.elapsedRealtime()
            if (now - lastFrameAt < 32 || !server.wantsFrame()) return  // max ~30 fps, never queue
            lastFrameAt = now
            val w = img.width and 1.inv()
            val h = img.height and 1.inv()
            if (w < 2 || h < 2) return
            toNv21(img, w, h)
            jpegOut.reset()
            YuvImage(nv21, ImageFormat.NV21, w, h, null).compressToJpeg(Rect(0, 0, w, h), 80, jpegOut)
            val bytes = jpegOut.toByteArray()
            server.sendFrame(bytes, bytes.size, w, h, (img.imageInfo.rotationDegrees / 90) and 3)
        } catch (_: Exception) {
        } finally {
            img.close()
        }
    }

    private fun toNv21(img: ImageProxy, w: Int, h: Int) {
        val need = w * h * 3 / 2
        if (nv21.size != need) nv21 = ByteArray(need)
        val yp = img.planes[0]
        val up = img.planes[1]
        val vp = img.planes[2]
        // Y
        val yb = yp.buffer
        val yStride = yp.rowStride
        var pos = 0
        if (yp.pixelStride == 1) {
            for (row in 0 until h) {
                yb.position(row * yStride)
                yb.get(nv21, pos, w)
                pos += w
            }
        } else {
            for (row in 0 until h) for (col in 0 until w) nv21[pos++] = yb.get(row * yStride + col * yp.pixelStride)
        }
        // interleaved V U
        val ub = up.buffer
        val vb = vp.buffer
        val cw = w / 2
        val ch = h / 2
        val uPix = up.pixelStride
        val vPix = vp.pixelStride
        val rowLen = (cw - 1) * maxOf(uPix, vPix) + 1
        if (rowA.size < rowLen + 8) {
            rowA = ByteArray(rowLen + 8)
            rowB = ByteArray(rowLen + 8)
        }
        for (row in 0 until ch) {
            val uStart = row * up.rowStride
            val vStart = row * vp.rowStride
            val uLen = minOf((cw - 1) * uPix + 1, ub.limit() - uStart)
            val vLen = minOf((cw - 1) * vPix + 1, vb.limit() - vStart)
            if (uLen <= 0 || vLen <= 0) break
            ub.position(uStart)
            ub.get(rowA, 0, uLen)
            vb.position(vStart)
            vb.get(rowB, 0, vLen)
            var ui = 0
            var vi = 0
            for (col in 0 until cw) {
                nv21[pos++] = if (vi < vLen) rowB[vi] else 0.toByte()
                nv21[pos++] = if (ui < uLen) rowA[ui] else 0.toByte()
                ui += uPix
                vi += vPix
            }
        }
        yb.rewind()
        ub.rewind()
        vb.rewind()
    }
}
