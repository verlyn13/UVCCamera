/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 * Copyright (c) 2024-2025 ScopeCam contributors
 *
 * File name: FrameSlotMetadata.h
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

#ifndef FRAMESLOTMETADATA_H_
#define FRAMESLOTMETADATA_H_

#include <cstdint>

/**
 * Per-slot metadata for AHardwareBuffer ring buffer.
 * Each buffer slot carries this metadata for fence handling,
 * format tracking, and telemetry.
 */
struct FrameSlotMetadata {
    int64_t  timestampNs;      // CLOCK_MONOTONIC capture time
    uint64_t frameNumber;      // Sequential frame counter
    uint32_t format;           // AHARDWAREBUFFER_FORMAT_*
    uint32_t width;
    uint32_t height;
    int32_t  strideBytes;      // ACTUAL stride from system (critical for GPU alignment)
    int      releaseFenceFd;   // Fence fd from unlock (-1 if none)
    bool     valid;            // Slot contains completed frame

    void reset() {
        timestampNs = 0;
        frameNumber = 0;
        format = 0;
        width = 0;
        height = 0;
        strideBytes = 0;
        releaseFenceFd = -1;
        valid = false;
    }
};

#endif /* FRAMESLOTMETADATA_H_ */
