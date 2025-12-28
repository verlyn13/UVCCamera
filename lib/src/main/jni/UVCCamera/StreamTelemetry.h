/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: StreamTelemetry.h
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

#ifndef STREAMTELEMETRY_H_
#define STREAMTELEMETRY_H_

#include <atomic>
#include <cstdint>

/**
 * Telemetry counters for monitoring frame buffer performance.
 * Provides hooks for real-time performance monitoring,
 * following professional camera HAL patterns.
 */
struct StreamTelemetry {
    // Frame layer counters (thread-safe)
    std::atomic<uint64_t> framesReceived{0};
    std::atomic<uint64_t> framesDropped{0};
    std::atomic<uint64_t> framesCorrupted{0};
    std::atomic<uint64_t> framesRendered{0};

    // Timing (thread-safe)
    std::atomic<int64_t> lastFrameTimestampNs{0};
    std::atomic<int64_t> avgDecodeTimeUs{0};
    std::atomic<int64_t> avgRenderTimeUs{0};

    // Negotiation state (set once at stream start)
    uint32_t negotiatedWidth{0};
    uint32_t negotiatedHeight{0};
    uint32_t negotiatedFps{0};
    uint32_t fallbackLevel{0};  // USB fallback ladder position

    void reset() {
        framesReceived.store(0, std::memory_order_relaxed);
        framesDropped.store(0, std::memory_order_relaxed);
        framesCorrupted.store(0, std::memory_order_relaxed);
        framesRendered.store(0, std::memory_order_relaxed);
        lastFrameTimestampNs.store(0, std::memory_order_relaxed);
        avgDecodeTimeUs.store(0, std::memory_order_relaxed);
        avgRenderTimeUs.store(0, std::memory_order_relaxed);
        negotiatedWidth = 0;
        negotiatedHeight = 0;
        negotiatedFps = 0;
        fallbackLevel = 0;
    }
};

#endif /* STREAMTELEMETRY_H_ */
