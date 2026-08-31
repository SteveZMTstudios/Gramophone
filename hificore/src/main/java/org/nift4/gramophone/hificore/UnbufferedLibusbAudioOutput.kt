package org.nift4.gramophone.hificore

import android.media.AudioDeviceInfo
import android.os.Looper
import androidx.media3.common.C
import androidx.media3.common.PlaybackParameters
import androidx.media3.common.util.Log
import androidx.media3.common.util.Util
import androidx.media3.exoplayer.audio.AudioOutput
import com.jwoolston.libusb.AsyncTransfer
import com.jwoolston.libusb.LibusbError
import com.jwoolston.libusb.UsbConstants
import com.jwoolston.libusb.UsbDevice
import com.jwoolston.libusb.UsbInterface
import com.jwoolston.libusb.TransferCallback
import java.nio.ByteBuffer

// For synchronous, the clock source is the USB clock. That means we send constant amount of
// samples per packet based on the assumption we send exactly samples for 125us, per packet.
// Adaptive sinks will essentially achieve the same result when we use the same strategy.
// The basic assumption for the above is that decoder is faster than real-time to ensure we
// always have enough data. We do NOT use an internal buffer, we use transfers as the buffer.
// If we read too many iso packets into one transfer at once, we would starve the decoder.
// If we do not read enough in, we waste CPU time with repeatedly having overhead of
// decoding and submitting transfer, so we optimally want as much as possible that would not
// starve decoder (but a lot would mean high packet queue size, which means high audio
// latency, which we don't want). The total packet queue (=buffer size, essentially) should
// be tuned for avoiding USB xrun if we are too slow to generate new packets, this means it
// should be some higher multiple of transfer queue to make sure if we are late once or
// twice we don't instantly xrun (maybe 4 times). It should also not be too high due to
// audio latency as previously mentioned.
// We can say 4 transfers and as such (packet queue size / 4) packets per transfer, with
// packet queue size being size of audio buffer. If audio buffer is too small, we will xrun,
// and if it's too big we simply have high latency. Pause/flush can be implemented like that too, by
// cancelling  transfers. So we can go safe and queue a lot of buffers and just cancel some
// transfers if we don't feel like sending those anymore.
class UnbufferedLibusbAudioOutput(
    private val device: UsbDevice, private val usbInterface: UsbInterface
) : AudioOutput, TransferCallback {
    companion object {
        private const val TAG = "UnbufferedLibusbAO"
    }
    private val listeners = mutableListOf<AudioOutput.Listener>()
    private val transfers: List<TimestampedAsyncTransfer>
    private val transferSendQueue = mutableListOf<TimestampedAsyncTransfer>()
    private val transferQueue = mutableListOf<TimestampedAsyncTransfer>()
    private var pendingTransfer: TimestampedAsyncTransfer? = null
    private val looper = Looper.myLooper()!!
    private var timestampFrames = 0L
    private var timestampWrite = 0L
    private var flushGeneration = 0
    private var sentAdvancing = false
    private var paused = false
    private var stopping = true
    private var released = false
    private val streamEp = usbInterface.endpointCount.let {
        for (i in 0..<it) {
            val ep = usbInterface.getEndpoint(i)
            if (ep.endpointNumber == 1 && ep.direction == UsbConstants.USB_DIR_OUT)
                return@let ep
        }
        throw IllegalArgumentException("no stream ep?")
    }

    init {
        device.manager.enableUsbEventsForLooper(looper)
        repeat(4) {
            // assume high speed for now :)
            transferQueue.add(TimestampedAsyncTransfer(device, 8 * 10)) // one transfer gets 10ms of audio, or 441 frames
        }
        transferQueue.forEach {
            it.ensureSize(2 /*16bit*/ * 2 /*stereo*/ * 441)
            it.fillIsochronousTransfer(streamEp, 0, 8 * 10)
            it.setCallbackLooper(looper)
            val ib = it.isoBuffer
            for (i in 0..<it.isoSlots) {
                val samples = (i + 1) * 441 / 80 - i * 441 / 80
                ib.putInt(i * AsyncTransfer.SIZEOF_ISO_PACKET_DESCRIPTOR +
                        AsyncTransfer.OFFSET_ISO_PACKET_SIZE, samples * 4)
            }
            it.callback = this
        }
        transfers = transferQueue.toList()

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

    override fun play() {
        Log.e(TAG, "-->play")
        paused = false
        stopping = false
        transferSendQueue.forEach {
            Log.e(TAG, "-->de-sq transfer with ts ${it.timestamp}, qs=${transferQueue.size}/${transfers.size} (sq=${transferSendQueue.size})")
            val ret = it.submit()
            if (ret != LibusbError.LIBUSB_SUCCESS) {
                Log.e(TAG, "failed to submit transfer: $ret")
            }
        }
        transferSendQueue.clear()
    }

    override fun pause() {
        paused = true
        sentAdvancing = false
        Log.e(TAG, "-->pause")
        //cancel() todo
    }

    override fun write(
        buffer: ByteBuffer,
        encodedAccessUnitCount: Int,
        presentationTimeUs: Long
    ): Boolean {
        while (pendingTransfer != null || transferQueue.isNotEmpty()) {
            val transfer = pendingTransfer.also { pendingTransfer = null }
                ?: transferQueue.removeAt(0).also {
                    it.timestamp = timestampWrite
                    it.flushGeneration = flushGeneration
                    it.buffer.mark()
                }
            val tb = transfer.buffer
            if (buffer.remaining() > tb.remaining()) {
                val oldLimit = buffer.limit()
                buffer.limit(buffer.position() + tb.remaining())
                tb.put(buffer)
                buffer.limit(oldLimit)
            } else {
                tb.put(buffer)
            }
            if (tb.hasRemaining()) { // we exhausted the input buffer, we want more data!
                pendingTransfer = transfer
                return true
            } else { // the transfer is full, time to send it out!
                tb.reset()
                if (!paused && !stopping) {
                    Log.e(TAG, "-->submit transfer with ts ${transfer.timestamp}(${timestampWrite}), qs=${transferQueue.size}/${transfers.size} (sq=${transferSendQueue.size})")
                    transfer.submit()
                } else {
                    Log.e(TAG, "-->sq transfer with ts ${transfer.timestamp}(${timestampWrite}), qs=${transferQueue.size}/${transfers.size} (sq=${transferSendQueue.size})")
                    transferSendQueue.add(transfer)
                }
                timestampWrite += tb.limit() / 4
            }
        }
        return false
    }

    private fun cancel() {
        Log.e(TAG, "cancelling transfers!!!!", RuntimeException())
        transfers.forEach {
            if (it.isInFlight) {
                val ret = it.cancel()
                if (ret != LibusbError.LIBUSB_SUCCESS) {
                    Log.e(TAG, "failed to cancel transfer: $ret")
                }
            }
        }
    }

    override fun flush() {
        Log.e(TAG, "-->flush")
        flushGeneration++
        sentAdvancing = false
        timestampFrames = 0
        timestampWrite = 0
        cancel()
        transferQueue.addAll(transferSendQueue)
        transferSendQueue.clear()
        pendingTransfer?.let { transferQueue.add(it) }
        pendingTransfer = null
    }

    override fun stop() {
        Log.e(TAG, "-->stop")
        stopping = true
        sentAdvancing = false
    }

    override fun release() {
        Log.e(TAG, "-->release")
        try {
            // set altsetting 0 (idle)
            device.setInterface(device.getConfigurationOrThrow()!!
                .getInterface(usbInterface.id, 0))
        } catch (e: Exception) {
            Log.e(TAG, "failed to reset to idle interface", e)
        }
        device.manager.disableUsbEventsForLooper(looper, false)
        released = true
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
        //TODO: this should NOT reset on flush ....or should it?!
        return Util.sampleCountToDurationUs(timestampFrames, sampleRate)
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

    override fun onTransferComplete(
        transfer: AsyncTransfer,
        bytesTransferred: Int
    ) {
        if (released) return
        val transfer = transfer as TimestampedAsyncTransfer
        val time = System.currentTimeMillis() - 10
        if (transfer.flushGeneration == flushGeneration) {
            this.timestampFrames = transfer.timestamp
            if (!paused && !stopping && !sentAdvancing) {
                listeners.forEach { it.onPositionAdvancing(time) }
                sentAdvancing = true
            }
        }
        transfer.buffer.clear()
        transferQueue.add(transfer)
        Log.e(TAG, "SUCCESSFUL transfer with ts ${transfer.flushGeneration} ${transfer.timestamp}(at gen${flushGeneration} $timestampFrames), tx=$bytesTransferred, qs=${transferQueue.size}/${transfers.size} (p=${pendingTransfer != null},sq=${transferSendQueue.size})")
    }

    override fun onTransferFailed(
        transfer: AsyncTransfer,
        result: LibusbError,
        bytesTransferred: Int
    ) {
        if (released) return
        val transfer = transfer as TimestampedAsyncTransfer
        //TODO()
        Log.e(TAG, "failed to send data: $result, tx=$bytesTransferred")
        transfer.buffer.clear()
        transferQueue.add(transfer)
    }

    private class TimestampedAsyncTransfer(device: UsbDevice, isoSlots: Int) : AsyncTransfer(device,
        isoSlots
    ) {
        var timestamp = 0L
        var flushGeneration = 0
    }
}