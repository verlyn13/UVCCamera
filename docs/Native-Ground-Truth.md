# Native Library Ground Truth: UVCCamera Integration Guide

**Last Updated:** January 7, 2026  
**Library Version:** 0.1.0 (Dual-Emit Architecture)  
**Package:** `com.serenegiant.usb`

> **Note:** This document is retained for ScopeCam-specific integration context.
> For canonical API documentation, see [api-reference.md](./api-reference.md).
> For architecture details, see [architecture.md](./architecture.md).

This document provides integration guidance for the `UVCCamera` native library.

---

## 1. Core Architecture

The library implements a **Hybrid Dual-Emit Architecture**:
1.  **Display Path (Ring Buffer):** Zero-copy stream to `AHardwareBuffer` ring for high-performance rendering.
2.  **Capture Path (Callback):** CPU-accessible stream via `ICaptureFrameCallback` for recording/analysis.

**Key Constraints:**
- **Language:** The library interface is **Java** (`UVCCamera.java`).
- **Thread Safety:** Native methods are thread-safe.
- **Priority:** Display path has priority. Capture frames are dropped (non-blocking) if the consumer is slow.

---

## 2. Integration Interfaces

### 2.1. The Camera Object
**Class:** `com.serenegiant.usb.UVCCamera`

This is the primary entry point. It manages the native pointer `mNativePtr`.

### 2.2. The Capture Callback
**Interface:** `com.serenegiant.usb.ICaptureFrameCallback`

Must be implemented by the app to receive recording frames.

```java
public interface ICaptureFrameCallback {
    /**
     * Called on the native conversion thread.
     * CRITICAL: The buffer is ONLY valid for the duration of this call.
     * You MUST copy data immediately. Do not store the ByteBuffer.
     *
     * @param buffer Direct ByteBuffer containing raw pixel data
     * @param width  Frame width in pixels
     * @param height Frame height in pixels
     * @param format Native format (0=RGBX, 1=NV21, 2=YUYV, 3=I420)
     * @param timestampNs Monotonic timestamp in nanoseconds
     */
    void onCaptureFrame(ByteBuffer buffer, int width, int height, int format, long timestampNs);
}
```

### 2.3. Capture Formats
Matches native `CapturePixelFormat` enum.

| Value | Name | Description |
| :--- | :--- | :--- |
| `0` | `RGBX` | 32-bit RGBA (Raw conversion output). Fastest. |
| `1` | `NV21` | YUV 4:2:0 Semi-planar (Android standard). **Recommended for MediaCodec**. |
| `2` | `YUYV` | YUV 4:2:2 Packed (Raw sensor output for most cams). |
| `3` | `I420` | YUV 4:2:0 Planar (Universal compatibility). |

---

## 3. Usage Sequences

### 3.1. Initialization (Kotlin-First Ownership)

The app controls the USB lifecycle and Ring Buffer allocation.

```kotlin
// 1. Open Camera (using FD)
camera.openSimple(fd, usbfsPath)

// 2. Configure Preview
camera.setPreviewSize(1920, 1080, 1, 30, 1, 1.0f)

// 3. Allocate Ring Buffer (App owns the handle)
// MUST match preview size
val ringHandle = camera.allocateRingBuffer(1920, 1080)

// 4. Inject Ring Buffer (Critical for handle alignment)
camera.setFrameBufferRing(ringHandle)

// 5. Enable Ring Buffer Mode
camera.setUseRingBuffer(true)

// 6. Start Stream
camera.startPreview()

// 7. Verify Actual FPS (Negotiated)
val actualFps = camera.getPreviewFps()
if (actualFps > 0) {
    Log.i("Cam", "Negotiated FPS: $actualFps")
}
```

### 3.2. Enabling Recording (Dual-Emit)

Can be done dynamically while preview is running.

```kotlin
// 1. Set Callback
camera.setCaptureCallback(myCallback)

// 2. Configure Format (e.g., NV21 for VideoEncoder)
camera.setCaptureFormat(1)

// 3. Set Target FPS (Decimation)
// e.g., Preview at 60fps, Record at 30fps
camera.setCaptureFrameRate(30)

// 4. Enable Emission
camera.enableCaptureCallback(true)
```

### 3.3. Stopping Recording

```kotlin
// Disable emission first to stop callbacks
camera.enableCaptureCallback(false)
```

### 3.4. Teardown

```kotlin
camera.stopPreview()
camera.destroyRingBuffer() // Releases native memory
camera.close() // Releases USB
```

---

## 4. API Reference (Native Methods)

These methods are exposed in `UVCCamera.java`.

### Lifecycle & Configuration
- `openSimple(int fd, String usbfs)`: Connect using existing file descriptor.
- `setPreviewSize(int w, int h, ...)`: Negotiate stream parameters.
- `getPreviewFps()`: Returns the actual negotiated frame rate (int).

### Ring Buffer (Display)
- `allocateRingBuffer(int w, int h)`: Allocates native `FrameBufferRing`. Returns `long` handle.
- `setFrameBufferRing(long handle)`: Injects handle into native previewer.
- `setUseRingBuffer(boolean use)`: Switches internal pipeline to ring buffer mode.
- `getRingBufferHandle()`: Retrieves the current handle.

### Capture (Recording)
- `setCaptureCallback(ICaptureFrameCallback cb)`: Registers listener.
- `setCaptureFormat(int format)`: Sets pixel format conversion (0-3).
- `setCaptureFrameRate(int fps)`: Sets target FPS for decimation.
- `enableCaptureCallback(boolean enable)`: Starts/Stops the callback stream.

### Capture Telemetry (Diagnostics)
- `getCaptureFramesEmitted()`: Total frames sent to callback.
- `getCaptureFramesDropped()`: Frames dropped due to callback contention (busy).
- `getCaptureCallbackBusy()`: Counter of busy events.

> **Note:** Ring buffer telemetry (`getDroppedNoSurface()`, `getDroppedQueueFull()`, etc.) is accessed via the ring buffer handle using the packed telemetry buffer API. See `native-telemetry-guide.md` for details.

---

## 5. Threading & Performance Model

### 5.1. Threading Diagram
```
[USB Thread] --(Raw Frame)--> [SPSC Queue]
                                    |
                                    v
                           [Conversion Thread]
                                    |
                  +-----------------+-----------------+
                  | (Priority 1)                      | (Priority 2)
                  v                                   v
          [Ring Buffer Write]                 [Capture Callback]
                  |                                   |
          (Display Latency: ~0ms)           (Copy/Encode Latency)
```

### 5.2. Safety Mechanisms
1.  **Non-Blocking Drop:** If `onCaptureFrame` takes longer than the next frame arrival, the new frame is dropped. The display path is **never blocked**.
2.  **Zero-Copy Display:** Writing to Ring Buffer involves color conversion directly into the `AHardwareBuffer` memory.
3.  **DirectBuffer:** The ByteBuffer passed to Kotlin is a direct reference to native memory. **Validity is strictly limited to the callback scope.**

---

## 6. Known Limitations

1.  **Scalar Conversion:** Color conversion (e.g., RGBX -> NV21) currently uses scalar C++ logic. High resolutions (4K) may tax the CPU.
    *   *Mitigation:* Use `setCaptureFrameRate` to decimate (e.g., 30fps -> 15fps) if CPU usage is high.
2.  **Memory Management:** The `ICaptureFrameCallback` requires the JVM to copy data out immediately (`buffer.get(byteArray)`). Failure to do so will result in data corruption.
