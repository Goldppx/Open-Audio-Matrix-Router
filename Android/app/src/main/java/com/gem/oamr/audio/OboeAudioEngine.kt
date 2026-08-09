package com.gem.oamr.audio

import android.content.Context
import android.media.AudioManager

data class AudioDevice(
    val id: Int,
    val name: String,
    val direction: String,
    val type: String,
    val sampleRates: String,
)

/** The first replaceable Android audio backend, implemented with Oboe. */
class OboeAudioEngine(context: Context) {
    private val audioManager = context.getSystemService(AudioManager::class.java)

    init { System.loadLibrary("oamr_audio") }

    fun listDevices(): List<AudioDevice> = audioManager.getDevices(AudioManager.GET_DEVICES_ALL)
        .map { device ->
            AudioDevice(
                id = device.id,
                name = device.productName?.toString().orEmpty().ifBlank { "未命名设备" },
                direction = if (device.isSource) "输入" else "输出",
                type = device.type.toString(),
                sampleRates = device.sampleRates.joinToString().ifBlank { "系统默认采样率" },
            )
        }
        .sortedBy { it.name }

    fun startInput(): String = nativeStartInput()
    external fun nativeStartInput(): String
}
