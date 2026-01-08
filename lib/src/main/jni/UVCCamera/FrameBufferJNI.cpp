/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: FrameBufferJNI.cpp
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

#define LOG_TAG "libUVCCamera/FrameBufferJNI"

#include "localdefines.h"
#include "utilbase.h"

#include <jni.h>
#include <unistd.h>
#include <errno.h>
#include <atomic>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>

#include "FrameBufferRing.h"
#include "HandleManager.h"

//======================================================================
// JNI Handle Validation Helper (Phase 4 - Dangling Handle Protection)
// Uses HandleManager for slot-based reference counting
//======================================================================

/**
 * Acquire a ScopedRef for a ring buffer handle.
 * Use this for methods that need to hold the reference across multiple operations.
 *
 * @param handle JNI jlong handle from Java
 * @param context Description of caller for error logging
 * @return ScopedRef with valid ptr, or ScopedRef{nullptr, nullptr}
 */
static HandleManager::ScopedRef acquireRingBuffer(jlong handle, const char* context) {
    auto ref = getRingBufferHandleManager().acquire(handle);
    if (!ref) {
        LOGE("JNI[%s]: Handle 0x%llx validation failed (invalid or stale)",
             context, (unsigned long long)handle);
        return {nullptr, nullptr};
    }

    auto* ring = static_cast<FrameBufferRing*>(ref.ptr);
    if (!ring->validateMagic()) {
        LOGE("JNI[%s]: Handle 0x%llx failed magic validation - memory corrupted!",
             context, (unsigned long long)handle);
        // ref destructor will release the active ref count
        return {nullptr, nullptr};
    }

    return ref;
}

/**
 * JNI bridge for FrameBufferRing.
 *
 * Provides Java/Kotlin access to the native AHardwareBuffer-based
 * triple-buffered ring buffer for UVC frame streaming.
 *
 * Thread Safety:
 * - nativeAllocate/nativeDestroy: call from main thread only
 * - nativeAcquireBuffer/nativeReleaseBuffer: call from render thread
 * - nativeLockWriteBuffer/nativeUnlockWriteBuffer: call from camera thread
 *
 * Usage Pattern (Kotlin):
 *   val handle = nativeAllocate(width, height, format)
 *   // ... camera thread writes frames via UVCPreview integration
 *   val buffer = nativeAcquireBuffer(handle)  // returns HardwareBuffer
 *   // ... render with buffer
 *   nativeReleaseBuffer(handle)
 *   nativeDestroy(handle)
 */

//======================================================================
// Lifecycle methods
//======================================================================

/**
 * Allocate a new FrameBufferRing with the specified dimensions.
 * @param width Frame width in pixels
 * @param height Frame height in pixels
 * @param format AHardwareBuffer format (default: AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM)
 * @return Native handle (generation-encoded), or 0 on failure
 */
static jlong nativeFrameBufferAllocate(JNIEnv *env, jobject thiz,
    jint width, jint height, jint format) {

    ENTER();

    FrameBufferRing *ring = new FrameBufferRing();
    if (UNLIKELY(!ring)) {
        LOGE("Failed to create FrameBufferRing");
        RETURN(0, jlong);
    }

    // Use RGBA8888 if format not specified
    uint32_t hwFormat = (format != 0) ? static_cast<uint32_t>(format)
                                      : AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;

    int result = ring->allocate(static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height),
                                hwFormat);
    if (UNLIKELY(result != 0)) {
        LOGE("FrameBufferRing::allocate failed: %d", result);
        delete ring;
        RETURN(0, jlong);
    }

    // Register with HandleManager for safe reference counting
    jlong handle = getRingBufferHandleManager().registerContext(ring);
    if (handle == INVALID_HANDLE) {
        LOGE("Failed to register FrameBufferRing with HandleManager");
        ring->destroy();
        delete ring;
        RETURN(0, jlong);
    }

    LOGI("FrameBufferRing allocated: %dx%d (format: 0x%x)", width, height, hwFormat);
    LOGI("HANDLE_DIAG: nativeFrameBufferAllocate ring=%p handle=0x%llx",
         ring, (unsigned long long)handle);
    RETURN(handle, jlong);
}

/**
 * Destroy the FrameBufferRing and release all AHardwareBuffers.
 * Uses HandleManager::invalidateAndFree to safely wait for all active
 * JNI calls to complete before deleting the object.
 *
 * @param handle Native handle from nativeFrameBufferAllocate
 */
static void nativeFrameBufferDestroy(JNIEnv *env, jobject thiz, jlong handle) {
    ENTER();

    LOGI("LIFECYCLE: nativeFrameBufferDestroy called for handle=0x%llx",
         (unsigned long long)handle);

    // invalidateAndFree will:
    // 1. Mark the handle as dead (new acquires will fail)
    // 2. Wait for all active JNI calls using this handle to complete
    // 3. Return the context pointer for us to delete
    void* ctx = getRingBufferHandleManager().invalidateAndFree(handle);
    if (ctx) {
        FrameBufferRing *ring = static_cast<FrameBufferRing*>(ctx);

        // Verify magic before destroy (defense-in-depth)
        if (ring->validateMagic()) {
            LOGI("LIFECYCLE: Destroying FrameBufferRing %p", ring);
            ring->destroy();
            delete ring;
            LOGI("FrameBufferRing destroyed");
        } else {
            LOGE("LIFECYCLE: Magic validation failed for ring %p - memory corrupted!", ring);
            // Don't delete - memory is corrupted, better to leak than crash
        }
    } else {
        LOGW("LIFECYCLE: nativeFrameBufferDestroy - handle 0x%llx was already invalid",
             (unsigned long long)handle);
    }

    EXIT();
}

//======================================================================
// Consumer API (render thread)
//======================================================================

/**
 * Acquire the most recent completed frame buffer.
 * The returned HardwareBuffer can be used with EGLImage or ImageReader.
 *
 * IMPORTANT: Caller MUST call nativeFrameBufferReleaseBuffer after use
 * to decrement the reference count.
 *
 * @param handle Native handle from nativeFrameBufferAllocate
 * @return HardwareBuffer object, or null if no frame available
 */
static jobject nativeFrameBufferAcquireBuffer(JNIEnv *env, jobject thiz, jlong handle) {
    ENTER();

    auto ref = acquireRingBuffer(handle, "acquireBuffer");
    if (!ref) {
        RETURN(nullptr, jobject);
    }
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);

    // HANDLE_DIAG: Log acquire attempts for pointer correlation
    static std::atomic<int> acquireCount{0};
    int count = ++acquireCount;
    if (count <= 10 || count % 500 == 0) {
        int latest = ring->getLatestCompleted();
        StreamTelemetry *telemetry = ring->getTelemetry();
        uint64_t received = telemetry ?
            telemetry->framesReceived.load(std::memory_order_relaxed) : 0;
        LOGI("HANDLE_DIAG: Consumer acquireBuffer[%d] handle=0x%llx ring=%p latest=%d received=%llu",
             count, (unsigned long long)handle, ring, latest, (unsigned long long)received);
    }

    FrameSlotMetadata metadata;
    AHardwareBuffer *buffer = ring->acquireReadBuffer(&metadata);
    if (UNLIKELY(!buffer)) {
        // No frame available yet - not an error
        RETURN(nullptr, jobject);
    }

    // Convert AHardwareBuffer to Java HardwareBuffer
    // Note: This increments the reference count in Java side
    jobject hwBuffer = AHardwareBuffer_toHardwareBuffer(env, buffer);
    if (UNLIKELY(!hwBuffer)) {
        LOGE("AHardwareBuffer_toHardwareBuffer failed");
        // Release our reference since we can't return it to Java
        ring->releaseReadBuffer();
        RETURN(nullptr, jobject);
    }

    RETURN(hwBuffer, jobject);
    // ~ScopedRef automatically decrements HandleManager activeRefs
}

/**
 * Release the previously acquired frame buffer.
 * Decrements the AHardwareBuffer reference count.
 *
 * @param handle Native handle from nativeFrameBufferAllocate
 */
static void nativeFrameBufferReleaseBuffer(JNIEnv *env, jobject thiz, jlong handle) {
    ENTER();

    auto ref = acquireRingBuffer(handle, "releaseBuffer");
    if (ref) {
        FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
        ring->releaseReadBuffer();
    }

    EXIT();
}

//======================================================================
// Bidirectional Fence API (Phase 4)
//======================================================================

/**
 * Get the acquire fence for the most recently acquired buffer.
 * The fence signals when the producer (CPU) has finished writing.
 * Consumer should import this into EGL for GPU synchronization.
 *
 * OWNERSHIP: Returns a dup'd FD - caller owns it and MUST close it.
 * This prevents fd leaks and double-close issues.
 *
 * @param handle Native handle from nativeFrameBufferAllocate
 * @return Fence file descriptor (caller owns), or -1 if no fence
 */
static jint nativeFrameBufferGetAcquireFence(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getAcquireFence");
    if (!ref) return -1;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);

    int idx = ring->getCurrentReadIndex();
    if (idx < 0) return -1;

    FrameSlotMetadata *meta = ring->getMetadata(idx);
    if (!meta || meta->acquireFenceFd < 0) return -1;

    // CRITICAL: dup() the FD so caller owns their copy
    int dupedFd = dup(meta->acquireFenceFd);
    if (dupedFd < 0) {
        LOGW("dup(acquireFenceFd) failed: %s", strerror(errno));
        return -1;
    }

    return dupedFd;
}

/**
 * Get the frame number for the most recently acquired buffer.
 * Used for race-safe release - consumer returns this with GPU fence
 * to identify the correct slot even if indices have advanced.
 *
 * @param handle Native handle from nativeFrameBufferAllocate
 * @return Frame number (monotonic counter), or -1 on error
 */
static jlong nativeFrameBufferGetFrameNumber(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getFrameNumber");
    if (!ref) return -1;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);

    int idx = ring->getCurrentReadIndex();
    if (idx < 0) return -1;

    FrameSlotMetadata *meta = ring->getMetadata(idx);
    return meta ? static_cast<jlong>(meta->frameNumber) : -1;
}

/**
 * Release the frame buffer with a GPU release fence.
 * This enables bidirectional fence synchronization:
 * - Consumer provides fence from eglDupNativeFenceFDANDROID
 * - Producer will wait on this fence before writing to the slot
 *
 * @param handle Native handle from nativeFrameBufferAllocate
 * @param frameNumber Frame number from nativeFrameBufferGetFrameNumber
 * @param gpuReleaseFenceFd GPU release fence fd (native takes ownership)
 */
static void nativeFrameBufferReleaseWithFence(JNIEnv *env, jobject thiz,
    jlong handle, jlong frameNumber, jint gpuReleaseFenceFd) {

    ENTER();

    auto ref = acquireRingBuffer(handle, "releaseWithFence");
    if (!ref) {
        if (gpuReleaseFenceFd >= 0) close(gpuReleaseFenceFd);
        EXIT();
        return;
    }
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);

    int slotIndex = ring->findSlotByFrameNumber(static_cast<uint64_t>(frameNumber));

    if (slotIndex < 0) {
        // Frame already recycled; close fence to prevent leak
        if (gpuReleaseFenceFd >= 0) close(gpuReleaseFenceFd);
        LOGW("Frame %lld already recycled, fence discarded", (long long)frameNumber);
    } else {
        ring->setGpuReleaseFence(slotIndex, gpuReleaseFenceFd);
    }

    ring->releaseReadBuffer();

    EXIT();
}

//======================================================================
// State queries
//======================================================================

/**
 * Check if the FrameBufferRing is allocated and ready.
 * @param handle Native handle
 * @return true if allocated
 */
static jboolean nativeFrameBufferIsAllocated(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "isAllocated");
    if (!ref) return JNI_FALSE;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    return ring->isAllocated() ? JNI_TRUE : JNI_FALSE;
}

/**
 * Get the allocated frame width.
 * @param handle Native handle
 * @return Width in pixels, or 0 if not allocated
 */
static jint nativeFrameBufferGetWidth(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getWidth");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    return static_cast<jint>(ring->getWidth());
}

/**
 * Get the allocated frame height.
 * @param handle Native handle
 * @return Height in pixels, or 0 if not allocated
 */
static jint nativeFrameBufferGetHeight(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getHeight");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    return static_cast<jint>(ring->getHeight());
}

//======================================================================
// Telemetry - Basic Counters
//======================================================================

/**
 * Get the number of frames received by the producer.
 * @param handle Native handle
 * @return Frame count
 */
static jlong nativeFrameBufferGetFramesReceived(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getFramesReceived");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->framesReceived.load(std::memory_order_relaxed));
}

/**
 * Get the number of frames rendered by the consumer.
 * @param handle Native handle
 * @return Frame count
 */
static jlong nativeFrameBufferGetFramesRendered(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getFramesRendered");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->framesRendered.load(std::memory_order_relaxed));
}

/**
 * Get the number of frames dropped (not rendered).
 * @param handle Native handle
 * @return Dropped frame count
 */
static jlong nativeFrameBufferGetFramesDropped(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getFramesDropped");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->framesDropped.load(std::memory_order_relaxed));
}

/**
 * Get the number of frames corrupted (conversion failed).
 * @param handle Native handle
 * @return Corrupted frame count
 */
static jlong nativeFrameBufferGetFramesCorrupted(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getFramesCorrupted");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->framesCorrupted.load(std::memory_order_relaxed));
}

//======================================================================
// Telemetry - Ring Buffer State
//======================================================================

/**
 * Get the number of producer stalls (consumer too slow).
 * @param handle Native handle
 * @return Stall count
 */
static jlong nativeFrameBufferGetProducerStalls(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getProducerStalls");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->producerStalls.load(std::memory_order_relaxed));
}

/**
 * Get the number of consumer starves (producer too slow).
 * @param handle Native handle
 * @return Starve count
 */
static jlong nativeFrameBufferGetConsumerStarves(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getConsumerStarves");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->consumerStarves.load(std::memory_order_relaxed));
}

/**
 * Get ring buffer slot states as an int array.
 * Values: 0=EMPTY, 1=WRITING, 2=READY, 3=READING
 * @param handle Native handle
 * @return int[3] array of slot states
 */
static jintArray nativeFrameBufferGetSlotStates(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getSlotStates");
    if (!ref) return nullptr;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);

    StreamTelemetry *telemetry = ring->getTelemetry();
    jintArray result = env->NewIntArray(FRAME_BUFFER_COUNT);
    if (!result) return nullptr;

    jint states[FRAME_BUFFER_COUNT];
    for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
        states[i] = telemetry->slotStates[i].load(std::memory_order_relaxed);
    }
    env->SetIntArrayRegion(result, 0, FRAME_BUFFER_COUNT, states);

    return result;
}

//======================================================================
// Telemetry - Timing
//======================================================================

/**
 * Get average decode time in microseconds.
 * Uses exponential moving average (alpha = 0.125).
 * @param handle Native handle
 * @return Average decode time in microseconds
 */
static jlong nativeFrameBufferGetAvgDecodeTimeUs(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getAvgDecodeTimeUs");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return telemetry->avgDecodeTimeUs.load(std::memory_order_relaxed);
}

/**
 * Get total cumulative fence wait time in nanoseconds.
 * @param handle Native handle
 * @return Total fence wait time in nanoseconds
 */
static jlong nativeFrameBufferGetFenceWaitTimeNs(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getFenceWaitTimeNs");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->fenceWaitTimeNs.load(std::memory_order_relaxed));
}

/**
 * Get total number of fence waits.
 * @param handle Native handle
 * @return Number of fence waits
 */
static jlong nativeFrameBufferGetTotalFenceWaits(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getTotalFenceWaits");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->totalFenceWaits.load(std::memory_order_relaxed));
}

//======================================================================
// Telemetry - Error Recording
//======================================================================

/**
 * Record an error into the telemetry circular buffer.
 * Called from Java when errors are detected at higher layers.
 * @param handle Native handle
 * @param errorCode Error code (libuvc, libusb, or app-specific)
 * @param source Error source string (max 15 chars)
 */
static void nativeFrameBufferRecordError(JNIEnv *env, jobject thiz,
    jlong handle, jint errorCode, jstring source) {

    auto ref = acquireRingBuffer(handle, "recordError");
    if (!ref) return;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);

    const char *sourceStr = env->GetStringUTFChars(source, nullptr);
    if (sourceStr) {
        StreamTelemetry *telemetry = ring->getTelemetry();
        telemetry->recordError(errorCode, sourceStr);
        env->ReleaseStringUTFChars(source, sourceStr);
    }
}

/**
 * Get the error history count (up to ERROR_HISTORY_SIZE).
 * @param handle Native handle
 * @return Number of errors in history (max 8)
 */
static jint nativeFrameBufferGetErrorCount(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getErrorCount");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return telemetry->errorHistoryCount.load(std::memory_order_relaxed);
}

//======================================================================
// Telemetry - Stream Negotiation
//======================================================================

/**
 * Set USB protocol details for telemetry.
 * Called when stream is negotiated.
 */
static void nativeFrameBufferSetUsbProtocol(JNIEnv *env, jobject thiz,
    jlong handle, jint endpoint, jint altSetting, jboolean isIsochronous, jint maxPacketSize) {

    auto ref = acquireRingBuffer(handle, "setUsbProtocol");
    if (!ref) return;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);

    StreamTelemetry *telemetry = ring->getTelemetry();
    telemetry->setUsbProtocol(
        static_cast<uint8_t>(endpoint),
        static_cast<uint8_t>(altSetting),
        isIsochronous == JNI_TRUE,
        static_cast<uint32_t>(maxPacketSize)
    );
}

/**
 * Set negotiated stream parameters for telemetry.
 */
static void nativeFrameBufferSetNegotiatedParams(JNIEnv *env, jobject thiz,
    jlong handle, jint width, jint height, jint fps) {

    auto ref = acquireRingBuffer(handle, "setNegotiatedParams");
    if (!ref) return;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);

    StreamTelemetry *telemetry = ring->getTelemetry();
    telemetry->negotiatedWidth = static_cast<uint32_t>(width);
    telemetry->negotiatedHeight = static_cast<uint32_t>(height);
    telemetry->negotiatedFps = static_cast<uint32_t>(fps);
    telemetry->markStreamStart();
}

/**
 * Get the fallback level (0=NORMAL, 1=FPS_REDUCED, etc.).
 * @param handle Native handle
 * @return Current fallback level
 */
static jint nativeFrameBufferGetFallbackLevel(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getFallbackLevel");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jint>(telemetry->fallbackLevel.load(std::memory_order_relaxed));
}

/**
 * Set fallback level with reason string.
 */
static void nativeFrameBufferSetFallbackLevel(JNIEnv *env, jobject thiz,
    jlong handle, jint level, jstring reason) {

    auto ref = acquireRingBuffer(handle, "setFallbackLevel");
    if (!ref) return;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);

    const char *reasonStr = nullptr;
    if (reason) {
        reasonStr = env->GetStringUTFChars(reason, nullptr);
    }

    StreamTelemetry *telemetry = ring->getTelemetry();
    telemetry->setFallback(static_cast<uint32_t>(level), reasonStr);

    if (reasonStr) {
        env->ReleaseStringUTFChars(reason, reasonStr);
    }
}

//======================================================================
// Telemetry - USB Layer (called from higher layers)
//======================================================================

/**
 * Increment USB packet received counter.
 */
static void nativeFrameBufferOnUsbPacket(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "onUsbPacket");
    if (!ref) return;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    telemetry->usbPacketsReceived.fetch_add(1, std::memory_order_relaxed);
}

/**
 * Increment USB overflow error counter.
 */
static void nativeFrameBufferOnUsbOverflow(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "onUsbOverflow");
    if (!ref) return;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    telemetry->usbOverflowErrors.fetch_add(1, std::memory_order_relaxed);
}

/**
 * Increment USB timeout error counter.
 */
static void nativeFrameBufferOnUsbTimeout(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "onUsbTimeout");
    if (!ref) return;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    telemetry->usbTimeoutErrors.fetch_add(1, std::memory_order_relaxed);
}

/**
 * Get USB packets received count.
 */
static jlong nativeFrameBufferGetUsbPacketsReceived(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getUsbPacketsReceived");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->usbPacketsReceived.load(std::memory_order_relaxed));
}

/**
 * Get USB overflow error count.
 */
static jlong nativeFrameBufferGetUsbOverflowErrors(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getUsbOverflowErrors");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->usbOverflowErrors.load(std::memory_order_relaxed));
}

/**
 * Get USB timeout error count.
 */
static jlong nativeFrameBufferGetUsbTimeoutErrors(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getUsbTimeoutErrors");
    if (!ref) return 0;
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->usbTimeoutErrors.load(std::memory_order_relaxed));
}

/**
 * Get complete telemetry snapshot as a direct ByteBuffer.
 * Much more efficient than the 29-parameter constructor approach.
 *
 * The ByteBuffer contains 36 int64_t values (288 bytes total).
 * Use TelemetryPackedBuffer::Field enum for field indices.
 *
 * IMPORTANT: The returned ByteBuffer is a view into native memory.
 * The data is only valid until the next call to this method or
 * until the FrameBufferRing is destroyed.
 *
 * @param handle Native FrameBufferRing handle
 * @return Direct ByteBuffer with packed telemetry, or null on error
 */
static jobject nativeGetTelemetryBuffer(JNIEnv *env, jobject thiz, jlong handle) {
    auto ref = acquireRingBuffer(handle, "getTelemetryBuffer");
    if (!ref) {
        return nullptr;
    }
    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);

    StreamTelemetry *telemetry = ring->getTelemetry();
    if (UNLIKELY(!telemetry)) {
        LOGE("nativeGetTelemetryBuffer: Telemetry not available");
        return nullptr;
    }

    // Pack current state into the buffer
    telemetry->packForJni();

    // Create direct ByteBuffer pointing to packed data
    // Note: ByteBuffer capacity is in bytes
    jobject buffer = env->NewDirectByteBuffer(
        telemetry->packedBuffer.data,
        TelemetryPackedBuffer::BUFFER_SIZE
    );

    if (UNLIKELY(!buffer)) {
        LOGE("nativeGetTelemetryBuffer: NewDirectByteBuffer failed");
        env->ExceptionClear();
        return nullptr;
    }

    return buffer;
}

//======================================================================
// Snapshot API (Phase 4 - captureToFd)
//======================================================================

/**
 * Capture current frame to file descriptor as JPEG.
 *
 * Uses FD-based I/O for Android scoped storage compatibility.
 * The fd should be obtained from ContentResolver or ParcelFileDescriptor.
 *
 * @param handle Native FrameBufferRing handle
 * @param fd Open file descriptor (caller manages lifecycle)
 * @param quality JPEG quality (1-100)
 * @return 0 on success, negative error code on failure:
 *         -1: No frame available or invalid handle
 *         -2: Failed to lock buffer for CPU read
 *         -3: Unsupported buffer format
 *         -4: JPEG compression failed
 *         -5: Write to fd failed
 */
static jint nativeFrameBufferCaptureToFd(JNIEnv *env, jobject thiz,
                                          jlong handle, jint fd, jint quality) {
    ENTER();

    auto ref = acquireRingBuffer(handle, "captureToFd");
    if (!ref) {
        RETURN(-1, jint);
    }

    FrameBufferRing *ring = static_cast<FrameBufferRing*>(ref.ptr);
    jint result = ring->captureToFd(fd, quality);

    RETURN(result, jint);
}

//======================================================================
// JNI Registration
//======================================================================

static JNINativeMethod frameBufferMethods[] = {
    // Lifecycle
    { "nativeFrameBufferAllocate",       "(III)J",                              (void *) nativeFrameBufferAllocate },
    { "nativeFrameBufferDestroy",        "(J)V",                                (void *) nativeFrameBufferDestroy },
    // Consumer API
    { "nativeFrameBufferAcquireBuffer",  "(J)Landroid/hardware/HardwareBuffer;", (void *) nativeFrameBufferAcquireBuffer },
    { "nativeFrameBufferReleaseBuffer",  "(J)V",                                (void *) nativeFrameBufferReleaseBuffer },
    // Bidirectional Fence API (Phase 4)
    { "nativeFrameBufferGetAcquireFence", "(J)I",                               (void *) nativeFrameBufferGetAcquireFence },
    { "nativeFrameBufferGetFrameNumber",  "(J)J",                               (void *) nativeFrameBufferGetFrameNumber },
    { "nativeFrameBufferReleaseWithFence", "(JJI)V",                            (void *) nativeFrameBufferReleaseWithFence },
    // State queries
    { "nativeFrameBufferIsAllocated",    "(J)Z",                                (void *) nativeFrameBufferIsAllocated },
    { "nativeFrameBufferGetWidth",       "(J)I",                                (void *) nativeFrameBufferGetWidth },
    { "nativeFrameBufferGetHeight",      "(J)I",                                (void *) nativeFrameBufferGetHeight },
    // Telemetry - Basic Counters
    { "nativeFrameBufferGetFramesReceived", "(J)J",                             (void *) nativeFrameBufferGetFramesReceived },
    { "nativeFrameBufferGetFramesRendered", "(J)J",                             (void *) nativeFrameBufferGetFramesRendered },
    { "nativeFrameBufferGetFramesDropped",  "(J)J",                             (void *) nativeFrameBufferGetFramesDropped },
    { "nativeFrameBufferGetFramesCorrupted", "(J)J",                            (void *) nativeFrameBufferGetFramesCorrupted },
    // Telemetry - Ring Buffer State
    { "nativeFrameBufferGetProducerStalls", "(J)J",                             (void *) nativeFrameBufferGetProducerStalls },
    { "nativeFrameBufferGetConsumerStarves", "(J)J",                            (void *) nativeFrameBufferGetConsumerStarves },
    { "nativeFrameBufferGetSlotStates",  "(J)[I",                               (void *) nativeFrameBufferGetSlotStates },
    // Telemetry - Timing
    { "nativeFrameBufferGetAvgDecodeTimeUs", "(J)J",                            (void *) nativeFrameBufferGetAvgDecodeTimeUs },
    { "nativeFrameBufferGetFenceWaitTimeNs", "(J)J",                            (void *) nativeFrameBufferGetFenceWaitTimeNs },
    { "nativeFrameBufferGetTotalFenceWaits", "(J)J",                            (void *) nativeFrameBufferGetTotalFenceWaits },
    // Telemetry - Error Recording
    { "nativeFrameBufferRecordError",    "(JILjava/lang/String;)V",             (void *) nativeFrameBufferRecordError },
    { "nativeFrameBufferGetErrorCount",  "(J)I",                                (void *) nativeFrameBufferGetErrorCount },
    // Telemetry - Stream Negotiation
    { "nativeFrameBufferSetUsbProtocol", "(JIIZI)V",                            (void *) nativeFrameBufferSetUsbProtocol },
    { "nativeFrameBufferSetNegotiatedParams", "(JIII)V",                        (void *) nativeFrameBufferSetNegotiatedParams },
    { "nativeFrameBufferGetFallbackLevel", "(J)I",                              (void *) nativeFrameBufferGetFallbackLevel },
    { "nativeFrameBufferSetFallbackLevel", "(JILjava/lang/String;)V",           (void *) nativeFrameBufferSetFallbackLevel },
    // Telemetry - USB Layer
    { "nativeFrameBufferOnUsbPacket",    "(J)V",                                (void *) nativeFrameBufferOnUsbPacket },
    { "nativeFrameBufferOnUsbOverflow",  "(J)V",                                (void *) nativeFrameBufferOnUsbOverflow },
    { "nativeFrameBufferOnUsbTimeout",   "(J)V",                                (void *) nativeFrameBufferOnUsbTimeout },
    { "nativeFrameBufferGetUsbPacketsReceived", "(J)J",                         (void *) nativeFrameBufferGetUsbPacketsReceived },
    { "nativeFrameBufferGetUsbOverflowErrors", "(J)J",                          (void *) nativeFrameBufferGetUsbOverflowErrors },
    { "nativeFrameBufferGetUsbTimeoutErrors", "(J)J",                           (void *) nativeFrameBufferGetUsbTimeoutErrors },
    // ByteBuffer Telemetry (efficient packed transfer)
    { "nativeGetTelemetryBuffer", "(J)Ljava/nio/ByteBuffer;",                   (void *) nativeGetTelemetryBuffer },
    // Snapshot API (Phase 4)
    { "nativeFrameBufferCaptureToFd", "(JII)I",                                 (void *) nativeFrameBufferCaptureToFd },
};

extern jint registerNativeMethods(JNIEnv* env, const char *class_name,
                                  JNINativeMethod *methods, int num_methods);

int register_framebuffer(JNIEnv *env) {
    LOGV("register_framebuffer (v2.1 - classloader-safe):");
    const int methodCount = NUM_ARRAY_ELEMENTS(frameBufferMethods);

    // PRIMARY: Register to FrameBufferManager (ScopeCam consumer)
    // This class is present in the ScopeCam app, which loads this library.
    jclass fbManagerClass = env->FindClass("com/scopecam/camera/buffer/FrameBufferManager");
    if (fbManagerClass != nullptr) {
        jint result = env->RegisterNatives(fbManagerClass, frameBufferMethods, methodCount);
        env->DeleteLocalRef(fbManagerClass);

        if (result == JNI_OK) {
            LOGI("FrameBufferJNI: Registered %d methods to FrameBufferManager", methodCount);
        } else {
            LOGE("FrameBufferJNI: FrameBufferManager registration failed (error %d)", result);
            return -1;
        }
    } else {
        // CRITICAL: Clear exception to prevent cascade abort
        env->ExceptionClear();
        LOGW("FrameBufferJNI: FrameBufferManager not found (ScopeCam app not present)");
    }

    // NOTE: UVCCamera dual-registration removed in v2.2 (2026-01-02)
    // Reason: nativeGetTelemetryBuffer was added to native but not declared in
    // UVCCamera.java, causing RegisterNatives to fail. Since ScopeCam only uses
    // FrameBufferManager, legacy UVCCamera registration is not needed.
    // See: JNI method count mismatch (33 native vs 32 Java declarations)

    return 0;
}
