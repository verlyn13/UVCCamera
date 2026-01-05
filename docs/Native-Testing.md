---
title: Native Layer Testing Infrastructure
category: testing
component: native
status: active
version: "1.0"
last_updated: 2024-12-29
tags: [testing, gtest, jni, contract, mocks]
priority: high
---

# Native Layer Testing Infrastructure

**Scope:** `nativecode/` module + Google Test + JNI contract verification
**Objective:** Establish Tier 2 native unit tests and define the authoritative JNI contract

---

### 1. Current State Assessment

Before beginning work, the agent must audit the existing native code:

```
REQUIRED AUDIT TASKS:
□ Inventory all .cpp/.h files in nativecode/
□ Identify which functions have JNI exports (JNIEXPORT)
□ Document all AHardwareBuffer API usage locations
□ List all Android-specific headers (#include <android/*.h>)
□ Identify atomic operations in StreamTelemetry
□ Map dependencies between native classes
```

**Expected findings based on specifications:**
- `FrameBufferRing.cpp/.h` - Ring buffer with MAILBOX policy
- `StreamTelemetry.h` - Atomic counters struct
- `TelemetryBridge.cpp/.h` - Library-agnostic interface
- `TelemetryJNI.cpp` - JNI exports for Kotlin
- `FrameSlotMetadata.h` - Per-slot metadata struct

---

### 2. Contract Definition (CRITICAL - Must Be Completed First)

The native agent is responsible for defining the **authoritative contract** that the Kotlin side must conform to. This contract lives in a header file that can be referenced by both native tests and documented for Kotlin.

**Create file: `nativecode/include/TelemetryContract.h`**

```cpp
// TelemetryContract.h
// AUTHORITATIVE CONTRACT - Kotlin layer must match these values
// Any changes here require coordinated update to Kotlin

#pragma once

namespace TelemetryContract {

// === Structural Constants ===
constexpr int FRAME_BUFFER_COUNT = 3;
constexpr int ERROR_HISTORY_SIZE = 8;

// === Enum Ordinal Contracts ===
// SlotState enum - Kotlin SlotState.entries must match these ordinals
namespace SlotStateOrdinal {
    constexpr int EMPTY = 0;
    constexpr int WRITING = 1;
    constexpr int READY = 2;
    constexpr int READING = 3;
    constexpr int COUNT = 4;
}

// FallbackLevel enum - Kotlin FallbackLevel.entries must match
namespace FallbackLevelOrdinal {
    constexpr int NORMAL = 0;
    constexpr int FPS_REDUCED = 1;
    constexpr int RES_REDUCED = 2;
    constexpr int ALT_SETTING = 3;
    constexpr int BULK_MODE = 4;
    constexpr int COUNT = 5;
}

// === Value Range Contracts ===
// These define valid ranges that both sides must respect
namespace Ranges {
    constexpr int MIN_WIDTH = 160;
    constexpr int MAX_WIDTH = 4096;
    constexpr int MIN_HEIGHT = 120;
    constexpr int MAX_HEIGHT = 2160;
    constexpr int MIN_FPS = 1;
    constexpr int MAX_FPS = 120;

    // Timing bounds (microseconds)
    constexpr int64_t MAX_DECODE_TIME_US = 100000;  // 100ms = failure
    constexpr int64_t MAX_RENDER_TIME_US = 50000;   // 50ms = severe lag

    // Fence file descriptor sentinel
    constexpr int NO_FENCE = -1;
}

// === JNI Method Signatures ===
// Document the expected signatures for JNI methods
// Format: (params)return_type using JNI type descriptors
namespace JniSignatures {
    // NativeTelemetry constructor
    // (long, long, long, int, int, boolean, int, long, long, long, long,
    //  long, long, long, long, [I, long, long, int, int, int, int, long,
    //  String, [ErrorEntry)V
    constexpr const char* NATIVE_TELEMETRY_CTOR =
        "(JJJIIZIIJJJJJJJJ[IJJIIIIJLjava/lang/String;[Lcom/scopecam/camera/NativeErrorEntry;)V";

    // FrameData constructor
    // (HardwareBuffer, long, long, int, int, int)V
    constexpr const char* FRAME_DATA_CTOR =
        "(Landroid/hardware/HardwareBuffer;JJIII)V";
}

// === Versioning ===
// Increment when contract changes require Kotlin updates
constexpr int CONTRACT_VERSION = 1;

} // namespace TelemetryContract
```

**Create file: `nativecode/include/ContractVerification.h`**

```cpp
// ContractVerification.h
// Static assertions that verify native code matches contract

#pragma once

#include "TelemetryContract.h"
#include "FrameBufferRing.h"
#include "StreamTelemetry.h"

namespace ContractVerification {

// Verify FRAME_BUFFER_COUNT matches
static_assert(
    FRAME_BUFFER_COUNT == TelemetryContract::FRAME_BUFFER_COUNT,
    "FRAME_BUFFER_COUNT mismatch with contract"
);

// Verify ERROR_HISTORY_SIZE matches
static_assert(
    ERROR_HISTORY_SIZE == TelemetryContract::ERROR_HISTORY_SIZE,
    "ERROR_HISTORY_SIZE mismatch with contract"
);

// Verify SlotState enum values (if using enum class)
// Note: Adjust based on actual enum definition
static_assert(
    static_cast<int>(SlotState::EMPTY) == TelemetryContract::SlotStateOrdinal::EMPTY,
    "SlotState::EMPTY ordinal mismatch"
);
static_assert(
    static_cast<int>(SlotState::READING) == TelemetryContract::SlotStateOrdinal::READING,
    "SlotState::READING ordinal mismatch"
);

} // namespace ContractVerification
```

---

### 3. Google Test Infrastructure Setup

**Modify: `nativecode/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.22)
project(scopecam-native)

# === Production Build (Android) ===
if(ANDROID)
    # Existing Android build configuration
    add_library(scopecam-native SHARED
        src/FrameBufferRing.cpp
        src/TelemetryBridge.cpp
        src/TelemetryJNI.cpp
    )

    find_library(log-lib log)
    find_library(nativewindow-lib nativewindow)
    find_library(android-lib android)
    find_library(sync-lib sync)

    target_link_libraries(scopecam-native
        ${log-lib}
        ${nativewindow-lib}
        ${android-lib}
        ${sync-lib}
    )

    target_include_directories(scopecam-native PUBLIC include)

# === Host Test Build (Linux/macOS) ===
else()
    message(STATUS "Building native tests for host")

    # Enable testing
    enable_testing()

    # Fetch Google Test
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)

    # Define test-only compilation flag
    add_compile_definitions(SCOPECAM_TESTING=1)

    # Test executable
    add_executable(native_tests
        # Test files
        test/ContractTest.cpp
        test/StreamTelemetryTest.cpp
        test/FrameBufferRingLogicTest.cpp
        test/TelemetryBridgeTest.cpp
        test/FenceSynchronizationTest.cpp

        # Source files under test (testable portions)
        src/TelemetryBridge.cpp

        # Test mocks and utilities
        test/mocks/AndroidApiMocks.cpp
        test/TestHelpers.cpp
    )

    target_include_directories(native_tests PRIVATE
        include
        test/mocks
    )

    target_link_libraries(native_tests
        GTest::gtest_main
        GTest::gmock
    )

    # Discover tests for CTest
    include(GoogleTest)
    gtest_discover_tests(native_tests)
endif()
```

---

### 4. Mock Infrastructure for Android APIs

**Create file: `nativecode/test/mocks/AndroidApiMocks.h`**

```cpp
// AndroidApiMocks.h
// Mock implementations of Android APIs for host testing

#pragma once

#ifdef SCOPECAM_TESTING

#include <cstdint>
#include <vector>
#include <functional>

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

// Mock buffer - just holds metadata
struct AHardwareBuffer {
    AHardwareBuffer_Desc desc;
    std::vector<uint8_t> mockMemory;
    int lockCount = 0;
    bool isValid = true;
};

// Mock implementations
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer);
void AHardwareBuffer_release(AHardwareBuffer* buffer);
void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc);
int AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence,
                         const void* rect, void** outVirtualAddress);
int AHardwareBuffer_unlock(AHardwareBuffer* buffer, int32_t* outFence);

// API 29+ mock
int AHardwareBuffer_lockAndGetInfo(AHardwareBuffer* buffer, uint64_t usage,
                                    int32_t fence, const void* rect,
                                    void** outVirtualAddress,
                                    int32_t* outBytesPerPixel,
                                    int32_t* outBytesPerStride);

// === sync_wait Mock ===
int sync_wait(int fd, int timeout_ms);

// === Test Control ===
namespace MockControl {
    // Configure mock behavior for specific tests
    void setAllocateFailAfter(int successCount);
    void setLockReturnStride(int32_t stride);
    void setUnlockReturnFence(int32_t fenceFd);
    void setSyncWaitBehavior(std::function<int(int, int)> behavior);
    void reset();

    // Inspection
    int getAllocateCallCount();
    int getLockCallCount();
    int getUnlockCallCount();
}

#endif // SCOPECAM_TESTING
```

**Create file: `nativecode/test/mocks/AndroidApiMocks.cpp`**

```cpp
// AndroidApiMocks.cpp

#ifdef SCOPECAM_TESTING

#include "AndroidApiMocks.h"
#include <cstring>

namespace {
    int g_allocateCallCount = 0;
    int g_allocateFailAfter = -1;
    int g_lockCallCount = 0;
    int32_t g_lockReturnStride = 0;
    int g_unlockCallCount = 0;
    int32_t g_unlockReturnFence = -1;
    std::function<int(int, int)> g_syncWaitBehavior = nullptr;
}

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer) {
    g_allocateCallCount++;

    if (g_allocateFailAfter >= 0 && g_allocateCallCount > g_allocateFailAfter) {
        *outBuffer = nullptr;
        return -1;  // Simulate allocation failure
    }

    auto* buffer = new AHardwareBuffer();
    buffer->desc = *desc;
    buffer->desc.stride = desc->width;  // Default stride = width

    // Allocate mock memory
    size_t size = desc->width * desc->height * 4;  // Assume RGBA
    buffer->mockMemory.resize(size, 0);

    *outBuffer = buffer;
    return 0;
}

void AHardwareBuffer_release(AHardwareBuffer* buffer) {
    if (buffer) {
        buffer->isValid = false;
        delete buffer;
    }
}

void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc) {
    if (buffer && outDesc) {
        *outDesc = buffer->desc;
    }
}

int AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence,
                         const void* rect, void** outVirtualAddress) {
    g_lockCallCount++;

    if (!buffer || !buffer->isValid) return -1;

    buffer->lockCount++;
    *outVirtualAddress = buffer->mockMemory.data();
    return 0;
}

int AHardwareBuffer_unlock(AHardwareBuffer* buffer, int32_t* outFence) {
    g_unlockCallCount++;

    if (!buffer || !buffer->isValid) return -1;
    if (buffer->lockCount <= 0) return -1;

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
        if (outBytesPerPixel) *outBytesPerPixel = 4;  // RGBA
        if (outBytesPerStride) {
            *outBytesPerStride = (g_lockReturnStride > 0)
                ? g_lockReturnStride
                : buffer->desc.stride * 4;
        }
    }
    return result;
}

int sync_wait(int fd, int timeout_ms) {
    if (g_syncWaitBehavior) {
        return g_syncWaitBehavior(fd, timeout_ms);
    }
    // Default: immediate success
    return 0;
}

namespace MockControl {
    void setAllocateFailAfter(int successCount) { g_allocateFailAfter = successCount; }
    void setLockReturnStride(int32_t stride) { g_lockReturnStride = stride; }
    void setUnlockReturnFence(int32_t fenceFd) { g_unlockReturnFence = fenceFd; }
    void setSyncWaitBehavior(std::function<int(int, int)> behavior) { g_syncWaitBehavior = behavior; }

    void reset() {
        g_allocateCallCount = 0;
        g_allocateFailAfter = -1;
        g_lockCallCount = 0;
        g_lockReturnStride = 0;
        g_unlockCallCount = 0;
        g_unlockReturnFence = -1;
        g_syncWaitBehavior = nullptr;
    }

    int getAllocateCallCount() { return g_allocateCallCount; }
    int getLockCallCount() { return g_lockCallCount; }
    int getUnlockCallCount() { return g_unlockCallCount; }
}

#endif // SCOPECAM_TESTING
```

---

### 5. Test Files to Create

**Test 1: `nativecode/test/ContractTest.cpp`**

```cpp
// ContractTest.cpp
// Verifies that native code conforms to the documented contract

#include <gtest/gtest.h>
#include "TelemetryContract.h"
#include "ContractVerification.h"
#include "StreamTelemetry.h"

using namespace TelemetryContract;

class ContractTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

// === Structural Contract Tests ===

TEST_F(ContractTest, FrameBufferCountMatchesContract) {
    EXPECT_EQ(FRAME_BUFFER_COUNT, 3);
}

TEST_F(ContractTest, ErrorHistorySizeMatchesContract) {
    EXPECT_EQ(ERROR_HISTORY_SIZE, 8);
}

// === SlotState Ordinal Tests ===

TEST_F(ContractTest, SlotStateEmptyOrdinalIsZero) {
    EXPECT_EQ(static_cast<int>(SlotState::EMPTY), SlotStateOrdinal::EMPTY);
}

TEST_F(ContractTest, SlotStateWritingOrdinalIsOne) {
    EXPECT_EQ(static_cast<int>(SlotState::WRITING), SlotStateOrdinal::WRITING);
}

TEST_F(ContractTest, SlotStateReadyOrdinalIsTwo) {
    EXPECT_EQ(static_cast<int>(SlotState::READY), SlotStateOrdinal::READY);
}

TEST_F(ContractTest, SlotStateReadingOrdinalIsThree) {
    EXPECT_EQ(static_cast<int>(SlotState::READING), SlotStateOrdinal::READING);
}

TEST_F(ContractTest, SlotStateCountIsFour) {
    EXPECT_EQ(SlotStateOrdinal::COUNT, 4);
}

// === FallbackLevel Ordinal Tests ===

TEST_F(ContractTest, FallbackLevelNormalOrdinalIsZero) {
    EXPECT_EQ(FallbackLevelOrdinal::NORMAL, 0);
}

TEST_F(ContractTest, FallbackLevelBulkModeOrdinalIsFour) {
    EXPECT_EQ(FallbackLevelOrdinal::BULK_MODE, 4);
}

TEST_F(ContractTest, FallbackLevelCountIsFive) {
    EXPECT_EQ(FallbackLevelOrdinal::COUNT, 5);
}

// === Range Contract Tests ===

TEST_F(ContractTest, ResolutionRangesAreReasonable) {
    EXPECT_LE(Ranges::MIN_WIDTH, 640);  // At least VGA
    EXPECT_GE(Ranges::MAX_WIDTH, 1920); // At least 1080p
    EXPECT_LE(Ranges::MIN_HEIGHT, 480);
    EXPECT_GE(Ranges::MAX_HEIGHT, 1080);
}

TEST_F(ContractTest, FpsRangesAreReasonable) {
    EXPECT_GE(Ranges::MIN_FPS, 1);
    EXPECT_LE(Ranges::MAX_FPS, 120);
}

TEST_F(ContractTest, NoFenceSentinelIsNegativeOne) {
    EXPECT_EQ(Ranges::NO_FENCE, -1);
}

// === Version Test ===

TEST_F(ContractTest, ContractVersionIsPositive) {
    EXPECT_GT(CONTRACT_VERSION, 0);
}
```

**Test 2: `nativecode/test/StreamTelemetryTest.cpp`**

```cpp
// StreamTelemetryTest.cpp
// Unit tests for StreamTelemetry atomic operations and EMA calculations

#include <gtest/gtest.h>
#include "StreamTelemetry.h"
#include <thread>
#include <vector>

class StreamTelemetryTest : public ::testing::Test {
protected:
    StreamTelemetry telemetry;

    void SetUp() override {
        // Fresh telemetry for each test
    }
};

// === Basic Counter Tests ===

TEST_F(StreamTelemetryTest, InitialCountersAreZero) {
    EXPECT_EQ(telemetry.usbPacketsReceived.load(), 0);
    EXPECT_EQ(telemetry.framesReceived.load(), 0);
    EXPECT_EQ(telemetry.framesDropped.load(), 0);
}

TEST_F(StreamTelemetryTest, IncrementCountersAreThreadSafe) {
    constexpr int THREAD_COUNT = 4;
    constexpr int INCREMENTS_PER_THREAD = 10000;

    std::vector<std::thread> threads;
    for (int i = 0; i < THREAD_COUNT; i++) {
        threads.emplace_back([this]() {
            for (int j = 0; j < INCREMENTS_PER_THREAD; j++) {
                telemetry.framesReceived.fetch_add(1);
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(telemetry.framesReceived.load(),
              THREAD_COUNT * INCREMENTS_PER_THREAD);
}

// === EMA Timing Tests ===

TEST_F(StreamTelemetryTest, DecodeTimeEmaConverges) {
    // Simulate consistent decode times
    constexpr int64_t CONSISTENT_TIME = 1000;  // 1ms

    for (int i = 0; i < 100; i++) {
        telemetry.updateDecodeTime(CONSISTENT_TIME);
    }

    // EMA should converge close to consistent value
    int64_t avg = telemetry.avgDecodeTimeUs.load();
    EXPECT_NEAR(avg, CONSISTENT_TIME, CONSISTENT_TIME * 0.1);  // Within 10%
}

TEST_F(StreamTelemetryTest, DecodeTimeMinMaxTracking) {
    telemetry.updateDecodeTime(500);
    telemetry.updateDecodeTime(1500);
    telemetry.updateDecodeTime(1000);

    EXPECT_EQ(telemetry.minDecodeTimeUs.load(), 500);
    EXPECT_EQ(telemetry.maxDecodeTimeUs.load(), 1500);
}

TEST_F(StreamTelemetryTest, LastDecodeTimeIsUpdatedImmediately) {
    telemetry.updateDecodeTime(12345);
    EXPECT_EQ(telemetry.lastDecodeTimeUs.load(), 12345);
}

// === Error Recording Tests ===

TEST_F(StreamTelemetryTest, ErrorHistoryCircularBuffer) {
    // Fill history beyond capacity
    for (int i = 0; i < ERROR_HISTORY_SIZE + 5; i++) {
        telemetry.recordError(i, "test");
    }

    // Should have wrapped around
    int idx = telemetry.errorHistoryIndex.load();
    EXPECT_EQ(idx % ERROR_HISTORY_SIZE, 5);
}

TEST_F(StreamTelemetryTest, ErrorEntrySourceTruncation) {
    telemetry.recordError(1, "this_is_a_very_long_source_name_that_exceeds_limit");

    int idx = (telemetry.errorHistoryIndex.load() - 1) % ERROR_HISTORY_SIZE;
    // Source should be truncated to 15 chars + null
    EXPECT_LE(strlen(telemetry.errorHistory[idx].source), 15);
}

// === Slot State Tests ===

TEST_F(StreamTelemetryTest, SlotStatesInitializedToEmpty) {
    for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
        EXPECT_EQ(telemetry.slotStates[i].load(), SlotState::EMPTY);
    }
}

TEST_F(StreamTelemetryTest, SlotStateTransitions) {
    telemetry.slotStates[0].store(SlotState::WRITING);
    EXPECT_EQ(telemetry.slotStates[0].load(), SlotState::WRITING);

    telemetry.slotStates[0].store(SlotState::READY);
    EXPECT_EQ(telemetry.slotStates[0].load(), SlotState::READY);
}

// === Contention Tracking Tests ===

TEST_F(StreamTelemetryTest, ProducerStallIncrement) {
    uint64_t before = telemetry.producerStalls.load();
    telemetry.producerStalls.fetch_add(1);
    EXPECT_EQ(telemetry.producerStalls.load(), before + 1);
}

TEST_F(StreamTelemetryTest, ConsumerStarveIncrement) {
    uint64_t before = telemetry.consumerStarves.load();
    telemetry.consumerStarves.fetch_add(1);
    EXPECT_EQ(telemetry.consumerStarves.load(), before + 1);
}
```

**Test 3: `nativecode/test/FrameBufferRingLogicTest.cpp`**

```cpp
// FrameBufferRingLogicTest.cpp
// Tests for ring buffer MAILBOX policy and index rotation

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "FrameBufferRing.h"
#include "mocks/AndroidApiMocks.h"

class FrameBufferRingTest : public ::testing::Test {
protected:
    FrameBufferRing ring;

    void SetUp() override {
        MockControl::reset();
    }

    void TearDown() override {
        ring.destroy();
        MockControl::reset();
    }
};

// === Allocation Tests ===

TEST_F(FrameBufferRingTest, AllocateSucceeds) {
    int result = ring.allocate(1920, 1080, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(ring.isAllocated());
}

TEST_F(FrameBufferRingTest, AllocateSetsCorrectDimensions) {
    ring.allocate(1280, 720, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    EXPECT_EQ(ring.getWidth(), 1280);
    EXPECT_EQ(ring.getHeight(), 720);
}

TEST_F(FrameBufferRingTest, AllocateFailureHandled) {
    MockControl::setAllocateFailAfter(1);  // Fail on second buffer

    int result = ring.allocate(1920, 1080, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    EXPECT_NE(result, 0);
    EXPECT_FALSE(ring.isAllocated());
}

TEST_F(FrameBufferRingTest, DoubleAllocateDestroysFirst) {
    ring.allocate(1920, 1080, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    ring.allocate(1280, 720, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    // Second allocation should work
    EXPECT_TRUE(ring.isAllocated());
    EXPECT_EQ(ring.getWidth(), 1280);
}

// === MAILBOX Policy Tests ===

TEST_F(FrameBufferRingTest, MailboxPolicyProducerAlwaysAdvances) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    // Produce 3 frames without consuming
    for (int i = 0; i < 3; i++) {
        int32_t stride;
        void* ptr = ring.lockWriteBuffer(&stride);
        ASSERT_NE(ptr, nullptr);
        ring.unlockWriteBuffer();
    }

    // Producer should have rotated through all slots
    EXPECT_EQ(MockControl::getLockCallCount(), 3);
}

TEST_F(FrameBufferRingTest, MailboxPolicyConsumerGetsLatest) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    // Produce 5 frames
    for (int i = 0; i < 5; i++) {
        int32_t stride;
        void* ptr = ring.lockWriteBuffer(&stride);
        ring.unlockWriteBuffer();
    }

    // Consumer should get the most recent frame
    FrameSlotMetadata meta;
    AHardwareBuffer* buffer = ring.acquireReadBuffer(&meta);

    ASSERT_NE(buffer, nullptr);
    // Frame number should be 4 (0-indexed, 5th frame)
    EXPECT_EQ(meta.frameNumber, 4);
}

TEST_F(FrameBufferRingTest, AcquireBeforeProduceReturnsNull) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    FrameSlotMetadata meta;
    AHardwareBuffer* buffer = ring.acquireReadBuffer(&meta);

    EXPECT_EQ(buffer, nullptr);
}

// === Stride Handling Tests ===

TEST_F(FrameBufferRingTest, StrideIsReturnedFromLock) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    MockControl::setLockReturnStride(2560);  // 640 * 4

    int32_t stride = 0;
    void* ptr = ring.lockWriteBuffer(&stride);

    EXPECT_GT(stride, 0);
}

TEST_F(FrameBufferRingTest, MetadataContainsStride) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    MockControl::setLockReturnStride(2560);

    int32_t stride;
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();

    FrameSlotMetadata meta;
    ring.acquireReadBuffer(&meta);

    EXPECT_EQ(meta.strideBytes, 2560);
}

// === Fence Handling Tests ===

TEST_F(FrameBufferRingTest, FenceFromUnlockStoredInMetadata) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    MockControl::setUnlockReturnFence(42);

    int32_t stride;
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();

    FrameSlotMetadata meta;
    ring.acquireReadBuffer(&meta);

    EXPECT_EQ(meta.releaseFenceFd, 42);
}

TEST_F(FrameBufferRingTest, NoFenceWhenUnlockReturnsNegativeOne) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    MockControl::setUnlockReturnFence(-1);

    int32_t stride;
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();

    FrameSlotMetadata meta;
    ring.acquireReadBuffer(&meta);

    EXPECT_EQ(meta.releaseFenceFd, TelemetryContract::Ranges::NO_FENCE);
}

// === Telemetry Integration Tests ===

TEST_F(FrameBufferRingTest, FramesReceivedIncrementsOnUnlock) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    auto* telemetry = ring.getTelemetry();
    uint64_t before = telemetry->framesReceived.load();

    int32_t stride;
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();

    EXPECT_EQ(telemetry->framesReceived.load(), before + 1);
}

TEST_F(FrameBufferRingTest, FrameTimestampRecordedOnUnlock) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    int32_t stride;
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();

    FrameSlotMetadata meta;
    ring.acquireReadBuffer(&meta);

    EXPECT_GT(meta.timestampNs, 0);
}
```

**Test 4: `nativecode/test/TelemetryBridgeTest.cpp`**

```cpp
// TelemetryBridgeTest.cpp
// Tests for library-agnostic telemetry bridge

#include <gtest/gtest.h>
#include "TelemetryBridge.h"

class TelemetryBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset global telemetry state
        g_telemetry = StreamTelemetry{};
    }
};

TEST_F(TelemetryBridgeTest, OnUsbPacketReceivedIncrements) {
    uint64_t before = g_telemetry.usbPacketsReceived.load();
    TelemetryBridge::onUsbPacketReceived();
    EXPECT_EQ(g_telemetry.usbPacketsReceived.load(), before + 1);
}

TEST_F(TelemetryBridgeTest, OnUsbOverflowIncrements) {
    uint64_t before = g_telemetry.usbOverflowErrors.load();
    TelemetryBridge::onUsbOverflow();
    EXPECT_EQ(g_telemetry.usbOverflowErrors.load(), before + 1);
}

TEST_F(TelemetryBridgeTest, OnFrameDecodedUpdatesCountersOnSuccess) {
    TelemetryBridge::onFrameDecoded(1000, true);

    EXPECT_EQ(g_telemetry.framesReceived.load(), 1);
    EXPECT_EQ(g_telemetry.framesCorrupted.load(), 0);
}

TEST_F(TelemetryBridgeTest, OnFrameDecodedUpdatesCountersOnFailure) {
    TelemetryBridge::onFrameDecoded(1000, false);

    EXPECT_EQ(g_telemetry.framesReceived.load(), 0);
    EXPECT_EQ(g_telemetry.framesCorrupted.load(), 1);
}

TEST_F(TelemetryBridgeTest, OnStreamNegotiatedSetsParameters) {
    TelemetryBridge::onStreamNegotiated(1920, 1080, 30, 1, true, "MJPEG");

    EXPECT_EQ(g_telemetry.negotiatedWidth, 1920);
    EXPECT_EQ(g_telemetry.negotiatedHeight, 1080);
    EXPECT_EQ(g_telemetry.negotiatedFps, 30);
    EXPECT_EQ(g_telemetry.usbAltSetting.load(), 1);
    EXPECT_TRUE(g_telemetry.usbIsIsochronous.load());
}

TEST_F(TelemetryBridgeTest, OnFallbackLevelChangedSetsLevelAndReason) {
    TelemetryBridge::onFallbackLevelChanged(2, "USB overflow burst");

    EXPECT_EQ(g_telemetry.fallbackLevel.load(), 2);
    EXPECT_STREQ(g_telemetry.fallbackReason, "USB overflow burst");
}

TEST_F(TelemetryBridgeTest, GetSnapshotCopiesAllFields) {
    g_telemetry.framesReceived.store(100);
    g_telemetry.framesDropped.store(5);
    g_telemetry.negotiatedWidth = 1280;

    auto snapshot = TelemetryBridge::getSnapshot();

    EXPECT_EQ(snapshot.framesReceived, 100);
    EXPECT_EQ(snapshot.framesDropped, 5);
    EXPECT_EQ(snapshot.negotiatedWidth, 1280);
}

TEST_F(TelemetryBridgeTest, OnNativeErrorRecordsToHistory) {
    TelemetryBridge::onNativeError(-5, "libusb");

    auto snapshot = TelemetryBridge::getSnapshot();
    EXPECT_GT(snapshot.errorHistoryCount, 0);
    EXPECT_EQ(snapshot.errorHistory[0].code, -5);
    EXPECT_STREQ(snapshot.errorHistory[0].source, "libusb");
}
```

---

### 6. Deliverables Checklist

```
NATIVE AGENT DELIVERABLES:

□ Phase N1: Contract Definition (BLOCKING - must complete first)
  □ TelemetryContract.h created with all constants
  □ ContractVerification.h created with static_asserts
  □ Contract document exported for Kotlin agent (see Section 7)

□ Phase N2: Test Infrastructure
  □ CMakeLists.txt updated with host test configuration
  □ AndroidApiMocks.h/.cpp created
  □ Verify build: cmake -B build && cmake --build build

□ Phase N3: Contract Tests
  □ ContractTest.cpp created and passing
  □ All enum ordinals verified
  □ All range constants verified

□ Phase N4: Core Logic Tests
  □ StreamTelemetryTest.cpp created and passing
  □ FrameBufferRingLogicTest.cpp created and passing
  □ TelemetryBridgeTest.cpp created and passing

□ Phase N5: Integration Points
  □ JNI signature validation test
  □ Document any contract changes needed

□ Phase N6: CI Integration
  □ Native tests run in GitHub Actions
  □ Test results reported
```

---

### 7. Contract Export for Kotlin Agent

After completing the contract definition, the native agent must produce this document for the Kotlin agent:

```markdown
# JNI Contract Specification v1
Generated from: nativecode/include/TelemetryContract.h

## Structural Constants
- FRAME_BUFFER_COUNT = 3
- ERROR_HISTORY_SIZE = 8

## SlotState Enum (must match ordinals)
| Kotlin Enum Value | Expected Ordinal |
|-------------------|------------------|
| EMPTY             | 0                |
| WRITING           | 1                |
| READY             | 2                |
| READING           | 3                |

## FallbackLevel Enum (must match ordinals)
| Kotlin Enum Value | Expected Ordinal |
|-------------------|------------------|
| NORMAL            | 0                |
| FPS_REDUCED       | 1                |
| RES_REDUCED       | 2                |
| ALT_SETTING       | 3                |
| BULK_MODE         | 4                |

## Value Ranges
- Resolution: 160-4096 x 120-2160
- FPS: 1-120
- NO_FENCE sentinel: -1

## NativeTelemetry JNI Constructor Signature
```
(JJJIIZIIJJJJJJJJ[IJJIIIIJLjava/lang/String;[Lcom/scopecam/camera/NativeErrorEntry;)V
```

Parameters in order:
1. usbPacketsReceived: Long
2. usbOverflowErrors: Long
3. usbTimeoutErrors: Long
4. usbEndpointAddress: Int
5. usbAltSetting: Int
6. usbIsIsochronous: Boolean
7. usbMaxPacketSize: Int
8. framesReceived: Long
9. framesDropped: Long
10. framesCorrupted: Long
11. framesRendered: Long
12. avgDecodeTimeUs: Long
13. avgRenderTimeUs: Long
14. bufferLockWaitTimeNs: Long
15. fenceWaitTimeNs: Long
16. slotStates: IntArray (size 3)
17. producerStalls: Long
18. consumerStarves: Long
19. negotiatedWidth: Int
20. negotiatedHeight: Int
21. negotiatedFps: Int
22. fallbackLevel: Int
23. fallbackSinceNs: Long
24. fallbackReason: String?
25. errorHistory: Array<NativeErrorEntry>
```

---


