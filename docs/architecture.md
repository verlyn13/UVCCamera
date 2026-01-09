# UVCCamera Architecture Reference

**Version:** 1.2
**Status:** Implemented
**Last Updated:** 2026-01-09

This document describes the production architecture of the UVCCamera library's frame processing pipeline.

---

## Overview

UVCCamera implements a **Broadcaster Pattern** for USB camera streaming on Android:

1. **Display Path**: Zero-copy stream via AHardwareBuffer ring buffer → GPU rendering
2. **Capture Path**: CPU-accessible frames via callback for recording/analysis

Both paths are fed from a single conversion thread using the **emit-first-then-route** pattern. The capture callback is always emitted (if enabled) before deciding the buffer's fate, ensuring capture continues even when the display surface is unavailable (WARM state).

---

## Pipeline Topology

```
USB Isochronous Transfer
        │
        ▼
┌───────────────────────────────────────────────────────────────────┐
│                  uvc_preview_frame_callback()                      │
│                                                                    │
│    Raw MJPEG/YUYV → SPSC Queue → signalConversionThread()         │
└───────────────────────────────────────────────────────────────────┘
        │
        ▼
┌───────────────────────────────────────────────────────────────────┐
│              do_conversion_loop() [Broadcaster Pattern]            │
│                                                                    │
│  1. Dequeue raw frame from SPSC                                   │
│  2. Lock AHardwareBuffer, convert to RGBX                         │
│  3. EMIT FIRST: Capture callback (while buffer locked) ────────┐  │
│  4. THEN ROUTE based on consumer availability:                  │  │
│     • HAS CONSUMER: unlockWriteBuffer() → Ring/Surface         │  │
│     • NO CONSUMER:  cancelWriteBuffer() → Recycle (drain)      │  │
│  5. Update telemetry                                            │  │
└─────────────────────────────────────────────────────────────────┼──┘
                                                                  │
                    ┌─────────────────────────────────────────────┘
                    │
        ┌───────────┴───────────┐
        │                       │
        ▼                       ▼
┌─────────────────────┐   ┌─────────────────────┐
│   CAPTURE PATH      │   │   DISPLAY PATH      │
│   (Always active)   │   │   (hasConsumer=1)   │
│                     │   │                     │
│ JNI Callback        │   │ HardwareBuffer      │
│ → ICaptureFrameCallback │ → GPU Renderer      │
│ → Recording/AI      │   │ → Ring/Surface      │
└─────────────────────┘   └─────────────────────┘
```

---

## Stage Details

### Stage 1: USB Callback (Native, RT-safe)

**Location:** `UVCPreview.cpp:uvc_preview_frame_callback()`

- Copies raw frame into reusable pending-slot buffer
- Enqueues to lock-free SPSC queue
- Signals conversion thread via eventfd
- **Hard rules:** No locks, no JNI, no allocations, drop if queue full

**Telemetry:** `framesReceived`, `framesDroppedQueueFull`

### Stage 2: SPSC Queue

**Location:** `FrameBufferRing.h:enqueuePendingFrame()/dequeuePendingFrame()`

- Lock-free producer/consumer with atomic indices
- Queue depth: 4 slots (configurable via `PENDING_QUEUE_SIZE`)
- Fail-fast: returns false on full queue

### Stage 3: Conversion Thread (Broadcaster Pattern)

**Location:** `UVCPreview.cpp:do_conversion_loop()`

- Waits on eventfd for new frames
- Locks AHardwareBuffer, converts MJPEG→RGBX or YUYV→RGBX
- **Emit First**: Sends to capture callback while buffer is still locked
- **Then Route**: Checks consumer availability (`hasConsumer = surfaceReady || ringConsumerActive`):
  - HAS CONSUMER: `unlockWriteBuffer()` - commits frame to ring for consumption
  - NO CONSUMER: `cancelWriteBuffer()` - recycles buffer immediately (active drain)

**Consumer detection:**
- `surfaceReady`: ANativeWindow display path is active
- `ringConsumerActive`: Ring buffer mode enabled AND ring buffer injected (Kotlin GL consumer)

**Critical invariant:** Capture emission MUST occur BEFORE `cancelWriteBuffer()` as the memory contract is voided after cancel.

### Stage 4: AHardwareBuffer Ring (Display Path)

**Location:** `FrameBufferRing.cpp`

- Triple-buffered ring with MAILBOX policy
- Bidirectional GPU fences for synchronization
- Producer waits on GPU release fence before writing
- Consumer receives acquire fence with buffer

### Stage 5: Capture Callback (Recording Path)

**Location:** `UVCPreview.cpp:emitCaptureFrame()`

- Non-blocking: uses `compare_exchange_strong` on busy flag
- If callback is busy, frame is dropped (display never blocked)
- Configurable format conversion (RGBX, NV21, YUYV, I420)
- Time-based frame rate decimation

---

## Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| **minSdk** | 26 | Enables AHardwareBuffer, native fences |
| **Fence Timeout** | Drop frame | Scientific-grade integrity over continuity |
| **GL Context** | Single Owner | Centralized fence policy, no desync |
| **Capture Priority** | Non-blocking drop | Display path never stalls |
| **Color Space** | BT.601 limited | Standard USB camera assumption |

### Color Conversion

USB cameras provide no colorspace metadata. The library assumes:
- Color Primaries: BT.601 (SMPTE 170M)
- Transfer Function: ~gamma 2.2
- Range: Limited (Y: 16-235, UV: 16-240)

```
R = Y + 1.402 × (V - 128)
G = Y - 0.344 × (U - 128) - 0.714 × (V - 128)
B = Y + 1.772 × (U - 128)
```

---

## Threading Model

| Thread | Responsibilities |
|--------|------------------|
| **USB Callback** | Copy raw frame to SPSC queue (microseconds) |
| **Conversion Thread** | Dequeue, decode, convert, write to ring, emit capture |
| **GL/Render Thread** | Acquire buffer, import fence, render, create release fence |
| **App Thread** | Camera lifecycle, configuration |

### Thread Safety

- **JNI handles**: Protected by HandleManager (slot-based reference counting)
- SPSC queue: Lock-free with atomic indices
- Ring buffer slots: Protected by slot state atomics
- Capture callback: Guarded by atomic busy flag
- All telemetry counters: Atomic operations
- **Surface swap**: Protected by handshake protocol (std::mutex + condition_variable)

---

## Preview State Machine

The preview pipeline uses a three-state lifecycle to handle Android surface availability:

```
           ┌─────────────────────────────────────────────────────┐
           │                     COLD                            │
           │   No USB streaming, preview thread not running      │
           └───────────────────┬─────────────────────────────────┘
                               │ startPreview()
                               ▼
    ┌──────────────────────────────────────────────────────────────┐
    │                     WARM (Broadcaster Mode)                   │
    │   USB streaming active, no surface attached                   │
    │   - Frames converted and routed to capture callback          │
    │   - Ring buffer slots recycled via cancelWriteBuffer()       │
    │   - Recording continues even without preview surface         │
    └───────────────┬─────────────────────────┬────────────────────┘
                    │ attachSurface()          │ stopPreview()
                    ▼                          ▼
    ┌───────────────────────────────┐    ┌──────────────────┐
    │            HOT                │    │      COLD        │
    │   USB + rendering active      │    │                  │
    │   - Full frame processing     │    │                  │
    │   - Ring buffer commits       │    │                  │
    │   - Capture callbacks         │    │                  │
    └───────────────┬───────────────┘    └──────────────────┘
                    │ detachSurface()
                    │ (Gallery navigation)
                    ▼
               Back to WARM
```

### Surface Swap Handshake

When surface changes occur (rotation, Gallery navigation), a handshake ensures safe transitions:

1. **Requester**: Sets `mSwappingSurface = true`
2. **Render thread**: Detects flag, parks itself, signals `mIsRenderIdle = true`
3. **Requester**: Waits for idle, performs ANativeWindow operations safely
4. **Requester**: Sets `mSwappingSurface = false`, notifies render thread
5. **Render thread**: Resumes in new state (WARM or HOT)

This prevents `ANativeWindow_release()` hangs and BufferQueue abandonment.

### Surface Lease API (Java Layer)

The Java layer exposes the state machine through a "Lease" API pattern for Kotlin integration:

```java
// Query diagnostic state (bitmask)
int diag = camera.querySessionDiagnostic();
boolean running = (diag & UVCCamera.DIAG_RUNNING) != 0;
boolean stagnant = (diag & UVCCamera.DIAG_STAGNATION) != 0;

// State transitions
camera.suspendSurfaceLease();     // HOT → WARM (before surface destruction)
camera.acquireSurfaceLease(surf); // WARM → HOT (when surface available)
```

**Diagnostic Bitmask Constants:**

| Constant | Value | Description |
|----------|-------|-------------|
| `DIAG_RUNNING` | 0x01 | Preview thread active |
| `DIAG_SURFACE_BOUND` | 0x02 | ANativeWindow attached |
| `DIAG_STATE_WARM` | 0x10 | Active Drain mode |
| `DIAG_STATE_HOT` | 0x20 | Rendering mode |
| `DIAG_STATE_COLD` | 0x40 | Stopped |
| `DIAG_STAGNATION` | 0x80 | No frames processed in >500ms |

**"Observe then Act" Pattern:**

Kotlin callers should always verify state transitions via `querySessionDiagnostic()` rather than blindly assuming success:

```kotlin
camera.suspendSurfaceLease()
val diag = camera.querySessionDiagnostic()
if ((diag and UVCCamera.DIAG_STATE_WARM) != 0) {
    // Transition succeeded
}
```

---

## Telemetry

### Ring Buffer Metrics (via packed buffer)

| Field | Description |
|-------|-------------|
| `framesProduced` | Total frames from USB |
| `framesDroppedQueueFull` | SPSC overflow |
| `framesDroppedMailbox` | Overwritten before read |
| `framesRendered` | Acquired by consumer |
| `producerStallCount` | Producer blocked by consumer |
| `consumerStarveCount` | Consumer found no frame |

### Capture Callback Metrics

| Field | Description |
|-------|-------------|
| `captureFramesEmitted` | Frames sent to callback |
| `captureFramesDropped` | Frames dropped (callback busy) |
| `captureCallbackBusy` | Busy contention events |

### Preview State Machine Metrics (Phase 4)

| Field | Description |
|-------|-------------|
| `previewState` | Current state (0=COLD, 1=WARM, 2=HOT) |
| `warmToHotTransitions` | Count of WARM→HOT transitions |
| `hotToWarmTransitions` | Count of HOT→WARM transitions |
| `lastStateTransitionTimeNs` | Timestamp of last state change |
| `totalWarmTimeNs` | Cumulative time spent in WARM state |

---

## Zero-Copy Snapshot (Phase 4)

### captureToFd API

**Location:** `FrameBufferRing.cpp:captureToFd()`

The `captureToFd()` method provides FD-based JPEG snapshot capture for Android scoped storage compatibility:

```cpp
/**
 * Capture current frame to file descriptor as JPEG.
 * @param fd Open file descriptor (caller manages lifecycle)
 * @param quality JPEG quality (1-100, default 90)
 * @return 0 on success, negative error code on failure
 */
int FrameBufferRing::captureToFd(int fd, int quality);
```

### Error Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| -1 | No frame available |
| -2 | Failed to lock buffer for CPU read |
| -3 | Unsupported buffer format |
| -4 | JPEG compression failed |
| -5 | Write to FD failed |

### Format Support

| Format | TurboJPEG Pixel Format | Status |
|--------|------------------------|--------|
| `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM` | `TJPF_RGBX` | ✅ Primary |
| `AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM` | `TJPF_XRGB` | ✅ Supported |
| `AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM` | `TJPF_RGB` | ✅ Supported |
| Other | - | ❌ Returns -3 |

### JNI Integration

```kotlin
// Kotlin usage example
val fd = contentResolver.openFileDescriptor(uri, "w")?.detachFd() ?: return
val result = nativeFrameBufferCaptureToFd(ringHandle, fd, quality = 90)
close(fd)
```

---

## JNI Handle Safety

### The Problem

Android lifecycle events can trigger camera cleanup while JNI calls are in-flight:

```
Thread A: nativeSetContrast(handle) → validates handle → uses camera
Thread B: onDestroy() → destroyCamera() → deletes camera object
Thread A: Uses dangling pointer → CRASH (SIGSEGV)
```

### The Solution: HandleManager

The `HandleManager` implements **slot-based reference counting** to prevent use-after-free crashes:

```cpp
// Before (UNSAFE):
UVCCamera* camera = reinterpret_cast<UVCCamera*>(id_camera);
camera->setContrast(value);  // CRASH if destroyed!

// After (SAFE):
auto ref = getCameraHandleManager().acquire(id_camera);
if (!ref) return JNI_ERR_INVALID_HANDLE;
UVCCamera* camera = static_cast<UVCCamera*>(ref.ptr);
camera->setContrast(value);  // Protected by ScopedRef
// ~ScopedRef() automatically releases
```

### Key Mechanisms

| Mechanism | Purpose |
|-----------|---------|
| **Generation Counter** | Odd = alive, even = dead. Detects stale handles. |
| **Active References** | Count of in-flight JNI calls using this handle. |
| **ScopedRef RAII** | Increments refs before validation, auto-decrements on scope exit. |
| **invalidateAndFree()** | Marks dead, spins until refs drain, then returns pointer for deletion. |
| **MTE-Safe Pointers** | Uses `std::atomic<uintptr_t>` to preserve ARM MTE tags (0xb400... addresses). |

### Handle Encoding

Handles are 64-bit integers encoding:
- Lower 32 bits: Slot index (0-63)
- Upper 32 bits: Generation counter

```cpp
int64_t handle = (generation << 32) | slotIndex;
```

### HandleSlot Structure

```cpp
// Cache-line aligned to prevent false sharing
struct alignas(CACHE_LINE_SIZE) HandleSlot {
    std::atomic<uint32_t> generation{0};  // Odd = alive, even = dead
    std::atomic<int> activeRefs{0};       // In-flight JNI calls
    std::atomic<ContextPtr> context{0};   // MTE-safe pointer storage
};
```

### Thread Safety Guarantees

1. **No use-after-free**: `invalidateAndFree()` blocks until all active refs drain
2. **Stale handle detection**: Generation mismatch returns error, not crash
3. **Lock-free fast path**: Atomic operations with acquire/release semantics
4. **Timeout protection**: 10-second max spin with logged warning
5. **ARM MTE compatibility**: Pointer tags preserved via `uintptr_t` storage

---

## File Locations

| Component | Path |
|-----------|------|
| **Handle Manager** | `lib/src/main/jni/UVCCamera/HandleManager.h/cpp` |
| Ring Buffer | `lib/src/main/jni/UVCCamera/FrameBufferRing.cpp/h` |
| Telemetry | `lib/src/main/jni/UVCCamera/StreamTelemetry.h` |
| Preview/Conversion | `lib/src/main/jni/UVCCamera/UVCPreview.cpp/h` |
| JNI Bridge | `lib/src/main/jni/UVCCamera/serenegiant_usb_UVCCamera.cpp` |
| Java API | `lib/src/main/java/com/serenegiant/usb/UVCCamera.java` |
| Capture Interface | `lib/src/main/java/com/serenegiant/usb/ICaptureFrameCallback.java` |

---

## Related Documentation

- [API Reference](./api-reference.md) - Public API documentation
- [Native-Kotlin Alignment](./Native-Kotlin-Alignment-Checklist.md) - JNI contracts
- [Producer-Consumer Trace](./Producer-Consumer-Handshake-Trace.md) - Detailed handshake
- [Fence Implementation](./Phase4-Bidirectional-Fence-Implementation.md) - GPU sync details
