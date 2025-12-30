/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 *
 * File name: IReadinessCallback.java
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
 * All files in the folder are under this Apache License, Version 2.0.
 */

package com.serenegiant.usb;

/**
 * Callback interface for native layer readiness notifications.
 * Called when the native preview thread is fully initialized and ready.
 * After receiving this callback, it is safe to call stopPreview().
 */
public interface IReadinessCallback {
    /**
     * Called when native layer is ready for streaming.
     * The preview thread has been created and is running.
     */
    void onNativeReady();
}
