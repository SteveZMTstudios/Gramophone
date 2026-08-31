package org.nift4.gramophone.hificore

import android.os.Handler
import android.os.Looper
import androidx.media3.common.util.Log
import com.jwoolston.libusb.LibusbError
import com.jwoolston.libusb.UsbDevice
import com.jwoolston.libusb.UsbInterface

abstract class Streaming(
    protected val device: UsbDevice, protected val usbInterface: UsbInterface,
    protected val handle: Long, protected val ptr: Long, private val autoReleaseNativeBuf: Boolean
) {
    companion object {
        private const val TAG = "Streaming"
        external fun nativeCreateExplicit(
            nativeObject: Long,
            endpointData: Byte,
            endpointFb: Byte,
            source: Long,
            isoSlots: Int,
            transferQueueSize: Int,
            audioFrameSize: Int,
            audioSampleRate: Int,
            maxIsoPacketSizeBytes: Int,
            feedbackTransferCount: Int,
            bRefresh: Int,
            feedbackMinIsoSlots: Int
        ): Long

        external fun nativeCreateImplicit(
            nativeObject: Long,
            endpointData: Byte,
            endpointFb: Byte,
            source: Long,
            isoSlots: Int,
            transferQueueSize: Int,
            audioFrameSize: Int,
            audioSampleRate: Int,
            feedbackFrameSize: Int,
            feedbackSampleRate: Int
        ): Long

        external fun nativeCreateSync(
            nativeObject: Long,
            endpointData: Byte,
            source: Long,
            isoSlots: Int,
            transferQueueSize: Int,
            audioFrameSize: Int,
            audioSampleRate: Int
        ): Long
    }
    protected val handler = Handler(Looper.myLooper()!!)
    protected var released = false
        private set
    protected val startRunnable = Runnable { startStreaming() }
    protected val tmp = LongArray(2)

    init {
        device.manager.enableUsbEventsForLooper(handler.looper)
    }

    protected fun errToStr(i: Int) = if (i <= 0) LibusbError.fromNative(i).toString()
    else if (i == 1) "Underflow" else "unknown: $i"

    @JvmName("getPtrChecked")
    protected fun getPtr(): Long {
        if (released) {
            throw IllegalStateException("Streaming was already released")
        }
        return ptr
    }

    protected open fun stopStreaming() {
        handler.removeCallbacks(startRunnable)
        nativeStop(getPtr())
    }

    open fun release() {
        if (released)
            return
        Log.e(TAG, "-->release")
        stopStreaming()
        try {
            // set altsetting 0 (idle)
            device.setInterface(device.getConfigurationOrThrow()!!
                .getInterface(usbInterface.id, 0))
        } catch (e: Exception) {
            Log.e(TAG, "failed to reset to idle interface", e)
        }
        device.manager.disableUsbEventsForLooper(handler.looper, false)
        nativeRelease(ptr, autoReleaseNativeBuf)
        UsbDevice.releaseReferenceStatic(handle)
        released = true
        onRelease()
    }

    protected open fun onRelease() {}
    protected abstract fun startStreaming()

    protected open fun finalize() {
        release()
    }

    // To keep streaming running:
    // 1. call nativeStart()
    // 2. if error is returned: handle error (for example, LIBUSB_ERROR_NO_DEVICE -> call stop),
    //    and if wanting to continue, go to step 1. if LIBUSB_SUCCESS is returned, go to step 3.
    // 3. wait 100ms, then go to step 1
    // ... and don't forget to write enough data :)
    protected external fun nativeStart(ptr: Long, empty: Boolean): Int
    private external fun nativeStop(ptr: Long)
    private external fun nativeRelease(ptr: Long, autoReleaseNativeBuf: Boolean)
}