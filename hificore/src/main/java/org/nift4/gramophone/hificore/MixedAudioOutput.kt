package org.nift4.gramophone.hificore

import android.media.AudioDeviceInfo
import androidx.annotation.GuardedBy
import androidx.media3.common.PlaybackParameters
import androidx.media3.common.util.Util
import androidx.media3.exoplayer.audio.AudioOutput
import java.nio.ByteBuffer
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit

class MixedAudioOutput(
    private val mixer: SoftMixedStreaming, private val audioSessionId: Int,
    private val javaBufferSizeFrames: Int, audioFrameSize: Int,
) : Buffer(true, javaBufferSizeFrames, audioFrameSize), AudioOutput {

    companion object {
        private val releaseExecutorLock: Any = Any()
    
        // Intentional statically shared mutable state
        @GuardedBy("releaseExecutorLock")
        private var releaseExecutor: ScheduledExecutorService? = null

        @GuardedBy("releaseExecutorLock")
        private var pendingReleaseCount: Int = 0

        /** Returns whether there are any pending asynchronous releases.  */
        internal fun hasPendingReleases(): Boolean {
            synchronized(releaseExecutorLock) {
                return pendingReleaseCount > 0
            }
        }

        private const val AUDIO_TRACK_VOLUME_RAMP_TIME_MS = 20
    }
    private val listeners = mutableListOf<AudioOutput.Listener>()
    private var sentAdvancing = false
    private var lastUnderrunCount = 0u
    private var lastTimestampRawPositionFrames = 0uL
    private var expectTimestampFramePositionReset = false
    private var accumulatedRawTimestampFramePosition = 0uL
    private val tmp = LongArray(2)

    init {
        setStopped(true)
        mixer.addBuffer(this)
    }

    internal fun periodicCallback() {
        synchronized(this) {
            if (released)
                return
            val underrunCount = getUnderrunCount()
            if (underrunCount != lastUnderrunCount) {
                listeners.forEach { it.onUnderrun() }
                lastUnderrunCount = underrunCount
            }
            if (!sentAdvancing) {
                getWriteCounter(tmp)
                if (tmp[1] != 0L) {
                    // TODO: we could convert monotonic nanotime to epoch if we cared
                    val start = System.currentTimeMillis()
                    listeners.forEach { it.onPositionAdvancing(start) }
                    sentAdvancing = true
                }
            }
        }
    }

    override fun write(
        buffer: ByteBuffer,
        encodedAccessUnitCount: Int,
        presentationTimeUs: Long
    ): Boolean {
        return write(buffer)
    }

    override fun play() {
        setStopped(false)
        val fadeInFrames = sampleRate * AUDIO_TRACK_VOLUME_RAMP_TIME_MS / 1000
        nativeSetFramesUntilPaused(getPtr(), -fadeInFrames - 1)
        // TODO: send advancing after pause and then play, maybe by doing removeBuffer after pause
        //  was processed or by making SoftMixer reset timestamps after pause is processed, or idk
    }

    override fun pause() {
        val fadeInFrames = sampleRate * AUDIO_TRACK_VOLUME_RAMP_TIME_MS / 1000
        nativeSetFramesUntilPaused(getPtr(), fadeInFrames)
    }

    override fun flush() {
        mixer.removeBuffer(this)
        super.flush()
        resetWriteCounter()
        expectTimestampFramePositionReset = true
        mixer.addBuffer(this)
    }

    override fun stop() {
        setStopped(true)
    }

    override fun release() {
        // awaitPause can take some time, so we call it on a background thread. The background
        // thread is shared statically to avoid creating many threads when multiple players are
        // released at the same time.
        val audioTrackThreadHandler = Util.createHandlerForCurrentLooper()
        synchronized(releaseExecutorLock) {
            if (releaseExecutor == null) {
                releaseExecutor = Util.newSingleThreadScheduledExecutor(
                    "ExoPlayer:MixedAudioOutputReleaseThread")
            }
            pendingReleaseCount++
            releaseExecutor!!.schedule(
                {
                    try {
                        nativeAwaitPause(getPtr())
                        synchronized(this) {
                            mixer.removeBuffer(this)
                            super.release()
                        }
                    } finally {
                        if (audioTrackThreadHandler.looper.thread.isAlive) {
                            audioTrackThreadHandler.post {
                                listeners.forEach { it.onReleased() }
                            }
                        }
                        synchronized(releaseExecutorLock) {
                            pendingReleaseCount--
                            if (pendingReleaseCount == 0) {
                                releaseExecutor!!.shutdown()
                                releaseExecutor = null
                            }
                        }
                    }
                },
                // We need to schedule the flush and release with a delay to ensure the audio system
                // can completely ramp down the audio output after the preceding pause.
                AUDIO_TRACK_VOLUME_RAMP_TIME_MS.toLong(),
                TimeUnit.MILLISECONDS
            )
        }
    }

    override fun setVolume(volume: Float) {
        nativeSetGain(getPtr(), volume)
    }

    override fun isOffloadedPlayback(): Boolean {
        return false
    }

    override fun getAudioSessionId(): Int {
        return audioSessionId
    }

    override fun getSampleRate(): Int {
        return mixer.getSampleRate()
    }

    override fun getBufferSizeInFrames(): Long {
        return javaBufferSizeFrames.toLong()
    }

    override fun getPositionUs(): Long {
        getWriteCounter(tmp)
        val rawPositionFrames = tmp[0].toULong()
        val nanoTime = tmp[1].toULong()
        if (nanoTime > 0uL) {
            if (lastTimestampRawPositionFrames > rawPositionFrames) {
                if (expectTimestampFramePositionReset) {
                    // ExoPlayer expects getPositionUs() to _not_ reset on a flush, but we reset it,
                    // hence we compensate for that here.
                    accumulatedRawTimestampFramePosition += lastTimestampRawPositionFrames
                    expectTimestampFramePositionReset = false
                } else {
                    // TODO wait, what?
                }
            }
            lastTimestampRawPositionFrames = rawPositionFrames
            val frameCounter = rawPositionFrames + accumulatedRawTimestampFramePosition
            val timestampPositionUs = Util.sampleCountToDurationUs(frameCounter.toLong(),
                sampleRate)
            val elapsedSinceTimestampUs = (System.nanoTime() - nanoTime.toLong()) / 1000
            return timestampPositionUs + elapsedSinceTimestampUs
        } else {
            return Util.sampleCountToDurationUs(accumulatedRawTimestampFramePosition.toLong(),
                sampleRate)
        }
    }

    override fun getPlaybackParameters(): PlaybackParameters {
        return PlaybackParameters.DEFAULT
    }

    override fun isStalled(): Boolean {
        //TODO("Not yet implemented")
        return false
    }

    override fun addListener(listener: AudioOutput.Listener) {
        listeners.add(listener)
    }

    override fun removeListener(listener: AudioOutput.Listener) {
        listeners.remove(listener)
    }

    override fun setPlaybackParameters(playbackParams: PlaybackParameters) {
        // no-op. ExoPlayer will call getPlaybackParameters() and notice this isn't working
    }

    override fun setOffloadDelayPadding(delayInFrames: Int, paddingInFrames: Int) {
        throw UnsupportedOperationException()
    }

    override fun setOffloadEndOfStream() {
        throw UnsupportedOperationException()
    }

    override fun attachAuxEffect(effectId: Int) {
        throw UnsupportedOperationException()
    }

    override fun setAuxEffectSendLevel(level: Float) {
        throw UnsupportedOperationException()
    }

    override fun setPreferredDevice(preferredDevice: AudioDeviceInfo?) {
        //TODO("Not yet implemented")
    }

    private external fun nativeSetGain(ptr: Long, gain: Float)
    private external fun nativeSetFramesUntilPaused(ptr: Long, frames: Int)
    private external fun nativeAwaitPause(ptr: Long)
}