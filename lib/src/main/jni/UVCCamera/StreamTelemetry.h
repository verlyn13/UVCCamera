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
#include <cstring>
#include <time.h>

#define FRAME_BUFFER_COUNT 3
#define ERROR_HISTORY_SIZE 8

/**
 * Ring buffer slot state for telemetry visualization.
 */
enum class SlotState : int {
    EMPTY = 0,
    WRITING = 1,
    READY = 2,
    READING = 3
};

/**
 * Error history entry for debugging.
 * Stores recent libuvc/libusb error codes.
 */
struct ErrorEntry {
    int64_t timestampNs;
    int32_t code;
    char source[16];  // "libuvc", "libusb", "decode", etc.

    void reset() {
        timestampNs = 0;
        code = 0;
        source[0] = '\0';
    }
};

/**
 * Comprehensive telemetry for monitoring frame buffer and USB performance.
 * Designed for ScopeCam telemetry dashboard integration.
 *
 * All atomic fields are safe for cross-thread access.
 * Non-atomic fields are set once at stream start.
 */
struct StreamTelemetry {
    // =========================================================================
    // USB Layer (populated by libuvc/libusb callbacks)
    // =========================================================================
    std::atomic<uint64_t> usbPacketsReceived{0};
    std::atomic<uint64_t> usbOverflowErrors{0};
    std::atomic<uint64_t> usbTimeoutErrors{0};

    // USB Protocol Details (set at stream negotiation)
    std::atomic<uint8_t>  usbEndpointAddress{0};
    std::atomic<uint8_t>  usbAltSetting{0};
    std::atomic<bool>     usbIsIsochronous{true};
    std::atomic<uint32_t> usbMaxPacketSize{0};
    std::atomic<uint32_t> usbActiveUrbs{0};
    std::atomic<uint32_t> usbQueuedUrbs{0};

    // =========================================================================
    // Frame Layer
    // =========================================================================
    std::atomic<uint64_t> framesReceived{0};
    std::atomic<uint64_t> framesDropped{0};
    std::atomic<uint64_t> framesCorrupted{0};
    std::atomic<uint64_t> framesRendered{0};

    // =========================================================================
    // Timing (microseconds unless noted)
    // =========================================================================
    std::atomic<int64_t> lastDecodeTimeUs{0};
    std::atomic<int64_t> lastRenderTimeUs{0};
    std::atomic<int64_t> avgDecodeTimeUs{0};
    std::atomic<int64_t> avgRenderTimeUs{0};
    std::atomic<int64_t> minDecodeTimeUs{INT64_MAX};
    std::atomic<int64_t> maxDecodeTimeUs{0};

    // Contention metrics (nanoseconds)
    std::atomic<uint64_t> bufferLockWaitTimeNs{0};
    std::atomic<uint64_t> fenceWaitTimeNs{0};
    std::atomic<uint64_t> totalFenceWaits{0};  // Count of fence waits

    // =========================================================================
    // Ring Buffer State
    // =========================================================================
    std::atomic<int> slotStates[FRAME_BUFFER_COUNT];  // SlotState enum values
    std::atomic<int> currentWriteSlot{0};
    std::atomic<int> currentReadSlot{-1};
    std::atomic<int> latestCompletedSlot{-1};
    std::atomic<uint64_t> producerStalls{0};   // Consumer too slow
    std::atomic<uint64_t> consumerStarves{0};  // Producer too slow (no new frame)
    std::atomic<bool> fencePending{false};

    // =========================================================================
    // Negotiation State (set once at stream start, non-atomic)
    // =========================================================================
    uint32_t negotiatedWidth{0};
    uint32_t negotiatedHeight{0};
    uint32_t negotiatedFps{0};
    std::atomic<uint32_t> fallbackLevel{0};
    int64_t fallbackSinceNs{0};
    char fallbackReason[64]{0};

    // UVC format details
    uint32_t formatIndex{0};
    uint32_t frameIndex{0};
    uint32_t frameInterval{0};  // 100ns units per UVC spec
    char formatName[16]{0};     // "MJPEG", "YUY2", "UNCOMPRESSED", etc.

    // =========================================================================
    // Error History (circular buffer)
    // =========================================================================
    ErrorEntry errorHistory[ERROR_HISTORY_SIZE];
    std::atomic<int> errorHistoryIndex{0};
    std::atomic<int> errorHistoryCount{0};

    // =========================================================================
    // Timestamps (nanoseconds, CLOCK_MONOTONIC)
    // =========================================================================
    std::atomic<int64_t> lastFrameTimestampNs{0};
    std::atomic<int64_t> streamStartTimestampNs{0};

    // =========================================================================
    // Helper Methods
    // =========================================================================

    /**
     * Thread-safe error recording into circular buffer.
     */
    void recordError(int32_t code, const char* source) {
        int idx = errorHistoryIndex.fetch_add(1, std::memory_order_relaxed) % ERROR_HISTORY_SIZE;
        errorHistory[idx].timestampNs = getCurrentTimeNs();
        errorHistory[idx].code = code;
        strncpy(errorHistory[idx].source, source, 15);
        errorHistory[idx].source[15] = '\0';

        int count = errorHistoryCount.load(std::memory_order_relaxed);
        if (count < ERROR_HISTORY_SIZE) {
            errorHistoryCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /**
     * Update decode time with exponential moving average (alpha = 0.125).
     */
    void updateDecodeTime(int64_t timeUs) {
        lastDecodeTimeUs.store(timeUs, std::memory_order_relaxed);

        // EMA: new_avg = old_avg * 0.875 + new_value * 0.125
        int64_t current = avgDecodeTimeUs.load(std::memory_order_relaxed);
        if (current == 0) {
            avgDecodeTimeUs.store(timeUs, std::memory_order_relaxed);
        } else {
            avgDecodeTimeUs.store((current * 7 + timeUs) / 8, std::memory_order_relaxed);
        }

        // Track min/max
        int64_t minVal = minDecodeTimeUs.load(std::memory_order_relaxed);
        if (timeUs < minVal) {
            minDecodeTimeUs.store(timeUs, std::memory_order_relaxed);
        }

        int64_t maxVal = maxDecodeTimeUs.load(std::memory_order_relaxed);
        if (timeUs > maxVal) {
            maxDecodeTimeUs.store(timeUs, std::memory_order_relaxed);
        }
    }

    /**
     * Update render time with exponential moving average.
     */
    void updateRenderTime(int64_t timeUs) {
        lastRenderTimeUs.store(timeUs, std::memory_order_relaxed);

        int64_t current = avgRenderTimeUs.load(std::memory_order_relaxed);
        if (current == 0) {
            avgRenderTimeUs.store(timeUs, std::memory_order_relaxed);
        } else {
            avgRenderTimeUs.store((current * 7 + timeUs) / 8, std::memory_order_relaxed);
        }
    }

    /**
     * Record fence wait time and update average.
     */
    void recordFenceWait(int64_t waitTimeNs) {
        totalFenceWaits.fetch_add(1, std::memory_order_relaxed);

        // Update cumulative wait time
        uint64_t currentTotal = fenceWaitTimeNs.load(std::memory_order_relaxed);
        fenceWaitTimeNs.store(currentTotal + waitTimeNs, std::memory_order_relaxed);
    }

    /**
     * Set slot state for telemetry visualization.
     */
    void setSlotState(int slot, SlotState state) {
        if (slot >= 0 && slot < FRAME_BUFFER_COUNT) {
            slotStates[slot].store(static_cast<int>(state), std::memory_order_relaxed);
        }
    }

    /**
     * Mark stream start timestamp.
     */
    void markStreamStart() {
        streamStartTimestampNs.store(getCurrentTimeNs(), std::memory_order_relaxed);
    }

    /**
     * Set fallback level with reason.
     */
    void setFallback(uint32_t level, const char* reason) {
        fallbackLevel.store(level, std::memory_order_relaxed);
        fallbackSinceNs = getCurrentTimeNs();
        if (reason) {
            strncpy(fallbackReason, reason, 63);
            fallbackReason[63] = '\0';
        }
    }

    /**
     * Set USB protocol details at stream negotiation.
     */
    void setUsbProtocol(uint8_t endpoint, uint8_t altSetting, bool isIso, uint32_t maxPacket) {
        usbEndpointAddress.store(endpoint, std::memory_order_relaxed);
        usbAltSetting.store(altSetting, std::memory_order_relaxed);
        usbIsIsochronous.store(isIso, std::memory_order_relaxed);
        usbMaxPacketSize.store(maxPacket, std::memory_order_relaxed);
    }

    /**
     * Set UVC format details at stream negotiation.
     */
    void setFormatDetails(uint32_t fmtIdx, uint32_t frmIdx, uint32_t interval, const char* name) {
        formatIndex = fmtIdx;
        frameIndex = frmIdx;
        frameInterval = interval;
        if (name) {
            strncpy(formatName, name, 15);
            formatName[15] = '\0';
        }
    }

    /**
     * Get current time in nanoseconds (CLOCK_MONOTONIC).
     */
    static int64_t getCurrentTimeNs() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    }

    /**
     * Reset all telemetry counters and state.
     */
    void reset() {
        // USB layer
        usbPacketsReceived.store(0, std::memory_order_relaxed);
        usbOverflowErrors.store(0, std::memory_order_relaxed);
        usbTimeoutErrors.store(0, std::memory_order_relaxed);
        usbEndpointAddress.store(0, std::memory_order_relaxed);
        usbAltSetting.store(0, std::memory_order_relaxed);
        usbIsIsochronous.store(true, std::memory_order_relaxed);
        usbMaxPacketSize.store(0, std::memory_order_relaxed);
        usbActiveUrbs.store(0, std::memory_order_relaxed);
        usbQueuedUrbs.store(0, std::memory_order_relaxed);

        // Frame layer
        framesReceived.store(0, std::memory_order_relaxed);
        framesDropped.store(0, std::memory_order_relaxed);
        framesCorrupted.store(0, std::memory_order_relaxed);
        framesRendered.store(0, std::memory_order_relaxed);

        // Timing
        lastDecodeTimeUs.store(0, std::memory_order_relaxed);
        lastRenderTimeUs.store(0, std::memory_order_relaxed);
        avgDecodeTimeUs.store(0, std::memory_order_relaxed);
        avgRenderTimeUs.store(0, std::memory_order_relaxed);
        minDecodeTimeUs.store(INT64_MAX, std::memory_order_relaxed);
        maxDecodeTimeUs.store(0, std::memory_order_relaxed);
        bufferLockWaitTimeNs.store(0, std::memory_order_relaxed);
        fenceWaitTimeNs.store(0, std::memory_order_relaxed);
        totalFenceWaits.store(0, std::memory_order_relaxed);

        // Ring buffer state
        for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
            slotStates[i].store(static_cast<int>(SlotState::EMPTY), std::memory_order_relaxed);
        }
        currentWriteSlot.store(0, std::memory_order_relaxed);
        currentReadSlot.store(-1, std::memory_order_relaxed);
        latestCompletedSlot.store(-1, std::memory_order_relaxed);
        producerStalls.store(0, std::memory_order_relaxed);
        consumerStarves.store(0, std::memory_order_relaxed);
        fencePending.store(false, std::memory_order_relaxed);

        // Negotiation state
        negotiatedWidth = 0;
        negotiatedHeight = 0;
        negotiatedFps = 0;
        fallbackLevel.store(0, std::memory_order_relaxed);
        fallbackSinceNs = 0;
        fallbackReason[0] = '\0';
        formatIndex = 0;
        frameIndex = 0;
        frameInterval = 0;
        formatName[0] = '\0';

        // Error history
        for (int i = 0; i < ERROR_HISTORY_SIZE; i++) {
            errorHistory[i].reset();
        }
        errorHistoryIndex.store(0, std::memory_order_relaxed);
        errorHistoryCount.store(0, std::memory_order_relaxed);

        // Timestamps
        lastFrameTimestampNs.store(0, std::memory_order_relaxed);
        streamStartTimestampNs.store(0, std::memory_order_relaxed);
    }
};

#endif /* STREAMTELEMETRY_H_ */
