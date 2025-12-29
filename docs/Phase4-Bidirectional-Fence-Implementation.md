# Phase 4: Bidirectional Fence Implementation

## UVCCamera Library (org.uvccamera:lib)

This document specifies the Phase 4 implementation for the UVCCamera library,
enabling bidirectional GPU/CPU fence synchronization for zero-copy rendering.

**Prerequisite**: Phases 1-3 completed (FrameBufferRing, JNI Bridge, UVCPreview Integration)

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│  PRODUCER (Native USB/libuvc Thread)                                   │
│                                                                         │
│  1. Wait on gpuReleaseFenceFd via sync_wait() before writing           │
│  2. Lock buffer, write frame data                                       │
│  3. Unlock and capture acquireFenceFd for consumer                      │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
                              │
                              │ JNI: nativeAcquireFrame()
                              │ Returns: FrameData with acquireFenceFd
                              ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  CONSUMER (Kotlin/OpenGL Render Thread - in ScopeCam app)              │
│                                                                         │
│  4. Import acquireFenceFd into EGL (GPU waits for CPU write)           │
│  5. Bind HardwareBuffer as texture, draw                                │
│  6. Create gpuReleaseFenceFd via eglDupNativeFenceFDANDROID            │
│  7. Return fence via nativeReleaseFrame(frameNumber, gpuReleaseFenceFd)│
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Changes Required

### 2.1 FrameSlotMetadata.h

Update metadata structure for bidirectional fences:

```cpp
struct FrameSlotMetadata {
    // Frame identification
    int64_t  timestampNs;
    uint64_t frameNumber;      // Monotonic counter for race-safe release

    // Buffer dimensions
    uint32_t format;
    uint32_t width;
    uint32_t height;
    int32_t  strideBytes;

    // BIDIRECTIONAL FENCE FIELDS
    int acquireFenceFd;        // Producer → Consumer (from AHardwareBuffer_unlock)
    int gpuReleaseFenceFd;     // Consumer → Producer (from GPU via JNI)

    // State tracking
    bool valid;
    bool isLockedByConsumer;   // Prevents producer overwrite during render

    void reset();
};
```

### 2.2 FrameBufferRing.h

Add new methods:

```cpp
class FrameBufferRing {
public:
    // ... existing methods ...

    // Frame lookup for race-safe release
    int findSlotByFrameNumber(uint64_t frameNumber);

    // Metadata access for JNI
    FrameSlotMetadata* getMetadata(int slotIndex);

    // GPU release fence handling
    void setGpuReleaseFence(int slotIndex, int fenceFd);

private:
    // ... existing members ...
};
```

### 2.3 FrameBufferRing.cpp

**lockWriteBuffer** - Wait on GPU release fence:

```cpp
void* FrameBufferRing::lockWriteBuffer(int32_t* outStrideBytes) {
    int idx = mWriteIndex.load(std::memory_order_relaxed);
    FrameSlotMetadata& slot = mMetadata[idx];

    // Wait for GPU to finish reading this buffer before we write
    // Use poll() as a portable sync fence wait (compatible with all NDK versions)
    if (slot.gpuReleaseFenceFd >= 0) {
        struct pollfd pfd = {
            .fd = slot.gpuReleaseFenceFd,
            .events = POLLIN,
            .revents = 0
        };
        int waitResult = poll(&pfd, 1, 33); // 33ms timeout
        if (waitResult <= 0) {
            mTelemetry.framesDropped.fetch_add(1, std::memory_order_relaxed);
        }
        close(slot.gpuReleaseFenceFd);
        slot.gpuReleaseFenceFd = -1;
    }

    // ... rest of existing lock implementation ...
}
```

**findSlotByFrameNumber** - Race-safe slot lookup:

```cpp
int FrameBufferRing::findSlotByFrameNumber(uint64_t frameNumber) {
    for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
        if (mMetadata[i].valid && mMetadata[i].frameNumber == frameNumber) {
            return i;
        }
    }
    return -1; // Frame already recycled
}
```

**setGpuReleaseFence** - Store GPU's release fence:

```cpp
void FrameBufferRing::setGpuReleaseFence(int slotIndex, int fenceFd) {
    if (slotIndex < 0 || slotIndex >= FRAME_BUFFER_COUNT) {
        if (fenceFd >= 0) close(fenceFd);
        return;
    }

    FrameSlotMetadata& slot = mMetadata[slotIndex];

    // Close old fence if exists (shouldn't happen, but defensive)
    if (slot.gpuReleaseFenceFd >= 0) {
        close(slot.gpuReleaseFenceFd);
    }

    slot.gpuReleaseFenceFd = fenceFd;
    slot.isLockedByConsumer = false;
}
```

### 2.4 FrameBufferJNI.cpp

**Update nativeFrameBufferAcquireBuffer** to return fence:

Current signature: `(J)Landroid/hardware/HardwareBuffer;`
New method: `nativeAcquireFrame` returning a data structure

For simplicity and to avoid creating a new JNI class, we'll add a separate
method to get the fence:

```cpp
// Get acquire fence for most recently acquired buffer
static jint nativeFrameBufferGetAcquireFence(JNIEnv *env, jobject thiz, jlong handle) {
    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    if (!ring) return -1;

    int idx = ring->getCurrentReadIndex();
    if (idx < 0) return -1;

    FrameSlotMetadata *meta = ring->getMetadata(idx);
    return meta ? meta->acquireFenceFd : -1;
}

// Get frame number for most recently acquired buffer
static jlong nativeFrameBufferGetFrameNumber(JNIEnv *env, jobject thiz, jlong handle) {
    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    if (!ring) return -1;

    int idx = ring->getCurrentReadIndex();
    if (idx < 0) return -1;

    FrameSlotMetadata *meta = ring->getMetadata(idx);
    return meta ? static_cast<jlong>(meta->frameNumber) : -1;
}

// Release with GPU fence
static void nativeFrameBufferReleaseWithFence(JNIEnv *env, jobject thiz,
    jlong handle, jlong frameNumber, jint gpuReleaseFenceFd) {

    FrameBufferRing *ring = reinterpret_cast<FrameBufferRing *>(handle);
    if (!ring) {
        if (gpuReleaseFenceFd >= 0) close(gpuReleaseFenceFd);
        return;
    }

    int slotIndex = ring->findSlotByFrameNumber(static_cast<uint64_t>(frameNumber));

    if (slotIndex < 0) {
        // Frame already recycled; close fence to prevent leak
        if (gpuReleaseFenceFd >= 0) close(gpuReleaseFenceFd);
        return;
    }

    ring->setGpuReleaseFence(slotIndex, gpuReleaseFenceFd);
    ring->releaseReadBuffer();
}
```

### 2.5 Android.mk

No additional libraries needed. The poll()-based fence wait uses standard POSIX APIs
that are included in the Android libc. The existing `-landroid` linkage is sufficient.

---

## 3. JNI Method Summary

| Method | Signature | Purpose |
|--------|-----------|---------|
| `nativeFrameBufferAllocate` | `(III)J` | Allocate ring buffer |
| `nativeFrameBufferDestroy` | `(J)V` | Destroy ring buffer |
| `nativeFrameBufferAcquireBuffer` | `(J)Landroid/hardware/HardwareBuffer;` | Get latest frame |
| `nativeFrameBufferGetAcquireFence` | `(J)I` | Get producer's fence fd |
| `nativeFrameBufferGetFrameNumber` | `(J)J` | Get current frame number |
| `nativeFrameBufferReleaseWithFence` | `(JJI)V` | Release with GPU fence |
| `nativeFrameBufferReleaseBuffer` | `(J)V` | Release without fence (fallback) |
| `nativeFrameBufferIsAllocated` | `(J)Z` | Check allocation state |
| `nativeFrameBufferGetWidth` | `(J)I` | Get buffer width |
| `nativeFrameBufferGetHeight` | `(J)I` | Get buffer height |
| `nativeFrameBufferGetFramesReceived` | `(J)J` | Telemetry: frames in |
| `nativeFrameBufferGetFramesRendered` | `(J)J` | Telemetry: frames out |
| `nativeFrameBufferGetFramesDropped` | `(J)J` | Telemetry: frames dropped |

---

## 4. Java/Kotlin Declarations

Add to `UVCCamera.java`:

```java
// Bidirectional fence support (Phase 4)
private native int nativeFrameBufferGetAcquireFence(long handle);
private native long nativeFrameBufferGetFrameNumber(long handle);
private native void nativeFrameBufferReleaseWithFence(long handle, long frameNumber, int gpuReleaseFenceFd);
```

---

## 5. Implementation Checklist

- [ ] Update FrameSlotMetadata.h with bidirectional fence fields
- [ ] Add findSlotByFrameNumber() to FrameBufferRing
- [ ] Add getMetadata() accessor to FrameBufferRing
- [ ] Add getCurrentReadIndex() accessor to FrameBufferRing
- [ ] Add setGpuReleaseFence() to FrameBufferRing
- [ ] Update lockWriteBuffer() to sync_wait on GPU fence
- [ ] Add nativeFrameBufferGetAcquireFence JNI method
- [ ] Add nativeFrameBufferGetFrameNumber JNI method
- [ ] Add nativeFrameBufferReleaseWithFence JNI method
- [ ] Update Android.mk to link libsync
- [ ] Add Java declarations in UVCCamera.java
- [ ] Build and verify no compiler errors
- [ ] Test: verify fence fd lifecycle (no leaks)

---

## 6. Consumer Integration (ScopeCam App)

The ScopeCam application will use these methods as follows:

```kotlin
// In HardwareBufferRenderer
fun renderFrame(): Boolean {
    val buffer = nativeFrameBufferAcquireBuffer(handle) ?: return false
    val acquireFence = nativeFrameBufferGetAcquireFence(handle)
    val frameNumber = nativeFrameBufferGetFrameNumber(handle)

    try {
        // Wait for producer's fence
        if (acquireFence >= 0) {
            waitForAcquireFence(acquireFence)  // EGL takes ownership
        }

        // Render
        bindAndDraw(buffer)

        // Create release fence
        val releaseFence = createReleaseFence()

        // Swap and release with fence
        EGL14.eglSwapBuffers(display, surface)
        nativeFrameBufferReleaseWithFence(handle, frameNumber, releaseFence)

        return true
    } catch (e: Exception) {
        nativeFrameBufferReleaseBuffer(handle)  // Fallback
        return false
    }
}
```

---

## 7. Compatibility Notes

- **poll()-based fence wait** uses standard POSIX APIs available in all NDK versions
- Alternative: `sync_wait()` requires `<android/sync.h>` (NDK r23+) or `<sync/sync.h>`
- Fallback for devices without fence support: use `nativeFrameBufferReleaseBuffer()`
- Consumer should check if fence fd is -1 and skip fence import
