package com.gem.oamr.control

import org.json.JSONArray
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.nio.charset.StandardCharsets

data class DesktopDevice(val id: String, val name: String, val renderLoopback: Boolean = false)
data class PairedEndpoint(val id: String, val name: String, val direction: String)
data class PairedPeer(val nodeId: String, val alias: String, val host: String, val port: Int, val endpoints: List<PairedEndpoint>)

data class DesktopRoute(
    val id: Long,
    val label: String,
    val enabled: Boolean,
    val network: Boolean,
    val profile: String,
)

data class DesktopSnapshot(
    val sources: List<DesktopDevice> = emptyList(),
    val sinks: List<DesktopDevice> = emptyList(),
    val routes: List<DesktopRoute> = emptyList(),
    val localAlias: String = "",
    val pairCode: String = "",
    val peers: List<PairedPeer> = emptyList(),
) {
    val sourceCount get() = sources.size
    val sinkCount get() = sinks.size
}

/** HTTP client for an explicitly user-selected desktop OAMR server. */
class DesktopController(host: String, port: String) {
    private val baseUrl = "http://${host.trim()}:${port.trim()}"

    fun snapshot(): DesktopSnapshot {
        val devices = org.json.JSONObject(request("GET", "/api/devices"))
        val routes = JSONArray(request("GET", "/api/routes"))
        val local = org.json.JSONObject(request("GET", "/api/pair/local"))
        return DesktopSnapshot(
            sources = devices.getJSONArray("sources").toDevices(true),
            sinks = devices.getJSONArray("sinks").toDevices(false),
            routes = routes.toRoutes(),
            localAlias = local.optString("alias"),
            pairCode = request("GET", "/api/pair/code"),
            peers = JSONArray(request("GET", "/api/pair/peers")).toPeers(),
        )
    }

    fun toggle(route: DesktopRoute) = request("POST", "/api/routes/${route.id}/toggle?enabled=${!route.enabled}")
    fun delete(route: DesktopRoute) = request("POST", "/api/routes/${route.id}/delete")
    fun stopAll() = request("POST", "/api/stop")
    fun createMatrix(source: DesktopDevice, sink: DesktopDevice) = request("POST",
        "/api/matrix?routes=" + encode("${source.id}\t${source.renderLoopback}\t${sink.id}"))
    fun createSender(source: DesktopDevice, host: String, port: String) = request("POST",
        "/api/network/send?source=${encode(source.id)}&loopback=${source.renderLoopback}&host=${encode(host)}&port=${encode(port)}&quality=medium&max-latency-ms=100&mode=auto")
    fun createReceiver(sink: DesktopDevice, port: String) = request("POST",
        "/api/network/receive?sink=${encode(sink.id)}&port=${encode(port)}&quality=medium&max-latency-ms=100&mode=auto")
    fun regeneratePairCode() = request("POST", "/api/pair/code")
    fun saveExposure(alias: String, sources: List<DesktopDevice>, sinks: List<DesktopDevice>) {
        val endpoints = (sources.map { "S\t${it.id}\t${it.name}" } + sinks.map { "K\t${it.id}\t${it.name}" }).joinToString("\n")
        request("POST", "/api/pair/config?alias=${encode(alias)}&endpoints=${encode(endpoints)}")
    }
    fun pair(host: String, code: String, alias: String) = request("POST",
        "/api/pair/connect?host=${encode(host)}&port=8791&alias=${encode(alias)}&code=${encode(code)}")
    fun deletePeer(peer: PairedPeer) = request("POST", "/api/pair/delete?node=${encode(peer.nodeId)}")
    fun createPairedRoute(peer: PairedPeer, kind: String, local: DesktopDevice, remote: PairedEndpoint) = request("POST",
        "/api/paired/route?peer=${encode(peer.nodeId)}&kind=$kind&local=${encode(local.id)}&remote=${encode(remote.id)}&quality=medium&max-latency-ms=100&mode=auto")

    private fun request(method: String, path: String): String {
        val connection = (URL(baseUrl + path).openConnection() as HttpURLConnection).apply {
            requestMethod = method
            connectTimeout = 4_000
            readTimeout = 6_000
        }
        return try {
            val stream = if (connection.responseCode in 200..299) connection.inputStream else connection.errorStream
            val body = stream?.bufferedReader()?.use { it.readText() }.orEmpty()
            if (connection.responseCode !in 200..299) error("桌面 OAMR 返回 HTTP ${connection.responseCode}: $body")
            body
        } finally {
            connection.disconnect()
        }
    }
}

private fun encode(value: String): String = URLEncoder.encode(value, StandardCharsets.UTF_8)

private fun JSONArray.toDevices(sources: Boolean): List<DesktopDevice> = buildList {
    for (index in 0 until length()) {
        val item = getJSONObject(index)
        add(DesktopDevice(item.getString("id"), item.getString("name"), sources && item.optBoolean("renderLoopback")))
    }
}

private fun JSONArray.toPeers(): List<PairedPeer> = buildList {
    for (index in 0 until length()) {
        val item = getJSONObject(index)
        val endpoints = item.getJSONArray("endpoints")
        add(PairedPeer(
            nodeId = item.getString("nodeId"), alias = item.getString("alias"), host = item.getString("host"), port = item.getInt("port"),
            endpoints = buildList {
                for (endpoint in 0 until endpoints.length()) {
                    val value = endpoints.getJSONObject(endpoint)
                    add(PairedEndpoint(value.getString("id"), value.getString("name"), value.getString("direction")))
                }
            }
        ))
    }
}

private fun JSONArray.toRoutes(): List<DesktopRoute> = buildList {
    for (index in 0 until length()) {
        val item = getJSONObject(index)
        val profile = if (item.optBoolean("network"))
            "${item.optString("quality")} · ${item.optInt("latency")} ms · ${item.optString("mode")}" else "本地路线"
        add(DesktopRoute(item.getLong("id"), item.getString("label"), item.getBoolean("enabled"), item.optBoolean("network"), profile))
    }
}
