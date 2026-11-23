package com.sic.timbre

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.os.SystemClock
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.SocketTimeoutException
import java.nio.ByteBuffer

data class Device(val name: String, val ip: String)

sealed class RingResult {
    data class Ok(val device: Device, val alreadyRinging: Boolean) : RingResult()
    data class Choose(val devices: List<Device>) : RingResult()
    data class Error(val message: String) : RingResult()
}

/**
 * Protocolo UDP (puerto 4210):
 *   PING -> "TIMBRE|nombre"      (descubrimiento por broadcast)
 *   RING -> "OK|nombre" o "BUSY|nombre"
 */
class TimbreClient(context: Context) {

    companion object {
        const val PORT = 4210
        private const val REPLY_TIMEOUT_MS = 600L
        private const val DISCOVERY_MS = 1200L
        private const val RETRIES = 3
    }

    private val cm = context.getSystemService(ConnectivityManager::class.java)
    private val prefs = context.getSharedPreferences("timbre", Context.MODE_PRIVATE)

    var saved: Device?
        get() {
            val ip = prefs.getString("ip", null) ?: return null
            val name = prefs.getString("name", null) ?: return null
            return Device(name, ip)
        }
        set(value) {
            prefs.edit().putString("ip", value?.ip).putString("name", value?.name).apply()
        }

    suspend fun ring(): RingResult = withContext(Dispatchers.IO) { doRing() }

    suspend fun discover(): List<Device>? = withContext(Dispatchers.IO) {
        val net = wifiNetwork() ?: return@withContext null
        discover(net)
    }

    suspend fun ringDevice(device: Device): RingResult = withContext(Dispatchers.IO) {
        val net = wifiNetwork() ?: return@withContext RingResult.Error("El teléfono no está conectado a WiFi")
        ringAt(net, device) ?: RingResult.Error("${device.name} no respondió")
    }

    // ------------------------------------------------------------------

    private fun doRing(): RingResult {
        val net = wifiNetwork() ?: return RingResult.Error("El teléfono no está conectado a WiFi")

        // 1) Intento directo a la última IP conocida
        val last = saved
        if (last != null) {
            ringAt(net, last)?.let { return it }
        }

        // 2) La IP cambió o no hay timbre guardado: buscar por broadcast
        val found = discover(net)
        if (found.isEmpty()) return RingResult.Error("No se encontró ningún timbre en esta red WiFi")

        val target = when {
            last != null -> found.firstOrNull { it.name == last.name }
                ?: if (found.size == 1) found[0] else return RingResult.Choose(found)
            found.size == 1 -> found[0]
            else -> return RingResult.Choose(found)
        }
        return ringAt(net, target) ?: RingResult.Error("${target.name} no respondió")
    }

    /** Envía RING con reintentos. Devuelve null si no hubo respuesta. */
    private fun ringAt(net: Network, device: Device): RingResult? {
        val addr = runCatching { InetAddress.getByName(device.ip) }.getOrNull() ?: return null
        repeat(RETRIES) {
            val reply = exchange(net, "RING", listOf(addr), REPLY_TIMEOUT_MS, stopOnFirst = true).firstOrNull()
            if (reply != null) {
                val (ip, kind, name) = reply
                if (kind == "OK" || kind == "BUSY") {
                    val dev = Device(name, ip)
                    saved = dev
                    return RingResult.Ok(dev, alreadyRinging = kind == "BUSY")
                }
            }
        }
        return null
    }

    private fun discover(net: Network): List<Device> =
        exchange(net, "PING", broadcastAddresses(net), DISCOVERY_MS, stopOnFirst = false)
            .filter { it.second == "TIMBRE" }
            .map { Device(it.third, it.first) }
            .distinctBy { it.ip }
            .sortedBy { it.name }

    /** Envía [msg] a cada destino y junta respuestas (ip, tipo, nombre) hasta el timeout. */
    private fun exchange(
        net: Network,
        msg: String,
        targets: List<InetAddress>,
        waitMs: Long,
        stopOnFirst: Boolean
    ): List<Triple<String, String, String>> {
        val out = mutableListOf<Triple<String, String, String>>()
        val socket = try {
            DatagramSocket().apply {
                broadcast = true
                net.bindSocket(this)  // forzar salida por WiFi aunque haya datos móviles
            }
        } catch (e: Exception) {
            return out
        }
        socket.use { s ->
            val data = msg.toByteArray()
            var sent = 0
            for (t in targets) {
                if (runCatching { s.send(DatagramPacket(data, data.size, t, PORT)) }.isSuccess) sent++
            }
            if (sent == 0) return out

            val buf = ByteArray(128)
            val end = SystemClock.elapsedRealtime() + waitMs
            while (true) {
                val left = end - SystemClock.elapsedRealtime()
                if (left <= 0) break
                s.soTimeout = left.toInt().coerceAtLeast(1)
                val p = DatagramPacket(buf, buf.size)
                try {
                    s.receive(p)
                } catch (e: SocketTimeoutException) {
                    break
                } catch (e: Exception) {
                    break
                }
                val txt = String(p.data, 0, p.length, Charsets.UTF_8).trim()
                val ip = p.address?.hostAddress ?: continue
                val parts = txt.split("|", limit = 2)
                if (parts.size != 2) continue
                out += Triple(ip, parts[0], parts[1])
                if (stopOnFirst) break
            }
        }
        return out
    }

    @Suppress("DEPRECATION")
    private fun wifiNetwork(): Network? {
        cm.activeNetwork?.let { n ->
            if (cm.getNetworkCapabilities(n)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true) return n
        }
        // WiFi sin internet: Android puede dejar los datos móviles como red activa
        return cm.allNetworks.firstOrNull {
            cm.getNetworkCapabilities(it)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
        }
    }

    private fun broadcastAddresses(net: Network): List<InetAddress> {
        val list = mutableListOf<InetAddress>()
        cm.getLinkProperties(net)?.linkAddresses?.forEach { la ->
            val a = la.address
            if (a is Inet4Address) {
                val prefix = la.prefixLength
                val ip = ByteBuffer.wrap(a.address).int
                val hostMask = if (prefix >= 32) 0 else (-1 ushr prefix)
                val bcast = ip or hostMask
                list += InetAddress.getByAddress(ByteBuffer.allocate(4).putInt(bcast).array())
            }
        }
        list += InetAddress.getByName("255.255.255.255")
        return list.distinct()
    }
}
