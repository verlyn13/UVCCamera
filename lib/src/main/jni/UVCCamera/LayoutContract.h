/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: LayoutContract.h
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

#ifndef LAYOUTCONTRACT_H_
#define LAYOUTCONTRACT_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <jni.h>

/**
 * Layout Contract for ABI-sensitive structures.
 *
 * This module provides compile-time and runtime validation of critical
 * memory layout assumptions. It detects ABI drift between builds that
 * could cause memory corruption or SIGSEGV crashes.
 *
 * Usage:
 * - Compile-time: static_asserts prevent building with incorrect assumptions
 * - Runtime: validateLayouts() logs all sizes for tombstone correlation
 * - Startup: validateCriticalOffsets() aborts early if layout is corrupted
 */
namespace LayoutContract {

//======================================================================
// Compile-Time Static Assertions
//======================================================================

// Pointer size must be 32-bit or 64-bit (ARM/ARM64/x86/x86_64 Android)
// Both are supported: 32-bit pointers fit in jlong with zero-extension,
// 64-bit pointers fit exactly in jlong.
static_assert(sizeof(void*) == 4 || sizeof(void*) == 8,
              "Pointer size must be 4 (32-bit) or 8 (64-bit) bytes");

// JNI types must match expected sizes
// jlong is always 8 bytes per JNI spec - this is critical for pointer handles
static_assert(sizeof(jlong) == 8, "jlong must be 64-bit for pointer handles");
static_assert(sizeof(jint) == 4, "jint must be 32-bit");

// Verify jlong can always hold a pointer (guaranteed by above assertions)
static_assert(sizeof(jlong) >= sizeof(void*),
              "jlong must be large enough to hold a pointer");

// Alignment requirements for pointer-to-jlong casts
// On 32-bit systems, jlong (8 bytes) may require stricter alignment than void* (4 bytes).
// This ensures we won't get SIGBUS from misaligned 64-bit access.
static_assert(alignof(jlong) >= alignof(void*),
              "jlong alignment must be >= pointer alignment for safe casts");

// C++ atomic types must be lock-free for performance
static_assert(sizeof(std::atomic<bool>) <= 4,
              "std::atomic<bool> too large for expected layout");
static_assert(sizeof(std::atomic<int>) <= 8,
              "std::atomic<int> too large for expected layout");

//======================================================================
// Runtime Validation Functions
//======================================================================

/**
 * Log all critical structure sizes.
 * Called during JNI_OnLoad for tombstone correlation.
 *
 * Output format: LAYOUT: sizeof(Type) = N
 */
void logLayoutDiagnostics();

/**
 * Validate critical struct offsets haven't changed.
 * Detects ABI drift between library versions.
 * Aborts if validation fails.
 */
void validateCriticalOffsets();

} // namespace LayoutContract

#endif /* LAYOUTCONTRACT_H_ */
