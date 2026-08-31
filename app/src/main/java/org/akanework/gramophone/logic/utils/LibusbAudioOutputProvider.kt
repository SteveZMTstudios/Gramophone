package org.akanework.gramophone.logic.utils

import android.content.Context
import android.media.AudioFormat
import androidx.media3.common.C
import androidx.media3.common.MimeTypes
import androidx.media3.exoplayer.audio.AudioOutput
import androidx.media3.exoplayer.audio.AudioOutputProvider
import org.akanework.gramophone.logic.gramophoneApplication
import org.nift4.gramophone.hificore.BufferedLibusbAudioOutput

class LibusbAudioOutputProvider(private val context: Context) : AudioOutputProvider {
    override fun getFormatSupport(formatConfig: AudioOutputProvider.FormatConfig): AudioOutputProvider.FormatSupport {
        return if (formatConfig.format.pcmEncoding == C.ENCODING_PCM_16BIT && formatConfig.format.sampleMimeType == MimeTypes.AUDIO_RAW &&
            formatConfig.format.sampleRate == 44100 && formatConfig.format.channelCount == 2)
            AudioOutputProvider.FormatSupport.Builder().setFormatSupportLevel(AudioOutputProvider.FORMAT_SUPPORTED_DIRECTLY).build()
        else AudioOutputProvider.FormatSupport.UNSUPPORTED
    }

    override fun getOutputConfig(formatConfig: AudioOutputProvider.FormatConfig): AudioOutputProvider.OutputConfig {
        if (formatConfig.format.pcmEncoding == C.ENCODING_PCM_16BIT && formatConfig.format.sampleMimeType == MimeTypes.AUDIO_RAW &&
            formatConfig.format.sampleRate == 44100 && formatConfig.format.channelCount == 2)
        return AudioOutputProvider.OutputConfig.Builder()
            .setAudioAttributes(formatConfig.audioAttributes)
            .setAudioSessionId(formatConfig.audioSessionId)
            .setEncoding(formatConfig.format.pcmEncoding)
            .setSampleRate(formatConfig.format.sampleRate)
            .setIsOffload(false)
            .setBufferSize(8 * 10 * 4 * 2 * 2)
            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
            .build()
        else throw AudioOutputProvider.ConfigurationException("bad fmt")
    }

    override fun getAudioOutput(config: AudioOutputProvider.OutputConfig): AudioOutput {
        context.gramophoneApplication.uacManager.interfaces?.let {
            return BufferedLibusbAudioOutput.new(context.gramophoneApplication.uacManager.openDevices.first().second, it.second)
        }
        throw IllegalStateException("pls grant usb dac perm")
    }

    override fun addListener(listener: AudioOutputProvider.Listener) {
        //TODO("Not yet implemented")
    }

    override fun removeListener(listener: AudioOutputProvider.Listener) {
        //TODO("Not yet implemented")
    }

    override fun release() {
        //TODO("Not yet implemented")
    }
}