/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: FrameBufferRing.h
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

#ifndef FRAMEBUFFERRING_H_
#define FRAMEBUFFERRING_H_

// Platform-specific: AHardwareBuffer
// When testing on host (UVCCAMERA_TESTING defined), use mock implementations
#ifdef UVCCAMERA_TESTING
    #include "AndroidApiMocks.h"
#else
    #include <android/hardware_buffer.h>
    #include <sys/eventfd.h>
    #include <poll.h>
#endif

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include "FrameSlotMetadata.h"
#include "StreamTelemetry.h"

// Note: FRAME_BUFFER_COUNT and PENDING_QUEUE_SIZE are defined in StreamTelemetry.h

//======================================================================
// PendingFrame for SPSC Queue (Hybrid Architecture)
//======================================================================

/**
 * Represents a raw frame awaiting conversion.
 * Padded to cache-line size to prevent false sharing between producer/consumer.
 * Note: We avoid alignas(64) on the struct itself to prevent requiring C++17
 * aligned allocation operators when this struct is used as an array member.
 */
struct PendingFrame {
    void* data{nullptr};              // Raw USB data (owned by this struct)
    size_t dataBytes{0};              // Actual data size
    size_t bufferCapacity{0};         // Allocated buffer size
    uint32_t width{0};
    uint32_t height{0};
    int frameFormat{0};               // uvc_frame_format enum value
    uint64_t callbackTimestampNs{0};  // When USB callback received frame

    // Ready flag for SPSC synchronization
    std::atomic<bool> ready{false};

    ~PendingFrame() {
        if (data) {
            free(data);
            data = nullptr;
        }
    }

    bool ensureCapacity(size_t needed) {
        if (bufferCapacity >= needed) return true;

        void* newBuf = realloc(data, needed);
        if (!newBuf) return false;

        data = newBuf;
        bufferCapacity = needed;
        return true;
    }

    void reset() {
        dataBytes = 0;
        width = 0;
        height = 0;
        frameFormat = 0;
        callbackTimestampNs = 0;
        ready.store(false, std::memory_order_relaxed);
        // Note: data buffer retained for reuse
    }
};

/**
 * AHardwareBuffer-based triple-buffered ring buffer for UVC frame streaming.
 *
 * Implements MAILBOX frame drop policy:
 * - Producer always writes to the next available buffer
 * - Consumer always reads the most recently completed frame
 * - Frames are dropped (not queued) when producer outpaces consumer
 *
 * This decouples camera frame acquisition from Surface rendering,
 * allowing the camera thread to continue regardless of UI state.
 *
 * Thread Safety:
 * - Producer (camera thread): calls lockWriteBuffer/unlockWriteBuffer
 * - Consumer (render thread): calls acquireReadBuffer/releaseReadBuffer
 * - Memory barriers ensure visibility across threads
 *
 * API Level Requirements:
 * - Minimum: API 26 (Android 8.0) for AHardwareBuffer
 * - Recommended: API 29+ for AHardwareBuffer_lockAndGetInfo (accurate stride)
 */
class FrameBufferRing {
public:
    FrameBufferRing();
    ~FrameBufferRing();

    /**
     * Allocate triple-buffered AHardwareBuffer ring.
     * @param width Frame width in pixels
     * @param height Frame height in pixels
     * @param format AHardwareBuffer format (e.g., AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM)
     * @return 0 on success, error code on failure
     */
    int allocate(uint32_t width, uint32_t height, uint32_t format);

    /**
     * Release all AHardwareBuffers and reset state.
     * Safe to call multiple times.
     */
    void destroy();

    /**
     * Producer API: Lock the current write buffer for CPU access.
     * @param outStrideBytes Output: actual row stride in bytes (critical for GPU alignment)
     * @return Pointer to buffer memory, or nullptr on failure
     */
    void* lockWriteBuffer(int32_t* outStrideBytes);

    /**
     * Producer API: Unlock write buffer and advance to next slot.
     * Captures release fence and updates MAILBOX pointer.
     */
    void unlockWriteBuffer();

    /**
     * Producer API: Cancel write and unlock buffer without committing.
     * Use when frame conversion fails - unlocks buffer but doesn't
     * update MAILBOX pointer, so consumer won't see corrupt frame.
     */
    void cancelWriteBuffer();

    /**
     * Consumer API: Acquire the most recent completed frame.
     * Increments AHardwareBuffer reference count for safety.
     * @param outMetadata Output: metadata for acquired frame
     * @return AHardwareBuffer pointer (caller must call releaseReadBuffer), or nullptr
     */
    AHardwareBuffer* acquireReadBuffer(FrameSlotMetadata* outMetadata);

    /**
     * Consumer API: Release the previously acquired read buffer.
     * Decrements AHardwareBuffer reference count.
     * For bidirectional fence sync, call setGpuReleaseFence() before releasing.
     */
    void releaseReadBuffer();

    /**
     * Find slot index by frame number for race-safe release.
     * @param frameNumber Frame number from FrameSlotMetadata
     * @return Slot index, or -1 if frame was already recycled
     */
    int findSlotByFrameNumber(uint64_t frameNumber);

    /**
     * Get metadata for a specific slot (for JNI access).
     * @param slotIndex Slot index (0 to FRAME_BUFFER_COUNT-1)
     * @return Pointer to metadata, or nullptr if invalid index
     */
    FrameSlotMetadata* getMetadata(int slotIndex);

    /**
     * Get current read index (for JNI to fetch metadata after acquire).
     * @return Current read slot index, or -1 if none
     */
    int getCurrentReadIndex() const;

    /**
     * Store GPU release fence for producer to wait on.
     * Called by consumer after rendering is queued to GPU.
     * @param slotIndex Slot index to store fence for
     * @param fenceFd GPU release fence fd (takes ownership, will be closed)
     */
    void setGpuReleaseFence(int slotIndex, int fenceFd);

    // State queries
    bool isAllocated() const;
    uint32_t getWidth() const;
    uint32_t getHeight() const;
    StreamTelemetry* getTelemetry();

    //==========================================================================
    // SPSC Queue API (Hybrid Architecture)
    //==========================================================================

    /**
     * Initialize the eventfd for thread signaling.
     * Call once during FrameBufferRing allocation.
     * @return 0 on success, -1 on failure
     */
    int initEventFd();

    /**
     * Cleanup eventfd.
     * Called during destroy().
     */
    void closeEventFd();

    /**
     * Get eventfd for poll() in conversion thread.
     */
    int getEventFd() const { return mEventFd; }

    /**
     * Enqueue a raw frame for deferred conversion.
     * Called from USB callback - MUST be fast (<100μs).
     *
     * @param data Raw frame data (will be copied)
     * @param bytes Data size
     * @param width Frame width
     * @param height Frame height
     * @param format uvc_frame_format enum value
     * @return true if enqueued, false if queue full (frame dropped)
     */
    bool enqueuePendingFrame(const void* data, size_t bytes,
                             uint32_t width, uint32_t height, int format);

    /**
     * Signal the conversion thread that a frame is available.
     * Called after successful enqueuePendingFrame().
     */
    void signalConversionThread();

    /**
     * Dequeue a pending frame for conversion.
     * Called from conversion thread.
     *
     * @return Pointer to pending frame, or nullptr if queue empty
     */
    PendingFrame* dequeuePendingFrame();

    /**
     * Mark a pending frame as processed.
     * Called after conversion completes.
     *
     * @param frame Pointer returned by dequeuePendingFrame()
     */
    void completePendingFrame(PendingFrame* frame);

    /**
     * Wait for signal from USB callback.
     * Blocks until signaled or timeout.
     *
     * @param timeoutMs Timeout in milliseconds (-1 for infinite)
     * @return true if signaled, false if timeout
     */
    bool waitForSignal(int timeoutMs);

private:
    AHardwareBuffer*   mBuffers[FRAME_BUFFER_COUNT];
    FrameSlotMetadata  mMetadata[FRAME_BUFFER_COUNT];

    // MAILBOX indices (atomic for thread safety)
    std::atomic<int>   mWriteIndex{0};      // Next buffer to write
    std::atomic<int>   mReadIndex{0};       // Buffer currently being read
    std::atomic<int>   mLatestCompleted{-1}; // Most recent completed frame

    uint64_t           mFrameCounter{0};
    uint32_t           mWidth{0};
    uint32_t           mHeight{0};
    uint32_t           mFormat{0};
    bool               mAllocated{false};
    StreamTelemetry    mTelemetry;

    //==========================================================================
    // SPSC Queue State (Hybrid Architecture)
    // Note: We avoid alignas(64) to prevent requiring C++17 aligned new/delete.
    // The atomic indices are still cache-line separated due to array placement.
    //==========================================================================
    PendingFrame mPendingFrames[PENDING_QUEUE_SIZE];
    std::atomic<int> mPendingWriteIdx{0};
    char _paddingWrite[60];  // Pad to separate from read index
    std::atomic<int> mPendingReadIdx{0};
    char _paddingRead[60];   // Pad to prevent false sharing

    // Eventfd for signaling conversion thread
    int mEventFd{-1};

    int64_t getCurrentTimeNs();
};

#endif /* FRAMEBUFFERRING_H_ */
