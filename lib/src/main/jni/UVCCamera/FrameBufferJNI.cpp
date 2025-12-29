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
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>

#include "FrameBufferRing.h"

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
 * @return Native handle (pointer), or 0 on failure
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

    LOGI("FrameBufferRing allocated: %dx%d (format: 0x%x)", width, height, hwFormat);
    RETURN(reinterpret_cast<jlong>(ring), jlong);
}

/**
 * Destroy the FrameBufferRing and release all AHardwareBuffers.
 * @param handle Native handle from nativeFrameBufferAllocate
 */
static void nativeFrameBufferDestroy(JNIEnv *env, jobject thiz, jlong handle) {
    ENTER();

    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    if (LIKELY(ring)) {
        ring->destroy();
        delete ring;
        LOGI("FrameBufferRing destroyed");
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

    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    if (UNLIKELY(!ring)) {
        LOGE("Invalid FrameBufferRing handle");
        RETURN(nullptr, jobject);
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
}

/**
 * Release the previously acquired frame buffer.
 * Decrements the AHardwareBuffer reference count.
 *
 * @param handle Native handle from nativeFrameBufferAllocate
 */
static void nativeFrameBufferReleaseBuffer(JNIEnv *env, jobject thiz, jlong handle) {
    ENTER();

    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    if (LIKELY(ring)) {
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
 * Note: Fence fd ownership remains with native - consumer should dup() if needed.
 *
 * @param handle Native handle from nativeFrameBufferAllocate
 * @return Fence file descriptor, or -1 if no fence
 */
static jint nativeFrameBufferGetAcquireFence(JNIEnv *env, jobject thiz, jlong handle) {
    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    if (UNLIKELY(!ring)) return -1;

    int idx = ring->getCurrentReadIndex();
    if (idx < 0) return -1;

    FrameSlotMetadata *meta = ring->getMetadata(idx);
    return meta ? meta->acquireFenceFd : -1;
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
    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    if (UNLIKELY(!ring)) return -1;

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

    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    if (UNLIKELY(!ring)) {
        if (gpuReleaseFenceFd >= 0) close(gpuReleaseFenceFd);
        EXIT();
        return;
    }

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
    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    return (ring && ring->isAllocated()) ? JNI_TRUE : JNI_FALSE;
}

/**
 * Get the allocated frame width.
 * @param handle Native handle
 * @return Width in pixels, or 0 if not allocated
 */
static jint nativeFrameBufferGetWidth(JNIEnv *env, jobject thiz, jlong handle) {
    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    return ring ? static_cast<jint>(ring->getWidth()) : 0;
}

/**
 * Get the allocated frame height.
 * @param handle Native handle
 * @return Height in pixels, or 0 if not allocated
 */
static jint nativeFrameBufferGetHeight(JNIEnv *env, jobject thiz, jlong handle) {
    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    return ring ? static_cast<jint>(ring->getHeight()) : 0;
}

//======================================================================
// Telemetry
//======================================================================

/**
 * Get the number of frames received by the producer.
 * @param handle Native handle
 * @return Frame count
 */
static jlong nativeFrameBufferGetFramesReceived(JNIEnv *env, jobject thiz, jlong handle) {
    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    if (UNLIKELY(!ring)) return 0;
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->framesReceived.load(std::memory_order_relaxed));
}

/**
 * Get the number of frames rendered by the consumer.
 * @param handle Native handle
 * @return Frame count
 */
static jlong nativeFrameBufferGetFramesRendered(JNIEnv *env, jobject thiz, jlong handle) {
    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    if (UNLIKELY(!ring)) return 0;
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->framesRendered.load(std::memory_order_relaxed));
}

/**
 * Get the number of frames dropped (not rendered).
 * @param handle Native handle
 * @return Dropped frame count
 */
static jlong nativeFrameBufferGetFramesDropped(JNIEnv *env, jobject thiz, jlong handle) {
    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    if (UNLIKELY(!ring)) return 0;
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->framesDropped.load(std::memory_order_relaxed));
}

/**
 * Get the number of frames corrupted (conversion failed).
 * @param handle Native handle
 * @return Corrupted frame count
 */
static jlong nativeFrameBufferGetFramesCorrupted(JNIEnv *env, jobject thiz, jlong handle) {
    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    if (UNLIKELY(!ring)) return 0;
    StreamTelemetry *telemetry = ring->getTelemetry();
    return static_cast<jlong>(telemetry->framesCorrupted.load(std::memory_order_relaxed));
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
    // Telemetry
    { "nativeFrameBufferGetFramesReceived", "(J)J",                             (void *) nativeFrameBufferGetFramesReceived },
    { "nativeFrameBufferGetFramesRendered", "(J)J",                             (void *) nativeFrameBufferGetFramesRendered },
    { "nativeFrameBufferGetFramesDropped",  "(J)J",                             (void *) nativeFrameBufferGetFramesDropped },
    { "nativeFrameBufferGetFramesCorrupted", "(J)J",                            (void *) nativeFrameBufferGetFramesCorrupted },
};

extern jint registerNativeMethods(JNIEnv* env, const char *class_name,
                                  JNINativeMethod *methods, int num_methods);

int register_framebuffer(JNIEnv *env) {
    LOGV("register_framebuffer:");
    // Register with the existing UVCCamera class for now
    // This can be moved to a separate FrameBufferManager class later
    if (registerNativeMethods(env,
        "com/serenegiant/usb/UVCCamera",
        frameBufferMethods, NUM_ARRAY_ELEMENTS(frameBufferMethods)) < 0) {
        return -1;
    }
    return 0;
}
