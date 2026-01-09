# UVCCamera Fork - Current Status

**Last Updated:** 2026-01-09
**Branch:** `develop`
**Build Status:** ✅ Passing
**AAR Published:** `~/.m2/repository/org/uvccamera/lib/`

---

## Executive Summary

This fork adds **zero-copy ring buffer support** for GPU rendering via AHardwareBuffer. The core P0 fix ensures frames are committed to the ring buffer when a Kotlin GL consumer is active.

**The library is functional and ready for ScopeCam integration.**

---

## Recent Changes (2026-01-09)

### P0 Fix: Consumer-Aware Frame Routing

**Problem:** Ring buffer `mLatestCompleted` was stuck at -1, causing Kotlin consumer to starve.

**Root Cause:** The routing branch only checked `surfaceReady` (ANativeWindow), ignoring the ring buffer consumer path.

**Fix Applied:** `UVCPreview.cpp:2832-2836`
```cpp
bool ringConsumerActive = mUseRingBuffer.load(...) && mRingBufferInjected.load(...);
bool hasConsumer = surfaceReady || ringConsumerActive;
```

**Verification:**
```bash
adb logcat | grep -E "STATE_TRACE|RING_WRITE|latest="
# Expected: hasConsumer=1 → COMMIT, latest=0,1,2...
```

---

## Architecture Overview

```
USB Camera → SPSC Queue → Conversion Thread → Ring Buffer → Kotlin GL Consumer
                              ↓
                        Capture Callback (Recording/AI)
```

### Key Files

| File | Purpose |
|------|---------|
| `UVCPreview.cpp` | Conversion thread, frame routing, state machine |
| `FrameBufferRing.cpp` | Triple-buffered AHardwareBuffer ring with MAILBOX policy |
| `StreamTelemetry.h` | Telemetry counters for monitoring |
| `UVCCamera.java` | Java API surface |
| `ICaptureFrameCallback.java` | Capture callback interface |

### Frame Routing Logic

```cpp
// Location: UVCPreview.cpp:2823-2857
bool hasConsumer = surfaceReady || ringConsumerActive;

if (hasConsumer) {
    ring->unlockWriteBuffer();   // COMMIT
} else {
    ring->cancelWriteBuffer();   // CANCEL (active drain)
}
```

---

## What Works

- ✅ USB camera enumeration and connection
- ✅ MJPEG and YUYV frame decoding
- ✅ Ring buffer allocation and injection
- ✅ Frame routing to ring buffer consumer
- ✅ Capture callback for recording path
- ✅ WARM/HOT state transitions
- ✅ Telemetry and diagnostics

---

## What's Not Yet Implemented

- ❌ Native test infrastructure (documented in Native-Testing.md)
- ❌ Configurable JNI class names (documented in MODERNIZATION-PLAN.md)
- ❌ Copyright header updates for upstream contribution

---

## Build Commands

```bash
# Build release AAR
./gradlew :lib:assembleRelease

# Publish to local Maven (required for ScopeCam)
./gradlew :lib:publishToMavenLocal

# Full rebuild (if needed)
./gradlew clean :lib:assembleRelease
```

---

## Debugging

### Key Log Patterns

```bash
# Frame routing
adb logcat | grep "STATE_TRACE"

# Ring buffer writes
adb logcat | grep "RING_WRITE"

# Producer/consumer metrics
adb logcat | grep -E "latest=|consumerStarves"
```

### Telemetry Counters

| Counter | Meaning |
|---------|---------|
| `framesReceived` | Total frames from USB |
| `framesDroppedNoSurface` | Frames dropped (no consumer) |
| `consumerStarves` | Consumer found no frame ready |
| `latest` | Most recent committed frame index |

---

## File Organization

```
docs/
├── README.md                    # Documentation index
├── CURRENT-STATUS.md            # This file
├── architecture.md              # System architecture
├── api-reference.md             # Public API reference
├── MODERNIZATION-PLAN.md        # Future roadmap
└── archive/                     # Historical documents

lib/src/main/
├── java/com/serenegiant/usb/
│   ├── UVCCamera.java           # Main Java API
│   └── ICaptureFrameCallback.java
└── jni/UVCCamera/
    ├── UVCPreview.cpp           # Frame routing (P0 fix here)
    ├── FrameBufferRing.cpp      # Ring buffer implementation
    └── StreamTelemetry.h        # Telemetry
```

---

## For New Developers

1. **Read first:** [architecture.md](./architecture.md) for system overview
2. **API usage:** [api-reference.md](./api-reference.md) for public methods
3. **Integration:** [ScopeCam-Integration-Guide.md](./ScopeCam-Integration-Guide.md) for app integration
4. **Build:** Run `./gradlew :lib:publishToMavenLocal` before ScopeCam development

---

## Known Issues

None currently blocking.

---

## Contact

This fork is maintained for ScopeCam integration. See the original [UVCCamera repository](https://github.com/alexey-pelykh/UVCCamera) for upstream issues.
