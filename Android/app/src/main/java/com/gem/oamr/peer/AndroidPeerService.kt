package com.gem.oamr.peer

import android.content.Context
import android.media.AudioManager
import java.net.HttpURLConnection
import java.net.Inet4Address
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.net.URL
import java.net.URLDecoder
import java.net.URLEncoder
import java.nio.charset.StandardCharsets
import java.security.SecureRandom
import java.util.UUID
import java.util.concurrent.Executors

data class AndroidEndpoint(val id: String, val name: String, val direction: Char)
data class AndroidPeer(val nodeId: String, val alias: String, val host: String, val port: Int)

/**
 * Android implementation of OAMR's TCP 8791 pairing control protocol.
 * It deliberately uses the same URL-encoded catalog format as the desktop
 * PairingService, so desktops can pair with an Android node without a bridge.
 */
class AndroidPeerService private constructor(private val context: Context) {
    private val preferences = context.getSharedPreferences("oamr-peer", Context.MODE_PRIVATE)
    private val workers = Executors.newCachedThreadPool()
    @Volatile private var listener: ServerSocket? = null
    @Volatile private var pairCode = ""
    @Volatile private var codeExpiresAt = 0L
    private val peers = linkedMapOf<String, AndroidPeer>()

    val nodeId: String = preferences.getString("node-id", null) ?: UUID.randomUUID().toString().replace("-", "").also {
        preferences.edit().putString("node-id", it).apply()
    }
    var alias: String
        get() = preferences.getString("alias", android.os.Build.MODEL) ?: android.os.Build.MODEL
        set(value) = preferences.edit().putString("alias", value).apply()

    fun endpoints(): List<AndroidEndpoint> = listOf(
        AndroidEndpoint("android-oboe-input", "Android microphone", 'S'),
        AndroidEndpoint("android-oboe-output", "Android speaker", 'K'),
    )

    @Synchronized fun start(): String {
        if (listener != null) return "Android pairing node is listening on TCP 8791"
        return try {
            listener = ServerSocket(8791)
            workers.execute { acceptLoop() }
            "Android pairing node is listening on TCP 8791"
        } catch (error: Exception) {
            "Could not start pairing node: ${error.message}"
        }
    }

    @Synchronized fun stop() {
        listener?.close()
        listener = null
    }

    @Synchronized fun newPairCode(): String {
        val alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
        pairCode = buildString(6) { repeat(6) { append(alphabet[SecureRandom().nextInt(alphabet.length)]) } }
        codeExpiresAt = System.currentTimeMillis() + 10 * 60 * 1000
        return pairCode
    }

    @Synchronized fun currentPairCode(): String = if (pairCode.isNotBlank() && System.currentTimeMillis() < codeExpiresAt) pairCode else newPairCode()

    fun localIpv4(): List<String> = NetworkInterface.getNetworkInterfaces().toList().flatMap { network ->
        network.inetAddresses.toList().filterIsInstance<Inet4Address>()
            .filter { !it.isLoopbackAddress && !it.isLinkLocalAddress }.map { it.hostAddress.orEmpty() }
    }

    fun knownPeers(): List<AndroidPeer> = synchronized(peers) { peers.values.toList() }

    fun pairDesktop(host: String, code: String): String {
        val path = "/pair?code=${encode(code)}&node=${encode(nodeId)}&alias=${encode(alias)}&port=8791"
        return try {
            val reply = request(host, 8791, path)
            val fields = query("?$reply")
            if (fields.containsKey("error")) return "Pairing failed: ${fields["error"]}"
            val remoteNode = fields["node"] ?: return "Pairing failed: invalid desktop reply"
            synchronized(peers) { peers[remoteNode] = AndroidPeer(remoteNode, fields["alias"].orEmpty().ifBlank { host }, host, 8791) }
            "Paired with ${fields["alias"].orEmpty().ifBlank { host }}"
        } catch (error: Exception) { "Pairing failed: ${error.message}" }
    }

    private fun acceptLoop() {
        while (true) {
            val socket = try { listener?.accept() ?: return } catch (_: Exception) { return }
            workers.execute { handle(socket) }
        }
    }

    private fun handle(socket: Socket) = socket.use {
        val firstLine = it.getInputStream().bufferedReader().readLine().orEmpty()
        val fields = firstLine.split(' ', limit = 3)
        val target = fields.getOrNull(1).orEmpty()
        val body = if (fields.firstOrNull() == "POST") handlePost(target, it.inetAddress.hostAddress.orEmpty()) else "error=method"
        val response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ${body.toByteArray().size}\r\nConnection: close\r\n\r\n$body"
        it.getOutputStream().write(response.toByteArray())
    }

    private fun handlePost(target: String, host: String): String {
        val params = query(target)
        if (target.startsWith("/pair?")) {
            val code = params["code"].orEmpty()
            val remoteNode = params["node"].orEmpty()
            val remoteAlias = params["alias"].orEmpty()
            val remotePort = params["port"]?.toIntOrNull() ?: 0
            if (code != currentPairCode() || remoteNode.isBlank() || remotePort !in 1..65535) return "error=invalid-or-expired-code"
            synchronized(this) { pairCode = "" }
            synchronized(peers) { peers[remoteNode] = AndroidPeer(remoteNode, remoteAlias.ifBlank { host }, host, remotePort) }
            return "node=${encode(nodeId)}&alias=${encode(alias)}&catalog=${encode(catalog())}"
        }
        if (target.startsWith("/update?")) return "ok"
        if (target.startsWith("/unpair?")) {
            params["node"]?.let { synchronized(peers) { peers.remove(it) } }
            return "ok"
        }
        // RTP/Opus route execution is connected by the native transport layer.
        if (target.startsWith("/route?")) return "error=android-rtp-route-not-ready"
        return "error=not-found"
    }

    private fun catalog(): String = endpoints().joinToString(";") { "${it.direction},${encode(it.name)},${encode(it.id)}" }

    private fun request(host: String, port: Int, path: String): String {
        val connection = (URL("http://$host:$port$path").openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"; connectTimeout = 4_000; readTimeout = 6_000
        }
        return try { connection.inputStream.bufferedReader().use { it.readText() } } finally { connection.disconnect() }
    }

    companion object {
        @Volatile private var instance: AndroidPeerService? = null
        fun get(context: Context): AndroidPeerService = instance ?: synchronized(this) {
            instance ?: AndroidPeerService(context.applicationContext).also { instance = it }
        }
        private fun encode(value: String): String = URLEncoder.encode(value, StandardCharsets.UTF_8)
        private fun query(value: String): Map<String, String> = value.substringAfter('?', "").split('&').mapNotNull { row ->
            if (row.isBlank()) null else row.substringBefore('=') to URLDecoder.decode(row.substringAfter('=', ""), StandardCharsets.UTF_8)
        }.toMap()
    }
}
