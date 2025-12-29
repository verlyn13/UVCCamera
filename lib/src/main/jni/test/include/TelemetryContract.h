/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: TelemetryContract.h
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

namespace TelemetryContract {

// === Structural Constants ===
// Note: Using different names than the macros in StreamTelemetry.h to avoid
// preprocessor conflicts. The production code uses #define macros which would
// cause these constants to be substituted unexpectedly.
constexpr int kFrameBufferCount = 3;
constexpr int kErrorHistorySize = 8;

// === SlotState Enum Ordinals ===
// Kotlin SlotState.entries must match these values
namespace SlotStateOrdinal {
    constexpr int EMPTY = 0;
    constexpr int WRITING = 1;
    constexpr int READY = 2;
    constexpr int READING = 3;
    constexpr int COUNT = 4;
}

// === FallbackLevel Enum Ordinals ===
namespace FallbackLevelOrdinal {
    constexpr int NORMAL = 0;
    constexpr int FPS_REDUCED = 1;
    constexpr int RES_REDUCED = 2;
    constexpr int ALT_SETTING = 3;
    constexpr int BULK_MODE = 4;
    constexpr int COUNT = 5;
}

// === Value Ranges ===
namespace Ranges {
    constexpr int MIN_WIDTH = 160;
    constexpr int MAX_WIDTH = 4096;
    constexpr int MIN_HEIGHT = 120;
    constexpr int MAX_HEIGHT = 2160;
    constexpr int MIN_FPS = 1;
    constexpr int MAX_FPS = 120;
    constexpr int NO_FENCE = -1;
}

// === JNI Method Signatures (for Kotlin handoff) ===
// Document exact parameter order and types for NativeTelemetry
namespace JniSignatures {
    // NativeTelemetry JNI methods - parameter order:
    // 1.  usbPacketsReceived: Long (J)
    // 2.  usbOverflowErrors: Long (J)
    // 3.  usbTimeoutErrors: Long (J)
    // 4.  usbEndpointAddress: Int (I)
    // 5.  usbAltSetting: Int (I)
    // 6.  usbIsIsochronous: Boolean (Z)
    // 7.  usbMaxPacketSize: Int (I)
    // 8.  framesReceived: Long (J)
    // 9.  framesDropped: Long (J)
    // 10. framesCorrupted: Long (J)
    // 11. framesRendered: Long (J)
    // 12. avgDecodeTimeUs: Long (J)
    // 13. avgRenderTimeUs: Long (J)
    // 14. bufferLockWaitTimeNs: Long (J)
    // 15. fenceWaitTimeNs: Long (J)
    // 16. slotStates: IntArray ([I)
    // 17. producerStalls: Long (J)
    // 18. consumerStarves: Long (J)
    // 19. negotiatedWidth: Int (I)
    // 20. negotiatedHeight: Int (I)
    // 21. negotiatedFps: Int (I)
    // 22. fallbackLevel: Int (I)
    // 23. fallbackSinceNs: Long (J)
    // 24. fallbackReason: String? (Ljava/lang/String;)
    // 25. errorHistory: Array<NativeErrorEntry> (see ErrorEntry struct)

    // Error source max length (for truncation)
    constexpr int ERROR_SOURCE_MAX_LENGTH = 15;
}

// === Contract Version ===
constexpr int CONTRACT_VERSION = 1;

} // namespace TelemetryContract
