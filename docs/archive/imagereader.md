````md
# android.media.ImageReader
**API Level:** 19+
**Languages:** Kotlin, Java
**Implements:** `AutoCloseable`

---

## Overview

`ImageReader` provides **direct application access to image buffers rendered into a `Surface`**.
It is a core low-level primitive for camera, codec, and GPU pipelines where precise control over
buffer flow, timing, and memory ownership is required.

**Common producers that can render into an ImageReader Surface:**
- `CameraDevice`
- `MediaCodec`
- `MediaPlayer`
- `ImageWriter`
- `RenderScript Allocations`

Images are delivered as `Image` objects and are **explicitly lifecycle-managed** by the consumer.
Failure to acquire and release images at the producer rate will result in backpressure, stalls, or drops.

---

## Core Concepts

### Buffer Queue Model
- Incoming images are queued internally.
- Images are accessed via:
  - `acquireLatestImage()` (recommended for real-time)
  - `acquireNextImage()` (FIFO, batch processing)
- At most `maxImages` may be acquired concurrently.
- Each acquired `Image` **must be closed**.

### Memory & Backpressure
- If acquired images are not closed:
  - Producers stall
  - Frames drop
  - `IllegalStateException` is thrown when exhausted
- `discardFreeBuffers()` allows reclaiming cached buffers.

---

## Class Signature

```java
public class ImageReader extends Object implements AutoCloseable
````

---

## Nested Types

### `ImageReader.Builder`

Builder-based construction with:

* default dataspace
* default hardware buffer format

---

### `ImageReader.OnImageAvailableListener`

Callback interface invoked when a new image becomes available.

---

## Construction

### Factory: With Usage Flags (API 29+)

```java
static ImageReader newInstance(
    int width,
    int height,
    int format,
    int maxImages,
    long usage
)
```

#### Supported Format / Usage Combinations

| Format                                     | Compatible Usage Flags                                        |
| ------------------------------------------ | ------------------------------------------------------------- |
| Non-PRIVATE (`ImageFormat`, `PixelFormat`) | `USAGE_CPU_READ_RARELY`, `USAGE_CPU_READ_OFTEN`               |
| `ImageFormat.PRIVATE`                      | `USAGE_VIDEO_ENCODE`, `USAGE_GPU_SAMPLED_IMAGE` (or combined) |

⚠ Unsupported combinations → `IllegalArgumentException`

---

### Factory: Without Usage Flags

```java
static ImageReader newInstance(
    int width,
    int height,
    int format,
    int maxImages
)
```

---

## Image Acquisition

### `acquireLatestImage()`

**API 19+**

```java
Image acquireLatestImage()
```

**Behavior**

* Drops all older frames
* Returns the most recent image
* Best choice for real-time pipelines

**Requirements**

* `maxImages >= 2` for meaningful discarding
* Throws `IllegalStateException` if maxImages exhausted

**Notes**

* API ≤ 36: Throws if acquired image format differs
* API 37+: Format mismatch tolerated

---

### `acquireNextImage()`

**API 19+**

```java
Image acquireNextImage()
```

**Behavior**

* FIFO acquisition
* No automatic discarding
* Intended for offline/batch processing

⚠ Misuse leads to:

* Growing latency
* Frame backlog
* Pipeline stalls

---

## Lifecycle Management

### `close()`

```java
void close()
```

* Releases all native resources
* Invalidates all previously acquired `Image` objects
* Further method calls throw `IllegalStateException`
* Reading prior `ByteBuffer`s → undefined behavior

---

### `discardFreeBuffers()` (API 28+)

```java
void discardFreeBuffers()
```

* Frees cached but unused buffers
* Does NOT:

  * affect acquired images
  * drop pending images
  * impact producer-held buffers

Used for aggressive memory reclamation.

---

## Properties & Introspection

### Dimensions

```java
int getWidth()
int getHeight()
```

* Default values
* Actual per-image size may differ → use `Image.getWidth()` / `getHeight()`

---

### Image Format

```java
int getImageFormat()
```

* Guaranteed compatible with requested format
* Actual image format via `Image.getFormat()`

---

### Dataspace (API 33+)

```java
int getDataSpace()
```

Possible values include:

* `DATASPACE_SRGB`
* `DATASPACE_DISPLAY_P3`
* `DATASPACE_BT2020_PQ`
* `DATASPACE_JPEG_R`
* etc.

---

### Hardware Buffer Format (API 33+)

```java
int getHardwareBufferFormat()
```

Examples:

* `HardwareBuffer.RGBA_8888`
* `HardwareBuffer.YCBCR_420_888`
* `HardwareBuffer.RGBA_FP16`
* Depth, stencil, HDR formats

---

### Usage Flags (API 33+)

```java
long getUsage()
```

May include:

* `USAGE_CPU_READ_OFTEN`
* `USAGE_VIDEO_ENCODE`
* `USAGE_GPU_SAMPLED_IMAGE`
* `USAGE_PROTECTED_CONTENT`
* `USAGE_SENSOR_DIRECT_DATA`

---

### Max Images

```java
int getMaxImages()
```

* Upper bound of concurrently acquired images
* Producer blocks when exhausted

---

## Surface Access

```java
Surface getSurface()
```

**Key Properties**

* Single producer at a time
* Acts as a *weak reference* to ImageReader
* Holding the Surface does NOT keep ImageReader alive

---

## Event Handling

### `setOnImageAvailableListener()`

```java
void setOnImageAvailableListener(
    ImageReader.OnImageAvailableListener listener,
    Handler handler
)
```

* Handler must have a Looper
* `null` handler → current thread looper
* No handler + no looper → `IllegalArgumentException`

---

## PRIVATE Format Semantics

When `ImageFormat.PRIVATE` is used:

* Image data is **not CPU-accessible**
* `Image.getPlanes()` returns empty array
* Ideal for:

  * Camera reprocessing
  * Hardware video encode
  * Zero-copy GPU/MediaCodec paths

More efficient than `YUV_420_888` when CPU access is unnecessary.

---

## Timestamp Semantics (Camera + VIDEO_ENCODE)

If:

* Used as Camera output
* Usage includes `USAGE_VIDEO_ENCODE`

Then:

* Image timestamps align with `SystemClock.uptimeMillis()`
* NOT `elapsedRealtimeNanos()`
* Enables correct A/V sync
* Not directly comparable with other streams

---

## Error Conditions Summary

| Condition                | Result                     |
| ------------------------ | -------------------------- |
| Acquire > maxImages      | `IllegalStateException`    |
| Using released Image     | Undefined behavior         |
| Unsupported format/usage | `IllegalArgumentException` |
| Listener without Looper  | `IllegalArgumentException` |

---

## GC Interaction

### `finalize()` (Deprecated Pattern)

* Invoked once by JVM
* Not deterministic
* Should not be relied upon
* Explicit `close()` REQUIRED

---

## Best Practices (Agent Notes)

* Prefer `acquireLatestImage()` for live pipelines
* Always `Image.close()` in `finally`
* Keep `maxImages` minimal (2–3 typical)
* Use `PRIVATE + VIDEO_ENCODE` for zero-copy video
* Avoid CPU write usage unless strictly necessary
* Monitor backpressure carefully in high-FPS systems

---

## Related APIs

* `Image`
* `ImageWriter`
* `HardwareBuffer`
* `CameraDevice`
* `MediaCodec`

---

```
```

