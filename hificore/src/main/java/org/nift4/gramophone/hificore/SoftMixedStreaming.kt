package org.nift4.gramophone.hificore

import androidx.media3.common.util.Log
import com.jwoolston.libusb.UsbDevice
import com.jwoolston.libusb.UsbInterface

class SoftMixedStreaming private constructor(
    device: UsbDevice, usbInterface: UsbInterface, handle: Long, ptr: Long
) : Streaming(device, usbInterface, handle, ptr, true) {
    companion object {
        private const val TAG = "SoftMixedStreaming"
    }
    private val buffers = arrayListOf<MixedAudioOutput>()

    init {
        startStreaming()
    }

    override fun startStreaming() {
        if (released) return
        while (true) {
            val i = nativeStart(getPtr(), false)
            if (i != 0) {
                Log.e(TAG, "-->start in play(): error ${errToStr(i)}")
                break//TODO are all other errors fatal
            } else {
                synchronized(buffers) { buffers.toList() }.forEach { buf ->
                    buf.periodicCallback()
                }
                break
            }
        }
        handler.postDelayed(startRunnable, 100)//TODO: 100ms, or maybe less?
    }

    // can be called from multiple threads at the same time
    fun addBuffer(buf: MixedAudioOutput): Boolean {
        synchronized(buffers) {
            val ret = nativeAddBuffer(getPtr(), buf.getPtr())
            if (ret == 1) {
                buffers.add(buf)
                return true
            }
            if (ret == 0) {
                return false // The buffer was already added
            }
            if (ret == -1) {
                throw IllegalArgumentException("This buffer is wrong size for this mixer")
            }
            throw IllegalStateException("forgot to handle return code: $ret")
        }
    }

    // can be called from multiple threads at the same time
    fun removeBuffer(buf: MixedAudioOutput): Boolean {
        synchronized(buffers) {
            val ret = nativeRemoveBuffer(getPtr(), buf.getPtr())
            if (ret) {
                buffers.remove(buf)
                return true
            }
            return false
        }
    }

    fun getSampleRate(): Int {
        return 44100 // TODO
    }

    // TODO: impl creation code
    private external fun nativeCreateSoftMixer(): Long
    private external fun nativeAddBuffer(ptr: Long, buf: Long): Int
    private external fun nativeRemoveBuffer(ptr: Long, buf: Long): Boolean
}