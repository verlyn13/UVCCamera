# Producer-Consumer Handshake Trace

## UVCCamera Library (org.uvccamera:lib)

This document traces the exact handoff points between the producer (native camera thread)
and consumer (Java/ScopeCam render thread) for the AHardwareBuffer ring buffer system.

---

## 1. Frame Flow Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  PRODUCER THREAD (Native USB/libuvc)                                        │
│  Location: UVCPreview.cpp:do_preview() loop                                │
│                                                                             │
│  1. waitPreviewFrame()                    → Get raw frame from libuvc      │
│  2. write_frame_to_ring_buffer()          → Convert & write to ring buffer │
│     ├── lockWriteBuffer()                 → Wait for GPU fence, lock slot  │
│     ├── convert_func()                    → YUYV → RGBA conversion         │
│     └── unlockWriteBuffer()               → Capture fence, update MAILBOX  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      │ mLatestCompleted (atomic, release)
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  CONSUMER THREAD (Kotlin/Java Render Thread via JNI)                        │
│  Location: ScopeCam HardwareBufferRenderer                                 │
│                                                                             │
│  1. nativeFrameBufferAcquireBuffer()      → Get latest AHardwareBuffer     │
│  2. nativeFrameBufferGetAcquireFence()    → Get producer's fence           │
│  3. Import fence to EGL, bind texture, draw                                 │
│  4. eglSwapBuffers() + create GPU release fence                            │
│  5. nativeFrameBufferReleaseWithFence()   → Return GPU fence to producer   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Producer Side: Exact Line Numbers

### 2.1 Frame Handoff Entry Point

**File:** `lib/src/main/jni/UVCCamera/UVCPreview.cpp`

```cpp
// Lines 540-549 (MJPEG mode) and 559-567 (YUYV mode)
if (mUseRingBuffer) {
    // Ring buffer path: write to AHardwareBuffer
    write_frame_to_ring_buffer(frame, uvc_any2rgbx);  // Line 542 / 561
    addCaptureFrame(frame);
}
```

### 2.2 Write to Ring Buffer

**File:** `lib/src/main/jni/UVCCamera/UVCPreview.cpp`

```cpp
// Lines 987-1027: write_frame_to_ring_buffer()
void UVCPreview::write_frame_to_ring_buffer(uvc_frame_t *frame, convFunc_t convert_func) {
    // Line 993: Lock the write buffer (may wait on GPU fence)
    uint8_t *destPtr = static_cast<uint8_t*>(mFrameBufferRing->lockWriteBuffer(&strideBytes));

    // Lines 1000-1017: Convert frame to RGBA
    uvc_error_t result = convert_func(frame, &dest_frame);

    // Line 1026: Unlock and update MAILBOX pointer
    mFrameBufferRing->unlockWriteBuffer();
}
```

### 2.3 Lock Write Buffer (Fence Wait)

**File:** `lib/src/main/jni/UVCCamera/FrameBufferRing.cpp`

```cpp
// Lines 122-201: lockWriteBuffer()

// Line 127: Get current write slot
int idx = mWriteIndex.load(std::memory_order_relaxed);

// Lines 131-149: Wait for GPU release fence (bidirectional sync)
if (slot.gpuReleaseFenceFd >= 0) {
    // Line 139: Wait up to 33ms for GPU to complete
    int waitResult = poll(&pfd, 1, 33);
    // Lines 140-142: Close fence after wait
    close(slot.gpuReleaseFenceFd);
    slot.gpuReleaseFenceFd = -1;
}

// Lines 163-175 (API 29+) or 178-192 (API 26-28): Lock AHardwareBuffer
res = AHardwareBuffer_lockAndGetInfo(...);
```

### 2.4 Unlock Write Buffer (MAILBOX Update)

**File:** `lib/src/main/jni/UVCCamera/FrameBufferRing.cpp`

```cpp
// Lines 224-265: unlockWriteBuffer()

// Line 233: Unlock buffer and capture release fence
AHardwareBuffer_unlock(mBuffers[idx], &fenceFd);

// Line 246: Store fence for consumer
mMetadata[idx].acquireFenceFd = fenceFd;

// Line 251: CRITICAL - Update MAILBOX pointer with release semantics
mLatestCompleted.store(idx, std::memory_order_release);

// Lines 255-260: Triple-buffer dance - advance to next slot
int nextWrite = (idx + 1) % FRAME_BUFFER_COUNT;
if (nextWrite == currentRead) {
    nextWrite = (nextWrite + 1) % FRAME_BUFFER_COUNT;
}
mWriteIndex.store(nextWrite, std::memory_order_relaxed);
```

---

## 3. Consumer Side: Exact Line Numbers

### 3.1 Acquire Buffer

**File:** `lib/src/main/jni/UVCCamera/FrameBufferRing.cpp`

```cpp
// Lines 267-293: acquireReadBuffer()

// Line 274: CRITICAL - Read MAILBOX with acquire semantics
int latest = mLatestCompleted.load(std::memory_order_acquire);

// Line 281: Mark buffer as being read
mReadIndex.store(latest, std::memory_order_relaxed);
mMetadata[latest].isLockedByConsumer = true;  // Line 282

// Line 290: Increment reference count (keeps buffer alive)
AHardwareBuffer_acquire(mBuffers[latest]);
```

### 3.2 JNI Bridge

**File:** `lib/src/main/jni/UVCCamera/FrameBufferJNI.cpp`

```cpp
// Lines 125-152: nativeFrameBufferAcquireBuffer()
// Returns: android.hardware.HardwareBuffer

// Lines 185-194: nativeFrameBufferGetAcquireFence()
// Returns: Producer's fence fd for GPU sync

// Lines 204-213: nativeFrameBufferGetFrameNumber()
// Returns: Frame number for race-safe release

// Lines 225-250: nativeFrameBufferReleaseWithFence()
// Accepts: GPU release fence from consumer
```

### 3.3 Release Buffer with GPU Fence

**File:** `lib/src/main/jni/UVCCamera/FrameBufferRing.cpp`

```cpp
// Lines 349-364: setGpuReleaseFence()
slot.gpuReleaseFenceFd = fenceFd;
slot.isLockedByConsumer = false;

// Lines 295-305: releaseReadBuffer()
mMetadata[idx].isLockedByConsumer = false;
AHardwareBuffer_release(mBuffers[idx]);
```

---

## 4. Synchronization Points

| Event | Memory Order | Purpose |
|-------|--------------|---------|
| `mLatestCompleted.store()` | `release` | Ensures metadata writes visible before MAILBOX update |
| `mLatestCompleted.load()` | `acquire` | Synchronizes with producer's release |
| `mReadIndex.store()` | `relaxed` | Only producer reads this for triple-buffer dance |
| `mWriteIndex.load()` | `relaxed` | Only producer writes, only producer reads |

---

## 5. Fence Lifecycle

### 5.1 Acquire Fence (Producer → Consumer)

| Step | Location | Action |
|------|----------|--------|
| Create | `FrameBufferRing.cpp:233` | `AHardwareBuffer_unlock(buf, &fenceFd)` |
| Store | `FrameBufferRing.cpp:246` | `mMetadata[idx].acquireFenceFd = fenceFd` |
| Close old | `FrameBufferRing.cpp:237` | Close unconsumed fence before storing new |
| Read | `FrameBufferJNI.cpp:193` | Return to Java via JNI |
| Consume | *ScopeCam* | Import to EGL, GPU waits on fence |
| Close | `FrameBufferRing.cpp:100` | `destroy()` closes all pending fences |

### 5.2 GPU Release Fence (Consumer → Producer)

| Step | Location | Action |
|------|----------|--------|
| Create | *ScopeCam* | `eglDupNativeFenceFDANDROID()` after glFlush |
| Pass | `FrameBufferJNI.cpp:237` | `findSlotByFrameNumber()` + `setGpuReleaseFence()` |
| Store | `FrameBufferRing.cpp:362` | `slot.gpuReleaseFenceFd = fenceFd` |
| Wait | `FrameBufferRing.cpp:139` | `poll(&pfd, 1, 33)` - 33ms timeout |
| Close | `FrameBufferRing.cpp:141` | Always close after wait (success or timeout) |

---

## 6. Timeout Handling

**Location:** `FrameBufferRing.cpp:139-149`

```cpp
int waitResult = poll(&pfd, 1, 33);  // 33ms = 1 frame @ 30fps
close(slot.gpuReleaseFenceFd);
slot.gpuReleaseFenceFd = -1;

if (waitResult <= 0) {
    // Timeout/error: proceed anyway for smooth streaming
    // GPU may see partial data (acceptable tradeoff)
    LOGW("GPU release fence wait timeout/error (%d), proceeding", waitResult);
}
```

**Design Decision:** On timeout, producer proceeds to write the frame anyway.
- **Pro:** Maintains smooth frame rate
- **Con:** Potential visual glitch if GPU is still reading
- **Rationale:** With triple buffering, this is rare; smoothness is prioritized

---

## 7. Error Recovery

### 7.1 Consumer Crash

If consumer crashes without calling `releaseReadBuffer()`:
- `isLockedByConsumer` stays `true` for that slot
- Producer checks this in `lockWriteBuffer()` (line 150-154)
- Returns `nullptr` to drop frame gracefully
- System degrades but doesn't crash

### 7.2 Producer Crash

If producer crashes:
- Consumer's `acquireReadBuffer()` returns `nullptr` (no new frames)
- Consumer should handle null gracefully (show last good frame)

### 7.3 Frame Number Race

If frame is recycled before consumer calls `releaseWithFence()`:
- `findSlotByFrameNumber()` returns -1
- Fence fd is closed to prevent leak
- Warning logged: "Frame already recycled, fence discarded"

---

## 8. Java API Usage Pattern

```java
// In ScopeCam HardwareBufferRenderer
public void renderFrame() {
    long handle = camera.getRingBufferHandle();
    if (handle == 0) return;  // Ring buffer not allocated

    // 1. Acquire latest frame
    HardwareBuffer buffer = nativeFrameBufferAcquireBuffer(handle);
    if (buffer == null) return;  // No frame available

    // 2. Get producer's fence for GPU synchronization
    int acquireFence = nativeFrameBufferGetAcquireFence(handle);
    long frameNumber = nativeFrameBufferGetFrameNumber(handle);

    try {
        // 3. Import fence to EGL (if available)
        if (acquireFence >= 0) {
            importFenceToEGL(acquireFence);
        }

        // 4. Render
        bindHardwareBufferAsTexture(buffer);
        drawQuad();

        // 5. Create GPU release fence
        glFlush();
        int releaseFence = createEGLFence();

        // 6. Swap and release with fence
        EGL14.eglSwapBuffers(display, surface);
        nativeFrameBufferReleaseWithFence(handle, frameNumber, releaseFence);

    } catch (Exception e) {
        // Fallback: release without fence
        nativeFrameBufferReleaseBuffer(handle);
    }
}
```

---

## 9. Verification Checklist

- [x] Producer waits on GPU fence before writing (line 139)
- [x] Producer closes fence after wait regardless of result (line 141)
- [x] Producer captures new acquire fence on unlock (line 246)
- [x] MAILBOX update uses release semantics (line 251)
- [x] Consumer uses acquire semantics on MAILBOX read (line 274)
- [x] Consumer increments refcount before returning buffer (line 290)
- [x] Consumer clears isLockedByConsumer on release (line 300)
- [x] Triple-buffer dance prevents overwriting active read slot (lines 255-260)
- [x] Frame number lookup handles recycled frames gracefully (lines 329-336)
- [x] All fences closed in destroy() (lines 99-104)
