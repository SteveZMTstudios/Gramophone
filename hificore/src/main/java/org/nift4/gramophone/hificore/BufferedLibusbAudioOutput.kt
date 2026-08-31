package org.nift4.gramophone.hificore

import android.media.AudioDeviceInfo
import androidx.media3.common.C
import androidx.media3.common.PlaybackParameters
import androidx.media3.common.util.Log
import androidx.media3.common.util.Util
import androidx.media3.exoplayer.audio.AudioOutput
import com.jwoolston.libusb.UsbConstants
import com.jwoolston.libusb.UsbDevice
import com.jwoolston.libusb.UsbEndpoint
import com.jwoolston.libusb.UsbInterface
import java.nio.ByteBuffer

class BufferedLibusbAudioOutput(
    device: UsbDevice, usbInterface: UsbInterface, handle: Long, ptr: Long, private val buf: Buffer
) : Streaming(device, usbInterface, handle, ptr, false), AudioOutput {
    companion object {
        private const val TAG = "BufferedLibusbAO"
        fun new(device: UsbDevice, usbInterface: UsbInterface) = createExplicitFeedback(device,
            usbInterface,
            usbInterface.endpointCount.let {
                for (i in 0..<it) {
                    val ep = usbInterface.getEndpoint(i)
                    if (ep.endpointNumber == 1 && ep.direction == UsbConstants.USB_DIR_OUT)
                        return@let ep
                }
                throw IllegalArgumentException("no stream ep?")
            }, usbInterface.endpointCount.let {
                for (i in 0..<it) {
                    val ep = usbInterface.getEndpoint(i)
                    if (ep.endpointNumber == 1 && ep.direction == UsbConstants.USB_DIR_IN)
                        return@let ep
                }
                throw IllegalArgumentException("no fb ep?")
            }, 4410, 8,
            10, 4, 44100,
            8, 0, 8)

        // let bRefresh be 0 if device is not UAC1
        fun createExplicitFeedback(device: UsbDevice, usbInterface: UsbInterface,
                                   endpointData: UsbEndpoint, endpointFb: UsbEndpoint,
                                   javaBufferSizeFrames: Int, isoSlots: Int, transferQueueSize: Int,
                                   audioFrameSize: Int, audioSampleRate: Int,
                                   feedbackTransferCount: Int, bRefresh: Int,
                                   feedbackMinIsoSlots: Int): BufferedLibusbAudioOutput {
            val handle = device.takeReference()
            val buf = Buffer(javaBufferSizeFrames, audioFrameSize)
            val ptr = nativeCreateExplicit(device.nativeObject, endpointData.address.toByte(),
                endpointFb.address.toByte(), buf.getPtr(), isoSlots,
                transferQueueSize, audioFrameSize, audioSampleRate, device
                    .getMaxPacketSizeForMicroFrame(usbInterface, endpointData),
                feedbackTransferCount, bRefresh, feedbackMinIsoSlots)
            return BufferedLibusbAudioOutput(device, usbInterface, handle, ptr, buf)
        }

        // in and out sample rate must be derived from the same clock, but one or both of these may
        // still be subjected to clock division, hence they may differ.
        fun createImplicitFeedback(device: UsbDevice, usbInterface: UsbInterface,
                                   endpointData: UsbEndpoint,
                                   endpointFb: UsbEndpoint, javaBufferSizeFrames: Int,
                                   isoSlots: Int, transferQueueSize: Int, audioFrameSize: Int,
                                   audioSampleRate: Int, feedbackFrameSize: Int,
                                   feedbackSampleRate: Int): BufferedLibusbAudioOutput {
            val handle = device.takeReference()
            val buf = Buffer(javaBufferSizeFrames, audioFrameSize)
            val ptr = nativeCreateImplicit(device.nativeObject, endpointData.address.toByte(),
                endpointFb.address.toByte(), buf.getPtr(), isoSlots,
                transferQueueSize, audioFrameSize, audioSampleRate, feedbackFrameSize,
                feedbackSampleRate)
            return BufferedLibusbAudioOutput(device, usbInterface, handle, ptr, buf)
        }

        fun createSync(device: UsbDevice, usbInterface: UsbInterface, endpointData: UsbEndpoint,
                       javaBufferSizeFrames: Int, isoSlots: Int, transferQueueSize: Int,
                       audioFrameSize: Int, audioSampleRate: Int): BufferedLibusbAudioOutput {
            val handle = device.takeReference()
            val buf = Buffer(javaBufferSizeFrames, audioFrameSize)
            val ptr = nativeCreateSync(device.nativeObject, endpointData.address.toByte(),
                buf.getPtr(), isoSlots, transferQueueSize, audioFrameSize, audioSampleRate)
            return BufferedLibusbAudioOutput(device, usbInterface, handle, ptr, buf)
        }
    }
    private val listeners = mutableListOf<AudioOutput.Listener>()
    private var sentAdvancing = false
    private var paused = true
    private var stopping = false
    private var lastTimestampRawPositionFrames = 0uL
    private var expectTimestampFramePositionReset = false
    private var accumulatedRawTimestampFramePosition = 0uL

    init {
        val rate = byteArrayOf(
            0x44.toByte(),
            0xAC.toByte(),
            0x00,
            0x00
        )

        // TODO: watch Active Alternate Setting Control
        // TODO: honor Valid Alternate Settings Control
        // TODO: Terminal Connector Control Interrupt support for jack detection
        // TODO: i have a fuzzy memory of some value that tells me how long i need to wait after altsetting until you can hear something
        //https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers#class-requests-and-interrupt-data-messages
        val r: Int = device.controlTransfer(
            0x21,
            0x01,
            0x0100,
            0x2900,
            rate,
            0,
            rate.size,
            1000
        )

        device.setInterface(usbInterface)
    }

    override fun startStreaming() {
        if (released) return
        while (true) {
            val i = nativeStart(getPtr(), stopping)
            if (i == 2 && stopping) {
                stopping = false
                sentAdvancing = false
                return // do not reschedule start runnable anymore, we successfully stopped
            }
            if (i != 0) {
                if (i == 1 || i == 2) {
                    if (paused || stopping)
                        break
                    Log.e(TAG, "-->start in play(): underflow")
                    listeners.forEach { it.onUnderrun() }
                    continue
                }
                Log.e(TAG, "-->start in play(): error ${errToStr(i)}")
                break//TODO are all other errors fatal
            } else {
                if (!sentAdvancing) {
                    buf.getWriteCounter(tmp)
                    if (tmp[1] != 0L) {
                        // TODO: we could convert monotonic nanotime to epoch if we cared
                        val start = System.currentTimeMillis()
                        listeners.forEach { it.onPositionAdvancing(start) }
                        sentAdvancing = true
                    }
                }
                break
            }
        }
        handler.postDelayed(startRunnable, 100)//TODO: 100ms, or maybe less?
    }

    override fun write(
        buffer: ByteBuffer,
        encodedAccessUnitCount: Int,
        presentationTimeUs: Long
    ): Boolean {
        return buf.write(buffer)
    }

    override fun play() {
        Log.e(TAG, "-->play")
        paused = false
        startStreaming()
    }

    override fun pause() {
        Log.e(TAG, "-->pause")
        paused = true
        stopStreaming()
        if (stopping)
            stopping = false
        sentAdvancing = false
    }

    override fun flush() {
        stopStreaming()
        buf.flush()
        buf.resetWriteCounter()
        expectTimestampFramePositionReset = true
        sentAdvancing = false
        if (stopping)
            stopping = false
        else if (!paused)
            startStreaming()
    }

    override fun stop() {
        Log.e(TAG, "-->stop")
        stopping = true
    }

    override fun onRelease() {
        super.onRelease()
        buf.release()
        listeners.forEach { it.onReleased() }
    }

    override fun setVolume(volume: Float) {
        //TODO("Not yet implemented")
    }

    override fun isOffloadedPlayback(): Boolean {
        return false
    }

    override fun getAudioSessionId(): Int {
        //TODO("Not yet implemented")
        return C.AUDIO_SESSION_ID_UNSET
    }

    override fun getSampleRate(): Int {
        //TODO("Not yet implemented")
        return 44100
    }

    override fun getBufferSizeInFrames(): Long {
        return 441 * 4
    }

    override fun getPositionUs(): Long {
        buf.getWriteCounter(tmp)
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
        //TODO("Not yet implemented")
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
        //TODO("Not yet implemented")
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
}