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

    // libjpeg-turbo for JPEG compression (Phase 4 - captureToFd)
    #include "turbojpeg.h"
#endif

#include "FrameBufferRing.h"

//======================================================================
// PendingFrame::ensureCapacity - Out-of-line for diagnostic logging
//======================================================================
bool PendingFrame::ensureCapacity(size_t needed) {
    static std::atomic<int> capacityCallCount{0};
    int callNum = ++capacityCallCount;

    // Log first 10 calls, then every 500th
    bool shouldLog = (callNum <= 10 || callNum % 500 == 0);

    if (shouldLog) {
        LOGI("CAPACITY_DIAG[%d]: ensureCapacity(%zu) called, current bufferCapacity=%zu data=%p",
             callNum, needed, bufferCapacity, data);
    }

    if (bufferCapacity >= needed) {
        if (shouldLog) {
            LOGI("CAPACITY_DIAG[%d]: Sufficient capacity, no realloc needed", callNum);
        }
        return true;
    }

    // Round up to add 25% headroom to reduce future reallocations
    size_t newCapacity = needed + (needed / 4);

    if (shouldLog) {
        LOGI("CAPACITY_DIAG[%d]: Calling realloc(%p, %zu) for newCapacity", callNum, data, newCapacity);
    }

    void* newBuf = realloc(data, newCapacity);

    if (shouldLog) {
        LOGI("CAPACITY_DIAG[%d]: realloc returned %p", callNum, newBuf);
    }

    if (newBuf == nullptr) {
        LOGE("CAPACITY_DIAG[%d]: realloc FAILED for %zu bytes!", callNum, newCapacity);
        return false;
    }

    // ========== CRITICAL: Log the pointer transition ==========
    if (shouldLog) {
        if (data != nullptr && newBuf != data) {
            LOGI("CAPACITY_DIAG[%d]: Pointer MOVED: old=%p new=%p", callNum, data, newBuf);
        } else if (data == nullptr) {
            LOGI("CAPACITY_DIAG[%d]: First allocation: %p", callNum, newBuf);
        } else {
            LOGI("CAPACITY_DIAG[%d]: Pointer unchanged (in-place realloc)", callNum);
        }
    }

    data = newBuf;
    bufferCapacity = newCapacity;

    if (shouldLog) {
        LOGI("CAPACITY_DIAG[%d]: Updated slot: data=%p bufferCapacity=%zu", callNum, data, bufferCapacity);
    }

    return true;
}

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

    // ========== ALLOC_DIAG: Validate mPendingFrames array initialization ==========
    LOGI("ALLOC_DIAG: FrameBufferRing::allocate() validating SPSC queue");
    LOGI("ALLOC_DIAG: this=%p PENDING_QUEUE_SIZE=%d", this, PENDING_QUEUE_SIZE);
    LOGI("ALLOC_DIAG: mPendingFrames array at %p", mPendingFrames);

    for (int i = 0; i < PENDING_QUEUE_SIZE; i++) {
        LOGI("ALLOC_DIAG: mPendingFrames[%d]: addr=%p data=%p bufferCapacity=%zu dataBytes=%zu",
             i, &mPendingFrames[i], mPendingFrames[i].data,
             mPendingFrames[i].bufferCapacity, mPendingFrames[i].dataBytes);

        // Detect inconsistent state
        if (mPendingFrames[i].data != nullptr && mPendingFrames[i].bufferCapacity == 0) {
            LOGE("ALLOC_DIAG: INCONSISTENT STATE at slot %d: data non-null but bufferCapacity=0", i);
        }
        if (mPendingFrames[i].data == nullptr && mPendingFrames[i].bufferCapacity > 0) {
            LOGE("ALLOC_DIAG: INCONSISTENT STATE at slot %d: data null but bufferCapacity=%zu",
                 i, mPendingFrames[i].bufferCapacity);
        }
    }

    LOGI("ALLOC_DIAG: SPSC queue indices: writeIdx=%d readIdx=%d",
         mPendingWriteIdx.load(std::memory_order_relaxed),
         mPendingReadIdx.load(std::memory_order_relaxed));

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
    // ========== CORRUPTION DETECTION: Validate ring buffer on entry ==========
    validateOrAbort("acquireReadBuffer entry");

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

//======================================================================
// Snapshot API (Phase 4 - captureToFd)
//======================================================================

int FrameBufferRing::captureToFd(int fd, int quality) {
#ifdef UVCCAMERA_TESTING
    // Mock implementation for host testing - not available
    LOGI("captureToFd: Not available in test mode");
    return -1;
#else
    // Validate quality range
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    LOGI("CAPTURE: captureToFd(fd=%d, quality=%d) - starting", fd, quality);
    int64_t startTimeNs = getCurrentTimeNs();

    // Acquire the most recent completed frame
    FrameSlotMetadata metadata;
    AHardwareBuffer* buffer = acquireReadBuffer(&metadata);
    if (UNLIKELY(!buffer)) {
        LOGW("CAPTURE: No frame available for capture");
        return -1;  // No frame available
    }

    // Get buffer description for format/dimensions
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(buffer, &desc);

    LOGI("CAPTURE: Acquired frame %llu (%ux%u, format=0x%x, stride=%u)",
         (unsigned long long)metadata.frameNumber, desc.width, desc.height,
         desc.format, desc.stride);

    // Lock buffer for CPU read
    void* pixels = nullptr;
    int lockResult = AHardwareBuffer_lock(buffer,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
        -1,      // No acquire fence
        nullptr, // Full buffer rect
        &pixels);

    if (UNLIKELY(lockResult != 0 || !pixels)) {
        LOGE("CAPTURE: Failed to lock buffer for CPU read: %d", lockResult);
        releaseReadBuffer();
        return -2;  // Lock failed
    }

    // Determine pixel format for TurboJPEG
    int tjPixelFormat;
    int bytesPerPixel;

    if (desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM) {
        // RGBX format (most common)
        tjPixelFormat = TJPF_RGBX;
        bytesPerPixel = 4;
    } else if (desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM) {
        // XRGB format
        tjPixelFormat = TJPF_XRGB;
        bytesPerPixel = 4;
    } else if (desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM) {
        // RGB format (no alpha)
        tjPixelFormat = TJPF_RGB;
        bytesPerPixel = 3;
    } else {
        LOGE("CAPTURE: Unsupported buffer format: 0x%x", desc.format);
        AHardwareBuffer_unlock(buffer, nullptr);
        releaseReadBuffer();
        return -3;  // Unsupported format
    }

    // Initialize TurboJPEG compressor
    tjhandle tjHandle = tjInitCompress();
    if (UNLIKELY(!tjHandle)) {
        LOGE("CAPTURE: Failed to initialize TurboJPEG compressor");
        AHardwareBuffer_unlock(buffer, nullptr);
        releaseReadBuffer();
        return -4;
    }

    // Compress to JPEG
    unsigned char* jpegBuf = nullptr;
    unsigned long jpegSize = 0;

    // Calculate actual pitch (stride in bytes)
    // Note: AHardwareBuffer stride is in pixels, multiply by bytesPerPixel for bytes
    int pitch = desc.stride * bytesPerPixel;

    int compressResult = tjCompress2(tjHandle,
        static_cast<unsigned char*>(pixels),
        desc.width,
        pitch,
        desc.height,
        tjPixelFormat,
        &jpegBuf,
        &jpegSize,
        TJSAMP_420,  // 4:2:0 subsampling (good quality/size balance)
        quality,
        TJFLAG_FASTDCT);

    // Unlock buffer - we've copied what we need
    AHardwareBuffer_unlock(buffer, nullptr);
    releaseReadBuffer();

    if (UNLIKELY(compressResult != 0)) {
        LOGE("CAPTURE: JPEG compression failed: %s", tjGetErrorStr());
        tjDestroy(tjHandle);
        if (jpegBuf) tjFree(jpegBuf);
        return -4;
    }

    LOGI("CAPTURE: Compressed to %lu bytes (%.1f%% of raw)",
         jpegSize, (float)jpegSize * 100.0f / (desc.width * desc.height * bytesPerPixel));

    // Write JPEG to file descriptor
    ssize_t totalWritten = 0;
    const unsigned char* writePtr = jpegBuf;
    size_t remaining = jpegSize;

    while (remaining > 0) {
        ssize_t written = write(fd, writePtr, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                // Interrupted by signal, retry
                continue;
            }
            LOGE("CAPTURE: Write to fd failed: %s (written %zd of %lu)",
                 strerror(errno), totalWritten, jpegSize);
            tjFree(jpegBuf);
            tjDestroy(tjHandle);
            return -5;  // Write failed
        }
        writePtr += written;
        remaining -= written;
        totalWritten += written;
    }

    // Cleanup
    tjFree(jpegBuf);
    tjDestroy(tjHandle);

    int64_t endTimeNs = getCurrentTimeNs();
    int64_t captureTimeUs = (endTimeNs - startTimeNs) / 1000;

    LOGI("CAPTURE: Success - wrote %zd bytes in %lld us", totalWritten, (long long)captureTimeUs);

    return 0;  // Success
#endif
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

//======================================================================
// Corruption Detection (Magic Number Validation)
//======================================================================

bool FrameBufferRing::validateMagic() const {
    bool valid = (mMagicHeader == MAGIC_HEADER) && (mMagicFooter == MAGIC_FOOTER);

    if (!valid) {
        // Check if this is a poisoned buffer (explicitly invalidated) vs random corruption
        bool isPoisoned = (mMagicHeader == MAGIC_POISON) || (mMagicFooter == MAGIC_POISON);
        
        if (isPoisoned) {
            LOGE("MAGIC_POISONED: ═══════════════════════════════════════════");
            LOGE("MAGIC_POISONED: Ring buffer was explicitly invalidated - stale handle access");
            LOGE("MAGIC_POISONED: header=0x%016llx footer=0x%016llx",
                 (unsigned long long)mMagicHeader, (unsigned long long)mMagicFooter);
            LOGE("MAGIC_POISONED: ═══════════════════════════════════════════");
        } else {
            LOGE("MAGIC_CORRUPT: ═══════════════════════════════════════════");
            LOGE("MAGIC_CORRUPT: header=0x%016llx (expected 0x%016llx)",
                 (unsigned long long)mMagicHeader, (unsigned long long)MAGIC_HEADER);
            LOGE("MAGIC_CORRUPT: footer=0x%016llx (expected 0x%016llx)",
                 (unsigned long long)mMagicFooter, (unsigned long long)MAGIC_FOOTER);

            // Log as ASCII for string corruption detection (heap spray, etc.)
            char headerAscii[9] = {0};
            char footerAscii[9] = {0};
            memcpy(headerAscii, &mMagicHeader, 8);
            memcpy(footerAscii, &mMagicFooter, 8);

            // Replace non-printable chars with '.'
            for (int i = 0; i < 8; i++) {
                if (headerAscii[i] < 32 || headerAscii[i] > 126) headerAscii[i] = '.';
                if (footerAscii[i] < 32 || footerAscii[i] > 126) footerAscii[i] = '.';
            }

            LOGE("MAGIC_CORRUPT: header as ASCII: '%.8s'", headerAscii);
            LOGE("MAGIC_CORRUPT: footer as ASCII: '%.8s'", footerAscii);
            LOGE("MAGIC_CORRUPT: ═══════════════════════════════════════════");
        }
    }

    return valid;
}

bool FrameBufferRing::isValid() const {
    // Check if magic headers are valid (not corrupted AND not poisoned)
    return (mMagicHeader == MAGIC_HEADER) && (mMagicFooter == MAGIC_FOOTER);
}

void FrameBufferRing::poisonMagicHeaders() {
    LOGI("LIFECYCLE: Poisoning ring buffer magic headers (was header=0x%016llx, footer=0x%016llx)",
         (unsigned long long)mMagicHeader, (unsigned long long)mMagicFooter);
    
    // Use volatile to prevent compiler from optimizing away these writes
    volatile uint64_t* headerPtr = &mMagicHeader;
    volatile uint64_t* footerPtr = &mMagicFooter;
    *headerPtr = MAGIC_POISON;
    *footerPtr = MAGIC_POISON;
    
    LOGI("LIFECYCLE: Ring buffer magic headers poisoned (now 0x%016llx)",
         (unsigned long long)MAGIC_POISON);
}

void FrameBufferRing::validateOrAbort(const char* context) const {
    if (!validateMagic()) {
        // Check if poisoned - if so, don't abort, just return (caller should handle)
        bool isPoisoned = (mMagicHeader == MAGIC_POISON) || (mMagicFooter == MAGIC_POISON);
        if (isPoisoned) {
            // Poisoned buffers don't abort - they're intentionally invalidated
            // The caller should check isValid() and handle gracefully
            return;
        }
        
        LOGE("MAGIC_CORRUPT: ═══════════════════════════════════════════");
        LOGE("MAGIC_CORRUPT: FATAL CORRUPTION - ABORTING");
        LOGE("MAGIC_CORRUPT: Context: %s", context);
        LOGE("MAGIC_CORRUPT: this=%p", this);

#ifndef UVCCAMERA_TESTING
        // Log thread info for tombstone correlation
        LOGE("MAGIC_CORRUPT: Thread ID: %d", gettid());

        // Log pointer tag for MTE analysis (ARM64 only, Android 16+)
#if __SIZEOF_POINTER__ == 8
        uintptr_t addr = reinterpret_cast<uintptr_t>(this);
        LOGE("MAGIC_CORRUPT: MTE tag: 0x%02x", (unsigned)(addr >> 56));
        LOGE("MAGIC_CORRUPT: Untagged addr: %p", (void*)(addr & 0x00FFFFFFFFFFFFFFULL));
#else
        LOGE("MAGIC_CORRUPT: (MTE not applicable on 32-bit)");
#endif

        // Force flush logs before abort
        __android_log_write(ANDROID_LOG_FATAL, "MAGIC_CORRUPT",
            "Aborting due to memory corruption - see logs above");
#endif

        LOGE("MAGIC_CORRUPT: ═══════════════════════════════════════════");

        // Use __builtin_trap() for cleaner stack trace on ARM64
        __builtin_trap();
    }
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
    // ========== CORRUPTION DETECTION: Validate ring buffer on entry ==========
    validateOrAbort("enqueuePendingFrame entry");

    static std::atomic<int> enqueueCallCount{0};
    int callNum = ++enqueueCallCount;

    // Log first 5 calls, then every 500th
    bool shouldLog = (callNum <= 5 || callNum % 500 == 0);

    if (shouldLog) {
        LOGI("ENQUEUE_DIAG[%d]: ╔═══════════════════════════════════════════╗", callNum);
        LOGI("ENQUEUE_DIAG[%d]: ║      enqueuePendingFrame() ENTRY          ║", callNum);
        LOGI("ENQUEUE_DIAG[%d]: ╚═══════════════════════════════════════════╝", callNum);
        LOGI("ENQUEUE_DIAG[%d]: this=%p data=%p bytes=%zu dims=%ux%u fmt=%d",
             callNum, this, data, bytes, width, height, format);
    }

    // Get current write position
    int writeIdx = mPendingWriteIdx.load(std::memory_order_relaxed);
    int readIdx = mPendingReadIdx.load(std::memory_order_acquire);
    int nextIdx = (writeIdx + 1) % PENDING_QUEUE_SIZE;

    if (shouldLog) {
        LOGI("ENQUEUE_DIAG[%d]: writeIdx=%d readIdx=%d nextIdx=%d PENDING_QUEUE_SIZE=%d",
             callNum, writeIdx, readIdx, nextIdx, PENDING_QUEUE_SIZE);
    }

    // Check if queue is full (producer would catch up to consumer)
    if (nextIdx == readIdx) {
        if (shouldLog) {
            LOGW("ENQUEUE_DIAG[%d]: Queue full, dropping frame", callNum);
        }
        mTelemetry.framesDroppedQueueFull.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // ========== CRITICAL: Log slot state BEFORE ensureCapacity ==========
    PendingFrame& slot = mPendingFrames[writeIdx];

    // ========== SLOT CORRUPTION DETECTION ==========
    if (!slot.validateSlotMagic()) {
        LOGE("SLOT_CORRUPT: Slot %d magic validation FAILED!", writeIdx);
        LOGE("SLOT_CORRUPT: slotMagicStart=0x%08x slotMagicEnd=0x%08x expected=0x%08x",
             slot.slotMagicStart, slot.slotMagicEnd, PendingFrame::SLOT_MAGIC_VALUE);
        mTelemetry.framesDroppedQueueFull.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (shouldLog) {
        LOGI("ENQUEUE_DIAG[%d]: slot address=&mPendingFrames[%d]=%p",
             callNum, writeIdx, &slot);
        LOGI("ENQUEUE_DIAG[%d]: PRE-ENSURE: slot.data=%p slot.bufferCapacity=%zu",
             callNum, slot.data, slot.bufferCapacity);

        // Validate slot.data before ensureCapacity (MTE tag analysis - 64-bit only)
        if (slot.data != nullptr) {
#if __SIZEOF_POINTER__ == 8
            uintptr_t addr = (uintptr_t)slot.data;
            uint8_t tag = (uint8_t)(addr >> 56);
            LOGI("ENQUEUE_DIAG[%d]: PRE-ENSURE: slot.data MTE_tag=0x%02x untagged=%p",
                 callNum, tag, (void*)(addr & 0x00FFFFFFFFFFFFFF));
#else
            LOGI("ENQUEUE_DIAG[%d]: PRE-ENSURE: slot.data=%p (32-bit, no MTE)",
                 callNum, slot.data);
#endif
        }

        LOGI("ENQUEUE_DIAG[%d]: Calling slot.ensureCapacity(%zu)...", callNum, bytes);
    }

    // Ensure buffer capacity (may realloc)
    if (!slot.ensureCapacity(bytes)) {
        LOGE("ENQUEUE_DIAG[%d]: ensureCapacity FAILED for %zu bytes", callNum, bytes);
        mTelemetry.framesDroppedQueueFull.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (shouldLog) {
        LOGI("ENQUEUE_DIAG[%d]: ensureCapacity returned true", callNum);

        // ========== CRITICAL: Log slot state AFTER ensureCapacity ==========
        LOGI("ENQUEUE_DIAG[%d]: POST-ENSURE: slot.data=%p slot.bufferCapacity=%zu",
             callNum, slot.data, slot.bufferCapacity);

        // ========== Validate slot.data is usable ==========
        if (slot.data == nullptr) {
            LOGE("ENQUEUE_DIAG[%d]: FATAL - slot.data is NULL after ensureCapacity!", callNum);
            return false;
        }

#if __SIZEOF_POINTER__ == 8
        uintptr_t dataAddr = (uintptr_t)slot.data;
        uint8_t dataTag = (uint8_t)(dataAddr >> 56);
        uintptr_t untaggedAddr = dataAddr & 0x00FFFFFFFFFFFFFF;

        LOGI("ENQUEUE_DIAG[%d]: POST-ENSURE: MTE_tag=0x%02x untagged=%p",
             callNum, dataTag, (void*)untaggedAddr);

        // Sanity check: untagged address should be in reasonable heap range
        if (untaggedAddr < 0x1000) {
            LOGE("ENQUEUE_DIAG[%d]: FATAL - slot.data looks like NULL page: %p",
                 callNum, slot.data);
            return false;
        }
#endif

        LOGI("ENQUEUE_DIAG[%d]: About to memcpy(%p, %p, %zu)",
             callNum, slot.data, data, bytes);
    }

    // Copy raw USB data - this is our ONLY copy in the pipeline
    memcpy(slot.data, data, bytes);

    if (shouldLog) {
        LOGI("ENQUEUE_DIAG[%d]: memcpy completed successfully", callNum);
    }

    slot.dataBytes = bytes;
    slot.width = width;
    slot.height = height;
    slot.frameFormat = format;

    // Timestamp for in-pipe latency tracking
    slot.callbackTimestampNs = getCurrentTimeNs();

    // Publish with release semantics (ensures data is visible before ready flag)
    slot.ready.store(true, std::memory_order_release);
    mPendingWriteIdx.store(nextIdx, std::memory_order_release);

    if (shouldLog) {
        LOGI("ENQUEUE_DIAG[%d]: Frame enqueued, new writeIdx=%d", callNum, nextIdx);
    }

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
