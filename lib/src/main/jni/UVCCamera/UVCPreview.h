/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 *
 * File name: UVCPreview.h
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

#ifndef UVCPREVIEW_H_
#define UVCPREVIEW_H_

#include "libUVCCamera.h"
#include <pthread.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <android/native_window.h>
#include "objectarray.h"
#include "FrameBufferRing.h"

// Forward declaration
class UVCReadinessCallback;

//======================================================================
// PreviewState: Three-state lifecycle for preview pipeline
//======================================================================
// COLD: No USB streaming, preview thread not running
// WARM: USB streaming active, no surface - frames drained but not rendered
// HOT:  USB streaming active + surface available - full rendering
//
// This enables instant resume when surface returns (Gallery navigation)
// without restarting USB streaming.
//======================================================================
enum class PreviewState : int {
    COLD = 0,    // No USB, no threads
    WARM = 1,    // USB running, no surface (frames drained/stashed)
    HOT  = 2     // USB running + rendering active
};

#pragma interface

#define DEFAULT_PREVIEW_WIDTH 640
#define DEFAULT_PREVIEW_HEIGHT 480
#define DEFAULT_PREVIEW_FPS_MIN 1
#define DEFAULT_PREVIEW_FPS_MAX 30
#define DEFAULT_PREVIEW_MODE 0
#define DEFAULT_BANDWIDTH 1.0f

// Timeout for waitPreviewFrame() to prevent indefinite blocking
#define PREVIEW_WAIT_TIMEOUT_MS 100

// Preview error codes for explicit error reporting
#define PREVIEW_ERROR_UNKNOWN                   -1
#define PREVIEW_ERROR_ALREADY_RUNNING           -2
#define PREVIEW_ERROR_RING_BUFFER_NOT_ALLOCATED -3
#define PREVIEW_ERROR_THREAD_CREATE_FAILED      -4
#define PREVIEW_ERROR_NO_OUTPUT_TARGET          -5

typedef uvc_error_t (*convFunc_t)(uvc_frame_t *in, uvc_frame_t *out);

#define PIXEL_FORMAT_RAW 0		// same as PIXEL_FORMAT_YUV
#define PIXEL_FORMAT_YUV 1
#define PIXEL_FORMAT_RGB565 2
#define PIXEL_FORMAT_RGBX 3
#define PIXEL_FORMAT_YUV20SP 4
#define PIXEL_FORMAT_NV21 5		// YVU420SemiPlanar

// for callback to Java object
typedef struct {
	jmethodID onFrame;
} Fields_iframecallback;

// Capture callback function type
// Parameters: data, dataSize, width, height, format, timestampNs
typedef void (*captureCallbackFunc_t)(
    void* userData,
    const uint8_t* data,
    size_t dataSize,
    int width,
    int height,
    int format,
    int64_t timestampNs
);

class UVCPreview {
public:
	// Capture pixel formats (matches Java/Kotlin CaptureFormat enum)
	enum CapturePixelFormat {
		CAPTURE_FORMAT_RGBX = 0,    // Direct from conversion (fastest)
		CAPTURE_FORMAT_NV21 = 1,    // YUV420SP - efficient for MediaCodec
		CAPTURE_FORMAT_YUYV = 2,    // YUV422 - raw sensor format
		CAPTURE_FORMAT_I420 = 3     // YUV420P - universal compatibility
	};

private:
	uvc_device_handle_t *mDeviceHandle;
	ANativeWindow *mPreviewWindow;
	std::atomic<bool> mIsRunning{false};
	std::atomic<bool> mSurfaceReady{false};

// ============================================================
// PREVIEW STATE MACHINE (Phase 2 - WARM State Support)
// ============================================================
// Enables USB streaming to continue while surface is unavailable.
// Frames are drained in WARM state to prevent USB backpressure.
	std::atomic<PreviewState> mPreviewState{PreviewState::COLD};

	// Keep Latest policy: stash frame for instant resume on surface return
	uvc_frame_t* mLastWarmFrame{nullptr};
	pthread_mutex_t mWarmFrameMutex;

	// Surface swap handshake: ensures render thread is idle before surface change
	std::mutex mSwapMutex;
	std::condition_variable mRenderThreadIdleCond;
	std::condition_variable mSwappingCond;
	std::atomic<bool> mSwappingSurface{false};
	std::atomic<bool> mIsRenderIdle{false};
	std::atomic<uint64_t> mDroppedNoSurface{0};
	std::atomic<uint64_t> mDroppedQueueFull{0};
	std::atomic<uint64_t> mTotalFramesProcessed{0};
	int requestWidth, requestHeight, requestMode;
	int requestMinFps, requestMaxFps;
	float requestBandwidth;
	int frameWidth, frameHeight;
	int frameMode;
	size_t frameBytes;
	pthread_t preview_thread;
	std::atomic<bool> mPreviewThreadValid{false};
	pthread_mutex_t preview_mutex;
	pthread_cond_t preview_sync;
	ObjectArray<uvc_frame_t *> previewFrames;
	int previewFormat;
	size_t previewBytes;
//
	std::atomic<bool> mIsCapturing{false};
	ANativeWindow *mCaptureWindow;
	pthread_t capture_thread;
	std::atomic<bool> mCaptureThreadValid{false};
	pthread_mutex_t capture_mutex;
	pthread_cond_t capture_sync;
	uvc_frame_t *captureQueu;			// keep latest frame
	jobject mFrameCallbackObj;
	convFunc_t mFrameCallbackFunc;
	Fields_iframecallback iframecallback_fields;
	int mPixelFormat;
	size_t callbackPixelBytes;
// improve performance by reducing memory allocation
	pthread_mutex_t pool_mutex;
	ObjectArray<uvc_frame_t *> mFramePool;
	uvc_frame_t *get_frame(size_t data_bytes);
	void recycle_frame(uvc_frame_t *frame);
	void init_pool(size_t data_bytes);
	void clear_pool();
//
	void clearDisplay();
	static void uvc_preview_frame_callback(uvc_frame_t *frame, void *vptr_args);
	void addPreviewFrame(uvc_frame_t *frame);
	uvc_frame_t *waitPreviewFrame();
	void clearPreviewFrame();
	static void *preview_thread_func(void *vptr_args);
	int prepare_preview(uvc_stream_ctrl_t *ctrl);
	void do_preview(uvc_stream_ctrl_t *ctrl);
	uvc_frame_t *draw_preview_one(uvc_frame_t *frame, ANativeWindow **window, convFunc_t func, int pixelBytes);
//
	void addCaptureFrame(uvc_frame_t *frame);
	uvc_frame_t *waitCaptureFrame();
	void clearCaptureFrame();
	static void *capture_thread_func(void *vptr_args);
	void do_capture(JNIEnv *env);
	void do_capture_surface(JNIEnv *env);
	void do_capture_idle_loop(JNIEnv *env);
	void do_capture_callback(JNIEnv *env, uvc_frame_t *frame);
	void callbackPixelFormatChanged();
//
// ============================================================
// THREAD-SAFE RING BUFFER STATE (P0 FIX - 2026-01-05)
// ============================================================
// Ring buffer support for decoupled frame streaming
// All members are atomic to prevent cross-thread visibility races
	std::atomic<FrameBufferRing*> mFrameBufferRing{nullptr};
	std::atomic<bool> mUseRingBuffer{false};
	std::atomic<bool> mRingBufferInjected{false};  // External ownership flag

// ============================================================
// CALLBACK DRAIN MECHANISM (P0 - USE-AFTER-FREE PREVENTION)
// ============================================================
// Tracks number of callbacks currently executing inside ring buffer code.
// clearRingBuffer() waits for this to reach zero before deleting.
	std::atomic<int> mCallbacksInFlight{0};

// ============================================================
// RE-ENTRANCY GUARD
// ============================================================
// Prevents multiple simultaneous or sequential clearRingBuffer() calls
	std::atomic<bool> mClearingRingBuffer{false};

// ============================================================
// RAII CALLBACK GUARD
// ============================================================
// Automatically increments mCallbacksInFlight on construction,
// decrements on destruction. Ensures proper cleanup even on early returns.
public:
	class CallbackGuard {
		std::atomic<int>& mCounter;
		bool mActive;
	public:
		explicit CallbackGuard(std::atomic<int>& c) : mCounter(c), mActive(true) {
			mCounter.fetch_add(1, std::memory_order_acquire);
		}
		~CallbackGuard() {
			if (mActive) {
				mCounter.fetch_sub(1, std::memory_order_release);
			}
		}
		// Disable guard without decrementing (for legacy path optimization)
		void disarm() { mActive = false; }
		// Non-copyable
		CallbackGuard(const CallbackGuard&) = delete;
		CallbackGuard& operator=(const CallbackGuard&) = delete;
	};
private:

// SIGSEGV diagnostic infrastructure (2026-01-05)
	static std::atomic<uint32_t> sInstanceCounter;
	uint32_t mInstanceId{0};
	FrameBufferRing* mFrameBufferRingOriginal{nullptr};  // Capture at injection time
	pthread_t mInjectionThreadId{0};                      // Thread that called setFrameBufferRing
	void write_frame_to_ring_buffer(uvc_frame_t *frame, convFunc_t convert_func);
// Readiness callback for signaling when preview thread is ready
	UVCReadinessCallback *mReadinessCallback;
// Conversion thread for hybrid architecture (SPSC queue consumer)
	pthread_t mConversionThread;
	std::atomic<bool> mConversionThreadValid{false};
	std::atomic<bool> mConversionThreadRunning{false};
	static void *conversion_thread_func(void *vptr_args);
	void do_conversion_loop();
// Telemetry helpers
	void incrementDroppedNoSurface();
	void incrementDroppedQueueFull();
	void incrementTotalFrames();

// ============================================================
// CAPTURE CALLBACK (DUAL-EMIT ARCHITECTURE)
// ============================================================
// Separate capture path for recording/snapshot, independent of display.
// Uses non-blocking drop policy to never stall display path.

private:
	// Capture callback state
	std::atomic<bool> mCaptureCallbackEnabled{false};
	jobject mCaptureCallbackObj{nullptr};
	jmethodID mCaptureCallbackMethod{nullptr};
	std::atomic<CapturePixelFormat> mCaptureFormat{CAPTURE_FORMAT_NV21};

	// Frame rate decimation
	std::atomic<int> mCaptureTargetFps{30};
	std::atomic<int> mPreviewFps{30};
	std::atomic<uint64_t> mCaptureFrameCounter{0};
	int64_t mLastCaptureEmitTimeNs{0};

	// Capture buffer (reused to avoid allocation per frame)
	uint8_t* mCaptureBuffer{nullptr};
	size_t mCaptureBufferCapacity{0};
	pthread_mutex_t mCaptureBufferMutex;

	// Non-blocking drop policy: if callback is still running, drop frame
	std::atomic<bool> mCaptureCallbackInProgress{false};

	// Capture telemetry
	std::atomic<uint64_t> mCaptureFramesEmitted{0};
	std::atomic<uint64_t> mCaptureFramesDropped{0};
	std::atomic<uint64_t> mCaptureCallbackBusy{0};

	// Private capture methods
	bool shouldEmitCaptureFrame();
	void emitCaptureFrame(const uint8_t* rgbxData, int width, int height, int64_t timestampNs);
	void convertRgbxToNv21(const uint8_t* __restrict rgbx, uint8_t* __restrict nv21, int width, int height);
	void convertRgbxToYuyv(const uint8_t* __restrict rgbx, uint8_t* __restrict yuyv, int width, int height);
	void convertRgbxToI420(const uint8_t* __restrict rgbx, uint8_t* __restrict i420, int width, int height);
	size_t getCaptureBufferSize(int width, int height, CapturePixelFormat format);
	void ensureCaptureBuffer(int width, int height, CapturePixelFormat format);
	void freeCaptureBuffer();
public:
	UVCPreview(uvc_device_handle_t *devh);
	~UVCPreview();

	inline const bool isRunning() const;
	int setPreviewSize(int width, int height, int min_fps, int max_fps, int mode, float bandwidth = 1.0f);
	int setPreviewDisplay(ANativeWindow *preview_window);
	int setFrameCallback(JNIEnv *env, jobject frame_callback_obj, int pixel_format);
	int startPreview();
	int stopPreview();
	void forceStop();  // For hard reset - stops without joining
	void setReadinessCallback(UVCReadinessCallback *callback);
	inline const bool isCapturing() const;
	int setCaptureDisplay(ANativeWindow *capture_window);
//
// Ring buffer control
	int setUseRingBuffer(bool use);
	int allocateRingBuffer(int width, int height);
	void destroyRingBuffer();
	FrameBufferRing* getFrameBufferRing();
	int setFrameBufferRing(FrameBufferRing *ring);
	void clearRingBuffer();  // Signal-Drain-Destroy lifecycle cleanup
	/**
	 * Invalidate ring buffer handle without freeing memory.
	 * Called from Kotlin when surface is destroyed but USB is still connected.
	 * This poisons the magic headers so any stale access fails gracefully
	 * with MAGIC_POISONED log rather than reading garbage data.
	 */
	void invalidateRingBufferHandle();
	/**
	 * Check if ring buffer handle is valid for operations.
	 * Returns false if ring buffer is null, magic headers are poisoned or corrupt.
	 */
	bool isRingBufferValid() const;
// Conversion thread control (hybrid architecture)
	int startConversionThread();
	void stopConversionThread();
//
// Preview state accessors (Phase 2 - WARM state)
	PreviewState getPreviewState() const;
	/**
	 * Returns bitmask of internal state for Kotlin diagnostics.
	 *
	 * Bitmask values:
	 *   0x01: isRunning (preview thread active)
	 *   0x02: Surface bound (mPreviewWindow != nullptr)
	 *   0x10: mPreviewState == WARM (Active Drain mode)
	 *   0x20: mPreviewState == HOT (Rendering mode)
	 *   0x40: mPreviewState == COLD (Stopped)
	 *   0x80: Stagnation detected (no frame processed in >500ms)
	 *
	 * Thread-safe: uses atomic reads only (no mutex needed)
	 */
	jint getInternalDiagnosticState() const;
	void detachSurface();     // Transition HOT → WARM
	void attachSurface(ANativeWindow *window);  // Transition WARM → HOT
//
// Telemetry accessors
	uint64_t getDroppedNoSurface() const;
	uint64_t getDroppedQueueFull() const;
	uint64_t getTotalFramesProcessed() const;
	bool isSurfaceReady() const;
//
// Capture callback API (dual-emit architecture)
	int setCaptureCallback(JNIEnv *env, jobject capture_callback_obj);
	int setCaptureFormat(int format);
	int setCaptureFrameRate(int targetFps);
	int enableCaptureCallback(bool enable);
	void clearCaptureCallback(JNIEnv *env);
	uint64_t getCaptureFramesEmitted() const;
	uint64_t getCaptureFramesDropped() const;
	uint64_t getCaptureCallbackBusy() const;
	int getPreviewFps() const;
};

#endif /* UVCPREVIEW_H_ */
