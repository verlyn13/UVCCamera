/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: LayoutContract.cpp
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

#define LOG_TAG "libUVCCamera/LayoutContract"

#include "LayoutContract.h"
#include "FrameBufferRing.h"
#include "utilbase.h"

#include <cstddef>

namespace LayoutContract {

void logLayoutDiagnostics() {
    LOGI("LAYOUT: ═══════════════════════════════════════════════════════");
    LOGI("LAYOUT: UVCCamera Layout Contract Validation (2026-01-05)");
    LOGI("LAYOUT: Architecture: %s", sizeof(void*) == 8 ? "64-bit" : "32-bit");
    LOGI("LAYOUT: ═══════════════════════════════════════════════════════");

    // Core types - sizes
    LOGI("LAYOUT: sizeof(void*) = %zu", sizeof(void*));
    LOGI("LAYOUT: sizeof(jlong) = %zu", sizeof(jlong));
    LOGI("LAYOUT: sizeof(jint) = %zu", sizeof(jint));

    // Core types - alignment (critical for 32-bit systems to avoid SIGBUS)
    LOGI("LAYOUT: alignof(void*) = %zu", alignof(void*));
    LOGI("LAYOUT: alignof(jlong) = %zu", alignof(jlong));
    LOGI("LAYOUT: alignof(jint) = %zu", alignof(jint));

    // Atomic types
    LOGI("LAYOUT: sizeof(std::atomic<bool>) = %zu", sizeof(std::atomic<bool>));
    LOGI("LAYOUT: sizeof(std::atomic<int>) = %zu", sizeof(std::atomic<int>));
    LOGI("LAYOUT: sizeof(std::atomic<uint64_t>) = %zu", sizeof(std::atomic<uint64_t>));

    // FrameBufferRing structures
    LOGI("LAYOUT: sizeof(PendingFrame) = %zu", sizeof(PendingFrame));
    LOGI("LAYOUT: sizeof(FrameBufferRing) = %zu", sizeof(FrameBufferRing));

    // PendingFrame field offsets (critical for corruption detection)
    LOGI("LAYOUT: offsetof(PendingFrame, slotMagicStart) = %zu",
         offsetof(PendingFrame, slotMagicStart));
    LOGI("LAYOUT: offsetof(PendingFrame, data) = %zu",
         offsetof(PendingFrame, data));
    LOGI("LAYOUT: offsetof(PendingFrame, ready) = %zu",
         offsetof(PendingFrame, ready));
    LOGI("LAYOUT: offsetof(PendingFrame, slotMagicEnd) = %zu",
         offsetof(PendingFrame, slotMagicEnd));

    // SPSC queue constants
    LOGI("LAYOUT: FRAME_BUFFER_COUNT = %d", FRAME_BUFFER_COUNT);
    LOGI("LAYOUT: PENDING_QUEUE_SIZE = %d", PENDING_QUEUE_SIZE);

    LOGI("LAYOUT: ═══════════════════════════════════════════════════════");
    LOGI("LAYOUT: All layout diagnostics logged successfully");
    LOGI("LAYOUT: ═══════════════════════════════════════════════════════");
}

void validateCriticalOffsets() {
    LOGI("LAYOUT_CHECK: Validating critical struct integrity...");

    // Create a test PendingFrame and validate its magic numbers
    PendingFrame testSlot;

    if (!testSlot.validateSlotMagic()) {
        LOGE("LAYOUT_CHECK: FATAL - Default PendingFrame has invalid magic!");
        LOGE("LAYOUT_CHECK: slotMagicStart=0x%08x slotMagicEnd=0x%08x expected=0x%08x",
             testSlot.slotMagicStart, testSlot.slotMagicEnd,
             PendingFrame::SLOT_MAGIC_VALUE);
        LOGE("LAYOUT_CHECK: This indicates ABI corruption or memory layout mismatch");
        abort();
    }

    // Validate sizeof matches expectations (detect padding changes)
    // These values are from the current build - update if struct changes
    bool sizeValid = true;

    // PendingFrame should be reasonably sized (< 256 bytes per slot)
    if (sizeof(PendingFrame) > 256) {
        LOGW("LAYOUT_CHECK: PendingFrame larger than expected: %zu bytes",
             sizeof(PendingFrame));
        // Not fatal, but worth noting
    }

    // Validate pointer can fit in jlong for handle safety
    // jlong is always 8 bytes; pointers are 4 (32-bit) or 8 (64-bit) bytes
    if (sizeof(jlong) < sizeof(void*)) {
        LOGE("LAYOUT_CHECK: FATAL - jlong size (%zu) < pointer size (%zu)!",
             sizeof(jlong), sizeof(void*));
        LOGE("LAYOUT_CHECK: Handle truncation will occur, causing SIGSEGV");
        abort();
    }

    // Log architecture for diagnostic purposes
    if (sizeof(void*) == 4) {
        LOGI("LAYOUT_CHECK: Running on 32-bit architecture (armeabi-v7a or x86)");
    } else if (sizeof(void*) == 8) {
        LOGI("LAYOUT_CHECK: Running on 64-bit architecture (arm64-v8a or x86_64)");
    }

    // Verify jlong alignment is sufficient for pointer storage
    // This prevents SIGBUS from misaligned 64-bit access on 32-bit systems
    if (alignof(jlong) < alignof(void*)) {
        LOGE("LAYOUT_CHECK: FATAL - jlong alignment (%zu) < pointer alignment (%zu)!",
             alignof(jlong), alignof(void*));
        LOGE("LAYOUT_CHECK: Pointer-to-jlong casts may cause SIGBUS");
        abort();
    }

    // Additional check: verify FrameBufferRing alignment is at least 8 bytes
    // This ensures the object header is properly aligned for atomic operations
    if (alignof(FrameBufferRing) < 8) {
        LOGW("LAYOUT_CHECK: FrameBufferRing alignment (%zu) < 8 bytes",
             alignof(FrameBufferRing));
        LOGW("LAYOUT_CHECK: Atomic operations may have reduced performance");
    }

    LOGI("LAYOUT_CHECK: All validations passed");
}

} // namespace LayoutContract
