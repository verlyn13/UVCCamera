/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 * 
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
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

#include <jni.h>
#include <android/log.h>

#define LOG_TAG "ExtensionUnitUnsafe"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

/**
 * DANGER: Temporary unsafe accessor for XU probing during POC phase.
 * TODO: Remove this by end of week and replace with proper API.
 * 
 * This assumes the UVCCamera object layout has:
 * - [0]: vptr (virtual function table pointer)
 * - [8]: mDeviceHandle pointer
 * 
 * This WILL break if the object layout changes!
 */
extern "C" JNIEXPORT jlong JNICALL
Java_com_serenegiant_usb_UVCCamera_nativeGetDeviceHandleUnsafe(
    JNIEnv *env, jobject /*thiz*/, jlong id_camera) {
    
    LOGW("UNSAFE: Accessing device handle through memory offset - TEMPORARY HACK");
    
    if (id_camera == 0) {
        LOGW("UNSAFE: Camera handle is null");
        return 0;
    }
    
    // Interpret id_camera as a pointer to the UVCCamera object
    auto *camera = reinterpret_cast<void **>(id_camera);
    
    // Assume layout: [vptr][mDeviceHandle]
    // Skip the vptr (first pointer) to get to mDeviceHandle
    void **device_handle_ptr_addr = reinterpret_cast<void **>(
        reinterpret_cast<char *>(camera) + sizeof(void *)
    );
    
    // Dereference to get the actual device handle
    void *device_handle = *device_handle_ptr_addr;
    
    LOGD("UNSAFE: Camera object at %p, device handle at %p", camera, device_handle);
    
    return reinterpret_cast<jlong>(device_handle);
}