# Ring Buffer Contract

**Version:** 1.0 (2026-01-05)
**Status:** Production-Critical

This document defines the thread-safety contract for the native `FrameBufferRing` used in UVC camera streaming.

---

## Ownership Model

### Creation
```kotlin
// Native ring buffer is created via JNI
val handle: Long = nativeFrameBufferCreate(width, height, format)
```

The caller owns the returned handle and is responsible for:
1. Storing the handle safely (never truncate to Int)
2. Calling `nativeFrameBufferDestroy()` exactly once before the handle becomes invalid
3. Not using the handle after destruction

### Injection
```kotlin
// Inject into camera preview
camera.setFrameBufferRing(handle)
```

After injection:
- The camera **borrows** the ring buffer (does not take ownership)
- The camera may access the ring buffer from USB callback threads
- The original owner remains responsible for destruction

### Safe Destruction Protocol
```kotlin
// WRONG: Direct destruction can race with USB callbacks
camera.stopPreview()
nativeFrameBufferDestroy(handle)  // RACE CONDITION!

// CORRECT: Let clearRingBuffer() handle the drain
camera.stopPreview()  // Internally calls clearRingBuffer() which:
                      // 1. Signals shutdown (sets atomic flags)
                      // 2. Drains in-flight callbacks (100ms max)
                      // 3. Deletes the ring buffer safely
// Handle is now invalid - do not use
```

---

## Thread Safety Rules

### Thread Roles
| Thread | Operations | Safety |
|--------|------------|--------|
| Main/UI | `setFrameBufferRing()`, `stopPreview()` | Single-threaded by contract |
| USB Callback | `enqueuePendingFrame()` | Protected by atomic guards |
| Conversion | `dequeuePendingFrame()`, `lockWriteBuffer()` | Protected by SPSC queue |
| Render/GL | `acquireReadBuffer()`, `releaseReadBuffer()` | Protected by slot ownership |

### Atomic Guarantees
The native implementation uses `std::atomic` with memory ordering:
- `mUseRingBuffer` - `memory_order_acquire/release` for fast-path rejection
- `mRingBufferInjected` - `memory_order_acquire/release` for validity check
- `mFrameBufferRing` - `memory_order_acq_rel` for pointer exchange
- `mCallbacksInFlight` - Tracks active USB callbacks for safe drain

### The Callback Guard Pattern
```cpp
void uvc_preview_frame_callback(uvc_frame_t *frame, void *user_ptr) {
    UVCPreview* preview = reinterpret_cast<UVCPreview*>(user_ptr);

    // RAII guard - increments counter on construction, decrements on exit
    CallbackGuard guard(preview->mCallbacksInFlight);

    // Fast rejection - if not using ring buffer, exit immediately
    if (!preview->mUseRingBuffer.load(std::memory_order_acquire)) {
        return;  // Guard destructor decrements counter
    }

    // Safe to use ring buffer - guard ensures cleanup waits for us
    FrameBufferRing* ring = preview->mFrameBufferRing.load(std::memory_order_acquire);
    // ... use ring ...
}
```

---

## Handle Validation

### JNI Handle Contract
- Handles are stored as `jlong` (64-bit) in Java/Kotlin
- Never truncate to `Int` or `int` - this causes sign-extension corruption
- Native code validates handles before use

### Validation Checks
The native `validateAndCastHandle()` function performs:
1. **Null check** - Rejects `handle == 0`
2. **Sign-extension detection** - Catches truncation corruption
3. **Magic number validation** - Detects stale/freed handles

Example corruption detection:
```
JNI[getWidth]: Detected sign-extended pointer! handle=0xffffffff708241d0
JNI[getWidth]: Handle 0xb40000708241d0 failed magic validation - stale!
```

---

## Magic Numbers

### Structure Canaries
```cpp
// FrameBufferRing header/footer
static constexpr uint64_t MAGIC_HEADER = 0xFB01CAFEBABE2026ULL;
static constexpr uint64_t MAGIC_FOOTER = 0xFB01DEADBEEF2026ULL;

// PendingFrame slot canaries
static constexpr uint32_t SLOT_MAGIC_VALUE = 0x534C4F54;  // "SLOT"
```

### Corruption Detection
If magic numbers don't match:
- Likely causes: use-after-free, heap overflow, MTE fault
- Action: Log diagnostics and abort immediately
- Diagnostic info includes thread ID, MTE tag, untagged address

---

## Error Handling

### Callback Drain Timeout
If callbacks don't drain within 100ms:
```
LIFECYCLE: Callback drain TIMEOUT! 2 callbacks still in flight.
LIFECYCLE: Proceeding with destruction - potential use-after-free risk!
```
This is a warning - destruction proceeds, but risk is documented.

### Magic Corruption
```
MAGIC_CORRUPT: header=0x0000000000000000 (expected 0xFB01CAFEBABE2026)
MAGIC_CORRUPT: FATAL - Aborting
```
This triggers immediate process termination to prevent undefined behavior.

---

## Layout Contract

### Compile-Time Validation
The native library includes static assertions:
```cpp
static_assert(sizeof(void*) == 4 || sizeof(void*) == 8);
static_assert(sizeof(jlong) == 8);
static_assert(sizeof(jlong) >= sizeof(void*));
static_assert(alignof(jlong) >= alignof(void*));
```

### Runtime Validation
On library load (`JNI_OnLoad`):
1. Layout diagnostics are logged (sizes, alignments)
2. Critical offsets are validated
3. Default-constructed structs are checked for valid magic

---

## Telemetry

### Available Metrics
| Metric | Description |
|--------|-------------|
| `framesReceived` | Total USB frames received |
| `framesDroppedQueueFull` | Frames dropped (SPSC queue full) |
| `framesConverted` | Frames successfully converted |
| `framesDroppedLockFailed` | Frames dropped (buffer lock failed) |
| `avgInPipeLatencyUs` | Average enqueue-to-convert latency |
| `peakInPipeLatencyUs` | Peak enqueue-to-convert latency |

### Telemetry Dump on Destruction
```
LIFECYCLE: Final telemetry: received=1500 dropped=12
LIFECYCLE:   avgInPipeLatencyUs=150 peakInPipeLatencyUs=2300
```

---

## Best Practices

### DO
- Store handles as `Long` in Kotlin, never `Int`
- Call `stopPreview()` before releasing camera
- Check return values from JNI methods
- Use the ScopeCam lifecycle patterns

### DON'T
- Call `nativeFrameBufferDestroy()` directly while camera is active
- Cache the native pointer - always use atomic loads
- Ignore `LIFECYCLE:` log warnings
- Assume callbacks stop immediately on `stopPreview()`

---

## Diagnostic Logs

### Log Tags
- `libUVCCamera/Preview` - Preview lifecycle events
- `libUVCCamera/FrameBufferRing` - Ring buffer operations
- `libUVCCamera/LayoutContract` - ABI validation
- `MAGIC_CORRUPT` - Fatal corruption detection
- `LIFECYCLE:` - Teardown protocol events

### Key Log Patterns
```
# Healthy injection
INJECT_DIAG: Storing ring buffer: this=0xb4...890

# Clean teardown
LIFECYCLE: clearRingBuffer() - beginning teardown
LIFECYCLE: Flags cleared, signaling shutdown
LIFECYCLE: Draining 1 in-flight callbacks...
LIFECYCLE: Callback drain complete after 3 retries (0.6ms)
LIFECYCLE: Deleting FrameBufferRing 0xb4...890
LIFECYCLE: FrameBufferRing deleted successfully

# Warning: race condition detected
INJECT_DIAG: WARNING - Replacing non-null ring buffer!
```
