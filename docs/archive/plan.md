# Phase 1 Revised: Native Dual-Emit Architecture (Corrected)

## Critical Implementation Directives (Read First)

1.  **Language Constraint**: `UVCCamera` is a Java class. Do **NOT** convert it to Kotlin. All library modifications must remain in Java to preserve binary compatibility and minimize build risk.
2.  **Module Boundary**: `ICaptureFrameCallback` must be defined in the `lib` module (`com.serenegiant.usb` package), NOT in an app module. This ensures the native layer can access it via JNI.
3.  **JNI Target**: The active JNI bridge file is `serenegiant_usb_UVCCamera.cpp`. Do NOT modify or create `_UVCCamera.cpp`.
4.  **Thread Safety**: The native capture callback must use the `compare_exchange_strong` pattern to ensure non-blocking behavior. The display path (Ring Buffer) has priority over the recording path.

---

## Executive Summary

The original Phase 1 plan assumed `frameStream` would receive frames via `IFrameCallback`. Discovery revealed that ring buffer mode bypasses this path entirely at the native level. This revised plan implements **Native Dual-Emit**: the conversion thread writes to the ring buffer (display) AND emits frames to a capture callback (recording/photo), with configurable format and frame rate.

---

# Part 1: Native Code & Library Plan

## Objective

Modify `UVCPreview.cpp`, `UVCCamera.java` and supporting files to emit frames to BOTH the ring buffer (display) AND a capture callback (recording), with:
- Configurable capture format (RGBX, NV21, YUYV)
- Configurable capture frame rate (decimate to target FPS)
- Non-blocking drop policy (never stall display for slow capture consumer)
- Telemetry for capture path metrics

---

## Architecture Overview

```
USB Isochronous Transfer
        │
        ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    uvc_preview_frame_callback()                      │
│                                                                      │
│    Raw MJPEG/YUYV → SPSC Queue → signalConversionThread()           │
└─────────────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      do_conversion_loop()                            │
│  ┌────────────────────────────────────────────────────────────────┐ │
│  │  1. Dequeue raw frame from SPSC                                 │ │
│  │  2. Convert to RGBX                                             │ │
│  │  3. Write to AHardwareBuffer (DISPLAY PATH) ──────────────────┐ │ │
│  │  4. IF capture enabled AND frame decimation allows:           │ │ │
│  │     a. Convert RGBX → capture format (if needed)              │ │ │
│  │     b. TryEmit to capture callback (non-blocking)  ─────────┐ │ │ │
│  │  5. Update telemetry                                        │ │ │ │
│  └─────────────────────────────────────────────────────────────┼─┼─┘ │
└────────────────────────────────────────────────────────────────┼─┼───┘
                                                                 │ │
                    ┌────────────────────────────────────────────┘ │
                    ▼                                              ▼
        ┌─────────────────────┐                      ┌─────────────────────┐
        │   CAPTURE PATH      │                      │   DISPLAY PATH      │
        │                     │                      │                     │
        │ JNI Callback        │                      │ HardwareBuffer      │
        │ → ICaptureFrameCallback (Java)             │ → GPU Renderer      │
        │ → Kotlin frameStream│                      │ → EGL SwapBuffers   │
        │ → VideoRecording    │                      │                     │
        └─────────────────────┘                      └─────────────────────┘
```

---

## File Changes

### 1. UVCPreview.h

**Location**: `lib/src/main/jni/UVCCamera/UVCPreview.h`

Add new members and methods:

```cpp
// === ADD: Capture callback configuration ===

// Capture pixel formats (matches Java/Kotlin CaptureFormat enum)
enum CapturePixelFormat {
    CAPTURE_FORMAT_RGBX = 0,    // Direct from conversion (fastest)
    CAPTURE_FORMAT_NV21 = 1,    // YUV420SP - efficient for MediaCodec
    CAPTURE_FORMAT_YUYV = 2,    // YUV422 - raw sensor format
    CAPTURE_FORMAT_I420 = 3     // YUV420P - universal compatibility
};

// Capture callback function type
// Parameters: data, dataSize, width, height, format, timestampNs
typedef void (*captureCallbackFunc_t)(
    void* userData,
    const uint8_t* data,
    size_t dataSize,
    int width,
    int height,
    int format,
    int64_t timestampNs
);

private:
    // === Capture callback state ===
    std::atomic<bool> mCaptureEnabled{false};
    std::atomic<captureCallbackFunc_t> mCaptureCallback{nullptr};
    std::atomic<void*> mCaptureUserData{nullptr};
    std::atomic<CapturePixelFormat> mCaptureFormat{CAPTURE_FORMAT_NV21};

    // === Frame rate decimation ===
    std::atomic<int> mCaptureTargetFps{30};
    std::atomic<int> mPreviewFps{30};
    std::atomic<uint64_t> mCaptureFrameCounter{0};

    // === Capture buffer (reused to avoid allocation) ===
    uint8_t* mCaptureBuffer{nullptr};
    size_t mCaptureBufferCapacity{0};
    std::mutex mCaptureBufferMutex;

    // === Capture telemetry ===
    std::atomic<uint64_t> mCaptureFramesEmitted{0};
    std::atomic<uint64_t> mCaptureFramesDropped{0};
    std::atomic<uint64_t> mCaptureCallbackBusy{0};
    std::atomic<bool> mCaptureCallbackInProgress{false};

    // === Private methods ===
    bool shouldEmitCaptureFrame();
    void emitCaptureFrame(const uint8_t* rgbxData, int width, int height, int64_t timestampNs);
    // Note: __restrict hints help compiler optimize memory access patterns
    void convertRgbxToNv21(const uint8_t* __restrict rgbx, uint8_t* __restrict nv21, int width, int height);
    void convertRgbxToYuyv(const uint8_t* __restrict rgbx, uint8_t* __restrict yuyv, int width, int height);
    void convertRgbxToI420(const uint8_t* __restrict rgbx, uint8_t* __restrict i420, int width, int height);
    size_t getCaptureBufferSize(int width, int height, CapturePixelFormat format);

public:
    // === Capture API ===
    int setCaptureCallback(captureCallbackFunc_t callback, void* userData);
    int setCaptureFormat(CapturePixelFormat format);
    int setCaptureFrameRate(int targetFps);
    int enableCapture(bool enable);

    // === Capture telemetry getters ===
    uint64_t getCaptureFramesEmitted() const;
    uint64_t getCaptureFramesDropped() const;
    uint64_t getCaptureCallbackBusy() const;
```

---

### 2. UVCPreview.cpp - Capture Configuration Methods

**Location**: `lib/src/main/jni/UVCCamera/UVCPreview.cpp` (Add after existing telemetry methods)

```cpp
//======================================================================
// Capture Callback Implementation (Dual-Emit Architecture)
//======================================================================

// ... (Implementation of setCaptureCallback, setCaptureFormat, etc. as originally planned) ...
// See "UVCPreview.cpp - Capture Configuration Methods" in original plan for code details
```

---

### 3. UVCPreview.cpp - Logic & Conversion

**Location**: `lib/src/main/jni/UVCCamera/UVCPreview.cpp`

Implement:
- `shouldEmitCaptureFrame()` (Decimation logic)
- `convertRgbxToNv21`, `convertRgbxToYuyv`, etc. (Color conversion)
- `emitCaptureFrame()` (The non-blocking drop logic)
- `do_conversion_loop()` (Add the emit call)
- `~UVCPreview()` (Cleanup)

*Note: Proceed with scalar conversion initially. If performance is bottlenecked, upgrade to NEON/libyuv later.*

---

### 4. JNI Bridge - serenegiant_usb_UVCCamera.cpp

**Location**: `lib/src/main/jni/UVCCamera/serenegiant_usb_UVCCamera.cpp`

**Corrected**: Use the existing JNI file, not `_UVCCamera.cpp`.

Add JNI methods for the new capture API:

```cpp
//======================================================================
// Capture Callback JNI Bridge
//======================================================================

// Global reference for capture callback
static jobject gCaptureCallbackObj = nullptr;
static jmethodID gCaptureCallbackMethod = nullptr;

/**
 * Native capture callback - bridges to Java/Kotlin.
 * Called from conversion thread.
 */
static void nativeCaptureCallback(
    void* userData,
    const uint8_t* data,
    size_t dataSize,
    int width,
    int height,
    int format,
    int64_t timestampNs
) {
    JNIEnv* env = nullptr;
    JavaVM* vm = getVM();

    bool attached = false;
    int envStatus = vm->GetEnv((void**)&env, JNI_VERSION_1_6);

    if (envStatus == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("CAPTURE_JNI: Failed to attach thread");
            return;
        }
        attached = true;
    }

    if (env && gCaptureCallbackObj && gCaptureCallbackMethod) {
        // Create DirectByteBuffer wrapping the native data
        // Note: This is safe because callback is synchronous within the scope of emitCaptureFrame
        jobject buffer = env->NewDirectByteBuffer((void*)data, dataSize);

        if (buffer) {
            env->CallVoidMethod(
                gCaptureCallbackObj,
                gCaptureCallbackMethod,
                buffer,
                width,
                height,
                format,
                timestampNs
            );

            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }

            env->DeleteLocalRef(buffer);
        }
    }

    if (attached) {
        vm->DetachCurrentThread();
    }
}

// ... JNI Export functions mapping to com.serenegiant.usb.UVCCamera ...
// Java_com_serenegiant_usb_UVCCamera_nativeSetCaptureCallback
// Java_com_serenegiant_usb_UVCCamera_nativeSetCaptureFormat
// Java_com_serenegiant_usb_UVCCamera_nativeSetCaptureFrameRate
// Java_com_serenegiant_usb_UVCCamera_nativeEnableCapture
// Java_com_serenegiant_usb_UVCCamera_nativeGetCaptureFramesEmitted
// Java_com_serenegiant_usb_UVCCamera_nativeGetCaptureFramesDropped
```

---

### 5. Define Interface (Java)

**Location**: `lib/src/main/java/com/serenegiant/usb/ICaptureFrameCallback.java`

**Corrected**: Defines the callback interface in the **Library Module** using **Java**.

```java
package com.serenegiant.usb;

import java.nio.ByteBuffer;

public interface ICaptureFrameCallback {
    /**
     * Called when a capture frame is ready.
     * @param buffer Direct ByteBuffer (valid only during call)
     */
    void onCaptureFrame(ByteBuffer buffer, int width, int height, int format, long timestampNs);
}
```

---

### 6. Update UVCCamera.java

**Location**: `lib/src/main/java/com/serenegiant/usb/UVCCamera.java`

**Corrected**: Add methods to the existing **Java** file.

```java
// Add import
import com.serenegiant.usb.ICaptureFrameCallback;

// ... inside UVCCamera class ...

    // === Capture API ===
    public void setCaptureCallback(final ICaptureFrameCallback callback) {
        if (mNativePtr != 0) {
            nativeSetCaptureCallback(mNativePtr, callback);
        }
    }

    public void setCaptureFormat(final int format) {
        if (mNativePtr != 0) {
            nativeSetCaptureFormat(mNativePtr, format);
        }
    }

    public void setCaptureFrameRate(final int targetFps) {
        if (mNativePtr != 0) {
            nativeSetCaptureFrameRate(mNativePtr, targetFps);
        }
    }

    public void enableCapture(final boolean enable) {
        if (mNativePtr != 0) {
            nativeEnableCapture(mNativePtr, enable);
        }
    }

    public long getCaptureFramesEmitted() {
        return (mNativePtr != 0) ? nativeGetCaptureFramesEmitted(mNativePtr) : 0;
    }

    public long getCaptureFramesDropped() {
        return (mNativePtr != 0) ? nativeGetCaptureFramesDropped(mNativePtr) : 0;
    }

    // Native declarations
    private final native int nativeSetCaptureCallback(long id_camera, ICaptureFrameCallback callback);
    private final native int nativeSetCaptureFormat(long id_camera, int format);
    private final native int nativeSetCaptureFrameRate(long id_camera, int targetFps);
    private final native int nativeEnableCapture(long id_camera, boolean enable);
    private final native long nativeGetCaptureFramesEmitted(long id_camera);
    private final native long nativeGetCaptureFramesDropped(long id_camera);
```

---

# Part 2: Consumer App Integration Guide (Out of Scope for Library)

> **Note**: This section documents how consuming applications (e.g., ScopeCam) should integrate
> with the new capture API. These files do NOT exist in this library repository.

## Objective

Wire the new native capture callback to Kotlin `frameStream`, add capture settings UI, and complete the ViewModel → Service recording flow.

## Implementation Steps (For Consuming Apps)

### Step 1: Add Capture Format Enum (Kotlin - App Module)

**File**: `<your-app>/src/main/kotlin/.../CaptureFormat.kt`

Ensure values match native/Java enums:
- `CAPTURE_FORMAT_RGBX = 0`
- `CAPTURE_FORMAT_NV21 = 1`
- `CAPTURE_FORMAT_YUYV = 2`
- `CAPTURE_FORMAT_I420 = 3`

### Step 2: Implement ICaptureFrameCallback (Kotlin - App Module)

**File**: `<your-app>/src/main/kotlin/.../UvcCameraManager.kt`

Implement the **Java** interface from the library:

```kotlin
import com.serenegiant.usb.ICaptureFrameCallback  // Import from library
import java.nio.ByteBuffer

// ... inside class ...

private val captureCallback = object : ICaptureFrameCallback {
    override fun onCaptureFrame(
        buffer: ByteBuffer,
        width: Int,
        height: Int,
        format: Int,
        timestampNs: Long
    ) {
        // CRITICAL: Copy buffer contents immediately - it's only valid during this call
        val copy = ByteArray(buffer.remaining())
        buffer.get(copy)
        
        // Emit to your frame stream / recording pipeline
        frameStream.emit(CaptureFrame(copy, width, height, format, timestampNs))
    }
}

// Wire up the callback
uvcCamera.setCaptureCallback(captureCallback)
uvcCamera.setCaptureFormat(CaptureFormat.NV21.ordinal)
uvcCamera.setCaptureFrameRate(30)
uvcCamera.enableCapture(true)
```

### Step 3: Service, ViewModel, Telemetry

Integrate with your app's architecture as needed.

---

## Testing Checklist

1.  **Build Library**: Verify `UVCCamera.java` compiles with new native methods and interface.
2.  **Build Native**: Verify `serenegiant_usb_UVCCamera.cpp` and `UVCPreview.cpp` compile.
3.  **App Integration**: Verify your app correctly implements the Java interface.
4.  **Functional Test**:
    *   Enable recording.
    *   Verify `onCaptureFrame` is hit.
    *   Verify frames are emitted to your stream.
    *   Verify performance (no display stutter).
