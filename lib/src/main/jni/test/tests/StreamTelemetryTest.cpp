/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: StreamTelemetryTest.cpp
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
 * Unit tests for StreamTelemetry struct - tests atomic operations,
 * EMA calculations, circular buffer, and state transitions.
 */

#include <gtest/gtest.h>
#include "StreamTelemetry.h"
#include <thread>
#include <vector>

class StreamTelemetryTest : public ::testing::Test {
protected:
    StreamTelemetry telemetry;
    void SetUp() override { telemetry.reset(); }
};

TEST_F(StreamTelemetryTest, InitialCountersAreZero) {
    EXPECT_EQ(telemetry.framesReceived.load(), 0);
    EXPECT_EQ(telemetry.framesDropped.load(), 0);
    EXPECT_EQ(telemetry.usbPacketsReceived.load(), 0);
}

TEST_F(StreamTelemetryTest, AtomicIncrementThreadSafe) {
    constexpr int THREADS = 4;
    constexpr int INCREMENTS = 10000;

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; i++) {
        threads.emplace_back([this]() {
            for (int j = 0; j < INCREMENTS; j++) {
                telemetry.framesReceived.fetch_add(1);
            }
        });
    }

    for (auto& t : threads) t.join();
    EXPECT_EQ(telemetry.framesReceived.load(), THREADS * INCREMENTS);
}

TEST_F(StreamTelemetryTest, EmaConvergesToConsistentValue) {
    constexpr int64_t CONSISTENT_TIME = 1000;

    for (int i = 0; i < 100; i++) {
        telemetry.updateDecodeTime(CONSISTENT_TIME);
    }

    int64_t avg = telemetry.avgDecodeTimeUs.load();
    EXPECT_NEAR(avg, CONSISTENT_TIME, CONSISTENT_TIME * 0.1);
}

TEST_F(StreamTelemetryTest, EmaRespondsToChanges) {
    // Start with low values
    for (int i = 0; i < 10; i++) {
        telemetry.updateDecodeTime(100);
    }

    // Shift to high values
    for (int i = 0; i < 50; i++) {
        telemetry.updateDecodeTime(1000);
    }

    // Average should have moved toward 1000
    int64_t avg = telemetry.avgDecodeTimeUs.load();
    EXPECT_GT(avg, 500);  // Should be closer to 1000 than 100
}

TEST_F(StreamTelemetryTest, MinMaxTracking) {
    telemetry.updateDecodeTime(500);
    telemetry.updateDecodeTime(1500);
    telemetry.updateDecodeTime(1000);

    EXPECT_EQ(telemetry.minDecodeTimeUs.load(), 500);
    EXPECT_EQ(telemetry.maxDecodeTimeUs.load(), 1500);
}

TEST_F(StreamTelemetryTest, MinMaxResetOnReset) {
    telemetry.updateDecodeTime(500);
    telemetry.reset();

    // After reset, min should be INT64_MAX (uninitialized state)
    EXPECT_EQ(telemetry.minDecodeTimeUs.load(), INT64_MAX);
    EXPECT_EQ(telemetry.maxDecodeTimeUs.load(), 0);
}

TEST_F(StreamTelemetryTest, ErrorHistoryCircular) {
    for (int i = 0; i < ERROR_HISTORY_SIZE + 5; i++) {
        telemetry.recordError(i, "test");
    }

    EXPECT_EQ(telemetry.errorHistoryCount.load(), ERROR_HISTORY_SIZE);
}

TEST_F(StreamTelemetryTest, ErrorHistoryPreservesRecentErrors) {
    // Fill buffer plus some extra
    for (int i = 0; i < ERROR_HISTORY_SIZE + 3; i++) {
        telemetry.recordError(i, "test");
    }

    // Check that the most recent error (ERROR_HISTORY_SIZE + 2) exists
    // The index wraps around, so we need to find it
    bool found = false;
    int expectedCode = ERROR_HISTORY_SIZE + 2;
    for (int i = 0; i < ERROR_HISTORY_SIZE; i++) {
        if (telemetry.errorHistory[i].code == expectedCode) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(StreamTelemetryTest, ErrorSourceTruncated) {
    // Source longer than 15 chars should be truncated
    telemetry.recordError(1, "this_is_a_very_long_source_name");

    // Find the recorded error and check truncation
    int idx = (telemetry.errorHistoryIndex.load() - 1 + ERROR_HISTORY_SIZE) % ERROR_HISTORY_SIZE;
    EXPECT_EQ(strlen(telemetry.errorHistory[idx].source), 15);
    EXPECT_STREQ(telemetry.errorHistory[idx].source, "this_is_a_very_");
}

TEST_F(StreamTelemetryTest, SlotStateTransitions) {
    telemetry.setSlotState(0, SlotState::WRITING);
    EXPECT_EQ(telemetry.slotStates[0].load(), static_cast<int>(SlotState::WRITING));

    telemetry.setSlotState(0, SlotState::READY);
    EXPECT_EQ(telemetry.slotStates[0].load(), static_cast<int>(SlotState::READY));
}

TEST_F(StreamTelemetryTest, SlotStateBoundsChecked) {
    // Setting state for invalid slot should not crash
    telemetry.setSlotState(-1, SlotState::READY);  // Should be no-op
    telemetry.setSlotState(FRAME_BUFFER_COUNT, SlotState::READY);  // Should be no-op

    // Valid slots should still be in EMPTY state from reset
    for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
        EXPECT_EQ(telemetry.slotStates[i].load(), static_cast<int>(SlotState::EMPTY));
    }
}

TEST_F(StreamTelemetryTest, FenceWaitTracking) {
    telemetry.recordFenceWait(1000000);  // 1ms
    telemetry.recordFenceWait(2000000);  // 2ms

    EXPECT_EQ(telemetry.totalFenceWaits.load(), 2);
    EXPECT_EQ(telemetry.fenceWaitTimeNs.load(), 3000000);
}

TEST_F(StreamTelemetryTest, RenderTimeTracking) {
    telemetry.updateRenderTime(500);
    telemetry.updateRenderTime(500);

    EXPECT_EQ(telemetry.lastRenderTimeUs.load(), 500);
    EXPECT_NEAR(telemetry.avgRenderTimeUs.load(), 500, 50);
}

TEST_F(StreamTelemetryTest, FallbackTracking) {
    telemetry.setFallback(2, "Resolution reduced due to bandwidth");

    EXPECT_EQ(telemetry.fallbackLevel.load(), 2);
    EXPECT_STREQ(telemetry.fallbackReason, "Resolution reduced due to bandwidth");
    EXPECT_GT(telemetry.fallbackSinceNs, 0);
}

TEST_F(StreamTelemetryTest, UsbProtocolTracking) {
    telemetry.setUsbProtocol(0x81, 5, true, 3072);

    EXPECT_EQ(telemetry.usbEndpointAddress.load(), 0x81);
    EXPECT_EQ(telemetry.usbAltSetting.load(), 5);
    EXPECT_TRUE(telemetry.usbIsIsochronous.load());
    EXPECT_EQ(telemetry.usbMaxPacketSize.load(), 3072);
}

TEST_F(StreamTelemetryTest, FormatDetailsTracking) {
    telemetry.setFormatDetails(1, 3, 333333, "MJPEG");

    EXPECT_EQ(telemetry.formatIndex, 1);
    EXPECT_EQ(telemetry.frameIndex, 3);
    EXPECT_EQ(telemetry.frameInterval, 333333);  // ~30fps in 100ns units
    EXPECT_STREQ(telemetry.formatName, "MJPEG");
}

TEST_F(StreamTelemetryTest, StreamStartTimestamp) {
    telemetry.markStreamStart();

    EXPECT_GT(telemetry.streamStartTimestampNs.load(), 0);
}

TEST_F(StreamTelemetryTest, ResetClearsAllState) {
    // Set various state
    telemetry.framesReceived.store(100);
    telemetry.framesDropped.store(5);
    telemetry.updateDecodeTime(1000);
    telemetry.recordError(1, "test");
    telemetry.setSlotState(0, SlotState::WRITING);
    telemetry.setFallback(1, "Test fallback");

    // Reset
    telemetry.reset();

    // Verify reset
    EXPECT_EQ(telemetry.framesReceived.load(), 0);
    EXPECT_EQ(telemetry.framesDropped.load(), 0);
    EXPECT_EQ(telemetry.avgDecodeTimeUs.load(), 0);
    EXPECT_EQ(telemetry.errorHistoryCount.load(), 0);
    EXPECT_EQ(telemetry.slotStates[0].load(), static_cast<int>(SlotState::EMPTY));
    EXPECT_EQ(telemetry.fallbackLevel.load(), 0);
}
