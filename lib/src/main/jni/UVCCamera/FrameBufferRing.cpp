/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: FrameBufferRing.cpp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#define LOG_TAG "libUVCCamera"

// Platform-specific includes
// When testing on host (UVCCAMERA_TESTING defined), use mock implementations
// and skip Android-specific headers like JNI
#ifdef UVCCAMERA_TESTING
    // Mocks are already included via FrameBufferRing.h
    // Include time.h for clock_gettime (available on all POSIX systems)
    #include <time.h>
#else
    #include "localdefines.h"
    #include "utilbase.h"

    #include <android/hardware_buffer.h>
    #include <poll.h>
    #include <time.h>
    #include <errno.h>
    #include <unistd.h>
#endif

#include "FrameBufferRing.h"

FrameBufferRing::FrameBufferRing()
    : mWriteIndex(0),
      mReadIndex(0),
      mLatestCompleted(-1),
      mFrameCounter(0),
      mWidth(0),
      mHeight(0),
      mFormat(0),
      mAllocated(false) {
    for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
        mBuffers[i] = nullptr;
        mMetadata[i].reset();
    }
}

FrameBufferRing::~FrameBufferRing() {
    destroy();
}

int FrameBufferRing::allocate(uint32_t width, uint32_t height, uint32_t format) {
    if (mAllocated) {
        destroy();
    }

    AHardwareBuffer_Desc desc = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = format,
        .usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
        .stride = 0,
        .rfu0 = 0,
        .rfu1 = 0,
    };

    for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
        int result = AHardwareBuffer_allocate(&desc, &mBuffers[i]);
        if (result != 0 || !mBuffers[i]) {
            LOGE("Failed to allocate AHardwareBuffer %d, error: %d", i, result);
            destroy();
            return result;
        }
        mMetadata[i].reset();
    }

    mWidth = width;
    mHeight = height;
    mFormat = format;
    mAllocated = true;

    // Initialize eventfd for SPSC queue signaling
    if (initEventFd() != 0) {
        LOGW("Failed to initialize eventfd, falling back to polling");
        // Non-fatal: waitForSignal() has a fallback for mEventFd < 0
    }

    // Reset telemetry and set stream parameters
    mTelemetry.reset();
    mTelemetry.negotiatedWidth = width;
    mTelemetry.negotiatedHeight = height;
    mTelemetry.markStreamStart();

    LOGI("FrameBufferRing allocated: %dx%d (format: 0x%x)", width, height, format);
    return 0;
}

void FrameBufferRing::destroy() {
    mAllocated = false;
    mLatestCompleted.store(-1, std::memory_order_relaxed);

    // Close eventfd and reset SPSC queue
    closeEventFd();

    for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
        // Close any pending fence fds before releasing buffers (bidirectional)
        if (mMetadata[i].acquireFenceFd >= 0) {
            close(mMetadata[i].acquireFenceFd);
        }
        if (mMetadata[i].gpuReleaseFenceFd >= 0) {
            close(mMetadata[i].gpuReleaseFenceFd);
        }
        mMetadata[i].reset();

        if (mBuffers[i]) {
            AHardwareBuffer_release(mBuffers[i]);
            mBuffers[i] = nullptr;
        }
    }

    mWriteIndex.store(0, std::memory_order_relaxed);
    mReadIndex.store(0, std::memory_order_relaxed);
    mFrameCounter = 0;
    mWidth = 0;
    mHeight = 0;
    mFormat = 0;
    mTelemetry.reset();
}

void* FrameBufferRing::lockWriteBuffer(int32_t* outStrideBytes) {
    if (UNLIKELY(!mAllocated)) {
        return nullptr;
    }

    int idx = mWriteIndex.load(std::memory_order_relaxed);
    FrameSlotMetadata& slot = mMetadata[idx];

    // Update telemetry: current write slot and state
    mTelemetry.currentWriteSlot.store(idx, std::memory_order_relaxed);
    mTelemetry.setSlotState(idx, SlotState::WRITING);

    // BIDIRECTIONAL FENCE: Wait for GPU to finish reading before we write
    if (slot.gpuReleaseFenceFd >= 0) {
        mTelemetry.fencePending.store(true, std::memory_order_relaxed);

        // Wait up to 33ms (1 frame @ 30fps) for GPU to complete
        // Use poll() as a portable sync fence wait
        struct pollfd pfd = {
            .fd = slot.gpuReleaseFenceFd,
            .events = POLLIN,
            .revents = 0
        };

        int64_t waitStartNs = StreamTelemetry::getCurrentTimeNs();
        int waitResult = poll(&pfd, 1, 33);
        int64_t waitEndNs = StreamTelemetry::getCurrentTimeNs();

        // Record fence wait time for telemetry
        mTelemetry.recordFenceWait(waitEndNs - waitStartNs);
        mTelemetry.fencePending.store(false, std::memory_order_relaxed);

        // Always close the fence after wait (success, timeout, or error)
        close(slot.gpuReleaseFenceFd);
        slot.gpuReleaseFenceFd = -1;

        if (waitResult <= 0) {
            // Timeout or error - GPU is too slow
            // We proceed anyway for smooth streaming; GPU may see partial data
            LOGW("GPU release fence wait timeout/error (%d), proceeding (potential GPU contention)", waitResult);
            // Note: This is not a "drop" - frame will still be written
        }
    } else if (slot.isLockedByConsumer) {
        // Non-fence fallback: consumer is still holding this buffer
        // Drop the frame rather than corrupt consumer's view
        LOGW("Buffer %d still locked by consumer (non-fence path), dropping frame", idx);
        mTelemetry.framesDropped.fetch_add(1, std::memory_order_relaxed);
        mTelemetry.producerStalls.fetch_add(1, std::memory_order_relaxed);
        mTelemetry.setSlotState(idx, SlotState::EMPTY);
        return nullptr;
    }

    void* vaddr = nullptr;
    int res;

#if __ANDROID_API__ >= 29
    // API 29+: Use lockAndGetInfo for accurate stride information
    int32_t bytesPerPixel = 0;
    int32_t bytesPerStride = 0;
    res = AHardwareBuffer_lockAndGetInfo(
        mBuffers[idx],
        AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
        -1,      // No acquire fence
        nullptr, // Full buffer rect
        &vaddr,
        &bytesPerPixel,
        &bytesPerStride
    );
    if (res == 0 && outStrideBytes) {
        *outStrideBytes = bytesPerStride;
        mMetadata[idx].strideBytes = bytesPerStride;
    }
#else
    // API 26-28: Use describe() then lock()
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(mBuffers[idx], &desc);
    res = AHardwareBuffer_lock(
        mBuffers[idx],
        AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
        -1,
        nullptr,
        &vaddr
    );
    // Note: On older APIs, stride is in pixels - multiply by 4 for RGBA_8888
    if (res == 0 && outStrideBytes) {
        int32_t strideBytes = desc.stride * 4;  // Assume RGBA
        *outStrideBytes = strideBytes;
        mMetadata[idx].strideBytes = strideBytes;
    }
#endif

    if (UNLIKELY(res != 0)) {
        LOGE("Failed to lock AHardwareBuffer for write: %d", res);
        return nullptr;
    }

    return vaddr;
}

void FrameBufferRing::cancelWriteBuffer() {
    if (UNLIKELY(!mAllocated)) {
        return;
    }

    int idx = mWriteIndex.load(std::memory_order_relaxed);
    int fenceFd = -1;

    // Unlock without updating MAILBOX pointer
    AHardwareBuffer_unlock(mBuffers[idx], &fenceFd);

    // Close fence if returned (we're not using it)
    if (fenceFd >= 0) {
        close(fenceFd);
    }

    // Don't update mLatestCompleted - consumer won't see this frame
    // Don't advance mWriteIndex - reuse this slot for next frame
    mTelemetry.framesCorrupted.fetch_add(1, std::memory_order_relaxed);
}

void FrameBufferRing::unlockWriteBuffer() {
    if (UNLIKELY(!mAllocated)) {
        return;
    }

    int idx = mWriteIndex.load(std::memory_order_relaxed);
    int fenceFd = -1;

    // Unlock and capture release fence for GPU synchronization
    AHardwareBuffer_unlock(mBuffers[idx], &fenceFd);

    // Close old acquire fence if it was never consumed (prevents fd leak)
    if (mMetadata[idx].acquireFenceFd >= 0) {
        close(mMetadata[idx].acquireFenceFd);
    }

    // Update metadata for this completed frame
    mMetadata[idx].timestampNs = getCurrentTimeNs();
    mMetadata[idx].frameNumber = ++mFrameCounter;
    mMetadata[idx].width = mWidth;
    mMetadata[idx].height = mHeight;
    mMetadata[idx].format = mFormat;
    mMetadata[idx].acquireFenceFd = fenceFd;
    mMetadata[idx].valid = true;

    // Update telemetry: slot state to READY
    mTelemetry.setSlotState(idx, SlotState::READY);

    // MAILBOX: Atomically point the reader to this latest frame
    // Uses release semantics to ensure metadata writes are visible
    mLatestCompleted.store(idx, std::memory_order_release);
    mTelemetry.latestCompletedSlot.store(idx, std::memory_order_relaxed);

    // Triple-buffer dance: Choose next buffer that isn't being read
    // This prevents the camera from overwriting the buffer the renderer is using
    int nextWrite = (idx + 1) % FRAME_BUFFER_COUNT;
    int currentRead = mReadIndex.load(std::memory_order_acquire);
    if (nextWrite == currentRead) {
        nextWrite = (nextWrite + 1) % FRAME_BUFFER_COUNT;
    }
    mWriteIndex.store(nextWrite, std::memory_order_relaxed);

    mTelemetry.framesReceived.fetch_add(1, std::memory_order_relaxed);
    mTelemetry.lastFrameTimestampNs.store(mMetadata[idx].timestampNs,
                                          std::memory_order_relaxed);
}

AHardwareBuffer* FrameBufferRing::acquireReadBuffer(FrameSlotMetadata* outMetadata) {
    if (UNLIKELY(!mAllocated)) {
        return nullptr;
    }

    // MAILBOX: Always read the most recently completed frame
    // Uses acquire semantics to synchronize with producer's release
    int latest = mLatestCompleted.load(std::memory_order_acquire);

    if (latest < 0 || !mMetadata[latest].valid) {
        // No frame available - consumer starving
        mTelemetry.consumerStarves.fetch_add(1, std::memory_order_relaxed);

        // DIAGNOSTIC: Log consumer starve events
        static int starveLogCount = 0;
        if (++starveLogCount <= 5) {
            LOGW("PIPELINE_DIAG: Consumer STARVE #%d (latest=%d, framesReceived=%llu)",
                starveLogCount, latest,
                (unsigned long long)mTelemetry.framesReceived.load(std::memory_order_relaxed));
        }
        return nullptr;
    }

    // DIAGNOSTIC: Log first few successful acquisitions
    static int acquireSuccessCount = 0;
    if (++acquireSuccessCount <= 3) {
        LOGI("PIPELINE_DIAG: Consumer ACQUIRE #%d (slot=%d, frame#=%llu)",
            acquireSuccessCount, latest,
            (unsigned long long)mMetadata[latest].frameNumber);
    }

    // Mark which buffer is being read (prevents overwrite)
    mReadIndex.store(latest, std::memory_order_relaxed);
    mMetadata[latest].isLockedByConsumer = true;

    // Update telemetry: current read slot and state
    mTelemetry.currentReadSlot.store(latest, std::memory_order_relaxed);
    mTelemetry.setSlotState(latest, SlotState::READING);

    if (outMetadata) {
        *outMetadata = mMetadata[latest];
    }

    // Increment reference count: keeps buffer alive if renderer takes long
    // This is the critical fix for the atomic lifecycle race condition
    AHardwareBuffer_acquire(mBuffers[latest]);

    return mBuffers[latest];
}

void FrameBufferRing::releaseReadBuffer() {
    int idx = mReadIndex.load(std::memory_order_relaxed);

    if (idx >= 0 && idx < FRAME_BUFFER_COUNT && mBuffers[idx]) {
        // Clear consumer lock flag (matches acquire in acquireReadBuffer)
        mMetadata[idx].isLockedByConsumer = false;

        // Update telemetry: slot state back to READY (or EMPTY if invalid)
        if (mMetadata[idx].valid) {
            mTelemetry.setSlotState(idx, SlotState::READY);
        } else {
            mTelemetry.setSlotState(idx, SlotState::EMPTY);
        }
        mTelemetry.currentReadSlot.store(-1, std::memory_order_relaxed);

        // Decrement reference count
        AHardwareBuffer_release(mBuffers[idx]);
        mTelemetry.framesRendered.fetch_add(1, std::memory_order_relaxed);
    }
}

int64_t FrameBufferRing::getCurrentTimeNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

StreamTelemetry* FrameBufferRing::getTelemetry() {
    return &mTelemetry;
}

bool FrameBufferRing::isAllocated() const {
    return mAllocated;
}

uint32_t FrameBufferRing::getWidth() const {
    return mWidth;
}

uint32_t FrameBufferRing::getHeight() const {
    return mHeight;
}

int FrameBufferRing::findSlotByFrameNumber(uint64_t frameNumber) {
    for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
        if (mMetadata[i].valid && mMetadata[i].frameNumber == frameNumber) {
            return i;
        }
    }
    return -1; // Frame already recycled
}

FrameSlotMetadata* FrameBufferRing::getMetadata(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= FRAME_BUFFER_COUNT) {
        return nullptr;
    }
    return &mMetadata[slotIndex];
}

int FrameBufferRing::getCurrentReadIndex() const {
    return mReadIndex.load(std::memory_order_relaxed);
}

void FrameBufferRing::setGpuReleaseFence(int slotIndex, int fenceFd) {
    if (slotIndex < 0 || slotIndex >= FRAME_BUFFER_COUNT) {
        if (fenceFd >= 0) close(fenceFd);
        return;
    }

    FrameSlotMetadata& slot = mMetadata[slotIndex];

    // Close old fence if exists (shouldn't happen, but defensive)
    if (slot.gpuReleaseFenceFd >= 0) {
        close(slot.gpuReleaseFenceFd);
    }

    slot.gpuReleaseFenceFd = fenceFd;
    slot.isLockedByConsumer = false;
}

//======================================================================
// SPSC Queue Implementation (Hybrid Architecture)
//======================================================================

int FrameBufferRing::initEventFd() {
#ifdef UVCCAMERA_TESTING
    // Mock implementation for host testing
    mEventFd = 999;  // Fake fd
    return 0;
#else
    if (mEventFd >= 0) {
        return 0;  // Already initialized
    }

    mEventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (mEventFd < 0) {
        LOGE("Failed to create eventfd: %s", strerror(errno));
        return -1;
    }

    LOGI("Eventfd created: fd=%d", mEventFd);
    return 0;
#endif
}

void FrameBufferRing::closeEventFd() {
    if (mEventFd >= 0) {
#ifndef UVCCAMERA_TESTING
        close(mEventFd);
#endif
        mEventFd = -1;
    }

    // Reset SPSC queue indices
    mPendingWriteIdx.store(0, std::memory_order_relaxed);
    mPendingReadIdx.store(0, std::memory_order_relaxed);

    // Reset pending frames (clear ready flags, but keep buffers allocated)
    for (int i = 0; i < PENDING_QUEUE_SIZE; i++) {
        mPendingFrames[i].reset();
    }
}

bool FrameBufferRing::enqueuePendingFrame(const void* data, size_t bytes,
                                          uint32_t width, uint32_t height,
                                          int format) {
    // Get current write position
    int writeIdx = mPendingWriteIdx.load(std::memory_order_relaxed);
    int nextIdx = (writeIdx + 1) % PENDING_QUEUE_SIZE;

    // Check if queue is full (producer would catch up to consumer)
    if (nextIdx == mPendingReadIdx.load(std::memory_order_acquire)) {
        mTelemetry.framesDroppedQueueFull.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    PendingFrame& slot = mPendingFrames[writeIdx];

    // Ensure buffer capacity (may realloc)
    if (!slot.ensureCapacity(bytes)) {
        LOGE("Failed to allocate pending frame buffer: %zu bytes", bytes);
        mTelemetry.framesDroppedQueueFull.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Copy raw USB data - this is our ONLY copy in the pipeline
    memcpy(slot.data, data, bytes);
    slot.dataBytes = bytes;
    slot.width = width;
    slot.height = height;
    slot.frameFormat = format;

    // Timestamp for in-pipe latency tracking
    slot.callbackTimestampNs = getCurrentTimeNs();

    // Publish with release semantics (ensures data is visible before ready flag)
    slot.ready.store(true, std::memory_order_release);
    mPendingWriteIdx.store(nextIdx, std::memory_order_release);

    return true;
}

void FrameBufferRing::signalConversionThread() {
#ifdef UVCCAMERA_TESTING
    // Mock: no-op for testing
    return;
#else
    if (mEventFd >= 0) {
        uint64_t val = 1;
        // Write is non-blocking due to EFD_NONBLOCK
        // Failure is acceptable - means conversion thread hasn't read yet
        ssize_t written = write(mEventFd, &val, sizeof(val));
        (void)written;  // Silence unused variable warning
    }
#endif
}

PendingFrame* FrameBufferRing::dequeuePendingFrame() {
    int readIdx = mPendingReadIdx.load(std::memory_order_relaxed);

    // Check if queue is empty
    if (readIdx == mPendingWriteIdx.load(std::memory_order_acquire)) {
        return nullptr;
    }

    PendingFrame& slot = mPendingFrames[readIdx];

    // Ensure producer has finished writing (check ready flag)
    if (!slot.ready.load(std::memory_order_acquire)) {
        return nullptr;
    }

    return &slot;
}

void FrameBufferRing::completePendingFrame(PendingFrame* frame) {
    if (!frame) return;

    // Clear ready flag
    frame->ready.store(false, std::memory_order_release);

    // Advance read index
    int readIdx = mPendingReadIdx.load(std::memory_order_relaxed);
    mPendingReadIdx.store((readIdx + 1) % PENDING_QUEUE_SIZE, std::memory_order_release);
}

bool FrameBufferRing::waitForSignal(int timeoutMs) {
#ifdef UVCCAMERA_TESTING
    // Mock: always return true for testing
    return true;
#else
    if (mEventFd < 0) {
        // Fallback: busy wait with short sleep
        usleep(1000);  // 1ms
        return true;
    }

    struct pollfd pfd;
    pfd.fd = mEventFd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, timeoutMs);

    if (ret > 0 && (pfd.revents & POLLIN)) {
        // Drain the eventfd (read the counter value)
        uint64_t val;
        ssize_t bytesRead = read(mEventFd, &val, sizeof(val));
        (void)bytesRead;  // Silence unused variable warning
        return true;
    }

    return false;  // Timeout or error
#endif
}
