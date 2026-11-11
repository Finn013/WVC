package com.wvc.phone

import android.content.Context
import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CaptureRequest
import android.os.SystemClock
import android.util.Range
import android.util.Size
import androidx.camera.camera2.interop.Camera2CameraInfo
import androidx.camera.camera2.interop.Camera2Interop
import androidx.camera.camera2.interop.ExperimentalCamera2Interop
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleOwner
import java.io.ByteArrayOutputStream
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * Back / front camera through CameraX ImageAnalysis (YUV_420_888, KEEP_ONLY_LATEST).
 *
 * v0.10 pipeline, the same on every phone:
 *  - analyzer thread: copies the frame into a reused NV21 buffer (bulk row copies; the common "semi-planar" layout
 *    is copied with one get() per row) and closes the image at once — it never waits for encoding or the network;
 *  - encoder thread: always encodes the newest copied frame (an older one waiting is replaced), JPEG quality adapts
 *    to how fast the network takes frames;
 *  - FrameServer send thread: one frame queue, the newest frame wins.
 * The picture is sent as the sensor gives it plus a rotation hint (clockwise, x 90°); WVC turns it upright.
 *
 * v0.11: with H.264 the camera draws into the hardware encoder's input surface (CameraX Preview use case), nothing is
 * copied or compressed by the CPU. «Камера через экран»: the Preview goes to the activity's full-screen view instead.
 */
class CameraSource(
    private val context: Context,
    private val owner: LifecycleOwner,
    private val server: FrameServer,
    private val onError: (String) -> Unit,
) {
    private val analyzeExecutor: ExecutorService = Executors.newSingleThreadExecutor { r -> Thread(r, "wvc-analyze") }
    private var provider: ProcessCameraProvider? = null
    private var analysis: ImageAnalysis? = null
    private var wantFront: Boolean? = null  // null = stopped
    private var wantLongSide = 720
    private var wantH264 = false
    private var previewTo: Preview.SurfaceProvider? = null
    private var boundKey = ""
    private var preview: Preview? = null
    private var enc: AvcEncoder? = null
    private val encExecutor: ExecutorService = Executors.newSingleThreadExecutor { r -> Thread(r, "wvc-camsurface") }
    @Volatile private var targetRotation = 0  // Surface.ROTATION_*
    @Volatile private var lastFrameAt = 0L

    // ---- frames between the analyzer and the encoder (guarded by lock)
    private class Frame(val data: ByteArray, var w: Int, var h: Int, var rot: Int)
    private val lock = Object()
    private val free = ArrayList<Frame>()
    private var pending: Frame? = null
    private var allocated = 0
    @Volatile private var encoderRunning = true
    private val encoderThread = Thread({ encodeLoop() }, "wvc-jpeg").apply { start() }

    // analyzer-thread scratch
    private var rowA = ByteArray(0)
    private var rowB = ByteArray(0)

    val isRunning: Boolean get() = wantFront != null

    /** Main thread. [h264] = into the hardware encoder; [target] = show in a view (camera through the screen). */
    fun start(front: Boolean, longSide: Int, h264: Boolean = false, target: Preview.SurfaceProvider? = null) {
        val key = "$front|$longSide|$h264|${target?.hashCode() ?: 0}"
        if (wantFront != null && key == boundKey && provider != null) return  // already like that
        wantFront = front
        wantLongSide = longSide
        wantH264 = h264
        previewTo = target
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
        boundKey = ""
        try { provider?.unbindAll() } catch (_: Exception) {}
        analysis = null
        preview = null
        enc?.stop()
        enc = null
        synchronized(lock) {
            pending?.let { free.add(it) }
            pending = null
        }
    }

    /** Device orientation changed (Surface.ROTATION_*). */
    fun setRotation(rotation: Int) {
        targetRotation = rotation
        analysis?.targetRotation = rotation
        preview?.targetRotation = rotation
    }

    fun requestKey() {
        enc?.requestKey()
    }

    fun release() {
        stop()
        analyzeExecutor.shutdown()
        encExecutor.shutdown()
        encoderRunning = false
        synchronized(lock) { lock.notifyAll() }
    }

    /** Frame rate range for steady ~30 fps: [30,30] when the camera has it, else the closest range ending at 30. */
    @androidx.annotation.OptIn(markerClass = [ExperimentalCamera2Interop::class])
    private fun pickFpsRange(p: ProcessCameraProvider, sel: CameraSelector): Range<Int>? {
        return try {
            val info = sel.filter(p.availableCameraInfos).firstOrNull() ?: return null
            val ranges = Camera2CameraInfo.from(info)
                .getCameraCharacteristic(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return null
            val upTo30 = ranges.filter { it.upper in 24..30 }
            val best = upTo30.maxWithOrNull(compareBy<Range<Int>>({ it.upper }, { it.lower }))
            best ?: ranges.filter { it.upper >= 24 }.minByOrNull { it.upper }
        } catch (_: Exception) {
            null
        }
    }

    @androidx.annotation.OptIn(markerClass = [ExperimentalCamera2Interop::class])
    private fun bind(p: ProcessCameraProvider) {
        val front = wantFront ?: return
        try {
            p.unbindAll()
            val cam = if (front) CameraSelector.DEFAULT_FRONT_CAMERA else CameraSelector.DEFAULT_BACK_CAMERA
            if (!p.hasCamera(cam)) {
                onError("camera_error")
                return
            }
            // exactly the chosen quality (sensor sizes are landscape); the closest size if the phone lacks it
            val size = if (wantLongSide >= 1080) Size(1920, 1080) else Size(1280, 720)
            val selector = ResolutionSelector.Builder()
                .setResolutionStrategy(
                    ResolutionStrategy(size, ResolutionStrategy.FALLBACK_RULE_CLOSEST_LOWER_THEN_HIGHER))
                .build()
            val range = pickFpsRange(p, cam)
            enc?.stop()
            enc = null
            analysis = null
            preview = null
            val target = previewTo
            if (target != null || wantH264) {
                val sp = target ?: encoderSurfaceProvider()
                val pv = try {
                    makePreview(selector, range, sp).also { p.bindToLifecycle(owner, cam, it) }
                } catch (e: Exception) {
                    if (range == null) throw e
                    p.unbindAll()
                    makePreview(selector, null, sp).also { p.bindToLifecycle(owner, cam, it) }
                }
                preview = pv
            } else {
                val a = try {
                    makeAnalysis(selector, range).also { p.bindToLifecycle(owner, cam, it) }
                } catch (e: Exception) {
                    if (range == null) throw e
                    p.unbindAll()  // the frame-rate hint was refused: bind without it
                    makeAnalysis(selector, null).also { p.bindToLifecycle(owner, cam, it) }
                }
                analysis = a
            }
            boundKey = "$front|$wantLongSide|$wantH264|${target?.hashCode() ?: 0}"
        } catch (e: Exception) {
            onError("camera_error")
        }
    }

    @androidx.annotation.OptIn(markerClass = [ExperimentalCamera2Interop::class])
    private fun makePreview(selector: ResolutionSelector, range: Range<Int>?, sp: Preview.SurfaceProvider): Preview {
        val builder = Preview.Builder()
            .setResolutionSelector(selector)
            .setTargetRotation(targetRotation)
        if (range != null) {
            try {
                Camera2Interop.Extender(builder).setCaptureRequestOption(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, range)
            } catch (_: Exception) {
            }
        }
        val pv = builder.build()
        pv.setSurfaceProvider(if (sp is EncoderProvider) encExecutor else ContextCompat.getMainExecutor(context), sp)
        return pv
    }

    private inner class EncoderProvider : Preview.SurfaceProvider {
        override fun onSurfaceRequested(request: androidx.camera.core.SurfaceRequest) {
            val size = request.resolution
            val e = AvcEncoder(server) { true }
            val s = e.start(size.width, size.height)
            if (s == null) {
                request.willNotProvideSurface()
                onError("encoder_error")
                return
            }
            enc?.stop()
            enc = e
            request.setTransformationInfoListener(encExecutor) { info ->
                e.rotation = (info.rotationDegrees / 90) and 3
            }
            request.provideSurface(s, encExecutor) { _ -> e.stop() }
            server.requestKeyFrame()
        }
    }

    private fun encoderSurfaceProvider(): Preview.SurfaceProvider = EncoderProvider()

    @androidx.annotation.OptIn(markerClass = [ExperimentalCamera2Interop::class])
    private fun makeAnalysis(selector: ResolutionSelector, range: Range<Int>?): ImageAnalysis {
        val builder = ImageAnalysis.Builder()
            .setResolutionSelector(selector)
            .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
            .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_YUV_420_888)
            .setTargetRotation(targetRotation)
        if (range != null) {
            try {
                Camera2Interop.Extender(builder).setCaptureRequestOption(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, range)
            } catch (_: Exception) {
            }
        }
        val a = builder.build()
        a.setAnalyzer(analyzeExecutor) { img -> analyze(img) }
        return a
    }

    /** A buffer to copy the next frame into; null = all busy (the frame is dropped). */
    private fun obtain(size: Int): Frame? = synchronized(lock) {
        while (free.isNotEmpty()) {
            val f = free.removeAt(free.size - 1)
            if (f.data.size == size) return f
            allocated--  // the quality changed: drop buffers of the old size
        }
        if (allocated < 3) {
            allocated++
            return Frame(ByteArray(size), 0, 0, 0)
        }
        val p = pending  // reuse the frame that is still waiting: a newer one is coming
        if (p != null && p.data.size == size) {
            pending = null
            return p
        }
        null
    }

    private fun analyze(img: ImageProxy) {
        try {
            if (!server.hasClient()) return
            val now = SystemClock.elapsedRealtime()
            if (now - lastFrameAt < 25) return  // ~30-40 fps at most (a 30 fps camera's jitter never drops a frame)
            val w = img.width and 1.inv()
            val h = img.height and 1.inv()
            if (w < 2 || h < 2) return
            val f = obtain(w * h * 3 / 2) ?: return
            if (!toNv21(img, w, h, f.data)) {
                synchronized(lock) { free.add(f) }
                return
            }
            lastFrameAt = now
            f.w = w
            f.h = h
            f.rot = (img.imageInfo.rotationDegrees / 90) and 3
            synchronized(lock) {
                pending?.let { free.add(it) }  // an older frame the encoder has not taken: replaced
                pending = f
                lock.notifyAll()
            }
        } catch (_: Exception) {
        } finally {
            img.close()
        }
    }

    private fun encodeLoop() {
        val out = ByteArrayOutputStream(256 * 1024)
        while (encoderRunning) {
            val f: Frame = synchronized(lock) {
                while (encoderRunning && pending == null) {
                    try { lock.wait(500) } catch (_: InterruptedException) {}
                }
                val p = pending
                pending = null
                p
            } ?: continue
            try {
                if (server.hasClient()) {
                    out.reset()
                    val q = server.jpegQuality(if (f.w * f.h > 1280 * 720) 75 else 80)
                    YuvImage(f.data, ImageFormat.NV21, f.w, f.h, null).compressToJpeg(Rect(0, 0, f.w, f.h), q, out)
                    val bytes = out.toByteArray()
                    server.sendFrame(bytes, bytes.size, f.w, f.h, f.rot)
                }
            } catch (_: Exception) {
            } finally {
                synchronized(lock) { free.add(f) }
            }
        }
    }

    /** YUV_420_888 -> NV21 (Y plane, then V U interleaved), respecting row and pixel strides. */
    private fun toNv21(img: ImageProxy, w: Int, h: Int, nv21: ByteArray): Boolean {
        val planes = img.planes
        if (planes.size < 3) return false
        val yp = planes[0]
        val up = planes[1]
        val vp = planes[2]
        // Y: one bulk copy when rows are packed, else one copy per row
        val yb = yp.buffer.duplicate()
        val yStride = yp.rowStride
        if (yp.pixelStride == 1) {
            if (yStride == w && yb.remaining() >= w * h) {
                yb.position(0)
                yb.get(nv21, 0, w * h)
            } else {
                for (row in 0 until h) {
                    yb.position(row * yStride)
                    yb.get(nv21, row * w, w)
                }
            }
        } else {
            var pos = 0
            for (row in 0 until h) for (col in 0 until w) nv21[pos++] = yb.get(row * yStride + col * yp.pixelStride)
        }
        val ub = up.buffer.duplicate()
        val vb = vp.buffer.duplicate()
        val cw = w / 2
        val ch = h / 2
        val base = w * h
        val uPix = up.pixelStride
        val vPix = vp.pixelStride
        val uRow = up.rowStride
        val vRow = vp.rowStride
        // Most phones: semi-planar memory where the V plane already is "V U V U ..." (NV21). Check a few samples.
        if (uPix == 2 && vPix == 2 && uRow == vRow && isVuInterleaved(ub, vb, vRow, cw, ch)) {
            val rowBytes = 2 * cw
            for (row in 0 until ch) {
                val start = row * vRow
                val dst = base + row * rowBytes
                val avail = vb.limit() - start
                if (avail <= 0) break
                val n = minOf(rowBytes, avail)
                vb.position(start)
                vb.get(nv21, dst, n)
                if (n < rowBytes) {  // the last U of the last row lies outside the V buffer
                    val ui = row * uRow + 2 * (cw - 1)
                    if (ui < ub.limit()) nv21[dst + rowBytes - 1] = ub.get(ui)
                }
            }
            return true
        }
        // Any other layout (planar I420, NV12 order, padded): row copies, then interleave
        val rowLenU = (cw - 1) * uPix + 1
        val rowLenV = (cw - 1) * vPix + 1
        if (rowA.size < rowLenU || rowB.size < rowLenV) {
            rowA = ByteArray(maxOf(rowLenU, rowLenV) + 8)
            rowB = ByteArray(maxOf(rowLenU, rowLenV) + 8)
        }
        var pos = base
        for (row in 0 until ch) {
            val uStart = row * uRow
            val vStart = row * vRow
            val uLen = minOf(rowLenU, ub.limit() - uStart)
            val vLen = minOf(rowLenV, vb.limit() - vStart)
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
        return true
    }

    /** true when U[i] == V[i + 1] (same memory, V first) at sample points: the V plane is NV21-ordered. */
    private fun isVuInterleaved(ub: java.nio.ByteBuffer, vb: java.nio.ByteBuffer, rowStride: Int, cw: Int, ch: Int): Boolean {
        if (cw < 2 || ch < 1) return false
        val rows = intArrayOf(0, ch / 3, (2 * ch) / 3, ch - 1)
        val cols = intArrayOf(0, cw / 4, cw / 2, (3 * cw) / 4, cw - 2)
        for (r in rows) for (c in cols) {
            val vi = r * rowStride + 2 * c + 1
            val ui = r * rowStride + 2 * c
            if (vi >= vb.limit() || ui >= ub.limit()) return false
            if (vb.get(vi) != ub.get(ui)) return false
            val vi2 = r * rowStride + 2 * c + 2  // and V[i + 2] == U[i + 1]
            val ui2 = r * rowStride + 2 * c + 1
            if (vi2 < vb.limit() && ui2 < ub.limit() && vb.get(vi2) != ub.get(ui2)) return false
        }
        return true
    }
}
