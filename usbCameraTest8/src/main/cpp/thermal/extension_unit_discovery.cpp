/*
 * Extension Unit Discovery
 * Helper functions for discovering and enumerating UVC extension units
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

#define DISCOVERY_LOG(...) __android_log_print(ANDROID_LOG_DEBUG, "XUDiscovery", __VA_ARGS__)
#define DISCOVERY_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "XUDiscovery", __VA_ARGS__)

// Known thermal GUID: {1229a78c-47b4-4094-b0ce-db07386fb938}
static const uint8_t THERMAL_GUID[] = {
    0x8c, 0xa7, 0x29, 0x12, 0xb4, 0x47, 0x94, 0x40,
    0xb0, 0xce, 0xdb, 0x07, 0x38, 0x6f, 0xb9, 0x38
};

/**
 * Check if a GUID matches the known thermal GUID
 */
static bool is_thermal_guid(const uint8_t* guid) {
    return memcmp(guid, THERMAL_GUID, 16) == 0;
}

/**
 * Format a GUID as a string
 */
static void format_guid(const uint8_t* guid, char* buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size,
        "{%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
        guid[3], guid[2], guid[1], guid[0],
        guid[5], guid[4],
        guid[7], guid[6],
        guid[8], guid[9],
        guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
}

/**
 * Enumerate extension units in the device
 * This is a discovery helper that logs what it finds
 */
extern "C" JNIEXPORT jstring JNICALL
Java_com_serenegiant_thermal_ThermalVerificationActivity_nativeEnumerateExtensionUnits(
    JNIEnv* env, jclass /*clazz*/, jlong deviceHandleLong) {
    
#if !HAVE_UVC_LIBS
    return env->NewStringUTF("{\"error\":\"UVC libraries not available for this architecture\"}");
#else
    if (deviceHandleLong == 0) {
        return env->NewStringUTF("{\"error\":\"Invalid device handle\"}");
    }
    
    uvc_device_handle_t* devh = reinterpret_cast<uvc_device_handle_t*>(deviceHandleLong);
    
    char result[2048];
    size_t pos = 0;
    
    pos += snprintf(result + pos, sizeof(result) - pos, 
        "{\"extension_units\":[");
    
    // Note: Full enumeration would require access to device descriptors
    // For now, we'll probe known unit IDs and check for responses
    
    DISCOVERY_LOG("Probing for extension units...");
    
    bool first = true;
    for (uint8_t unit_id = 1; unit_id <= 10; unit_id++) {
        // Try a simple GET_INFO request to see if unit exists
        uint8_t info;
        int ret = uvc_get_ctrl(devh, unit_id, 0x01, &info, 1, UVC_GET_INFO);
        
        if (ret >= 0) {
            DISCOVERY_LOG("Found extension unit at ID %d", unit_id);
            
            if (!first) {
                pos += snprintf(result + pos, sizeof(result) - pos, ",");
            }
            
            pos += snprintf(result + pos, sizeof(result) - pos,
                "{\"unit_id\":%d,\"info\":\"0x%02x\"}", unit_id, info);
            
            first = false;
        }
    }
    
    pos += snprintf(result + pos, sizeof(result) - pos, "]}");
    
    return env->NewStringUTF(result);
#endif
}

/**
 * Verify if the thermal extension unit is present
 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_serenegiant_thermal_ThermalXuApi_isThermalUnitPresent(
    JNIEnv* /*env*/, jclass /*clazz*/, jlong deviceHandleLong) {
    
#if !HAVE_UVC_LIBS
    return JNI_FALSE;
#else
    if (deviceHandleLong == 0) {
        return JNI_FALSE;
    }
    
    uvc_device_handle_t* devh = reinterpret_cast<uvc_device_handle_t*>(deviceHandleLong);
    
    // Try to read from known thermal unit/selector combinations
    // Unit IDs 3-6, selector 1 are common for thermal
    for (uint8_t unit_id = 3; unit_id <= 6; unit_id++) {
        uint8_t data[2] = {0};
        int ret = uvc_get_ctrl(devh, unit_id, 1, data, 2, UVC_GET_CUR);
        
        if (ret >= 0) {
            // Check if the value looks like a plausible temperature
            // (e.g., between -40°C and 120°C in deci-Celsius)
            int16_t value = (int16_t)(data[0] | (data[1] << 8));
            if (value >= -400 && value <= 1200) {
                DISCOVERY_LOG("Found plausible thermal data at unit %d: %d deciC", 
                    unit_id, value);
                return JNI_TRUE;
            }
        }
    }
    
    return JNI_FALSE;
#endif
}