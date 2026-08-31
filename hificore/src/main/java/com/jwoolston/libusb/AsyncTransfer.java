/*
 * Copyright (C) 2017 Jared Woolston
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.jwoolston.libusb;

import android.os.Looper;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * @author Jared Woolston (Jared.Woolston@gmail.com)
 */
public class AsyncTransfer {

    public static final int SIZEOF_ISO_PACKET_DESCRIPTOR = 12;
    public static final int OFFSET_ISO_PACKET_SIZE = 0;
    public static final int OFFSET_ISO_PACKET_ACTUAL_SIZE = 4;
    public static final int OFFSET_ISO_PACKET_STATUS = 8;
    public final int isoSlots;
    protected final UsbDevice device;
    @Nullable Looper callbackLooper;
    private long nativeObject;
    private ByteBuffer buffer; // TODO: DMA support (libusb_dev_mem_alloc/free)
    private final ByteBuffer isoSizeBuffer;
    TransferCallback callback;

    public AsyncTransfer(@NonNull UsbDevice device, int isoSlots) {
        this.isoSlots = isoSlots;
        this.device = device;
        this.nativeObject = nativeAllocate(isoSlots);
        this.isoSizeBuffer = isoSlots > 0 ? nativeGetIsoBuffer(nativeObject, isoSlots) : null;
        if (isoSizeBuffer != null) {
            isoSizeBuffer.order(ByteOrder.nativeOrder());
        }
    }

    // TODO should every method accessing nativeObject be synchronized to avoid UAF with another
    //  thread calling release? all of our code is safe, just if user calls us twice it's possible

    public boolean isInFlight() {
        return nativeIsInFlight(getNativeObject());
    }

    boolean readyForCallback() {
        return nativeReadyForCallback(getNativeObject());
    }

    void fly() {
        nativeFly(getNativeObject());
    }

    void callbackOnLooper() {
        nativeCallback(getNativeObject());
    }

    long getNativeObject() {
        if (nativeObject == 0) {
            throw new IllegalStateException("This transfer was already released");
        }
        return nativeObject;
    }

    public void setCallbackLooper(Looper callbackLooper) {
        if (isInFlight()) {
            throw new IllegalStateException("Transfer is in flight, can't change looper anymore");
        }
        this.callbackLooper = callbackLooper;
    }

    /** Calls {@link UsbDevice#asyncTransfer(AsyncTransfer)}. */
    public LibusbError submit() {
        return device.asyncTransfer(this);
    }

    /** Calls {@link UsbDevice#cancelAsyncTransfer(AsyncTransfer)}. */
    public LibusbError cancel() {
        return device.cancelAsyncTransfer(this);
    }

    public void setBuffer(@NonNull ByteBuffer buffer) {
        if (!buffer.isDirect()) {
            throw new IllegalArgumentException("Buffer should be direct");
        }
        if (nativeObject == 0) {
            throw new IllegalArgumentException("This transfer was already released");
        }
        if (isInFlight()) {
            throw new IllegalStateException("Transfer is in flight, can't change buffer anymore");
        }
        this.buffer = buffer;
    }

    public final ByteBuffer getBuffer() {
        if (buffer == null) {
            throw new IllegalStateException("Buffer of transfer not set yet");
        }
        if (nativeObject != 0 && isInFlight()) {
            throw new IllegalStateException("Transfer is in flight, can't use buffer anymore");
        }
        return buffer;
    }

    public boolean hasBuffer() {
        return buffer != null;
    }

    public ByteBuffer ensureSize(int size) {
        if (hasBuffer()) {
            ByteBuffer buffer = getBuffer();
            // Avoid allocation churn if similarly sized buffer is available
            if (buffer.capacity() >= size && buffer.capacity() <= 2 * size) {
                buffer.clear();
                buffer.limit(size);
                return buffer;
            }
        }
        ByteBuffer buffer = ByteBuffer.allocateDirect(size).order(ByteOrder.nativeOrder());
        setBuffer(buffer);
        return buffer;
    }

    public void setCallback(@NonNull TransferCallback callback) {
        if (nativeObject == 0) {
            throw new IllegalArgumentException("This transfer was already released");
        }
        this.callback = callback;
    }

    // JNI will call this after the transfer already completed.
    public TransferCallback getCallback() {
        return callback;
    }

    /**
     * Convenience function that prepares an empty {@link AsyncTransfer} to be sent out as control
     * transfer. First, sets the transfer type to control transfer, the target endpoint to 0, and
     * the request timeout to the parameter value. Then, writes the control setup packet into the
     * buffer, and finally writes the control data from the byteArr parameter into the buffer.
     * The buffer position is then reset so that the buffer is ready for sending.
     *
     * @param requestType
     * @param request
     * @param value
     * @param index
     * @param byteArr
     * @param offset
     * @param length
     * @param timeout milliseconds or 0 for infinite
     */
    public void fillControlTransfer(int requestType, int request, int value, int index,
                                    byte[] byteArr, int offset, int length, int timeout) {
        ByteBuffer buffer = ensureSize(8 + length);
        fillControlTransferWithSetupIntoCurrentBuffer(requestType, request, value, index, timeout,
                length);
        buffer.put(byteArr, offset, length);
        buffer.reset();
    }

    /**
     * Sets the transfer type to control transfer, the target endpoint to 0, and the request timeout
     * to the parameter value. The buffer is not modified or used.
     *
     * @param timeout milliseconds or 0 for infinite
     */
    public void fillControlTransfer(int timeout) {
        if (isInFlight()) {
            throw new IllegalStateException("Transfer is in flight, can't change transfer anymore");
        }
        nativeFillControlTransfer(getNativeObject(), timeout);
    }

    /**
     * Convenience function that calls both {@link
     * #writeControlTransferSetupIntoCurrentBuffer(int, int, int, int, int)} and {@link
     * #fillControlTransfer(int)}. After this function returns, write the control data (if any) into
     * the buffer with {@link ByteBuffer#put} or similar, and then call {@link ByteBuffer#reset()}
     * to prepare the buffer for sending.
     *
     * @param requestType
     * @param request
     * @param value
     * @param index
     * @param timeout
     * @param length length of the control data that will be written after this function returned
     */
    public void fillControlTransferWithSetupIntoCurrentBuffer(int requestType, int request,
                                                              int value, int index, int timeout,
                                                              int length) {
        fillControlTransfer(timeout);
        buffer.mark();
        writeControlTransferSetupIntoCurrentBuffer(requestType, request, value, index, length);
        buffer.limit(buffer.position() + length);
    }

    /**
     * Writes 8 bytes into buffer at the current position, advancing the position by 8. The written
     * bytes are the control setup packet, filled with the parameter values.
     *
     * @param requestType
     * @param request
     * @param value
     * @param index
     */
    public void writeControlTransferSetupIntoCurrentBuffer(int requestType, int request, int value,
                                                           int index, int length) {
        ByteBuffer buffer = getBuffer();
        int startPos = buffer.position();
        nativeSetupControlTransfer(buffer, requestType, request, value, index, length, startPos);
        buffer.position(startPos + 8);
    }

    private native void nativeSetupControlTransfer(ByteBuffer buffer,
                                                  int requestType, int request, int value,
                                                  int index, int length, int offset);

    private native void nativeFillControlTransfer(long nativeObject, int timeout);

    /**
     * Sets the transfer type to bulk transfer, and the target endpoint plus the request timeout
     * to the parameter value. The buffer is not modified or used.
     *
     * @param endpoint target endpoint
     * @param timeout milliseconds or 0 for infinite
     */
    public void fillBulkTransfer(UsbEndpoint endpoint, int timeout) {
        if (isInFlight()) {
            throw new IllegalStateException("Transfer is in flight, can't change transfer anymore");
        }
        nativeFillBulkTransfer(getNativeObject(), endpoint.getAddress(), timeout);
    }

    /**
     * Convenience function that sets the transfer type to bulk transfer and applies the requested
     * target endpoint and timeout parameter values. Then, it writes the passed buffer into the
     * transfer buffer, and resets the buffer position so that the buffer is ready for sending.
     *
     * @param endpoint target endpoint
     * @param buffer
     * @param offset
     * @param length
     * @param timeout milliseconds or 0 for infinite
     */
    public void fillBulkTransfer(UsbEndpoint endpoint, byte[] buffer, int offset, int length, int timeout) {
        fillBulkTransfer(endpoint, timeout);
        ByteBuffer buffer1 = ensureSize(length);
        buffer1.put(buffer, offset, length);
        buffer1.position(0);
    }

    private native void nativeFillBulkTransfer(long nativeObject, int address, int timeout);

    /**
     * Sets the transfer type to bulk stream transfer, and the target endpoint plus the request
     * timeout to the parameter value. The buffer is not modified or used.
     *
     * @param endpoint target endpoint
     * @param timeout milliseconds or 0 for infinite
     * @param streamId
     */
    public void fillBulkStreamTransfer(UsbEndpoint endpoint, int timeout, int streamId) {
        if (isInFlight()) {
            throw new IllegalStateException("Transfer is in flight, can't change transfer anymore");
        }
        nativeFillBulkStreamTransfer(getNativeObject(), endpoint.getAddress(), timeout, streamId);
    }

    /**
     * Convenience function that sets the transfer type to bulk stream transfer and applies the
     * requested target endpoint and timeout parameter values. Then, it writes the passed buffer
     * into the transfer buffer, and resets the buffer position so that the buffer is ready for
     * sending.
     *
     * @param endpoint target endpoint
     * @param buffer
     * @param offset
     * @param length
     * @param timeout milliseconds or 0 for infinite
     * @param streamId
     */
    public void fillBulkStreamTransfer(UsbEndpoint endpoint, byte[] buffer, int offset, int length, int timeout, int streamId) {
        fillBulkStreamTransfer(endpoint, timeout, streamId);
        ByteBuffer buffer1 = ensureSize(length);
        buffer1.put(buffer, offset, length);
        buffer1.position(0);
    }

    private native void nativeFillBulkStreamTransfer(long nativeObject, int address, int timeout, int streamId);

    /**
     * Sets the transfer type to interrupt transfer, and the target endpoint plus the request
     * timeout to the parameter value. The buffer is not modified or used.
     *
     * @param endpoint target endpoint
     * @param timeout milliseconds or 0 for infinite
     */
    public void fillInterruptTransfer(UsbEndpoint endpoint, int timeout) {
        if (isInFlight()) {
            throw new IllegalStateException("Transfer is in flight, can't change transfer anymore");
        }
        nativeFillInterruptTransfer(getNativeObject(), endpoint.getAddress(), timeout);
    }

    /**
     * Convenience function that sets the transfer type to interrupt transfer and applies the
     * requested target endpoint and timeout parameter values. Then, it writes the passed buffer
     * into the transfer buffer, and resets the buffer position so that the buffer is ready for
     * sending.
     *
     * @param endpoint target endpoint
     * @param buffer
     * @param offset
     * @param length
     * @param timeout milliseconds or 0 for infinite
     */
    public void fillInterruptTransfer(UsbEndpoint endpoint, byte[] buffer, int offset, int length, int timeout) {
        fillInterruptTransfer(endpoint, timeout);
        ByteBuffer buffer1 = ensureSize(length);
        buffer1.put(buffer, offset, length);
        buffer1.position(0);
    }

    private native void nativeFillInterruptTransfer(long nativeObject, int address, int timeout);

    /**
     * Sets the transfer type to isochronous transfer, and the target endpoint plus the request
     * timeout plus the packet count to the parameter value. The buffer is not modified or used.
     *
     * @param endpoint target endpoint
     * @param timeout milliseconds or 0 for infinite
     * @param numPackets
     */
    public void fillIsochronousTransfer(UsbEndpoint endpoint, int timeout, int numPackets) {
        if (numPackets > isoSlots) {
            throw new IllegalArgumentException("Transfer was allocated with maximum of " + isoSlots
                    + " packets but tried to set packet count to " + numPackets);
        }
        if (isInFlight()) {
            throw new IllegalStateException("Transfer is in flight, can't change transfer anymore");
        }
        nativeFillIsochronousTransfer(getNativeObject(), endpoint.getAddress(), timeout, numPackets);
    }

    /** Set the packet size of a specific isochronous packet (or -1 for all packets). */
    public void setIsochronousPacketSize(int packetNumber, int size) {
        if (isInFlight()) {
            throw new IllegalStateException("Transfer is in flight, can't change transfer anymore");
        }
        if (packetNumber >= isoSlots) {
            throw new IllegalArgumentException("Packet number " + packetNumber + " would be out " +
                    "of bounds (" + isoSlots + ")");
        }
        if (packetNumber >= 0) {
            isoSizeBuffer.putInt(packetNumber * SIZEOF_ISO_PACKET_DESCRIPTOR +
                    OFFSET_ISO_PACKET_SIZE, size);
        } else {
            for (int i = 0; i < isoSlots; i++) {
                isoSizeBuffer.putInt(i * SIZEOF_ISO_PACKET_DESCRIPTOR +
                        OFFSET_ISO_PACKET_SIZE, size);
            }
        }
    }

    /**
     * Returns a direct ByteBuffer pointing to a C-array of libusb_iso_packet_descriptor. They can
     * be modified using the ByteBuffer methods and helper sizes like {@link
     * #SIZEOF_ISO_PACKET_DESCRIPTOR}, {@link #OFFSET_ISO_PACKET_SIZE}, {@link
     * #OFFSET_ISO_PACKET_ACTUAL_SIZE} and {@link #OFFSET_ISO_PACKET_STATUS}.<p>
     *
     * Take special care to not use this buffer after submitting a transfer!
     */
    public ByteBuffer getIsoBuffer() {
        if (isoSlots == 0) {
            throw new IllegalArgumentException("This transfer was set up with zero iso slots");
        }
        if (isInFlight()) {
            throw new IllegalStateException("Transfer is in flight, can't change transfer anymore");
        }
        return isoSizeBuffer;
    }

    private native void nativeFillIsochronousTransfer(long nativeObject, int address, int timeout, int numPackets);

    public void setFlags(int flags, int mask) {
        if (isInFlight()) {
            throw new IllegalStateException("Transfer is in flight, can't change transfer anymore");
        }
        nativeSetFlags(getNativeObject(), flags, mask);
    }
    private native void nativeSetFlags(long nativeObject, int flags, int mask);

    @Override
    protected void finalize() throws Throwable {
        release();
        super.finalize();
    }

    public void release() {
        if (nativeObject == 0)
            return;
        if (isInFlight())
            throw new IllegalStateException("Can't release in-progress transfer");
        nativeDestroy(getNativeObject());
        device.manager.onTransferReleased(this);
        nativeObject = 0;
    }

    private native long nativeAllocate(int isoSlots);
    private native ByteBuffer nativeGetIsoBuffer(long nativeObject, int isoSlots);
    private native boolean nativeIsInFlight(long nativeObject);
    private native boolean nativeReadyForCallback(long nativeObject);
    private native void nativeFly(long nativeObject);
    private native void nativeCallback(long nativeObject);
    private native void nativeDestroy(long nativeObject);
}
