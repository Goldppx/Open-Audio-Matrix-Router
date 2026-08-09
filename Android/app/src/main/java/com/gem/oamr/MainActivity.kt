package com.gem.oamr

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.DarkMode
import androidx.compose.material.icons.outlined.Devices
import androidx.compose.material.icons.outlined.Home
import androidx.compose.material.icons.outlined.Language
import androidx.compose.material.icons.outlined.LightMode
import androidx.compose.material.icons.outlined.Link
import androidx.compose.material.icons.outlined.Mic
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.Router
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.outlined.Speaker
import androidx.compose.material.icons.outlined.Stop
import androidx.compose.material.icons.outlined.Wifi
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import com.gem.oamr.audio.AudioDevice
import com.gem.oamr.audio.OboeAudioEngine
import com.gem.oamr.control.DesktopController
import com.gem.oamr.control.DesktopDevice
import com.gem.oamr.control.DesktopRoute
import com.gem.oamr.control.DesktopSnapshot
import com.gem.oamr.peer.AndroidEndpoint
import com.gem.oamr.peer.AndroidPeer
import com.gem.oamr.peer.AndroidPeerService
import com.gem.oamr.peer.OamrNodeService
import com.gem.oamr.ui.theme.OAMRTheme

private const val UI_PREFERENCES = "oamr-ui"

private enum class Destination(val icon: ImageVector) {
    Home(Icons.Outlined.Home), Settings(Icons.Outlined.Settings)
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent { OAMRApp(this) }
    }
}

@Composable
@OptIn(ExperimentalMaterial3Api::class)
private fun OAMRApp(activity: ComponentActivity) {
    val preferences = remember { activity.getSharedPreferences(UI_PREFERENCES, ComponentActivity.MODE_PRIVATE) }
    var english by remember { mutableStateOf(preferences.getBoolean("english", false)) }
    var darkMode by remember { mutableStateOf(preferences.getBoolean("dark-mode", false)) }
    var autoStartNode by remember { mutableStateOf(preferences.getBoolean("auto-start-node", false)) }
    var desktopControls by remember { mutableStateOf(preferences.getBoolean("desktop-controls", true)) }
    var destination by remember { mutableStateOf(Destination.Home) }

    OAMRTheme(darkTheme = darkMode) {
        val engine = remember { OboeAudioEngine(activity) }
        val node = remember { AndroidPeerService.get(activity) }
        var devices by remember { mutableStateOf(engine.listDevices()) }
        var nodeStatus by remember { mutableStateOf(t(english, "节点尚未启动", "Node is stopped")) }
        var audioStatus by remember { mutableStateOf(t(english, "麦克风待命", "Microphone is idle")) }
        var pairCode by remember { mutableStateOf(node.currentPairCode()) }
        var peers by remember { mutableStateOf(node.knownPeers()) }
        var desktopHost by remember { mutableStateOf("192.168.1.") }
        var desktopPort by remember { mutableStateOf("8790") }
        var desktopStatus by remember { mutableStateOf(t(english, "未连接桌面 OAMR", "Desktop OAMR is not connected")) }
        var desktopSnapshot by remember { mutableStateOf(DesktopSnapshot()) }
        var androidPairHost by remember { mutableStateOf("") }
        var androidPairCode by remember { mutableStateOf("") }
        var selectedPeer by remember { mutableStateOf<AndroidPeer?>(null) }
        var selectedEndpoint by remember { mutableStateOf<AndroidEndpoint?>(null) }
        var matrixDirection by remember { mutableStateOf("send") }
        var selectedDesktopSource by remember { mutableStateOf<DesktopDevice?>(null) }
        var selectedDesktopSink by remember { mutableStateOf<DesktopDevice?>(null) }
        var permissionAction by remember { mutableIntStateOf(0) }

        fun startNodeService() {
            ContextCompat.startForegroundService(activity, Intent(activity, OamrNodeService::class.java))
            nodeStatus = node.start()
        }
        fun changeLanguage(value: Boolean) {
            if (nodeStatus == t(english, "节点尚未启动", "Node is stopped")) nodeStatus = t(value, "节点尚未启动", "Node is stopped")
            if (audioStatus == t(english, "麦克风待命", "Microphone is idle")) audioStatus = t(value, "麦克风待命", "Microphone is idle")
            if (desktopStatus == t(english, "未连接桌面 OAMR", "Desktop OAMR is not connected")) desktopStatus = t(value, "未连接桌面 OAMR", "Desktop OAMR is not connected")
            english = value
            preferences.edit().putBoolean("english", value).apply()
        }
        val microphonePermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (!granted) {
                audioStatus = t(english, "未授予麦克风权限", "Microphone permission was denied")
            } else if (permissionAction == 1) {
                startNodeService()
            } else {
                audioStatus = engine.startInput()
            }
            permissionAction = 0
        }
        fun ensureNodeRunning() {
            if (ContextCompat.checkSelfPermission(activity, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) startNodeService()
            else { permissionAction = 1; microphonePermission.launch(Manifest.permission.RECORD_AUDIO) }
        }
        fun loadDesktop() {
            Thread {
                try {
                    val snapshot = DesktopController(desktopHost, desktopPort).snapshot()
                    activity.runOnUiThread {
                        desktopSnapshot = snapshot
                        selectedDesktopSource = snapshot.sources.find { it.id == selectedDesktopSource?.id }
                        selectedDesktopSink = snapshot.sinks.find { it.id == selectedDesktopSink?.id }
                        desktopStatus = t(english, "已连接：${snapshot.sourceCount} 输入 · ${snapshot.sinkCount} 输出", "Connected: ${snapshot.sourceCount} inputs · ${snapshot.sinkCount} outputs")
                    }
                } catch (error: Exception) {
                    activity.runOnUiThread { desktopStatus = t(english, "连接失败：${error.message}", "Connection failed: ${error.message}") }
                }
            }.start()
        }
        fun desktopOperation(operation: DesktopController.() -> Unit) {
            Thread {
                try {
                    DesktopController(desktopHost, desktopPort).operation()
                    loadDesktop()
                } catch (error: Exception) {
                    activity.runOnUiThread { desktopStatus = t(english, "操作失败：${error.message}", "Operation failed: ${error.message}") }
                }
            }.start()
        }
        node.routeHandler = { direction, endpoint, host, port ->
            when {
                direction == "send" && endpoint == "android-oboe-input" -> engine.startRtpSender(host, port)
                direction == "receive" && endpoint == "android-oboe-output" -> engine.startRtpReceiver(port)
                else -> "error=unsupported-android-endpoint"
            }
        }
        LaunchedEffect(autoStartNode) {
            if (autoStartNode && ContextCompat.checkSelfPermission(activity, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) startNodeService()
        }

        Scaffold(
            topBar = {
                CenterAlignedTopAppBar(
                    title = { Text(if (destination == Destination.Home) "OAMR" else t(english, "设置", "Settings"), fontWeight = FontWeight.SemiBold) },
                    actions = {
                        IconButton(onClick = {
                            darkMode = !darkMode
                            preferences.edit().putBoolean("dark-mode", darkMode).apply()
                        }) {
                            Icon(if (darkMode) Icons.Outlined.LightMode else Icons.Outlined.DarkMode, t(english, "切换主题", "Toggle theme"))
                        }
                    },
                    colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.surface)
                )
            },
            bottomBar = {
                NavigationBar {
                    Destination.entries.forEach { item ->
                        NavigationBarItem(
                            selected = destination == item,
                            onClick = { destination = item },
                            icon = { Icon(item.icon, null) },
                            label = { Text(if (item == Destination.Home) t(english, "主页", "Home") else t(english, "设置", "Settings")) }
                        )
                    }
                }
            }
        ) { padding ->
            if (destination == Destination.Home) HomePage(
                padding, english, node, nodeStatus, audioStatus, peers, devices, desktopStatus, desktopSnapshot,
                onRefresh = { peers = node.knownPeers(); devices = engine.listDevices(); if (desktopControls) loadDesktop() },
                onStartNode = { ensureNodeRunning() },
                onStartMic = {
                    if (ContextCompat.checkSelfPermission(activity, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) audioStatus = engine.startInput()
                    else { permissionAction = 2; microphonePermission.launch(Manifest.permission.RECORD_AUDIO) }
                },
                onStopDesktop = { desktopOperation { stopAll() } }
            ) else SettingsPage(
                padding = padding, english = english, darkMode = darkMode, autoStartNode = autoStartNode, desktopControls = desktopControls,
                pairCode = pairCode, node = node, nodeStatus = nodeStatus, peers = peers, devices = devices,
                androidPairHost = androidPairHost, androidPairCode = androidPairCode, desktopHost = desktopHost, desktopPort = desktopPort,
                desktopStatus = desktopStatus, desktopSnapshot = desktopSnapshot, selectedPeer = selectedPeer, selectedEndpoint = selectedEndpoint,
                matrixDirection = matrixDirection, selectedDesktopSource = selectedDesktopSource, selectedDesktopSink = selectedDesktopSink,
                onEnglish = { changeLanguage(it) },
                onDarkMode = { darkMode = it; preferences.edit().putBoolean("dark-mode", it).apply() },
                onAutoStart = { autoStartNode = it; preferences.edit().putBoolean("auto-start-node", it).apply() },
                onDesktopControls = { desktopControls = it; preferences.edit().putBoolean("desktop-controls", it).apply() },
                onStartNode = { ensureNodeRunning() },
                onNewCode = { pairCode = node.newPairCode() },
                onPairHost = { androidPairHost = it }, onPairCode = { androidPairCode = it },
                onPair = {
                    Thread { val message = node.pairDesktop(androidPairHost, androidPairCode); activity.runOnUiThread { nodeStatus = message; peers = node.knownPeers() } }.start()
                },
                onRefreshPeers = { peers = node.knownPeers() }, onRefreshDevices = { devices = engine.listDevices() },
                onDesktopHost = { desktopHost = it }, onDesktopPort = { desktopPort = it }, onConnectDesktop = { loadDesktop() },
                onDesktopSource = { selectedDesktopSource = it }, onDesktopSink = { selectedDesktopSink = it },
                onCreateDesktopMatrix = { desktopOperation { createMatrix(selectedDesktopSource!!, selectedDesktopSink!!) } },
                onPeer = { selectedPeer = it; selectedEndpoint = null }, onDirection = { matrixDirection = it; selectedEndpoint = null }, onEndpoint = { selectedEndpoint = it },
                onCreateAndroidMatrix = {
                    val peer = selectedPeer ?: return@SettingsPage
                    val endpoint = selectedEndpoint ?: return@SettingsPage
                    Thread { val message = node.createPairedMatrix(peer.nodeId, matrixDirection, endpoint); activity.runOnUiThread { nodeStatus = message } }.start()
                }
            )
        }
    }
}

@Composable
private fun HomePage(
    padding: PaddingValues, english: Boolean, node: AndroidPeerService, nodeStatus: String, audioStatus: String,
    peers: List<AndroidPeer>, devices: List<AudioDevice>, desktopStatus: String, desktop: DesktopSnapshot,
    onRefresh: () -> Unit, onStartNode: () -> Unit, onStartMic: () -> Unit, onStopDesktop: () -> Unit,
) {
    val ips = node.localIpv4().joinToString().ifBlank { t(english, "未连接局域网", "No LAN connection") }
    val inputs = devices.count { it.direction == "输入" }
    val outputs = devices.size - inputs
    LazyColumn(
        modifier = Modifier.fillMaxSize().padding(padding), contentPadding = PaddingValues(start = 16.dp, top = 12.dp, end = 16.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        item {
            StatusCard(
                icon = Icons.Outlined.Router, title = t(english, "本机节点", "This node"),
                primary = ips, secondary = "TCP 8791 · $nodeStatus", running = !nodeStatus.contains("stopped", true) && !nodeStatus.contains("未启动")
            )
        }
        item {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                MetricCard(Modifier.weight(1f), Icons.Outlined.Mic, t(english, "输入", "Inputs"), inputs.toString(), audioStatus)
                MetricCard(Modifier.weight(1f), Icons.Outlined.Speaker, t(english, "输出", "Outputs"), outputs.toString(), t(english, "可用端点", "Available endpoints"))
            }
        }
        item {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                MetricCard(Modifier.weight(1f), Icons.Outlined.Devices, t(english, "已连接设备", "Peers"), peers.size.toString(), t(english, "已配对节点", "Paired nodes"))
                MetricCard(Modifier.weight(1f), Icons.Outlined.Link, t(english, "运行路线", "Routes"), desktop.routes.count { it.enabled }.toString(), t(english, "桌面矩阵", "Desktop matrix"))
            }
        }
        item { SectionTitle(t(english, "连接状态", "Connection status"), Icons.Outlined.Wifi, onRefresh) }
        item {
            Card(shape = MaterialTheme.shapes.extraLarge) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    StatusLine(Icons.Outlined.Router, t(english, "桌面 OAMR", "Desktop OAMR"), desktopStatus)
                    HorizontalDivider()
                    if (peers.isEmpty()) Text(t(english, "尚无已配对设备。请到设置页完成配对。", "No paired devices yet. Pair a device in Settings."), style = MaterialTheme.typography.bodyMedium)
                    else peers.forEach { peer -> StatusLine(Icons.Outlined.Devices, peer.alias, "${peer.host}:${peer.port} · ${peer.endpoints.size} ${t(english, "个端点", "endpoints")}") }
                }
            }
        }
        item { SectionTitle(t(english, "音频端点", "Audio endpoints"), Icons.Outlined.Speaker) }
        items(devices.take(6), key = { it.id }) { device -> EndpointRow(device, english) }
        if (desktop.routes.isNotEmpty()) {
            item { SectionTitle(t(english, "当前路线", "Active routes"), Icons.Outlined.Link) }
            items(desktop.routes, key = { it.id }) { route -> RouteRow(route, english) }
            item { OutlinedButton(onClick = onStopDesktop, modifier = Modifier.fillMaxWidth()) { Icon(Icons.Outlined.Stop, null); Text(t(english, "停止桌面全部路线", "Stop all desktop routes"), Modifier.padding(start = 8.dp)) } }
        }
        item {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Button(onClick = onStartNode, modifier = Modifier.weight(1f)) { Text(t(english, "启动节点", "Start node")) }
                OutlinedButton(onClick = onStartMic, modifier = Modifier.weight(1f)) { Text(t(english, "启动麦克风", "Start mic")) }
            }
        }
    }
}

@Composable
private fun SettingsPage(
    padding: PaddingValues, english: Boolean, darkMode: Boolean, autoStartNode: Boolean, desktopControls: Boolean,
    pairCode: String, node: AndroidPeerService, nodeStatus: String, peers: List<AndroidPeer>, devices: List<AudioDevice>,
    androidPairHost: String, androidPairCode: String, desktopHost: String, desktopPort: String, desktopStatus: String, desktopSnapshot: DesktopSnapshot,
    selectedPeer: AndroidPeer?, selectedEndpoint: AndroidEndpoint?, matrixDirection: String, selectedDesktopSource: DesktopDevice?, selectedDesktopSink: DesktopDevice?,
    onEnglish: (Boolean) -> Unit, onDarkMode: (Boolean) -> Unit, onAutoStart: (Boolean) -> Unit, onDesktopControls: (Boolean) -> Unit,
    onStartNode: () -> Unit, onNewCode: () -> Unit, onPairHost: (String) -> Unit, onPairCode: (String) -> Unit, onPair: () -> Unit,
    onRefreshPeers: () -> Unit, onRefreshDevices: () -> Unit, onDesktopHost: (String) -> Unit, onDesktopPort: (String) -> Unit, onConnectDesktop: () -> Unit,
    onDesktopSource: (DesktopDevice) -> Unit, onDesktopSink: (DesktopDevice) -> Unit, onCreateDesktopMatrix: () -> Unit,
    onPeer: (AndroidPeer) -> Unit, onDirection: (String) -> Unit, onEndpoint: (AndroidEndpoint) -> Unit, onCreateAndroidMatrix: () -> Unit,
) {
    LazyColumn(
        modifier = Modifier.fillMaxSize().padding(padding), contentPadding = PaddingValues(start = 16.dp, top = 12.dp, end = 16.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        item { SettingsSection(t(english, "外观", "Appearance"), Icons.Outlined.LightMode) {
            SettingToggle(Icons.Outlined.Language, t(english, "语言", "Language"), if (english) "English" else "中文", checked = english, onCheckedChange = onEnglish)
            HorizontalDivider()
            SettingToggle(if (darkMode) Icons.Outlined.DarkMode else Icons.Outlined.LightMode, t(english, "深色模式", "Dark mode"), t(english, "手动切换", "Manual override"), darkMode, onDarkMode)
        } }
        item { SettingsSection(t(english, "节点与功能", "Node & features"), Icons.Outlined.Router) {
            Text("${node.localIpv4().joinToString().ifBlank { t(english, "未连接局域网", "No LAN connection") }}:8791", style = MaterialTheme.typography.titleSmall)
            Text(nodeStatus, style = MaterialTheme.typography.bodySmall)
            Button(onClick = onStartNode, modifier = Modifier.fillMaxWidth()) { Text(t(english, "启动常驻节点服务", "Start persistent node service")) }
            HorizontalDivider()
            SettingToggle(Icons.Outlined.Router, t(english, "自动启动节点", "Auto-start node"), t(english, "打开应用时启动", "Start when app opens"), autoStartNode, onAutoStart)
            HorizontalDivider()
            SettingToggle(Icons.Outlined.Devices, t(english, "桌面控制", "Desktop controls"), t(english, "显示桌面矩阵控制", "Show desktop matrix controls"), desktopControls, onDesktopControls)
        } }
        item { SettingsSection(t(english, "配对", "Pairing"), Icons.Outlined.Link) {
            Text("${t(english, "一次性代码", "One-time code")}  $pairCode", style = MaterialTheme.typography.titleMedium)
            OutlinedButton(onClick = onNewCode) { Icon(Icons.Outlined.Refresh, null); Text(t(english, "生成新代码", "Generate new code"), Modifier.padding(start = 8.dp)) }
            OutlinedTextField(androidPairHost, onPairHost, label = { Text(t(english, "桌面 OAMR IP 地址", "Desktop OAMR IP address")) }, singleLine = true, modifier = Modifier.fillMaxWidth())
            OutlinedTextField(androidPairCode, onPairCode, label = { Text(t(english, "桌面一次性代码", "Desktop one-time code")) }, singleLine = true, modifier = Modifier.fillMaxWidth())
            Button(onClick = onPair, enabled = androidPairHost.isNotBlank() && androidPairCode.isNotBlank(), modifier = Modifier.fillMaxWidth()) { Text(t(english, "与桌面配对", "Pair with desktop")) }
        } }
        item { SettingsSection(t(english, "本机配对矩阵", "This node's paired matrix"), Icons.Outlined.Router) {
            OutlinedButton(onClick = onRefreshPeers, modifier = Modifier.fillMaxWidth()) { Text(t(english, "刷新已配对设备", "Refresh paired devices")) }
            if (peers.isEmpty()) Text(t(english, "尚无设备。先完成配对。", "No peers yet. Pair a device first."))
            peers.forEach { peer -> ChoiceButton(peer.alias, selectedPeer?.nodeId == peer.nodeId) { onPeer(peer) } }
            selectedPeer?.let { peer ->
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FilterChip(selected = matrixDirection == "send", onClick = { onDirection("send") }, label = { Text(t(english, "手机 → 对方", "Phone → peer")) })
                    FilterChip(selected = matrixDirection == "receive", onClick = { onDirection("receive") }, label = { Text(t(english, "对方 → 手机", "Peer → phone")) })
                }
                val required = if (matrixDirection == "send") 'K' else 'S'
                peer.endpoints.filter { it.direction == required }.forEach { endpoint -> ChoiceButton(endpoint.name, selectedEndpoint?.id == endpoint.id) { onEndpoint(endpoint) } }
                if (peer.endpoints.isEmpty()) Text(t(english, "端点目录尚未同步，请重新配对此设备。", "Endpoint catalog is unavailable; pair this device again."), style = MaterialTheme.typography.bodySmall)
                Button(onClick = onCreateAndroidMatrix, enabled = selectedEndpoint != null, modifier = Modifier.fillMaxWidth()) { Text(t(english, "创建配对矩阵", "Create paired matrix")) }
            }
        } }
        if (desktopControls) {
            item { SettingsSection(t(english, "桌面 OAMR", "Desktop OAMR"), Icons.Outlined.Devices) {
                OutlinedTextField(desktopHost, onDesktopHost, label = { Text(t(english, "桌面 IP 地址", "Desktop IP address")) }, singleLine = true, modifier = Modifier.fillMaxWidth())
                OutlinedTextField(desktopPort, onDesktopPort, label = { Text(t(english, "HTTP 端口", "HTTP port")) }, singleLine = true, modifier = Modifier.fillMaxWidth())
                Text(desktopStatus, style = MaterialTheme.typography.bodySmall)
                Button(onClick = onConnectDesktop, modifier = Modifier.fillMaxWidth()) { Text(t(english, "连接并刷新", "Connect and refresh")) }
                if (desktopSnapshot.sources.isNotEmpty() || desktopSnapshot.sinks.isNotEmpty()) {
                    Text(t(english, "创建桌面本地矩阵", "Create desktop local matrix"), style = MaterialTheme.typography.titleSmall)
                    desktopSnapshot.sources.forEach { ChoiceButton(it.name, selectedDesktopSource?.id == it.id) { onDesktopSource(it) } }
                    desktopSnapshot.sinks.forEach { ChoiceButton(it.name, selectedDesktopSink?.id == it.id) { onDesktopSink(it) } }
                    Button(onClick = onCreateDesktopMatrix, enabled = selectedDesktopSource != null && selectedDesktopSink != null, modifier = Modifier.fillMaxWidth()) { Text(t(english, "创建桌面路线", "Create desktop route")) }
                }
            } }
        }
        item { SettingsSection(t(english, "设备预览", "Device preview"), Icons.Outlined.Devices) {
            OutlinedButton(onClick = onRefreshDevices, modifier = Modifier.fillMaxWidth()) { Icon(Icons.Outlined.Refresh, null); Text(t(english, "刷新音频设备", "Refresh audio devices"), Modifier.padding(start = 8.dp)) }
            devices.forEach { EndpointRow(it, english) }
        } }
    }
}

@Composable private fun StatusCard(icon: ImageVector, title: String, primary: String, secondary: String, running: Boolean) {
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.primaryContainer), shape = MaterialTheme.shapes.extraLarge) {
        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) { Icon(icon, null); Text(title, Modifier.padding(start = 10.dp), style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold); AssistChip(onClick = {}, label = { Text(if (running) "ONLINE" else "IDLE") }, Modifier.padding(start = 8.dp)) }
            Text(primary, style = MaterialTheme.typography.headlineSmall, maxLines = 2, overflow = TextOverflow.Ellipsis)
            Text(secondary, style = MaterialTheme.typography.bodyMedium)
        }
    }
}

@Composable private fun MetricCard(modifier: Modifier, icon: ImageVector, title: String, value: String, subtitle: String) {
    Card(modifier, shape = MaterialTheme.shapes.extraLarge) { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(5.dp)) { Icon(icon, null); Text(value, style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.Bold); Text(title, style = MaterialTheme.typography.titleSmall); Text(subtitle, style = MaterialTheme.typography.labelSmall, maxLines = 2, overflow = TextOverflow.Ellipsis) } }
}

@Composable private fun SectionTitle(title: String, icon: ImageVector, onRefresh: (() -> Unit)? = null) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) { Icon(icon, null); Text(title, Modifier.padding(start = 8.dp).weight(1f), style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold); if (onRefresh != null) IconButton(onClick = onRefresh) { Icon(Icons.Outlined.Refresh, "Refresh") } }
}

@Composable private fun StatusLine(icon: ImageVector, title: String, detail: String) { Row(verticalAlignment = Alignment.CenterVertically) { Icon(icon, null); Column(Modifier.padding(start = 12.dp).weight(1f)) { Text(title, style = MaterialTheme.typography.titleSmall); Text(detail, style = MaterialTheme.typography.bodySmall, maxLines = 2, overflow = TextOverflow.Ellipsis) } } }

@Composable private fun EndpointRow(device: AudioDevice, english: Boolean) { Card(shape = MaterialTheme.shapes.large) { Row(Modifier.fillMaxWidth().padding(14.dp), verticalAlignment = Alignment.CenterVertically) { Icon(if (device.direction == "输入") Icons.Outlined.Mic else Icons.Outlined.Speaker, null); Column(Modifier.padding(start = 12.dp).weight(1f)) { Text(device.name, style = MaterialTheme.typography.titleSmall); Text("${if (device.direction == "输入") t(english, "输入", "Input") else t(english, "输出", "Output")} · ${device.sampleRates}", style = MaterialTheme.typography.bodySmall, maxLines = 1, overflow = TextOverflow.Ellipsis) }; AssistChip(onClick = {}, label = { Text(t(english, "可用", "Ready")) }) } } }

@Composable private fun RouteRow(route: DesktopRoute, english: Boolean) { Card(shape = MaterialTheme.shapes.large) { Row(Modifier.fillMaxWidth().padding(14.dp), verticalAlignment = Alignment.CenterVertically) { Icon(Icons.Outlined.Link, null); Column(Modifier.padding(start = 12.dp).weight(1f)) { Text(route.label, style = MaterialTheme.typography.titleSmall); Text(route.profile, style = MaterialTheme.typography.bodySmall) }; AssistChip(onClick = {}, label = { Text(if (route.enabled) t(english, "运行", "Live") else t(english, "暂停", "Paused")) }) } } }

@Composable private fun SettingsSection(title: String, icon: ImageVector, content: @Composable () -> Unit) { Card(shape = MaterialTheme.shapes.extraLarge) { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) { Row(verticalAlignment = Alignment.CenterVertically) { Icon(icon, null); Text(title, Modifier.padding(start = 10.dp), style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold) }; content() } } }

@Composable private fun SettingToggle(icon: ImageVector, title: String, subtitle: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit) { Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) { Icon(icon, null); Column(Modifier.padding(start = 12.dp).weight(1f)) { Text(title, style = MaterialTheme.typography.titleSmall); Text(subtitle, style = MaterialTheme.typography.bodySmall) }; Switch(checked, onCheckedChange) } }

@Composable private fun ChoiceButton(label: String, selected: Boolean, onClick: () -> Unit) { OutlinedButton(onClick, Modifier.fillMaxWidth()) { Text(if (selected) "✓  $label" else label, maxLines = 1, overflow = TextOverflow.Ellipsis) } }

private fun t(english: Boolean, chinese: String, englishText: String): String = if (english) englishText else chinese
