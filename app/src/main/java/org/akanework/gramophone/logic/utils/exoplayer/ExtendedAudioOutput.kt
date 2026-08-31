package org.akanework.gramophone.logic.utils.exoplayer

import android.media.AudioDeviceInfo
import android.media.AudioRouting
import android.media.AudioTrack
import android.os.Build
import android.os.Handler
import androidx.media3.common.util.Log
import androidx.media3.exoplayer.audio.AudioOutput
import androidx.media3.exoplayer.audio.AudioOutputProvider
import androidx.media3.exoplayer.audio.AudioTrackAudioOutput
import androidx.media3.exoplayer.audio.ForwardingAudioOutput
import androidx.media3.exoplayer.audio.ForwardingAudioOutputProvider
import org.nift4.gramophone.hificore.AudioSystemHiddenApi
import org.nift4.gramophone.hificore.AudioTrackHiddenApi
import org.nift4.gramophone.hificore.NativeTrack
import java.util.IdentityHashMap

class ExtendedAudioOutputProvider(sink: AudioOutputProvider) : ForwardingAudioOutputProvider(sink) {
    override fun getAudioOutput(config: AudioOutputProvider.OutputConfig): ExtendedAudioOutput {
        return when (val ao = super.getAudioOutput(config)) {
            is AudioTrackAudioOutput -> AudioTrackExtendedAudioOutput(ao)
            is NativeTrackAudioOutput -> NativeTrackExtendedAudioOutput(ao)
            else -> throw IllegalStateException("Unsupported ao ${ao.javaClass.name}")
        }
    }
}

interface ExtendedAudioOutput : AudioOutput {
    val routedDevice: AudioDeviceInfo?
    val isInitialized: Boolean

    fun addOnRoutingChangedListener(listener: RoutingChangedListener, handler: Handler)
    fun removeOnRoutingChangedListener(listener: RoutingChangedListener)
    fun getOutputPort(): Int?
    fun getLatency(): Int?
    fun getHalSampleRate(): UInt?
    fun getGrantedFlags(): Int?
    fun dump(): String?
    fun getHalFormat(): UInt?
    fun getHalChannelCount(): Int?
    fun getPtr(): Long

    fun interface RoutingChangedListener {
        fun onRoutingChanged(router: ExtendedAudioOutput)
    }
}

class NativeTrackExtendedAudioOutput(private val ao: NativeTrackAudioOutput) : ForwardingAudioOutput(ao), ExtendedAudioOutput {
    companion object {
        private const val TAG = "NTExtendedAO"
    }
    private val listeners = IdentityHashMap<ExtendedAudioOutput.RoutingChangedListener, NativeTrack.OnRoutingChangedListener>()
    override val routedDevice: AudioDeviceInfo?
        get() = ao.nativeTrack.getRoutedDevice()
    override val isInitialized: Boolean
        get() = ao.nativeTrack.myState != NativeTrack.State.RELEASED

    override fun addOnRoutingChangedListener(
        listener: ExtendedAudioOutput.RoutingChangedListener,
        handler: Handler
    ) {
        val nativeListener = synchronized(listeners) {
            listeners.getOrPut(listener) {
                NativeTrack.OnRoutingChangedListener { listener.onRoutingChanged(this) }
            }
        }
        ao.nativeTrack.addOnRoutingChangedListener(nativeListener, handler)
    }

    override fun removeOnRoutingChangedListener(listener: ExtendedAudioOutput.RoutingChangedListener) {
        val nativeListener = synchronized(listeners) { listeners.remove(listener) } ?: return
        ao.nativeTrack.removeOnRoutingChangedListener(nativeListener)
    }

    override fun getLatency(): Int? {
        return try {
            ao.nativeTrack.latency().toInt()
        } catch (t: Throwable) {
            Log.e(TAG, Log.getThrowableString(t)!!)
            null
        }
    }

    override fun getHalSampleRate(): UInt? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) return null
        return try {
            ao.nativeTrack.getHalSampleRate()
        } catch (t: Throwable) {
            Log.e(TAG, Log.getThrowableString(t)!!)
            null
        }
    }

    override fun getGrantedFlags(): Int? {
        return try {
            ao.nativeTrack.flags()
        } catch (t: Throwable) {
            Log.e(TAG, Log.getThrowableString(t)!!)
            null
        }
    }

    override fun dump(): String? {
        return try {
            ao.nativeTrack.dump()
        } catch (t: Throwable) {
            Log.e(TAG, Log.getThrowableString(t)!!)
            null
        }
    }

    override fun getHalFormat(): UInt? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            val output = getOutputPort() ?: return null
            return AudioSystemHiddenApi.getFormat(output)
        }
        return try {
            ao.nativeTrack.getHalFormat()
        } catch (t: Throwable) {
            Log.e(TAG, Log.getThrowableString(t)!!)
            null
        }
    }

    override fun getHalChannelCount(): Int? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) return null
        return try {
            ao.nativeTrack.getHalChannelCount()
        } catch (t: Throwable) {
            Log.e(TAG, Log.getThrowableString(t)!!)
            null
        }
    }

    override fun getPtr(): Long {
        return ao.nativeTrack.getAudioTrackPtr()
    }

    override fun getOutputPort(): Int? {
        return try {
            ao.nativeTrack.getOutput()
        } catch (t: Throwable) {
            Log.e(TAG, Log.getThrowableString(t)!!)
            null
        }
    }
}

class AudioTrackExtendedAudioOutput(private val ao: AudioTrackAudioOutput) : ForwardingAudioOutput(ao), ExtendedAudioOutput {
    companion object {
        private const val TAG = "ATExtendedAO"
    }
    private val listeners = IdentityHashMap<ExtendedAudioOutput.RoutingChangedListener, Any>()
    override val routedDevice: AudioDeviceInfo?
        get() = ao.audioTrack.routedDevice
    override val isInitialized: Boolean
        get() = ao.audioTrack.state != AudioTrack.STATE_UNINITIALIZED

    override fun addOnRoutingChangedListener(
        listener: ExtendedAudioOutput.RoutingChangedListener,
        handler: Handler
    ) {
        val nativeListener = synchronized(listeners) {
            listeners.getOrPut(listener) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                    AudioRouting.OnRoutingChangedListener { listener.onRoutingChanged(this) }
                } else {
                    @Suppress("deprecation")
                    AudioTrack.OnRoutingChangedListener { listener.onRoutingChanged(this) }
                }
            }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            ao.audioTrack.addOnRoutingChangedListener(nativeListener as AudioRouting.OnRoutingChangedListener, handler)
        } else {
            @Suppress("deprecation")
            ao.audioTrack.addOnRoutingChangedListener(nativeListener as AudioTrack.OnRoutingChangedListener, handler)
        }
    }

    override fun removeOnRoutingChangedListener(listener: ExtendedAudioOutput.RoutingChangedListener) {
        val nativeListener = synchronized(listeners) { listeners.remove(listener) } ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            ao.audioTrack.removeOnRoutingChangedListener(nativeListener as AudioRouting.OnRoutingChangedListener)
        } else {
            @Suppress("deprecation")
            ao.audioTrack.removeOnRoutingChangedListener(nativeListener as AudioTrack.OnRoutingChangedListener)
        }
    }

    override fun getLatency(): Int? {
        return try {
            AudioTrack::class.java.getMethod("getLatency").invoke(ao.audioTrack) as Int
        } catch (t: Throwable) {
            Log.e(TAG, Log.getThrowableString(t)!!)
            null
        }
    }

    override fun getHalSampleRate(): UInt? {
        return AudioTrackHiddenApi.getHalSampleRate(ao.audioTrack)
    }

    override fun getGrantedFlags(): Int? {
        return AudioTrackHiddenApi.getGrantedFlags(ao.audioTrack)
    }

    override fun dump(): String? {
        return AudioTrackHiddenApi.dump(ao.audioTrack)
    }

    override fun getHalFormat(): UInt? {
        return AudioTrackHiddenApi.getHalFormat(ao.audioTrack)
    }

    override fun getHalChannelCount(): Int? {
        return AudioTrackHiddenApi.getHalChannelCount(ao.audioTrack)
    }

    override fun getPtr(): Long {
        return AudioTrackHiddenApi.getAudioTrackPtr(ao.audioTrack)
    }

    override fun getOutputPort(): Int? {
        return AudioTrackHiddenApi.getOutput(ao.audioTrack)
    }
}