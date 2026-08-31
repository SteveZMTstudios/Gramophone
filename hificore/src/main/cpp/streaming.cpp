/*
 *     Copyright (C) 2026 nift4
 *
 *     Gramophone is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     Gramophone is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <libusb.h>
#include <jni.h>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <vector>
#include <stdckdint.h>
#include "farbot/include/farbot/RealtimeObject.hpp"
#include "farbot/include/farbot/RealtimeTraits.hpp"

// Basic idea: the feedback number is slots(!)/microframe. Isochronous means 1 microframe
// is one packet, so it's how much samples (integer samples only) one packet should be.
// And the question of how many packets are queued at once, is simply based on OS scheduling
// constraints, as the packets need to return from flight, filled up again, and submitted,
// before the USB controller buffer is empty.
// (It's important to remember that 10 transfers with each 1 iso packet, and 1 transfer with
// 10 iso packets, are exactly the same on the USB bus. It's just a question of how often we
// get woken up to refill buffers! So the tuning of packets per transfer is a question of
// efficiency, while the question of packet queue size is a question of will it underflow or
// not. The queue can also be too big: then the feedback loop becomes big enough the device
// can no longer compensate because it's internal buffer is empty or full, and xrun occurs.)
// For each SINGLE iso packet(!!! not per transfer) we do:
//    We clear the accumulated number's decimal part to just keep the fraction to carry over.
//    We read the feedback and add it to the accumulated number. The accumulated number is
//    exactly how much samples we need to send in this packet. (Except the fraction which is
//    kept for next time)
// repeat this until we have enough iso packets to fill one transfer, and send it out! then,
// once one of the earlier transfers is done, prepare the next one. (Notably it should
// always use the latest feedback value and not use any averages or similar.)

// For async mode, the feedback must be as low-latency as possible, so the packet queue must be as
// small as possible and refilled in real-time-safe environment, that is, native thread. But because
// decoder isn't real-time-safe, we use an internal buffer for async mode. This internal buffer must
// be big enough to compensate for non-real-time decoder, while the packet queue is small to keep
// feedback latency small. We can also choose to use low amount of transfers if we use a
// real-time-safe transfer filling environment.)

enum StreamingError {
    STREAMING_NO_ERROR = LIBUSB_SUCCESS,
    STREAMING_ERROR_UNDERFLOW,
    STREAMING_ERROR_GLOBAL_UNDERFLOW,
    STREAMING_ERROR_INVALID_ARGUMENT,
};

struct timestamp {
    uint64_t frameCount;
    uint64_t nanoTime;
};
using RealtimeTimestamp = farbot::RealtimeObject<timestamp, farbot::RealtimeObjectOptions::realtimeMutatable>;
static_assert (farbot::is_realtime_move_assignable<timestamp>::value);

static void LIBUSB_CALL transfer_callback_wrapper(libusb_transfer* transfer);
class Transfer {
private:
    enum State {
        Idle,
        Active, // waiting for callback to then re-submit, staying active
        Canceled, // waiting for callback to then go to Idle
    };
    std::atomic<State> state{State::Idle};
    static_assert(std::atomic<State>::is_always_lock_free);
    std::atomic<bool> reallyIdle{true};
    static_assert(std::atomic<bool>::is_always_lock_free);
    std::atomic<int> error = LIBUSB_SUCCESS; // negative=libusb_error, 0=ok, positive=custom error
    static_assert(std::atomic<int>::is_always_lock_free);
    void* dma = nullptr;

protected:
    libusb_transfer* transfer;
    ssize_t bufferSize;

    virtual int doSubmit() {
        return libusb_submit_transfer(transfer);
    }

    virtual void doCancel() {
        libusb_cancel_transfer(transfer);
    }

public:
    Transfer(int isoSlots, libusb_device_handle* device, char endpoint, ssize_t buffer_size,
             bool allowDmaMemory) :
    bufferSize(buffer_size) {
        transfer = libusb_alloc_transfer(isoSlots);
        transfer->num_iso_packets = isoSlots;
        transfer->dev_handle = device;
        transfer->flags = 0;
        transfer->type = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
        transfer->timeout = 60000; // placeholder, TBD
        transfer->endpoint = endpoint;
        transfer->length = buffer_size;
        if (allowDmaMemory) {
            dma = libusb_dev_mem_alloc(device, transfer->length);
        }
        transfer->buffer = static_cast<unsigned char *>(dma != nullptr ? dma :
                                                            malloc(transfer->length));
        transfer->user_data = this;
        transfer->callback = transfer_callback_wrapper;
    }

    void awaitStop() {
        state.wait(Transfer::Active);
        state.wait(Transfer::Canceled);
        while (!reallyIdle.load())
            ; // budget spinlock
    }

    // Threading contract: there is thread A which calls start/cancel, and thread B which is the
    // libusb event thread. So, start and cancel can't run at the same time, and two callbacks can't
    // run at the same time either, but start/cancel and callback can race with each other.
    bool start1() {
        State t = state.exchange(Transfer::Active);
        if (t == State::Active) {
            // already as requested.
            return false;
        }
        if (t == State::Canceled) {
            // if state is currently canceled: we set it to Active again, so the callback will
            // re-submit once the prior cancellation is done.
            return false;
        }
        // if state is currently idle: we'll have to submit the transfer.
        reallyIdle.store(false);
        int rc = process(false);
        error = rc;
        if (rc != 0) {
            state.store(Transfer::Idle);
            reallyIdle.store(true);
            return false;
        }
        return true;
    }

    // The reason start process is split into two parts is that process() shouldn't race with itself
    void start2() {
        int rc = doSubmit();
        error = (libusb_error) -abs(rc);
        if (rc < LIBUSB_SUCCESS) {
            state.store(Transfer::Idle);
            reallyIdle.store(true);
            return;
        }
    }

    int dequeueError() {
        return error.exchange(LIBUSB_SUCCESS);
    }

    bool dequeueUnderflow() {
        int expected = STREAMING_ERROR_UNDERFLOW;
        if (error.compare_exchange_strong(expected, LIBUSB_SUCCESS)) {
            awaitStop();
            return true;
        }
        return false;
    }

    bool isIdle() {
        return state.load() == Transfer::Idle;
    }

    void cancel() {
        State t = Transfer::Active;
        if (state.compare_exchange_strong(t, Transfer::Canceled)) {
            // It's possible this races with callback completing on another thread, but that's OK,
            // libusb is explicitly thread-safe so the design here can be simple.
            // Another possible race is the callback reading that it's active, us then setting
            // canceled state and cancelling, and it only then resubmitting (so the cancellation
            // didn't take effect), which is OK, because we'll just cancel it once the transfer is
            // done which won't take long. Another possible tradeoff to make is to fix this specific
            // race with a mutex, which has the problem that it could block the callback thread if
            // libusb_cancel_transfer takes a long time.
            // TODO but is blocking really an issue? we don't need real time transfers when we
            //  cancel everything after all ^^
            doCancel();
        }
        // the transfer isn't active anymore, either it's already being canceled or idle.
    }

    // returns error code if nothing to send/receive, 0 if should (re)submit
    virtual int process(bool inCallback) = 0;

    virtual void callback(libusb_transfer* theTransfer) {
        State t = Transfer::Canceled;
        if (state.compare_exchange_strong(t, Transfer::Idle)) {
            // the transfer got canceled. we now gave it back to the caller. let's drop the
            // transfer status because it doesn't matter.
            // TODO: it is unknown whether this is lock free with NDK. C++29 will expose a way
            //  to query this (https://github.com/cplusplus/papers/issues/1915), but a manual
            //  audit would certainly be worth it.
            state.notify_all();
            reallyIdle.store(true);
            return;
        }
        // The transfer is active. (If it were idle, we wouldn't be in a callback.)
        bool ok = false;
        int rc;
        switch (transfer->status) {
            case LIBUSB_TRANSFER_COMPLETED:
                rc = process(true);
                if (rc == 0) {
                    ok = true;
                } else {
                    error = rc;
                }
                break;
            case LIBUSB_TRANSFER_TIMED_OUT:
                error = LIBUSB_ERROR_TIMEOUT;
                break;
            case LIBUSB_TRANSFER_STALL:
                error = LIBUSB_ERROR_PIPE;
                break;
            case LIBUSB_TRANSFER_NO_DEVICE:
                error = LIBUSB_ERROR_NO_DEVICE;
                break;
            case LIBUSB_TRANSFER_OVERFLOW:
                error = LIBUSB_ERROR_OVERFLOW;
                break;
            case LIBUSB_TRANSFER_ERROR:
            case LIBUSB_TRANSFER_CANCELLED:
                error = LIBUSB_ERROR_IO;
                break;
            default:
                error = LIBUSB_ERROR_OTHER;
                break;
        }
        if (ok) {
            rc = doSubmit();
            error = (libusb_error) -abs(rc);
            ok = rc >= LIBUSB_SUCCESS;
        }
        if (!ok) {
            state.store(Transfer::Idle);
            // TODO: it is unknown whether this is lock free with NDK. C++29 will expose a way
            //  to query this (https://github.com/cplusplus/papers/issues/1915), but a manual
            //  audit would certainly be worth it.
            state.notify_all();
            reallyIdle.store(true);
            return;
        }
    }

    // dtor can only be canceled after cancel()
    virtual ~Transfer() {
        awaitStop();
        if (dma != nullptr)
            libusb_dev_mem_free(transfer->dev_handle, transfer->buffer,
                                transfer->length);
        else
            free(transfer->buffer);
        libusb_free_transfer(transfer);
    }
};
static void LIBUSB_CALL transfer_callback_wrapper(libusb_transfer* transfer) {
    ((Transfer*) transfer->user_data)->callback(transfer);
}

static uint16_t u16Accumulate(uint32_t* accumulator, uint32_t value) {
    *accumulator += value;
    uint16_t frames = *accumulator >> 16;
    *accumulator &= 0xffff;
    return frames;
}

// Unambiguously U10.14 feedback for full-speed operation of USB Audio Class 1.0 device.
constexpr ssize_t kFeedbackSizeUac1 = 3;
// Some devices sadly misinterpret the USB Audio Class 2.0 specification, or worse, understand it
// correctly but add a workaround for Windows, sending U16.16 feedback instead of U10.14 even if
// they're only full-speed, so we always have to use HS-sized buffer. By specification, full-speed
// devices send Q10.14 feedback, and high-speed devices send U16.16 feedback.
constexpr ssize_t kFeedbackSizeUac2 = 4;
// The issue with feedback polling in userspace is that it is hard to time this to be every n
// (micro)frames (we do know in which microframe a device updates its feedback value based on
// bRefresh for UAC1 or bInterval for newer USB Audio Class versions). We would ideally want a
// reduced polling rate - lower than bInterval - because scheduler won't let us wake up this often,
// but when we poll, we want an value that was freshly updated by the device.
// But we can't reduce polling rate as we don't have interval field in usbfs URBs :( the kernel is
// hardcoded to schedule ISO frames at maximum polling rate, and we can't work around this (if we
// try to submit URBs that are too small, the ISO queue will empty and the timeslot will be used for
// something else, which incurs even worse delays upwards of 16 microframes on some controllers).
// As such, we are forced to use the bInterval of the EP as the polling rate, and hence get multiple
// packets at once, but discard all except the latest one. In UAC1 case where bInterval is always
// faster than bRefresh, we can't even time batches to have the last packet as the most up-to-date
// one, because libusb doesn't support non-ASAP transfers. (But given that UAC2 and later aren't
// affected by this limitation, it doesn't seem worth to fix at the moment.)
// Hence, we want queue size to be as small as possible for UAC2 (because every packet is meaningful
// there), but at least bRefresh size for UAC1 to not waste CPU. We also don't want to go far above
// the minimum as too much latency in handling feedback will confuse the device.
static int calculateIso(int bRefresh, int minIsoSlots, libusb_device_handle* device) {
    int isoSlots = minIsoSlots;
    if (bRefresh != 0)
        isoSlots = std::max(isoSlots, (int)pow(2, bRefresh));
    if (libusb_get_device_speed(libusb_get_device(device)) != LIBUSB_SPEED_FULL)
        isoSlots *= 8; // convert unit from frame to microframe (HS) / bus interval (SS)
    return isoSlots;
}
class ExplicitFeedbackTransfer : public Transfer {
private:
    std::atomic<uint32_t>* out;
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    ExplicitFeedbackTransfer(libusb_device_handle *device, char endpoint, std::atomic<uint32_t>* out,
                     int isoSlots, int feedbackSize) : Transfer(isoSlots, device, endpoint,
                                              isoSlots * feedbackSize,
                                              true), out(out) {
        libusb_set_iso_packet_lengths(transfer, feedbackSize);
    }
public:
    // let bRefresh be 0 if device is not UAC1
    ExplicitFeedbackTransfer(int bRefresh, int minIsoSlots, libusb_device_handle *device,
                             char endpoint, std::atomic<uint32_t>* out) :
            ExplicitFeedbackTransfer(device, endpoint, out, calculateIso(bRefresh,
                                                                         minIsoSlots, device),
                             bRefresh != 0 ? kFeedbackSizeUac1 : kFeedbackSizeUac2) {}
    int process(bool inCallback) override {
        if (inCallback) {
            for (int i = transfer->num_iso_packets - 1; i >= 0; i--) {
                if (transfer->iso_packet_desc[i].status == LIBUSB_TRANSFER_COMPLETED) {
                    if (transfer->iso_packet_desc[i].actual_length == 3) {
                        unsigned char *buf = libusb_get_iso_packet_buffer_simple(transfer, i);
                        // U10.14 -> U16.16
                        out->store((((uint32_t) buf[0]) | ((uint32_t) buf[1]) << 8
                            | ((uint32_t) buf[2]) << 16) << 2);
                        break;
                    } else if (transfer->iso_packet_desc[i].actual_length == 4) {
                        void *buf = libusb_get_iso_packet_buffer_simple(transfer, i);
                        out->store(*(uint32_t *) buf);
                        break;
                    }
                }
            }
        }
        return 0;
    }
};

class AudioSource {
    int frameSize;
protected:
    RealtimeTimestamp writeCounter;
public:
    AudioSource(int frameSize) : frameSize(frameSize) {}
    virtual void registerTransfer(void** sourcePrivateOut) {}
    virtual void unregisterTransfer(void* sourcePrivate) {}
    virtual uint32_t readAudio(unsigned char* outBuf, size_t length, void* sourcePrivate) = 0;
    virtual bool allowDmaMemory() = 0;
    virtual ~AudioSource() = default;
    virtual timestamp getWriteCounter() {
        {
            RealtimeTimestamp::ScopedAccess<farbot::ThreadType::nonRealtime> t(writeCounter);

            return *t;
        }
    }
    virtual void resetWriteCounter() {
        {
            RealtimeTimestamp::ScopedAccess<farbot::ThreadType::realtime> t(writeCounter);

            t->frameCount = 0;
            t->nanoTime = 0;
        }
    }

    virtual void completedWrite(timespec tp, size_t bytes, void* sourcePrivate) {
        {
            RealtimeTimestamp::ScopedAccess<farbot::ThreadType::realtime> t(writeCounter);

            t->nanoTime = (uint64_t)((tp.tv_sec * 1000000000LL) + tp.tv_nsec);
            t->frameCount += bytes / frameSize;
        }
    }
};

class Buffer : public AudioSource { // SPSC
public:
    unsigned char* data;
    uint32_t size;
    std::atomic<uint32_t> read;
    std::atomic<uint32_t> write;
    std::atomic<uint32_t> underrunCounter;
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    // this doesn't actually control if we read from it, but just if we're mad about underrun. to
    // stop reading, stop transfer loop / remove buffer from mixer.
    std::atomic<bool> stopped;
    static_assert(std::atomic<bool>::is_always_lock_free);

    Buffer(int bufferSizeFrames, int frameSize) : AudioSource(frameSize) {
        size = bufferSizeFrames * frameSize;
        data = static_cast<unsigned char *>(malloc(size));
    }

    uint32_t readAudio(unsigned char* outBuf, size_t length, void* sourcePrivate) override {
        if (length == 0) {
            return 0;
        }
        uint32_t readCount = read.load();
        uint32_t readMod = readCount % size;
        uint32_t available = write.load() - readCount;
        if (available < length) {
            if (!stopped) {
                // The buffer is in fact not stopping, so count underrun
                underrunCounter += length;
            }
            length = available;
        }
        if (readMod + length <= size) { // normal case, single memcpy
            memcpy(outBuf, data + readMod, length);
        } else { // wrap is in the middle of our transfer
            memcpy(outBuf, data + readMod, size - readMod);
            memcpy(outBuf + (size - readMod), data, length - (size - readMod));
        }
        read.store(readCount + length);
        return length;
    }

    uint32_t writeAudio(unsigned char* inBuf, uint32_t length) {
        if (length == 0) {
            return 0;
        }
        uint32_t writeCount = write.load();
        uint32_t writeMod = writeCount % size;
        uint32_t space = size - (write - read.load());
        if (space == 0) {
            return 0;
        }
        if (space < length)
            length = space;
        if (writeMod + length <= size) { // normal case, single memcpy
            memcpy(data + writeMod, inBuf, length);
        } else { // wrap is in the middle of our transfer
            memcpy(data + writeMod, inBuf, size - writeMod);
            memcpy(data, inBuf + (size - writeMod), length - (size - writeMod));
        }
        write.store(writeCount + length);
        return length;
    }

    bool allowDmaMemory() override {
        return true;
    }

    ~Buffer() override {
        free(data);
    }
};

struct pause_progress {
    unsigned int framesFadedSoFar:24;
    unsigned int framesLeft:24;
    bool fadingIn:1;
};

class MixerBuffer : public Buffer {
public:
    MixerBuffer(int bufferSizeFrames, int frameSize) : Buffer(bufferSizeFrames, frameSize) {}
    std::atomic<pause_progress> pauseProgress;
    static_assert(std::atomic<pause_progress>::is_always_lock_free);
    std::atomic<float> gain;
    static_assert(std::atomic<float>::is_always_lock_free);
};

class __attribute__((packed)) int24_t {
    unsigned char data[3];
public:
    inline int24_t& operator=(int32_t v)
    {
        data[0] = (unsigned char)v;
        data[1] = (unsigned char)(v >> 8);
        data[2] = (unsigned char)(v >> 16);
        return *this;
    }
};
static_assert(sizeof(int24_t) == 3);

struct soft_mixer_private {

};

class SoftMixer : public AudioSource {
protected:
    using BufferList = farbot::RealtimeObject<std::vector<MixerBuffer*>, farbot::RealtimeObjectOptions::nonRealtimeMutatable>;
    static_assert (farbot::is_realtime_move_assignable<std::vector<MixerBuffer*>>::value);
    BufferList list;

    bool allowDmaMemory() override {
        return false;
    }
    void registerTransfer(void **sourcePrivateOut) override {
        *sourcePrivateOut = new soft_mixer_private();
    }
    void unregisterTransfer(void *sourcePrivate) override {
        delete (soft_mixer_private*)sourcePrivate;
    }
    uint32_t readAudio(unsigned char *outBuf, size_t length, void* sourcePrivate) override {
        auto* softMixerPrivate = static_cast<soft_mixer_private *>(sourcePrivate);
        if (length > 0) {
            BufferList::ScopedAccess <farbot::ThreadType::realtime> buffers(list);
            clear(outBuf, length);
            for (MixerBuffer* b : *buffers) {
                uint32_t readCount;
                uint32_t readMod;
                uint32_t available;
                uint32_t readLength;
                // gain = (gainA / gainB) * gainC
                uint32_t gainA = 1;
                uint32_t gainB = 1;
                float gainC = 1.0f;
                uint32_t frameSize = frameSizeBytes();
                while (true) {
                    pause_progress p = b->pauseProgress.load();
                    if (!p.framesLeft && !p.fadingIn)
                        goto skip_this_buffer;
                    else if (p.framesLeft) { // some work to do
                        gainB = p.framesLeft + p.framesFadedSoFar; // total frames
                        if (p.fadingIn)
                            gainA = gainB - p.framesLeft; // frames already faded in
                        else
                            gainA = p.framesLeft; // frames left to fade-out
                        uint32_t fadingNow = length / frameSize;
                        pause_progress tmp = p;
                        tmp.framesFadedSoFar += fadingNow;
                        tmp.framesLeft -= fadingNow;
                        if (!b->pauseProgress.compare_exchange_weak(p, tmp)) {
                            gainA = 1;
                            gainB = 1;
                            continue; // try again
                        }
                        // TODO: it is unknown whether this is lock free with NDK. C++29 will expose
                        //  a way to query this (https://github.com/cplusplus/papers/issues/1915),
                        //  but a manual audit would certainly be worth it.
                        b->pauseProgress.notify_all(); // notify anyone in awaitPause()
                    }
                    break;
                }
                gainC = b->gain.load();
                readCount = b->read.load();
                readMod = readCount % b->size;
                available = b->write.load() - readCount;
                readLength = length;
                if (available < readLength) {
                    if (b->stopped) {
                        // The buffer is in fact not stopping, so count underrun
                        b->underrunCounter++;
                    }
                    readLength = available;
                }
                if (readMod + length <= b->size) { // normal case, single mix
                    mix(outBuf, b->data + readMod, readLength, gainA, gainB, gainC);
                } else { // wrap is in the middle of our transfer
                    mix(outBuf, b->data + readMod, b->size - readMod, gainA, gainB, gainC);
                    mix(outBuf + (b->size - readMod), b->data, readLength - (b->size - readMod),
                        gainA, gainB, gainC);
                }
                b->read.store(readCount + readLength);
                skip_this_buffer:;
            }
        }
        return length;
    }

    virtual size_t frameSizeBytes() = 0;
    virtual void clear(unsigned char* out, size_t length) = 0;
    // gain = (gainA / gainB) * gainC
    virtual void mix(unsigned char* out, unsigned char* src, size_t length, uint32_t gainA,
                     uint32_t gainB, float gainC) = 0;

public:
    // 1 if successfully added, 0 if already in list, -1 if invalid size
    int addBuffer(MixerBuffer* b) {
        if (b->size % frameSizeBytes() != 0)
            return -1;
        {
            BufferList::ScopedAccess<farbot::ThreadType::nonRealtime> buffers(list);
            auto position = std::find(buffers->begin(), buffers->end(), b);
            if (position != buffers->end())
                return false;
            buffers->push_back(b);
            return true;
        }
    }
    void completedWrite(timespec tp, size_t bytes, void* sourcePrivate) override {
        AudioSource::completedWrite(tp, bytes, nullptr);
        auto* softMixerPrivate = static_cast<soft_mixer_private *>(sourcePrivate);
        // TODO: this doesn't consider late-joining buffers that would need to skip this call a few
        //  times in order to be correct. should be fixed
        {
            BufferList::ScopedAccess<farbot::ThreadType::realtime> buffers(list);
            for (MixerBuffer* b : *buffers) {
                b->completedWrite(tp, bytes, nullptr);
            }
        }
    }
    void resetWriteCounter() override {
        AudioSource::resetWriteCounter();
        {
            BufferList::ScopedAccess<farbot::ThreadType::realtime> buffers(list);
            for (MixerBuffer* b : *buffers) {
                b->resetWriteCounter();
            }
        }
    }
    // true if successfully removed and may now be freed, false if wasn't in list
    bool removeBuffer(MixerBuffer* b) {
        {
            BufferList::ScopedAccess<farbot::ThreadType::nonRealtime> buffers(list);
            auto position = std::find(buffers->begin(), buffers->end(), b);
            if (position == buffers->end())
                return false;
            buffers->erase(position);
            return true;
        }
    }
};

template<class output_format, class mixing_format>
class PcmSoftMixer : public SoftMixer {
    size_t channelCount;
    uint32_t readAudio(unsigned char *outBuf, size_t length, void* sourcePrivate) override {
        // TODO: implement dither for both int32->int16 (or uint8_t, that needs it even more) and
        //  int32->int32 with bBitResolution=24 (or any other number actually)
        if constexpr (!std::is_same_v<output_format, mixing_format>) {
            size_t mixBufLength = length * sizeof(mixing_format) / sizeof(output_format);
            unsigned char mixBuf[mixBufLength];
            // we can ignore returned value, we know it's same as mixBufLength
            SoftMixer::readAudio(mixBuf, mixBufLength, sourcePrivate);
            for (int i = 0; i < length / sizeof(output_format); i++) {
                mixing_format* in = (mixing_format*)(mixBuf) + i;
                output_format* out = (output_format*)(outBuf) + i;
                static_assert(false); // TODO: format conversion not yet implemented
            }
            return STREAMING_NO_ERROR;
        } else {
            return SoftMixer::readAudio(outBuf, length, sourcePrivate);
        }
    }
    size_t frameSizeBytes() override {
        return channelCount * sizeof(output_format);
    }
    void clear(unsigned char* out, size_t length) override {
        for (size_t i = 0; i < length / sizeof(mixing_format); i++) {
            mixing_format* entry = (mixing_format*)(out) + i;
            if constexpr (std::is_unsigned_v<mixing_format>) {
                *entry = std::numeric_limits<mixing_format>::max() / 2 + 1;
            } else {
                *entry = 0;
            }
        }
    }
    void mix(unsigned char* out, unsigned char* src, size_t length, uint32_t gainA, uint32_t gainB, float gainC) override {
        for (size_t i = 0; i < length / sizeof(mixing_format); i++) {
            mixing_format* entryOut = (mixing_format*)(out) + i;
            mixing_format* entryIn = (mixing_format*)(src) + i;
            //TODO:need to implement https://www.vttoth.com/CMS/technical-notes/?view=article&id=68
            static_assert(false);
            /*if constexpr (std::is_signed_v<mixing_format>) {
                if (!ckd_add(entryOut, *entryIn, *entryOut)) {
                    *entryOut = (*entryIn >= 0) ? std::numeric_limits<mixing_format>::max()
                                                : std::numeric_limits<mixing_format>::lowest();
                }
            } else if constexpr (std::is_unsigned_v<mixing_format>) {
                mixing_format silence = std::numeric_limits<mixing_format>::max() / 2 + 1;
                static_assert(sizeof(mixing_format) < 4); // not yet implemented, probably unneeded
                int32_t a = (int32_t)*entryIn - silence;
                int32_t b = (int32_t)*entryOut - silence;
                std::make_signed_t<mixing_format> result;
                if (ckd_add(&result, a, b)) {
                    *entryOut = result + silence;
                } else {
                    *entryOut = (a >= 0) ? std::numeric_limits<mixing_format>::max()
                                         : std::numeric_limits<mixing_format>::lowest();
                }
            } else {
                static_assert(false); // makes no sense
            }*/
        }
    }
public:
    PcmSoftMixer(size_t channelCount) : channelCount(channelCount) {}
};

// Implicit feedback boils down to: 1. start capture 2. wait for URB to return 3. send exactly
// as many samples to output 4. repeat.
// Some different designs are possible here: the simplest one is a that a large number of feedback
// EP transfers are queued, and they dequeue a data EP transfer from backlog, fill in the data,
// queue the data EP transfer, and then re-queue themselves (feedback EP transfer). Implementing
// this is annoying, because the completion callbacks of feedback EP and data EP are interleaved
// randomly. However, from single endpoint POV, the callback order is exactly as it was submitted,
// which means the completion callback of a feedback EP would, when using a queue design,
// always dequeue the exact same data EP transfer in every loop iteration. This means we can
// establish this pairing ahead of time, and because libusb callbacks are serialized in _some_ order
// we are completely lock free for common case.
static int calculateNormalSlotCountPerIso(libusb_device_handle *device, int sampleRate) {
    bool f = libusb_get_device_speed(libusb_get_device(device)) == LIBUSB_SPEED_FULL;
    return (sampleRate * (f ? 1000 : 125) / 1000000);
}
class ImplicitFeedbackTransfer : public Transfer {
private:
    libusb_transfer* transferData;
    void* dma = nullptr;
    std::atomic<int> waitingCount;
    static_assert(std::atomic<int>::is_always_lock_free);
    int frameSizeIn;
    int frameSizeOut;
    int sampleRateOut;
    int sampleRateIn;
    uint32_t* u16Accumulator;
    AudioSource* b;
    void* sourcePrivate = nullptr;

    ImplicitFeedbackTransfer(libusb_device_handle *device, char endpoint, char endpointData,
                             int isoSlots, int sampleRateIn, int sampleRateOut, int dataSizeFrames,
                             int feedbackSizeFrames, int frameSizeIn, int frameSizeOut, uint32_t*
    u16Accumulator, AudioSource* b) : Transfer
    (isoSlots, device, endpoint, isoSlots * feedbackSizeFrames * frameSizeIn,
     true),
                                      frameSizeIn(frameSizeIn), frameSizeOut(frameSizeOut), sampleRateIn(sampleRateIn),
                                      sampleRateOut(sampleRateOut), u16Accumulator(u16Accumulator), b(b) {
        int dataSize = dataSizeFrames * frameSizeOut;
        libusb_set_iso_packet_lengths(transfer, feedbackSizeFrames * frameSizeIn);
        transferData = libusb_alloc_transfer(isoSlots);
        transferData->num_iso_packets = 0;
        transferData->dev_handle = device;
        transferData->flags = 0;
        transferData->type = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
        transferData->timeout = 60000; // placeholder, TBD
        transferData->endpoint = endpointData;
        transferData->length = isoSlots * dataSize;
        if (b->allowDmaMemory()) {
            dma = libusb_dev_mem_alloc(device, transferData->length);
        }
        transferData->buffer = static_cast<unsigned char *>(dma != nullptr ? dma :
                malloc(transferData->length));
        transferData->user_data = this;
        transferData->callback = transfer_callback_wrapper;
        libusb_set_iso_packet_lengths(transferData, dataSize);
        b->registerTransfer(&sourcePrivate);
    }

    int doSubmit() override {
        if (transferData->num_iso_packets > 0) {
            waitingCount += 2;
            auto rc = (libusb_error) libusb_submit_transfer(transferData);
            if (rc != LIBUSB_SUCCESS) {
                transferData->num_iso_packets = 0; // mark as not ready to send
                waitingCount -= 2;
                return rc;
            }
            rc = (libusb_error) libusb_submit_transfer(transfer);
            if (rc != LIBUSB_SUCCESS) {
                if (--waitingCount == 0)
                    return rc; // can set idle, the other callback is already done
                return -rc; // positive error -> wait for callback before setting idle
            }
            return rc;
        } else {
            waitingCount += 1;
            auto rc = (libusb_error) libusb_submit_transfer(transfer);
            if (rc != LIBUSB_SUCCESS) {
                waitingCount -= 1;
                return rc;
            }
            return rc;
        }
    }

    void doCancel() override {
        libusb_cancel_transfer(transfer);
        libusb_cancel_transfer(transferData);
    }

    void callback(libusb_transfer *theTransfer) override {
        if (theTransfer == transferData) {
            timespec tp{};
            //TODO: can we get time from libusb or kernel, or something...
            clock_gettime(CLOCK_MONOTONIC, &tp);
            b->completedWrite(tp, theTransfer->length, sourcePrivate);
        }
        if (--waitingCount > 0)
            return;
        transferData->num_iso_packets *= -1; // mark as not ready to send
        Transfer::callback(theTransfer);
    }

public:
    // in and out sample rate must be derived from the same clock, but one or both of these may
    // still be subjected to clock division, hence they may differ.
    ImplicitFeedbackTransfer(libusb_device_handle *device, char endpoint, char endpointData,
                             int isoSlots, int sampleRateIn, int sampleRateOut, int frameSizeIn,
                             int frameSizeOut, uint32_t* u16Accumulator, AudioSource* b
    ) : ImplicitFeedbackTransfer(
            device, endpoint, endpointData, isoSlots, sampleRateIn, sampleRateOut,
            calculateNormalSlotCountPerIso(device, sampleRateOut) + 1,
            calculateNormalSlotCountPerIso(device, sampleRateIn) + 1,
            frameSizeIn, frameSizeOut, u16Accumulator, b) {}

    ~ImplicitFeedbackTransfer() override {
        awaitStop();
        if (dma != nullptr)
            libusb_dev_mem_free(transferData->dev_handle, transferData->buffer,
                                transferData->length);
        else
            free(transferData->buffer);
        b->unregisterTransfer(sourcePrivate);
        libusb_free_transfer(transferData);
        Transfer::~Transfer();
    }

    int process(bool inCallback) override {
        if (inCallback) {
            int j = 0;
            unsigned int totalLengthToSend = 0;
            for (int i = 0; i < transfer->num_iso_packets; i++) {
                uint32_t outputFramesU16;
                if (transfer->iso_packet_desc[i].status == LIBUSB_TRANSFER_COMPLETED &&
                        transfer->iso_packet_desc[i].actual_length > 0) {
                    // one packet must have integer number of audio frames
                    uint32_t inFrames = transfer->iso_packet_desc[i].actual_length / frameSizeIn;
                    outputFramesU16 = ((uint64_t)inFrames * sampleRateOut << 16) / sampleRateIn;
                } else {
                    // guesstimate at least somewhat, as some amount of errors is expected with ISO
                    uint32_t usbFrameDuration = libusb_get_device_speed(libusb_get_device(
                            transfer->dev_handle)) == LIBUSB_SPEED_FULL ? 1000 : 125;
                    outputFramesU16 = ((uint64_t)sampleRateOut * usbFrameDuration << 16) / 1000000;
                }
                uint32_t outBytes = (uint32_t) u16Accumulate(
                        u16Accumulator, outputFramesU16) * frameSizeOut;
                transferData->iso_packet_desc[j++].length = outBytes;
                totalLengthToSend += outBytes;
            }
            transferData->num_iso_packets = j;
            transferData->length = (int)totalLengthToSend;
            uint32_t actualLength = b->readAudio(transferData->buffer, totalLengthToSend, sourcePrivate);
            // sanity check, but this never happens
            if (actualLength > totalLengthToSend || (actualLength % frameSizeOut) != 0) {
                return STREAMING_ERROR_INVALID_ARGUMENT;
            }
            if (actualLength < totalLengthToSend) {
                // We can't do short transfers as it would offset our perception of time and USB
                // device's perception of time permanently if the host(!) controller's ISO queue
                // empties. UAC spec defines zero-length packets as valid way to signal there's no more
                // data available right now, so let's make use of that.
                int i;
                for (i = j - 1; i >= 0 && actualLength < totalLengthToSend; i--) {
                    while (actualLength < totalLengthToSend &&
                            transferData->iso_packet_desc[i].length > 0) {
                        transferData->iso_packet_desc[i].length -= frameSizeOut;
                        totalLengthToSend -= frameSizeOut;
                    }
                }
                transferData->length = (int) totalLengthToSend;
            }
        }
        return STREAMING_NO_ERROR;
    }
};

class AudioTransfer : public Transfer {
    uint32_t* u16Accumulator;
    int frameSize;
    int sampleRate;
    AudioSource* b;
    void* sourcePrivate = nullptr;
protected:
    virtual uint32_t getFeedbackOrDefault() {
        uint32_t usbFrameDuration = libusb_get_device_speed(libusb_get_device(
                transfer->dev_handle)) == LIBUSB_SPEED_FULL ? 1000 : 125;
        return ((uint64_t)sampleRate * usbFrameDuration << 16) / 1000000;
    }
public:
    AudioTransfer(int isoSlots, libusb_device_handle *device, char endpoint,
                  int maxIsoPacketSizeBytes, int frameSize, int sampleRate,
                  uint32_t* u16Accumulator, AudioSource* b)
            : Transfer(isoSlots, device, endpoint, maxIsoPacketSizeBytes * isoSlots,
                       b->allowDmaMemory()),
              frameSize(frameSize), sampleRate(sampleRate), u16Accumulator(u16Accumulator), b(b) {
        b->registerTransfer(&sourcePrivate);
    }

    void callback(libusb_transfer *theTransfer) override {
        timespec tp{};
        //TODO: can we get time from libusb or kernel, or something...
        clock_gettime(CLOCK_MONOTONIC, &tp);
        b->completedWrite(tp, theTransfer->length, sourcePrivate);
        Transfer::callback(theTransfer);
    }

    int process(bool inCallback) override {
        uint32_t feedback = getFeedbackOrDefault();
        unsigned int totalLengthToSend = 0;
        int maxSize = bufferSize / transfer->num_iso_packets;
        for (int i = 0; i < transfer->num_iso_packets; i++) {
            uint32_t outBytes = u16Accumulate(u16Accumulator, feedback) * frameSize;
            if (outBytes > maxSize)
                outBytes = maxSize;
            transfer->iso_packet_desc[i].length = outBytes;
            totalLengthToSend += outBytes;
        }
        uint32_t actualLength = b->readAudio(transfer->buffer, totalLengthToSend, sourcePrivate);
        if (totalLengthToSend > 0 && actualLength == 0) {
            return STREAMING_ERROR_UNDERFLOW;
        }
        // sanity check, but this never happens
        if (actualLength > totalLengthToSend || (actualLength % frameSize) != 0) {
            return STREAMING_ERROR_INVALID_ARGUMENT;
        }
        if (actualLength < totalLengthToSend) {
            // We can't do short transfers as it would offset our perception of time and USB
            // device's perception of time permanently if the host(!) controller's ISO queue
            // empties. UAC spec defines zero-length packets as valid way to signal there's no more
            // data available right now, so let's make use of that.
            for (int i = transfer->num_iso_packets - 1; i >= 0 && actualLength < totalLengthToSend; i--) {
                while (actualLength < totalLengthToSend &&
                        transfer->iso_packet_desc[i].length > 0) {
                    transfer->iso_packet_desc[i].length -= frameSize;
                    totalLengthToSend -= frameSize;
                }
            }
            transfer->length = (int) totalLengthToSend;
        }
        return STREAMING_NO_ERROR;
    }

    ~AudioTransfer() override {
        b->unregisterTransfer(sourcePrivate);
    }
};

class FeedbackAudioTransfer : public AudioTransfer {
    std::atomic<uint32_t>* feedbackIn;
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    uint32_t getFeedbackOrDefault() override {
        uint32_t feedback = feedbackIn->load();
        if (feedback == 0) {
            // guesstimate at least somewhat, as some amount of errors is expected with ISO
            feedback = AudioTransfer::getFeedbackOrDefault();
        }
        return feedback;
    }
public:
    FeedbackAudioTransfer(int isoSlots, libusb_device_handle *device, char endpoint,
                  int maxIsoPacketSizeBytes, int frameSize, int sampleRate,
                  std::atomic<uint32_t>* feedbackIn, uint32_t* u16Accumulator,
                  AudioSource* b) : AudioTransfer(
            isoSlots, device, endpoint, maxIsoPacketSizeBytes, frameSize, sampleRate,
            u16Accumulator, b), feedbackIn(feedbackIn) {}
};

class Streaming {
public:
    AudioSource* b;
    Streaming(AudioSource* b) : b(b) {}
    // To keep streaming running:
    // 1. call start(whether you intend to empty the buffer)
    // 2. if error is returned: handle error (for example, LIBUSB_ERROR_NO_DEVICE -> call stop),
    //    and if wanting to continue, go to step 1. if LIBUSB_SUCCESS is returned, go to step 3.
    // 3. wait 100ms, then go to step 1
    virtual int start(bool empty) = 0;
    virtual void stop() = 0;
    virtual ~Streaming() = default;
};

class ExplicitAsyncFeedbackStreaming : public Streaming {
    std::atomic<uint32_t> feedback;
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    uint32_t accumulator = 0;
    std::vector<ExplicitFeedbackTransfer*> feedbackTransfers;
    std::vector<AudioTransfer*> audioTransfers;

public:
    ExplicitAsyncFeedbackStreaming(libusb_device_handle *device, char endpointData, char endpointFb,
                                   AudioSource* audioSource, int audioIsoSlots,
                                   int audioTransferCount, int audioFrameSize, int audioSampleRate,
                                   int maxIsoPacketSizeBytes, int feedbackTransferCount,
                                   int bRefresh, int feedbackMinIsoSlots) : Streaming(audioSource) {
        for (int i = 0; i < feedbackTransferCount; i++) {
            feedbackTransfers.push_back(new ExplicitFeedbackTransfer(
                    bRefresh, feedbackMinIsoSlots, device, endpointFb,
                    &feedback));
        }
        for (int i = 0; i < audioTransferCount; i++) {
            audioTransfers.push_back(new FeedbackAudioTransfer(
                    audioIsoSlots, device, endpointData,
                    maxIsoPacketSizeBytes, audioFrameSize,
                    audioSampleRate, &feedback, &accumulator, b));
        }
    }

    int start(bool empty) override {
        bool allUnderrun = false;
        bool oneUnderrun = false;
        for (auto & audioTransfer : audioTransfers) {
            bool underrun = audioTransfer->dequeueUnderflow();
            oneUnderrun = oneUnderrun || underrun;
            allUnderrun = allUnderrun && underrun;
        }
        if (allUnderrun)
            return STREAMING_ERROR_GLOBAL_UNDERFLOW;
        else if (oneUnderrun)
            return STREAMING_ERROR_UNDERFLOW;
        for (auto & audioTransfer : audioTransfers) {
            int error = audioTransfer->dequeueError();
            if (error != LIBUSB_SUCCESS)
                return error;
        }
        for (auto & feedbackTransfer : feedbackTransfers) {
            int error = feedbackTransfer->dequeueError();
            if (error != LIBUSB_SUCCESS)
                return error;
        }
        bool allIdle = false;
        bool oneIdle = false;
        for (auto & audioTransfer : audioTransfers) {
            bool idle = audioTransfer->isIdle();
            oneIdle = oneIdle || idle;
            allIdle = allIdle && idle;
        }
        if (!allIdle && oneIdle) {
            // to ensure we read data from ring buffer in proper order and don't cause races, stop
            // transfers and restart them properly
            if (empty) {
                for (auto & audioTransfer : audioTransfers) {
                    // first let the audio transfers stop themselves
                    audioTransfer->awaitStop();
                }
            }
            stop();
        }
        feedback.store(0);
        std::vector<Transfer*> transfersToStart;
        for (auto & audioTransfer : audioTransfers) {
            if (audioTransfer->start1())
                transfersToStart.push_back(audioTransfer);
        }
        for (auto & feedbackTransfer : feedbackTransfers) {
            if (feedbackTransfer->start1())
                transfersToStart.push_back(feedbackTransfer);
        }
        for (auto & transfer : transfersToStart) {
            transfer->start2();
        }
        for (auto & audioTransfer : audioTransfers) {
            int error = audioTransfer->dequeueError();
            if (error != LIBUSB_SUCCESS)
                return error;
        }
        for (auto & feedbackTransfer : feedbackTransfers) {
            int error = feedbackTransfer->dequeueError();
            if (error != LIBUSB_SUCCESS)
                return error;
        }
        return LIBUSB_SUCCESS;
    }

    void stop() override {
        for (auto & audioTransfer : audioTransfers) {
            audioTransfer->cancel();
        }
        for (auto & feedbackTransfer : feedbackTransfers) {
            feedbackTransfer->cancel();
        }
        for (auto & audioTransfer : audioTransfers) {
            audioTransfer->awaitStop();
            audioTransfer->dequeueError(); // drop error if any
        }
        for (auto & feedbackTransfer : feedbackTransfers) {
            feedbackTransfer->awaitStop();
            feedbackTransfer->dequeueError(); // drop error if any
        }
    }
    ~ExplicitAsyncFeedbackStreaming() override {
        for (auto & audioTransfer : audioTransfers) {
            delete audioTransfer;
        }
        for (auto & feedbackTransfer : feedbackTransfers) {
            delete feedbackTransfer;
        }
    }
};

class SyncStreaming : public Streaming {
    uint32_t accumulator = 0;
    std::vector<AudioTransfer*> audioTransfers;

public:
    SyncStreaming(libusb_device_handle *device, char endpointData,
                          AudioSource* audioSource, int audioIsoSlots,
                          int audioTransferCount, int audioFrameSize, int audioSampleRate)
                          : Streaming(audioSource) {
        int maxIsoPacketSizeBytes = audioFrameSize * (calculateNormalSlotCountPerIso(
                device, audioSampleRate) + 1);
        for (int i = 0; i < audioTransferCount; i++) {
            audioTransfers.push_back(new AudioTransfer(
                    audioIsoSlots, device, endpointData,
                    maxIsoPacketSizeBytes, audioFrameSize,
                    audioSampleRate, &accumulator, b));
        }
    }

    int start(bool empty) override {
        bool allUnderrun = false;
        bool oneUnderrun = false;
        for (auto & audioTransfer : audioTransfers) {
            bool underrun = audioTransfer->dequeueUnderflow();
            oneUnderrun = oneUnderrun || underrun;
            allUnderrun = allUnderrun && underrun;
        }
        if (allUnderrun)
            return STREAMING_ERROR_GLOBAL_UNDERFLOW;
        else if (oneUnderrun)
            return STREAMING_ERROR_UNDERFLOW;
        for (auto & audioTransfer : audioTransfers) {
            int error = audioTransfer->dequeueError();
            if (error != LIBUSB_SUCCESS)
                return error;
        }
        bool allIdle = false;
        bool oneIdle = false;
        for (auto & audioTransfer : audioTransfers) {
            bool idle = audioTransfer->isIdle();
            oneIdle = oneIdle || idle;
            allIdle = allIdle && idle;
        }
        if (!allIdle && oneIdle) {
            // to ensure we read data from ring buffer in proper order and don't cause races, stop
            // transfers and restart them properly
            if (empty) {
                for (auto & audioTransfer : audioTransfers) {
                    // first let the audio transfers stop themselves
                    audioTransfer->awaitStop();
                }
            }
            stop();
        }
        std::vector<Transfer*> transfersToStart;
        for (auto & audioTransfer : audioTransfers) {
            if (audioTransfer->start1())
                transfersToStart.push_back(audioTransfer);
        }
        for (auto & transfer : transfersToStart) {
            transfer->start2();
        }
        for (auto & audioTransfer : audioTransfers) {
            int error = audioTransfer->dequeueError();
            if (error != LIBUSB_SUCCESS)
                return error;
        }

        return LIBUSB_SUCCESS;
    }

    void stop() override {
        for (auto & audioTransfer : audioTransfers) {
            audioTransfer->cancel();
        }
        for (auto & audioTransfer : audioTransfers) {
            audioTransfer->awaitStop();
            audioTransfer->dequeueError(); // drop error if any
        }
    }
    ~SyncStreaming() override {
        for (auto & audioTransfer : audioTransfers) {
            delete audioTransfer;
        }
    }
};

class ImplicitAsyncFeedbackStreaming : public Streaming {
    std::vector<ImplicitFeedbackTransfer*> transfers;
    uint32_t u16Accumulator = 0; // only accessed on callback thread

public:
    ImplicitAsyncFeedbackStreaming(libusb_device_handle *device, char endpointData, char endpointFb,
                                   AudioSource* audioSource, int isoSlots, int transferQueueSize,
                                   int audioFrameSize, int audioSampleRate,
                                   int feedbackFrameSize, int feedbackSampleRate
                                   ) : Streaming(audioSource) {
        for (int i = 0; i < transferQueueSize; i++) {
            transfers.push_back(new ImplicitFeedbackTransfer(
                    device, endpointFb, endpointData, isoSlots,
                    audioSampleRate, feedbackSampleRate,
                    feedbackFrameSize, audioFrameSize, &u16Accumulator, b));
        }
    }
    int start(bool empty) override {
        bool allUnderrun = false;
        bool oneUnderrun = false;
        for (auto & transfer : transfers) {
            bool underrun = transfer->dequeueUnderflow();
            oneUnderrun = oneUnderrun || underrun;
            allUnderrun = allUnderrun && underrun;
        }
        if (allUnderrun)
            return STREAMING_ERROR_GLOBAL_UNDERFLOW;
        else if (oneUnderrun)
            return STREAMING_ERROR_UNDERFLOW;
        for (auto & transfer : transfers) {
            int error = transfer->dequeueError();
            if (error != LIBUSB_SUCCESS)
                return error;
        }
        bool allIdle = false;
        bool oneIdle = false;
        for (auto & transfer : transfers) {
            bool idle = transfer->isIdle();
            oneIdle = oneIdle || idle;
            allIdle = allIdle && idle;
        }
        if (!allIdle && oneIdle) {
            // to ensure we read data from ring buffer in proper order and don't cause races, stop
            // transfers and restart them properly
            if (empty) {
                for (auto &transfer: transfers) {
                    // first let the audio transfers stop themselves
                    transfer->awaitStop();
                }
            }
            stop();
        }
        std::vector<Transfer*> transfersToStart;
        for (auto & transfer : transfers) {
            if (transfer->start1())
                transfersToStart.push_back(transfer);
        }
        for (auto & transfer : transfersToStart) {
            transfer->start2();
        }
        for (auto & transfer : transfers) {
            int error = transfer->dequeueError();
            if (error != LIBUSB_SUCCESS)
                return error;
        }
        return LIBUSB_SUCCESS;
    }

    void stop() override {
        for (auto & transfer : transfers) {
            transfer->cancel();
        }
        for (auto & transfer : transfers) {
            transfer->awaitStop();
        }
    }

    ~ImplicitAsyncFeedbackStreaming() override {
        for (auto & transfer : transfers) {
            delete transfer;
        }
    }
};

extern "C"
JNIEXPORT jint JNICALL
Java_org_nift4_gramophone_hificore_Streaming_nativeStart(JNIEnv *env, jobject thiz,
                                                                         jlong ptr, jboolean empty) {
    auto* pStreaming = (Streaming*) ptr;
    return pStreaming->start(empty);
}

extern "C"
JNIEXPORT void JNICALL
Java_org_nift4_gramophone_hificore_Streaming_nativeStop(JNIEnv *env, jobject thiz,
                                                                        jlong ptr) {
    auto* pStreaming = (Streaming*) ptr;
    pStreaming->stop();
}

extern "C"
JNIEXPORT void JNICALL
Java_org_nift4_gramophone_hificore_Streaming_nativeRelease(JNIEnv *env, jobject thiz,
                                                                           jlong ptr,
                                                           jboolean auto_release_native_buf) {
    auto* pStreaming = (Streaming*) ptr;
    if (auto_release_native_buf)
        delete pStreaming->b;
    delete pStreaming;
}

extern "C"
JNIEXPORT jlong JNICALL
Java_org_nift4_gramophone_hificore_Streaming_00024Companion_nativeCreateExplicit(
        JNIEnv *env, jobject thiz, jlong native_object,
        jbyte endpoint_data, jbyte endpoint_fb, jlong src, jint iso_slots,
        jint transfer_queue_size, jint audio_frame_size, jint audio_sample_rate,
        jint max_iso_packet_size_bytes, jint feedback_transfer_count, jint b_refresh,
        jint feedback_min_iso_slots) {
    auto* device = (libusb_device_handle*) native_object;
    return (jlong) new ExplicitAsyncFeedbackStreaming(
            device, endpoint_data, endpoint_fb, (AudioSource*) src,
                    iso_slots, transfer_queue_size,
                    audio_frame_size, audio_sample_rate,
                    max_iso_packet_size_bytes,
                    feedback_transfer_count, b_refresh,
                    feedback_min_iso_slots);
}

extern "C"
JNIEXPORT jlong JNICALL
Java_org_nift4_gramophone_hificore_Streaming_00024Companion_nativeCreateImplicit(
        JNIEnv *env, jobject thiz, jlong native_object,
        jbyte endpoint_data, jbyte endpoint_fb, jlong src, jint iso_slots,
        jint transfer_queue_size, jint audio_frame_size, jint audio_sample_rate,
        jint feedback_frame_size, jint feedback_sample_rate) {
    auto* device = (libusb_device_handle*) native_object;
    return (jlong) new ImplicitAsyncFeedbackStreaming(
            device, endpoint_data, endpoint_fb, (AudioSource*) src,
                    iso_slots, transfer_queue_size,
                    audio_frame_size, audio_sample_rate,
                    feedback_frame_size,feedback_sample_rate);
}

extern "C"
JNIEXPORT jlong JNICALL
Java_org_nift4_gramophone_hificore_Streaming_00024Companion_nativeCreateSync(
        JNIEnv *env, jobject thiz, jlong native_object,
        jbyte endpoint_data, jlong src, jint iso_slots,
        jint transfer_queue_size, jint audio_frame_size, jint audio_sample_rate) {
    auto* device = (libusb_device_handle*) native_object;
    return (jlong) new SyncStreaming(
            device, endpoint_data, (AudioSource*) src,
            iso_slots, transfer_queue_size,
            audio_frame_size, audio_sample_rate);
}

extern "C"
JNIEXPORT jlong JNICALL
Java_org_nift4_gramophone_hificore_Buffer_nativeCreateBuffer(JNIEnv *env,
                                                             jobject thiz,
                                                             jboolean forMixer,
                                                             jint buffer_size_frames,
                                                             jint frame_size) {
    return (jlong)(forMixer ? new MixerBuffer(buffer_size_frames, frame_size) :
        new Buffer(buffer_size_frames, frame_size));
}

extern "C"
JNIEXPORT void JNICALL
Java_org_nift4_gramophone_hificore_Buffer_nativeFlush(JNIEnv *env, jobject thiz,
                                                           jlong ptr) {
    auto* b = (Buffer*) ptr;
    // allow to keep correlating timestamps and write counter
    uint32_t writtenFrameCounter = b->getWriteCounter().frameCount;
    b->read.store(writtenFrameCounter);
    b->write.store(writtenFrameCounter);
    auto* mb = dynamic_cast<MixerBuffer*>(b);
    if (mb != nullptr) {
        pause_progress p = mb->pauseProgress.load();
        p.framesLeft = 0; // ensure to cut off any running fade too, and just keep play/pause state
        mb->pauseProgress.store(p);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_org_nift4_gramophone_hificore_MixedAudioOutput_nativeAwaitPause(JNIEnv *env, jobject thiz,
                                                                jlong ptr) {
    auto* b = (MixerBuffer*) ptr;
    while (true) {
        pause_progress p = b->pauseProgress.load();
        if (!p.framesLeft || p.fadingIn)
            return;
        b->pauseProgress.wait(p);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_org_nift4_gramophone_hificore_Buffer_nativeGetWriteCounter(JNIEnv *env, jobject thiz,
                                                                jlong ptr, jlongArray out) {
    auto* b = (Buffer*) ptr;
    timestamp ts = b->getWriteCounter();
    env->SetLongArrayRegion(out, 0, 2, (jlong*)&ts);
}

extern "C"
JNIEXPORT void JNICALL
Java_org_nift4_gramophone_hificore_Buffer_nativeResetWriteCounter(JNIEnv *env, jobject thiz,
                                                                jlong ptr) {
    auto* b = (Buffer*) ptr;
    // resetWriteCounter is actually called on nonRealtime thread, but while the realtime
    // thread isn't running, so we can impersonate it. (I know, it's evil)
    b->resetWriteCounter();
}

extern "C"
JNIEXPORT void JNICALL
Java_org_nift4_gramophone_hificore_MixedAudioOutput_nativeSetGain(JNIEnv *env, jobject thiz,
                                                                  jlong ptr, jfloat volume) {
    auto* b = (MixerBuffer*) ptr;
    b->gain.store(volume);
}

extern "C"
JNIEXPORT void JNICALL
Java_org_nift4_gramophone_hificore_MixedAudioOutput_nativeSetFramesUntilPaused(JNIEnv *env,
                                                                          jobject thiz,
                                                                          jlong ptr, jint value) {
    auto* b = (MixerBuffer*) ptr;
    uint32_t newTotal = value >= 0 ? value : -(value + 1);
    bool newIsFadeIn = value < 0;
    while (true) {
        pause_progress old = b->pauseProgress.load();
        pause_progress p = old;
        if (p.fadingIn == newIsFadeIn)
            return;
        if (!old.framesLeft) { // no fade running, just start a new one
            p.framesLeft = newTotal;
            p.framesFadedSoFar = 0;
        } else { // there is a fade running, let's reverse it for smooth transition
            // if old: soFar=25/left=75 fade in -> volume 25%, then p: soFar=75/left=25 fade out
            // if old: soFar=25/left=75 fade out -> volume 75%, then p: soFar=75/left=25 fade in
            uint32_t oldTotal = old.framesLeft + old.framesFadedSoFar;
            p.framesFadedSoFar = old.framesLeft * newTotal / oldTotal;
            p.framesLeft = old.framesFadedSoFar * newTotal / oldTotal;
        }
        p.fadingIn = newIsFadeIn;
        if (b->pauseProgress.compare_exchange_weak(old, p)) {
            return;
        }
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_org_nift4_gramophone_hificore_Buffer_nativeStop(JNIEnv *env, jobject thiz,
                                                                 jlong ptr, jboolean stopped) {
    auto* b = (Buffer*) ptr;
    b->stopped.store(stopped);
}

extern "C"
JNIEXPORT jint JNICALL
Java_org_nift4_gramophone_hificore_Buffer_nativeGetUnderrunCount(JNIEnv *env, jobject thiz,
                                                                 jlong ptr) {
    auto* b = (Buffer*) ptr;
    return (jint)b->underrunCounter.load();
}

extern "C"
JNIEXPORT jint JNICALL
Java_org_nift4_gramophone_hificore_Buffer_nativeWrite(JNIEnv *env, jobject thiz,
                                                      jlong ptr, jobject buf,
                                                      jint position, jint remaining) {
    auto* b = (Buffer*) ptr;
    auto* inBuf = static_cast<unsigned char *>(env->GetDirectBufferAddress(buf));
    return (jint)b->writeAudio(inBuf + position, remaining);
}

extern "C"
JNIEXPORT void JNICALL
Java_org_nift4_gramophone_hificore_Buffer_nativeRelease(JNIEnv *env,
                                                             jobject thiz,
                                                        jlong ptr) {
    delete (Buffer*)ptr;
}

extern "C"
JNIEXPORT jlong JNICALL
Java_org_nift4_gramophone_hificore_SoftMixedStreaming_00024Companion_nativeCreateSoftMixer(
        JNIEnv *env, jobject thiz) {
    // TODO: implement nativeCreateSoftMixer()
    return 0;
}

extern "C"
JNIEXPORT jint JNICALL
Java_org_nift4_gramophone_hificore_SoftMixedStreaming_nativeAddBuffer(JNIEnv *env, jobject thiz,
                                                                      jlong ptr, jlong buf) {
    auto* pStreaming = (Streaming*) ptr;
    return ((SoftMixer*)pStreaming->b)->addBuffer((MixerBuffer*)buf);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_org_nift4_gramophone_hificore_SoftMixedStreaming_nativeRemoveBuffer(JNIEnv *env, jobject thiz,
                                                                         jlong ptr, jlong buf) {
    auto* pStreaming = (Streaming*) ptr;
    return ((SoftMixer*)pStreaming->b)->removeBuffer((MixerBuffer*)buf);
}