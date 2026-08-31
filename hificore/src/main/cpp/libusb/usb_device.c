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
//
// Created by Jared Woolston (Jared.Woolston@gmail.com)
//

#include <malloc.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include "common.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#define  LOG_TAG    "UsbDevice-Native"

#define STR_ALLOC_LENGTH 50
#define SIGN_MASK 0xFF

JNIEXPORT jstring JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeGetStringDescriptor(JNIEnv *env, jclass type, jlong device,
                                                              jint index) {
    if (index == 0) {
        return NULL;
    }
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    size_t length = STR_ALLOC_LENGTH * sizeof(unsigned char);
    unsigned char *name = malloc(length);
    libusb_get_string_descriptor_ascii(deviceHandle, (uint8_t) (SIGN_MASK & index), name, (int) length);
    jstring retval = (*env)->NewStringUTF(env, (const char *) name);
    free(name);
    return retval;
}

JNIEXPORT jlong JNICALL
Java_com_jwoolston_libusb_UsbDevice_wrapDevice(JNIEnv *env, jclass type, jlong context, jint fd) {
    LOGD("Wrapping USB Device Handle.");
    struct libusb_device_handle *deviceHandle;

    struct libusb_context *ctx = (struct libusb_context *) context;
    int ret = libusb_wrap_sys_device(ctx, fd, &deviceHandle);

    if (deviceHandle == NULL) {
        LOGE("Failed to wrap usb device file descriptor. Error: %s", libusb_strerror((enum libusb_error) ret));
        return 0;
    }

    return (jlong)deviceHandle;
}

JNIEXPORT jstring JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeGetManufacturerString(JNIEnv *env, jobject instance, jlong device,
                                                                jlong descriptor) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    struct libusb_device_descriptor *deviceDescriptor = (struct libusb_device_descriptor *) descriptor;

    size_t length = STR_ALLOC_LENGTH * sizeof(unsigned char);
    unsigned char *name = malloc(length);
    libusb_get_string_descriptor_ascii(deviceHandle, deviceDescriptor->iManufacturer, name, (int) length);
    jstring retval = (*env)->NewStringUTF(env, (const char *) name);
    free(name);
    return retval;
}

JNIEXPORT jstring JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeGetSerialString(JNIEnv *env, jobject instance, jlong device,
                                                          jlong descriptor) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    struct libusb_device_descriptor *deviceDescriptor = (struct libusb_device_descriptor *) descriptor;

    size_t length = STR_ALLOC_LENGTH * sizeof(unsigned char);
    unsigned char *serial = malloc(length);
    libusb_get_string_descriptor_ascii(deviceHandle, deviceDescriptor->iSerialNumber, serial, (int) length);
    jstring retval = (*env)->NewStringUTF(env, (const char *) serial);
    free(serial);
    return retval;
}

JNIEXPORT jstring JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeGetProductNameString(JNIEnv *env, jobject instance, jlong device,
                                                               jlong descriptor) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    struct libusb_device_descriptor *deviceDescriptor = (struct libusb_device_descriptor *) descriptor;

    size_t length = STR_ALLOC_LENGTH * sizeof(unsigned char);
    unsigned char *name = malloc(length);
    libusb_get_string_descriptor_ascii(deviceHandle, deviceDescriptor->iProduct, name, (int) length);
    jstring retval = (*env)->NewStringUTF(env, (const char *) name);
    free(name);
    return retval;
}

JNIEXPORT jstring JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeGetDeviceVersion(JNIEnv *env, jobject instance, jlong descriptor) {
    struct libusb_device_descriptor *deviceDescriptor = (struct libusb_device_descriptor *) descriptor;
    uint16_t bcdDevice = deviceDescriptor->bcdDevice;
    size_t length = 4 * sizeof(unsigned char);
    char *version = malloc(length);
    snprintf(version, length, "%i.%i", SIGN_MASK & (bcdDevice >> 8), SIGN_MASK & bcdDevice);
    jstring retval = (*env)->NewStringUTF(env, (const char *) version);
    free(version);
    return retval;
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeGetDeviceSpeed(JNIEnv *env, jobject instance, jlong device,
                                                         jlong descriptor) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    return libusb_get_device_speed(libusb_get_device(deviceHandle));
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeGetConfigurationCount(JNIEnv *env, jobject instance, jlong device) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    struct libusb_device_descriptor desc;
    libusb_get_device_descriptor(libusb_get_device(deviceHandle), &desc);
    return desc.bNumConfigurations;
}

static jclass errorClass;
static jmethodID getError;
static jmethodID getNativePtrFromAsyncTransfer;
static jmethodID transferCallback;
static jmethodID transferFailedCallback;
static jmethodID getTransferCallback;
static jmethodID byteBufferLimit;

void LIBUSB_CALL libusb_transfer_callback(struct libusb_transfer *transfer) {
    struct transfer_callback_holder *holder = (struct transfer_callback_holder *) transfer->user_data;
    if (holder == NULL) {
        LOGE("Null callback holder! Callback cannot be called.");
        abort();
    }
    if (holder->ready == 1 && holder->fd != -1) {
        uint64_t counter = 1;
        int fd = holder->fd;
        // RT safety: technically not, this is a system call. but the alternative is busy looping,
        // which is criminal on a battery powered device, and these system calls are unlikely to
        // block (it's kernel-space atomics + changing thread sleeping->ready).
        // Java will iterate though list of weak references with transfers to find this one. This is
        // safe because we have a JNI-side global reference we'll only release if Java finds us.
        holder->ready = 2; // don't access holder after this, java may reap it instantly
        write(fd, &counter, sizeof(counter)); // notify java to wake up if sleeping
        close(fd);
        return;
    }

    int result;
    switch (transfer->status) {
        case LIBUSB_TRANSFER_COMPLETED:
            result = LIBUSB_SUCCESS;
            break;
        case LIBUSB_TRANSFER_TIMED_OUT:
            result = LIBUSB_ERROR_TIMEOUT;
            break;
        case LIBUSB_TRANSFER_STALL:
            result = LIBUSB_ERROR_PIPE;
            break;
        case LIBUSB_TRANSFER_NO_DEVICE:
            result = LIBUSB_ERROR_NO_DEVICE;
            break;
        case LIBUSB_TRANSFER_OVERFLOW:
            result = LIBUSB_ERROR_OVERFLOW;
            break;
        case LIBUSB_TRANSFER_ERROR:
        case LIBUSB_TRANSFER_CANCELLED:
            result = LIBUSB_ERROR_IO;
            break;
        default:
            LOGE("Unrecognised status code %d", transfer->status);
            result = LIBUSB_ERROR_OTHER;
    }
    JNIEnv *env;
    int jniResult = (*holder->vm)->GetEnv(holder->vm, (void **) &env, JNI_VERSION_1_6);
    if (jniResult != JNI_OK) {
        result = LIBUSB_ERROR_OTHER;
        LOGE("Failed to retrieve JNI environment: %i", jniResult);
    }

    unsigned char *data = NULL;
    jobject callback = NULL;

    int transferred = 0;
    if (transfer->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
        // individual packet length and status is already exposed through getIsoBuffer().
        for (int i = 0; i < transfer->num_iso_packets; ++i) {
            if (transfer->iso_packet_desc[i].actual_length > 0) {
                transferred += transfer->iso_packet_desc[i].actual_length;
            } else {
                break;
            }
        }
    } else if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        transferred = LIBUSB_CONTROL_SETUP_SIZE + transfer->actual_length;
    } else {
        transferred = transfer->actual_length;
    }
    if (result >= 0) {
        jobject useless = (*env)->CallObjectMethod(
                env, holder->buffer, byteBufferLimit, transferred);
        (*env)->DeleteLocalRef(env, useless);
        (*env)->DeleteGlobalRef(env, holder->buffer);
    }
    callback = (*env)->CallObjectMethod(env, holder->transfer, getTransferCallback);
    jobject gTransfer = holder->transfer;
    // Mark the transfer as no longer in-flight now that we're no longer using it.
    // After this, we mustn't access it anymore as ownership returns to JVM with this.
    // But we should do it before calling the callback.
    holder->ready = 0; transfer = NULL;
    if (callback != NULL) {
        if (result >= 0) {
            (*env)->CallVoidMethod(env, callback, transferCallback, gTransfer, transferred);
        } else {
            jobject error = (*env)->CallStaticObjectMethod(env, errorClass, getError, result);
            (*env)->CallVoidMethod(env, callback, transferFailedCallback, gTransfer, error,
                                   transferred);
            (*env)->DeleteLocalRef(env, error);
        }
        (*env)->DeleteLocalRef(env, callback);
    }
    // allow freeing the transfer
    (*env)->DeleteGlobalRef(env, gTransfer);
}

JNIEXPORT jboolean JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeInitialize(JNIEnv *env, jclass type) {
    initializeUacLog(env);
    // Find the interrupt transfer callback method
    jobject clazz = (*env)->FindClass(env, "com/jwoolston/libusb/TransferCallback");
    if (clazz == NULL) {
        LOGE("Failed to find class com.jwoolston.libusb.TransferCallback");
        return JNI_FALSE;
    }
    transferCallback = (*env)->GetMethodID(env, clazz, "onTransferComplete",
                                           "(Lcom/jwoolston/libusb/AsyncTransfer;I)V");
    if (transferCallback == NULL) {
        LOGE("Failed to find onTransferComplete(AsyncTransfer, int) method.");
        return JNI_FALSE;
    }
    transferFailedCallback = (*env)->GetMethodID(env, clazz, "onTransferFailed",
                                                 "(Lcom/jwoolston/libusb/AsyncTransfer;Lcom/jwoolston/libusb/LibusbError;I)V");
    if (transferFailedCallback == NULL) {
        LOGE("Failed to find onTransferFailed(AsyncTransfer, LibusbError, int) method.");
        return JNI_FALSE;
    }

    // Find the byte buffer limit()
    clazz = (*env)->FindClass(env, "java/nio/ByteBuffer");
    if (clazz == NULL) {
        LOGE("Failed to find class java.nio.ByteBuffer");
        return JNI_FALSE;
    }
    byteBufferLimit = (*env)->GetMethodID(env, clazz, "limit", "(I)Ljava/nio/Buffer;");
    if (byteBufferLimit == NULL) {
        LOGE("Failed to find limit(int) method.");
        return JNI_FALSE;
    }

    clazz = (*env)->FindClass(env, "com/jwoolston/libusb/AsyncTransfer");
    if (clazz == NULL) {
        LOGE("Failed to find class com.jwoolston.libusb.AsyncTransfer");
        return JNI_FALSE;
    }
    getNativePtrFromAsyncTransfer = (*env)->GetMethodID(env, clazz, "getNativeObject", "()J");
    if (getNativePtrFromAsyncTransfer == NULL) {
        LOGE("Failed to find getNativeObject() method.");
        return JNI_FALSE;
    }
    getTransferCallback = (*env)->GetMethodID(env, clazz, "getCallback", "()Lcom/jwoolston/libusb/TransferCallback;");
    if (getTransferCallback == NULL) {
        LOGE("Failed to find getCallback() method.");
        return JNI_FALSE;
    }

    clazz = (*env)->FindClass(env, "com/jwoolston/libusb/LibusbError");
    if (clazz == NULL) {
        LOGE("Failed to find class com.jwoolston.libusb.LibusbError");
        return JNI_FALSE;
    }
    errorClass = (*env)->NewGlobalRef(env, clazz);
    getError = (*env)->GetStaticMethodID(env, errorClass, "fromNative", "(I)Lcom/jwoolston/libusb/LibusbError;");
    if (getError == NULL) {
        LOGE("Failed to find fromNative(int) method.");
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeClose(JNIEnv *env, jobject instance, jlong device) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    libusb_close(deviceHandle);
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeClearStall(JNIEnv *env, jobject instance, jlong device,
                                                               jint address) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    return libusb_clear_halt(deviceHandle, (unsigned char) address);
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeClaimInterface(JNIEnv *env, jobject instance,
                                                                   jlong device, jint interfaceID) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    return libusb_claim_interface(deviceHandle, interfaceID);
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeReleaseInterface(JNIEnv *env, jobject instance,
                                                                     jlong device, jint interfaceID) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    return libusb_release_interface(deviceHandle, interfaceID);
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeHasKernelDriver(JNIEnv *env, jobject thiz,
                                                             jlong device, jint interface_id) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    return libusb_kernel_driver_active(deviceHandle, interface_id);
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeAttachKernelDriver(JNIEnv *env, jobject thiz,
                                                             jlong device, jint interface_id) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    return libusb_attach_kernel_driver(deviceHandle, interface_id);
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeDetachKernelDriver(JNIEnv *env, jobject thiz,
                                                             jlong device, jint interface_id) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    return libusb_detach_kernel_driver(deviceHandle, interface_id);
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeSetInterface(JNIEnv *env, jobject instance, jlong device,
                                                                 jint interfaceID, jint alternateSetting) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    return libusb_set_interface_alt_setting(deviceHandle, interfaceID, alternateSetting);
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeGetConfiguration(JNIEnv *env, jobject instance,
                                                           jlong device) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    int config;
    int ret = libusb_get_configuration(deviceHandle, &config);
    if (ret < 0) {
        return ret;
    }
    return config;
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeSetConfiguration(JNIEnv *env, jobject instance,
                                                                     jlong device, jint configurationID) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    return libusb_set_configuration(deviceHandle, configurationID);
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeRequestAsync(JNIEnv *env, jobject instance,
                                                       jlong device,
                                                       jobject transferObject,
                                                       jint fd, jobject buffer,
                                                       jint offset, jint length) {
    struct libusb_transfer *_transfer = (struct libusb_transfer *)
            (*env)->CallLongMethod(env, transferObject, getNativePtrFromAsyncTransfer);
    struct transfer_callback_holder *holder = _transfer->user_data;
    if (holder == NULL) {
        LOGE("Null user data set");
        if (fd != -1) close(fd);
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    if (holder->ready != 1) {
        LOGE("Bad ready set: %d", holder->ready);
        if (fd != -1) close(fd);
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    // Ensure none of the free flags, which are footguns here, are set
    if ((_transfer->flags & (LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER)) != 0) {
        LOGE("Bad flags set: %d", _transfer->flags);
        if (fd != -1) close(fd);
        holder->ready = 0;
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    unsigned char *_buffer = (*env)->GetDirectBufferAddress(env, buffer);
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;

    // caller must fill num_iso_packets, iso_packet_desc, timeout, flags, type and endpoint
    _transfer->dev_handle = deviceHandle;
    _transfer->buffer = _buffer + offset;
    _transfer->length = length;
    _transfer->callback = libusb_transfer_callback;

    // Ensure that, in case of control transferObject, the length is declared properly.
    if (_transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL && libusb_control_transfer_get_setup(
            _transfer)->wLength != length - LIBUSB_CONTROL_SETUP_SIZE) {
        LOGE("Bad control setup length set: %d vs %d", libusb_control_transfer_get_setup(
                _transfer)->wLength, length);
        if (fd != -1) close(fd);
        holder->ready = 0;
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    // Ensure that, in case of isochronous transferObject, the buffer is big enough.
    if (_transfer->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS && _transfer->num_iso_packets > 0) {
        int isoLength = libusb_get_iso_packet_buffer(_transfer, _transfer->
            num_iso_packets - 1) - _transfer->buffer + (int)_transfer->iso_packet_desc[_transfer->
                num_iso_packets - 1].length;
        if (isoLength != length || length < 1 || isoLength < 1) {
            LOGE("Bad isochronous length set: %d vs %d", isoLength, length);
            if (fd != -1) close(fd);
            holder->ready = 0;
            return LIBUSB_ERROR_INVALID_PARAM;
        }
    }

    // Populate the transferObject structure
    JavaVM *vm;
    (*env)->GetJavaVM(env, &vm);
    holder->vm = vm;
    holder->transfer = (*env)->NewGlobalRef(env, transferObject);
    holder->buffer = (*env)->NewGlobalRef(env, buffer);
    holder->fd = fd;

    // Submit the transferObject
    int ret = libusb_submit_transfer(_transfer);
    if (ret == LIBUSB_ERROR_BUSY) {
        LOGE("Duplicate transfer submission");
        abort();
    }
    if (ret != 0) {
        // We must always free our callback holder's refs
        (*env)->DeleteGlobalRef(env, holder->transfer);
        (*env)->DeleteGlobalRef(env, holder->buffer);
        holder->ready = 0;
        if (fd != -1) close(fd);
    }
    return ret;
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeCancelAsync(JNIEnv *env, jobject thiz, jlong transfer) {
    return libusb_cancel_transfer((struct libusb_transfer*)transfer);
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeResetDevice(JNIEnv *env, jobject instance, jlong device) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    return libusb_reset_device(deviceHandle);
}

JNIEXPORT jint JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeGetMaxAltPacketSize(JNIEnv *env, jobject thiz,
                                                              jlong device, jint if_number,
                                                              jint if_alt, jint address) {
    struct libusb_device_handle *deviceHandle = (struct libusb_device_handle *) device;
    return libusb_get_max_alt_packet_size(libusb_get_device(deviceHandle),
                                          if_number,if_alt,
                                          address);
}

JNIEXPORT jlong JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeTakeReference(JNIEnv *env, jobject thiz) {
    return (jlong) (*env)->NewGlobalRef(env, thiz);
}

JNIEXPORT void JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeReleaseReference(JNIEnv *env, jobject thiz, jlong ref) {
    (*env)->DeleteGlobalRef(env, (jobject) ref);
}

JNIEXPORT jobject JNICALL
Java_com_jwoolston_libusb_UsbDevice_nativeGetReference(JNIEnv *env, jclass clazz, jlong ref) {
    return (jobject) ref;
}

#pragma clang diagnostic pop