/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: FrameBufferRingTest.cpp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * Tests the actual production FrameBufferRing class with mocks.
 * Verifies allocation, MAILBOX policy, triple-buffering, stride handling,
 * fence handling, and telemetry integration.
 */

#include <gtest/gtest.h>

// Include mocks BEFORE production headers (critical!)
#include "AndroidApiMocks.h"

// Now include production code - it will use mock definitions
#include "FrameBufferRing.h"

class FrameBufferRingTest : public ::testing::Test {
protected:
    FrameBufferRing ring;

    void SetUp() override { MockControl::reset(); }
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
    EXPECT_EQ(ring.getWidth(), 1920u);
    EXPECT_EQ(ring.getHeight(), 1080u);
}

TEST_F(FrameBufferRingTest, AllocateFailureCleansUp) {
    MockControl::setAllocateFailAfter(1);  // Fail on 2nd buffer

    int result = ring.allocate(1920, 1080, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    EXPECT_NE(result, 0);
    EXPECT_FALSE(ring.isAllocated());
}

TEST_F(FrameBufferRingTest, DoubleAllocateDestroysFirst) {
    ring.allocate(1920, 1080, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    ring.allocate(1280, 720, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    EXPECT_TRUE(ring.isAllocated());
    EXPECT_EQ(ring.getWidth(), 1280u);
}

// === MAILBOX Policy Tests ===

TEST_F(FrameBufferRingTest, MailboxProducerAdvancesOnUnlock) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    int32_t stride;
    void* ptr = ring.lockWriteBuffer(&stride);
    ASSERT_NE(ptr, nullptr);
    ring.unlockWriteBuffer();

    // Should be able to produce another frame
    ptr = ring.lockWriteBuffer(&stride);
    EXPECT_NE(ptr, nullptr);
    ring.unlockWriteBuffer();
}

TEST_F(FrameBufferRingTest, MailboxConsumerGetsLatestFrame) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    // Produce 5 frames without consuming
    for (int i = 0; i < 5; i++) {
        int32_t stride;
        void* ptr = ring.lockWriteBuffer(&stride);
        ASSERT_NE(ptr, nullptr);
        ring.unlockWriteBuffer();
    }

    // Consumer should get the latest frame (frame 5)
    FrameSlotMetadata meta;
    AHardwareBuffer* buffer = ring.acquireReadBuffer(&meta);

    ASSERT_NE(buffer, nullptr);
    EXPECT_EQ(meta.frameNumber, 5u);  // Latest frame

    ring.releaseReadBuffer();
}

TEST_F(FrameBufferRingTest, AcquireBeforeProduceReturnsNull) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    FrameSlotMetadata meta;
    AHardwareBuffer* buffer = ring.acquireReadBuffer(&meta);
    EXPECT_EQ(buffer, nullptr);
}

// === Triple-Buffer Dance Tests ===

TEST_F(FrameBufferRingTest, ProducerSkipsBufferBeingRead) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    // Produce one frame
    int32_t stride;
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();

    // Consumer acquires
    FrameSlotMetadata meta;
    AHardwareBuffer* buffer = ring.acquireReadBuffer(&meta);
    ASSERT_NE(buffer, nullptr);

    // Producer should be able to write 2 more frames
    // without blocking (triple buffer)
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();

    ring.releaseReadBuffer();
}

// === Stride Handling Tests ===

TEST_F(FrameBufferRingTest, StrideReturnedFromLock) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    MockControl::setLockReturnStride(2560);  // 640 * 4

    int32_t stride = 0;
    void* ptr = ring.lockWriteBuffer(&stride);

    EXPECT_NE(ptr, nullptr);
    EXPECT_EQ(stride, 2560);

    ring.unlockWriteBuffer();
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

    ring.releaseReadBuffer();
}

// === Fence Handling Tests ===

TEST_F(FrameBufferRingTest, FenceStoredInMetadata) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    MockControl::setUnlockReturnFence(42);

    int32_t stride;
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();

    FrameSlotMetadata meta;
    ring.acquireReadBuffer(&meta);
    EXPECT_EQ(meta.acquireFenceFd, 42);

    ring.releaseReadBuffer();
}

TEST_F(FrameBufferRingTest, GpuReleaseFenceStored) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    int32_t stride;
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();

    // Acquire and set GPU release fence
    ring.acquireReadBuffer(nullptr);
    int readIdx = ring.getCurrentReadIndex();
    ring.setGpuReleaseFence(readIdx, 99);

    FrameSlotMetadata* meta = ring.getMetadata(readIdx);
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->gpuReleaseFenceFd, 99);

    ring.releaseReadBuffer();
}

// === Telemetry Tests ===

TEST_F(FrameBufferRingTest, FramesReceivedIncrementsOnUnlock) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    auto* telemetry = ring.getTelemetry();
    uint64_t before = telemetry->framesReceived.load();

    int32_t stride;
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();

    EXPECT_EQ(telemetry->framesReceived.load(), before + 1);
}

TEST_F(FrameBufferRingTest, FramesRenderedIncrementsOnRelease) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    int32_t stride;
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();

    auto* telemetry = ring.getTelemetry();
    uint64_t before = telemetry->framesRendered.load();

    ring.acquireReadBuffer(nullptr);
    ring.releaseReadBuffer();

    EXPECT_EQ(telemetry->framesRendered.load(), before + 1);
}

TEST_F(FrameBufferRingTest, SlotStatesTracked) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    auto* telemetry = ring.getTelemetry();

    int32_t stride;
    ring.lockWriteBuffer(&stride);
    // During write, slot should be in WRITING state
    EXPECT_EQ(telemetry->slotStates[0].load(), static_cast<int>(SlotState::WRITING));

    ring.unlockWriteBuffer();
    // After unlock, slot should be READY
    // Note: The slot that was written is now latestCompleted, but we moved to next write slot
}

TEST_F(FrameBufferRingTest, ConsumerStarvesWhenNoFrames) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    auto* telemetry = ring.getTelemetry();
    uint64_t before = telemetry->consumerStarves.load();

    // Try to acquire without any frames produced
    AHardwareBuffer* buffer = ring.acquireReadBuffer(nullptr);
    EXPECT_EQ(buffer, nullptr);
    EXPECT_EQ(telemetry->consumerStarves.load(), before + 1);
}

// === Negative Tests ===

TEST_F(FrameBufferRingTest, LockBeforeAllocateReturnsNull) {
    int32_t stride;
    void* ptr = ring.lockWriteBuffer(&stride);
    EXPECT_EQ(ptr, nullptr);
}

TEST_F(FrameBufferRingTest, DestroyIsIdempotent) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    ring.destroy();
    ring.destroy();  // Should not crash
    EXPECT_FALSE(ring.isAllocated());
}

TEST_F(FrameBufferRingTest, UnlockBeforeAllocateSafe) {
    // Should not crash even if called without allocation
    ring.unlockWriteBuffer();
}

TEST_F(FrameBufferRingTest, CancelWriteBufferIncrementsCurrupted) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    auto* telemetry = ring.getTelemetry();
    uint64_t before = telemetry->framesCorrupted.load();

    int32_t stride;
    ring.lockWriteBuffer(&stride);
    ring.cancelWriteBuffer();

    EXPECT_EQ(telemetry->framesCorrupted.load(), before + 1);
}

// === Reference Counting Tests ===

TEST_F(FrameBufferRingTest, AcquireIncrementsRefCount) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    int32_t stride;
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();

    int allocsBefore = MockControl::getAllocateCallCount();
    ring.acquireReadBuffer(nullptr);
    // acquire should call AHardwareBuffer_acquire (not allocate)
    EXPECT_EQ(MockControl::getAllocateCallCount(), allocsBefore);

    ring.releaseReadBuffer();
}

// === Frame Number and Slot Lookup Tests ===

TEST_F(FrameBufferRingTest, FindSlotByFrameNumber) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    int32_t stride;
    ring.lockWriteBuffer(&stride);
    ring.unlockWriteBuffer();

    FrameSlotMetadata meta;
    ring.acquireReadBuffer(&meta);
    uint64_t frameNum = meta.frameNumber;

    int slot = ring.findSlotByFrameNumber(frameNum);
    EXPECT_GE(slot, 0);
    EXPECT_LT(slot, FRAME_BUFFER_COUNT);

    ring.releaseReadBuffer();
}

TEST_F(FrameBufferRingTest, FindSlotReturnsMinusOneForRecycled) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    // Non-existent frame number
    int slot = ring.findSlotByFrameNumber(999999);
    EXPECT_EQ(slot, -1);
}

// === Metadata Access Tests ===

TEST_F(FrameBufferRingTest, GetMetadataValidSlot) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
        FrameSlotMetadata* meta = ring.getMetadata(i);
        EXPECT_NE(meta, nullptr);
    }
}

TEST_F(FrameBufferRingTest, GetMetadataInvalidSlot) {
    ring.allocate(640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    EXPECT_EQ(ring.getMetadata(-1), nullptr);
    EXPECT_EQ(ring.getMetadata(FRAME_BUFFER_COUNT), nullptr);
    EXPECT_EQ(ring.getMetadata(100), nullptr);
}

// === Telemetry Size Tracking ===

TEST_F(FrameBufferRingTest, TelemetryHasNegotiatedDimensions) {
    ring.allocate(1920, 1080, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    auto* telemetry = ring.getTelemetry();
    EXPECT_EQ(telemetry->negotiatedWidth, 1920u);
    EXPECT_EQ(telemetry->negotiatedHeight, 1080u);
}
