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

// SIGSEGV diagnostic infrastructure - static member initialization
std::atomic<uint32_t> UVCPreview::sInstanceCounter{0};

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
	// NOTE: mFrameBufferRing, mUseRingBuffer, mRingBufferInjected are now std::atomic
	// and are initialized in the header with default values (nullptr, false, false)
	mReadinessCallback(NULL) {

	ENTER();
	// SIGSEGV diagnostic: assign unique instance ID
	mInstanceId = ++sInstanceCounter;
	LOGI("INSTANCE_DIAG: UVCPreview instance %u created at %p", mInstanceId, this);

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
	// Initialize capture callback mutex
	pthread_mutex_init(&mCaptureBufferMutex, NULL);
	// Initialize WARM state frame stash mutex (Phase 2)
	pthread_mutex_init(&mWarmFrameMutex, NULL);
	EXIT();
}

UVCPreview::~UVCPreview() {

	ENTER();
	LOGI("LIFECYCLE: ~UVCPreview() instance=%u", mInstanceId);

	// CRITICAL: Stop threads before destroying mutexes they may be using
	stopPreview();
	// Stop conversion thread if running
	stopConversionThread();
	// Clear ring buffer with Signal-Drain-Destroy protocol
	// (may already be cleared by stopPreview, but clearRingBuffer is re-entrant safe)
	clearRingBuffer();
	// Free capture callback resources
	freeCaptureBuffer();
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
	pthread_mutex_destroy(&mCaptureBufferMutex);
	// Clean up WARM state resources (Phase 2)
	pthread_mutex_lock(&mWarmFrameMutex);
	if (mLastWarmFrame) {
		recycle_frame(mLastWarmFrame);
		mLastWarmFrame = nullptr;
	}
	pthread_mutex_unlock(&mWarmFrameMutex);
	pthread_mutex_destroy(&mWarmFrameMutex);
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

	// Use atomic loads for ring buffer state
	bool useRing = mUseRingBuffer.load(std::memory_order_acquire);
	FrameBufferRing* ring = mFrameBufferRing.load(std::memory_order_acquire);
	bool injected = mRingBufferInjected.load(std::memory_order_acquire);

#if LOCAL_DEBUG
	LOGI("FORENSIC-001: startPreview() ENTRY - mIsRunning=%d, mPreviewWindow=%p, mUseRingBuffer=%d",
		 mIsRunning.load(std::memory_order_relaxed), mPreviewWindow, (int)useRing);
#endif

	int result = PREVIEW_ERROR_UNKNOWN;

	if (mIsRunning.load(std::memory_order_acquire)) {
		LOGW("Preview already running");
		RETURN(PREVIEW_ERROR_ALREADY_RUNNING, int);
	}

	// Ring buffer mode validation with HANDLE_DIAG for debugging
	if (useRing && !ring) {
		LOGE("HANDLE_DIAG: startPreview FAILED - ring buffer mode enabled but no ring buffer!");
		LOGE("HANDLE_DIAG: Call nativeSetFrameBufferRing() before startPreview()");
		RETURN(PREVIEW_ERROR_RING_BUFFER_NOT_ALLOCATED, int);
	}

	// Log the ring buffer state for diagnostics
	if (useRing) {
		LOGI("HANDLE_DIAG: startPreview with ring=%p injected=%s",
			 ring, injected ? "true" : "false");
	}

	// Start conversion thread first (if using ring buffer - hybrid architecture)
	if (useRing && ring) {
		if (startConversionThread() != 0) {
			LOGE("Failed to start conversion thread");
			RETURN(PREVIEW_ERROR_THREAD_CREATE_FAILED, int);
		}
	}

	// Allow thread creation if EITHER window OR ring buffer is ready
	if (LIKELY(mPreviewWindow || useRing)) {
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
	LOGI("LIFECYCLE: stopPreview() called");

	bool wasRunning = mIsRunning.exchange(false, std::memory_order_acq_rel);
	if (LIKELY(wasRunning)) {
		pthread_cond_signal(&preview_sync);
		pthread_cond_signal(&capture_sync);

		// CRITICAL: Clear ring buffer state BEFORE stopping threads
		// This ensures callbacks exit cleanly before we join threads
		clearRingBuffer();

		// Stop conversion thread (hybrid architecture)
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

	// ========== ATOMIC RING BUFFER PATH CHECK ==========
	// Load flag with acquire semantics to see if ring buffer mode is active
	bool useRing = preview->mUseRingBuffer.load(std::memory_order_acquire);

	// HYBRID PATH: Ring buffer with SPSC queue
	// This path is optimized for minimal USB callback latency (<100μs target)
	if (useRing) {
		// ========== RAII CALLBACK GUARD (P0 FIX) ==========
		// CRITICAL: Increment mCallbacksInFlight BEFORE any ring buffer access.
		// This allows clearRingBuffer() to wait for all callbacks to complete.
		CallbackGuard guard(preview->mCallbacksInFlight);

		// ========== ATOMIC LOADS WITH ACQUIRE SEMANTICS ==========
		// Cache pointer into local variable (critical for thread safety)
		FrameBufferRing* ring = preview->mFrameBufferRing.load(std::memory_order_acquire);
		bool injected = preview->mRingBufferInjected.load(std::memory_order_acquire);

		// Fast-path rejection if ring buffer not ready
		if (!injected || ring == nullptr) {
			return;  // Guard destructor will decrement mCallbacksInFlight
		}

		static std::atomic<int> usbCallbackCount{0};
		int callNum = ++usbCallbackCount;
		pthread_t current_thread = pthread_self();

		// ========== SIGSEGV DIAGNOSTIC: PRE-DEREFERENCE LOGGING ==========
		// Log first 5 callbacks for initial debugging, then every 500th
		if (callNum <= 5 || callNum % 500 == 0) {
			LOGI("CRASH_DIAG[%d]: ╔══════════════════════════════════════════════════╗", callNum);
			LOGI("CRASH_DIAG[%d]: ║         USB CALLBACK - PRE-DEREFERENCE           ║", callNum);
			LOGI("CRASH_DIAG[%d]: ╚══════════════════════════════════════════════════╝", callNum);

			// Raw pointer capture - using locally cached ring pointer
			void* raw_preview = (void*)preview;
			void* raw_ring = (void*)ring;
			void* raw_original = (void*)preview->mFrameBufferRingOriginal;

			LOGI("CRASH_DIAG[%d]: callback_thread=%lu injection_thread=%lu SAME=%s",
				 callNum,
				 (unsigned long)current_thread,
				 (unsigned long)preview->mInjectionThreadId,
				 (current_thread == preview->mInjectionThreadId) ? "YES" : "NO-CROSS-THREAD");

			LOGI("CRASH_DIAG[%d]: preview=%p instance=%u inFlight=%d",
				 callNum, raw_preview, preview->mInstanceId,
				 preview->mCallbacksInFlight.load(std::memory_order_relaxed));

			// Pointer analysis (MTE tags only on 64-bit)
			uintptr_t ring_addr = (uintptr_t)raw_ring;
			uintptr_t orig_addr = (uintptr_t)raw_original;
#if __SIZEOF_POINTER__ == 8
			uint8_t ring_tag = (uint8_t)(ring_addr >> 56);
			uint8_t orig_tag = (uint8_t)(orig_addr >> 56);
#else
			uint8_t ring_tag = 0;
			uint8_t orig_tag = 0;
#endif

			LOGI("CRASH_DIAG[%d]: mFrameBufferRing=%p (MTE_tag=0x%02x)",
				 callNum, raw_ring, ring_tag);
			LOGI("CRASH_DIAG[%d]: mFrameBufferRingOriginal=%p (MTE_tag=0x%02x)",
				 callNum, raw_original, orig_tag);
			LOGI("CRASH_DIAG[%d]: mRingBufferInjected=%d (atomic)",
				 callNum, (int)injected);

			// Corruption detection
			if (raw_ring != raw_original) {
				LOGE("CRASH_DIAG[%d]: *** POINTER MISMATCH DETECTED ***", callNum);
				LOGE("CRASH_DIAG[%d]: current=%p vs original=%p delta=%lld bytes",
					 callNum, raw_ring, raw_original,
					 (long long)((uintptr_t)raw_ring - (uintptr_t)raw_original));
			}

			if (ring_tag == 0xb4) {
				LOGI("CRASH_DIAG[%d]: MTE/HWASan tagged pointer detected (0xb4 prefix)", callNum);
				LOGI("CRASH_DIAG[%d]: Untagged address would be: %p",
					 callNum, (void*)(ring_addr & 0x00FFFFFFFFFFFFFF));
			}

			// Safe dereference with field-by-field logging (using cached 'ring')
			LOGI("CRASH_DIAG[%d]: Calling isAllocated()...", callNum);
			bool is_alloc = ring->isAllocated();
			LOGI("CRASH_DIAG[%d]: isAllocated() returned %d", callNum, (int)is_alloc);

			LOGI("CRASH_DIAG[%d]: Calling getWidth()...", callNum);
			uint32_t width = ring->getWidth();
			LOGI("CRASH_DIAG[%d]: getWidth() returned %u", callNum, width);

			LOGI("CRASH_DIAG[%d]: Calling getHeight()...", callNum);
			uint32_t height = ring->getHeight();
			LOGI("CRASH_DIAG[%d]: getHeight() returned %u", callNum, height);

			LOGI("CRASH_DIAG[%d]: Ring buffer validated successfully", callNum);
			LOGI("CRASH_DIAG[%d]: Frame: format=%d bytes=%zu dims=%dx%d",
				 callNum, frame->frame_format, frame->data_bytes,
				 frame->width, frame->height);

			LOGI("CRASH_DIAG[%d]: ═══ CALLING enqueuePendingFrame() ═══", callNum);
		}

		// Enqueue raw frame data to SPSC queue (single memcpy)
		// Use locally cached 'ring' pointer for all operations
		size_t actualBytes = frame->actual_bytes > 0 ? frame->actual_bytes : frame->data_bytes;
		bool enqueued = ring->enqueuePendingFrame(
			frame->data,
			actualBytes,
			frame->width,
			frame->height,
			static_cast<int>(frame->frame_format)
		);

		if (callNum <= 5) {
			LOGI("CRASH_DIAG[%d]: enqueuePendingFrame returned %d", callNum, (int)enqueued);
		}

		if (enqueued) {
			// Signal conversion thread to wake up
			ring->signalConversionThread();
		} else if (callNum <= 10) {
			LOGW("PIPELINE_DIAG: Frame enqueue FAILED (queue full) at callback #%d", callNum);
		}
		// Note: Frame drops are tracked in enqueuePendingFrame via telemetry
		return;  // Done - conversion thread handles the rest
		         // Guard destructor will decrement mCallbacksInFlight
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

		// Update preview FPS for capture decimation
		if (ctrl->dwFrameInterval > 0) {
			int fps = 10000000 / ctrl->dwFrameInterval;
			mPreviewFps.store(fps, std::memory_order_release);
			LOGI("prepare_preview: Configured FPS = %d", fps);
		}
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
		FrameBufferRing* ring = mFrameBufferRing.load(std::memory_order_acquire);
		if (ring) {
			ring->getTelemetry()->recordError(result, "uvc_stream");
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

		// Set initial state based on surface availability (Phase 2)
		if (mPreviewWindow) {
			mPreviewState.store(PreviewState::HOT, std::memory_order_release);
			LOGI("WARM_STATE: Starting in HOT state (surface available)");
		} else {
			mPreviewState.store(PreviewState::WARM, std::memory_order_release);
			LOGI("WARM_STATE: Starting in WARM state (no surface)");
		}

#if LOCAL_DEBUG
		LOGI("Streaming...");
#endif
		if (frameMode) {
			// MJPEG mode
			for ( ; LIKELY(isRunning()) ; ) {
				// ========== SURFACE SWAP HANDSHAKE (Phase 2) ==========
				// Check if surface swap is requested - park thread if so
				if (mSwappingSurface.load(std::memory_order_acquire)) {
					mIsRenderIdle.store(true, std::memory_order_release);
					mRenderThreadIdleCond.notify_one();

					// Wait for swap to complete
					std::unique_lock<std::mutex> lock(mSwapMutex);
					mSwappingCond.wait(lock, [this]{
						return !mSwappingSurface.load(std::memory_order_acquire);
					});
					mIsRenderIdle.store(false, std::memory_order_release);
				}

				// Check current preview state
				PreviewState currentState = mPreviewState.load(std::memory_order_acquire);

				frame_mjpeg = waitPreviewFrame();
				if (LIKELY(frame_mjpeg)) {
					if (currentState == PreviewState::WARM) {
						// ========== WARM PATH: Active Drain (Phase 2) ==========
						// Drain frame without conversion/rendering to save CPU
						// Keep latest for instant resume when surface returns
						incrementTotalFrames();
						incrementDroppedNoSurface();

						pthread_mutex_lock(&mWarmFrameMutex);
						if (mLastWarmFrame) {
							recycle_frame(mLastWarmFrame);
						}
						mLastWarmFrame = frame_mjpeg;  // Stash for instant resume
						pthread_mutex_unlock(&mWarmFrameMutex);
						// Don't recycle - kept for later use

					} else {
						// ========== HOT PATH: Normal Rendering ==========
						// Check if we have a stashed frame from WARM state
						pthread_mutex_lock(&mWarmFrameMutex);
						if (mLastWarmFrame) {
							// We just transitioned HOT - recycle stashed frame
							// (current frame is more recent)
							recycle_frame(mLastWarmFrame);
							mLastWarmFrame = nullptr;
						}
						pthread_mutex_unlock(&mWarmFrameMutex);

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
			}
		} else {
			// yuyv mode
			for ( ; LIKELY(isRunning()) ; ) {
				// ========== SURFACE SWAP HANDSHAKE (Phase 2) ==========
				// Check if surface swap is requested - park thread if so
				if (mSwappingSurface.load(std::memory_order_acquire)) {
					mIsRenderIdle.store(true, std::memory_order_release);
					mRenderThreadIdleCond.notify_one();

					// Wait for swap to complete
					std::unique_lock<std::mutex> lock(mSwapMutex);
					mSwappingCond.wait(lock, [this]{
						return !mSwappingSurface.load(std::memory_order_acquire);
					});
					mIsRenderIdle.store(false, std::memory_order_release);
				}

				// Check current preview state
				PreviewState currentState = mPreviewState.load(std::memory_order_acquire);

				frame = waitPreviewFrame();
				if (LIKELY(frame)) {
					if (currentState == PreviewState::WARM) {
						// ========== WARM PATH: Active Drain (Phase 2) ==========
						// Drain frame without conversion/rendering to save CPU
						// Keep latest for instant resume when surface returns
						incrementTotalFrames();
						incrementDroppedNoSurface();

						pthread_mutex_lock(&mWarmFrameMutex);
						if (mLastWarmFrame) {
							recycle_frame(mLastWarmFrame);
						}
						mLastWarmFrame = frame;  // Stash for instant resume
						pthread_mutex_unlock(&mWarmFrameMutex);
						// Don't recycle - kept for later use

					} else {
						// ========== HOT PATH: Normal Rendering ==========
						// Check if we have a stashed frame from WARM state
						pthread_mutex_lock(&mWarmFrameMutex);
						if (mLastWarmFrame) {
							// We just transitioned HOT - recycle stashed frame
							// (current frame is more recent)
							recycle_frame(mLastWarmFrame);
							mLastWarmFrame = nullptr;
						}
						pthread_mutex_unlock(&mWarmFrameMutex);

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
		}
		pthread_cond_signal(&capture_sync);
#if LOCAL_DEBUG
		LOGI("preview_thread_func:wait for all callbacks complete");
#endif
		uvc_stop_streaming(mDeviceHandle);

		// Clean up any stashed WARM frame on shutdown
		pthread_mutex_lock(&mWarmFrameMutex);
		if (mLastWarmFrame) {
			recycle_frame(mLastWarmFrame);
			mLastWarmFrame = nullptr;
		}
		pthread_mutex_unlock(&mWarmFrameMutex);

		// Return to COLD state
		mPreviewState.store(PreviewState::COLD, std::memory_order_release);

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
	if (use && !mFrameBufferRing.load(std::memory_order_acquire)) {
		LOGE("Cannot enable ring buffer mode: not allocated");
		RETURN(-1, int);
	}
	mUseRingBuffer.store(use, std::memory_order_release);
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

	// Use atomic loads for reading current state
	FrameBufferRing* currentRing = mFrameBufferRing.load(std::memory_order_acquire);
	bool wasInjected = mRingBufferInjected.load(std::memory_order_acquire);

	// CRITICAL: If ring buffer was injected, don't allocate a new one
	if (wasInjected && currentRing) {
		LOGW("HANDLE_DIAG: allocateRingBuffer called but ring already injected ring=%p", currentRing);

		// Verify dimensions match
		if (currentRing->getWidth() != static_cast<uint32_t>(width) ||
			currentRing->getHeight() != static_cast<uint32_t>(height)) {
			LOGE("HANDLE_DIAG: Dimension mismatch! injected=%dx%d requested=%dx%d",
				 currentRing->getWidth(), currentRing->getHeight(), width, height);
			RETURN(-3, int);
		}

		LOGI("HANDLE_DIAG: Reusing injected ring buffer (dimensions match)");
		RETURN(0, int);
	}

	// Destroy existing self-allocated ring buffer if any
	if (currentRing && !wasInjected) {
		currentRing->destroy();
		delete currentRing;
	}

	FrameBufferRing* newRing = new FrameBufferRing();
	if (!newRing) {
		LOGE("Failed to create FrameBufferRing");
		mFrameBufferRing.store(nullptr, std::memory_order_release);
		RETURN(-1, int);
	}

	int result = newRing->allocate(
		static_cast<uint32_t>(width),
		static_cast<uint32_t>(height),
		AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM
	);

	if (result != 0) {
		LOGE("Failed to allocate ring buffer: %d", result);
		delete newRing;
		mFrameBufferRing.store(nullptr, std::memory_order_release);
		RETURN(result, int);
	}

	// Atomic stores
	mFrameBufferRing.store(newRing, std::memory_order_release);
	mRingBufferInjected.store(false, std::memory_order_release);  // Self-allocated, we own it
	LOGI("HANDLE_DIAG: allocateRingBuffer SELF-ALLOCATED ring=%p %dx%d", newRing, width, height);
	RETURN(0, int);
}

/**
 * Destroy the frame buffer ring and release all resources.
 * Automatically disables ring buffer mode.
 * For injected ring buffers, only clears the pointer (external ownership).
 * NOTE: This is the LEGACY destroy method. For thread-safe destruction,
 * use clearRingBuffer() which implements Signal-Drain-Destroy protocol.
 */
void UVCPreview::destroyRingBuffer() {
	ENTER();
	mUseRingBuffer.store(false, std::memory_order_release);

	FrameBufferRing* currentRing = mFrameBufferRing.load(std::memory_order_acquire);
	bool wasInjected = mRingBufferInjected.load(std::memory_order_acquire);

	if (currentRing) {
		if (wasInjected) {
			// External ownership - just clear pointer, don't delete
			LOGI("HANDLE_DIAG: destroyRingBuffer clearing injected ring=%p (not deleting)", currentRing);
			mFrameBufferRing.store(nullptr, std::memory_order_release);
		} else {
			// Self-allocated - destroy and delete
			LOGI("HANDLE_DIAG: destroyRingBuffer destroying self-allocated ring=%p", currentRing);
			mFrameBufferRing.store(nullptr, std::memory_order_release);
			currentRing->destroy();
			delete currentRing;
		}
	}

	mRingBufferInjected.store(false, std::memory_order_release);
	EXIT();
}

/**
 * Thread-safe ring buffer cleanup using "Signal, Drain, and Destroy" protocol.
 *
 * This method safely clears the ring buffer by:
 * 1. SIGNAL: Setting flags to stop new callbacks from using the ring buffer
 * 2. DRAIN: Waiting for all in-flight callbacks to complete
 * 3. DESTROY: Safely deleting the ring buffer after all access is complete
 *
 * The callback drain has a 100ms timeout to prevent indefinite blocking.
 * This method is re-entrant safe and handles double-call scenarios.
 */
void UVCPreview::clearRingBuffer() {
	// ========== RE-ENTRANCY GUARD ==========
	// Prevents multiple simultaneous or sequential calls from causing issues.
	// Uses exchange to ensure only one caller proceeds.
	bool alreadyClearing = mClearingRingBuffer.exchange(true, std::memory_order_acq_rel);
	if (alreadyClearing) {
		LOGI("LIFECYCLE: clearRingBuffer() already in progress, skipping");
		return;
	}

	// Use RAII to reset the flag on all exit paths
	struct ClearGuard {
		std::atomic<bool>& flag;
		~ClearGuard() { flag.store(false, std::memory_order_release); }
	} clearGuard{mClearingRingBuffer};

	LOGI("LIFECYCLE: clearRingBuffer() - beginning teardown");

	// ========== STEP 0: STOP NEW CALLBACKS FROM STARTING ==========
	// Set the flags to make new callbacks exit immediately
	// BEFORE they increment mCallbacksInFlight.
	// This prevents the drain phase from being a "moving target."
	bool wasUsingRing = mUseRingBuffer.exchange(false, std::memory_order_acq_rel);
	mRingBufferInjected.store(false, std::memory_order_release);

	if (!wasUsingRing) {
		LOGI("LIFECYCLE: Ring buffer was not active, nothing to drain");
		// Still clear the pointer in case of partial state
		FrameBufferRing* oldRing = mFrameBufferRing.exchange(nullptr,
			std::memory_order_acq_rel);
		if (oldRing) {
			LOGI("LIFECYCLE: Deleting orphaned ring buffer %p", oldRing);
			delete oldRing;
		}
		// Clear diagnostic state
		mFrameBufferRingOriginal = nullptr;
		mInjectionThreadId = 0;
		LOGI("LIFECYCLE: clearRingBuffer() complete (fast path)");
		return;
	}

	LOGI("LIFECYCLE: Flags cleared, signaling shutdown");

	// ========== STEP 1: THE "FENCE OF DOOM" ==========
	// Full barrier ensures all stores are visible to other threads
	std::atomic_thread_fence(std::memory_order_seq_cst);

	// ========== STEP 2: DRAIN IN-FLIGHT CALLBACKS ==========
	int currentInFlight = mCallbacksInFlight.load(std::memory_order_acquire);
	LOGI("LIFECYCLE: Draining %d in-flight callbacks...", currentInFlight);

	int retryCount = 0;
	const int maxRetries = 500;      // 500 * 200us = 100ms max wait
	const int retryDelayUs = 200;

	while (mCallbacksInFlight.load(std::memory_order_acquire) > 0) {
		if (retryCount >= maxRetries) {
			int remaining = mCallbacksInFlight.load(std::memory_order_acquire);
			LOGW("LIFECYCLE: Callback drain TIMEOUT! %d callbacks still in flight.", remaining);
			LOGW("LIFECYCLE: Proceeding with destruction - potential use-after-free risk!");
			break;
		}
		usleep(retryDelayUs);
		retryCount++;
	}

	if (retryCount > 0 && retryCount < maxRetries) {
		LOGI("LIFECYCLE: Callback drain complete after %d retries (%.1fms)",
			 retryCount, (retryCount * retryDelayUs) / 1000.0f);
	}

	// ========== STEP 3: SAFE POINTER EXCHANGE ==========
	FrameBufferRing* oldRing = mFrameBufferRing.exchange(nullptr, std::memory_order_acq_rel);

	// ========== STEP 4: TELEMETRY DUMP & EXPLICIT DESTRUCTION ==========
	if (oldRing != nullptr) {
		// Dump final telemetry before destruction (aids post-mortem analysis)
		StreamTelemetry* telemetry = oldRing->getTelemetry();
		if (telemetry) {
			LOGI("LIFECYCLE: Final telemetry before deletion:");
			LOGI("LIFECYCLE:   framesReceived=%llu framesDropped=%llu",
				 (unsigned long long)telemetry->framesReceived.load(std::memory_order_relaxed),
				 (unsigned long long)telemetry->framesDroppedQueueFull.load(std::memory_order_relaxed));
			LOGI("LIFECYCLE:   avgInPipeLatencyUs=%lld peakInPipeLatencyUs=%lld",
				 (long long)telemetry->avgInPipeLatencyUs.load(std::memory_order_relaxed),
				 (long long)telemetry->peakInPipeLatencyUs.load(std::memory_order_relaxed));
		}

		// ========== POISON BEFORE DELETE ==========
		// This makes use-after-free immediately identifiable in logs.
		// If Kotlin sees MAGIC_POISON (0xDEADD00D), we know it's accessing freed memory
		// (as opposed to random garbage from reallocation).
		oldRing->poisonMagicHeaders();

		LOGI("LIFECYCLE: Deleting FrameBufferRing %p", oldRing);
		delete oldRing;
		LOGI("LIFECYCLE: FrameBufferRing deleted successfully");
	} else {
		LOGI("LIFECYCLE: No ring buffer to delete (was already nullptr)");
	}

	// ========== STEP 5: CLEAR DIAGNOSTIC STATE ==========
	mFrameBufferRingOriginal = nullptr;
	mInjectionThreadId = 0;

	LOGI("LIFECYCLE: clearRingBuffer() complete");
}

/**
 * Invalidate ring buffer handle without freeing memory.
 * Called from Kotlin when surface is destroyed but USB is still connected.
 * This poisons the magic headers so any stale access fails gracefully
 * with MAGIC_POISONED rather than reading garbage data.
 */
void UVCPreview::invalidateRingBufferHandle() {
	ENTER();
	LOGI("LIFECYCLE: invalidateRingBufferHandle() called");

	FrameBufferRing* ring = mFrameBufferRing.load(std::memory_order_acquire);
	if (ring) {
		// Poison magic headers - any access will now fail validation
		ring->poisonMagicHeaders();
		
		// Note: We do NOT free the memory here.
		// The buffer will be freed when clearRingBuffer() is called.
		// This method just marks the handle as invalid for Kotlin's benefit.
		LOGI("LIFECYCLE: Ring buffer handle invalidated (poisoned) at %p", ring);
	} else {
		LOGI("LIFECYCLE: No ring buffer to invalidate");
	}

	EXIT();
}

/**
 * Check if ring buffer handle is valid for operations.
 * Returns false if ring buffer is null, magic headers are poisoned or corrupt.
 */
bool UVCPreview::isRingBufferValid() const {
	FrameBufferRing* ring = mFrameBufferRing.load(std::memory_order_acquire);
	if (!ring) {
		return false;
	}
	return ring->isValid();
}

/**
 * Get the frame buffer ring pointer for JNI access.
 * Thread-safe atomic load with acquire semantics.
 * @return FrameBufferRing pointer, or NULL if not allocated
 */
FrameBufferRing* UVCPreview::getFrameBufferRing() {
	return mFrameBufferRing.load(std::memory_order_acquire);
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
	pthread_t current_thread = pthread_self();

	// ========== ATOMIC LOADS FOR DIAGNOSTICS ==========
	// Use atomic loads for reading current state
	FrameBufferRing* previous = mFrameBufferRing.load(std::memory_order_acquire);
	bool wasInjected = mRingBufferInjected.load(std::memory_order_acquire);

	// ========== SIGSEGV DIAGNOSTIC: INJECTION-TIME LOGGING ==========
	LOGI("INJECT_DIAG: ========== RING BUFFER INJECTION START ==========");
	LOGI("INJECT_DIAG: instance=%u this=%p thread=%lu",
		 mInstanceId, this, (unsigned long)current_thread);
#if __SIZEOF_POINTER__ == 8
	LOGI("INJECT_DIAG: incoming_ring=%p (tag=0x%02x)",
		 (void*)ring, (unsigned)((uintptr_t)ring >> 56));
#else
	LOGI("INJECT_DIAG: incoming_ring=%p", (void*)ring);
#endif
	LOGI("INJECT_DIAG: previous mFrameBufferRing=%p mRingBufferInjected=%d",
		 (void*)previous, (int)wasInjected);

	// ========== LIFECYCLE LEAK DETECTION ==========
	if (previous != nullptr && previous != ring) {
		LOGW("INJECT_DIAG: WARNING - Replacing non-null ring buffer!");
	}

	// ═══════════════════════════════════════════════════════════════════════
	// FIX: POINTER EQUALITY GUARD (UAF Prevention)
	// ═══════════════════════════════════════════════════════════════════════
	//
	// When Kotlin calls allocateRingBuffer() then setFrameBufferRing() with
	// the SAME pointer:
	//   - allocateRingBuffer() stores ring at 0xABC, sets wasInjected=false
	//   - setFrameBufferRing(0xABC) sees previous=0xABC, wasInjected=false
	//   - OLD LOGIC: deletes 0xABC, then stores 0xABC → USE-AFTER-FREE
	//
	// FIX: If pointers match, just mark as injected and return.
	// The ring is already stored; we just need to update the flag.
	// ═══════════════════════════════════════════════════════════════════════
	if (previous == ring && ring != nullptr) {
		LOGI("INJECT_DIAG: Pointer equality detected (%p == %p)", previous, ring);
		LOGI("INJECT_DIAG: Ring already stored - marking as injected without deletion");

		// Validate the ring is still healthy before accepting
		if (!ring->validateMagic()) {
			LOGE("INJECT_DIAG: ERROR - Ring at %p already corrupted!", ring);
			LOGE("INJECT_DIAG: Cannot complete injection");
			RETURN(-4, int);
		}

		// Ring is valid and already stored - just update flags
		mRingBufferInjected.store(true, std::memory_order_release);
		mUseRingBuffer.store(true, std::memory_order_release);

		LOGI("INJECT_DIAG: Flags updated: mRingBufferInjected=true, mUseRingBuffer=true");
		LOGI("INJECT_DIAG: ========== INJECTION COMPLETE (pointer equality path) ==========");
		RETURN(0, int);
	}

	// ═══════════════════════════════════════════════════════════════════════
	// EXISTING LOGIC: Handle different pointers or null injection
	// ═══════════════════════════════════════════════════════════════════════

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

	// Validate incoming ring BEFORE storing
	LOGI("INJECT_DIAG: ring->isAllocated()=%d dims=%dx%d",
		 ring->isAllocated(),
		 ring->getWidth(), ring->getHeight());

	// If we had a previous ring and it was self-allocated (different pointer), destroy it
	if (previous != nullptr && !wasInjected) {
		LOGW("INJECT_DIAG: Destroying self-allocated ring %p before new injection", previous);
		previous->destroy();
		delete previous;
	} else if (previous != nullptr && wasInjected) {
		// Previously injected from external source - don't delete, just clear reference
		LOGI("INJECT_DIAG: Clearing reference to externally-injected ring %p (not deleting)", previous);
	}

	// Capture for later comparison (SIGSEGV diagnostic)
	mFrameBufferRingOriginal = ring;
	mInjectionThreadId = current_thread;

	// ========== ATOMIC STORES WITH RELEASE SEMANTICS ==========
	// Store pointer FIRST, then flags (consumers check flags before pointer)
	mFrameBufferRing.store(ring, std::memory_order_release);
	mRingBufferInjected.store(true, std::memory_order_release);
	mUseRingBuffer.store(true, std::memory_order_release);

	LOGI("INJECT_DIAG: STORED mFrameBufferRing=%p mRingBufferInjected=true (atomic)",
		 (void*)ring);
	LOGI("INJECT_DIAG: ========== RING BUFFER INJECTION COMPLETE ==========");
	RETURN(0, int);
}

/**
 * Write a frame to the ring buffer after converting to RGBA.
 * This replaces the ANativeWindow path when ring buffer mode is enabled.
 * @param frame Source frame (YUYV or other format)
 * @param convert_func Conversion function (e.g., uvc_any2rgbx)
 */
void UVCPreview::write_frame_to_ring_buffer(uvc_frame_t *frame, convFunc_t convert_func) {
	// Use atomic load to get ring buffer pointer
	FrameBufferRing* ring = mFrameBufferRing.load(std::memory_order_acquire);

	if (UNLIKELY(!ring || !ring->isAllocated())) {
		return;
	}

	int32_t strideBytes = 0;
	uint8_t *destPtr = static_cast<uint8_t*>(ring->lockWriteBuffer(&strideBytes));
	if (UNLIKELY(!destPtr)) {
		LOGW("Failed to lock ring buffer for write");
		return;
	}

	// Convert frame to RGBA and write directly to ring buffer
	if (convert_func) {
		// Create a temporary frame structure pointing to the ring buffer
		uvc_frame_t dest_frame;
		dest_frame.data = destPtr;
		dest_frame.data_bytes = ring->getWidth() * ring->getHeight() * 4;
		dest_frame.width = ring->getWidth();
		dest_frame.height = ring->getHeight();
		dest_frame.frame_format = UVC_FRAME_FORMAT_RGBX;
		dest_frame.step = strideBytes;  // Use actual stride from ring buffer
		dest_frame.library_owns_data = 0;

		uvc_error_t result = convert_func(frame, &dest_frame);
		if (UNLIKELY(result != UVC_SUCCESS)) {
			LOGW("Failed to convert frame for ring buffer: %d", result);
			// Cancel the write - unlocks buffer without committing
			ring->cancelWriteBuffer();
			return;
		}
	} else {
		// Direct copy if no conversion needed (rare case)
		size_t copyBytes = frame->width * frame->height * 4;
		if (copyBytes <= ring->getWidth() * ring->getHeight() * 4) {
			memcpy(destPtr, frame->data, copyBytes);
		}
	}

	ring->unlockWriteBuffer();
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
// Preview State Machine (Phase 2 - WARM State Support)
//======================================================================

PreviewState UVCPreview::getPreviewState() const {
	return mPreviewState.load(std::memory_order_acquire);
}

/**
 * Detach the preview surface, transitioning from HOT to WARM state.
 * USB streaming continues, but frames are drained without rendering.
 * Call this when the surface is destroyed (e.g., onSurfaceDestroyed).
 */
void UVCPreview::detachSurface() {
	ENTER();
	LOGI("WARM_STATE: detachSurface() called - transitioning to WARM");

	std::unique_lock<std::mutex> lock(mSwapMutex);

	// Signal that we're about to swap the surface
	mSwappingSurface.store(true, std::memory_order_release);

	// Wait for render thread to acknowledge and become idle
	// This prevents ANativeWindow_release from hanging on a locked buffer
	mRenderThreadIdleCond.wait(lock, [this]{
		return mIsRenderIdle.load(std::memory_order_acquire) || !isRunning();
	});

	// Safe to release surface now - render thread is parked
	pthread_mutex_lock(&preview_mutex);
	if (mPreviewWindow) {
		ANativeWindow_release(mPreviewWindow);
		mPreviewWindow = nullptr;
	}
	mSurfaceReady.store(false, std::memory_order_release);
	mPreviewState.store(PreviewState::WARM, std::memory_order_release);
	pthread_mutex_unlock(&preview_mutex);

	// Resume render thread (it will run in WARM drain mode)
	mSwappingSurface.store(false, std::memory_order_release);
	mSwappingCond.notify_all();

	LOGI("WARM_STATE: Now in WARM state - USB streaming continues");
	EXIT();
}

/**
 * Attach a new preview surface, transitioning from WARM to HOT state.
 * Call this when a new surface is available (e.g., onSurfaceCreated).
 */
void UVCPreview::attachSurface(ANativeWindow *window) {
	ENTER();
	LOGI("WARM_STATE: attachSurface() called - transitioning to HOT");

	if (!window) {
		LOGW("WARM_STATE: attachSurface called with null window");
		EXIT();
		return;
	}

	std::unique_lock<std::mutex> lock(mSwapMutex);

	// Signal surface swap in progress
	mSwappingSurface.store(true, std::memory_order_release);

	// Wait for render thread to become idle
	mRenderThreadIdleCond.wait(lock, [this]{
		return mIsRenderIdle.load(std::memory_order_acquire) || !isRunning();
	});

	// Configure the new surface
	pthread_mutex_lock(&preview_mutex);
	if (mPreviewWindow && mPreviewWindow != window) {
		ANativeWindow_release(mPreviewWindow);
	}
	mPreviewWindow = window;

	// Set geometry for the new surface
	int32_t err = ANativeWindow_setBuffersGeometry(mPreviewWindow,
		frameWidth, frameHeight, previewFormat);

	if (err != 0) {
		LOGE("WARM_STATE: Failed to set geometry: %d, staying in WARM", err);
		ANativeWindow_release(mPreviewWindow);
		mPreviewWindow = nullptr;
		mSurfaceReady.store(false, std::memory_order_release);
		mPreviewState.store(PreviewState::WARM, std::memory_order_release);
	} else {
		mSurfaceReady.store(true, std::memory_order_release);
		mPreviewState.store(PreviewState::HOT, std::memory_order_release);
		LOGI("WARM_STATE: Now in HOT state - rendering active");
	}
	pthread_mutex_unlock(&preview_mutex);

	// Resume render thread
	mSwappingSurface.store(false, std::memory_order_release);
	mSwappingCond.notify_all();

	EXIT();
}

//======================================================================
// Capture Callback Implementation (Dual-Emit Architecture)
//======================================================================

/**
 * Set the capture callback for dual-emit architecture.
 * @param env JNI environment
 * @param capture_callback_obj Java ICaptureFrameCallback object (or null to clear)
 * @return 0 on success
 */
int UVCPreview::setCaptureCallback(JNIEnv *env, jobject capture_callback_obj) {
	ENTER();

	// Disable capture while changing callback
	mCaptureCallbackEnabled.store(false, std::memory_order_release);

	// Clear previous callback
	if (mCaptureCallbackObj) {
		env->DeleteGlobalRef(mCaptureCallbackObj);
		mCaptureCallbackObj = nullptr;
		mCaptureCallbackMethod = nullptr;
	}

	if (capture_callback_obj) {
		// Create global reference
		mCaptureCallbackObj = env->NewGlobalRef(capture_callback_obj);
		if (!mCaptureCallbackObj) {
			LOGE("Failed to create global ref for capture callback");
			RETURN(-1, int);
		}

		// Get the callback method
		jclass clazz = env->GetObjectClass(mCaptureCallbackObj);
		if (clazz) {
			mCaptureCallbackMethod = env->GetMethodID(clazz,
				"onCaptureFrame", "(Ljava/nio/ByteBuffer;IIIJ)V");
			env->DeleteLocalRef(clazz);

			if (!mCaptureCallbackMethod) {
				LOGE("Failed to find onCaptureFrame method");
				env->DeleteGlobalRef(mCaptureCallbackObj);
				mCaptureCallbackObj = nullptr;
				RETURN(-1, int);
			}
		}

		LOGI("Capture callback set successfully");
	}

	RETURN(0, int);
}

/**
 * Set the pixel format for capture callback.
 * @param format One of CAPTURE_FORMAT_* constants
 * @return 0 on success
 */
int UVCPreview::setCaptureFormat(int format) {
	ENTER();

	if (format < CAPTURE_FORMAT_RGBX || format > CAPTURE_FORMAT_I420) {
		LOGE("Invalid capture format: %d", format);
		RETURN(-1, int);
	}

	mCaptureFormat.store(static_cast<CapturePixelFormat>(format), std::memory_order_release);
	LOGI("Capture format set to %d", format);

	RETURN(0, int);
}

/**
 * Set target frame rate for capture callback (decimation).
 * @param targetFps Target FPS (capture will drop frames to match this rate)
 * @return 0 on success
 */
int UVCPreview::setCaptureFrameRate(int targetFps) {
	ENTER();

	if (targetFps < 1 || targetFps > 120) {
		LOGE("Invalid capture frame rate: %d", targetFps);
		RETURN(-1, int);
	}

	mCaptureTargetFps.store(targetFps, std::memory_order_release);
	LOGI("Capture frame rate set to %d fps", targetFps);

	RETURN(0, int);
}

/**
 * Enable or disable capture callback.
 * @param enable true to enable, false to disable
 * @return 0 on success
 */
int UVCPreview::enableCaptureCallback(bool enable) {
	ENTER();

	if (enable && !mCaptureCallbackObj) {
		LOGE("Cannot enable capture: no callback set");
		RETURN(-1, int);
	}

	mCaptureCallbackEnabled.store(enable, std::memory_order_release);
	LOGI("Capture callback %s", enable ? "enabled" : "disabled");

	// Reset frame counter and timing when enabling
	if (enable) {
		mCaptureFrameCounter.store(0, std::memory_order_relaxed);
		mLastCaptureEmitTimeNs = 0;
	}

	RETURN(0, int);
}

/**
 * Clear capture callback and release resources.
 * @param env JNI environment
 */
void UVCPreview::clearCaptureCallback(JNIEnv *env) {
	ENTER();

	mCaptureCallbackEnabled.store(false, std::memory_order_release);

	// Wait for any in-progress callback to complete
	while (mCaptureCallbackInProgress.load(std::memory_order_acquire)) {
		usleep(1000);  // 1ms
	}

	if (mCaptureCallbackObj && env) {
		env->DeleteGlobalRef(mCaptureCallbackObj);
	}
	mCaptureCallbackObj = nullptr;
	mCaptureCallbackMethod = nullptr;

	EXIT();
}

/**
 * Get capture telemetry: frames emitted to callback.
 */
uint64_t UVCPreview::getCaptureFramesEmitted() const {
	return mCaptureFramesEmitted.load(std::memory_order_relaxed);
}

/**
 * Get capture telemetry: frames dropped due to format conversion issues.
 */
uint64_t UVCPreview::getCaptureFramesDropped() const {
	return mCaptureFramesDropped.load(std::memory_order_relaxed);
}

/**
 * Get capture telemetry: frames dropped because callback was busy.
 */
uint64_t UVCPreview::getCaptureCallbackBusy() const {
	return mCaptureCallbackBusy.load(std::memory_order_relaxed);
}

/**
 * Calculate buffer size needed for a given format.
 */
size_t UVCPreview::getCaptureBufferSize(int width, int height, CapturePixelFormat format) {
	switch (format) {
		case CAPTURE_FORMAT_RGBX:
			return width * height * 4;
		case CAPTURE_FORMAT_NV21:
		case CAPTURE_FORMAT_I420:
			return width * height * 3 / 2;  // Y + UV (4:2:0)
		case CAPTURE_FORMAT_YUYV:
			return width * height * 2;  // 4:2:2
		default:
			return width * height * 4;
	}
}

/**
 * Ensure capture buffer is allocated with sufficient capacity.
 */
void UVCPreview::ensureCaptureBuffer(int width, int height, CapturePixelFormat format) {
	size_t needed = getCaptureBufferSize(width, height, format);

	pthread_mutex_lock(&mCaptureBufferMutex);
	if (mCaptureBufferCapacity < needed) {
		if (mCaptureBuffer) {
			free(mCaptureBuffer);
		}
		mCaptureBuffer = static_cast<uint8_t*>(malloc(needed));
		mCaptureBufferCapacity = mCaptureBuffer ? needed : 0;
		LOGI("Allocated capture buffer: %zu bytes", needed);
	}
	pthread_mutex_unlock(&mCaptureBufferMutex);
}

/**
 * Free capture buffer.
 */
void UVCPreview::freeCaptureBuffer() {
	pthread_mutex_lock(&mCaptureBufferMutex);
	if (mCaptureBuffer) {
		free(mCaptureBuffer);
		mCaptureBuffer = nullptr;
		mCaptureBufferCapacity = 0;
	}
	pthread_mutex_unlock(&mCaptureBufferMutex);
}

/**
 * Check if we should emit a capture frame based on frame rate decimation.
 * Uses time-based decimation for smoother frame distribution.
 */
bool UVCPreview::shouldEmitCaptureFrame() {
	if (!mCaptureCallbackEnabled.load(std::memory_order_acquire)) {
		return false;
	}

	int targetFps = mCaptureTargetFps.load(std::memory_order_relaxed);
	if (targetFps <= 0) return false;

	int64_t nowNs = StreamTelemetry::getCurrentTimeNs();
	int64_t frameIntervalNs = 1000000000LL / targetFps;

	// First frame always emits
	if (mLastCaptureEmitTimeNs == 0) {
		return true;
	}

	// Check if enough time has passed since last emit
	return (nowNs - mLastCaptureEmitTimeNs) >= frameIntervalNs;
}

/**
 * Convert RGBX to NV21 (YVU420SP) format.
 * NV21 layout: Y plane (width*height), then interleaved VU plane (width*height/2)
 */
void UVCPreview::convertRgbxToNv21(const uint8_t* __restrict rgbx,
                                    uint8_t* __restrict nv21,
                                    int width, int height) {
	uint8_t* yPlane = nv21;
	uint8_t* vuPlane = nv21 + width * height;

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int srcIdx = (y * width + x) * 4;
			uint8_t r = rgbx[srcIdx];
			uint8_t g = rgbx[srcIdx + 1];
			uint8_t b = rgbx[srcIdx + 2];

			// BT.601 RGB to YUV conversion (limited range)
			int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
			yPlane[y * width + x] = static_cast<uint8_t>(yVal < 16 ? 16 : (yVal > 235 ? 235 : yVal));

			// Subsample UV (every 2x2 block)
			if ((x & 1) == 0 && (y & 1) == 0) {
				int vVal = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
				int uVal = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
				int vuIdx = (y / 2) * width + x;
				vuPlane[vuIdx] = static_cast<uint8_t>(vVal < 16 ? 16 : (vVal > 240 ? 240 : vVal));
				vuPlane[vuIdx + 1] = static_cast<uint8_t>(uVal < 16 ? 16 : (uVal > 240 ? 240 : uVal));
			}
		}
	}
}

/**
 * Convert RGBX to YUYV (YUV422) format.
 * YUYV layout: Y0 U0 Y1 V0 (4 bytes per 2 pixels)
 */
void UVCPreview::convertRgbxToYuyv(const uint8_t* __restrict rgbx,
                                    uint8_t* __restrict yuyv,
                                    int width, int height) {
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x += 2) {
			int srcIdx0 = (y * width + x) * 4;
			int srcIdx1 = (y * width + x + 1) * 4;

			uint8_t r0 = rgbx[srcIdx0], g0 = rgbx[srcIdx0 + 1], b0 = rgbx[srcIdx0 + 2];
			uint8_t r1 = rgbx[srcIdx1], g1 = rgbx[srcIdx1 + 1], b1 = rgbx[srcIdx1 + 2];

			// BT.601 conversion
			int y0 = ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16;
			int y1 = ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16;

			// Average for U/V
			int avgR = (r0 + r1) / 2;
			int avgG = (g0 + g1) / 2;
			int avgB = (b0 + b1) / 2;
			int u = ((-38 * avgR - 74 * avgG + 112 * avgB + 128) >> 8) + 128;
			int v = ((112 * avgR - 94 * avgG - 18 * avgB + 128) >> 8) + 128;

			int dstIdx = (y * width + x) * 2;
			yuyv[dstIdx] = static_cast<uint8_t>(y0 < 16 ? 16 : (y0 > 235 ? 235 : y0));
			yuyv[dstIdx + 1] = static_cast<uint8_t>(u < 16 ? 16 : (u > 240 ? 240 : u));
			yuyv[dstIdx + 2] = static_cast<uint8_t>(y1 < 16 ? 16 : (y1 > 235 ? 235 : y1));
			yuyv[dstIdx + 3] = static_cast<uint8_t>(v < 16 ? 16 : (v > 240 ? 240 : v));
		}
	}
}

/**
 * Convert RGBX to I420 (YUV420P) format.
 * I420 layout: Y plane, then U plane, then V plane (all separate)
 */
void UVCPreview::convertRgbxToI420(const uint8_t* __restrict rgbx,
                                    uint8_t* __restrict i420,
                                    int width, int height) {
	uint8_t* yPlane = i420;
	uint8_t* uPlane = i420 + width * height;
	uint8_t* vPlane = uPlane + (width * height / 4);

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int srcIdx = (y * width + x) * 4;
			uint8_t r = rgbx[srcIdx];
			uint8_t g = rgbx[srcIdx + 1];
			uint8_t b = rgbx[srcIdx + 2];

			// BT.601 conversion
			int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
			yPlane[y * width + x] = static_cast<uint8_t>(yVal < 16 ? 16 : (yVal > 235 ? 235 : yVal));

			// Subsample UV (every 2x2 block)
			if ((x & 1) == 0 && (y & 1) == 0) {
				int uVal = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
				int vVal = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
				int uvIdx = (y / 2) * (width / 2) + (x / 2);
				uPlane[uvIdx] = static_cast<uint8_t>(uVal < 16 ? 16 : (uVal > 240 ? 240 : uVal));
				vPlane[uvIdx] = static_cast<uint8_t>(vVal < 16 ? 16 : (vVal > 240 ? 240 : vVal));
			}
		}
	}
}

/**
 * Emit a frame to the capture callback.
 * Uses non-blocking drop policy: if callback is still running, drop this frame.
 * @param rgbxData Source RGBX data from ring buffer
 * @param width Frame width
 * @param height Frame height
 * @param timestampNs Frame timestamp in nanoseconds
 */
void UVCPreview::emitCaptureFrame(const uint8_t* rgbxData, int width, int height, int64_t timestampNs) {
	// Non-blocking check: if callback is busy, drop frame
	bool expected = false;
	if (!mCaptureCallbackInProgress.compare_exchange_strong(expected, true,
			std::memory_order_acquire, std::memory_order_relaxed)) {
		mCaptureCallbackBusy.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	// Get JNI environment for this thread
	JNIEnv* env = nullptr;
	JavaVM* vm = getVM();
	if (!vm) {
		mCaptureCallbackInProgress.store(false, std::memory_order_release);
		return;
	}

	bool attached = false;
	int envStatus = vm->GetEnv((void**)&env, JNI_VERSION_1_6);

	if (envStatus == JNI_EDETACHED) {
		if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
			LOGE("CAPTURE: Failed to attach thread");
			mCaptureCallbackInProgress.store(false, std::memory_order_release);
			return;
		}
		attached = true;
	}

	if (!env || !mCaptureCallbackObj || !mCaptureCallbackMethod) {
		if (attached) vm->DetachCurrentThread();
		mCaptureCallbackInProgress.store(false, std::memory_order_release);
		return;
	}

	CapturePixelFormat format = mCaptureFormat.load(std::memory_order_relaxed);
	size_t bufferSize = getCaptureBufferSize(width, height, format);

	// Ensure capture buffer is allocated
	ensureCaptureBuffer(width, height, format);

	pthread_mutex_lock(&mCaptureBufferMutex);

	if (!mCaptureBuffer || mCaptureBufferCapacity < bufferSize) {
		pthread_mutex_unlock(&mCaptureBufferMutex);
		mCaptureFramesDropped.fetch_add(1, std::memory_order_relaxed);
		if (attached) vm->DetachCurrentThread();
		mCaptureCallbackInProgress.store(false, std::memory_order_release);
		return;
	}

	// Convert to target format
	switch (format) {
		case CAPTURE_FORMAT_RGBX:
			memcpy(mCaptureBuffer, rgbxData, bufferSize);
			break;
		case CAPTURE_FORMAT_NV21:
			convertRgbxToNv21(rgbxData, mCaptureBuffer, width, height);
			break;
		case CAPTURE_FORMAT_YUYV:
			convertRgbxToYuyv(rgbxData, mCaptureBuffer, width, height);
			break;
		case CAPTURE_FORMAT_I420:
			convertRgbxToI420(rgbxData, mCaptureBuffer, width, height);
			break;
	}

	// Create DirectByteBuffer wrapping the capture buffer
	jobject buffer = env->NewDirectByteBuffer(mCaptureBuffer, bufferSize);

	pthread_mutex_unlock(&mCaptureBufferMutex);

	if (buffer) {
		// Call the Java callback
		env->CallVoidMethod(mCaptureCallbackObj, mCaptureCallbackMethod,
			buffer, width, height, static_cast<int>(format), timestampNs);

		if (env->ExceptionCheck()) {
			env->ExceptionDescribe();
			env->ExceptionClear();
			mCaptureFramesDropped.fetch_add(1, std::memory_order_relaxed);
		} else {
			mCaptureFramesEmitted.fetch_add(1, std::memory_order_relaxed);
			mLastCaptureEmitTimeNs = StreamTelemetry::getCurrentTimeNs();
		}

		env->DeleteLocalRef(buffer);
	} else {
		mCaptureFramesDropped.fetch_add(1, std::memory_order_relaxed);
	}

	if (attached) {
		vm->DetachCurrentThread();
	}

	mCaptureCallbackInProgress.store(false, std::memory_order_release);
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
	FrameBufferRing* ring = mFrameBufferRing.load(std::memory_order_acquire);
	if (ring) {
		ring->signalConversionThread();
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

	// Use atomic load for ring buffer
	FrameBufferRing* ring = mFrameBufferRing.load(std::memory_order_acquire);
	bool injected = mRingBufferInjected.load(std::memory_order_acquire);

	if (!ring) {
		LOGE("HANDLE_DIAG: do_conversion_loop - ring buffer is NULL!");
		EXIT();
		return;
	}

	// Log ring buffer pointer at thread start for correlation
	LOGI("HANDLE_DIAG: Conversion thread started with ring=%p injected=%s",
		 ring, injected ? "true" : "false");

	StreamTelemetry* telemetry = ring->getTelemetry();
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

		// Try to dequeue a pending frame (use local cached ring pointer)
		PendingFrame* pending = ring->dequeuePendingFrame();

		if (!pending) {
			// Wait for signal with timeout
			ring->waitForSignal(100);  // 100ms timeout
			continue;
		}

		// Timestamp conversion start
		int64_t convStartNs = StreamTelemetry::getCurrentTimeNs();

		// Lock destination AHardwareBuffer
		int32_t strideBytes = 0;
		uint8_t* destPtr = static_cast<uint8_t*>(
			ring->lockWriteBuffer(&strideBytes));

		if (UNLIKELY(!destPtr)) {
			LOGW("Failed to lock ring buffer for write");
			telemetry->producerStalls.fetch_add(1, std::memory_order_relaxed);
			ring->completePendingFrame(pending);
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
		dest_frame.width = ring->getWidth();
		dest_frame.height = ring->getHeight();
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
					ring->cancelWriteBuffer();
					ring->completePendingFrame(pending);
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
			ring->unlockWriteBuffer();

			// Record latency metrics (microseconds)
			int64_t inPipeLatencyUs = (convEndNs - pending->callbackTimestampNs) / 1000;
			int64_t conversionTimeUs = (convEndNs - convStartNs) / 1000;
			telemetry->recordInPipeLatency(inPipeLatencyUs, conversionTimeUs);

			// === DUAL-EMIT: Capture callback path ===
			// NOTE: We read from destPtr AFTER unlockWriteBuffer().
			// This is safe because:
			// 1. Triple-buffering: GPU reads different slot than we're reading
			// 2. CPU cache: Data is still hot in L1/L2 cache
			// 3. Ring rotation: This slot won't be overwritten for 2+ frames
			//
			// If you see tearing in recorded video, move this block BEFORE
			// unlockWriteBuffer() at the cost of ~1-2ms display latency.
			// ============================================================
			if (shouldEmitCaptureFrame()) {
				// destPtr still points to the converted RGBX data
				emitCaptureFrame(destPtr, dest_frame.width, dest_frame.height,
				                 pending->callbackTimestampNs);
			}

			// HANDLE_DIAG: Log successful writes for pointer correlation
			static std::atomic<int> writeCount{0};
			int wc = ++writeCount;
			if (wc <= 10 || wc % 1000 == 0) {
				int latest = ring->getLatestCompleted();
				LOGI("HANDLE_DIAG: Producer write[%d] ring=%p latest=%d",
					 wc, ring, latest);
			}

			// DIAGNOSTIC: Log first few successful conversions
			static int convSuccessCount = 0;
			if (++convSuccessCount <= 3) {
				LOGI("PIPELINE_DIAG: Conversion #%d SUCCESS (latency=%lldμs, conv=%lldμs)",
					convSuccessCount, (long long)inPipeLatencyUs, (long long)conversionTimeUs);
			}

		} else {
			ring->cancelWriteBuffer();
			telemetry->framesCorrupted.fetch_add(1, std::memory_order_relaxed);
			LOGW("Frame conversion failed: %d", result);
		}

		ring->completePendingFrame(pending);
	}

	// Cleanup
	if (temp_yuyv.data) {
		free(temp_yuyv.data);
	}

	LOGI("HANDLE_DIAG: Conversion thread exiting (ring=%p)", ring);
	EXIT();
}

int UVCPreview::getPreviewFps() const {
	return mPreviewFps.load(std::memory_order_relaxed);
}
