/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: ContractVerification.h
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
 */

#pragma once

#include "TelemetryContract.h"

// Include StreamTelemetry.h AFTER TelemetryContract.h to get the production macros
// Note: The macros FRAME_BUFFER_COUNT and ERROR_HISTORY_SIZE are #defines in
// StreamTelemetry.h, so we need to be careful not to use them where they would
// conflict with namespace member names.
#include "StreamTelemetry.h"

namespace ContractVerification {

// Verify FRAME_BUFFER_COUNT matches
// Note: FRAME_BUFFER_COUNT is a macro from StreamTelemetry.h
// kFrameBufferCount is the contract constant
static_assert(
    FRAME_BUFFER_COUNT == TelemetryContract::kFrameBufferCount,
    "FRAME_BUFFER_COUNT mismatch with contract"
);

// Verify ERROR_HISTORY_SIZE matches
// Note: ERROR_HISTORY_SIZE is a macro from StreamTelemetry.h
// kErrorHistorySize is the contract constant
static_assert(
    ERROR_HISTORY_SIZE == TelemetryContract::kErrorHistorySize,
    "ERROR_HISTORY_SIZE mismatch with contract"
);

// Verify SlotState enum ordinals
static_assert(
    static_cast<int>(SlotState::EMPTY) == TelemetryContract::SlotStateOrdinal::EMPTY,
    "SlotState::EMPTY ordinal mismatch"
);
static_assert(
    static_cast<int>(SlotState::WRITING) == TelemetryContract::SlotStateOrdinal::WRITING,
    "SlotState::WRITING ordinal mismatch"
);
static_assert(
    static_cast<int>(SlotState::READY) == TelemetryContract::SlotStateOrdinal::READY,
    "SlotState::READY ordinal mismatch"
);
static_assert(
    static_cast<int>(SlotState::READING) == TelemetryContract::SlotStateOrdinal::READING,
    "SlotState::READING ordinal mismatch"
);

} // namespace ContractVerification
