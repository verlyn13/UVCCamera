/**
 * Simplified Extension Unit Enumerator for UVCCamera
 * 
 * Target GUID: {1229a78c-47b4-4094-b0ce-db07386fb938}
 * Expected format: 16-bit deci-Celsius (0.1°C units)
 */

#include <jni.h>
#include <android/log.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "ExtensionUnitEnum"

// Include actual libuvc and libusb headers
#include "libuvc/libuvc.h"
#include "libusb.h"
#include "UVCCamera.h"

/**
 * Simple test function to verify extension units can be accessed
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_scopecam_camera_UvcCameraManager_nativeTestExtensionUnits(
    JNIEnv* env,
    jobject thiz,
    jlong cameraHandle) {
    
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Testing Extension Units");
    
    // Get the UVCCamera instance
    UVCCamera* camera = reinterpret_cast<UVCCamera*>(cameraHandle);
    if (!camera) {
        return env->NewStringUTF("ERROR: Invalid camera handle");
    }
    
    // Get the device handle
    uvc_device_handle_t* devh = camera->mDeviceHandle;
    if (!devh) {
        return env->NewStringUTF("ERROR: No device handle");
    }
    
    char resultBuffer[2048];
    snprintf(resultBuffer, sizeof(resultBuffer),
        "Extension Unit Test\n"
        "===================\n"
        "Camera Handle: Valid\n"
        "Device Handle: Valid\n"
        "\n"
        "Testing thermal access:\n"
        "Target GUID: {1229a78c-47b4-4094-b0ce-db07386fb938}\n"
        "\n"
        "Attempting to read from common extension units...\n");
    
    // Try to read from extension unit IDs 1-10, selector 1
    bool foundData = false;
    for (uint8_t unitId = 1; unitId <= 10; unitId++) {
        uint8_t data[4] = {0};
        
        // Try to read 2 bytes from selector 1
        int ret = uvc_get_ctrl(devh, unitId, 1, data, 2, UVC_GET_CUR);
        
        if (ret == UVC_SUCCESS) {
            uint16_t value = data[0] | (data[1] << 8);
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, 
                "Unit %d: Read successful, value=%d", unitId, value);
            
            // Check if it could be temperature (0-100°C = 0-1000 deci-Celsius)
            if (value > 100 && value < 500) {
                float temp = value / 10.0f;
                char tempStr[256];
                snprintf(tempStr, sizeof(tempStr),
                    "\n✓ Unit %d, Selector 1: %.1f°C (raw: %d)\n",
                    unitId, temp, value);
                strncat(resultBuffer, tempStr, sizeof(resultBuffer) - strlen(resultBuffer) - 1);
                foundData = true;
            }
        }
    }
    
    if (!foundData) {
        strncat(resultBuffer, "\nNo temperature data found in units 1-10\n",
                sizeof(resultBuffer) - strlen(resultBuffer) - 1);
    }
    
    strncat(resultBuffer, "\nTest complete.\n",
            sizeof(resultBuffer) - strlen(resultBuffer) - 1);
    
    return env->NewStringUTF(resultBuffer);
}

/**
 * Read temperature once we know the unit ID and selector
 */
extern "C" JNIEXPORT jfloat JNICALL
Java_com_scopecam_camera_UvcCameraManager_nativeReadTemperature(
    JNIEnv* env,
    jobject thiz,
    jlong cameraHandle,
    jint unitId,
    jint selector) {
    
    UVCCamera* camera = reinterpret_cast<UVCCamera*>(cameraHandle);
    if (!camera || !camera->mDeviceHandle) {
        return -1.0f;
    }
    
    uint8_t data[2] = {0};
    int ret = uvc_get_ctrl(camera->mDeviceHandle, unitId, selector, data, 2, UVC_GET_CUR);
    
    if (ret == UVC_SUCCESS) {
        uint16_t value = data[0] | (data[1] << 8);
        // Convert from deci-Celsius to Celsius
        return value / 10.0f;
    }
    
    return -1.0f;
}