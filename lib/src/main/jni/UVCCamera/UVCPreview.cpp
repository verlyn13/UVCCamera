/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 *
 * File name: UVCPreview.cpp
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

#include <stdlib.h>
#include <linux/time.h>
#include <unistd.h>

#if 1	// set 1 if you don't need debug log
	#ifndef LOG_NDEBUG
		#define	LOG_NDEBUG		// w/o LOGV/LOGD/MARK
	#endif
	#undef USE_LOGALL
#else
	#define USE_LOGALL
	#undef LOG_NDEBUG
//	#undef NDEBUG
#endif

#include "utilbase.h"
#include "UVCPreview.h"
#include "UVCReadinessCallback.h"
#include "libuvc_internal.h"

#define	LOCAL_DEBUG 0
#define MAX_FRAME 4
#define PREVIEW_PIXEL_BYTES 4	// RGBA/RGBX
#define FRAME_POOL_SZ MAX_FRAME + 2

UVCPreview::UVCPreview(uvc_device_handle_t *devh)
:	mPreviewWindow(NULL),
	mCaptureWindow(NULL),
	mDeviceHandle(devh),
	requestWidth(DEFAULT_PREVIEW_WIDTH),
	requestHeight(DEFAULT_PREVIEW_HEIGHT),
	requestMinFps(DEFAULT_PREVIEW_FPS_MIN),
	requestMaxFps(DEFAULT_PREVIEW_FPS_MAX),
	requestMode(DEFAULT_PREVIEW_MODE),
	requestBandwidth(DEFAULT_BANDWIDTH),
	frameWidth(DEFAULT_PREVIEW_WIDTH),
	frameHeight(DEFAULT_PREVIEW_HEIGHT),
	frameBytes(DEFAULT_PREVIEW_WIDTH * DEFAULT_PREVIEW_HEIGHT * 2),	// YUYV
	frameMode(0),
	previewBytes(DEFAULT_PREVIEW_WIDTH * DEFAULT_PREVIEW_HEIGHT * PREVIEW_PIXEL_BYTES),
	previewFormat(WINDOW_FORMAT_RGBA_8888),
	captureQueu(NULL),
	mFrameCallbackObj(NULL),
	mFrameCallbackFunc(NULL),
	callbackPixelBytes(2),
	mFrameBufferRing(NULL),
	mUseRingBuffer(false),
	mReadinessCallback(NULL) {

	ENTER();
	// Initialize pthread_t members to zero (prevents crashes if stopPreview called before startPreview)
	memset(&preview_thread, 0, sizeof(preview_thread));
	memset(&capture_thread, 0, sizeof(capture_thread));
	memset(&mConversionThread, 0, sizeof(mConversionThread));
	// Use CLOCK_MONOTONIC for condition variables to avoid NTP time change issues
	pthread_condattr_t condattr;
	pthread_condattr_init(&condattr);
	pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC);
	pthread_cond_init(&preview_sync, &condattr);
	pthread_mutex_init(&preview_mutex, NULL);
//
	pthread_cond_init(&capture_sync, &condattr);
	pthread_mutex_init(&capture_mutex, NULL);
	pthread_condattr_destroy(&condattr);
//	
	pthread_mutex_init(&pool_mutex, NULL);
	EXIT();
}

UVCPreview::~UVCPreview() {

	ENTER();
	// CRITICAL: Stop threads before destroying mutexes they may be using
	stopPreview();
	// Stop conversion thread if running
	stopConversionThread();
	// Destroy ring buffer first
	destroyRingBuffer();
	if (mPreviewWindow)
		ANativeWindow_release(mPreviewWindow);
	mPreviewWindow = NULL;
	if (mCaptureWindow)
		ANativeWindow_release(mCaptureWindow);
	mCaptureWindow = NULL;
	clearPreviewFrame();
	clearCaptureFrame();
	clear_pool();
	pthread_mutex_destroy(&preview_mutex);
	pthread_cond_destroy(&preview_sync);
	pthread_mutex_destroy(&capture_mutex);
	pthread_cond_destroy(&capture_sync);
	pthread_mutex_destroy(&pool_mutex);
	EXIT();
}

/**
 * get uvc_frame_t from frame pool
 * if pool is empty, create new frame
 * this function does not confirm the frame size
 * and you may need to confirm the size
 */
uvc_frame_t *UVCPreview::get_frame(size_t data_bytes) {
	uvc_frame_t *frame = NULL;
	pthread_mutex_lock(&pool_mutex);
	{
		if (!mFramePool.isEmpty()) {
			frame = mFramePool.last();
		}
	}
	pthread_mutex_unlock(&pool_mutex);
	if UNLIKELY(!frame) {
		LOGW("allocate new frame");
		frame = uvc_allocate_frame(data_bytes);
	}
	return frame;
}

void UVCPreview::recycle_frame(uvc_frame_t *frame) {
	pthread_mutex_lock(&pool_mutex);
	if (LIKELY(mFramePool.size() < FRAME_POOL_SZ)) {
		mFramePool.put(frame);
		frame = NULL;
	}
	pthread_mutex_unlock(&pool_mutex);
	if (UNLIKELY(frame)) {
		uvc_free_frame(frame);
	}
}


void UVCPreview::init_pool(size_t data_bytes) {
	ENTER();

	clear_pool();
	pthread_mutex_lock(&pool_mutex);
	{
		for (int i = 0; i < FRAME_POOL_SZ; i++) {
			mFramePool.put(uvc_allocate_frame(data_bytes));
		}
	}
	pthread_mutex_unlock(&pool_mutex);

	EXIT();
}

void UVCPreview::clear_pool() {
	ENTER();

	pthread_mutex_lock(&pool_mutex);
	{
		const int n = mFramePool.size();
		for (int i = 0; i < n; i++) {
			uvc_free_frame(mFramePool[i]);
		}
		mFramePool.clear();
	}
	pthread_mutex_unlock(&pool_mutex);
	EXIT();
}

inline const bool UVCPreview::isRunning() const {return mIsRunning.load(std::memory_order_acquire); }

int UVCPreview::setPreviewSize(int width, int height, int min_fps, int max_fps, int mode, float bandwidth) {
	ENTER();
	
	int result = 0;
	if ((requestWidth != width) || (requestHeight != height) || (requestMode != mode)) {
		requestWidth = width;
		requestHeight = height;
		requestMinFps = min_fps;
		requestMaxFps = max_fps;
		requestMode = mode;
		requestBandwidth = bandwidth;

		uvc_stream_ctrl_t ctrl;
		result = uvc_get_stream_ctrl_format_size_fps(mDeviceHandle, &ctrl,
			!requestMode ? UVC_FRAME_FORMAT_YUYV : UVC_FRAME_FORMAT_MJPEG,
			requestWidth, requestHeight, requestMinFps, requestMaxFps);
	}
	
	RETURN(result, int);
}

int UVCPreview::setPreviewDisplay(ANativeWindow *preview_window) {
	ENTER();
	bool surfaceChanged = false;
	pthread_mutex_lock(&preview_mutex);
	{
		if (mPreviewWindow != preview_window) {
			surfaceChanged = true;
			// Mark surface as not ready during transition
			mSurfaceReady.store(false, std::memory_order_release);
			if (mPreviewWindow)
				ANativeWindow_release(mPreviewWindow);
			mPreviewWindow = preview_window;
			if (LIKELY(mPreviewWindow)) {
				ANativeWindow_setBuffersGeometry(mPreviewWindow,
					frameWidth, frameHeight, previewFormat);
				// Surface is now configured and ready
				mSurfaceReady.store(true, std::memory_order_release);
			}
		}
	}
	pthread_mutex_unlock(&preview_mutex);
	// Clear stale frames outside lock to prevent "burst" on resume
	// This prevents old frames from being rendered to new surface
	if (surfaceChanged) {
		clearPreviewFrame();
	}
	RETURN(0, int);
}

int UVCPreview::setFrameCallback(JNIEnv *env, jobject frame_callback_obj, int pixel_format) {
	
	ENTER();
	pthread_mutex_lock(&capture_mutex);
	{
		if (isRunning() && isCapturing()) {
			mIsCapturing.store(false, std::memory_order_release);
			if (mFrameCallbackObj) {
				pthread_cond_signal(&capture_sync);
				pthread_cond_wait(&capture_sync, &capture_mutex);	// wait finishing capturing
			}
		}
		if (!env->IsSameObject(mFrameCallbackObj, frame_callback_obj))	{
			iframecallback_fields.onFrame = NULL;
			if (mFrameCallbackObj) {
				env->DeleteGlobalRef(mFrameCallbackObj);
			}
			mFrameCallbackObj = frame_callback_obj;
			if (frame_callback_obj) {
				// get method IDs of Java object for callback
				jclass clazz = env->GetObjectClass(frame_callback_obj);
				if (LIKELY(clazz)) {
					iframecallback_fields.onFrame = env->GetMethodID(clazz,
						"onFrame",	"(Ljava/nio/ByteBuffer;)V");
				} else {
					LOGW("failed to get object class");
				}
				env->ExceptionClear();
				if (!iframecallback_fields.onFrame) {
					LOGE("Can't find IFrameCallback#onFrame");
					env->DeleteGlobalRef(frame_callback_obj);
					mFrameCallbackObj = frame_callback_obj = NULL;
				}
			}
		}
		if (frame_callback_obj) {
			mPixelFormat = pixel_format;
			callbackPixelFormatChanged();
		}
	}
	pthread_mutex_unlock(&capture_mutex);
	RETURN(0, int);
}

void UVCPreview::callbackPixelFormatChanged() {
	mFrameCallbackFunc = NULL;
	const size_t sz = requestWidth * requestHeight;
	switch (mPixelFormat) {
	  case PIXEL_FORMAT_RAW:
		LOGI("PIXEL_FORMAT_RAW:");
		callbackPixelBytes = sz * 2;
		break;
	  case PIXEL_FORMAT_YUV:
		LOGI("PIXEL_FORMAT_YUV:");
		callbackPixelBytes = sz * 2;
		break;
	  case PIXEL_FORMAT_RGB565:
		LOGI("PIXEL_FORMAT_RGB565:");
		mFrameCallbackFunc = uvc_any2rgb565;
		callbackPixelBytes = sz * 2;
		break;
	  case PIXEL_FORMAT_RGBX:
		LOGI("PIXEL_FORMAT_RGBX:");
		mFrameCallbackFunc = uvc_any2rgbx;
		callbackPixelBytes = sz * 4;
		break;
	  case PIXEL_FORMAT_YUV20SP:
		LOGI("PIXEL_FORMAT_YUV20SP:");
		mFrameCallbackFunc = uvc_yuyv2iyuv420SP;
		callbackPixelBytes = (sz * 3) / 2;
		break;
	  case PIXEL_FORMAT_NV21:
		LOGI("PIXEL_FORMAT_NV21:");
		mFrameCallbackFunc = uvc_yuyv2yuv420SP;
		callbackPixelBytes = (sz * 3) / 2;
		break;
	}
}

void UVCPreview::clearDisplay() {
	ENTER();

	ANativeWindow_Buffer buffer;
	pthread_mutex_lock(&capture_mutex);
	{
		if (LIKELY(mCaptureWindow)) {
			if (LIKELY(ANativeWindow_lock(mCaptureWindow, &buffer, NULL) == 0)) {
				uint8_t *dest = (uint8_t *)buffer.bits;
				const size_t bytes = buffer.width * PREVIEW_PIXEL_BYTES;
				const int stride = buffer.stride * PREVIEW_PIXEL_BYTES;
				for (int i = 0; i < buffer.height; i++) {
					memset(dest, 0, bytes);
					dest += stride;
				}
				ANativeWindow_unlockAndPost(mCaptureWindow);
			}
		}
	}
	pthread_mutex_unlock(&capture_mutex);
	pthread_mutex_lock(&preview_mutex);
	{
		if (LIKELY(mPreviewWindow)) {
			if (LIKELY(ANativeWindow_lock(mPreviewWindow, &buffer, NULL) == 0)) {
				uint8_t *dest = (uint8_t *)buffer.bits;
				const size_t bytes = buffer.width * PREVIEW_PIXEL_BYTES;
				const int stride = buffer.stride * PREVIEW_PIXEL_BYTES;
				for (int i = 0; i < buffer.height; i++) {
					memset(dest, 0, bytes);
					dest += stride;
				}
				ANativeWindow_unlockAndPost(mPreviewWindow);
			}
		}
	}
	pthread_mutex_unlock(&preview_mutex);

	EXIT();
}

int UVCPreview::startPreview() {
	ENTER();
#if LOCAL_DEBUG
	LOGI("FORENSIC-001: startPreview() ENTRY - mIsRunning=%d, mPreviewWindow=%p, mUseRingBuffer=%d",
		 mIsRunning.load(std::memory_order_relaxed), mPreviewWindow, mUseRingBuffer);
#endif

	int result = PREVIEW_ERROR_UNKNOWN;

	if (mIsRunning.load(std::memory_order_acquire)) {
		LOGW("Preview already running");
		RETURN(PREVIEW_ERROR_ALREADY_RUNNING, int);
	}

	// Ring buffer mode validation with HANDLE_DIAG for debugging
	if (mUseRingBuffer && !mFrameBufferRing) {
		LOGE("HANDLE_DIAG: startPreview FAILED - ring buffer mode enabled but no ring buffer!");
		LOGE("HANDLE_DIAG: Call nativeSetFrameBufferRing() before startPreview()");
		RETURN(PREVIEW_ERROR_RING_BUFFER_NOT_ALLOCATED, int);
	}

	// Log the ring buffer state for diagnostics
	if (mUseRingBuffer) {
		LOGI("HANDLE_DIAG: startPreview with ring=%p injected=%s",
			 mFrameBufferRing, mRingBufferInjected ? "true" : "false");
	}

	// Start conversion thread first (if using ring buffer - hybrid architecture)
	if (mUseRingBuffer && mFrameBufferRing) {
		if (startConversionThread() != 0) {
			LOGE("Failed to start conversion thread");
			RETURN(PREVIEW_ERROR_THREAD_CREATE_FAILED, int);
		}
	}

	// Allow thread creation if EITHER window OR ring buffer is ready
	if (LIKELY(mPreviewWindow || mUseRingBuffer)) {
		mIsRunning.store(true, std::memory_order_release);
		pthread_mutex_lock(&preview_mutex);
		{
			result = pthread_create(&preview_thread, NULL, preview_thread_func, (void *)this);
			if (result == EXIT_SUCCESS) {
				mPreviewThreadValid.store(true, std::memory_order_release);
			}
		}
		pthread_mutex_unlock(&preview_mutex);

		if (UNLIKELY(result != EXIT_SUCCESS)) {
			LOGE("pthread_create failed: %d (%s)", result, strerror(result));
			mIsRunning.store(false, std::memory_order_release);
			mPreviewThreadValid.store(false, std::memory_order_release);
			stopConversionThread();  // Clean up conversion thread on failure
			pthread_mutex_lock(&preview_mutex);
			{
				pthread_cond_signal(&preview_sync);
			}
			pthread_mutex_unlock(&preview_mutex);
			RETURN(PREVIEW_ERROR_THREAD_CREATE_FAILED, int);
		}
	} else {
		LOGW("Cannot start preview: no window and ring buffer not enabled");
		RETURN(PREVIEW_ERROR_NO_OUTPUT_TARGET, int);
	}

#if LOCAL_DEBUG
	LOGI("FORENSIC-001: startPreview() EXIT - result=%d, mIsRunning=%d",
		 result, mIsRunning.load(std::memory_order_relaxed));
#endif
	RETURN(result, int);
}

int UVCPreview::stopPreview() {
	ENTER();
	bool wasRunning = mIsRunning.exchange(false, std::memory_order_acq_rel);
	if (LIKELY(wasRunning)) {
		pthread_cond_signal(&preview_sync);
		pthread_cond_signal(&capture_sync);

		// Stop conversion thread first (hybrid architecture)
		stopConversionThread();

		// Only join capture_thread if it was created
		if (mCaptureThreadValid.exchange(false, std::memory_order_acq_rel)) {
			if (pthread_join(capture_thread, NULL) != EXIT_SUCCESS) {
				LOGW("UVCPreview::terminate capture thread: pthread_join failed");
			}
			memset(&capture_thread, 0, sizeof(capture_thread));
		}

		// Only join preview_thread if it was created
		if (mPreviewThreadValid.exchange(false, std::memory_order_acq_rel)) {
			if (pthread_join(preview_thread, NULL) != EXIT_SUCCESS) {
				LOGW("UVCPreview::terminate preview thread: pthread_join failed");
			}
			memset(&preview_thread, 0, sizeof(preview_thread));
		}

		clearDisplay();
	}
	clearPreviewFrame();
	clearCaptureFrame();
	pthread_mutex_lock(&preview_mutex);
	if (mPreviewWindow) {
		ANativeWindow_release(mPreviewWindow);
		mPreviewWindow = NULL;
	}
	pthread_mutex_unlock(&preview_mutex);
	pthread_mutex_lock(&capture_mutex);
	if (mCaptureWindow) {
		ANativeWindow_release(mCaptureWindow);
		mCaptureWindow = NULL;
	}
	pthread_mutex_unlock(&capture_mutex);
	RETURN(0, int);
}

void UVCPreview::forceStop() {
	ENTER();
	// Force stop without joining threads - for hard reset scenarios
	mIsRunning.store(false, std::memory_order_release);
	mIsCapturing.store(false, std::memory_order_release);
	// Wake any blocked threads
	pthread_cond_broadcast(&preview_sync);
	pthread_cond_broadcast(&capture_sync);
	// Mark threads as invalid (skip join in destructor/stopPreview)
	mPreviewThreadValid.store(false, std::memory_order_release);
	mCaptureThreadValid.store(false, std::memory_order_release);
	EXIT();
}

void UVCPreview::setReadinessCallback(UVCReadinessCallback *callback) {
	ENTER();
	mReadinessCallback = callback;
	EXIT();
}

//**********************************************************************
//
//**********************************************************************
void UVCPreview::uvc_preview_frame_callback(uvc_frame_t *frame, void *vptr_args) {
	UVCPreview *preview = reinterpret_cast<UVCPreview *>(vptr_args);

	// FORENSIC-004: Periodic frame callback logging (production-safe)
	static int forensic_frame_count = 0;
	forensic_frame_count++;

#if LOCAL_DEBUG
	// Log first frame, then every 300th (~10 seconds at 30fps)
	if (forensic_frame_count == 1 || forensic_frame_count % 300 == 0) {
		LOGI("FORENSIC-004: Frame #%d - %dx%d, format=%d, bytes=%zu",
			 forensic_frame_count,
			 frame ? frame->width : 0,
			 frame ? frame->height : 0,
			 frame ? frame->frame_format : -1,
			 frame ? frame->data_bytes : 0);
	}
#endif

	// Zero-length frame detection (always enabled - indicates USB issue)
	if (frame && frame->data_bytes == 0) {
		LOGW("FORENSIC-004: ZERO-LENGTH FRAME #%d - USB not delivering data", forensic_frame_count);
	}

	// Fast validation
	if UNLIKELY(!preview->isRunning() || !frame || !frame->frame_format || !frame->data || !frame->data_bytes) return;

	// Dimension/format validation
	if (UNLIKELY(
		((frame->frame_format != UVC_FRAME_FORMAT_MJPEG) && (frame->actual_bytes < preview->frameBytes))
		|| (frame->width != preview->frameWidth) || (frame->height != preview->frameHeight) )) {
#if LOCAL_DEBUG
		LOGD("broken frame!:format=%d,actual_bytes=%d/%d(%d,%d/%d,%d)",
			frame->frame_format, frame->actual_bytes, preview->frameBytes,
			frame->width, frame->height, preview->frameWidth, preview->frameHeight);
#endif
		return;
	}

	// HYBRID PATH: Ring buffer with SPSC queue
	// This path is optimized for minimal USB callback latency (<100μs target)
	if (preview->mUseRingBuffer && preview->mFrameBufferRing) {
		// DIAGNOSTIC: Log first few USB callbacks to confirm frames are arriving
		static int usbCallbackCount = 0;
		if (++usbCallbackCount <= 3) {
			LOGI("PIPELINE_DIAG: USB callback #%d received (format=%d, bytes=%zu, %dx%d)",
				usbCallbackCount, frame->frame_format, frame->data_bytes,
				frame->width, frame->height);
		}

		// Enqueue raw frame data to SPSC queue (single memcpy)
		size_t actualBytes = frame->actual_bytes > 0 ? frame->actual_bytes : frame->data_bytes;
		bool enqueued = preview->mFrameBufferRing->enqueuePendingFrame(
			frame->data,
			actualBytes,
			frame->width,
			frame->height,
			static_cast<int>(frame->frame_format)
		);

		if (enqueued) {
			// Signal conversion thread to wake up
			preview->mFrameBufferRing->signalConversionThread();
		} else if (usbCallbackCount <= 10) {
			LOGW("PIPELINE_DIAG: Frame enqueue FAILED (queue full) at callback #%d", usbCallbackCount);
		}
		// Note: Frame drops are tracked in enqueuePendingFrame via telemetry
		return;  // Done - conversion thread handles the rest
	}

	// LEGACY PATH: Direct queue for ANativeWindow rendering
	// Fast-path drop: if surface isn't ready, drop early
	if (!preview->mSurfaceReady.load(std::memory_order_acquire)) {
		preview->incrementDroppedNoSurface();
		return;
	}

	if (LIKELY(preview->isRunning())) {
		uvc_frame_t *copy = preview->get_frame(frame->data_bytes);
		if (UNLIKELY(!copy)) {
#if LOCAL_DEBUG
			LOGE("uvc_callback:unable to allocate duplicate frame!");
#endif
			return;
		}
		uvc_error_t ret = uvc_duplicate_frame(frame, copy);
		if (UNLIKELY(ret)) {
			preview->recycle_frame(copy);
			return;
		}
		preview->addPreviewFrame(copy);
	}
}

void UVCPreview::addPreviewFrame(uvc_frame_t *frame) {

	pthread_mutex_lock(&preview_mutex);
	if (isRunning() && (previewFrames.size() < MAX_FRAME)) {
		previewFrames.put(frame);
		frame = NULL;
		pthread_cond_signal(&preview_sync);
	} else if (isRunning()) {
		// Queue is full - track this drop
		incrementDroppedQueueFull();
	}
	pthread_mutex_unlock(&preview_mutex);
	if (frame) {
		recycle_frame(frame);
	}
}

uvc_frame_t *UVCPreview::waitPreviewFrame() {
	uvc_frame_t *frame = NULL;
	pthread_mutex_lock(&preview_mutex);
	{
		if (!previewFrames.size() && isRunning()) {
			// Use timed wait to prevent indefinite blocking during shutdown/navigation
			// CLOCK_MONOTONIC avoids issues with NTP time changes
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			ts.tv_nsec += PREVIEW_WAIT_TIMEOUT_MS * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec += 1;
				ts.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&preview_sync, &preview_mutex, &ts);
		}
		if (LIKELY(isRunning() && previewFrames.size() > 0)) {
			frame = previewFrames.remove(0);
		}
	}
	pthread_mutex_unlock(&preview_mutex);
	return frame;
}

void UVCPreview::clearPreviewFrame() {
	pthread_mutex_lock(&preview_mutex);
	{
		for (int i = 0; i < previewFrames.size(); i++)
			recycle_frame(previewFrames[i]);
		previewFrames.clear();
	}
	pthread_mutex_unlock(&preview_mutex);
}

void *UVCPreview::preview_thread_func(void *vptr_args) {
	int result;

	ENTER();
#if LOCAL_DEBUG
	LOGI("FORENSIC-002: Preview thread STARTED - thread_id=%lu", (unsigned long)pthread_self());
#endif

	UVCPreview *preview = reinterpret_cast<UVCPreview *>(vptr_args);
	if (LIKELY(preview)) {
		uvc_stream_ctrl_t ctrl;
		result = preview->prepare_preview(&ctrl);
		LOGI("PIPELINE_DIAG: prepare_preview returned %d", result);
		if (LIKELY(!result)) {
			// Signal readiness - preview thread is running, stopPreview is now safe to call
			if (preview->mReadinessCallback) {
				LOGI("PIPELINE_DIAG: Calling readiness callback");
				preview->mReadinessCallback->notifyReady();
				LOGI("PIPELINE_DIAG: Readiness callback returned, entering do_preview");
			}
			preview->do_preview(&ctrl);
			LOGI("PIPELINE_DIAG: do_preview returned");
		} else {
			LOGE("PIPELINE_DIAG: prepare_preview FAILED with error %d", result);
		}
	} else {
		LOGE("PIPELINE_DIAG: preview_thread_func received NULL preview pointer!");
	}
	PRE_EXIT();
	pthread_exit(NULL);
}

int UVCPreview::prepare_preview(uvc_stream_ctrl_t *ctrl) {
	uvc_error_t result;

	ENTER();
	result = uvc_get_stream_ctrl_format_size_fps(mDeviceHandle, ctrl,
		!requestMode ? UVC_FRAME_FORMAT_YUYV : UVC_FRAME_FORMAT_MJPEG,
		requestWidth, requestHeight, requestMinFps, requestMaxFps
	);
	if (LIKELY(!result)) {
#if LOCAL_DEBUG
		uvc_print_stream_ctrl(ctrl, stderr);
#endif
		uvc_frame_desc_t *frame_desc;
		result = uvc_get_frame_desc(mDeviceHandle, ctrl, &frame_desc);
		if (LIKELY(!result)) {
			frameWidth = frame_desc->wWidth;
			frameHeight = frame_desc->wHeight;
			LOGI("frameSize=(%d,%d)@%s", frameWidth, frameHeight, (!requestMode ? "YUYV" : "MJPEG"));
			pthread_mutex_lock(&preview_mutex);
			if (LIKELY(mPreviewWindow)) {
				ANativeWindow_setBuffersGeometry(mPreviewWindow,
					frameWidth, frameHeight, previewFormat);
			}
			pthread_mutex_unlock(&preview_mutex);
		} else {
			frameWidth = requestWidth;
			frameHeight = requestHeight;
		}
		frameMode = requestMode;
		frameBytes = frameWidth * frameHeight * (!requestMode ? 2 : 4);
		previewBytes = frameWidth * frameHeight * PREVIEW_PIXEL_BYTES;
	} else {
		LOGE("could not negotiate with camera:err=%d", result);
	}
	RETURN(result, int);
}

void UVCPreview::do_preview(uvc_stream_ctrl_t *ctrl) {
	ENTER();

	uvc_frame_t *frame = NULL;
	uvc_frame_t *frame_mjpeg = NULL;

#if LOCAL_DEBUG
	LOGI("FORENSIC-003: Calling uvc_start_streaming - devh=%p, ctrl=%p, callback=%p, bandwidth=%.2f",
		 mDeviceHandle, ctrl, (void*)uvc_preview_frame_callback, requestBandwidth);
#endif

	uvc_error_t result = uvc_start_streaming_bandwidth(
		mDeviceHandle, ctrl, uvc_preview_frame_callback, (void *)this, requestBandwidth, 0);

#if LOCAL_DEBUG
	LOGI("FORENSIC-003: uvc_start_streaming returned %d (%s)", result, uvc_strerror(result));
#endif

	// Log streaming failures (always enabled - critical diagnostic)
	if (result != UVC_SUCCESS) {
		LOGE("FORENSIC-003: STREAMING START FAILED - error=%d (%s)", result, uvc_strerror(result));
		if (mFrameBufferRing) {
			mFrameBufferRing->getTelemetry()->recordError(result, "uvc_stream");
		}
	}

	if (LIKELY(!result)) {
		clearPreviewFrame();
		// Track capture thread creation for safe join
		int createResult = pthread_create(&capture_thread, NULL, capture_thread_func, (void *)this);
		if (createResult == EXIT_SUCCESS) {
			mCaptureThreadValid.store(true, std::memory_order_release);
		} else {
			LOGE("Failed to create capture thread: %d", createResult);
			mCaptureThreadValid.store(false, std::memory_order_release);
		}

#if LOCAL_DEBUG
		LOGI("Streaming...");
#endif
		if (frameMode) {
			// MJPEG mode
			for ( ; LIKELY(isRunning()) ; ) {
				frame_mjpeg = waitPreviewFrame();
				if (LIKELY(frame_mjpeg)) {
					frame = get_frame(frame_mjpeg->width * frame_mjpeg->height * 2);
					result = uvc_mjpeg2yuyv(frame_mjpeg, frame);   // MJPEG => yuyv
					recycle_frame(frame_mjpeg);
					if (LIKELY(!result)) {
						if (mUseRingBuffer) {
							// Ring buffer path: write to AHardwareBuffer
							write_frame_to_ring_buffer(frame, uvc_any2rgbx);
							addCaptureFrame(frame);
						} else {
							// Legacy path: draw directly to ANativeWindow
							frame = draw_preview_one(frame, &mPreviewWindow, uvc_any2rgbx, 4);
							addCaptureFrame(frame);
						}
					} else {
						recycle_frame(frame);
					}
				}
			}
		} else {
			// yuyv mode
			for ( ; LIKELY(isRunning()) ; ) {
				frame = waitPreviewFrame();
				if (LIKELY(frame)) {
					if (mUseRingBuffer) {
						// Ring buffer path: write to AHardwareBuffer
						write_frame_to_ring_buffer(frame, uvc_any2rgbx);
						addCaptureFrame(frame);
					} else {
						// Legacy path: draw directly to ANativeWindow
						frame = draw_preview_one(frame, &mPreviewWindow, uvc_any2rgbx, 4);
						addCaptureFrame(frame);
					}
				}
			}
		}
		pthread_cond_signal(&capture_sync);
#if LOCAL_DEBUG
		LOGI("preview_thread_func:wait for all callbacks complete");
#endif
		uvc_stop_streaming(mDeviceHandle);
#if LOCAL_DEBUG
		LOGI("Streaming finished");
#endif
	} else {
		uvc_perror(result, "failed start_streaming");
	}

	EXIT();
}

static void copyFrame(const uint8_t *src, uint8_t *dest, const int width, int height, const int stride_src, const int stride_dest) {
	const int h8 = height % 8;
	for (int i = 0; i < h8; i++) {
		memcpy(dest, src, width);
		dest += stride_dest; src += stride_src;
	}
	for (int i = 0; i < height; i += 8) {
		memcpy(dest, src, width);
		dest += stride_dest; src += stride_src;
		memcpy(dest, src, width);
		dest += stride_dest; src += stride_src;
		memcpy(dest, src, width);
		dest += stride_dest; src += stride_src;
		memcpy(dest, src, width);
		dest += stride_dest; src += stride_src;
		memcpy(dest, src, width);
		dest += stride_dest; src += stride_src;
		memcpy(dest, src, width);
		dest += stride_dest; src += stride_src;
		memcpy(dest, src, width);
		dest += stride_dest; src += stride_src;
		memcpy(dest, src, width);
		dest += stride_dest; src += stride_src;
	}
}


// transfer specific frame data to the Surface(ANativeWindow)
int copyToSurface(uvc_frame_t *frame, ANativeWindow **window) {
	// ENTER();
	int result = 0;
	if (LIKELY(*window)) {
		ANativeWindow_Buffer buffer;
		if (LIKELY(ANativeWindow_lock(*window, &buffer, NULL) == 0)) {
			// source = frame data
			const uint8_t *src = (uint8_t *)frame->data;
			const int src_w = frame->width * PREVIEW_PIXEL_BYTES;
			const int src_step = frame->width * PREVIEW_PIXEL_BYTES;
			// destination = Surface(ANativeWindow)
			uint8_t *dest = (uint8_t *)buffer.bits;
			const int dest_w = buffer.width * PREVIEW_PIXEL_BYTES;
			const int dest_step = buffer.stride * PREVIEW_PIXEL_BYTES;
			// use lower transfer bytes
			const int w = src_w < dest_w ? src_w : dest_w;
			// use lower height
			const int h = frame->height < buffer.height ? frame->height : buffer.height;
			// transfer from frame data to the Surface
			copyFrame(src, dest, w, h, src_step, dest_step);
			ANativeWindow_unlockAndPost(*window);
		} else {
			result = -1;
		}
	} else {
		result = -1;
	}
	return result; //RETURN(result, int);
}

// changed to return original frame instead of returning converted frame even if convert_func is not null.
// Fixed TOCTOU race: conversion happens outside lock, but check-and-render is atomic under lock.
uvc_frame_t *UVCPreview::draw_preview_one(uvc_frame_t *frame, ANativeWindow **window, convFunc_t convert_func, int pixcelBytes) {
	// ENTER();

	uvc_frame_t *converted = nullptr;
	bool conversionSuccess = false;

	// Convert OUTSIDE lock (CPU intensive work)
	if (convert_func) {
		converted = get_frame(frame->width * frame->height * pixcelBytes);
		if (LIKELY(converted)) {
			if (convert_func(frame, converted) == 0) {
				conversionSuccess = true;
			} else {
				LOGE("failed converting");
				recycle_frame(converted);
				converted = nullptr;
			}
		}
	} else {
		converted = frame;
		conversionSuccess = true;
	}

	// ATOMIC check-and-render under lock - prevents TOCTOU race
	if (conversionSuccess && converted) {
		pthread_mutex_lock(&preview_mutex);
		if (*window != NULL) {
			copyToSurface(converted, window);
			incrementTotalFrames();
		} else {
			incrementDroppedNoSurface();
		}
		pthread_mutex_unlock(&preview_mutex);
	}

	// Clean up converted frame (if we allocated one)
	if (converted && convert_func) {
		recycle_frame(converted);
	}

	return frame; //RETURN(frame, uvc_frame_t *);
}

//======================================================================
//
//======================================================================
inline const bool UVCPreview::isCapturing() const { return mIsCapturing.load(std::memory_order_acquire); }

int UVCPreview::setCaptureDisplay(ANativeWindow *capture_window) {
	ENTER();
	pthread_mutex_lock(&capture_mutex);
	{
		if (isRunning() && isCapturing()) {
			mIsCapturing.store(false, std::memory_order_release);
			if (mCaptureWindow) {
				pthread_cond_signal(&capture_sync);
				pthread_cond_wait(&capture_sync, &capture_mutex);	// wait finishing capturing
			}
		}
		if (mCaptureWindow != capture_window) {
			// release current Surface if already assigned.
			if (UNLIKELY(mCaptureWindow))
				ANativeWindow_release(mCaptureWindow);
			mCaptureWindow = capture_window;
			// if you use Surface came from MediaCodec#createInputSurface
			// you could not change window format at least when you use
			// ANativeWindow_lock / ANativeWindow_unlockAndPost
			// to write frame data to the Surface...
			// So we need check here.
			if (mCaptureWindow) {
				ANativeWindow_setBuffersGeometry(mCaptureWindow,
					frameWidth, frameHeight, previewFormat);
				int32_t window_format = ANativeWindow_getFormat(mCaptureWindow);
				if ((window_format != WINDOW_FORMAT_RGB_565)
					&& (previewFormat == WINDOW_FORMAT_RGB_565)) {
					LOGE("window format mismatch, cancelled movie capturing.");
					ANativeWindow_release(mCaptureWindow);
					mCaptureWindow = NULL;
				}
			}
		}
	}
	pthread_mutex_unlock(&capture_mutex);
	RETURN(0, int);
}

void UVCPreview::addCaptureFrame(uvc_frame_t *frame) {
	pthread_mutex_lock(&capture_mutex);
	if (LIKELY(isRunning())) {
		// keep only latest one
		if (captureQueu) {
			recycle_frame(captureQueu);
		}
		captureQueu = frame;
		pthread_cond_broadcast(&capture_sync);
	}
	pthread_mutex_unlock(&capture_mutex);
}

/**
 * get frame data for capturing, if not exist, block and wait
 */
uvc_frame_t *UVCPreview::waitCaptureFrame() {
	uvc_frame_t *frame = NULL;
	pthread_mutex_lock(&capture_mutex);
	{
		if (!captureQueu) {
			pthread_cond_wait(&capture_sync, &capture_mutex);
		}
		if (LIKELY(isRunning() && captureQueu)) {
			frame = captureQueu;
			captureQueu = NULL;
		}
	}
	pthread_mutex_unlock(&capture_mutex);
	return frame;
}

/**
 * clear drame data for capturing
 */
void UVCPreview::clearCaptureFrame() {
	pthread_mutex_lock(&capture_mutex);
	{
		if (captureQueu)
			recycle_frame(captureQueu);
		captureQueu = NULL;
	}
	pthread_mutex_unlock(&capture_mutex);
}

//======================================================================
/*
 * thread function
 * @param vptr_args pointer to UVCPreview instance
 */
// static
void *UVCPreview::capture_thread_func(void *vptr_args) {
	int result;

	ENTER();
	UVCPreview *preview = reinterpret_cast<UVCPreview *>(vptr_args);
	if (LIKELY(preview)) {
		JavaVM *vm = getVM();
		JNIEnv *env;
		// attach to JavaVM
		vm->AttachCurrentThread(&env, NULL);
		preview->do_capture(env);	// never return until finish previewing
		// detach from JavaVM
		vm->DetachCurrentThread();
		MARK("DetachCurrentThread");
	}
	PRE_EXIT();
	pthread_exit(NULL);
}

/**
 * the actual function for capturing
 */
void UVCPreview::do_capture(JNIEnv *env) {

	ENTER();

	clearCaptureFrame();
	callbackPixelFormatChanged();
	for (; isRunning() ;) {
		mIsCapturing.store(true, std::memory_order_release);
		if (mCaptureWindow) {
			do_capture_surface(env);
		} else {
			do_capture_idle_loop(env);
		}
		pthread_cond_broadcast(&capture_sync);
	}	// end of for (; isRunning() ;)
	EXIT();
}

void UVCPreview::do_capture_idle_loop(JNIEnv *env) {
	ENTER();
	
	for (; isRunning() && isCapturing() ;) {
		do_capture_callback(env, waitCaptureFrame());
	}
	
	EXIT();
}

/**
 * write frame data to Surface for capturing
 */
void UVCPreview::do_capture_surface(JNIEnv *env) {
	ENTER();

	uvc_frame_t *frame = NULL;
	uvc_frame_t *converted = NULL;
	char *local_picture_path;

	for (; isRunning() && isCapturing() ;) {
		frame = waitCaptureFrame();
		if (LIKELY(frame)) {
			// frame data is always YUYV format.
			if LIKELY(isCapturing()) {
				if (UNLIKELY(!converted)) {
					converted = get_frame(previewBytes);
				}
				if (LIKELY(converted)) {
					int b = uvc_any2rgbx(frame, converted);
					if (!b) {
						if (LIKELY(mCaptureWindow)) {
							copyToSurface(converted, &mCaptureWindow);
						}
					}
				}
			}
			do_capture_callback(env, frame);
		}
	}
	if (converted) {
		recycle_frame(converted);
	}
	if (mCaptureWindow) {
		ANativeWindow_release(mCaptureWindow);
		mCaptureWindow = NULL;
	}

	EXIT();
}

/**
* call IFrameCallback#onFrame if needs
 */
void UVCPreview::do_capture_callback(JNIEnv *env, uvc_frame_t *frame) {
	ENTER();
	pthread_mutex_lock(&capture_mutex);
	if (LIKELY(frame)) {
		uvc_frame_t *callback_frame = frame;
		if (mFrameCallbackObj) {
			if (mFrameCallbackFunc) {
				callback_frame = get_frame(callbackPixelBytes);
				if (LIKELY(callback_frame)) {
					int b = mFrameCallbackFunc(frame, callback_frame);
					recycle_frame(frame);
					if (UNLIKELY(b)) {
						LOGW("failed to convert for callback frame");
						goto SKIP;
					}
				} else {
					LOGW("failed to allocate for callback frame");
					callback_frame = frame;
					goto SKIP;
				}
			}
			jobject buf = env->NewDirectByteBuffer(callback_frame->data, callbackPixelBytes);
			env->CallVoidMethod(mFrameCallbackObj, iframecallback_fields.onFrame, buf);
			env->ExceptionClear();
			env->DeleteLocalRef(buf);
		}
 SKIP:
		recycle_frame(callback_frame);
	}
	pthread_mutex_unlock(&capture_mutex);
	EXIT();
}

//======================================================================
// Ring buffer support for decoupled frame streaming
//======================================================================

/**
 * Enable or disable ring buffer mode.
 * When enabled, frames are written to the ring buffer instead of ANativeWindow.
 * The ring buffer must be allocated before enabling.
 * @param use true to enable ring buffer mode
 * @return 0 on success, -1 if ring buffer not allocated
 */
int UVCPreview::setUseRingBuffer(bool use) {
	ENTER();
	if (use && !mFrameBufferRing) {
		LOGE("Cannot enable ring buffer mode: not allocated");
		RETURN(-1, int);
	}
	mUseRingBuffer = use;
	LOGI("Ring buffer mode: %s", use ? "enabled" : "disabled");
	RETURN(0, int);
}

/**
 * Allocate the frame buffer ring with the specified dimensions.
 * Uses AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM format.
 * If a ring buffer was injected via setFrameBufferRing(), this method
 * validates dimensions and reuses the injected buffer.
 * @param width Frame width in pixels
 * @param height Frame height in pixels
 * @return 0 on success, error code on failure
 */
int UVCPreview::allocateRingBuffer(int width, int height) {
	ENTER();

	// CRITICAL: If ring buffer was injected, don't allocate a new one
	if (mRingBufferInjected && mFrameBufferRing) {
		LOGW("HANDLE_DIAG: allocateRingBuffer called but ring already injected ring=%p", mFrameBufferRing);

		// Verify dimensions match
		if (mFrameBufferRing->getWidth() != static_cast<uint32_t>(width) ||
			mFrameBufferRing->getHeight() != static_cast<uint32_t>(height)) {
			LOGE("HANDLE_DIAG: Dimension mismatch! injected=%dx%d requested=%dx%d",
				 mFrameBufferRing->getWidth(), mFrameBufferRing->getHeight(), width, height);
			RETURN(-3, int);
		}

		LOGI("HANDLE_DIAG: Reusing injected ring buffer (dimensions match)");
		RETURN(0, int);
	}

	// Destroy existing self-allocated ring buffer if any
	if (mFrameBufferRing && !mRingBufferInjected) {
		mFrameBufferRing->destroy();
		delete mFrameBufferRing;
		mFrameBufferRing = NULL;
	}

	mFrameBufferRing = new FrameBufferRing();
	if (!mFrameBufferRing) {
		LOGE("Failed to create FrameBufferRing");
		RETURN(-1, int);
	}

	int result = mFrameBufferRing->allocate(
		static_cast<uint32_t>(width),
		static_cast<uint32_t>(height),
		AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM
	);

	if (result != 0) {
		LOGE("Failed to allocate ring buffer: %d", result);
		delete mFrameBufferRing;
		mFrameBufferRing = NULL;
		RETURN(result, int);
	}

	mRingBufferInjected = false;  // Self-allocated, we own it
	LOGI("HANDLE_DIAG: allocateRingBuffer SELF-ALLOCATED ring=%p %dx%d", mFrameBufferRing, width, height);
	RETURN(0, int);
}

/**
 * Destroy the frame buffer ring and release all resources.
 * Automatically disables ring buffer mode.
 * For injected ring buffers, only clears the pointer (external ownership).
 */
void UVCPreview::destroyRingBuffer() {
	ENTER();
	mUseRingBuffer = false;

	if (mFrameBufferRing) {
		if (mRingBufferInjected) {
			// External ownership - just clear pointer, don't delete
			LOGI("HANDLE_DIAG: destroyRingBuffer clearing injected ring=%p (not deleting)", mFrameBufferRing);
			mFrameBufferRing = NULL;
		} else {
			// Self-allocated - destroy and delete
			LOGI("HANDLE_DIAG: destroyRingBuffer destroying self-allocated ring=%p", mFrameBufferRing);
			mFrameBufferRing->destroy();
			delete mFrameBufferRing;
			mFrameBufferRing = NULL;
		}
	}

	mRingBufferInjected = false;
	EXIT();
}

/**
 * Get the frame buffer ring pointer for JNI access.
 * @return FrameBufferRing pointer, or NULL if not allocated
 */
FrameBufferRing* UVCPreview::getFrameBufferRing() {
	return mFrameBufferRing;
}

/**
 * Inject an externally-allocated FrameBufferRing.
 * MUST be called before startPreview() when using ring buffer mode.
 * OWNERSHIP: UVCPreview does NOT take ownership - caller is responsible for lifecycle.
 * @param ring Pointer to allocated FrameBufferRing
 * @return 0 on success, -1 if preview already running, -2 if ring is null, -3 if conversion thread running
 */
int UVCPreview::setFrameBufferRing(FrameBufferRing *ring) {
	ENTER();

	if (mIsRunning.load(std::memory_order_acquire)) {
		LOGE("HANDLE_DIAG: setFrameBufferRing failed - preview already running");
		RETURN(-1, int);
	}

	// Also check conversion thread
	if (mConversionThreadRunning.load(std::memory_order_acquire)) {
		LOGE("HANDLE_DIAG: setFrameBufferRing failed - conversion thread still running");
		RETURN(-3, int);
	}

	if (!ring) {
		LOGE("HANDLE_DIAG: setFrameBufferRing failed - null ring");
		RETURN(-2, int);
	}

	// If we had a self-allocated ring buffer, destroy it
	if (mFrameBufferRing && !mRingBufferInjected) {
		LOGW("HANDLE_DIAG: Destroying self-allocated ring buffer before injection");
		mFrameBufferRing->destroy();
		delete mFrameBufferRing;
	}

	mFrameBufferRing = ring;
	mRingBufferInjected = true;
	mUseRingBuffer = true;

	LOGI("HANDLE_DIAG: setFrameBufferRing injected ring=%p (external ownership)", mFrameBufferRing);
	RETURN(0, int);
}

/**
 * Write a frame to the ring buffer after converting to RGBA.
 * This replaces the ANativeWindow path when ring buffer mode is enabled.
 * @param frame Source frame (YUYV or other format)
 * @param convert_func Conversion function (e.g., uvc_any2rgbx)
 */
void UVCPreview::write_frame_to_ring_buffer(uvc_frame_t *frame, convFunc_t convert_func) {
	if (UNLIKELY(!mFrameBufferRing || !mFrameBufferRing->isAllocated())) {
		return;
	}

	int32_t strideBytes = 0;
	uint8_t *destPtr = static_cast<uint8_t*>(mFrameBufferRing->lockWriteBuffer(&strideBytes));
	if (UNLIKELY(!destPtr)) {
		LOGW("Failed to lock ring buffer for write");
		return;
	}

	// Convert frame to RGBA and write directly to ring buffer
	if (convert_func) {
		// Create a temporary frame structure pointing to the ring buffer
		uvc_frame_t dest_frame;
		dest_frame.data = destPtr;
		dest_frame.data_bytes = mFrameBufferRing->getWidth() * mFrameBufferRing->getHeight() * 4;
		dest_frame.width = mFrameBufferRing->getWidth();
		dest_frame.height = mFrameBufferRing->getHeight();
		dest_frame.frame_format = UVC_FRAME_FORMAT_RGBX;
		dest_frame.step = strideBytes;  // Use actual stride from ring buffer
		dest_frame.library_owns_data = 0;

		uvc_error_t result = convert_func(frame, &dest_frame);
		if (UNLIKELY(result != UVC_SUCCESS)) {
			LOGW("Failed to convert frame for ring buffer: %d", result);
			// Cancel the write - unlocks buffer without committing
			mFrameBufferRing->cancelWriteBuffer();
			return;
		}
	} else {
		// Direct copy if no conversion needed (rare case)
		size_t copyBytes = frame->width * frame->height * 4;
		if (copyBytes <= mFrameBufferRing->getWidth() * mFrameBufferRing->getHeight() * 4) {
			memcpy(destPtr, frame->data, copyBytes);
		}
	}

	mFrameBufferRing->unlockWriteBuffer();
}

//======================================================================
// Telemetry methods
//======================================================================

void UVCPreview::incrementDroppedNoSurface() {
	mDroppedNoSurface.fetch_add(1, std::memory_order_relaxed);
}

void UVCPreview::incrementDroppedQueueFull() {
	mDroppedQueueFull.fetch_add(1, std::memory_order_relaxed);
}

void UVCPreview::incrementTotalFrames() {
	mTotalFramesProcessed.fetch_add(1, std::memory_order_relaxed);
}

uint64_t UVCPreview::getDroppedNoSurface() const {
	return mDroppedNoSurface.load(std::memory_order_relaxed);
}

uint64_t UVCPreview::getDroppedQueueFull() const {
	return mDroppedQueueFull.load(std::memory_order_relaxed);
}

uint64_t UVCPreview::getTotalFramesProcessed() const {
	return mTotalFramesProcessed.load(std::memory_order_relaxed);
}

bool UVCPreview::isSurfaceReady() const {
	return mSurfaceReady.load(std::memory_order_acquire);
}

//======================================================================
// Conversion Thread Implementation (Hybrid Architecture)
//======================================================================

/**
 * Start the conversion thread for hybrid frame processing.
 * The conversion thread dequeues raw frames from SPSC queue,
 * performs MJPEG/YUYV to RGBX conversion, and writes to AHardwareBuffer.
 * @return 0 on success, -1 on failure
 */
int UVCPreview::startConversionThread() {
	ENTER();

	if (mConversionThreadValid.load(std::memory_order_acquire)) {
		LOGW("Conversion thread already running");
		RETURN(0, int);
	}

	mConversionThreadRunning.store(true, std::memory_order_release);

	int result = pthread_create(&mConversionThread, NULL,
	                            conversion_thread_func, (void*)this);

	if (result != 0) {
		LOGE("Failed to create conversion thread: %d (%s)", result, strerror(result));
		mConversionThreadRunning.store(false, std::memory_order_release);
		RETURN(-1, int);
	}

	mConversionThreadValid.store(true, std::memory_order_release);

	// Set high priority for conversion thread (below USB callback, above render)
	struct sched_param param;
	param.sched_priority = sched_get_priority_max(SCHED_FIFO) - 2;
	if (pthread_setschedparam(mConversionThread, SCHED_FIFO, &param) != 0) {
		LOGW("Failed to set conversion thread priority (non-fatal)");
	}

	LOGI("Conversion thread started");
	RETURN(0, int);
}

/**
 * Stop the conversion thread.
 * Signals the thread to exit and waits for it to complete.
 */
void UVCPreview::stopConversionThread() {
	ENTER();

	mConversionThreadRunning.store(false, std::memory_order_release);

	// Signal thread to wake up and exit
	if (mFrameBufferRing) {
		mFrameBufferRing->signalConversionThread();
	}

	if (mConversionThreadValid.exchange(false, std::memory_order_acq_rel)) {
		pthread_join(mConversionThread, NULL);
		memset(&mConversionThread, 0, sizeof(mConversionThread));
		LOGI("Conversion thread stopped");
	}

	EXIT();
}

/**
 * Conversion thread entry point (static).
 */
void *UVCPreview::conversion_thread_func(void *vptr_args) {
	ENTER();

	UVCPreview *preview = reinterpret_cast<UVCPreview*>(vptr_args);
	if (LIKELY(preview)) {
		preview->do_conversion_loop();
	}

	PRE_EXIT();
	pthread_exit(NULL);
}

/**
 * Main conversion loop.
 * Dequeues raw frames from SPSC queue, converts to RGBX, writes to ring buffer.
 * Tracks in-pipe latency for telemetry.
 */
void UVCPreview::do_conversion_loop() {
	ENTER();

	if (!mFrameBufferRing) {
		LOGE("HANDLE_DIAG: do_conversion_loop - ring buffer is NULL!");
		EXIT();
		return;
	}

	// Log ring buffer pointer at thread start for correlation
	LOGI("HANDLE_DIAG: Conversion thread started with ring=%p injected=%s",
		 mFrameBufferRing, mRingBufferInjected ? "true" : "false");

	StreamTelemetry* telemetry = mFrameBufferRing->getTelemetry();
	uvc_frame_t temp_yuyv;
	memset(&temp_yuyv, 0, sizeof(temp_yuyv));
	temp_yuyv.data = nullptr;
	size_t temp_yuyv_capacity = 0;

	// DIAGNOSTIC: Periodic counter for telemetry dumps
	int64_t lastDiagDumpNs = StreamTelemetry::getCurrentTimeNs();
	constexpr int64_t DIAG_INTERVAL_NS = 5000000000LL;  // 5 seconds

	LOGI("Conversion loop starting");

	while (mConversionThreadRunning.load(std::memory_order_acquire)) {
		// DIAGNOSTIC: Periodic telemetry dump every 5 seconds
		int64_t nowNs = StreamTelemetry::getCurrentTimeNs();
		if (nowNs - lastDiagDumpNs >= DIAG_INTERVAL_NS) {
			lastDiagDumpNs = nowNs;
			LOGI("PIPELINE_DIAG @5s: framesReceived=%llu, droppedQueueFull=%llu, consumerStarves=%llu, corrupted=%llu",
				(unsigned long long)telemetry->framesReceived.load(std::memory_order_relaxed),
				(unsigned long long)telemetry->framesDroppedQueueFull.load(std::memory_order_relaxed),
				(unsigned long long)telemetry->consumerStarves.load(std::memory_order_relaxed),
				(unsigned long long)telemetry->framesCorrupted.load(std::memory_order_relaxed));
		}

		// Try to dequeue a pending frame
		PendingFrame* pending = mFrameBufferRing->dequeuePendingFrame();

		if (!pending) {
			// Wait for signal with timeout
			mFrameBufferRing->waitForSignal(100);  // 100ms timeout
			continue;
		}

		// Timestamp conversion start
		int64_t convStartNs = StreamTelemetry::getCurrentTimeNs();

		// Lock destination AHardwareBuffer
		int32_t strideBytes = 0;
		uint8_t* destPtr = static_cast<uint8_t*>(
			mFrameBufferRing->lockWriteBuffer(&strideBytes));

		if (UNLIKELY(!destPtr)) {
			LOGW("Failed to lock ring buffer for write");
			telemetry->producerStalls.fetch_add(1, std::memory_order_relaxed);
			mFrameBufferRing->completePendingFrame(pending);
			continue;
		}

		// Build source frame descriptor
		uvc_frame_t src_frame;
		memset(&src_frame, 0, sizeof(src_frame));
		src_frame.data = pending->data;
		src_frame.data_bytes = pending->dataBytes;
		src_frame.actual_bytes = pending->dataBytes;
		src_frame.width = pending->width;
		src_frame.height = pending->height;
		src_frame.frame_format = static_cast<uvc_frame_format>(pending->frameFormat);

		// Build destination frame descriptor
		uvc_frame_t dest_frame;
		memset(&dest_frame, 0, sizeof(dest_frame));
		dest_frame.data = destPtr;
		dest_frame.width = mFrameBufferRing->getWidth();
		dest_frame.height = mFrameBufferRing->getHeight();
		dest_frame.frame_format = UVC_FRAME_FORMAT_RGBX;
		dest_frame.step = strideBytes;
		dest_frame.data_bytes = dest_frame.width * dest_frame.height * 4;
		dest_frame.library_owns_data = 0;

		// Perform conversion based on source format
		uvc_error_t result;

		if (pending->frameFormat == UVC_FRAME_FORMAT_MJPEG) {
			// MJPEG: First decode to YUYV, then convert to RGBX
			size_t needed = pending->width * pending->height * 2;
			if (temp_yuyv_capacity < needed) {
				void* newBuf = realloc(temp_yuyv.data, needed);
				if (newBuf) {
					temp_yuyv.data = newBuf;
					temp_yuyv_capacity = needed;
				} else {
					LOGE("Failed to allocate temp YUYV buffer");
					mFrameBufferRing->cancelWriteBuffer();
					mFrameBufferRing->completePendingFrame(pending);
					continue;
				}
			}
			temp_yuyv.width = pending->width;
			temp_yuyv.height = pending->height;
			temp_yuyv.data_bytes = needed;
			temp_yuyv.frame_format = UVC_FRAME_FORMAT_YUYV;

			result = uvc_mjpeg2yuyv(&src_frame, &temp_yuyv);
			if (result == UVC_SUCCESS) {
				result = uvc_any2rgbx(&temp_yuyv, &dest_frame);
			}
		} else {
			// YUYV or other: direct conversion to RGBX
			result = uvc_any2rgbx(&src_frame, &dest_frame);
		}

		// Timestamp conversion end
		int64_t convEndNs = StreamTelemetry::getCurrentTimeNs();

		if (LIKELY(result == UVC_SUCCESS)) {
			mFrameBufferRing->unlockWriteBuffer();

			// Record latency metrics (microseconds)
			int64_t inPipeLatencyUs = (convEndNs - pending->callbackTimestampNs) / 1000;
			int64_t conversionTimeUs = (convEndNs - convStartNs) / 1000;
			telemetry->recordInPipeLatency(inPipeLatencyUs, conversionTimeUs);

			// HANDLE_DIAG: Log successful writes for pointer correlation
			static std::atomic<int> writeCount{0};
			int wc = ++writeCount;
			if (wc <= 10 || wc % 1000 == 0) {
				int latest = mFrameBufferRing->getLatestCompleted();
				LOGI("HANDLE_DIAG: Producer write[%d] ring=%p latest=%d",
					 wc, mFrameBufferRing, latest);
			}

			// DIAGNOSTIC: Log first few successful conversions
			static int convSuccessCount = 0;
			if (++convSuccessCount <= 3) {
				LOGI("PIPELINE_DIAG: Conversion #%d SUCCESS (latency=%lldμs, conv=%lldμs)",
					convSuccessCount, (long long)inPipeLatencyUs, (long long)conversionTimeUs);
			}

		} else {
			mFrameBufferRing->cancelWriteBuffer();
			telemetry->framesCorrupted.fetch_add(1, std::memory_order_relaxed);
			LOGW("Frame conversion failed: %d", result);
		}

		mFrameBufferRing->completePendingFrame(pending);
	}

	// Cleanup
	if (temp_yuyv.data) {
		free(temp_yuyv.data);
	}

	LOGI("HANDLE_DIAG: Conversion thread exiting (ring=%p)", mFrameBufferRing);
	EXIT();
}
