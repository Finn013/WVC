package com.wvc.phone

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import androidx.camera.view.PreviewView
import android.widget.Button
import android.widget.CheckBox
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.TextView
import android.widget.Toast

/** One screen: IP address, status, source buttons, quality, keep-screen-on. */
@Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
class MainActivity : Activity() {

    companion object {
        const val EXTRA_ASK_PROJECTION = "ask_projection"
        private const val REQ_PERMS = 1
        private const val REQ_PROJECTION = 2
    }

    private lateinit var tvIp: TextView
    private lateinit var tvIpMore: TextView
    private lateinit var tvStatus: TextView
    private lateinit var tvDetail: TextView
    private lateinit var btnBack: Button
    private lateinit var btnFront: Button
    private lateinit var btnScreen: Button
    private lateinit var resGroup: RadioGroup
    private lateinit var res720: RadioButton
    private lateinit var res1080: RadioButton
    private lateinit var keepOn: CheckBox
    private var overlay: PreviewView? = null  // «Камера через экран»: the camera fills the screen
    private var overlayOn = false

    private val ui = Handler(Looper.getMainLooper())
    private var projectionAsked = false
    private var syncing = false

    private val refresh = object : Runnable {
        override fun run() {
            syncOverlay()
            updateUi()
            ui.postDelayed(this, 700)
        }
    }

    private val prefs get() = getSharedPreferences("wvc", Context.MODE_PRIVATE)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        tvIp = findViewById(R.id.ip)
        tvIpMore = findViewById(R.id.ipMore)
        tvStatus = findViewById(R.id.status)
        tvDetail = findViewById(R.id.detail)
        btnBack = findViewById(R.id.btnBack)
        btnFront = findViewById(R.id.btnFront)
        btnScreen = findViewById(R.id.btnScreen)
        resGroup = findViewById(R.id.resGroup)
        res720 = findViewById(R.id.res720)
        res1080 = findViewById(R.id.res1080)
        keepOn = findViewById(R.id.keepOn)
        val pv = PreviewView(this).apply {
            implementationMode = PreviewView.ImplementationMode.COMPATIBLE
            scaleType = PreviewView.ScaleType.FILL_CENTER
            visibility = View.GONE
            setBackgroundColor(0xFF000000.toInt())
        }
        overlay = pv
        addContentView(pv, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT))

        btnBack.setOnClickListener { selectSource("back") }
        btnFront.setOnClickListener { selectSource("front") }
        btnScreen.setOnClickListener { selectSource("screen") }
        resGroup.setOnCheckedChangeListener { _, id ->
            if (syncing) return@setOnCheckedChangeListener
            val r = if (id == R.id.res1080) 1080 else 720
            prefs.edit().putInt("res", r).apply()
            StreamService.instance?.setResolution(r)
        }
        keepOn.isChecked = prefs.getBoolean("keepOn", false)
        keepOn.setOnCheckedChangeListener { _, on ->
            prefs.edit().putBoolean("keepOn", on).apply()
            applyKeepOn()
        }
        findViewById<Button>(R.id.btnStop).setOnClickListener {
            StreamService.instance?.stopEverything()
            finishAndRemoveTask()
        }
        requestNeededPermissions()
        handleIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        StreamService.activity = this
        applyKeepOn()
        ensureService()
        ui.post(refresh)
    }

    override fun onPause() {
        if (overlayOn) hideOverlay()
        StreamService.activity = null
        ui.removeCallbacks(refresh)
        super.onPause()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(i: Intent?) {
        if (i?.getBooleanExtra(EXTRA_ASK_PROJECTION, false) == true) {
            i.removeExtra(EXTRA_ASK_PROJECTION)
            requestProjection()
        }
    }

    /** Show the full-screen camera while WVC wants «камера через экран», hide it otherwise. */
    private fun syncOverlay() {
        val svc = StreamService.instance
        val want = svc != null && svc.camThroughScreen && hasCamera()
        val pv = overlay ?: return
        if (want && !overlayOn) {
            overlayOn = true
            pv.visibility = View.VISIBLE
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_STABLE)
            svc?.attachPreview(pv.surfaceProvider)
        } else if (!want && overlayOn) {
            hideOverlay()
        }
    }

    private fun hideOverlay() {
        overlayOn = false
        overlay?.visibility = View.GONE
        window.decorView.systemUiVisibility = 0
        applyKeepOn()
        StreamService.instance?.attachPreview(null)
    }

    override fun onBackPressed() {
        val svc = StreamService.instance
        if (overlayOn && svc != null) {  // leave «камера через экран»: the same camera, the normal way
            val src = if (svc.source == "front_screen") "front" else "back"
            hideOverlay()
            selectSource(src)
            return
        }
        super.onBackPressed()
    }

    private fun applyKeepOn() {
        if (keepOn.isChecked) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    private fun hasCamera() = checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    private fun requestNeededPermissions() {
        val need = ArrayList<String>()
        if (!hasCamera()) need.add(Manifest.permission.CAMERA)
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) need.add(Manifest.permission.POST_NOTIFICATIONS)
        if (need.isNotEmpty()) requestPermissions(need.toTypedArray(), REQ_PERMS)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_PERMS) {
            ensureService()
            StreamService.instance?.let { it.setSource(it.source) }  // camera may be allowed now
            updateUi()
        }
    }

    /** The service runs while the app is open (or until «Остановить»); it needs the camera permission or a screen consent. */
    private fun ensureService(source: String? = null) {
        if (StreamService.instance != null && source == null) return
        if (!hasCamera() && StreamService.pendingProjection == null) return
        val i = Intent(this, StreamService::class.java)
        if (source != null) i.putExtra(StreamService.EXTRA_SOURCE, source)
        try {
            startForegroundService(i)
        } catch (e: Exception) {
            Toast.makeText(this, "Не удалось запустить трансляцию: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }

    private fun selectSource(src: String) {
        prefs.edit().putString("source", src).apply()
        val svc = StreamService.instance
        if (src == "screen" && (svc == null || !svc.hasProjection)) {
            svc?.setSource("screen")  // reports need_consent to WVC while the dialog is open
            requestProjection()
            return
        }
        if (src != "screen" && !hasCamera()) {
            Toast.makeText(this, "Разрешите доступ к камере", Toast.LENGTH_SHORT).show()
            requestNeededPermissions()
            return
        }
        if (svc != null) svc.setSource(src) else ensureService(src)
        updateUi()
    }

    /** Ask Android for screen capture (needed again after every stop of the projection). */
    fun requestProjection() {
        if (projectionAsked) return
        projectionAsked = true
        try {
            val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
            startActivityForResult(mpm.createScreenCaptureIntent(), REQ_PROJECTION)
        } catch (e: Exception) {
            projectionAsked = false
            Toast.makeText(this, "Показ экрана недоступен на этом телефоне", Toast.LENGTH_LONG).show()
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_PROJECTION) return
        projectionAsked = false
        if (resultCode == RESULT_OK && data != null) {
            StreamService.pendingProjection = Pair(resultCode, data)
            prefs.edit().putString("source", "screen").apply()
            ensureService("screen")  // startForeground with mediaProjection, then the service takes the projection
        } else {
            Toast.makeText(this, "Без разрешения экран показать нельзя", Toast.LENGTH_LONG).show()
            StreamService.instance?.onConsentDenied()
        }
        updateUi()
    }

    private fun updateUi() {
        val ips = Net.localIps()
        tvIp.text = ips.firstOrNull() ?: "нет Wi-Fi"
        if (ips.size > 1) {
            tvIpMore.text = "Ещё: " + ips.drop(1).joinToString(", ")
            tvIpMore.visibility = android.view.View.VISIBLE
        } else {
            tvIpMore.visibility = android.view.View.GONE
        }
        val svc = StreamService.instance
        val source: String
        val res: Int
        if (svc == null) {
            source = prefs.getString("source", "back") ?: "back"
            res = prefs.getInt("res", 720)
            tvStatus.text = if (!hasCamera()) "Нужно разрешение на камеру" else "Запускаю…"
            tvDetail.text = if (!hasCamera()) "Без камеры можно показывать только экран — нажмите «Экран»." else ""
        } else {
            source = svc.source
            res = svc.longSide
            val c = svc.clientAddr
            tvStatus.text = if (c == null) "Ожидание WVC…" else "Подключено ${Net.clientLabel(c)}"
            tvDetail.text = if (c == null) {
                "Сейчас выбрано: ${Net.stateName(source)}"
            } else {
                "Сейчас: ${Net.stateName(svc.state)} · ${res}p · ${Math.round(svc.fps)} кадров/с"
            }
        }
        btnBack.text = (if (source == "back") "● " else "") + "Задняя камера"
        btnFront.text = (if (source == "front") "● " else "") + "Передняя камера"
        btnScreen.text = (if (source == "screen") "● " else "") + "Экран"
        syncing = true
        if (res >= 1080) res1080.isChecked = true else res720.isChecked = true
        syncing = false
    }
}
