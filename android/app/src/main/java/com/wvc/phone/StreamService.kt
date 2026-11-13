package com.wvc.phone

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.content.res.Configuration
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.view.OrientationEventListener
import android.view.Surface
import androidx.camera.core.Preview
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.lifecycle.LifecycleService

/**
 * Foreground service (camera | mediaProjection) that keeps streaming with the screen off.
 * WVC on the PC connects to it (FrameServer) and switches the source with text commands.
 */
class StreamService : LifecycleService(), FrameServer.Listener {

    companion object {
        const val ACTION_STOP = "com.wvc.phone.STOP"
        const val EXTRA_SOURCE = "source"
        private const val NOTIF_ID = 1
        private const val CONSENT_ID = 2
        private const val CH_STREAM = "stream"
        private const val CH_ALERT = "alert"

        @Volatile var instance: StreamService? = null
            private set

        /** Screen-capture consent from the activity, consumed by the service right after startForeground. */
        @Volatile var pendingProjection: Pair<Int, Intent>? = null

        /** The visible activity (null when it is in the background). */
        @Volatile var activity: MainActivity? = null

        val SOURCES = listOf("back", "front", "screen", "back_screen", "front_screen")
    }

    private val main = Handler(Looper.getMainLooper())
    private lateinit var server: FrameServer
    private lateinit var camera: CameraSource
    private lateinit var screen: ScreenSource
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private var orientation: OrientationEventListener? = null
    private var stopped = false

    /** What should be shown: back / front / screen / back_screen / front_screen (camera through the screen) */
    var source = "back"
        private set
    /** What is shown now (reported to WVC): back / front / screen / need_consent / camera_error ... */
    var state = "back"
        private set
    var longSide = 720
    /** Frame rate WVC asked for ("FPS 60" when its camera runs at 50/60); 30 otherwise. Camera + H.264 only. */
    var wantFps = 30
        private set
    var clientAddr: String? = null
        private set
    var fps = 0.0
        private set
    val hasProjection: Boolean get() = screen.hasProjection
    /** WVC asked for H.264 on this connection. */
    var h264 = false
        private set
    /** «Камера через экран»: the activity's full-screen view the camera is shown in. */
    private var previewTarget: Preview.SurfaceProvider? = null
    val camThroughScreen: Boolean get() = source.endsWith("_screen")

    private var lastFrames = 0
    private var lastTick = 0L
    private var lastNotifText = ""

    private val tick = object : Runnable {
        override fun run() {
            server.checkStall()
            val now = SystemClock.elapsedRealtime()
            val sent = server.framesSent
            if (lastTick != 0L && now > lastTick) fps = (sent - lastFrames) * 1000.0 / (now - lastTick)
            lastFrames = sent
            lastTick = now
            if (clientAddr != null) server.sendText("STATE $state")  // also a keep-alive for WVC
            updateNotification()
            main.postDelayed(this, 2000)
        }
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        val prefs = getSharedPreferences("wvc", Context.MODE_PRIVATE)
        source = prefs.getString("source", "back") ?: "back"
        if (source !in SOURCES) source = "back"
        longSide = if (prefs.getInt("res", 720) >= 1080) 1080 else 720
        state = source
        createChannels()
        server = FrameServer(this, Net.deviceName())
        camera = CameraSource(this, this, server) { err ->
            main.post {
                if (err == "encoder_error") onEncoderFailed() else if (source != "screen") setState(err)
            }
        }
        screen = ScreenSource(this, server) { main.post { onProjectionStopped() } }
        screen.onEncoderFailed = { main.post { onEncoderFailed() } }
        server.start()
        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        multicastLock = wifi.createMulticastLock("wvc-discovery").apply {
            setReferenceCounted(false)
            try { acquire() } catch (_: Exception) {}
        }
        orientation = object : OrientationEventListener(this) {
            override fun onOrientationChanged(deg: Int) {
                if (deg == ORIENTATION_UNKNOWN) return
                val rot = when (deg) {
                    in 45..134 -> Surface.ROTATION_270
                    in 135..224 -> Surface.ROTATION_180
                    in 225..314 -> Surface.ROTATION_90
                    else -> Surface.ROTATION_0
                }
                camera.setRotation(rot)
            }
        }
        try { orientation?.enable() } catch (_: Exception) {}
        main.postDelayed(tick, 2000)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        super.onStartCommand(intent, flags, startId)
        if (intent?.action == ACTION_STOP) {
            stopEverything()
            return START_NOT_STICKY
        }
        intent?.getStringExtra(EXTRA_SOURCE)?.let { if (it in SOURCES) saveSource(it) }
        if (!goForeground()) {
            stopEverything()
            return START_NOT_STICKY
        }
        consumePendingProjection()
        applySource()
        return START_NOT_STICKY
    }

    private fun hasCameraPermission() =
        checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    /** startForeground with the types we may use right now. */
    private fun goForeground(): Boolean {
        var types = 0
        if (hasCameraPermission() && Build.VERSION.SDK_INT >= 30) types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
        if (screen.hasProjection || pendingProjection != null) {
            if (Build.VERSION.SDK_INT >= 29) types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
        }
        if (types == 0 && Build.VERSION.SDK_INT >= 29 && !hasCameraPermission()) return false
        return try {
            ServiceCompat.startForeground(this, NOTIF_ID, buildNotification(), types)
            true
        } catch (e: Exception) {
            false
        }
    }

    private fun consumePendingProjection() {
        val pp = pendingProjection ?: return
        pendingProjection = null
        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        val p: MediaProjection? = try { mpm.getMediaProjection(pp.first, pp.second) } catch (e: Exception) { null }
        if (p != null && screen.start(p, longSide)) {
            getSystemService(NotificationManager::class.java).cancel(CONSENT_ID)
        }
    }

    // ---- control (main thread)

    private fun saveSource(src: String) {
        source = src
        getSharedPreferences("wvc", Context.MODE_PRIVATE).edit().putString("source", src).apply()
    }

    fun setSource(src: String) {
        if (src !in SOURCES) return
        saveSource(src)
        applySource()
    }

    fun setResolution(res: Int) {
        val r = if (res >= 1080) 1080 else 720
        if (r == longSide) {
            server.sendText("RES $r")
            return
        }
        longSide = r
        getSharedPreferences("wvc", Context.MODE_PRIVATE).edit().putInt("res", r).apply()
        server.sendText("RES $r")
        if (camera.isRunning) applySource()
        screen.resize(r)
    }

    /** WVC 0.13: "FPS 30|60". The answer "FPS n" says what the phone will try. */
    fun setFps(f: Int) {
        val v = if (f >= 50) 60 else 30
        server.sendText("FPS $v")
        if (v == wantFps) return
        wantFps = v
        if (camera.isRunning) applySource()
    }

    private fun applySource() {
        if (stopped) return
        val connected = clientAddr != null
        when (source) {
            "back", "front" -> {
                screen.active = false
                if (!hasCameraPermission()) {
                    camera.stop()
                    setState("no_camera_permission")
                } else {
                    if (connected) camera.start(source == "front", longSide, h264, null, wantFps) else camera.stop()
                    setState(source)
                }
            }
            "screen" -> {
                camera.stop()
                if (screen.hasProjection) {
                    screen.active = connected
                    setState("screen")
                } else {
                    screen.active = false
                    setState("need_consent")
                    if (connected) askConsent()
                }
            }
            "back_screen", "front_screen" -> {
                // the camera fills the phone screen (the activity), the screen is what is sent
                if (!screen.hasProjection) {
                    camera.stop()
                    screen.active = false
                    setState("need_consent")
                    if (connected) askConsent()
                    return
                }
                val target = previewTarget
                if (target == null || !hasCameraPermission()) {
                    camera.stop()
                    screen.active = false
                    setState(if (!hasCameraPermission()) "no_camera_permission" else "need_activity")
                    if (connected) openActivity()
                    return
                }
                camera.start(source == "front_screen", longSide, false, target)
                screen.active = connected
                setState(source)
            }
        }
    }

    /** The activity shows (or stops showing) the full-screen camera view. */
    fun attachPreview(target: Preview.SurfaceProvider?) {
        if (previewTarget === target) return
        previewTarget = target
        if (camThroughScreen) applySource()
    }

    private fun openActivity() {
        if (activity != null) return
        try {
            startActivity(Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP))
        } catch (_: Exception) {
        }
    }

    /** No usable H.264 encoder on this phone: tell WVC and go on in JPEG. */
    private fun onEncoderFailed() {
        if (!h264) return
        h264 = false
        server.useH264(false)
        server.sendText("CODEC jpeg")
        screen.setH264(false)
        applySource()
    }

    private fun setState(s: String) {
        state = s
        server.sendText("STATE $s")
        updateNotification()
    }

    private fun askConsent() {
        val a = activity
        if (a != null) {
            a.requestProjection()
            return
        }
        val i = Intent(this, MainActivity::class.java)
            .putExtra(MainActivity.EXTRA_ASK_PROJECTION, true)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        val pi = PendingIntent.getActivity(this, 2, i, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val n = NotificationCompat.Builder(this, CH_ALERT)
            .setSmallIcon(R.drawable.ic_stat)
            .setContentTitle("WVC просит показать экран")
            .setContentText("Нажмите, чтобы разрешить показ экрана телефона")
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setAutoCancel(true)
            .setContentIntent(pi)
            .build()
        try { getSystemService(NotificationManager::class.java).notify(CONSENT_ID, n) } catch (_: Exception) {}
    }

    /** The user declined screen capture in the system dialog. */
    fun onConsentDenied() {
        if (source == "screen") setState("need_consent")
    }

    private fun onProjectionStopped() {
        if (stopped) return
        if (source == "screen") setState("need_consent")
        goForeground()  // drop the mediaProjection type
    }

    // ---- FrameServer.Listener (network threads)

    override fun onClientConnected(address: String) {
        main.post {
            clientAddr = address
            h264 = false  // WVC 0.11 sends "CODEC h264" first; older WVC gets JPEG as before
            wantFps = 30  // WVC 0.13 sends "FPS 60" when it wants it
            screen.setH264(false)
            acquireLocks()
            server.sendText("NAME ${Net.deviceName()}")
            server.sendText("RES $longSide")
            applySource()
        }
    }

    override fun onClientDisconnected() {
        main.post {
            if (server.isConnected) return@post
            clientAddr = null
            releaseLocks()
            camera.stop()
            screen.active = false
            fps = 0.0
            updateNotification()
        }
    }

    override fun onCommand(line: String) {
        main.post {
            val parts = line.split(' ', limit = 2)
            val arg = parts.getOrNull(1)?.trim() ?: ""
            when (parts[0].uppercase()) {
                "SRC" -> setSource(arg.lowercase())
                "RES" -> arg.toIntOrNull()?.let { setResolution(it) }
                "CODEC" -> {
                    val on = arg.lowercase() == "h264"
                    h264 = on
                    server.useH264(on)
                    server.sendText(if (on) "CODEC h264" else "CODEC jpeg")
                    screen.setH264(on)
                    applySource()
                }
                "KEY" -> server.requestKeyFrame()
                "FPS" -> arg.toIntOrNull()?.let { setFps(it) }
            }
        }
    }

    // ---- locks, notification, lifecycle

    @Suppress("DEPRECATION")
    private fun acquireLocks() {
        try {
            if (wakeLock == null) {
                val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
                wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "wvc:stream").apply {
                    setReferenceCounted(false)
                    acquire()
                }
            }
            if (wifiLock == null) {
                val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
                wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "wvc:stream").apply {
                    setReferenceCounted(false)
                    acquire()
                }
            }
        } catch (_: Exception) {
        }
    }

    private fun releaseLocks() {
        try { wakeLock?.release() } catch (_: Exception) {}
        wakeLock = null
        try { wifiLock?.release() } catch (_: Exception) {}
        wifiLock = null
    }

    private fun createChannels() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CH_STREAM, "Трансляция", NotificationManager.IMPORTANCE_LOW))
        nm.createNotificationChannel(
            NotificationChannel(CH_ALERT, "Запросы от WVC", NotificationManager.IMPORTANCE_HIGH))
    }

    fun statusLine(): String {
        val c = clientAddr
        return if (c == null) {
            val ip = Net.localIps().firstOrNull()
            "Ожидание WVC…" + if (ip != null) " · IP $ip" else " · нет Wi-Fi (можно по USB)"
        } else {
            "Подключено ${Net.clientLabel(c)} · ${Net.stateName(state)}" + (if (h264) " · H.264" else "")
        }
    }

    private fun buildNotification(): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val stop = PendingIntent.getService(this, 1, Intent(this, StreamService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val text = statusLine()
        lastNotifText = text
        return NotificationCompat.Builder(this, CH_STREAM)
            .setSmallIcon(R.drawable.ic_stat)
            .setContentTitle("WVC Phone")
            .setContentText(text)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(open)
            .addAction(0, "Остановить", stop)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    private fun updateNotification() {
        if (stopped || statusLine() == lastNotifText) return
        try { getSystemService(NotificationManager::class.java).notify(NOTIF_ID, buildNotification()) } catch (_: Exception) {}
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        screen.resize(longSide)  // the screen turned: new width / height
    }

    fun stopEverything() {
        if (!stopped) {
            stopped = true
            main.removeCallbacks(tick)
            try { orientation?.disable() } catch (_: Exception) {}
            camera.release()
            screen.release()
            server.stop()
            releaseLocks()
            try { multicastLock?.release() } catch (_: Exception) {}
            getSystemService(NotificationManager::class.java).cancel(CONSENT_ID)
        }
        try { ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE) } catch (_: Exception) {}
        stopSelf()
    }

    override fun onDestroy() {
        stopEverything()
        if (instance === this) instance = null
        super.onDestroy()
    }
}
