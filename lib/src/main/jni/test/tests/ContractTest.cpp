/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: ContractTest.cpp
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
 * Contract tests to verify that production code constants match the
 * authoritative contract defined in TelemetryContract.h.
 */

#include <gtest/gtest.h>
#include "TelemetryContract.h"
#include "ContractVerification.h"
#include "StreamTelemetry.h"

using namespace TelemetryContract;

TEST(ContractTest, FrameBufferCountMatchesContract) {
    EXPECT_EQ(FRAME_BUFFER_COUNT, kFrameBufferCount);
}

TEST(ContractTest, ErrorHistorySizeMatchesContract) {
    EXPECT_EQ(ERROR_HISTORY_SIZE, kErrorHistorySize);
}

TEST(ContractTest, SlotStateEnumOrdinals) {
    EXPECT_EQ(static_cast<int>(SlotState::EMPTY), SlotStateOrdinal::EMPTY);
    EXPECT_EQ(static_cast<int>(SlotState::WRITING), SlotStateOrdinal::WRITING);
    EXPECT_EQ(static_cast<int>(SlotState::READY), SlotStateOrdinal::READY);
    EXPECT_EQ(static_cast<int>(SlotState::READING), SlotStateOrdinal::READING);
}

TEST(ContractTest, FallbackLevelOrdinals) {
    EXPECT_EQ(FallbackLevelOrdinal::NORMAL, 0);
    EXPECT_EQ(FallbackLevelOrdinal::BULK_MODE, 4);
    EXPECT_EQ(FallbackLevelOrdinal::COUNT, 5);
}

TEST(ContractTest, NoFenceSentinel) {
    EXPECT_EQ(Ranges::NO_FENCE, -1);
}

TEST(ContractTest, ContractVersionPositive) {
    EXPECT_GT(CONTRACT_VERSION, 0);
}

TEST(ContractTest, ErrorSourceMaxLength) {
    // Verify that error source length in production matches contract
    ErrorEntry entry;
    // ErrorEntry.source is char[16], so max content is 15 + null terminator
    EXPECT_EQ(sizeof(entry.source) - 1, JniSignatures::ERROR_SOURCE_MAX_LENGTH);
}
