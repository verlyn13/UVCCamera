/**
 * EGLImageHelperJNI.cpp
 *
 * Zero-copy bridge between AHardwareBuffer and OpenGL ES via EGLImage.
 * Implements the 2026 "Fence-First" synchronization pattern.
 *
 * Required Extensions:
 *   - EGL_ANDROID_image_native_buffer
 *   - EGL_ANDROID_native_fence_sync
 *   - EGL_KHR_image_base
 *   - GL_OES_EGL_image_external
 *
 * @author UVCCamera Native Agent
 * @version 2.6
 * @date 2026-01-06
 */

#include <jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer_jni.h>
#include <android/log.h>
#include <atomic>

#define LOG_TAG "EGLImageHelperJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ═══════════════════════════════════════════════════════════════════════════
// EGL EXTENSION FUNCTION POINTERS
// ═══════════════════════════════════════════════════════════════════════════

// EGL_ANDROID_image_native_buffer
static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC eglGetNativeClientBufferANDROID = nullptr;

// EGL_KHR_image_base
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;

// EGL_KHR_fence_sync / EGL_ANDROID_native_fence_sync
static PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR = nullptr;
static PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR = nullptr;
static PFNEGLWAITSYNCKHRPROC eglWaitSyncKHR = nullptr;
static PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR = nullptr;
static PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID = nullptr;

// GL_OES_EGL_image_external
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = nullptr;

// Extension load state
static std::atomic<bool> sExtensionsLoaded{false};
static std::atomic<bool> sExtensionsValid{false};

// ═══════════════════════════════════════════════════════════════════════════
// EXTENSION LOADING
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Loads EGL/GL extension function pointers.
 * Thread-safe via atomic flag.
 *
 * @return true if critical extensions are available
 */
static bool loadExtensions() {
    // Fast path: already loaded
    if (sExtensionsLoaded.load(std::memory_order_acquire)) {
        return sExtensionsValid.load(std::memory_order_acquire);
    }

    LOGI("Loading EGL/GL extensions for zero-copy pipeline");

    // EGL_ANDROID_image_native_buffer (REQUIRED)
    eglGetNativeClientBufferANDROID = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)
        eglGetProcAddress("eglGetNativeClientBufferANDROID");

    // EGL_KHR_image_base (REQUIRED)
    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");

    // EGL_KHR_fence_sync (REQUIRED for Fence-First pattern)
    eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)
        eglGetProcAddress("eglCreateSyncKHR");
    eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)
        eglGetProcAddress("eglDestroySyncKHR");
    eglWaitSyncKHR = (PFNEGLWAITSYNCKHRPROC)
        eglGetProcAddress("eglWaitSyncKHR");
    eglClientWaitSyncKHR = (PFNEGLCLIENTWAITSYNCKHRPROC)
        eglGetProcAddress("eglClientWaitSyncKHR");

    // EGL_ANDROID_native_fence_sync (OPTIONAL but recommended)
    eglDupNativeFenceFDANDROID = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
        eglGetProcAddress("eglDupNativeFenceFDANDROID");

    // GL_OES_EGL_image_external (REQUIRED)
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");

    // Log extension availability
    LOGI("Extension pointers loaded:");
    LOGI("  eglGetNativeClientBufferANDROID: %p %s",
         eglGetNativeClientBufferANDROID,
         eglGetNativeClientBufferANDROID ? "OK" : "MISSING (REQUIRED)");
    LOGI("  eglCreateImageKHR: %p %s",
         eglCreateImageKHR,
         eglCreateImageKHR ? "OK" : "MISSING (REQUIRED)");
    LOGI("  eglDestroyImageKHR: %p %s",
         eglDestroyImageKHR,
         eglDestroyImageKHR ? "OK" : "MISSING (REQUIRED)");
    LOGI("  eglCreateSyncKHR: %p %s",
         eglCreateSyncKHR,
         eglCreateSyncKHR ? "OK" : "MISSING (REQUIRED)");
    LOGI("  eglDestroySyncKHR: %p %s",
         eglDestroySyncKHR,
         eglDestroySyncKHR ? "OK" : "MISSING (REQUIRED)");
    LOGI("  eglDupNativeFenceFDANDROID: %p %s",
         eglDupNativeFenceFDANDROID,
         eglDupNativeFenceFDANDROID ? "OK" : "(optional)");
    LOGI("  glEGLImageTargetTexture2DOES: %p %s",
         glEGLImageTargetTexture2DOES,
         glEGLImageTargetTexture2DOES ? "OK" : "MISSING (REQUIRED)");

    // Validate critical extensions
    bool valid = eglGetNativeClientBufferANDROID != nullptr &&
                 eglCreateImageKHR != nullptr &&
                 eglDestroyImageKHR != nullptr &&
                 eglCreateSyncKHR != nullptr &&
                 eglDestroySyncKHR != nullptr &&
                 glEGLImageTargetTexture2DOES != nullptr;

    if (!valid) {
        LOGE("CRITICAL: Required EGL extensions not available!");
        LOGE("Zero-copy rendering will not work on this device.");
    } else {
        LOGI("All required extensions available - zero-copy pipeline ready");
    }

    sExtensionsValid.store(valid, std::memory_order_release);
    sExtensionsLoaded.store(true, std::memory_order_release);

    return valid;
}

// ═══════════════════════════════════════════════════════════════════════════
// CONTEXT SAFETY HELPERS (2026 Standard)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Validates that an EGL context is current.
 * In multi-threaded agentic workflows, JNI calls may fire before
 * the render thread has initialized its EGL surface.
 *
 * @param operation Name of operation for logging
 * @return Current EGLDisplay, or EGL_NO_DISPLAY if not ready
 */
static EGLDisplay validateEGLContext(const char* operation) {
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY) {
        LOGW("%s: No current EGL display", operation);
        return EGL_NO_DISPLAY;
    }

    EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) {
        LOGW("%s: No current EGL context (render thread not ready?)", operation);
        return EGL_NO_DISPLAY;
    }

    return display;
}

/**
 * Translates EGL error codes to human-readable strings.
 */
static const char* eglErrorString(EGLint error) {
    switch (error) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC (check HardwareBuffer usage flags)";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
        default: return "Unknown EGL error";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// JNI IMPLEMENTATIONS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Creates an EGLImage from an Android HardwareBuffer for zero-copy texture binding.
 *
 * This is the core of the zero-copy pipeline:
 *   HardwareBuffer (camera) → EGLImage → GL_TEXTURE_EXTERNAL_OES
 *
 * @param eglDisplay The EGLDisplay (Java wrapper, we use current display)
 * @param hardwareBuffer The HardwareBuffer containing frame data
 * @return EGLImage handle as jlong, or 0 on failure
 *
 * Common Errors:
 *   - EGL_BAD_ALLOC (0x3003): HardwareBuffer usage flags incompatible
 *     Solution: Ensure AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE is set
 */
static jlong nativeCreateEGLImageFromHardwareBuffer(
        JNIEnv *env, jobject thiz, jobject eglDisplay, jobject hardwareBuffer) {

    // Step 1: Load extensions (idempotent)
    if (!loadExtensions()) {
        LOGE("createEGLImage: Required extensions not available");
        return 0;
    }

    // Step 2: Validate EGL context (2026 Context Safety standard)
    EGLDisplay display = validateEGLContext("createEGLImage");
    if (display == EGL_NO_DISPLAY) {
        return 0;
    }

    // Step 3: Get native AHardwareBuffer from Java HardwareBuffer
    if (hardwareBuffer == nullptr) {
        LOGE("createEGLImage: HardwareBuffer is null");
        return 0;
    }

    AHardwareBuffer* buffer = AHardwareBuffer_fromHardwareBuffer(env, hardwareBuffer);
    if (buffer == nullptr) {
        LOGE("createEGLImage: Failed to get AHardwareBuffer from HardwareBuffer");
        return 0;
    }

    // Step 4: Get EGLClientBuffer from AHardwareBuffer
    EGLClientBuffer clientBuffer = eglGetNativeClientBufferANDROID(buffer);
    if (clientBuffer == nullptr) {
        EGLint error = eglGetError();
        LOGE("createEGLImage: eglGetNativeClientBufferANDROID failed: 0x%x (%s)",
             error, eglErrorString(error));
        return 0;
    }

    // Step 5: Create EGLImage with appropriate attributes
    // EGL_IMAGE_PRESERVED_KHR ensures content survives context changes
    EGLint attrs[] = {
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_NONE
    };

    EGLImageKHR image = eglCreateImageKHR(
        display,
        EGL_NO_CONTEXT,  // No context needed for NATIVE_BUFFER_ANDROID
        EGL_NATIVE_BUFFER_ANDROID,
        clientBuffer,
        attrs
    );

    if (image == EGL_NO_IMAGE_KHR) {
        EGLint error = eglGetError();
        LOGE("createEGLImage: eglCreateImageKHR failed: 0x%x (%s)",
             error, eglErrorString(error));

        if (error == EGL_BAD_ALLOC) {
            LOGE("HINT: EGL_BAD_ALLOC usually means HardwareBuffer usage flags are wrong.");
            LOGE("HINT: Ensure AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE is set in Kotlin.");
        }
        return 0;
    }

    LOGI("createEGLImage: Created EGLImage %p from HardwareBuffer %p", image, buffer);
    return reinterpret_cast<jlong>(image);
}

/**
 * Destroys an EGLImage previously created by nativeCreateEGLImageFromHardwareBuffer.
 *
 * @param eglDisplay The EGLDisplay (Java wrapper, we use current display)
 * @param image The EGLImage handle to destroy
 */
static void nativeDestroyEGLImage(
        JNIEnv *env, jobject thiz, jobject eglDisplay, jlong image) {

    if (!loadExtensions() || eglDestroyImageKHR == nullptr) {
        LOGE("destroyEGLImage: eglDestroyImageKHR not available");
        return;
    }

    if (image == 0) {
        LOGW("destroyEGLImage: Null image handle, nothing to destroy");
        return;
    }

    EGLDisplay display = validateEGLContext("destroyEGLImage");
    if (display == EGL_NO_DISPLAY) {
        // Try to destroy anyway using the default display
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY) {
            LOGE("destroyEGLImage: Cannot get any EGL display");
            return;
        }
    }

    EGLImageKHR eglImage = reinterpret_cast<EGLImageKHR>(image);
    EGLBoolean result = eglDestroyImageKHR(display, eglImage);

    if (result == EGL_FALSE) {
        EGLint error = eglGetError();
        LOGE("destroyEGLImage: eglDestroyImageKHR failed: 0x%x (%s)",
             error, eglErrorString(error));
    } else {
        LOGI("destroyEGLImage: Destroyed EGLImage %p", eglImage);
    }
}

/**
 * Binds an EGLImage to the currently bound GL texture.
 *
 * IMPORTANT: The target should be GL_TEXTURE_EXTERNAL_OES for camera frames.
 * This allows the GPU's fixed-function YUV→RGB conversion hardware to work.
 *
 * @param target GL texture target (typically GL_TEXTURE_EXTERNAL_OES)
 * @param image The EGLImage handle to bind
 */
static void nativeGlEGLImageTargetTexture2DOES(
        JNIEnv *env, jobject thiz, jint target, jlong image) {

    if (!loadExtensions() || glEGLImageTargetTexture2DOES == nullptr) {
        LOGE("glEGLImageTargetTexture2DOES: Extension not available");
        return;
    }

    if (image == 0) {
        LOGE("glEGLImageTargetTexture2DOES: Null image handle");
        return;
    }

    // Validate context for GL operations
    if (validateEGLContext("glEGLImageTargetTexture2DOES") == EGL_NO_DISPLAY) {
        return;
    }

    EGLImageKHR eglImage = reinterpret_cast<EGLImageKHR>(image);

    // Clear any pending GL errors
    while (glGetError() != GL_NO_ERROR) {}

    // Bind the EGLImage to the texture
    glEGLImageTargetTexture2DOES(static_cast<GLenum>(target), eglImage);

    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        LOGE("glEGLImageTargetTexture2DOES: GL error 0x%x", error);

        if (target != GL_TEXTURE_EXTERNAL_OES) {
            LOGW("HINT: For camera frames, use GL_TEXTURE_EXTERNAL_OES (0x%x), not 0x%x",
                 GL_TEXTURE_EXTERNAL_OES, target);
        }
    }
}

/**
 * Imports a native fence file descriptor as an EGLSync object.
 * Used for Fence-First synchronization pattern.
 *
 * @param eglDisplay The EGLDisplay (Java wrapper, we use current display)
 * @param fenceFd The native fence file descriptor
 * @return EGLSync handle as jlong, or 0 on failure
 */
static jlong nativeImportNativeFence(
        JNIEnv *env, jobject thiz, jobject eglDisplay, jint fenceFd) {

    if (!loadExtensions() || eglCreateSyncKHR == nullptr) {
        LOGE("importNativeFence: eglCreateSyncKHR not available");
        return 0;
    }

    EGLDisplay display = validateEGLContext("importNativeFence");
    if (display == EGL_NO_DISPLAY) {
        return 0;
    }

    if (fenceFd < 0) {
        // -1 means no fence, which is valid (no wait needed)
        return 0;
    }

    // Import the native fence FD as an EGL sync object
    // NOTE: EGL takes ownership of the FD after this call
    EGLint attrs[] = {
        EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fenceFd,
        EGL_NONE
    };

    EGLSyncKHR sync = eglCreateSyncKHR(
        display,
        EGL_SYNC_NATIVE_FENCE_ANDROID,
        attrs
    );

    if (sync == EGL_NO_SYNC_KHR) {
        EGLint error = eglGetError();
        LOGE("importNativeFence: eglCreateSyncKHR failed: 0x%x (%s)",
             error, eglErrorString(error));
        return 0;
    }

    LOGI("importNativeFence: Imported fence fd=%d as EGLSync %p", fenceFd, sync);
    return reinterpret_cast<jlong>(sync);
}

/**
 * Destroys an EGLSync object.
 *
 * @param eglDisplay The EGLDisplay (Java wrapper, we use current display)
 * @param syncHandle The EGLSync handle to destroy
 */
static void nativeDestroySync(
        JNIEnv *env, jobject thiz, jobject eglDisplay, jlong syncHandle) {

    if (!loadExtensions() || eglDestroySyncKHR == nullptr) {
        return;
    }

    if (syncHandle == 0) {
        return;  // Nothing to destroy
    }

    EGLDisplay display = validateEGLContext("destroySync");
    if (display == EGL_NO_DISPLAY) {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY) {
            return;
        }
    }

    EGLSyncKHR sync = reinterpret_cast<EGLSyncKHR>(syncHandle);
    eglDestroySyncKHR(display, sync);
}

/**
 * Creates a native fence FD that signals when the GPU has finished
 * processing the current frame. This implements the "Fence-First" rule:
 * the producer (UVC camera) must wait for this fence before overwriting
 * the buffer that the consumer (GL) is reading.
 *
 * @param eglDisplay The EGLDisplay (Java wrapper, we use current display)
 * @return Native fence file descriptor, or -1 on failure
 */
static jint nativeCreateReleaseFence(
        JNIEnv *env, jobject thiz, jobject eglDisplay) {

    if (!loadExtensions()) {
        LOGE("createReleaseFence: Extensions not available");
        return -1;
    }

    if (eglCreateSyncKHR == nullptr || eglDupNativeFenceFDANDROID == nullptr) {
        // Native fence sync not available - this is optional
        LOGW("createReleaseFence: Native fence sync not available (optional)");
        return -1;
    }

    EGLDisplay display = validateEGLContext("createReleaseFence");
    if (display == EGL_NO_DISPLAY) {
        return -1;
    }

    // Create a fence sync that will signal when GPU completes current commands
    // Pass NULL attrs to create a fence at the current point in the command stream
    EGLSyncKHR sync = eglCreateSyncKHR(
        display,
        EGL_SYNC_NATIVE_FENCE_ANDROID,
        nullptr  // No FD to import; create new fence at current point
    );

    if (sync == EGL_NO_SYNC_KHR) {
        EGLint error = eglGetError();
        LOGE("createReleaseFence: eglCreateSyncKHR failed: 0x%x (%s)",
             error, eglErrorString(error));
        return -1;
    }

    // CRITICAL: Flush GL commands to ensure fence is inserted into command stream
    // Without this, the fence might signal before GPU actually starts processing
    glFlush();

    // Duplicate the fence FD so we can return it to Kotlin
    // The sync object still owns its copy; we're creating a new FD
    int fenceFd = eglDupNativeFenceFDANDROID(display, sync);

    if (fenceFd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
        EGLint error = eglGetError();
        LOGE("createReleaseFence: eglDupNativeFenceFDANDROID failed: 0x%x (%s)",
             error, eglErrorString(error));
        eglDestroySyncKHR(display, sync);
        return -1;
    }

    // Destroy the EGL sync object - the native FD is independent now
    eglDestroySyncKHR(display, sync);

    LOGI("createReleaseFence: Created release fence fd=%d", fenceFd);
    return fenceFd;
}

// ═══════════════════════════════════════════════════════════════════════════
// JNI REGISTRATION (Classloader-Safe Pattern)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Registers EGLImageHelper native methods.
 *
 * Uses classloader-safe pattern: if the target class doesn't exist
 * (consumer app doesn't use EGLImage rendering), registration is skipped
 * without failing library load.
 *
 * @param env JNI environment from JNI_OnLoad
 * @return 0 on success (including skip), -1 on registration failure
 */
int register_eglimagehelper(JNIEnv *env) {
    LOGI("Registering EGLImageHelper JNI methods");

    jclass clazz = env->FindClass("com/scopecam/camera/render/EGLImageHelper");
    if (clazz == nullptr) {
        // Class not found - this is OK, consumer app may not use this feature
        LOGW("EGLImageHelper class not found - skipping registration (classloader-safe)");
        env->ExceptionClear();  // Clear the ClassNotFoundException
        return 0;  // Return success - don't fail library load
    }

    static JNINativeMethod methods[] = {
        {"nativeCreateEGLImageFromHardwareBuffer",
         "(Landroid/opengl/EGLDisplay;Landroid/hardware/HardwareBuffer;)J",
         (void*)nativeCreateEGLImageFromHardwareBuffer},
        {"nativeDestroyEGLImage",
         "(Landroid/opengl/EGLDisplay;J)V",
         (void*)nativeDestroyEGLImage},
        {"nativeGlEGLImageTargetTexture2DOES",
         "(IJ)V",
         (void*)nativeGlEGLImageTargetTexture2DOES},
        {"nativeImportNativeFence",
         "(Landroid/opengl/EGLDisplay;I)J",
         (void*)nativeImportNativeFence},
        {"nativeDestroySync",
         "(Landroid/opengl/EGLDisplay;J)V",
         (void*)nativeDestroySync},
        {"nativeCreateReleaseFence",
         "(Landroid/opengl/EGLDisplay;)I",
         (void*)nativeCreateReleaseFence},
    };

    int numMethods = sizeof(methods) / sizeof(methods[0]);
    if (env->RegisterNatives(clazz, methods, numMethods) < 0) {
        LOGE("RegisterNatives failed for EGLImageHelper");
        return -1;
    }

    LOGI("EGLImageHelper: Registered %d native methods", numMethods);
    return 0;
}
