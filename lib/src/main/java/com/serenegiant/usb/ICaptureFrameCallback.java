/*
 *  UVCCamera
 *  library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 *  All files in the folder are under this Apache License, Version 2.0.
 *  Files in the libjpeg-turbo, libusb, libuvc, rapidjson folder
 *  may have a different license, see the respective files.
 */

package com.serenegiant.usb;

import java.nio.ByteBuffer;

/**
 * Callback interface for native capture frame delivery.
 *
 * ============================================================================
 * CRITICAL MEMORY SAFETY REQUIREMENTS:
 * ============================================================================
 *
 * 1. THREADING: This callback is invoked from the native conversion thread.
 *    Implementation MUST be non-blocking (<1ms) to avoid frame drops.
 *
 * 2. BUFFER LIFETIME: The ByteBuffer is a direct buffer pointing to native
 *    memory. It is ONLY VALID during this callback invocation.
 *
 * 3. IMMEDIATE COPY REQUIRED: You MUST copy data from the buffer before
 *    returning from this method. The native memory is reused immediately.
 *
 *    CORRECT:
 *      byte[] data = new byte[buffer.remaining()];
 *      buffer.get(data);
 *      // ... use data ...
 *
 *    WRONG:
 *      savedBuffer = buffer;  // CRASH - buffer invalid after return
 */
public interface ICaptureFrameCallback {
    /**
     * Called when a capture frame is ready.
     *
     * @param buffer Direct ByteBuffer containing frame data (VALID ONLY DURING THIS CALL)
     * @param width Frame width in pixels
     * @param height Frame height in pixels
     * @param format Native format code
     * @param timestampNs Frame timestamp in nanoseconds (monotonic)
     */
    void onCaptureFrame(ByteBuffer buffer, int width, int height, int format, long timestampNs);
}