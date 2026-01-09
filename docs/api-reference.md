# UVCCamera API Reference

**Version:** 1.1
**Status:** Implemented
**Last Updated:** 2026-01-08
**Package:** `com.serenegiant.usb`

This document provides the public API reference for the UVCCamera library.

---

## Core Classes

### UVCCamera

Main camera control class. Manages USB camera lifecycle, preview, and capture.

**Location:** `lib/src/main/java/com/serenegiant/usb/UVCCamera.java`

### USBMonitor

USB device enumeration and permission management.

**Location:** `lib/src/main/java/com/serenegiant/usb/USBMonitor.java`

---

## Camera Lifecycle

### Opening a Camera

```java
// Using file descriptor (recommended)
camera.openSimple(int fd, String usbfsPath);

// Using UsbControlBlock (legacy)
camera.open(UsbControlBlock ctrlBlock);
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `fd` | `int` | USB file descriptor from `UsbDeviceConnection.getFileDescriptor()` |
| `usbfsPath` | `String` | USB filesystem path (e.g., `/dev/bus/usb/001/002`) |

### Configuring Preview

```java
camera.setPreviewSize(int width, int height, int min_fps, int max_fps, int mode, float bandwidth);
```

| Parameter | Description |
|-----------|-------------|
| `width` | Frame width in pixels |
| `height` | Frame height in pixels |
| `min_fps` | Minimum frame rate |
| `max_fps` | Maximum frame rate |
| `mode` | Frame format (1=MJPEG, 2=YUYV) |
| `bandwidth` | Bandwidth factor (1.0 = 100%) |

### Starting/Stopping Preview

```java
camera.startPreview();
camera.stopPreview();
```

### Closing Camera

```java
camera.close();
```

---

## Surface Lease API (State Machine)

The Surface Lease API enables USB streaming to continue while the Android surface is unavailable (e.g., during Gallery navigation). This provides instant preview resume without USB reconnection.

### State Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `PREVIEW_STATE_COLD` | 0 | No USB streaming, preview thread not running |
| `PREVIEW_STATE_WARM` | 1 | USB streaming active, no surface (frames drained) |
| `PREVIEW_STATE_HOT` | 2 | USB streaming active + surface rendering |

### Diagnostic Bitmask Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `DIAG_RUNNING` | 0x01 | Preview thread active |
| `DIAG_SURFACE_BOUND` | 0x02 | ANativeWindow attached |
| `DIAG_STATE_WARM` | 0x10 | Active Drain mode |
| `DIAG_STATE_HOT` | 0x20 | Rendering mode |
| `DIAG_STATE_COLD` | 0x40 | Stopped |
| `DIAG_STAGNATION` | 0x80 | No frames processed in >500ms |

### Methods

```java
// Query diagnostic state (bitmask)
int querySessionDiagnostic();

// Query simple state
int getPreviewState();

// Transition HOT → WARM (call BEFORE surface destruction)
void suspendSurfaceLease();

// Transition WARM → HOT (when surface becomes available)
void acquireSurfaceLease(Surface surface);
```

### Usage Example

```kotlin
// Before navigating to Gallery (surface about to be destroyed)
camera.suspendSurfaceLease()

// Verify transition succeeded
val diag = camera.querySessionDiagnostic()
check((diag and UVCCamera.DIAG_STATE_WARM) != 0) { "Failed to enter WARM state" }
check((diag and UVCCamera.DIAG_RUNNING) != 0) { "USB streaming stopped unexpectedly" }

// ... Gallery navigation occurs ...

// When returning with new surface
camera.acquireSurfaceLease(newSurface)

// Verify HOT state
val diagAfter = camera.querySessionDiagnostic()
if ((diagAfter and UVCCamera.DIAG_STATE_HOT) == 0) {
    // Handle failure - may need to restart preview
}
```

### Stagnation Detection

Use the `DIAG_STAGNATION` flag to detect when the camera has stopped producing frames:

```kotlin
val diag = camera.querySessionDiagnostic()
if ((diag and UVCCamera.DIAG_STAGNATION) != 0) {
    // No frames processed in >500ms - camera may be stalled
    // Consider recovery action (restart preview, show error)
}
```

---

## Ring Buffer (Display Path)

The ring buffer provides zero-copy frame delivery for GPU rendering.

### Allocation

```java
long handle = camera.allocateRingBuffer(int width, int height);
camera.setFrameBufferRing(long handle);
camera.setUseRingBuffer(boolean use);
```

### Acquisition (Native/JNI)

```kotlin
// Kotlin consumer side
val buffer = nativeFrameBufferAcquireBuffer(handle)
val frameNumber = nativeFrameBufferGetFrameNumber(handle)
val acquireFence = nativeFrameBufferGetAcquireFence(handle)

// After rendering
nativeFrameBufferReleaseWithFence(handle, frameNumber, gpuReleaseFenceFd)
```

### Destruction

```java
camera.destroyRingBuffer();
```

---

## Capture Callback (Recording Path)

The capture callback provides CPU-accessible frames for recording and analysis.

### Interface

```java
public interface ICaptureFrameCallback {
    /**
     * Called on the native conversion thread.
     * 
     * CRITICAL: The buffer is ONLY valid for the duration of this call.
     * You MUST copy data immediately. Do not store the ByteBuffer reference.
     *
     * @param buffer      Direct ByteBuffer containing pixel data
     * @param width       Frame width in pixels
     * @param height      Frame height in pixels
     * @param format      Pixel format (see format constants)
     * @param timestampNs Monotonic timestamp in nanoseconds
     */
    void onCaptureFrame(ByteBuffer buffer, int width, int height, 
                        int format, long timestampNs);
}
```

### Format Constants

| Constant | Value | Description | Buffer Size |
|----------|-------|-------------|-------------|
| `CAPTURE_FORMAT_RGBX` | 0 | 32-bit RGBA | width × height × 4 |
| `CAPTURE_FORMAT_NV21` | 1 | YUV 4:2:0 semi-planar | width × height × 3/2 |
| `CAPTURE_FORMAT_YUYV` | 2 | YUV 4:2:2 packed | width × height × 2 |
| `CAPTURE_FORMAT_I420` | 3 | YUV 4:2:0 planar | width × height × 3/2 |

### Configuration Methods

```java
// Register callback
int result = camera.setCaptureCallback(ICaptureFrameCallback callback);

// Set output format (default: RGBX)
int result = camera.setCaptureFormat(int format);

// Set target frame rate for decimation (1-120)
int result = camera.setCaptureFrameRate(int targetFps);

// Enable/disable capture emission
int result = camera.enableCaptureCallback(boolean enable);
```

### Telemetry Methods

```java
long emitted = camera.getCaptureFramesEmitted();  // Total frames sent to callback
long dropped = camera.getCaptureFramesDropped();  // Frames dropped (any reason)
long busyCount = camera.getCaptureCallbackBusy(); // Frames dropped due to slow consumer (counter, not boolean)
```

### Usage Example

```kotlin
// Kotlin implementation
val callback = object : ICaptureFrameCallback {
    override fun onCaptureFrame(
        buffer: ByteBuffer,
        width: Int,
        height: Int,
        format: Int,
        timestampNs: Long
    ) {
        // MUST copy immediately - buffer invalid after return
        val data = ByteArray(buffer.remaining())
        buffer.get(data)
        
        // Send to encoder/processor
        videoEncoder.queueFrame(data, width, height, timestampNs)
    }
}

// Setup
camera.setCaptureCallback(callback)
camera.setCaptureFormat(ICaptureFrameCallback.CAPTURE_FORMAT_NV21)
camera.setCaptureFrameRate(30)
camera.enableCaptureCallback(true)

// Teardown
camera.enableCaptureCallback(false)
```

---

## Telemetry (Ring Buffer)

### Packed Buffer API

Access comprehensive telemetry via a 296-byte DirectByteBuffer.

```kotlin
val buffer = nativeGetTelemetryBuffer(ringHandle)
buffer.order(ByteOrder.LITTLE_ENDIAN)

val version = buffer.getLong(0)           // Field 0
val framesProduced = buffer.getLong(8)    // Field 1
val framesDroppedMailbox = buffer.getLong(16)  // Field 2
// ... 37 total fields, each 8 bytes
```

See [native-telemetry-guide.md](../scopecam/native-telemetry-guide.md) for complete field layout.

### Individual Getters

```kotlin
external fun nativeFrameBufferGetFramesReceived(handle: Long): Long
external fun nativeFrameBufferGetFramesRendered(handle: Long): Long
external fun nativeFrameBufferGetFramesDropped(handle: Long): Long
external fun nativeFrameBufferGetProducerStalls(handle: Long): Long
external fun nativeFrameBufferGetConsumerStarves(handle: Long): Long
```

---

## Camera Controls

### White Balance

```java
camera.setAutoWhiteBlance(boolean autoWhiteBlance);
int wb = camera.getWhiteBlance();
camera.setWhiteBlance(int whiteBlance);
```

### Focus

```java
camera.setAutoFocus(boolean autoFocus);
int focus = camera.getFocus();
camera.setFocus(int focus);
```

### Exposure

```java
camera.setAutoExposure(int mode);  // 1=manual, 2=auto, 4=shutter, 8=aperture
int exposure = camera.getExposure();
camera.setExposure(int exposure);
```

### Gain

```java
int gain = camera.getGain();
camera.setGain(int gain);
```

### Brightness, Contrast, etc.

```java
camera.setBrightness(int brightness);
camera.setContrast(int contrast);
camera.setSaturation(int saturation);
camera.setSharpness(int sharpness);
camera.setGamma(int gamma);
camera.setHue(int hue);
```

---

## Error Codes

### libuvc Errors

| Code | Name | Description |
|------|------|-------------|
| 0 | `UVC_SUCCESS` | Success |
| -1 | `UVC_ERROR_IO` | I/O error |
| -2 | `UVC_ERROR_INVALID_PARAM` | Invalid parameter |
| -3 | `UVC_ERROR_ACCESS` | Access denied |
| -4 | `UVC_ERROR_NO_DEVICE` | Device not found |
| -5 | `UVC_ERROR_NOT_FOUND` | Entity not found |
| -6 | `UVC_ERROR_BUSY` | Device busy |
| -7 | `UVC_ERROR_TIMEOUT` | Timeout |
| -9 | `UVC_ERROR_NO_MEM` | Out of memory |

---

## Threading Model

| Thread | Safe Operations |
|--------|-----------------|
| Main/UI | Configuration, lifecycle methods |
| USB Callback | Internal only (enqueue frames) |
| Conversion | Internal only (decode, emit) |
| GL/Render | `acquireBuffer`, `releaseBuffer`, telemetry |

### Thread Safety Notes

- All public methods are synchronized on `UVCCamera` instance
- Native telemetry getters use atomic reads (safe from any thread)
- `ICaptureFrameCallback.onCaptureFrame()` is called on conversion thread
- DirectByteBuffer in callback is valid only during callback scope

---

## Related Documentation

- [Architecture](./architecture.md) - System architecture and pipeline
- [ScopeCam Integration](./ScopeCam-Integration-Guide.md) - App integration guide
- [Native-Kotlin Alignment](./Native-Kotlin-Alignment-Checklist.md) - JNI contracts
