package org.nift4.gramophone.hificore

import java.nio.ByteBuffer

open class Buffer protected constructor(forMixer: Boolean, bufferSizeFrames: Int, frameSize: Int) {
    constructor(bufferSizeFrames: Int, frameSize: Int) : this(false, bufferSizeFrames, frameSize)
    private val ptr = nativeCreateBuffer(forMixer, bufferSizeFrames, frameSize)
    protected var released = false

    internal fun getPtr(): Long {
        if (released) {
            throw IllegalStateException("Streaming was already released")
        }
        return ptr
    }

    internal fun write(
        buffer: ByteBuffer
    ): Boolean {
        if (!buffer.isDirect) {
            throw IllegalArgumentException("Buffer must be direct")
        }
        val progress = nativeWrite(getPtr(), buffer, buffer.position(),
            buffer.remaining())
        buffer.position(buffer.position() + progress)
        return !buffer.hasRemaining()
    }

    protected fun setStopped(stopped: Boolean) {
        return nativeStop(getPtr(), stopped)
    }

    internal open fun getWriteCounter(out: LongArray) {
        return nativeGetWriteCounter(getPtr(), out)
    }

    // Caution: may only be called if transfers are stopped / buffer isn't in mixer
    internal open fun resetWriteCounter() {
        nativeResetWriteCounter(getPtr())
    }

    // Caution: may only be called if transfers are stopped / buffer isn't in mixer
    internal open fun flush() {
        nativeFlush(getPtr())
    }

    @JvmName("getUnderrunCount")
    protected fun getUnderrunCount(): UInt {
        return nativeGetUnderrunCount(getPtr()).toUInt()
    }

    internal open fun release() {
        if (released)
            return
        nativeRelease(getPtr())
        released = true
    }

    protected fun finalize() {
        release()
    }

    private external fun nativeCreateBuffer(forMixer: Boolean, bufferSizeFrames: Int, frameSize: Int): Long
    private external fun nativeStop(ptr: Long, stopped: Boolean)
    private external fun nativeGetUnderrunCount(ptr: Long): Int
    private external fun nativeFlush(ptr: Long)
    private external fun nativeWrite(ptr: Long, buf: ByteBuffer, position: Int, remaining: Int): Int
    private external fun nativeGetWriteCounter(ptr: Long, out: LongArray)
    private external fun nativeResetWriteCounter(ptr: Long)
    private external fun nativeRelease(ptr: Long)
}