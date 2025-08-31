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
    
    // For now, we can't access the private mDeviceHandle directly
    // This would need to be exposed via a public method or friend function
    // For testing, we'll just report that we can't access it yet
    return env->NewStringUTF(
        "Extension Unit Test\n"
        "===================\n"
        "Status: Camera connected but cannot access device handle\n"
        "Reason: mDeviceHandle is private - needs API modification\n"
        "\n"
        "Next steps:\n"
        "1. Add public getter for device handle in UVCCamera.h\n"
        "2. Or make extension unit functions friend functions\n"
        "3. Or add extension unit methods directly to UVCCamera class\n"
    );
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
    
    // Cannot access private mDeviceHandle without API changes
    // Return -1 to indicate not implemented yet
    return -1.0f;
}