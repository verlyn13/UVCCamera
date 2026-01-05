---
title: Native Testing Expert Assessment & Implementation Guide
category: testing
component: native
status: active
version: "1.0"
last_updated: 2024-12-29
tags: [testing, mocks, implementation, android-api]
priority: high
related: [Native-Testing.md]
---

# Native Testing Expert Assessment & Implementation Guide

This document provides expert assessment of the native testing infrastructure plan and detailed implementation guidance for Android API mocks.

## Expert Assessment

### What's Good

**Build System Isolation:** Keeping CMake in a separate `test/` directory rather than trying to retrofit the existing Android.mk is the right call. This avoids polluting the production build and lets host tests evolve independently.

**Contract-First Approach:** Defining `TelemetryContract.h` as the authoritative source is correct. The static assertions in `ContractVerification.h` will catch drift at compile time.

**Mock Granularity:** The `MockControl` namespace for test configuration is clean and follows Google Test patterns.

### Critical Issues to Address

#### Issue 1: StreamTelemetry.h Inclusion Problem

The plan assumes `StreamTelemetry.h` can be included directly in host tests, but based on the specifications, this header likely contains:

```cpp
#include <android/hardware_buffer.h>  // Won't exist on host
#include <poll.h>                      // Different on macOS vs Linux
```

**Required Fix:** The production header needs conditional compilation guards:

```cpp
// StreamTelemetry.h - add near top
#ifdef UVCCAMERA_TESTING
    #include "AndroidApiMocks.h"  // Use mocks
#else
    #include <android/hardware_buffer.h>
    #include <poll.h>
#endif
```

**Action for Native Agent:** Audit `StreamTelemetry.h` and `FrameBufferRing.h` for Android-specific includes. Document which headers need `#ifdef UVCCAMERA_TESTING` guards. This is a production code change that must be reviewed carefully.

#### Issue 2: FrameBufferRing.cpp Not Testable As-Is

The plan's `FrameBufferRingLogicTest.cpp` only tests index rotation logic independently—it doesn't actually test `FrameBufferRing.cpp`. The comment acknowledges this:

```cpp
// Note: We need a testable version of FrameBufferRing that compiles with mocks
// For now, test the logic patterns independently
```

This is a significant gap. The core ring buffer logic is untested.

**Required Fix:** Either:

1. **Option A (Recommended):** Add `#ifdef UVCCAMERA_TESTING` guards to `FrameBufferRing.cpp` so it can compile with mocks, then add it to the test build:

```cmake
add_executable(native_tests
    # Production code under test (with mock guards)
    ${PRODUCTION_SRC}/FrameBufferRing.cpp

    # Test files
    tests/ContractTest.cpp
    ...
)
```

2. **Option B:** Extract pure logic into separate functions that don't touch Android APIs, test those in isolation.

**Action for Native Agent:** Choose an approach and document the production code modifications required.

#### Issue 3: Missing `poll()` Mock

The fence synchronization code likely uses `poll()` for fence waiting:

```cpp
// From Fence Synchronization spec
int result = sync_wait(slot.gpuReleaseFenceFd, 33);  // Uses poll() internally
```

The mock header has no `poll()` stub.

**Required Fix:** Add to `AndroidApiMocks.h`:

```cpp
// === poll() Mock (for fence waiting) ===
#include <sys/poll.h>  // For struct pollfd definition

// On macOS, poll() exists but we want to control behavior
#ifdef __APPLE__
// macOS has poll(), but we override for testing
#define poll mock_poll
#endif

int mock_poll(struct pollfd* fds, nfds_t nfds, int timeout);

namespace MockControl {
    void setPollReturnValue(int value);
    void setPollBehavior(std::function<int(struct pollfd*, nfds_t, int)> behavior);
}
```

#### Issue 4: No `clock_gettime` Mock

`StreamTelemetry` uses timestamps:

```cpp
int64_t getCurrentTimeNs();  // Uses CLOCK_MONOTONIC
```

This will behave differently on host vs Android and needs either:
- A mock implementation for deterministic testing
- Acceptance that timing tests will be non-deterministic

**Required Fix:** Add to mocks:

```cpp
// === Time Mocks ===
namespace MockControl {
    void setCurrentTimeNs(int64_t time);
    void advanceTimeNs(int64_t delta);
}
```

#### Issue 5: Contract Test Gap - JNI Signatures Not Verified

The contract defines structural constants and enum ordinals, but the JNI method signatures from the coordination document are missing:

```cpp
// Missing from TelemetryContract.h:
namespace JniSignatures {
    constexpr const char* NATIVE_TELEMETRY_CTOR =
        "(JJJIIZIIJJJJJJJJ[IJJIIIIJLjava/lang/String;...)V";
}
```

These signatures are critical for the Kotlin agent to implement matching data classes.

**Required Fix:** Add JNI signature documentation to `TelemetryContract.h`, or create a separate `JniContract.h`.

#### Issue 6: macOS vs Linux Compatibility

The CI runs on `ubuntu-latest`, but developers likely use macOS. Some differences:

- `poll.h` location differs
- `clock_gettime` behavior differs
- Atomic implementation details differ

**Required Fix:** Add macOS CI job or document macOS-specific build requirements:

```yaml
native-tests:
  strategy:
    matrix:
      os: [ubuntu-latest, macos-latest]
  runs-on: ${{ matrix.os }}
```

### Minor Issues

#### Issue 7: Missing Test for Error Source Truncation

The `StreamTelemetryTest.cpp` tests circular buffer behavior but not the source string truncation logic from the spec:

```cpp
// Should test this behavior:
strncpy(errorHistory[idx].source, source, 15);
errorHistory[idx].source[15] = '\0';
```

**Add test:**
```cpp
TEST_F(StreamTelemetryTest, ErrorSourceTruncatedTo15Chars) {
    telemetry.recordError(1, "this_is_a_very_long_source_name");
    // Verify truncation
    EXPECT_LE(strlen(telemetry.errorHistory[0].source), 15);
}
```

#### Issue 8: No Negative Test for Lock Without Allocate

The mock tests verify happy paths but don't test error conditions in the ring buffer:

```cpp
TEST_F(FrameBufferRingLogicTest, LockBeforeAllocateFails) {
    // Should return nullptr or error
}

TEST_F(FrameBufferRingLogicTest, DoubleUnlockHandled) {
    // Should not crash
}
```

### Revised Plan Recommendation

Here's what I recommend the native agent address before proceeding:

```
BLOCKING CHANGES (must fix before approval):

□ 1. Audit production headers for Android-specific includes
     - List all #include <android/*.h> in StreamTelemetry.h, FrameBufferRing.h
     - Add #ifdef UVCCAMERA_TESTING guards (document exact changes)

□ 2. Make FrameBufferRing.cpp compile-testable
     - Add necessary guards for mock compatibility
     - Add to CMakeLists.txt test build
     - Add actual FrameBufferRing unit tests (not just logic patterns)

□ 3. Add poll() mock for fence synchronization testing

□ 4. Add JNI signature constants to TelemetryContract.h
     - Document the 25-parameter NativeTelemetry constructor signature
     - This is the handoff contract for Kotlin agent

RECOMMENDED CHANGES (improve quality):

□ 5. Add clock_gettime mock for deterministic timestamp testing

□ 6. Add macOS to CI matrix

□ 7. Add error source truncation test

□ 8. Add negative/edge case tests for ring buffer
```

### Specific Feedback for Native Agent

Send this directive to the native agent:

---

**Native Agent: Plan Review Feedback**

Your testing infrastructure plan is approved with required modifications. The core architecture (separate CMake build, mock infrastructure, contract-first approach) is sound.

**Required Changes Before Implementation:**

1. **Production Code Audit:** Before creating test files, audit these production headers and document which Android-specific includes they contain:
   - `StreamTelemetry.h`
   - `FrameBufferRing.h`
   - `FrameSlotMetadata.h`
   - `FrameBufferJNI.cpp`

   For each Android include found, propose the `#ifdef UVCCAMERA_TESTING` guard placement.

2. **FrameBufferRing Testing Strategy:** Your current `FrameBufferRingLogicTest.cpp` only tests index math in isolation. Revise the plan to either:
   - (A) Add guards to `FrameBufferRing.cpp` and include it in the test build, OR
   - (B) Extract testable pure functions from `FrameBufferRing.cpp` into a separate header

   Document your choice and the implementation approach.

3. **Add Missing Mocks:**
   - `poll()` or `sync_wait()` for fence testing
   - `clock_gettime()` for timestamp testing (optional but recommended)

4. **JNI Signature Contract:** Add to `TelemetryContract.h`:
   ```cpp
   namespace JniSignatures {
       // Document the exact parameter order for NativeTelemetry constructor
       // This is the handoff contract for the Kotlin agent
   }
   ```

5. **Update CMakeLists.txt:** After addressing #2, update to include production source files under test.

**Deliverable:** Revised plan document addressing items 1-5, then proceed with implementation.

---

### Impact on Coordination Timeline

If these changes are made, add 1-2 days to the native agent's Phase N2 (Test Infrastructure). The blocking dependency for the Kotlin agent (contract handoff) should not be delayed—`TelemetryContract.h` can be delivered on schedule even while the FrameBufferRing testing strategy is being refined.

---

## Production Code Guard Implementation Guide

### Guiding Principles

1. **Zero Runtime Impact:** Guards only affect compilation, never runtime behavior
2. **Minimal Invasion:** Change as few lines as possible in production code
3. **Single Point of Truth:** Mock definitions live in test infrastructure, not scattered in production
4. **Build Verification:** Every guarded file must still compile for Android after changes

---

### Guard Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         COMPILATION PATHS                               │
│                                                                         │
│  ANDROID BUILD (ndk-build)              HOST TEST BUILD (cmake)         │
│  ────────────────────────               ─────────────────────           │
│  UVCCAMERA_TESTING = undefined          UVCCAMERA_TESTING = 1           │
│           │                                      │                      │
│           ▼                                      ▼                      │
│  #include <android/hardware_buffer.h>   #include "AndroidApiMocks.h"   │
│  #include <android/log.h>               // Mocks provide same API      │
│  Real AHardwareBuffer                   Mock AHardwareBuffer            │
│  Real __android_log_print               Stub LOGE/LOGW/etc             │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

### File-by-File Guard Implementation

#### 1. StreamTelemetry.h

**Current State (hypothetical based on specs):**
```cpp
#ifndef STREAMTELEMETRY_H
#define STREAMTELEMETRY_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <android/log.h>  // Problem: Android-only

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "UVCCamera", __VA_ARGS__)

// ... rest of file
```

**Guarded Version:**
```cpp
#ifndef STREAMTELEMETRY_H
#define STREAMTELEMETRY_H

// === Standard Library (always available) ===
#include <atomic>
#include <cstdint>
#include <cstring>

// === Platform-Specific Includes ===
#ifdef UVCCAMERA_TESTING
    // Host testing: Use mock definitions
    // Note: AndroidApiMocks.h provides LOGE, LOGW, etc. as no-ops
    // and any Android types we need
    #include "AndroidApiMocks.h"
#else
    // Android production build
    #include <android/log.h>

    #define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "StreamTelemetry", __VA_ARGS__)
    #define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "StreamTelemetry", __VA_ARGS__)
    #define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "StreamTelemetry", __VA_ARGS__)
    #define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "StreamTelemetry", __VA_ARGS__)
    #define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, "StreamTelemetry", __VA_ARGS__)
#endif

// === Constants (platform-independent) ===
#define FRAME_BUFFER_COUNT 3
#define ERROR_HISTORY_SIZE 8

// ... rest of StreamTelemetry struct (no changes needed)
```

**Key Points:**
- Standard library includes (`<atomic>`, `<cstdint>`) stay outside guards—they work everywhere
- Only Android-specific includes go inside the `#else` branch
- Logging macros defined in both paths (real vs no-op)

---

#### 2. FrameBufferRing.h

**Current State (hypothetical):**
```cpp
#ifndef FRAMEBUFFERRING_H
#define FRAMEBUFFERRING_H

#include <android/hardware_buffer.h>  // Problem: Android-only
#include "StreamTelemetry.h"
#include "FrameSlotMetadata.h"

class FrameBufferRing {
public:
    int allocate(uint32_t width, uint32_t height, uint32_t format);
    void destroy();

    void* lockWriteBuffer(int32_t* outStrideBytes);
    void unlockWriteBuffer();

    AHardwareBuffer* acquireReadBuffer(FrameSlotMetadata* outMetadata);
    void releaseReadBuffer();

    // ...

private:
    AHardwareBuffer* mBuffers[FRAME_BUFFER_COUNT];  // Uses Android type
    // ...
};
```

**Guarded Version:**
```cpp
#ifndef FRAMEBUFFERRING_H
#define FRAMEBUFFERRING_H

// === Standard Library ===
#include <atomic>
#include <cstdint>

// === Platform-Specific: AHardwareBuffer ===
#ifdef UVCCAMERA_TESTING
    #include "AndroidApiMocks.h"  // Provides mock AHardwareBuffer
#else
    #include <android/hardware_buffer.h>
#endif

// === Project Headers (must come AFTER platform includes) ===
#include "StreamTelemetry.h"
#include "FrameSlotMetadata.h"

class FrameBufferRing {
    // No changes to class definition needed!
    // AHardwareBuffer* works with both real and mock types
    // ...
};

#endif // FRAMEBUFFERRING_H
```

**Key Points:**
- `AHardwareBuffer*` pointer type works with both real and mock structs
- Project headers that depend on platform types must come AFTER the platform include guard
- The class interface doesn't change at all

---

#### 3. FrameBufferRing.cpp

**Current State (hypothetical):**
```cpp
#include "FrameBufferRing.h"
#include <android/hardware_buffer.h>
#include <poll.h>  // For fence waiting

int FrameBufferRing::allocate(uint32_t width, uint32_t height, uint32_t format) {
    AHardwareBuffer_Desc desc = {
        .width = width,
        .height = height,
        // ...
    };

    for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
        int result = AHardwareBuffer_allocate(&desc, &mBuffers[i]);
        if (result != 0) {
            LOGE("AHardwareBuffer_allocate failed: %d", result);
            destroy();
            return result;
        }
    }
    // ...
}

void* FrameBufferRing::lockWriteBuffer(int32_t* outStrideBytes) {
    // Check GPU release fence before writing
    if (mMetadata[idx].gpuReleaseFenceFd >= 0) {
        struct pollfd pfd = { .fd = mMetadata[idx].gpuReleaseFenceFd, .events = POLLIN };
        int ret = poll(&pfd, 1, 33);  // 33ms timeout
        if (ret < 0) {
            LOGW("Fence wait failed: %d", errno);
        }
        close(mMetadata[idx].gpuReleaseFenceFd);
        mMetadata[idx].gpuReleaseFenceFd = -1;
    }

#if __ANDROID_API__ >= 29
    int result = AHardwareBuffer_lockAndGetInfo(...);
#else
    int result = AHardwareBuffer_lock(...);
#endif
    // ...
}
```

**Guarded Version:**
```cpp
#include "FrameBufferRing.h"

// === Platform-Specific Includes ===
#ifdef UVCCAMERA_TESTING
    #include "AndroidApiMocks.h"
    // poll() mock is provided by AndroidApiMocks.h
#else
    #include <android/hardware_buffer.h>
    #include <poll.h>
    #include <unistd.h>  // close()
    #include <errno.h>
#endif

// === Implementation ===

int FrameBufferRing::allocate(uint32_t width, uint32_t height, uint32_t format) {
    // NO CHANGES TO FUNCTION BODY
    // AHardwareBuffer_allocate() resolves to mock in test build
    AHardwareBuffer_Desc desc = {
        .width = width,
        .height = height,
        // ...
    };

    for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
        int result = AHardwareBuffer_allocate(&desc, &mBuffers[i]);
        if (result != 0) {
            LOGE("AHardwareBuffer_allocate failed: %d", result);
            destroy();
            return result;
        }
    }
    // ...
}

void* FrameBufferRing::lockWriteBuffer(int32_t* outStrideBytes) {
    // Check GPU release fence before writing
    if (mMetadata[idx].gpuReleaseFenceFd >= 0) {
        struct pollfd pfd = { .fd = mMetadata[idx].gpuReleaseFenceFd, .events = POLLIN };
        int ret = poll(&pfd, 1, 33);  // Resolves to mock_poll in test build
        if (ret < 0) {
            LOGW("Fence wait failed");
        }
        close(mMetadata[idx].gpuReleaseFenceFd);  // Need to mock this too
        mMetadata[idx].gpuReleaseFenceFd = -1;
    }

    // API level check still works—mock provides both functions
#if __ANDROID_API__ >= 29 || defined(UVCCAMERA_TESTING)
    int result = AHardwareBuffer_lockAndGetInfo(...);
#else
    int result = AHardwareBuffer_lock(...);
#endif
    // ...
}
```

**Key Points:**
- Function bodies remain unchanged—they call the same API names
- In test build, those names resolve to mock functions
- The `__ANDROID_API__` check needs special handling (see below)

---

#### 4. Handling __ANDROID_API__ Checks

Production code often has API level checks:

```cpp
#if __ANDROID_API__ >= 29
    AHardwareBuffer_lockAndGetInfo(...);  // API 29+
#else
    AHardwareBuffer_lock(...);            // API 26-28
#endif
```

**Problem:** `__ANDROID_API__` is undefined on host, so only the `#else` branch compiles.

**Solution:** Ensure mocks provide ALL API variants, then adjust the check:

```cpp
#if __ANDROID_API__ >= 29 || defined(UVCCAMERA_TESTING)
    // Use newer API (real on Android 29+, mock in tests)
    AHardwareBuffer_lockAndGetInfo(...);
#else
    // Fallback for Android 26-28 only
    AHardwareBuffer_lock(...);
#endif
```

**Alternative:** Define a compat macro:

```cpp
// In a common header or AndroidApiMocks.h
#ifdef UVCCAMERA_TESTING
    #define UVCCAMERA_API_LEVEL 29  // Test against API 29 behavior
#else
    #define UVCCAMERA_API_LEVEL __ANDROID_API__
#endif

// In production code:
#if UVCCAMERA_API_LEVEL >= 29
    AHardwareBuffer_lockAndGetInfo(...);
#else
    AHardwareBuffer_lock(...);
#endif
```

---

#### 5. FrameSlotMetadata.h

This file likely has no Android dependencies—just data types:

```cpp
#ifndef FRAMESLOTMETADATA_H
#define FRAMESLOTMETADATA_H

#include <cstdint>

struct FrameSlotMetadata {
    int64_t  timestampNs;
    uint64_t frameNumber;
    uint32_t format, width, height;
    int32_t  strideBytes;
    int      acquireFenceFd;
    int      gpuReleaseFenceFd;
    bool     valid;
    bool     isLockedByConsumer;

    void reset() { /* ... */ }
};

#endif
```

**Assessment:** No guards needed—this is pure C++ with standard types.

---

#### 6. FrameBufferJNI.cpp

JNI code requires special handling because it includes `<jni.h>` and uses JNI types.

**Current State:**
```cpp
#include <jni.h>
#include <android/hardware_buffer_jni.h>  // AHardwareBuffer_toHardwareBuffer
#include "FrameBufferRing.h"

extern "C" JNIEXPORT jobject JNICALL
Java_com_scopecam_camera_FrameBufferManager_nativeAcquireFrame(
    JNIEnv* env, jobject thiz, jlong handle) {

    auto* ring = reinterpret_cast<FrameBufferRing*>(handle);
    FrameSlotMetadata metadata;

    AHardwareBuffer* buffer = ring->acquireReadBuffer(&metadata);
    if (!buffer) return nullptr;

    // Convert to Java HardwareBuffer
    jobject hwBuffer = AHardwareBuffer_toHardwareBuffer(env, buffer);
    // ...
}
```

**Strategy:** JNI code is NOT included in host tests. It's Android-only by nature.

```cpp
// FrameBufferJNI.cpp - NO GUARDS NEEDED
// This file is only compiled for Android (via Android.mk)
// It is NOT included in the CMake test build

#include <jni.h>
#include <android/hardware_buffer_jni.h>
#include "FrameBufferRing.h"

// ... all JNI functions unchanged
```

**CMakeLists.txt:**
```cmake
add_executable(native_tests
    # Do NOT include FrameBufferJNI.cpp - it's Android-only
    ${PRODUCTION_SRC}/FrameBufferRing.cpp
    ${PRODUCTION_SRC}/StreamTelemetry.cpp  # If exists

    tests/ContractTest.cpp
    tests/StreamTelemetryTest.cpp
    tests/FrameBufferRingLogicTest.cpp

    mocks/AndroidApiMocks.cpp
)
```

---

### Updated AndroidApiMocks.h (Complete)

Based on the guard analysis, here's the complete mock header:

```cpp
// AndroidApiMocks.h
// Mock implementations of Android APIs for host testing

#pragma once

#ifdef UVCCAMERA_TESTING

#include <cstdint>
#include <vector>
#include <functional>

// === Simulate API Level for Conditional Compilation ===
#ifndef __ANDROID_API__
    #define __ANDROID_API__ 29  // Test against API 29 behavior
#endif

// === AHardwareBuffer Mock ===

struct AHardwareBuffer_Desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t rfu0;
    uint64_t rfu1;
};

struct AHardwareBuffer {
    AHardwareBuffer_Desc desc;
    std::vector<uint8_t> mockMemory;
    int refCount = 1;
    int lockCount = 0;
    bool isValid = true;
};

// AHardwareBuffer API
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer);
void AHardwareBuffer_release(AHardwareBuffer* buffer);
void AHardwareBuffer_acquire(AHardwareBuffer* buffer);
void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc);
int AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence,
                         const void* rect, void** outVirtualAddress);
int AHardwareBuffer_unlock(AHardwareBuffer* buffer, int32_t* outFence);

// API 29+ function
int AHardwareBuffer_lockAndGetInfo(AHardwareBuffer* buffer, uint64_t usage,
                                    int32_t fence, const void* rect,
                                    void** outVirtualAddress,
                                    int32_t* outBytesPerPixel,
                                    int32_t* outBytesPerStride);

// Usage flags (must match Android values)
#define AHARDWAREBUFFER_USAGE_CPU_READ_NEVER     0x00000000ULL
#define AHARDWAREBUFFER_USAGE_CPU_READ_RARELY    0x00000002ULL
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN     0x00000003ULL
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER    0x00000000ULL
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY   0x00000020ULL
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN    0x00000030ULL
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE  0x00000100ULL
#define AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT   0x00000200ULL

// Format values (must match Android values)
#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM    1
#define AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM    2
#define AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM      3

// === poll() Mock ===

// Provide pollfd struct if not available
#ifndef POLLIN
    #define POLLIN  0x001
    #define POLLOUT 0x004
    #define POLLERR 0x008

    struct pollfd {
        int   fd;
        short events;
        short revents;
    };

    typedef unsigned long nfds_t;
#endif

int poll(struct pollfd* fds, nfds_t nfds, int timeout);

// === close() Mock ===
int close(int fd);

// === clock_gettime() Mock ===
#ifndef CLOCK_MONOTONIC
    #define CLOCK_MONOTONIC 1
#endif

struct timespec {
    long tv_sec;
    long tv_nsec;
};

int clock_gettime(int clk_id, struct timespec* tp);

// === Logging Macros (No-ops) ===
#define LOGE(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
#define LOGD(...) ((void)0)
#define LOGV(...) ((void)0)

// === Branch Hints ===
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)

// === Mock Control Interface ===
namespace MockControl {
    // AHardwareBuffer controls
    void setAllocateFailAfter(int successCount);
    void setLockReturnStride(int32_t stride);
    void setUnlockReturnFence(int32_t fenceFd);

    // poll() controls
    void setPollReturnValue(int value);
    void setPollShouldSignal(bool signal);

    // clock_gettime() controls
    void setCurrentTimeNs(int64_t timeNs);
    void advanceTimeNs(int64_t deltaNs);

    // close() tracking
    int getCloseCallCount();
    int getLastClosedFd();

    // Reset all mocks
    void reset();

    // Inspection
    int getAllocateCallCount();
    int getReleaseCallCount();
    int getLockCallCount();
    int getUnlockCallCount();
    int getPollCallCount();
}

#endif // UVCCAMERA_TESTING
```

---

### Updated AndroidApiMocks.cpp (Complete)

```cpp
// AndroidApiMocks.cpp

#ifdef UVCCAMERA_TESTING

#include "AndroidApiMocks.h"
#include <cstring>

namespace {
    // AHardwareBuffer state
    int g_allocateCallCount = 0;
    int g_releaseCallCount = 0;
    int g_allocateFailAfter = -1;
    int g_lockCallCount = 0;
    int g_unlockCallCount = 0;
    int32_t g_lockReturnStride = 0;
    int32_t g_unlockReturnFence = -1;

    // poll() state
    int g_pollCallCount = 0;
    int g_pollReturnValue = 1;  // Default: success, fd ready
    bool g_pollShouldSignal = true;

    // close() state
    int g_closeCallCount = 0;
    int g_lastClosedFd = -1;

    // clock_gettime() state
    int64_t g_currentTimeNs = 1000000000LL;  // Start at 1 second
}

// === AHardwareBuffer Implementation ===

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer) {
    g_allocateCallCount++;

    if (g_allocateFailAfter >= 0 && g_allocateCallCount > g_allocateFailAfter) {
        *outBuffer = nullptr;
        return -1;  // ENOMEM equivalent
    }

    auto* buffer = new AHardwareBuffer();
    buffer->desc = *desc;
    buffer->desc.stride = desc->width;  // Default stride = width
    buffer->mockMemory.resize(desc->width * desc->height * 4, 0);
    buffer->refCount = 1;
    buffer->lockCount = 0;
    buffer->isValid = true;

    *outBuffer = buffer;
    return 0;
}

void AHardwareBuffer_acquire(AHardwareBuffer* buffer) {
    if (buffer && buffer->isValid) {
        buffer->refCount++;
    }
}

void AHardwareBuffer_release(AHardwareBuffer* buffer) {
    g_releaseCallCount++;
    if (buffer) {
        buffer->refCount--;
        if (buffer->refCount <= 0) {
            buffer->isValid = false;
            delete buffer;
        }
    }
}

void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc) {
    if (buffer && buffer->isValid && outDesc) {
        *outDesc = buffer->desc;
    }
}

int AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence,
                         const void* rect, void** outVirtualAddress) {
    g_lockCallCount++;

    if (!buffer || !buffer->isValid) {
        return -1;  // EINVAL
    }

    buffer->lockCount++;
    *outVirtualAddress = buffer->mockMemory.data();
    return 0;
}

int AHardwareBuffer_unlock(AHardwareBuffer* buffer, int32_t* outFence) {
    g_unlockCallCount++;

    if (!buffer || !buffer->isValid) {
        return -1;
    }

    if (buffer->lockCount <= 0) {
        return -1;  // Not locked
    }

    buffer->lockCount--;

    if (outFence) {
        *outFence = g_unlockReturnFence;
    }

    return 0;
}

int AHardwareBuffer_lockAndGetInfo(AHardwareBuffer* buffer, uint64_t usage,
                                    int32_t fence, const void* rect,
                                    void** outVirtualAddress,
                                    int32_t* outBytesPerPixel,
                                    int32_t* outBytesPerStride) {
    int result = AHardwareBuffer_lock(buffer, usage, fence, rect, outVirtualAddress);

    if (result == 0) {
        if (outBytesPerPixel) {
            *outBytesPerPixel = 4;  // RGBA
        }
        if (outBytesPerStride) {
            *outBytesPerStride = (g_lockReturnStride > 0)
                ? g_lockReturnStride
                : static_cast<int32_t>(buffer->desc.stride * 4);
        }
    }

    return result;
}

// === poll() Implementation ===

int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    g_pollCallCount++;

    if (g_pollReturnValue < 0) {
        return -1;  // Error
    }

    if (g_pollShouldSignal && nfds > 0) {
        fds[0].revents = fds[0].events;  // Signal ready
    }

    return g_pollReturnValue;
}

// === close() Implementation ===

int close(int fd) {
    g_closeCallCount++;
    g_lastClosedFd = fd;
    return 0;
}

// === clock_gettime() Implementation ===

int clock_gettime(int clk_id, struct timespec* tp) {
    if (!tp) return -1;

    tp->tv_sec = g_currentTimeNs / 1000000000LL;
    tp->tv_nsec = g_currentTimeNs % 1000000000LL;

    return 0;
}

// === MockControl Implementation ===

namespace MockControl {
    void setAllocateFailAfter(int successCount) {
        g_allocateFailAfter = successCount;
    }

    void setLockReturnStride(int32_t stride) {
        g_lockReturnStride = stride;
    }

    void setUnlockReturnFence(int32_t fenceFd) {
        g_unlockReturnFence = fenceFd;
    }

    void setPollReturnValue(int value) {
        g_pollReturnValue = value;
    }

    void setPollShouldSignal(bool signal) {
        g_pollShouldSignal = signal;
    }

    void setCurrentTimeNs(int64_t timeNs) {
        g_currentTimeNs = timeNs;
    }

    void advanceTimeNs(int64_t deltaNs) {
        g_currentTimeNs += deltaNs;
    }

    int getCloseCallCount() {
        return g_closeCallCount;
    }

    int getLastClosedFd() {
        return g_lastClosedFd;
    }

    void reset() {
        g_allocateCallCount = 0;
        g_releaseCallCount = 0;
        g_allocateFailAfter = -1;
        g_lockCallCount = 0;
        g_unlockCallCount = 0;
        g_lockReturnStride = 0;
        g_unlockReturnFence = -1;
        g_pollCallCount = 0;
        g_pollReturnValue = 1;
        g_pollShouldSignal = true;
        g_closeCallCount = 0;
        g_lastClosedFd = -1;
        g_currentTimeNs = 1000000000LL;
    }

    int getAllocateCallCount() { return g_allocateCallCount; }
    int getReleaseCallCount() { return g_releaseCallCount; }
    int getLockCallCount() { return g_lockCallCount; }
    int getUnlockCallCount() { return g_unlockCallCount; }
    int getPollCallCount() { return g_pollCallCount; }
}

#endif // UVCCAMERA_TESTING
```

---

### Verification Checklist

After implementing guards, verify both build paths work:

```bash
# === Step 1: Verify Android Build Still Works ===
cd UVCCamera  # or wherever Android.mk lives
ndk-build clean
ndk-build -j8

# Expected: No errors, library builds successfully
# Verify: UVCCAMERA_TESTING is NOT defined
# Verify: Real Android headers are used

# === Step 2: Verify Host Test Build Works ===
cd lib/src/main/jni/test
rm -rf build
cmake -B build
cmake --build build

# Expected: No errors, test binary builds successfully
# Verify: UVCCAMERA_TESTING=1 is defined
# Verify: Mock headers are used

# === Step 3: Run Tests ===
./build/native_tests

# Expected: All tests pass

# === Step 4: Verify No Behavior Difference ===
# Install APK on device, run camera, verify it still works
```

---

### Guard Implementation Order

Execute in this sequence to minimize risk:

```
1. Create test/ directory with mock infrastructure (no production changes yet)
2. Add UVCCAMERA_TESTING guards to StreamTelemetry.h
   - Test: Host build compiles
   - Test: Android build still compiles
3. Add UVCCAMERA_TESTING guards to FrameBufferRing.h
   - Test: Host build compiles
   - Test: Android build still compiles
4. Add UVCCAMERA_TESTING guards to FrameBufferRing.cpp
   - Test: Host build compiles
   - Test: Android build still compiles
5. Update CMakeLists.txt to include FrameBufferRing.cpp
6. Write actual FrameBufferRing tests
7. Run full test suite
8. Run Android build + device test to verify no regressions
```

---

### Common Pitfalls

**Pitfall 1: Include Order Matters**

```cpp
// WRONG: Mock header after Android header
#include "FrameBufferRing.h"  // This might include <android/hardware_buffer.h>
#include "AndroidApiMocks.h"  // Too late! Real types already defined

// CORRECT: Mock header before anything else in .cpp files
#include "AndroidApiMocks.h"  // Define mocks first (only in test build)
#include "FrameBufferRing.h"  // Now sees mock types
```

**Pitfall 2: Transitive Dependencies**

If `FrameBufferRing.h` includes `StreamTelemetry.h` which includes Android headers, you need guards in the transitively included header too.

**Pitfall 3: Macro Redefinition**

```cpp
// If AndroidApiMocks.h defines LOGE, and production code also defines it:
// warning: 'LOGE' macro redefined

// Solution: Use #ifndef guards in production code:
#ifndef LOGE
    #define LOGE(...) __android_log_print(...)
#endif
```

**Pitfall 4: Type Size Differences**

`int` and `long` may have different sizes on 32-bit Android vs 64-bit host. Use fixed-width types (`int32_t`, `int64_t`) everywhere.

---

This guidance should enable the native agent to implement production code guards safely.
