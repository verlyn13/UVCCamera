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
 *
 * Magic number canaries (slotMagicStart/End) are used to detect memory corruption.
 */
struct PendingFrame {
    // ========== CORRUPTION DETECTION CANARIES ==========
    static constexpr uint32_t SLOT_MAGIC_VALUE = 0x534C4F54;  // "SLOT" in ASCII

    uint32_t slotMagicStart{SLOT_MAGIC_VALUE};  // Start canary

    void* data{nullptr};              // Raw USB data (owned by this struct)
    size_t dataBytes{0};              // Actual data size
    size_t bufferCapacity{0};         // Allocated buffer size
    uint32_t width{0};
    uint32_t height{0};
    int frameFormat{0};               // uvc_frame_format enum value
    uint64_t callbackTimestampNs{0};  // When USB callback received frame

    // Ready flag for SPSC synchronization
    std::atomic<bool> ready{false};

    uint32_t slotMagicEnd{SLOT_MAGIC_VALUE};    // End canary

    ~PendingFrame() {
        if (data) {
            free(data);
            data = nullptr;
        }
    }

    // Ensure buffer has at least 'needed' bytes capacity
    // Implementation moved to FrameBufferRing.cpp for diagnostic logging
    bool ensureCapacity(size_t needed);

    // Validate slot magic numbers
    bool validateSlotMagic() const {
        return slotMagicStart == SLOT_MAGIC_VALUE &&
               slotMagicEnd == SLOT_MAGIC_VALUE;
    }

    void reset() {
        dataBytes = 0;
        width = 0;
        height = 0;
        frameFormat = 0;
        callbackTimestampNs = 0;
        ready.store(false, std::memory_order_relaxed);
        // Note: data buffer retained for reuse
        // Note: Magic canaries are NOT reset - if corrupted, that's a fatal error
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
    // ========== CORRUPTION DETECTION MAGIC NUMBERS ==========
    // These appear at the start and end of the object to detect memory corruption.
    // The values are chosen to be recognizable in hex dumps and not common in normal data.
    static constexpr uint64_t MAGIC_HEADER = 0xFB01CAFEBABE2026ULL;
    static constexpr uint64_t MAGIC_FOOTER = 0xFB01DEADBEEF2026ULL;
    // Poison value used to mark explicitly invalidated buffers (distinguishable from random garbage)
    static constexpr uint64_t MAGIC_POISON = 0xDEADD00DDEADD00DULL;

private:
    uint64_t mMagicHeader{MAGIC_HEADER};  // MUST BE FIRST DATA MEMBER

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

    //==========================================================================
    // Snapshot API (Phase 4)
    //==========================================================================

    /**
     * Capture current frame to file descriptor as JPEG.
     * Uses FD-based I/O for Android scoped storage compatibility.
     *
     * This method acquires the most recent completed frame, locks it for
     * CPU read, compresses to JPEG using libjpeg-turbo, and writes to the fd.
     *
     * Thread Safety:
     * - Can be called from any thread
     * - Uses internal acquire/release for buffer access
     * - Non-blocking if no frame available (returns error)
     *
     * Format Support:
     * - AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM (RGBX) - primary
     * - Other formats return -3 (unsupported)
     *
     * @param fd Open file descriptor (caller manages lifecycle, must be writable)
     * @param quality JPEG quality (1-100, default 90)
     * @return 0 on success, negative error code on failure:
     *         -1: No frame available
     *         -2: Failed to lock buffer for CPU read
     *         -3: Unsupported buffer format
     *         -4: JPEG compression failed
     *         -5: Write to fd failed
     */
    int captureToFd(int fd, int quality = 90);

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

    // ========== CORRUPTION DETECTION API ==========
    /**
     * Validate magic numbers at start and end of object.
     * @return true if magic numbers are intact, false if corrupted or poisoned
     */
    bool validateMagic() const;

    /**
     * Validate magic numbers and abort if corrupted.
     * Logs detailed diagnostic information before aborting.
     * @param context Description of the calling location for diagnostics
     */
    void validateOrAbort(const char* context) const;

    /**
     * Check if ring buffer handle is valid for operations.
     * Returns false if null, poisoned, or corrupt.
     * Unlike validateOrAbort, this does NOT abort - returns false instead.
     */
    bool isValid() const;

    /**
     * Poison magic headers to mark buffer as explicitly invalidated.
     * Any subsequent access will see MAGIC_POISON and know the buffer
     * was intentionally invalidated (vs random garbage from use-after-free).
     * Called before freeing memory in clearRingBuffer().
     */
    void poisonMagicHeaders();

    /**
     * Get the index of the most recently completed write slot.
     * @return Slot index (0-2), or -1 if no frames completed yet
     */
    int getLatestCompleted() const {
        return mLatestCompleted.load(std::memory_order_acquire);
    }

    /**
     * Get a unique identifier for this ring buffer instance.
     * Used for handle alignment diagnostics.
     */
    uint64_t getInstanceId() const {
        return reinterpret_cast<uint64_t>(this);
    }

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

    // ========== CORRUPTION DETECTION - MUST BE LAST DATA MEMBER ==========
    uint64_t mMagicFooter{MAGIC_FOOTER};
};

#endif /* FRAMEBUFFERRING_H_ */
