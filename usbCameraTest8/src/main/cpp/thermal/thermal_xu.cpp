/*
 * Thermal Extension Unit Probe
 * Quick scan implementation for discovering thermal data from UVC cameras
 */

#include <jni.h>
#include <string.h>
#include <stdio.h>
#include <android/log.h>
#if HAVE_UVC_LIBS
// Forward declarations instead of including full header
struct uvc_device_handle;
typedef struct uvc_device_handle uvc_device_handle_t;

// UVC control request codes
#define UVC_GET_CUR 0x81
#define UVC_GET_INFO 0x82

// External function from libuvc
extern "C" {
int uvc_get_ctrl(uvc_device_handle_t* devh, uint8_t unit, uint8_t ctrl, 
                 void* data, int len, uint8_t req_code);
}
#endif

#define THERMAL_LOG(...) __android_log_print(ANDROID_LOG_DEBUG, "ThermalXU", __VA_ARGS__)
#define THERMAL_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "ThermalXU", __VA_ARGS__)
#define THERMAL_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ThermalXU", __VA_ARGS__)

// Forward declaration for the unsafe accessor from UVCCamera
extern "C" JNIEXPORT jlong JNICALL
Java_com_serenegiant_usb_UVCCamera_nativeGetDeviceHandleUnsafe(JNIEnv *env, jobject thiz, jlong id_camera);

/**
 * Try to read from an extension unit
 * Returns 0 on success, negative on error
 */
#if HAVE_UVC_LIBS
static int try_xu_get(uvc_device_handle_t* devh, uint8_t unit_id, uint8_t selector, uint8_t* data, int len) {
    if (!devh || !data) {
        THERMAL_LOGW("try_xu_get: invalid parameters");
        return -1;
    }
    
    // Use UVC GET_CUR request for extension unit
    int ret = uvc_get_ctrl(devh, unit_id, selector, data, len, UVC_GET_CUR);
    
    if (ret < 0) {
        // This is expected for many unit/selector combinations
        return ret;
    }
    
    return 0;
}
#endif

/**
 * Native quick thermal test - scans multiple unit IDs and selectors
 * Returns JSON string with discovered thermal values
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_serenegiant_thermal_ThermalVerificationActivity_nativeQuickThermalTest(
    JNIEnv* env, jclass /*clazz*/, jlong idCamera) {
    
    THERMAL_LOG("Starting thermal XU probe with camera handle: %ld", idCamera);
    
#if !HAVE_UVC_LIBS
    THERMAL_LOGW("UVC libraries not available for this architecture");
    return env->NewStringUTF("{\"error\":\"UVC libraries not available for this architecture\"}");
#else
    
    if (idCamera == 0) {
        THERMAL_LOGE("Camera handle is null");
        return env->NewStringUTF("{\"error\":\"Camera handle is null\"}");
    }
    
    // Get the unsafe device handle - temporary hack for POC
    jclass uvcCameraClass = env->FindClass("com/serenegiant/usb/UVCCamera");
    if (!uvcCameraClass) {
        THERMAL_LOGE("Failed to find UVCCamera class");
        return env->NewStringUTF("{\"error\":\"Failed to find UVCCamera class\"}");
    }
    
    jmethodID getHandleMethod = env->GetStaticMethodID(uvcCameraClass, 
        "nativeGetDeviceHandleUnsafe", "(J)J");
    if (!getHandleMethod) {
        THERMAL_LOGE("Failed to find nativeGetDeviceHandleUnsafe method");
        return env->NewStringUTF("{\"error\":\"Failed to find unsafe handle method\"}");
    }
    
    jlong deviceHandleLong = env->CallStaticLongMethod(uvcCameraClass, getHandleMethod, idCamera);
    if (deviceHandleLong == 0) {
        THERMAL_LOGE("Failed to get device handle");
        return env->NewStringUTF("{\"error\":\"Failed to get device handle\"}");
    }
    
    uvc_device_handle_t* devh = reinterpret_cast<uvc_device_handle_t*>(deviceHandleLong);
    THERMAL_LOG("Got device handle: %p", devh);
    
    // Build JSON result
    char result[4096];
    size_t pos = 0;
    
    pos += snprintf(result + pos, sizeof(result) - pos, 
        "{\"scan_result\":{\"units\":[");
    
    bool first_unit = true;
    int total_reads = 0;
    
    // Scan unit IDs 3-6 as specified in the plan
    for (uint8_t unit_id = 3; unit_id <= 6; unit_id++) {
        bool unit_has_data = false;
        char unit_buffer[1024];
        size_t unit_pos = 0;
        
        unit_pos += snprintf(unit_buffer + unit_pos, sizeof(unit_buffer) - unit_pos,
            "{\"unit_id\":%d,\"selectors\":[", unit_id);
        
        bool first_selector = true;
        
        // Scan selectors 1-10
        for (uint8_t selector = 1; selector <= 10; selector++) {
            uint8_t data[2] = {0};
            
            int ret = try_xu_get(devh, unit_id, selector, data, sizeof(data));
            
            if (ret >= 0) {
                unit_has_data = true;
                total_reads++;
                
                // Convert to 16-bit little-endian value
                int16_t value = (int16_t)(data[0] | (data[1] << 8));
                double celsius = value / 10.0;
                
                if (!first_selector) {
                    unit_pos += snprintf(unit_buffer + unit_pos, 
                        sizeof(unit_buffer) - unit_pos, ",");
                }
                
                unit_pos += snprintf(unit_buffer + unit_pos, 
                    sizeof(unit_buffer) - unit_pos,
                    "{\"selector\":%d,\"hex\":\"%02x%02x\",\"int16_le\":%d,\"deciC\":%d,\"celsius\":%.1f}",
                    selector, data[0], data[1], value, value, celsius);
                
                first_selector = false;
                
                THERMAL_LOG("Unit %d, Selector %d: 0x%02x%02x = %d deciC = %.1f°C", 
                    unit_id, selector, data[0], data[1], value, celsius);
            }
        }
        
        unit_pos += snprintf(unit_buffer + unit_pos, sizeof(unit_buffer) - unit_pos, "]}");
        
        // Only add unit to result if it had data
        if (unit_has_data) {
            if (!first_unit) {
                pos += snprintf(result + pos, sizeof(result) - pos, ",");
            }
            pos += snprintf(result + pos, sizeof(result) - pos, "%s", unit_buffer);
            first_unit = false;
        }
    }
    
    pos += snprintf(result + pos, sizeof(result) - pos, 
        "],\"total_reads\":%d,\"status\":\"success\"}}", total_reads);
    
    THERMAL_LOG("Thermal scan complete. Total successful reads: %d", total_reads);
    
    return env->NewStringUTF(result);
#endif
}

/**
 * Try to read a specific unit/selector combination
 * Returns 2-byte array or null
 */
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_serenegiant_thermal_ThermalXuApi_tryRead(
    JNIEnv* env, jclass /*clazz*/, jint unitId, jint selector, jlong cameraId) {
    
#if !HAVE_UVC_LIBS
    return nullptr;
#else
    if (cameraId == 0) {
        return nullptr;
    }
    
    // Get device handle using unsafe accessor
    jclass uvcCameraClass = env->FindClass("com/serenegiant/usb/UVCCamera");
    if (!uvcCameraClass) {
        return nullptr;
    }
    
    jmethodID getHandleMethod = env->GetStaticMethodID(uvcCameraClass, 
        "nativeGetDeviceHandleUnsafe", "(J)J");
    if (!getHandleMethod) {
        return nullptr;
    }
    
    jlong deviceHandleLong = env->CallStaticLongMethod(uvcCameraClass, getHandleMethod, cameraId);
    if (deviceHandleLong == 0) {
        return nullptr;
    }
    
    uvc_device_handle_t* devh = reinterpret_cast<uvc_device_handle_t*>(deviceHandleLong);
    
    uint8_t data[2] = {0};
    int ret = try_xu_get(devh, (uint8_t)unitId, (uint8_t)selector, data, sizeof(data));
    
    if (ret < 0) {
        return nullptr;
    }
    
    // Create and return byte array
    jbyteArray result = env->NewByteArray(2);
    env->SetByteArrayRegion(result, 0, 2, (jbyte*)data);
    
    return result;
#endif
}