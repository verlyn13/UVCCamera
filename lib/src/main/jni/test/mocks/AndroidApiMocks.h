/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: AndroidApiMocks.h
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
 * Mock implementations for Android APIs to enable host-based testing.
 * This file provides mock versions of Android-specific APIs (AHardwareBuffer)
 * and controllable versions of system calls (poll, close) for testing.
 */

#pragma once

#ifdef UVCCAMERA_TESTING

#include <cstdint>
#include <vector>
#include <functional>

// === Simulate API Level for Conditional Compilation ===
#ifndef __ANDROID_API__
    #define __ANDROID_API__ 29  // Test against API 29 behavior
#endif

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

struct AHardwareBuffer {
    AHardwareBuffer_Desc desc;
    std::vector<uint8_t> mockMemory;
    int refCount = 1;
    int lockCount = 0;
    bool isValid = true;
};

// AHardwareBuffer API
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer);
void AHardwareBuffer_release(AHardwareBuffer* buffer);
void AHardwareBuffer_acquire(AHardwareBuffer* buffer);
void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc);
int AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence,
                         const void* rect, void** outVirtualAddress);
int AHardwareBuffer_unlock(AHardwareBuffer* buffer, int32_t* outFence);
int AHardwareBuffer_lockAndGetInfo(AHardwareBuffer* buffer, uint64_t usage,
                                    int32_t fence, const void* rect,
                                    void** outVirtualAddress,
                                    int32_t* outBytesPerPixel,
                                    int32_t* outBytesPerStride);

// Usage flags (must match Android values)
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN     0x00000003ULL
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN    0x00000030ULL
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE  0x00000100ULL
#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM    1

// === poll() Mock ===
// Use uvccamera_ prefix to avoid conflicts with system poll
#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008

struct uvccamera_pollfd {
    int   fd;
    short events;
    short revents;
};

// Mock poll function (different signature to avoid conflict with system poll)
int uvccamera_poll(struct uvccamera_pollfd* fds, unsigned long nfds, int timeout);

// Redirect production code's poll calls to our mock
#define pollfd uvccamera_pollfd
#define poll(fds, nfds, timeout) uvccamera_poll(fds, nfds, timeout)

// === close() Mock ===
// Use uvccamera_ prefix to avoid conflicts with system close
int uvccamera_close(int fd);

// Redirect production code's close calls to our mock
#define close(fd) uvccamera_close(fd)

// === Logging Stubs ===
#define LOG_TAG "test"
#define LOGE(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
#define LOGD(...) ((void)0)
#define LOGV(...) ((void)0)

// === Branch Hints ===
#ifndef LIKELY
#define LIKELY(x)   (x)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) (x)
#endif

// === Mock Control Interface ===
namespace MockControl {
    // AHardwareBuffer controls
    void setAllocateFailAfter(int successCount);
    void setLockReturnStride(int32_t stride);
    void setUnlockReturnFence(int32_t fenceFd);

    // poll() controls
    void setPollReturnValue(int value);
    void setPollShouldSignal(bool signal);

    // clock_gettime() controls (for StreamTelemetry)
    void setCurrentTimeNs(int64_t timeNs);
    void advanceTimeNs(int64_t deltaNs);
    int64_t getCurrentTimeNs();

    // close() tracking
    int getCloseCallCount();
    int getLastClosedFd();

    // Reset all mocks
    void reset();

    // Inspection
    int getAllocateCallCount();
    int getReleaseCallCount();
    int getLockCallCount();
    int getUnlockCallCount();
    int getPollCallCount();
}

#endif // UVCCAMERA_TESTING
