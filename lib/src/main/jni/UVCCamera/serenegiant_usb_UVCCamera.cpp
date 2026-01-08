/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 *
 * File name: serenegiant_usb_UVCCamera.cpp
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
 * Files in the jni/libjpeg, jni/libusb, jin/libuvc, jni/rapidjson folder may have a different license, see the respective files.
*/

#if 1	// デバッグ情報を出さない時
	#ifndef LOG_NDEBUG
		#define	LOG_NDEBUG		// LOGV/LOGD/MARKを出力しない時
		#endif
	#undef USE_LOGALL			// 指定したLOGxだけを出力
#else
	#define USE_LOGALL
	#undef LOG_NDEBUG
	#undef NDEBUG
#endif

#include <jni.h>
#include <android/native_window_jni.h>

#include "libUVCCamera.h"
#include "UVCCamera.h"
#include "FrameBufferRing.h"
#include "HandleManager.h"

/**
 * set the value into the long field
 * @param env: this param should not be null
 * @param bullet_obj: this param should not be null
 * @param field_name
 * @params val
 */
static jlong setField_long(JNIEnv *env, jobject java_obj, const char *field_name, jlong val) {
#if LOCAL_DEBUG
	LOGV("setField_long:");
#endif

	jclass clazz = env->GetObjectClass(java_obj);
	jfieldID field = env->GetFieldID(clazz, field_name, "J");
	if (LIKELY(field))
		env->SetLongField(java_obj, field, val);
	else {
		LOGE("__setField_long:field '%s' not found", field_name);
	}
#ifdef ANDROID_NDK
	env->DeleteLocalRef(clazz);
#endif
	return val;
}

/**
 * @param env: this param should not be null
 * @param bullet_obj: this param should not be null
 */
static jlong __setField_long(JNIEnv *env, jobject java_obj, jclass clazz, const char *field_name, jlong val) {
#if LOCAL_DEBUG
	LOGV("__setField_long:");
#endif

	jfieldID field = env->GetFieldID(clazz, field_name, "J");
	if (LIKELY(field))
		env->SetLongField(java_obj, field, val);
	else {
		LOGE("__setField_long:field '%s' not found", field_name);
	}
	return val;
}

/**
 * @param env: this param should not be null
 * @param bullet_obj: this param should not be null
 */
jint __setField_int(JNIEnv *env, jobject java_obj, jclass clazz, const char *field_name, jint val) {
	LOGV("__setField_int:");

	jfieldID id = env->GetFieldID(clazz, field_name, "I");
	if (LIKELY(id))
		env->SetIntField(java_obj, id, val);
	else {
		LOGE("__setField_int:field '%s' not found", field_name);
		env->ExceptionClear();	// clear java.lang.NoSuchFieldError exception
	}
	return val;
}

/**
 * set the value into int field
 * @param env: this param should not be null
 * @param java_obj: this param should not be null
 * @param field_name
 * @params val
 */
jint setField_int(JNIEnv *env, jobject java_obj, const char *field_name, jint val) {
	LOGV("setField_int:");

	jclass clazz = env->GetObjectClass(java_obj);
	__setField_int(env, java_obj, clazz, field_name, val);
#ifdef ANDROID_NDK
	env->DeleteLocalRef(clazz);
#endif
	return val;
}

static ID_TYPE nativeCreate(JNIEnv *env, jobject thiz) {

	ENTER();
	UVCCamera *camera = new UVCCamera();

	// Register with HandleManager - returns generation-encoded handle
	int64_t handle = getCameraHandleManager().registerContext(camera);
	if (handle == INVALID_HANDLE) {
		LOGE("nativeCreate: failed to register camera handle");
		delete camera;
		RETURN(0, ID_TYPE);
	}

	setField_long(env, thiz, "mNativePtr", handle);
	RETURN(handle, ID_TYPE);
}

// native側のカメラオブジェクトを破棄
static void nativeDestroy(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	setField_long(env, thiz, "mNativePtr", 0);

	// Use HandleManager to safely invalidate and get context
	// This blocks until all active JNI calls using this handle complete
	void* ctx = getCameraHandleManager().invalidateAndFree(id_camera);
	if (ctx) {
		UVCCamera *camera = static_cast<UVCCamera *>(ctx);
		SAFE_DELETE(camera);
	}
	EXIT();
}

//======================================================================
// カメラへ接続
static jint nativeConnect(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera,
	jint vid, jint pid, jint fd,
	jint busNum, jint devAddr, jstring usbfs_str) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		LOGE("nativeConnect: invalid camera handle");
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}

	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	const char *c_usbfs = env->GetStringUTFChars(usbfs_str, JNI_FALSE);
	int result = JNI_ERR;
	if (fd > 0) {
		result = camera->connect(vid, pid, fd, busNum, devAddr, c_usbfs);
	}
	env->ReleaseStringUTFChars(usbfs_str, c_usbfs);
	RETURN(result, jint);
}

/**
 * E1-OPT-B-003: JNI bridge for connectSimple.
 *
 * This is a THIN TRANSLATOR - performs only type marshaling.
 * All business logic (validation, parsing) is in the C++ layer.
 *
 * Implements the 2026 Kotlin-First Hardware Ownership pattern:
 * - Kotlin owns UsbDeviceConnection lifecycle
 * - Native receives FD capability token
 * - Native duplicates FD and owns its copy
 *
 * @param id_camera Native UVCCamera handle (generation-encoded)
 * @param fd File descriptor from UsbDeviceConnection.getFileDescriptor()
 * @param usbfs_str Device path (e.g., "/dev/bus/usb/001/002")
 * @return 0 on success, negative on error
 */
static jint nativeConnectSimple(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint fd, jstring usbfs_str) {

	ENTER();

	// Acquire handle with ScopedRef (blocks destruction during this call)
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		LOGE("nativeConnectSimple: invalid camera handle");
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}

	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);

	// Extract path string
	const char *path = env->GetStringUTFChars(usbfs_str, JNI_FALSE);
	if (UNLIKELY(!path)) {
		LOGE("nativeConnectSimple: failed to get usbfs path string");
		RETURN(-2, jint);
	}

	// Delegate entirely to C++ layer - NO parsing or validation here
	jint result = camera->connectSimple(fd, path);

	// Cleanup JNI string
	env->ReleaseStringUTFChars(usbfs_str, path);

	RETURN(result, jint);
}

// カメラとの接続を解除
static jint nativeRelease(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}

	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int result = camera->release();
	RETURN(result, jint);
}

//======================================================================
static jint nativeSetStatusCallback(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jobject jIStatusCallback) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}

	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	jobject status_callback_obj = env->NewGlobalRef(jIStatusCallback);
	jint result = camera->setStatusCallback(env, status_callback_obj);
	RETURN(result, jint);
}

static jint nativeSetButtonCallback(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jobject jIButtonCallback) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}

	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	jobject button_callback_obj = env->NewGlobalRef(jIButtonCallback);
	jint result = camera->setButtonCallback(env, button_callback_obj);
	RETURN(result, jint);
}

static jint nativeSetReadinessCallback(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jobject jIReadinessCallback) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}

	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	jobject readiness_callback_obj = env->NewGlobalRef(jIReadinessCallback);
	jint result = camera->setReadinessCallback(env, readiness_callback_obj);
	RETURN(result, jint);
}

static jboolean nativeIsReady(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_FALSE, jboolean);
	}

	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	jboolean result = camera->isReady() ? JNI_TRUE : JNI_FALSE;
	RETURN(result, jboolean);
}

static jint nativeCleanup(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint level) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}

	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	jint result = camera->cleanup(static_cast<CleanupLevel>(level));
	RETURN(result, jint);
}

static jint nativeReleaseInterface(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}

	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	jint result = camera->releaseInterface();
	RETURN(result, jint);
}

static jint nativeHardReset(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}

	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	jint result = camera->hardReset();
	RETURN(result, jint);
}

static jobject nativeGetSupportedSize(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(NULL, jobject);
	}

	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	char *c_str = camera->getSupportedSize();
	jstring result = NULL;
	if (LIKELY(c_str)) {
		result = env->NewStringUTF(c_str);
		free(c_str);
	}
	RETURN(result, jobject);
}

//======================================================================
// プレビュー画面の大きさをセット
static jint nativeSetPreviewSize(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint width, jint height, jint min_fps, jint max_fps, jint mode, jfloat bandwidth) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setPreviewSize(width, height, min_fps, max_fps, mode, bandwidth), jint);
}

static jint nativeStartPreview(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	LOGI("FORENSIC-012: nativeStartPreview ENTRY - id_camera=0x%llx",
		 (unsigned long long)id_camera);

	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		LOGE("FORENSIC-012: nativeStartPreview handle validation failed!");
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}

	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	LOGI("FORENSIC-012: Calling camera->startPreview() camera=%p", camera);
	jint result = camera->startPreview();
	LOGI("FORENSIC-012: camera->startPreview() returned %d", result);
	if (result != EXIT_SUCCESS) {
		LOGE("FORENSIC-012: nativeStartPreview failed with code: %d", result);
	}

	RETURN(result, jint);
}

// プレビューを停止
static jint nativeStopPreview(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->stopPreview(), jint);
}

static jint nativeSetPreviewDisplay(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jobject jSurface) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	ANativeWindow *preview_window = jSurface ? ANativeWindow_fromSurface(env, jSurface) : NULL;
	RETURN(camera->setPreviewDisplay(preview_window), jint);
}

static jint nativeSetFrameCallback(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jobject jIFrameCallback, jint pixel_format) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	jobject frame_callback_obj = env->NewGlobalRef(jIFrameCallback);
	RETURN(camera->setFrameCallback(env, frame_callback_obj, pixel_format), jint);
}

static jint nativeSetCaptureDisplay(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jobject jSurface) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	ANativeWindow *capture_window = jSurface ? ANativeWindow_fromSurface(env, jSurface) : NULL;
	RETURN(camera->setCaptureDisplay(capture_window), jint);
}

//======================================================================
// Ring buffer support for decoupled frame streaming (Phase 4)
//======================================================================

/**
 * Enable or disable ring buffer mode.
 * When enabled, frames are written to the ring buffer instead of ANativeWindow.
 * @param use JNI_TRUE to enable, JNI_FALSE to disable
 * @return 0 on success, -1 if ring buffer not allocated
 */
static jint nativeSetUseRingBuffer(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jboolean use) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setUseRingBuffer(use == JNI_TRUE), jint);
}

/**
 * Allocate the ring buffer with specified dimensions.
 * @param width Frame width in pixels
 * @param height Frame height in pixels
 * @return 0 on success, error code on failure
 */
static jint nativeAllocateRingBuffer(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint width, jint height) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->allocateRingBuffer(width, height), jint);
}

/**
 * Destroy the ring buffer and release resources.
 */
static void nativeDestroyRingBuffer(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		EXIT();
		return;
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	camera->destroyRingBuffer();
	EXIT();
}

/**
 * Get the native handle to the ring buffer.
 * This handle is used with the FrameBuffer JNI methods for consumer access.
 * @return Native pointer to FrameBufferRing, or 0 if not allocated
 */
static jlong nativeGetRingBufferHandle(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(0, jlong);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getRingBufferHandle(), jlong);
}

/**
 * Inject an externally-allocated FrameBufferRing into the camera preview.
 * This establishes the "single source of truth" - Kotlin owns the handle,
 * native preview writes to it.
 * @param id_camera Native UVCCamera handle from HandleManager
 * @param ringHandle FrameBufferRing handle from HandleManager
 * @return 0 on success, negative on error
 */
static jint nativeSetFrameBufferRing(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jlong ringHandle) {

	ENTER();

	// Acquire camera handle with ScopedRef
	auto camRef = getCameraHandleManager().acquire(id_camera);
	if (!camRef) {
		LOGE("HANDLE_DIAG: nativeSetFrameBufferRing - invalid camera handle");
		RETURN(-1, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(camRef.ptr);

	// Acquire ring buffer handle with ScopedRef
	auto ringRef = getRingBufferHandleManager().acquire(ringHandle);
	if (!ringRef) {
		LOGE("HANDLE_DIAG: nativeSetFrameBufferRing - invalid ring handle 0x%llx",
			 (unsigned long long)ringHandle);
		RETURN(-2, jint);
	}
	FrameBufferRing *ring = static_cast<FrameBufferRing *>(ringRef.ptr);

	jint result = camera->setFrameBufferRing(ring);

	// Also set telemetry pointer for connection readiness tracking
	// The ring owns the telemetry - UVCCamera just references it
	if (result == 0) {
		StreamTelemetry *telemetry = ring->getTelemetry();
		camera->setTelemetry(telemetry);
		LOGI("HANDLE_DIAG: nativeSetFrameBufferRing also set telemetry=%p", telemetry);
	}

	LOGI("HANDLE_DIAG: nativeSetFrameBufferRing camera=%p ring=%p handle=0x%llx result=%d",
		 camera, ring, (unsigned long long)ringHandle, result);

	RETURN(result, jint);
}

/**
 * Invalidate ring buffer handle without freeing memory.
 * Called from Kotlin when surface is destroyed but USB is still connected.
 * This poisons the magic headers so any stale access fails gracefully.
 */
static void nativeInvalidateRingBufferHandle(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		EXIT();
		return;
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	camera->invalidateRingBufferHandle();
	EXIT();
}

/**
 * Check if ring buffer handle is valid for operations.
 * Returns false if ring buffer is null, poisoned, or corrupt.
 */
static jboolean nativeIsRingBufferValid(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		return JNI_FALSE;
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	return camera->isRingBufferValid() ? JNI_TRUE : JNI_FALSE;
}

//======================================================================
// Telemetry for native layer diagnostics
//======================================================================

/**
 * Get count of frames dropped because surface was not ready
 */
static jlong nativeGetDroppedNoSurface(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		return 0;
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	return static_cast<jlong>(camera->getDroppedNoSurface());
}

/**
 * Get count of frames dropped because queue was full
 */
static jlong nativeGetDroppedQueueFull(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		return 0;
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	return static_cast<jlong>(camera->getDroppedQueueFull());
}

/**
 * Get total count of frames processed through preview
 */
static jlong nativeGetTotalFramesProcessed(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		return 0;
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	return static_cast<jlong>(camera->getTotalFramesProcessed());
}

/**
 * Check if the native surface is ready for rendering
 */
static jboolean nativeIsSurfaceReady(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		return JNI_FALSE;
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	return camera->isSurfaceReady() ? JNI_TRUE : JNI_FALSE;
}

/**
 * Check if the USB file descriptor is still valid
 * Returns false if OS has reclaimed the USB connection
 */
static jboolean nativeIsUsbFdValid(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		return JNI_FALSE;
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	return camera->isUsbFdValid() ? JNI_TRUE : JNI_FALSE;
}

//======================================================================
// Capture Callback JNI Bridge (Dual-Emit Architecture)
//======================================================================

/**
 * Set the capture callback for dual-emit architecture.
 * @param id_camera Camera handle
 * @param callback ICaptureFrameCallback object (or null to clear)
 * @return 0 on success, error code on failure
 */
static jint nativeSetCaptureCallback(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jobject callback) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setCaptureCallback(env, callback), jint);
}

/**
 * Set the pixel format for capture callback.
 * @param id_camera Camera handle
 * @param format CAPTURE_FORMAT_* constant
 * @return 0 on success, error code on failure
 */
static jint nativeSetCaptureFormat(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint format) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setCaptureFormat(format), jint);
}

/**
 * Set target frame rate for capture callback (decimation).
 * @param id_camera Camera handle
 * @param targetFps Target FPS
 * @return 0 on success, error code on failure
 */
static jint nativeSetCaptureFrameRate(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint targetFps) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setCaptureFrameRate(targetFps), jint);
}

/**
 * Enable or disable the capture callback.
 * @param id_camera Camera handle
 * @param enable JNI_TRUE to enable, JNI_FALSE to disable
 * @return 0 on success, error code on failure
 */
static jint nativeEnableCaptureCallback(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jboolean enable) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->enableCaptureCallback(enable == JNI_TRUE), jint);
}

/**
 * Get capture telemetry: frames emitted to callback.
 */
static jlong nativeGetCaptureFramesEmitted(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		return 0;
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	return camera->getCaptureFramesEmitted();
}

/**
 * Get capture telemetry: frames dropped.
 */
static jlong nativeGetCaptureFramesDropped(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		return 0;
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	return camera->getCaptureFramesDropped();
}

/**
 * Get capture telemetry: callback busy count (frames dropped due to slow consumer).
 */
static jlong nativeGetCaptureCallbackBusy(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		return 0;
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	return camera->getCaptureCallbackBusy();
}

/**
 * Get the negotiated preview FPS.
 */
static jint nativeGetPreviewFps(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		return 0;
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	return camera->getPreviewFps();
}

//======================================================================
// カメラコントロールでサポートしている機能を取得する
static jlong nativeGetCtrlSupports(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	jlong result = 0;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(0, jlong);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	uint64_t supports;
	int r = camera->getCtrlSupports(&supports);
	if (!r)
		result = supports;
	RETURN(result, jlong);
}

// プロセッシングユニットでサポートしている機能を取得する
static jlong nativeGetProcSupports(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	jlong result = 0;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(0, jlong);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	uint64_t supports;
	int r = camera->getProcSupports(&supports);
	if (!r)
		result = supports;
	RETURN(result, jlong);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateScanningModeLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateScanningModeLimit(min, max, def);
	if (!result) {
		// Java側へ書き込む
		setField_int(env, thiz, "mScanningModeMin", min);
		setField_int(env, thiz, "mScanningModeMax", max);
		setField_int(env, thiz, "mScanningModeDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetScanningMode(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint scanningMode) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setScanningMode(scanningMode), jint);
}

static jint nativeGetScanningMode(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getScanningMode(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateExposureModeLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateExposureModeLimit(min, max, def);
	if (!result) {
		// Java側へ書き込む
		setField_int(env, thiz, "mExposureModeMin", min);
		setField_int(env, thiz, "mExposureModeMax", max);
		setField_int(env, thiz, "mExposureModeDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetExposureMode(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, int exposureMode) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setExposureMode(exposureMode), jint);
}

static jint nativeGetExposureMode(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getExposureMode(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateExposurePriorityLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateExposurePriorityLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mExposurePriorityMin", min);
		setField_int(env, thiz, "mExposurePriorityMax", max);
		setField_int(env, thiz, "mExposurePriorityDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetExposurePriority(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, int priority) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setExposurePriority(priority), jint);
}

static jint nativeGetExposurePriority(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getExposurePriority(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateExposureLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateExposureLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mExposureMin", min);
		setField_int(env, thiz, "mExposureMax", max);
		setField_int(env, thiz, "mExposureDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetExposure(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, int exposure) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setExposure(exposure), jint);
}

static jint nativeGetExposure(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getExposure(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateExposureRelLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateExposureRelLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mExposureRelMin", min);
		setField_int(env, thiz, "mExposureRelMax", max);
		setField_int(env, thiz, "mExposureRelDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetExposureRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint exposure_rel) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setExposureRel(exposure_rel), jint);
}

static jint nativeGetExposureRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getExposureRel(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateAutoFocusLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateAutoFocusLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mAutoFocusMin", min);
		setField_int(env, thiz, "mAutoFocusMax", max);
		setField_int(env, thiz, "mAutoFocusDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetAutoFocus(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jboolean autofocus) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setAutoFocus(autofocus), jint);
}

static jint nativeGetAutoFocus(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getAutoFocus(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateAutoWhiteBlanceLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateAutoWhiteBlanceLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mAutoWhiteBlanceMin", min);
		setField_int(env, thiz, "mAutoWhiteBlanceMax", max);
		setField_int(env, thiz, "mAutoWhiteBlanceDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetAutoWhiteBlance(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jboolean autofocus) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setAutoWhiteBlance(autofocus), jint);
}

static jint nativeGetAutoWhiteBlance(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getAutoWhiteBlance(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateAutoWhiteBlanceCompoLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateAutoWhiteBlanceCompoLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mAutoWhiteBlanceCompoMin", min);
		setField_int(env, thiz, "mAutoWhiteBlanceCompoMax", max);
		setField_int(env, thiz, "mAutoWhiteBlanceCompoDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetAutoWhiteBlanceCompo(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jboolean autofocus_compo) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setAutoWhiteBlanceCompo(autofocus_compo), jint);
}

static jint nativeGetAutoWhiteBlanceCompo(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getAutoWhiteBlanceCompo(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateBrightnessLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateBrightnessLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mBrightnessMin", min);
		setField_int(env, thiz, "mBrightnessMax", max);
		setField_int(env, thiz, "mBrightnessDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetBrightness(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint brightness) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setBrightness(brightness), jint);
}

static jint nativeGetBrightness(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(0, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getBrightness(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateFocusLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateFocusLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mFocusMin", min);
		setField_int(env, thiz, "mFocusMax", max);
		setField_int(env, thiz, "mFocusDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetFocus(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint focus) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setFocus(focus), jint);
}

static jint nativeGetFocus(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(0, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getFocus(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateFocusRelLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateFocusRelLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mFocusRelMin", min);
		setField_int(env, thiz, "mFocusRelMax", max);
		setField_int(env, thiz, "mFocusRelDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetFocusRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint focus_rel) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setFocusRel(focus_rel), jint);
}

static jint nativeGetFocusRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(0, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getFocusRel(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateIrisLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateIrisLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mIrisMin", min);
		setField_int(env, thiz, "mIrisMax", max);
		setField_int(env, thiz, "mIrisDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetIris(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint iris) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setIris(iris), jint);
}

static jint nativeGetIris(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(0, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getIris(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateIrisRelLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateIrisRelLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mIrisRelMin", min);
		setField_int(env, thiz, "mIrisRelMax", max);
		setField_int(env, thiz, "mIrisRelDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetIrisRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint iris_rel) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setIrisRel(iris_rel), jint);
}

static jint nativeGetIrisRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(0, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getIrisRel(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdatePanLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updatePanLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mPanMin", min);
		setField_int(env, thiz, "mPanMax", max);
		setField_int(env, thiz, "mPanDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetPan(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint pan) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setPan(pan), jint);
}

static jint nativeGetPan(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(0, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getPan(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateTiltLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateTiltLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mTiltMin", min);
		setField_int(env, thiz, "mTiltMax", max);
		setField_int(env, thiz, "mTiltDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetTilt(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint tilt) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setTilt(tilt), jint);
}

static jint nativeGetTilt(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(0, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getTilt(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateRollLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateRollLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mRollMin", min);
		setField_int(env, thiz, "mRollMax", max);
		setField_int(env, thiz, "mRollDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetRoll(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint roll) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setRoll(roll), jint);
}

static jint nativeGetRoll(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(0, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getRoll(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdatePanRelLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updatePanRelLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mPanRelMin", min);
		setField_int(env, thiz, "mPanRelMax", max);
		setField_int(env, thiz, "mPanRelDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetPanRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint pan_rel) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setPanRel(pan_rel), jint);
}

static jint nativeGetPanRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(0, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getPanRel(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateTiltRelLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateTiltRelLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mTiltRelMin", min);
		setField_int(env, thiz, "mTiltRelMax", max);
		setField_int(env, thiz, "mTiltRelDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetTiltRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint tilt_rel) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setTiltRel(tilt_rel), jint);
}

static jint nativeGetTiltRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(0, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getTiltRel(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateRollRelLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	jint result = JNI_ERR;
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	result = camera->updateRollRelLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mRollRelMin", min);
		setField_int(env, thiz, "mRollRelMax", max);
		setField_int(env, thiz, "mRollRelDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetRollRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint roll_rel) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setRollRel(roll_rel), jint);
}

static jint nativeGetRollRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getRollRel(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateContrastLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateContrastLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mContrastMin", min);
		setField_int(env, thiz, "mContrastMax", max);
		setField_int(env, thiz, "mContrastDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetContrast(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint contrast) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setContrast(contrast), jint);
}

static jint nativeGetContrast(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getContrast(), jint);
}

//======================================================================
// Java method correspond to this function should not be a static mathod
static jint nativeUpdateAutoContrastLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateAutoContrastLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mAutoContrastMin", min);
		setField_int(env, thiz, "mAutoContrastMax", max);
		setField_int(env, thiz, "mAutoContrastDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetAutoContrast(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jboolean autocontrast) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setAutoContrast(autocontrast), jint);
}

static jint nativeGetAutoContrast(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getAutoContrast(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateSharpnessLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateSharpnessLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mSharpnessMin", min);
		setField_int(env, thiz, "mSharpnessMax", max);
		setField_int(env, thiz, "mSharpnessDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetSharpness(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint sharpness) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setSharpness(sharpness), jint);
}

static jint nativeGetSharpness(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getSharpness(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateGainLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateGainLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mGainMin", min);
		setField_int(env, thiz, "mGainMax", max);
		setField_int(env, thiz, "mGainDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetGain(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint gain) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setGain(gain), jint);
}

static jint nativeGetGain(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getGain(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateGammaLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateGammaLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mGammaMin", min);
		setField_int(env, thiz, "mGammaMax", max);
		setField_int(env, thiz, "mGammaDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetGamma(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint gamma) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setGamma(gamma), jint);
}

static jint nativeGetGamma(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getGamma(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateWhiteBlanceLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateWhiteBlanceLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mWhiteBlanceMin", min);
		setField_int(env, thiz, "mWhiteBlanceMax", max);
		setField_int(env, thiz, "mWhiteBlanceDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetWhiteBlance(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint whiteBlance) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setWhiteBlance(whiteBlance), jint);
}

static jint nativeGetWhiteBlance(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getWhiteBlance(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateWhiteBlanceCompoLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateWhiteBlanceCompoLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mWhiteBlanceCompoMin", min);
		setField_int(env, thiz, "mWhiteBlanceCompoMax", max);
		setField_int(env, thiz, "mWhiteBlanceCompoDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetWhiteBlanceCompo(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint whiteBlance_compo) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setWhiteBlanceCompo(whiteBlance_compo), jint);
}

static jint nativeGetWhiteBlanceCompo(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getWhiteBlanceCompo(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateBacklightCompLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateBacklightCompLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mBacklightCompMin", min);
		setField_int(env, thiz, "mBacklightCompMax", max);
		setField_int(env, thiz, "mBacklightCompDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetBacklightComp(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint backlight_comp) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setBacklightComp(backlight_comp), jint);
}

static jint nativeGetBacklightComp(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getBacklightComp(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateSaturationLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateSaturationLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mSaturationMin", min);
		setField_int(env, thiz, "mSaturationMax", max);
		setField_int(env, thiz, "mSaturationDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetSaturation(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint saturation) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setSaturation(saturation), jint);
}

static jint nativeGetSaturation(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getSaturation(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateHueLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateHueLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mHueMin", min);
		setField_int(env, thiz, "mHueMax", max);
		setField_int(env, thiz, "mHueDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetHue(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint hue) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setHue(hue), jint);
}

static jint nativeGetHue(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getHue(), jint);
}

//======================================================================
// Java method correspond to this function should not be a static mathod
static jint nativeUpdateAutoHueLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateAutoHueLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mAutoHueMin", min);
		setField_int(env, thiz, "mAutoHueMax", max);
		setField_int(env, thiz, "mAutoHueDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetAutoHue(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jboolean autohue) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setAutoHue(autohue), jint);
}

static jint nativeGetAutoHue(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getAutoHue(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdatePowerlineFrequencyLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updatePowerlineFrequencyLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mPowerlineFrequencyMin", min);
		setField_int(env, thiz, "mPowerlineFrequencyMax", max);
		setField_int(env, thiz, "mPowerlineFrequencyDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetPowerlineFrequency(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint frequency) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setPowerlineFrequency(frequency), jint);
}

static jint nativeGetPowerlineFrequency(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getPowerlineFrequency(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateZoomLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateZoomLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mZoomMin", min);
		setField_int(env, thiz, "mZoomMax", max);
		setField_int(env, thiz, "mZoomDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetZoom(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint zoom) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setZoom(zoom), jint);
}

static jint nativeGetZoom(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getZoom(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateZoomRelLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateZoomRelLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mZoomRelMin", min);
		setField_int(env, thiz, "mZoomRelMax", max);
		setField_int(env, thiz, "mZoomRelDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetZoomRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint zoom_rel) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setZoomRel(zoom_rel), jint);
}

static jint nativeGetZoomRel(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getZoomRel(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateDigitalMultiplierLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateDigitalMultiplierLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mDigitalMultiplierMin", min);
		setField_int(env, thiz, "mDigitalMultiplierMax", max);
		setField_int(env, thiz, "mDigitalMultiplierDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetDigitalMultiplier(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint multiplier) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setDigitalMultiplier(multiplier), jint);
}

static jint nativeGetDigitalMultiplier(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getDigitalMultiplier(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateDigitalMultiplierLimitLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateDigitalMultiplierLimitLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mDigitalMultiplierLimitMin", min);
		setField_int(env, thiz, "mDigitalMultiplierLimitMax", max);
		setField_int(env, thiz, "mDigitalMultiplierLimitDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetDigitalMultiplierLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint multiplier_limit) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setDigitalMultiplierLimit(multiplier_limit), jint);
}

static jint nativeGetDigitalMultiplierLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getDigitalMultiplierLimit(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateAnalogVideoStandardLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateAnalogVideoStandardLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mAnalogVideoStandardMin", min);
		setField_int(env, thiz, "mAnalogVideoStandardMax", max);
		setField_int(env, thiz, "mAnalogVideoStandardDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetAnalogVideoStandard(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint standard) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setAnalogVideoStandard(standard), jint);
}

static jint nativeGetAnalogVideoStandard(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getAnalogVideoStandard(), jint);
}

//======================================================================
// Java mnethod correspond to this function should not be a static mathod
static jint nativeUpdateAnalogVideoLockStateLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updateAnalogVideoLockStateLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mAnalogVideoLockStateMin", min);
		setField_int(env, thiz, "mAnalogVideoLockStateMax", max);
		setField_int(env, thiz, "mAnalogVideoLockStateDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetAnalogVideoLockState(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jint state) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setAnalogVideoLockState(state), jint);
}

static jint nativeGetAnalogVideoLockState(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getAnalogVideoLockState(), jint);
}

//======================================================================
// Java method correspond to this function should not be a static mathod
static jint nativeUpdatePrivacyLimit(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {
	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	int min, max, def;
	jint result = camera->updatePrivacyLimit(min, max, def);
	if (!result) {
		setField_int(env, thiz, "mPrivacyMin", min);
		setField_int(env, thiz, "mPrivacyMax", max);
		setField_int(env, thiz, "mPrivacyDef", def);
	}
	RETURN(result, jint);
}

static jint nativeSetPrivacy(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera, jboolean privacy) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->setPrivacy(privacy ? 1: 0), jint);
}

static jint nativeGetPrivacy(JNIEnv *env, jobject thiz,
	ID_TYPE id_camera) {

	ENTER();
	auto ref = getCameraHandleManager().acquire(id_camera);
	if (!ref) {
		RETURN(JNI_ERR_INVALID_HANDLE, jint);
	}
	UVCCamera *camera = static_cast<UVCCamera *>(ref.ptr);
	RETURN(camera->getPrivacy(), jint);
}

//**********************************************************************
//
//**********************************************************************
jint registerNativeMethods(JNIEnv* env, const char *class_name, JNINativeMethod *methods, int num_methods) {
	int result = 0;

	jclass clazz = env->FindClass(class_name);
	if (LIKELY(clazz)) {
		int result = env->RegisterNatives(clazz, methods, num_methods);
		if (UNLIKELY(result < 0)) {
			LOGE("registerNativeMethods failed(class=%s)", class_name);
		}
	} else {
		LOGE("registerNativeMethods: class'%s' not found", class_name);
	}
	return result;
}

static JNINativeMethod methods[] = {
	{ "nativeCreate",					"()J", (void *) nativeCreate },
	{ "nativeDestroy",					"(J)V", (void *) nativeDestroy },
	//
	{ "nativeConnect",					"(JIIIIILjava/lang/String;)I", (void *) nativeConnect },
	{ "nativeConnectSimple",			"(JILjava/lang/String;)I", (void *) nativeConnectSimple },
	{ "nativeRelease",					"(J)I", (void *) nativeRelease },

	{ "nativeSetStatusCallback",		"(JLcom/serenegiant/usb/IStatusCallback;)I", (void *) nativeSetStatusCallback },
	{ "nativeSetButtonCallback",		"(JLcom/serenegiant/usb/IButtonCallback;)I", (void *) nativeSetButtonCallback },
	{ "nativeSetReadinessCallback",		"(JLcom/serenegiant/usb/IReadinessCallback;)I", (void *) nativeSetReadinessCallback },
	{ "nativeIsReady",					"(J)Z", (void *) nativeIsReady },

	{ "nativeCleanup",					"(JI)I", (void *) nativeCleanup },
	{ "nativeReleaseInterface",			"(J)I", (void *) nativeReleaseInterface },
	{ "nativeHardReset",				"(J)I", (void *) nativeHardReset },

	{ "nativeGetSupportedSize",			"(J)Ljava/lang/String;", (void *) nativeGetSupportedSize },
	{ "nativeSetPreviewSize",			"(JIIIIIF)I", (void *) nativeSetPreviewSize },
	{ "nativeStartPreview",				"(J)I", (void *) nativeStartPreview },
	{ "nativeStopPreview",				"(J)I", (void *) nativeStopPreview },
	{ "nativeSetPreviewDisplay",		"(JLandroid/view/Surface;)I", (void *) nativeSetPreviewDisplay },
	{ "nativeSetFrameCallback",			"(JLcom/serenegiant/usb/IFrameCallback;I)I", (void *) nativeSetFrameCallback },

	{ "nativeSetCaptureDisplay",		"(JLandroid/view/Surface;)I", (void *) nativeSetCaptureDisplay },

	// Ring buffer support (Phase 4)
	{ "nativeSetUseRingBuffer",			"(JZ)I", (void *) nativeSetUseRingBuffer },
	{ "nativeAllocateRingBuffer",		"(JII)I", (void *) nativeAllocateRingBuffer },
	{ "nativeDestroyRingBuffer",		"(J)V", (void *) nativeDestroyRingBuffer },
	{ "nativeGetRingBufferHandle",		"(J)J", (void *) nativeGetRingBufferHandle },
	{ "nativeSetFrameBufferRing",		"(JJ)I", (void *) nativeSetFrameBufferRing },
	{ "nativeInvalidateRingBufferHandle", "(J)V", (void *) nativeInvalidateRingBufferHandle },
	{ "nativeIsRingBufferValid",		"(J)Z", (void *) nativeIsRingBufferValid },

	// Telemetry methods
	{ "nativeGetDroppedNoSurface",		"(J)J", (void *) nativeGetDroppedNoSurface },
	{ "nativeGetDroppedQueueFull",		"(J)J", (void *) nativeGetDroppedQueueFull },
	{ "nativeGetTotalFramesProcessed",	"(J)J", (void *) nativeGetTotalFramesProcessed },
	{ "nativeIsSurfaceReady",			"(J)Z", (void *) nativeIsSurfaceReady },
	{ "nativeIsUsbFdValid",				"(J)Z", (void *) nativeIsUsbFdValid },

	// Capture Callback API (Dual-Emit Architecture)
	{ "nativeSetCaptureCallback",		"(JLcom/serenegiant/usb/ICaptureFrameCallback;)I", (void *) nativeSetCaptureCallback },
	{ "nativeSetCaptureFormat",			"(JI)I", (void *) nativeSetCaptureFormat },
	{ "nativeSetCaptureFrameRate",		"(JI)I", (void *) nativeSetCaptureFrameRate },
	{ "nativeEnableCaptureCallback",	"(JZ)I", (void *) nativeEnableCaptureCallback },
	{ "nativeGetCaptureFramesEmitted",	"(J)J", (void *) nativeGetCaptureFramesEmitted },
	{ "nativeGetCaptureFramesDropped",	"(J)J", (void *) nativeGetCaptureFramesDropped },
	{ "nativeGetCaptureCallbackBusy",	"(J)J", (void *) nativeGetCaptureCallbackBusy },
	{ "nativeGetPreviewFps",			"(J)I", (void *) nativeGetPreviewFps },

	{ "nativeGetCtrlSupports",			"(J)J", (void *) nativeGetCtrlSupports },
	{ "nativeGetProcSupports",			"(J)J", (void *) nativeGetProcSupports },

	{ "nativeUpdateScanningModeLimit",	"(J)I", (void *) nativeUpdateScanningModeLimit },
	{ "nativeSetScanningMode",			"(JI)I", (void *) nativeSetScanningMode },
	{ "nativeGetScanningMode",			"(J)I", (void *) nativeGetScanningMode },
			
	{ "nativeUpdateExposureModeLimit",	"(J)I", (void *) nativeUpdateExposureModeLimit },
	{ "nativeSetExposureMode",			"(JI)I", (void *) nativeSetExposureMode },
	{ "nativeGetExposureMode",			"(J)I", (void *) nativeGetExposureMode },

	{ "nativeUpdateExposurePriorityLimit","(J)I", (void *) nativeUpdateExposurePriorityLimit },
	{ "nativeSetExposurePriority",		"(JI)I", (void *) nativeSetExposurePriority },
	{ "nativeGetExposurePriority",		"(J)I", (void *) nativeGetExposurePriority },
			
	{ "nativeUpdateExposureLimit",		"(J)I", (void *) nativeUpdateExposureLimit },
	{ "nativeSetExposure",				"(JI)I", (void *) nativeSetExposure },
	{ "nativeGetExposure",				"(J)I", (void *) nativeGetExposure },
			
	{ "nativeUpdateExposureRelLimit",	"(J)I", (void *) nativeUpdateExposureRelLimit },
	{ "nativeSetExposureRel",			"(JI)I", (void *) nativeSetExposureRel },
	{ "nativeGetExposureRel",			"(J)I", (void *) nativeGetExposureRel },
			
	{ "nativeUpdateAutoFocusLimit",		"(J)I", (void *) nativeUpdateAutoFocusLimit },
	{ "nativeSetAutoFocus",				"(JZ)I", (void *) nativeSetAutoFocus },
	{ "nativeGetAutoFocus",				"(J)I", (void *) nativeGetAutoFocus },

	{ "nativeUpdateFocusLimit",			"(J)I", (void *) nativeUpdateFocusLimit },
	{ "nativeSetFocus",					"(JI)I", (void *) nativeSetFocus },
	{ "nativeGetFocus",					"(J)I", (void *) nativeGetFocus },

	{ "nativeUpdateFocusRelLimit",		"(J)I", (void *) nativeUpdateFocusRelLimit },
	{ "nativeSetFocusRel",				"(JI)I", (void *) nativeSetFocusRel },
	{ "nativeGetFocusRel",				"(J)I", (void *) nativeGetFocusRel },
	
//	{ "nativeUpdateFocusSimpleLimit",	"(J)I", (void *) nativeUpdateFocusSimpleLimit },
//	{ "nativeSetFocusSimple",			"(JI)I", (void *) nativeSetFocusSimple },
//	{ "nativeGetFocusSimple",			"(J)I", (void *) nativeGetFocusSimple },
			
	{ "nativeUpdateIrisLimit",			"(J)I", (void *) nativeUpdateIrisLimit },
	{ "nativeSetIris",					"(JI)I", (void *) nativeSetIris },
	{ "nativeGetIris",					"(J)I", (void *) nativeGetIris },
	
	{ "nativeUpdateIrisRelLimit",		"(J)I", (void *) nativeUpdateIrisRelLimit },
	{ "nativeSetIrisRel",				"(JI)I", (void *) nativeSetIrisRel },
	{ "nativeGetIrisRel",				"(J)I", (void *) nativeGetIrisRel },
	
	{ "nativeUpdatePanLimit",			"(J)I", (void *) nativeUpdatePanLimit },
	{ "nativeSetPan",					"(JI)I", (void *) nativeSetPan },
	{ "nativeGetPan",					"(J)I", (void *) nativeGetPan },
	
	{ "nativeUpdateTiltLimit",			"(J)I", (void *) nativeUpdateTiltLimit },
	{ "nativeSetTilt",					"(JI)I", (void *) nativeSetTilt },
	{ "nativeGetTilt",					"(J)I", (void *) nativeGetTilt },
	
	{ "nativeUpdateRollLimit",			"(J)I", (void *) nativeUpdateRollLimit },
	{ "nativeSetRoll",					"(JI)I", (void *) nativeSetRoll },
	{ "nativeGetRoll",					"(J)I", (void *) nativeGetRoll },
	
	{ "nativeUpdatePanRelLimit",		"(J)I", (void *) nativeUpdatePanRelLimit },
	{ "nativeSetPanRel",				"(JI)I", (void *) nativeSetPanRel },
	{ "nativeGetPanRel",				"(J)I", (void *) nativeGetPanRel },
	
	{ "nativeUpdateTiltRelLimit",		"(J)I", (void *) nativeUpdateTiltRelLimit },
	{ "nativeSetTiltRel",				"(JI)I", (void *) nativeSetTiltRel },
	{ "nativeGetTiltRel",				"(J)I", (void *) nativeGetTiltRel },
	
	{ "nativeUpdateRollRelLimit",		"(J)I", (void *) nativeUpdateRollRelLimit },
	{ "nativeSetRollRel",				"(JI)I", (void *) nativeSetRollRel },
	{ "nativeGetRollRel",				"(J)I", (void *) nativeGetRollRel },
	
	{ "nativeUpdateAutoWhiteBlanceLimit","(J)I", (void *) nativeUpdateAutoWhiteBlanceLimit },
	{ "nativeSetAutoWhiteBlance",		"(JZ)I", (void *) nativeSetAutoWhiteBlance },
	{ "nativeGetAutoWhiteBlance",		"(J)I", (void *) nativeGetAutoWhiteBlance },

	{ "nativeUpdateAutoWhiteBlanceCompoLimit","(J)I", (void *) nativeUpdateAutoWhiteBlanceCompoLimit },
	{ "nativeSetAutoWhiteBlanceCompo",		"(JZ)I", (void *) nativeSetAutoWhiteBlanceCompo },
	{ "nativeGetAutoWhiteBlanceCompo",		"(J)I", (void *) nativeGetAutoWhiteBlanceCompo },
	
	{ "nativeUpdateWhiteBlanceLimit",	"(J)I", (void *) nativeUpdateWhiteBlanceLimit },
	{ "nativeSetWhiteBlance",			"(JI)I", (void *) nativeSetWhiteBlance },
	{ "nativeGetWhiteBlance",			"(J)I", (void *) nativeGetWhiteBlance },

	{ "nativeUpdateWhiteBlanceCompoLimit","(J)I", (void *) nativeUpdateWhiteBlanceCompoLimit },
	{ "nativeSetWhiteBlanceCompo",		"(JI)I", (void *) nativeSetWhiteBlanceCompo },
	{ "nativeGetWhiteBlanceCompo",		"(J)I", (void *) nativeGetWhiteBlanceCompo },
	
	{ "nativeUpdateBacklightCompLimit",	"(J)I", (void *) nativeUpdateBacklightCompLimit },
	{ "nativeSetBacklightComp",			"(JI)I", (void *) nativeSetBacklightComp },
	{ "nativeGetBacklightComp",			"(J)I", (void *) nativeGetBacklightComp },

	{ "nativeUpdateBrightnessLimit",	"(J)I", (void *) nativeUpdateBrightnessLimit },
	{ "nativeSetBrightness",			"(JI)I", (void *) nativeSetBrightness },
	{ "nativeGetBrightness",			"(J)I", (void *) nativeGetBrightness },

	{ "nativeUpdateContrastLimit",		"(J)I", (void *) nativeUpdateContrastLimit },
	{ "nativeSetContrast",				"(JI)I", (void *) nativeSetContrast },
	{ "nativeGetContrast",				"(J)I", (void *) nativeGetContrast },

	{ "nativeUpdateAutoContrastLimit",	"(J)I", (void *) nativeUpdateAutoContrastLimit },
	{ "nativeSetAutoContrast",			"(JZ)I", (void *) nativeSetAutoContrast },
	{ "nativeGetAutoContrast",			"(J)I", (void *) nativeGetAutoContrast },

	{ "nativeUpdateSharpnessLimit",		"(J)I", (void *) nativeUpdateSharpnessLimit },
	{ "nativeSetSharpness",				"(JI)I", (void *) nativeSetSharpness },
	{ "nativeGetSharpness",				"(J)I", (void *) nativeGetSharpness },

	{ "nativeUpdateGainLimit",			"(J)I", (void *) nativeUpdateGainLimit },
	{ "nativeSetGain",					"(JI)I", (void *) nativeSetGain },
	{ "nativeGetGain",					"(J)I", (void *) nativeGetGain },

	{ "nativeUpdateGammaLimit",			"(J)I", (void *) nativeUpdateGammaLimit },
	{ "nativeSetGamma",					"(JI)I", (void *) nativeSetGamma },
	{ "nativeGetGamma",					"(J)I", (void *) nativeGetGamma },

	{ "nativeUpdateSaturationLimit",	"(J)I", (void *) nativeUpdateSaturationLimit },
	{ "nativeSetSaturation",			"(JI)I", (void *) nativeSetSaturation },
	{ "nativeGetSaturation",			"(J)I", (void *) nativeGetSaturation },

	{ "nativeUpdateHueLimit",			"(J)I", (void *) nativeUpdateHueLimit },
	{ "nativeSetHue",					"(JI)I", (void *) nativeSetHue },
	{ "nativeGetHue",					"(J)I", (void *) nativeGetHue },

	{ "nativeUpdateAutoHueLimit",		"(J)I", (void *) nativeUpdateAutoHueLimit },
	{ "nativeSetAutoHue",				"(JZ)I", (void *) nativeSetAutoHue },
	{ "nativeGetAutoHue",				"(J)I", (void *) nativeGetAutoHue },
			
	{ "nativeUpdatePowerlineFrequencyLimit","(J)I", (void *) nativeUpdatePowerlineFrequencyLimit },
	{ "nativeSetPowerlineFrequency",	"(JI)I", (void *) nativeSetPowerlineFrequency },
	{ "nativeGetPowerlineFrequency",	"(J)I", (void *) nativeGetPowerlineFrequency },

	{ "nativeUpdateZoomLimit",			"(J)I", (void *) nativeUpdateZoomLimit },
	{ "nativeSetZoom",					"(JI)I", (void *) nativeSetZoom },
	{ "nativeGetZoom",					"(J)I", (void *) nativeGetZoom },
	
	{ "nativeUpdateZoomRelLimit",		"(J)I", (void *) nativeUpdateZoomRelLimit },
	{ "nativeSetZoomRel",				"(JI)I", (void *) nativeSetZoomRel },
	{ "nativeGetZoomRel",				"(J)I", (void *) nativeGetZoomRel },
	
	{ "nativeUpdateDigitalMultiplierLimit","(J)I", (void *) nativeUpdateDigitalMultiplierLimit },
	{ "nativeSetDigitalMultiplier","(JI)I", (void *) nativeSetDigitalMultiplier },
	{ "nativeGetDigitalMultiplier","(J)I", (void *) nativeGetDigitalMultiplier },
	
	{ "nativeUpdateDigitalMultiplierLimitLimit","(J)I", (void *) nativeUpdateDigitalMultiplierLimitLimit },
	{ "nativeSetDigitalMultiplierLimit","(JI)I", (void *) nativeSetDigitalMultiplierLimit },
	{ "nativeGetDigitalMultiplierLimit","(J)I", (void *) nativeGetDigitalMultiplierLimit },
	
	{ "nativeUpdateAnalogVideoStandardLimit","(J)I", (void *) nativeUpdateAnalogVideoStandardLimit },
	{ "nativeSetAnalogVideoStandard",		"(JI)I", (void *) nativeSetAnalogVideoStandard },
	{ "nativeGetAnalogVideoStandard",		"(J)I", (void *) nativeGetAnalogVideoStandard },
	
	{ "nativeUpdateAnalogVideoLockStateLimit","(J)I", (void *) nativeUpdateAnalogVideoLockStateLimit },
	{ "nativeSetAnalogVideoLoackState",	"(JI)I", (void *) nativeSetAnalogVideoLockState },
	{ "nativeGetAnalogVideoLoackState",	"(J)I", (void *) nativeGetAnalogVideoLockState },
	
	{ "nativeUpdatePrivacyLimit",		"(J)I", (void *) nativeUpdatePrivacyLimit },
	{ "nativeSetPrivacy",				"(JZ)I", (void *) nativeSetPrivacy },
	{ "nativeGetPrivacy",				"(J)I", (void *) nativeGetPrivacy },
};

int register_uvccamera(JNIEnv *env) {
	LOGV("register_uvccamera:");
	if (registerNativeMethods(env,
		"com/serenegiant/usb/UVCCamera",
		methods, NUM_ARRAY_ELEMENTS(methods)) < 0) {
		return -1;
	}
    return 0;
}
