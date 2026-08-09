package com.gem.oamr

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import com.gem.oamr.audio.AudioDevice
import com.gem.oamr.audio.OboeAudioEngine
import com.gem.oamr.control.DesktopController
import com.gem.oamr.control.DesktopDevice
import com.gem.oamr.control.DesktopRoute
import com.gem.oamr.control.DesktopSnapshot
import com.gem.oamr.control.PairedEndpoint
import com.gem.oamr.control.PairedPeer
import com.gem.oamr.peer.AndroidPeerService
import com.gem.oamr.ui.theme.OAMRTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent { OAMRTheme { OAMRApp(this) } }
    }
}

@Composable
fun OAMRApp(activity: ComponentActivity) {
    val engine = remember { OboeAudioEngine(activity) }
    val androidNode = remember { AndroidPeerService.get(activity) }
    var status by remember { mutableStateOf("音频引擎尚未启动") }
    var devices by remember { mutableStateOf(engine.listDevices()) }
    var desktopHost by remember { mutableStateOf("192.168.1.") }
    var desktopPort by remember { mutableStateOf("8790") }
    var desktopStatus by remember { mutableStateOf("尚未连接桌面 OAMR") }
    var desktopSnapshot by remember { mutableStateOf(DesktopSnapshot()) }
    var selectedSource by remember { mutableStateOf<DesktopDevice?>(null) }
    var selectedSink by remember { mutableStateOf<DesktopDevice?>(null) }
    var senderHost by remember { mutableStateOf("") }
    var senderPort by remember { mutableStateOf("5004") }
    var receiverPort by remember { mutableStateOf("5004") }
    var androidNodeStatus by remember { mutableStateOf("Android node is stopped") }
    var androidPairHost by remember { mutableStateOf("") }
    var androidPairCode by remember { mutableStateOf("") }
    var androidCode by remember { mutableStateOf(androidNode.currentPairCode()) }
    var localAlias by remember { mutableStateOf("") }
    var peerHost by remember { mutableStateOf("") }
    var peerCode by remember { mutableStateOf("") }
    var selectedPeer by remember { mutableStateOf<PairedPeer?>(null) }
    var pairedDirection by remember { mutableStateOf("send") }
    var selectedRemoteEndpoint by remember { mutableStateOf<PairedEndpoint?>(null) }
    val microphonePermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        status = if (granted) engine.startInput() else "未授予麦克风权限"
    }
    fun loadDesktop() {
        Thread {
            try {
                val snapshot = DesktopController(desktopHost, desktopPort).snapshot()
                activity.runOnUiThread {
                    desktopSnapshot = snapshot
                    localAlias = snapshot.localAlias
                    if (selectedSource?.id !in snapshot.sources.map { it.id }) selectedSource = null
                    if (selectedSink?.id !in snapshot.sinks.map { it.id }) selectedSink = null
                    selectedPeer = snapshot.peers.find { it.nodeId == selectedPeer?.nodeId }
                    if (selectedRemoteEndpoint?.id !in selectedPeer?.endpoints.orEmpty().map { it.id }) selectedRemoteEndpoint = null
                    desktopStatus = "已连接：${snapshot.sourceCount} 个来源、${snapshot.sinkCount} 个输出"
                }
            } catch (error: Exception) {
                activity.runOnUiThread { desktopStatus = "连接失败：${error.message}" }
            }
        }.start()
    }
    fun runDesktopOperation(operation: DesktopController.() -> Unit) {
        Thread {
            try {
                DesktopController(desktopHost, desktopPort).operation()
                activity.runOnUiThread { loadDesktop() }
            } catch (error: Exception) {
                activity.runOnUiThread { desktopStatus = "操作失败：${error.message}" }
            }
        }.start()
    }

    Scaffold(modifier = Modifier.fillMaxSize()) { padding ->
        LazyColumn(
            modifier = Modifier.fillMaxSize().padding(padding).padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            item {
                Text("Open Audio Matrix Router", style = MaterialTheme.typography.headlineSmall)
                Text("Android 音频端点 · Oboe", style = MaterialTheme.typography.bodyMedium)
            }
            item {
                Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.tertiaryContainer)) {
                    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("Android OAMR 节点", style = MaterialTheme.typography.titleMedium)
                        Text("地址：${androidNode.localIpv4().joinToString().ifBlank { "未连接局域网" }}:8791")
                        Text("一次性代码：$androidCode")
                        Text(androidNodeStatus, style = MaterialTheme.typography.bodySmall)
                        Button(onClick = { androidNodeStatus = androidNode.start() }) { Text("启动配对服务") }
                        OutlinedButton(onClick = { androidCode = androidNode.newPairCode() }) { Text("生成新代码") }
                        OutlinedTextField(androidPairHost, { androidPairHost = it }, label = { Text("桌面 OAMR IP 地址") }, singleLine = true)
                        OutlinedTextField(androidPairCode, { androidPairCode = it }, label = { Text("桌面一次性代码") }, singleLine = true)
                        Button(onClick = {
                            Thread {
                                val message = androidNode.pairDesktop(androidPairHost, androidPairCode)
                                activity.runOnUiThread { androidNodeStatus = message }
                            }.start()
                        }, enabled = androidPairHost.isNotBlank() && androidPairCode.isNotBlank()) { Text("让 Android 与桌面配对") }
                    }
                }
            }
            item {
                Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.primaryContainer)) {
                    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("桌面 OAMR 控制器", style = MaterialTheme.typography.titleMedium)
                        OutlinedTextField(desktopHost, { desktopHost = it }, label = { Text("桌面 IP 地址") }, singleLine = true)
                        OutlinedTextField(desktopPort, { desktopPort = it }, label = { Text("HTTP 端口") }, singleLine = true)
                        Text(desktopStatus, style = MaterialTheme.typography.bodySmall)
                        Button(onClick = { loadDesktop() }) { Text("连接并刷新") }
                        if (desktopSnapshot.routes.isNotEmpty()) Button(onClick = { runDesktopOperation { stopAll() } }) { Text("停止桌面全部路线") }
                    }
                }
            }
            if (desktopSnapshot.sources.isNotEmpty() || desktopSnapshot.sinks.isNotEmpty()) {
                item {
                    Card {
                        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("设备配对", style = MaterialTheme.typography.titleMedium)
                            OutlinedTextField(localAlias, { localAlias = it }, label = { Text("本机别名") }, singleLine = true)
                            Text("一次性配对代码：${desktopSnapshot.pairCode.ifBlank { "点击生成" }}")
                            Button(onClick = { runDesktopOperation { regeneratePairCode() } }) { Text("生成新配对代码") }
                            Button(onClick = { runDesktopOperation { saveExposure(localAlias, desktopSnapshot.sources, desktopSnapshot.sinks) } }) { Text("公开全部本机端点") }
                            OutlinedTextField(peerHost, { peerHost = it }, label = { Text("对方 OAMR 的 IP 地址") }, singleLine = true)
                            OutlinedTextField(peerCode, { peerCode = it }, label = { Text("对方的一次性配对代码") }, singleLine = true)
                            Button(
                                onClick = { runDesktopOperation { pair(peerHost, peerCode, localAlias) } },
                                enabled = peerHost.isNotBlank() && peerCode.isNotBlank() && localAlias.isNotBlank()
                            ) { Text("开始配对") }
                        }
                    }
                }
                if (desktopSnapshot.peers.isNotEmpty()) {
                    item { Text("已配对设备", style = MaterialTheme.typography.titleMedium) }
                    items(desktopSnapshot.peers, key = { it.nodeId }) { peer ->
                        PairedPeerRow(peer, selectedPeer?.nodeId == peer.nodeId,
                            onSelect = { selectedPeer = peer; selectedRemoteEndpoint = null },
                            onDelete = { runDesktopOperation { deletePeer(peer) } })
                    }
                }
                item { Text("创建桌面路线", style = MaterialTheme.typography.titleMedium) }
                item {
                    Card {
                        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("1. 选择来源")
                            desktopSnapshot.sources.forEach { device ->
                                OutlinedButton(onClick = { selectedSource = device }, modifier = Modifier.fillMaxWidth()) {
                                    Text(if (selectedSource?.id == device.id) "✓ ${device.name}" else device.name)
                                }
                            }
                            Text("2. 选择输出")
                            desktopSnapshot.sinks.forEach { device ->
                                OutlinedButton(onClick = { selectedSink = device }, modifier = Modifier.fillMaxWidth()) {
                                    Text(if (selectedSink?.id == device.id) "✓ ${device.name}" else device.name)
                                }
                            }
                            Button(
                                onClick = { runDesktopOperation { createMatrix(selectedSource!!, selectedSink!!) } },
                                enabled = selectedSource != null && selectedSink != null
                            ) { Text("创建本机音频路线") }
                        }
                    }
                }
                item {
                    Card {
                        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("发送 RTP/Opus 到局域网")
                            OutlinedTextField(senderHost, { senderHost = it }, label = { Text("接收端 IP 地址") }, singleLine = true)
                            OutlinedTextField(senderPort, { senderPort = it }, label = { Text("UDP 端口") }, singleLine = true)
                            Button(
                                onClick = { runDesktopOperation { createSender(selectedSource!!, senderHost, senderPort) } },
                                enabled = selectedSource != null && senderHost.isNotBlank()
                            ) { Text("创建发送路线") }
                        }
                    }
                }
                item {
                    Card {
                        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("接收 RTP/Opus")
                            OutlinedTextField(receiverPort, { receiverPort = it }, label = { Text("UDP 端口") }, singleLine = true)
                            Button(
                                onClick = { runDesktopOperation { createReceiver(selectedSink!!, receiverPort) } },
                                enabled = selectedSink != null
                            ) { Text("创建接收路线") }
                        }
                    }
                }
                if (selectedPeer != null) {
                    item {
                        Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.tertiaryContainer)) {
                            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                Text("配对矩阵 · ${selectedPeer!!.alias}", style = MaterialTheme.typography.titleMedium)
                                Text("选择流向")
                                OutlinedButton(onClick = { pairedDirection = "send"; selectedRemoteEndpoint = null }) {
                                    Text(if (pairedDirection == "send") "✓ 本机来源 → 对方输出" else "本机来源 → 对方输出")
                                }
                                OutlinedButton(onClick = { pairedDirection = "receive"; selectedRemoteEndpoint = null }) {
                                    Text(if (pairedDirection == "receive") "✓ 对方来源 → 本机输出" else "对方来源 → 本机输出")
                                }
                                Text("选择对方端点")
                                selectedPeer!!.endpoints.filter { it.direction == if (pairedDirection == "send") "sink" else "source" }.forEach { endpoint ->
                                    OutlinedButton(onClick = { selectedRemoteEndpoint = endpoint }, modifier = Modifier.fillMaxWidth()) {
                                        Text(if (selectedRemoteEndpoint?.id == endpoint.id) "✓ ${endpoint.name}" else endpoint.name)
                                    }
                                }
                                Button(
                                    onClick = {
                                        val local = if (pairedDirection == "send") selectedSource else selectedSink
                                        runDesktopOperation { createPairedRoute(selectedPeer!!, pairedDirection, local!!, selectedRemoteEndpoint!!) }
                                    },
                                    enabled = selectedRemoteEndpoint != null && if (pairedDirection == "send") selectedSource != null else selectedSink != null
                                ) { Text("创建配对矩阵路线") }
                            }
                        }
                    }
                }
            }
            if (desktopSnapshot.routes.isNotEmpty()) {
                item { Text("桌面路线", style = MaterialTheme.typography.titleMedium) }
                items(desktopSnapshot.routes, key = { it.id }) { route ->
                    DesktopRouteRow(route,
                        onToggle = { runDesktopOperation { toggle(route) } },
                        onDelete = { runDesktopOperation { delete(route) } })
                }
            }
            item {
                Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer)) {
                    Column(Modifier.padding(16.dp)) {
                        Text("Oboe 输入引擎", style = MaterialTheme.typography.titleMedium)
                        Spacer(Modifier.height(6.dp))
                        Text(status)
                        Spacer(Modifier.height(12.dp))
                        Button(onClick = {
                            if (ContextCompat.checkSelfPermission(activity, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED)
                                status = engine.startInput()
                            else microphonePermission.launch(Manifest.permission.RECORD_AUDIO)
                        }) { Text("启动麦克风") }
                    }
                }
            }
            item { Text("可用音频设备", style = MaterialTheme.typography.titleMedium) }
            items(devices, key = { it.id }) { device -> DeviceRow(device) }
            item {
                Button(onClick = { devices = engine.listDevices() }, modifier = Modifier.fillMaxWidth()) {
                    Text("刷新设备")
                }
            }
        }
    }
}

@Composable
private fun PairedPeerRow(peer: PairedPeer, selected: Boolean, onSelect: () -> Unit, onDelete: () -> Unit) {
    Card {
        Column(Modifier.fillMaxWidth().padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(if (selected) "✓ ${peer.alias}" else peer.alias, style = MaterialTheme.typography.titleSmall)
            Text("${peer.host}:${peer.port} · ${peer.endpoints.size} 个端点", style = MaterialTheme.typography.bodySmall)
            OutlinedButton(onClick = onSelect) { Text("选择用于矩阵") }
            OutlinedButton(onClick = onDelete) { Text("删除配对") }
        }
    }
}

@Composable
private fun DesktopRouteRow(route: DesktopRoute, onToggle: () -> Unit, onDelete: () -> Unit) {
    Card {
        Column(Modifier.fillMaxWidth().padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(route.label, style = MaterialTheme.typography.titleSmall)
            Text(if (route.enabled) "运行中 · ${route.profile}" else "已暂停 · ${route.profile}", style = MaterialTheme.typography.bodySmall)
            Button(onClick = onToggle) { Text(if (route.enabled) "暂停" else "恢复") }
            Button(onClick = onDelete) { Text("删除") }
        }
    }
}

@Composable
private fun DeviceRow(device: AudioDevice) {
    Card {
        Column(Modifier.fillMaxWidth().padding(14.dp)) {
            Text(device.name, style = MaterialTheme.typography.titleSmall)
            Text("${device.direction} · ${device.type} · ${device.sampleRates}", style = MaterialTheme.typography.bodySmall)
        }
    }
}
