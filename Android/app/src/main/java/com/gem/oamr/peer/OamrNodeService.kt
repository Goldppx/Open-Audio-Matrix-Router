package com.gem.oamr.peer

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.IBinder
import androidx.core.app.NotificationCompat
import com.gem.oamr.R
import com.gem.oamr.audio.OboeAudioEngine

/** Keeps this handset reachable as an OAMR node after its UI is backgrounded. */
class OamrNodeService : Service() {
    override fun onCreate() {
        super.onCreate()
        val channel = NotificationChannel(CHANNEL, "OAMR node", NotificationManager.IMPORTANCE_LOW)
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        startForeground(NOTIFICATION_ID, NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_account_box)
            .setContentTitle("OAMR Android 节点运行中")
            .setContentText("正在等待配对与音频矩阵路由")
            .setOngoing(true)
            .build())
        val audio = OboeAudioEngine(this)
        val node = AndroidPeerService.get(this)
        node.routeHandler = { direction, endpoint, host, port ->
            when {
                direction == "send" && endpoint == "android-oboe-input" -> audio.startRtpSender(host, port)
                direction == "receive" && endpoint == "android-oboe-output" -> audio.startRtpReceiver(port)
                else -> "error=unsupported-android-endpoint"
            }
        }
        node.start()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_STICKY
    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        const val CHANNEL = "oamr-node"
        const val NOTIFICATION_ID = 8791
    }
}
