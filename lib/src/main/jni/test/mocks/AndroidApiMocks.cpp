/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: AndroidApiMocks.cpp
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

#ifdef UVCCAMERA_TESTING

#include "AndroidApiMocks.h"
#include <cstring>

namespace {
    // AHardwareBuffer state
    int g_allocateCallCount = 0;
    int g_releaseCallCount = 0;
    int g_allocateFailAfter = -1;
    int g_lockCallCount = 0;
    int g_unlockCallCount = 0;
    int32_t g_lockReturnStride = 0;
    int32_t g_unlockReturnFence = -1;

    // poll() state
    int g_pollCallCount = 0;
    int g_pollReturnValue = 1;  // Default: success
    bool g_pollShouldSignal = true;

    // close() state
    int g_closeCallCount = 0;
    int g_lastClosedFd = -1;

    // clock_gettime() state
    int64_t g_currentTimeNs = 1000000000LL;  // Start at 1 second
}

// === AHardwareBuffer Implementation ===

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer) {
    g_allocateCallCount++;
    if (g_allocateFailAfter >= 0 && g_allocateCallCount > g_allocateFailAfter) {
        *outBuffer = nullptr;
        return -1;
    }
    auto* buffer = new AHardwareBuffer();
    buffer->desc = *desc;
    buffer->desc.stride = desc->width;
    buffer->mockMemory.resize(desc->width * desc->height * 4, 0);
    buffer->refCount = 1;
    *outBuffer = buffer;
    return 0;
}

void AHardwareBuffer_acquire(AHardwareBuffer* buffer) {
    if (buffer && buffer->isValid) buffer->refCount++;
}

void AHardwareBuffer_release(AHardwareBuffer* buffer) {
    g_releaseCallCount++;
    if (buffer) {
        buffer->refCount--;
        if (buffer->refCount <= 0) {
            buffer->isValid = false;
            delete buffer;
        }
    }
}

void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc) {
    if (buffer && buffer->isValid && outDesc) *outDesc = buffer->desc;
}

int AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence,
                         const void* rect, void** outVirtualAddress) {
    g_lockCallCount++;
    if (!buffer || !buffer->isValid) return -1;
    buffer->lockCount++;
    *outVirtualAddress = buffer->mockMemory.data();
    return 0;
}

int AHardwareBuffer_unlock(AHardwareBuffer* buffer, int32_t* outFence) {
    g_unlockCallCount++;
    if (!buffer || !buffer->isValid || buffer->lockCount <= 0) return -1;
    buffer->lockCount--;
    if (outFence) *outFence = g_unlockReturnFence;
    return 0;
}

int AHardwareBuffer_lockAndGetInfo(AHardwareBuffer* buffer, uint64_t usage,
                                    int32_t fence, const void* rect,
                                    void** outVirtualAddress,
                                    int32_t* outBytesPerPixel,
                                    int32_t* outBytesPerStride) {
    int result = AHardwareBuffer_lock(buffer, usage, fence, rect, outVirtualAddress);
    if (result == 0) {
        if (outBytesPerPixel) *outBytesPerPixel = 4;
        if (outBytesPerStride) {
            *outBytesPerStride = (g_lockReturnStride > 0)
                ? g_lockReturnStride
                : static_cast<int32_t>(buffer->desc.stride * 4);
        }
    }
    return result;
}

// === poll() Implementation ===

int uvccamera_poll(struct uvccamera_pollfd* fds, unsigned long nfds, int timeout) {
    g_pollCallCount++;
    if (g_pollReturnValue < 0) return -1;
    if (g_pollShouldSignal && nfds > 0) {
        fds[0].revents = fds[0].events;
    }
    return g_pollReturnValue;
}

// === close() Implementation ===

int uvccamera_close(int fd) {
    g_closeCallCount++;
    g_lastClosedFd = fd;
    return 0;
}

// === MockControl Implementation ===

namespace MockControl {
    void setAllocateFailAfter(int successCount) { g_allocateFailAfter = successCount; }
    void setLockReturnStride(int32_t stride) { g_lockReturnStride = stride; }
    void setUnlockReturnFence(int32_t fenceFd) { g_unlockReturnFence = fenceFd; }
    void setPollReturnValue(int value) { g_pollReturnValue = value; }
    void setPollShouldSignal(bool signal) { g_pollShouldSignal = signal; }
    void setCurrentTimeNs(int64_t timeNs) { g_currentTimeNs = timeNs; }
    void advanceTimeNs(int64_t deltaNs) { g_currentTimeNs += deltaNs; }
    int64_t getCurrentTimeNs() { return g_currentTimeNs; }
    int getCloseCallCount() { return g_closeCallCount; }
    int getLastClosedFd() { return g_lastClosedFd; }

    void reset() {
        g_allocateCallCount = 0;
        g_releaseCallCount = 0;
        g_allocateFailAfter = -1;
        g_lockCallCount = 0;
        g_unlockCallCount = 0;
        g_lockReturnStride = 0;
        g_unlockReturnFence = -1;
        g_pollCallCount = 0;
        g_pollReturnValue = 1;
        g_pollShouldSignal = true;
        g_closeCallCount = 0;
        g_lastClosedFd = -1;
        g_currentTimeNs = 1000000000LL;
    }

    int getAllocateCallCount() { return g_allocateCallCount; }
    int getReleaseCallCount() { return g_releaseCallCount; }
    int getLockCallCount() { return g_lockCallCount; }
    int getUnlockCallCount() { return g_unlockCallCount; }
    int getPollCallCount() { return g_pollCallCount; }
}

#endif // UVCCAMERA_TESTING
