package com.wvc.phone

import android.os.Build
import java.net.Inet4Address
import java.net.NetworkInterface

object Net {
    /** IPv4 addresses of Wi-Fi / hotspot / Ethernet interfaces, Wi-Fi first (mobile data skipped). */
    fun localIps(): List<String> {
        val result = ArrayList<Pair<Int, String>>()
        try {
            val all = NetworkInterface.getNetworkInterfaces() ?: return emptyList()
            for (ni in all.toList()) {
                if (!ni.isUp || ni.isLoopback) continue
                val n = ni.name ?: ""
                if (n.startsWith("rmnet") || n.startsWith("ccmni") || n.startsWith("dummy") || n.startsWith("tun") ||
                    n.startsWith("v4-rmnet")) continue
                val rank = when {
                    n.startsWith("wlan") -> 0
                    n.startsWith("swlan") || n.startsWith("ap") -> 1
                    n.startsWith("eth") -> 2
                    else -> 3
                }
                for (a in ni.inetAddresses.toList()) {
                    if (a is Inet4Address && !a.isLoopbackAddress) {
                        val h = a.hostAddress
                        if (!h.isNullOrEmpty()) result.add(rank to h)
                    }
                }
            }
        } catch (_: Exception) {
        }
        return result.sortedBy { it.first }.map { it.second }.distinct()
    }

    fun deviceName(): String {
        val man = Build.MANUFACTURER ?: ""
        val model = Build.MODEL ?: "Android"
        val name = if (man.isEmpty() || model.startsWith(man, ignoreCase = true)) model else "$man $model"
        return name.replaceFirstChar { it.uppercase() }
    }

    fun stateName(state: String): String = when (state) {
        "back" -> "задняя камера"
        "front" -> "передняя камера"
        "screen" -> "экран"
        "need_consent" -> "ждём разрешения на показ экрана"
        "camera_error" -> "камера недоступна"
        "no_camera_permission" -> "нет разрешения на камеру"
        else -> state
    }
}
