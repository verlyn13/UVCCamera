/*
 *  UVCCamera
 *  library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 *  All files in the folder are under this Apache License, Version 2.0.
 *  Files in the libjpeg-turbo, libusb, libuvc, rapidjson folder
 *  may have a different license, see the respective files.
 */

package com.serenegiant.usb;

import java.util.ArrayList;
import java.util.List;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import android.graphics.SurfaceTexture;
import android.hardware.usb.UsbDevice;
import android.text.TextUtils;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;

import com.serenegiant.usb.USBMonitor.UsbControlBlock;

public class UVCCamera {
	private static final boolean DEBUG = false;	// TODO set false when releasing
	private static final String TAG = UVCCamera.class.getSimpleName();
	private static final String DEFAULT_USBFS = "/dev/bus/usb";

	public static final int DEFAULT_PREVIEW_WIDTH = 640;
	public static final int DEFAULT_PREVIEW_HEIGHT = 480;
	public static final int DEFAULT_PREVIEW_MODE = 0;
	public static final int DEFAULT_PREVIEW_MIN_FPS = 1;
	public static final int DEFAULT_PREVIEW_MAX_FPS = 30;
	public static final float DEFAULT_BANDWIDTH = 1.0f;

	public static final int FRAME_FORMAT_YUYV = 0;
	public static final int FRAME_FORMAT_MJPEG = 1;

	public static final int PIXEL_FORMAT_RAW = 0;
	public static final int PIXEL_FORMAT_YUV = 1;
	public static final int PIXEL_FORMAT_RGB565 = 2;
	public static final int PIXEL_FORMAT_RGBX = 3;
	public static final int PIXEL_FORMAT_YUV420SP = 4;
	public static final int PIXEL_FORMAT_NV21 = 5;		// = YVU420SemiPlanar

	// Cleanup level constants for graduated resource release
	public static final int CLEANUP_PREVIEW_ONLY = 0;  // Stop preview, keep USB
	public static final int CLEANUP_CAMERA = 1;        // Close camera handle
	public static final int CLEANUP_INTERFACE = 2;     // Release USB interface
	public static final int CLEANUP_FULL = 3;          // Release everything

	// Preview error codes (returned by startPreview)
	public static final int PREVIEW_ERROR_SUCCESS = 0;
	public static final int PREVIEW_ERROR_UNKNOWN = -1;
	public static final int PREVIEW_ERROR_ALREADY_RUNNING = -2;
	public static final int PREVIEW_ERROR_RING_BUFFER_NOT_ALLOCATED = -3;
	public static final int PREVIEW_ERROR_THREAD_CREATE_FAILED = -4;
	public static final int PREVIEW_ERROR_NO_OUTPUT_TARGET = -5;

	// ═══════════════════════════════════════════════════════════════════════════
	// Preview State Constants (COLD/WARM/HOT State Machine)
	// ═══════════════════════════════════════════════════════════════════════════

	/** No USB streaming, preview thread not running */
	public static final int PREVIEW_STATE_COLD = 0;
	/** USB streaming active, no surface - frames drained but not rendered */
	public static final int PREVIEW_STATE_WARM = 1;
	/** USB streaming active + surface available - full rendering */
	public static final int PREVIEW_STATE_HOT = 2;

	// Diagnostic bitmask constants
	/** Preview thread active */
	public static final int DIAG_RUNNING = 0x01;
	/** ANativeWindow attached */
	public static final int DIAG_SURFACE_BOUND = 0x02;
	/** Active Drain mode (WARM state) */
	public static final int DIAG_STATE_WARM = 0x10;
	/** Rendering mode (HOT state) */
	public static final int DIAG_STATE_HOT = 0x20;
	/** Stopped (COLD state) */
	public static final int DIAG_STATE_COLD = 0x40;
	/** No frames processed in >500ms */
	public static final int DIAG_STAGNATION = 0x80;

	//--------------------------------------------------------------------------------
    public static final int	CTRL_SCANNING		= 0x00000001;	// D0:  Scanning Mode
    public static final int CTRL_AE				= 0x00000002;	// D1:  Auto-Exposure Mode
    public static final int CTRL_AE_PRIORITY	= 0x00000004;	// D2:  Auto-Exposure Priority
    public static final int CTRL_AE_ABS			= 0x00000008;	// D3:  Exposure Time (Absolute)
    public static final int CTRL_AR_REL			= 0x00000010;	// D4:  Exposure Time (Relative)
    public static final int CTRL_FOCUS_ABS		= 0x00000020;	// D5:  Focus (Absolute)
    public static final int CTRL_FOCUS_REL		= 0x00000040;	// D6:  Focus (Relative)
    public static final int CTRL_IRIS_ABS		= 0x00000080;	// D7:  Iris (Absolute)
    public static final int CTRL_IRIS_REL		= 0x00000100;	// D8:  Iris (Relative)
    public static final int CTRL_ZOOM_ABS		= 0x00000200;	// D9:  Zoom (Absolute)
    public static final int CTRL_ZOOM_REL		= 0x00000400;	// D10: Zoom (Relative)
    public static final int CTRL_PANTILT_ABS	= 0x00000800;	// D11: PanTilt (Absolute)
    public static final int CTRL_PANTILT_REL	= 0x00001000;	// D12: PanTilt (Relative)
    public static final int CTRL_ROLL_ABS		= 0x00002000;	// D13: Roll (Absolute)
    public static final int CTRL_ROLL_REL		= 0x00004000;	// D14: Roll (Relative)
    public static final int CTRL_FOCUS_AUTO		= 0x00020000;	// D17: Focus, Auto
    public static final int CTRL_PRIVACY		= 0x00040000;	// D18: Privacy
    public static final int CTRL_FOCUS_SIMPLE	= 0x00080000;	// D19: Focus, Simple
    public static final int CTRL_WINDOW			= 0x00100000;	// D20: Window

    public static final int PU_BRIGHTNESS		= 0x80000001;	// D0: Brightness
    public static final int PU_CONTRAST			= 0x80000002;	// D1: Contrast
    public static final int PU_HUE				= 0x80000004;	// D2: Hue
    public static final int PU_SATURATION		= 0x80000008;	// D3: Saturation
    public static final int PU_SHARPNESS		= 0x80000010;	// D4: Sharpness
    public static final int PU_GAMMA			= 0x80000020;	// D5: Gamma
    public static final int PU_WB_TEMP			= 0x80000040;	// D6: White Balance Temperature
    public static final int PU_WB_COMPO			= 0x80000080;	// D7: White Balance Component
    public static final int PU_BACKLIGHT		= 0x80000100;	// D8: Backlight Compensation
    public static final int PU_GAIN				= 0x80000200;	// D9: Gain
    public static final int PU_POWER_LF			= 0x80000400;	// D10: Power Line Frequency
    public static final int PU_HUE_AUTO			= 0x80000800;	// D11: Hue, Auto
    public static final int PU_WB_TEMP_AUTO		= 0x80001000;	// D12: White Balance Temperature, Auto
    public static final int PU_WB_COMPO_AUTO	= 0x80002000;	// D13: White Balance Component, Auto
    public static final int PU_DIGITAL_MULT		= 0x80004000;	// D14: Digital Multiplier
    public static final int PU_DIGITAL_LIMIT	= 0x80008000;	// D15: Digital Multiplier Limit
    public static final int PU_AVIDEO_STD		= 0x80010000;	// D16: Analog Video Standard
    public static final int PU_AVIDEO_LOCK		= 0x80020000;	// D17: Analog Video Lock Status
    public static final int PU_CONTRAST_AUTO	= 0x80040000;	// D18: Contrast, Auto

	// uvc_status_class from libuvc.h
	public static final int STATUS_CLASS_CONTROL = 0x10;
	public static final int STATUS_CLASS_CONTROL_CAMERA = 0x11;
	public static final int STATUS_CLASS_CONTROL_PROCESSING = 0x12;

	// uvc_status_attribute from libuvc.h
	public static final int STATUS_ATTRIBUTE_VALUE_CHANGE = 0x00;
	public static final int STATUS_ATTRIBUTE_INFO_CHANGE = 0x01;
	public static final int STATUS_ATTRIBUTE_FAILURE_CHANGE = 0x02;
	public static final int STATUS_ATTRIBUTE_UNKNOWN = 0xff;

	private static boolean isLoaded;
	static {
		if (!isLoaded) {
			System.loadLibrary("jpeg-turbo1500");
			System.loadLibrary("usb100");
			System.loadLibrary("uvc");
			System.loadLibrary("UVCCamera");
			isLoaded = true;
		}
	}

	private UsbControlBlock mCtrlBlock;
    protected long mControlSupports;			// カメラコントロールでサポートしている機能フラグ
    protected long mProcSupports;				// プロセッシングユニットでサポートしている機能フラグ
    protected int mCurrentFrameFormat = FRAME_FORMAT_MJPEG;
	protected int mCurrentWidth = DEFAULT_PREVIEW_WIDTH, mCurrentHeight = DEFAULT_PREVIEW_HEIGHT;
	protected float mCurrentBandwidthFactor = DEFAULT_BANDWIDTH;
    protected String mSupportedSize;
    protected List<Size> mCurrentSizeList;
	// these fields from here are accessed from native code and do not change name and remove
    protected long mNativePtr;
    protected int mScanningModeMin, mScanningModeMax, mScanningModeDef;
    protected int mExposureModeMin, mExposureModeMax, mExposureModeDef;
    protected int mExposurePriorityMin, mExposurePriorityMax, mExposurePriorityDef;
    protected int mExposureMin, mExposureMax, mExposureDef;
    protected int mAutoFocusMin, mAutoFocusMax, mAutoFocusDef;
    protected int mFocusMin, mFocusMax, mFocusDef;
    protected int mFocusRelMin, mFocusRelMax, mFocusRelDef;
    protected int mFocusSimpleMin, mFocusSimpleMax, mFocusSimpleDef;
    protected int mIrisMin, mIrisMax, mIrisDef;
    protected int mIrisRelMin, mIrisRelMax, mIrisRelDef;
    protected int mPanMin, mPanMax, mPanDef;
    protected int mTiltMin, mTiltMax, mTiltDef;
    protected int mRollMin, mRollMax, mRollDef;
    protected int mPanRelMin, mPanRelMax, mPanRelDef;
    protected int mTiltRelMin, mTiltRelMax, mTiltRelDef;
    protected int mRollRelMin, mRollRelMax, mRollRelDef;
    protected int mPrivacyMin, mPrivacyMax, mPrivacyDef;
    protected int mAutoWhiteBlanceMin, mAutoWhiteBlanceMax, mAutoWhiteBlanceDef;
    protected int mAutoWhiteBlanceCompoMin, mAutoWhiteBlanceCompoMax, mAutoWhiteBlanceCompoDef;
    protected int mWhiteBlanceMin, mWhiteBlanceMax, mWhiteBlanceDef;
    protected int mWhiteBlanceCompoMin, mWhiteBlanceCompoMax, mWhiteBlanceCompoDef;
    protected int mWhiteBlanceRelMin, mWhiteBlanceRelMax, mWhiteBlanceRelDef;
    protected int mBacklightCompMin, mBacklightCompMax, mBacklightCompDef;
    protected int mBrightnessMin, mBrightnessMax, mBrightnessDef;
    protected int mContrastMin, mContrastMax, mContrastDef;
    protected int mSharpnessMin, mSharpnessMax, mSharpnessDef;
    protected int mGainMin, mGainMax, mGainDef;
    protected int mGammaMin, mGammaMax, mGammaDef;
    protected int mSaturationMin, mSaturationMax, mSaturationDef;
    protected int mHueMin, mHueMax, mHueDef;
    protected int mZoomMin, mZoomMax, mZoomDef;
    protected int mZoomRelMin, mZoomRelMax, mZoomRelDef;
    protected int mPowerlineFrequencyMin, mPowerlineFrequencyMax, mPowerlineFrequencyDef;
    protected int mMultiplierMin, mMultiplierMax, mMultiplierDef;
    protected int mMultiplierLimitMin, mMultiplierLimitMax, mMultiplierLimitDef;
    protected int mAnalogVideoStandardMin, mAnalogVideoStandardMax, mAnalogVideoStandardDef;
    protected int mAnalogVideoLockStateMin, mAnalogVideoLockStateMax, mAnalogVideoLockStateDef;
    // until here
    /**
     * the sonctructor of this class should be call within the thread that has a looper
     * (UI thread or a thread that called Looper.prepare)
     */
    public UVCCamera() {
    	mNativePtr = nativeCreate();
    	mSupportedSize = null;
	}

    /**
     * connect to a UVC camera
     * USB permission is necessary before this method is called
     * @param ctrlBlock
     */
    public synchronized void open(final UsbControlBlock ctrlBlock) {
    	int result;
    	try {
			mCtrlBlock = ctrlBlock.clone();
			result = nativeConnect(mNativePtr,
				mCtrlBlock.getVenderId(), mCtrlBlock.getProductId(),
				mCtrlBlock.getFileDescriptor(),
				mCtrlBlock.getBusNum(),
				mCtrlBlock.getDevNum(),
				getUSBFSName(mCtrlBlock));
		} catch (final Exception e) {
			Log.w(TAG, e);
			result = -1;
		}
		if (result != 0) {
			throw new UnsupportedOperationException("open failed:result=" + result);
		}
    	if (mNativePtr != 0 && TextUtils.isEmpty(mSupportedSize)) {
    		mSupportedSize = nativeGetSupportedSize(mNativePtr);
    	}
		nativeSetPreviewSize(mNativePtr, DEFAULT_PREVIEW_WIDTH, DEFAULT_PREVIEW_HEIGHT,
			DEFAULT_PREVIEW_MIN_FPS, DEFAULT_PREVIEW_MAX_FPS, DEFAULT_PREVIEW_MODE, DEFAULT_BANDWIDTH);
    }

	/**
	 * E1-OPT-B-004: Modern FD-based connection (2026 pattern).
	 *
	 * Opens the camera using a pre-authorized file descriptor.
	 * This bypasses UsbControlBlock, giving Kotlin full lifecycle control.
	 *
	 * The usbfsPath should be the device path (e.g., "/dev/bus/usb/001/002").
	 * Bus number and device address are parsed from this path.
	 * VID/PID are not needed - they are read from the FD's device descriptor.
	 *
	 * NOTE: Unlike {@link #open(UsbControlBlock)}, this method does NOT set mCtrlBlock.
	 * As a result:
	 * - {@link #getDevice()}, {@link #getDeviceName()}, {@link #getUsbControlBlock()} return null
	 * - Caller is responsible for managing the UsbDeviceConnection lifecycle
	 * - {@link #close()} will still work correctly (releases native resources)
	 *
	 * @param fd File descriptor from UsbDeviceConnection.getFileDescriptor()
	 * @param usbfsPath Device path, e.g., "/dev/bus/usb/001/002"
	 * @throws UnsupportedOperationException if connection fails
	 * @throws IllegalArgumentException if fd <= 0 or path is null/empty
	 */
	public synchronized void openSimple(final int fd, final String usbfsPath) {
		// Java-level validation (defense in depth)
		if (fd <= 0) {
			throw new IllegalArgumentException("Invalid file descriptor: " + fd);
		}
		if (usbfsPath == null || usbfsPath.isEmpty()) {
			throw new IllegalArgumentException("Invalid usbfs path: null or empty");
		}

		// Call native
		int result = nativeConnectSimple(mNativePtr, fd, usbfsPath);

		if (result != 0) {
			throw new UnsupportedOperationException(
				"openSimple failed: result=" + result + ", fd=" + fd + ", path=" + usbfsPath
			);
		}

		// Note: mCtrlBlock is NOT set when using openSimple
		// Kotlin manages the UsbDeviceConnection lifecycle directly

		// Initialize camera parameters (mirrors open(UsbControlBlock) behavior)
		if (mNativePtr != 0 && TextUtils.isEmpty(mSupportedSize)) {
			mSupportedSize = nativeGetSupportedSize(mNativePtr);
		}
		nativeSetPreviewSize(mNativePtr, DEFAULT_PREVIEW_WIDTH, DEFAULT_PREVIEW_HEIGHT,
			DEFAULT_PREVIEW_MIN_FPS, DEFAULT_PREVIEW_MAX_FPS, DEFAULT_PREVIEW_MODE, DEFAULT_BANDWIDTH);

		if (DEBUG) Log.d(TAG, "openSimple: SUCCESS fd=" + fd + " path=" + usbfsPath);
	}

	/**
	 * set status callback
	 * @param callback
	 */
	public void setStatusCallback(final IStatusCallback callback) {
		if (mNativePtr != 0) {
			nativeSetStatusCallback(mNativePtr, callback);
		}
	}

	/**
	 * set button callback
	 * @param callback
	 */
	public void setButtonCallback(final IButtonCallback callback) {
		if (mNativePtr != 0) {
			nativeSetButtonCallback(mNativePtr, callback);
		}
	}

	/**
	 * set readiness callback
	 * Called when native preview thread is ready and stopPreview() is safe to call
	 * @param callback
	 */
	public void setReadinessCallback(final IReadinessCallback callback) {
		if (mNativePtr != 0) {
			nativeSetReadinessCallback(mNativePtr, callback);
		}
	}

	/**
	 * check if native layer is ready for stopPreview
	 * @return true if the preview thread has started and is ready
	 */
	public boolean isReady() {
		return mNativePtr != 0 && nativeIsReady(mNativePtr);
	}

	/**
	 * Graduated cleanup with levels
	 * @param level CLEANUP_PREVIEW_ONLY (0) to CLEANUP_FULL (3)
	 * @return 0 on success, negative on error
	 */
	public int cleanup(int level) {
		return mNativePtr != 0 ? nativeCleanup(mNativePtr, level) : -1;
	}

	/**
	 * Release USB interface explicitly
	 * @return 0 on success, negative on error
	 */
	public int releaseInterface() {
		return mNativePtr != 0 ? nativeReleaseInterface(mNativePtr) : -1;
	}

	/**
	 * Hard reset - nuclear option for DeviceBusy recovery
	 * Forces cleanup without joining threads
	 * @return 0 on success, negative on error
	 */
	public int hardReset() {
		return mNativePtr != 0 ? nativeHardReset(mNativePtr) : -1;
	}

    /**
     * close and release UVC camera
     */
    public synchronized void close() {
    	stopPreview();
    	if (mNativePtr != 0) {
    		nativeRelease(mNativePtr);
//    		mNativePtr = 0;	// nativeDestroyを呼ぶのでここでクリアしちゃダメ
    	}
    	if (mCtrlBlock != null) {
			mCtrlBlock.close();
   			mCtrlBlock = null;
		}
		mControlSupports = mProcSupports = 0;
		mCurrentFrameFormat = -1;
		mCurrentBandwidthFactor = 0;
		mSupportedSize = null;
		mCurrentSizeList = null;
    	if (DEBUG) Log.v(TAG, "close:finished");
    }

	public UsbDevice getDevice() {
		return mCtrlBlock != null ? mCtrlBlock.getDevice() : null;
	}

	public String getDeviceName(){
		return mCtrlBlock != null ? mCtrlBlock.getDeviceName() : null;
	}

	public UsbControlBlock getUsbControlBlock() {
		return mCtrlBlock;
	}

	public synchronized String getSupportedSize() {
    	return !TextUtils.isEmpty(mSupportedSize) ? mSupportedSize : (mSupportedSize = nativeGetSupportedSize(mNativePtr));
    }

	public Size getPreviewSize() {
		Size result = null;
		final List<Size> list = getSupportedSizeList();
		for (final Size sz: list) {
			if ((sz.width == mCurrentWidth)
				&& (sz.height == mCurrentHeight)) {
				result =sz;
				break;
			}
		}
		return result;
	}
	
	/**
	 * Set preview size and preview mode
	 * @param width
	   @param height
	 */
	public void setPreviewSize(final int width, final int height) {
		setPreviewSize(width, height, DEFAULT_PREVIEW_MIN_FPS, DEFAULT_PREVIEW_MAX_FPS, mCurrentFrameFormat, mCurrentBandwidthFactor);
	}

	/**
	 * Set preview size and preview mode
	 * @param width
	 * @param height
	 * @param frameFormat either FRAME_FORMAT_YUYV(0) or FRAME_FORMAT_MJPEG(1)
	 */
	public void setPreviewSize(final int width, final int height, final int frameFormat) {
		setPreviewSize(width, height, DEFAULT_PREVIEW_MIN_FPS, DEFAULT_PREVIEW_MAX_FPS, frameFormat, mCurrentBandwidthFactor);
	}
	
	/**
	 * Set preview size and preview mode
	 * @param width
	   @param height
	   @param frameFormat either FRAME_FORMAT_YUYV(0) or FRAME_FORMAT_MJPEG(1)
	   @param bandwidth [0.0f,1.0f]
	 */
	public void setPreviewSize(final int width, final int height, final int frameFormat, final float bandwidth) {
		setPreviewSize(width, height, DEFAULT_PREVIEW_MIN_FPS, DEFAULT_PREVIEW_MAX_FPS, frameFormat, bandwidth);
	}

	/**
	 * Set preview size and preview mode
	 * @param width
	 * @param height
	 * @param min_fps
	 * @param max_fps
	 * @param frameFormat either FRAME_FORMAT_YUYV(0) or FRAME_FORMAT_MJPEG(1)
	 * @param bandwidthFactor
	 */
	public void setPreviewSize(final int width, final int height, final int min_fps, final int max_fps, final int frameFormat, final float bandwidthFactor) {
		if ((width == 0) || (height == 0))
			throw new IllegalArgumentException("invalid preview size");
		if (mNativePtr != 0) {
			final int result = nativeSetPreviewSize(mNativePtr, width, height, min_fps, max_fps, frameFormat, bandwidthFactor);
			if (result != 0)
				throw new IllegalArgumentException("Failed to set preview size");
			mCurrentFrameFormat = frameFormat;
			mCurrentWidth = width;
			mCurrentHeight = height;
			mCurrentBandwidthFactor = bandwidthFactor;
		}
	}

	public List<Size> getSupportedSizeList() {
		final int type = (mCurrentFrameFormat > 0) ? 6 : 4;
		return getSupportedSize(type, mSupportedSize);
	}

	public static List<Size> getSupportedSize(final int type, final String supportedSize) {
		final List<Size> result = new ArrayList<Size>();
		if (!TextUtils.isEmpty(supportedSize))
		try {
			final JSONObject json = new JSONObject(supportedSize);
			final JSONArray formats = json.getJSONArray("formats");
			final int format_nums = formats.length();
			for (int i = 0; i < format_nums; i++) {
				final JSONObject format = formats.getJSONObject(i);
				if(format.has("type") && format.has("size")) {
					final int format_type = format.getInt("type");
					if ((format_type == type) || (type == -1)) {
						addSize(format, format_type, 0, result);
					}
				}
			}
		} catch (final JSONException e) {
			e.printStackTrace();
		}
		return result;
	}

	private static final void addSize(final JSONObject format, final int formatType, final int frameType, final List<Size> size_list) throws JSONException {
		final JSONArray size = format.getJSONArray("size");
		final int size_nums = size.length();
		for (int j = 0; j < size_nums; j++) {
			final String[] sz = size.getString(j).split("x");
			try {
				size_list.add(new Size(formatType, frameType, j, Integer.parseInt(sz[0]), Integer.parseInt(sz[1])));
			} catch (final Exception e) {
				break;
			}
		}
	}

    /**
     * set preview surface with SurfaceHolder</br>
     * you can use SurfaceHolder came from SurfaceView/GLSurfaceView
     * @param holder
     */
    public synchronized void setPreviewDisplay(final SurfaceHolder holder) {
    	// V2-HANDLE-002: Guard for openSimple compatibility
    	if (mNativePtr != 0) {
    		nativeSetPreviewDisplay(mNativePtr, holder.getSurface());
    	} else {
    		Log.w(TAG, "setPreviewDisplay(SurfaceHolder): mNativePtr is 0 - camera not initialized");
    	}
    }

    /**
     * set preview surface with SurfaceTexture.
     * this method require API >= 14
     * @param texture
     */
    public synchronized void setPreviewTexture(final SurfaceTexture texture) {	// API >= 11
    	// V2-HANDLE-002: Guard for openSimple compatibility
    	if (mNativePtr != 0) {
    		final Surface surface = new Surface(texture);	// XXX API >= 14
    		nativeSetPreviewDisplay(mNativePtr, surface);
    	} else {
    		Log.w(TAG, "setPreviewTexture: mNativePtr is 0 - camera not initialized");
    	}
    }

    /**
     * set preview surface with Surface
     * @param surface
     */
    public synchronized void setPreviewDisplay(final Surface surface) {
    	// V2-HANDLE-002: Guard for openSimple compatibility
    	if (mNativePtr != 0) {
    		nativeSetPreviewDisplay(mNativePtr, surface);
    	} else {
    		Log.w(TAG, "setPreviewDisplay(Surface): mNativePtr is 0 - camera not initialized");
    	}
    }

    /**
     * set frame callback
     * @param callback
     * @param pixelFormat
     */
    public void setFrameCallback(final IFrameCallback callback, final int pixelFormat) {
    	if (mNativePtr != 0) {
        	nativeSetFrameCallback(mNativePtr, callback, pixelFormat);
    	}
    }

    /**
     * start preview
     * @return 0 on success, negative error code on failure:
     *         -1: unknown error
     *         -2: preview already running
     *         -3: ring buffer mode enabled but not allocated
     *         -4: thread creation failed
     *         -5: no output target (no window and ring buffer not enabled)
     */
    public synchronized int startPreview() {
    	// V2-HANDLE-002: Check mNativePtr instead of mCtrlBlock
    	// openSimple() doesn't set mCtrlBlock, but still initializes mNativePtr
    	if (DEBUG) Log.d(TAG, "startPreview: mNativePtr=0x" + Long.toHexString(mNativePtr) +
    		", mCtrlBlock=" + (mCtrlBlock != null ? "SET" : "NULL"));
    	if (mNativePtr != 0) {
    		int result = nativeStartPreview(mNativePtr);
    		if (DEBUG) Log.d(TAG, "startPreview: nativeStartPreview returned " + result);
    		return result;
    	}
    	Log.w(TAG, "startPreview: SKIPPED - mNativePtr is 0! Camera not initialized.");
    	return PREVIEW_ERROR_UNKNOWN;
    }

    /**
     * stop preview
     */
    public synchronized void stopPreview() {
    	setFrameCallback(null, 0);
    	// V2-HANDLE-002: Check mNativePtr instead of mCtrlBlock
    	// openSimple() doesn't set mCtrlBlock, but still initializes mNativePtr
    	if (mNativePtr != 0) {
    		nativeStopPreview(mNativePtr);
    	}
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Surface Lease API (Persistent Session Model)
    // Enables WARM↔HOT transitions without USB reconnection
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Query the internal diagnostic state of the preview pipeline.
     *
     * <p>Use this to verify state transitions succeeded and detect stagnation.</p>
     *
     * <p><b>Bitmask values:</b></p>
     * <ul>
     *   <li>{@link #DIAG_RUNNING} (0x01): Preview thread active</li>
     *   <li>{@link #DIAG_SURFACE_BOUND} (0x02): Surface attached</li>
     *   <li>{@link #DIAG_STATE_WARM} (0x10): Active Drain mode</li>
     *   <li>{@link #DIAG_STATE_HOT} (0x20): Rendering mode</li>
     *   <li>{@link #DIAG_STATE_COLD} (0x40): Stopped</li>
     *   <li>{@link #DIAG_STAGNATION} (0x80): No frames processed in >500ms</li>
     * </ul>
     *
     * @return Diagnostic bitmask, or -1 if camera not initialized
     */
    public int querySessionDiagnostic() {
        return mNativePtr != 0 ? nativeGetInternalDiagnosticState(mNativePtr) : -1;
    }

    /**
     * Get the current preview state.
     *
     * <p>For detailed diagnostics, use {@link #querySessionDiagnostic()} instead.</p>
     *
     * @return {@link #PREVIEW_STATE_COLD}, {@link #PREVIEW_STATE_WARM},
     *         or {@link #PREVIEW_STATE_HOT}. Returns -1 if not initialized.
     */
    public int getPreviewState() {
        return mNativePtr != 0 ? nativeGetPreviewState(mNativePtr) : -1;
    }

    /**
     * Suspend the surface lease, transitioning from HOT to WARM state.
     *
     * <p>USB streaming continues in Active Drain mode - frames are received
     * and the latest is stashed for instant resume. No rendering occurs.</p>
     *
     * <p><b>Call this BEFORE surface destruction</b> (e.g., in onSurfaceDestroyed,
     * onPause, or when navigating to Gallery) to prevent ANativeWindow hangs.</p>
     *
     * <p><b>Thread Safety:</b> Blocks until render thread acknowledges detachment.</p>
     *
     * <p><b>Verification:</b> After calling, use {@link #querySessionDiagnostic()}
     * to confirm {@link #DIAG_STATE_WARM} is set.</p>
     *
     * @see #acquireSurfaceLease(Surface) to resume rendering
     */
    public void suspendSurfaceLease() {
        if (mNativePtr != 0) {
            nativeDetachSurface(mNativePtr);
        }
    }

    /**
     * Acquire a surface lease, transitioning from WARM to HOT state.
     *
     * <p>Resumes rendering with instant preview - no USB reconnection needed.
     * Uses the stashed frame from WARM state for immediate display.</p>
     *
     * <p><b>Prerequisites:</b></p>
     * <ul>
     *   <li>Camera must be in WARM state (call {@link #suspendSurfaceLease()} first if HOT)</li>
     *   <li>Preview size must be set via {@link #setPreviewSize}</li>
     * </ul>
     *
     * <p><b>Error Handling:</b> If surface geometry setup fails, camera remains
     * in WARM state. Use {@link #querySessionDiagnostic()} to verify
     * {@link #DIAG_STATE_HOT} is set after calling.</p>
     *
     * <p><b>Thread Safety:</b> Blocks until render thread completes surface binding.</p>
     *
     * @param surface The new Surface to render preview frames to (must not be null)
     * @throws NullPointerException if surface is null
     * @see #suspendSurfaceLease() to transition back to WARM
     */
    public void acquireSurfaceLease(final Surface surface) {
        if (surface == null) {
            throw new NullPointerException("acquireSurfaceLease: surface cannot be null");
        }
        if (mNativePtr != 0) {
            nativeAttachSurface(mNativePtr, surface);
        }
    }

    /**
     * destroy UVCCamera object
     */
    public synchronized void destroy() {
    	close();
    	if (mNativePtr != 0) {
    		nativeDestroy(mNativePtr);
    		mNativePtr = 0;
    	}
    }

    // wrong result may return when you call this just after camera open.
    // it is better to wait several hundreads millseconds.
	public boolean checkSupportFlag(final long flag) {
    	updateCameraParams();
    	if ((flag & 0x80000000) == 0x80000000)
    		return ((mProcSupports & flag) == (flag & 0x7ffffffF));
    	else
    		return (mControlSupports & flag) == flag;
    }

//================================================================================
	public synchronized void setAutoFocus(final boolean autoFocus) {
    	if (mNativePtr != 0) {
    		nativeSetAutoFocus(mNativePtr, autoFocus);
    	}
    }

	public synchronized boolean getAutoFocus() {
    	boolean result = true;
    	if (mNativePtr != 0) {
    		result = nativeGetAutoFocus(mNativePtr) > 0;
    	}
    	return result;
    }
//================================================================================
    /**
     * @param focus [%]
     */
	public synchronized void setFocus(final int focus) {
    	if (mNativePtr != 0) {
 		   final float range = Math.abs(mFocusMax - mFocusMin);
 		   if (range > 0)
 			   nativeSetFocus(mNativePtr, (int)(focus / 100.f * range) + mFocusMin);
    	}
    }

    /**
     * @param focus_abs
     * @return focus[%]
     */
	public synchronized int getFocus(final int focus_abs) {
	   int result = 0;
	   if (mNativePtr != 0) {
		   nativeUpdateFocusLimit(mNativePtr);
		   final float range = Math.abs(mFocusMax - mFocusMin);
		   if (range > 0) {
			   result = (int)((focus_abs - mFocusMin) * 100.f / range);
		   }
	   }
	   return result;
	}

    /**
     * @return focus[%]
     */
	public synchronized int getFocus() {
    	return getFocus(nativeGetFocus(mNativePtr));
    }

	public synchronized void resetFocus() {
    	if (mNativePtr != 0) {
    		nativeSetFocus(mNativePtr, mFocusDef);
    	}
    }

//================================================================================
	public synchronized void setAutoWhiteBlance(final boolean autoWhiteBlance) {
    	if (mNativePtr != 0) {
    		nativeSetAutoWhiteBlance(mNativePtr, autoWhiteBlance);
    	}
    }

	public synchronized boolean getAutoWhiteBlance() {
    	boolean result = true;
    	if (mNativePtr != 0) {
    		result = nativeGetAutoWhiteBlance(mNativePtr) > 0;
    	}
    	return result;
    }

//================================================================================
    /**
     * @param whiteBlance [%]
     */
	public synchronized void setWhiteBlance(final int whiteBlance) {
    	if (mNativePtr != 0) {
 		   final float range = Math.abs(mWhiteBlanceMax - mWhiteBlanceMin);
 		   if (range > 0)
 			   nativeSetWhiteBlance(mNativePtr, (int)(whiteBlance / 100.f * range) + mWhiteBlanceMin);
    	}
    }

    /**
     * @param whiteBlance_abs
     * @return whiteBlance[%]
     */
	public synchronized int getWhiteBlance(final int whiteBlance_abs) {
	   int result = 0;
	   if (mNativePtr != 0) {
		   nativeUpdateWhiteBlanceLimit(mNativePtr);
		   final float range = Math.abs(mWhiteBlanceMax - mWhiteBlanceMin);
		   if (range > 0) {
			   result = (int)((whiteBlance_abs - mWhiteBlanceMin) * 100.f / range);
		   }
	   }
	   return result;
	}

    /**
     * @return white blance[%]
     */
	public synchronized int getWhiteBlance() {
    	return getFocus(nativeGetWhiteBlance(mNativePtr));
    }

	public synchronized void resetWhiteBlance() {
    	if (mNativePtr != 0) {
    		nativeSetWhiteBlance(mNativePtr, mWhiteBlanceDef);
    	}
    }
//================================================================================
    /**
     * @param brightness [%]
     */
	public synchronized void setBrightness(final int brightness) {
    	if (mNativePtr != 0) {
 		   final float range = Math.abs(mBrightnessMax - mBrightnessMin);
 		   if (range > 0)
 			   nativeSetBrightness(mNativePtr, (int)(brightness / 100.f * range) + mBrightnessMin);
    	}
    }

    /**
     * @param brightness_abs
     * @return brightness[%]
     */
	public synchronized int getBrightness(final int brightness_abs) {
	   int result = 0;
	   if (mNativePtr != 0) {
		   nativeUpdateBrightnessLimit(mNativePtr);
		   final float range = Math.abs(mBrightnessMax - mBrightnessMin);
		   if (range > 0) {
			   result = (int)((brightness_abs - mBrightnessMin) * 100.f / range);
		   }
	   }
	   return result;
	}

    /**
     * @return brightness[%]
     */
	public synchronized int getBrightness() {
    	return getBrightness(nativeGetBrightness(mNativePtr));
    }

	public synchronized void resetBrightness() {
    	if (mNativePtr != 0) {
    		nativeSetBrightness(mNativePtr, mBrightnessDef);
    	}
    }

//================================================================================
    /**
     * @param contrast [%]
     */
	public synchronized void setContrast(final int contrast) {
    	if (mNativePtr != 0) {
    		nativeUpdateContrastLimit(mNativePtr);
	    	final float range = Math.abs(mContrastMax - mContrastMin);
	    	if (range > 0)
	    		nativeSetContrast(mNativePtr, (int)(contrast / 100.f * range) + mContrastMin);
    	}
    }

    /**
     * @param contrast_abs
     * @return contrast[%]
     */
	public synchronized int getContrast(final int contrast_abs) {
	   int result = 0;
	   if (mNativePtr != 0) {
		   final float range = Math.abs(mContrastMax - mContrastMin);
		   if (range > 0) {
			   result = (int)((contrast_abs - mContrastMin) * 100.f / range);
		   }
	   }
	   return result;
	}

    /**
     * @return contrast[%]
     */
	public synchronized int getContrast() {
    	return getContrast(nativeGetContrast(mNativePtr));
    }

	public synchronized void resetContrast() {
    	if (mNativePtr != 0) {
    		nativeSetContrast(mNativePtr, mContrastDef);
    	}
    }

//================================================================================
    /**
     * @param sharpness [%]
     */
	public synchronized void setSharpness(final int sharpness) {
    	if (mNativePtr != 0) {
 		   final float range = Math.abs(mSharpnessMax - mSharpnessMin);
 		   if (range > 0)
 			   nativeSetSharpness(mNativePtr, (int)(sharpness / 100.f * range) + mSharpnessMin);
    	}
    }

    /**
     * @param sharpness_abs
     * @return sharpness[%]
     */
	public synchronized int getSharpness(final int sharpness_abs) {
	   int result = 0;
	   if (mNativePtr != 0) {
		   nativeUpdateSharpnessLimit(mNativePtr);
		   final float range = Math.abs(mSharpnessMax - mSharpnessMin);
		   if (range > 0) {
			   result = (int)((sharpness_abs - mSharpnessMin) * 100.f / range);
		   }
	   }
	   return result;
	}

    /**
     * @return sharpness[%]
     */
	public synchronized int getSharpness() {
    	return getSharpness(nativeGetSharpness(mNativePtr));
    }

	public synchronized void resetSharpness() {
    	if (mNativePtr != 0) {
    		nativeSetSharpness(mNativePtr, mSharpnessDef);
    	}
    }
//================================================================================
    /**
     * @param gain [%]
     */
	public synchronized void setGain(final int gain) {
    	if (mNativePtr != 0) {
 		   final float range = Math.abs(mGainMax - mGainMin);
 		   if (range > 0)
 			   nativeSetGain(mNativePtr, (int)(gain / 100.f * range) + mGainMin);
    	}
    }

    /**
     * @param gain_abs
     * @return gain[%]
     */
	public synchronized int getGain(final int gain_abs) {
	   int result = 0;
	   if (mNativePtr != 0) {
		   nativeUpdateGainLimit(mNativePtr);
		   final float range = Math.abs(mGainMax - mGainMin);
		   if (range > 0) {
			   result = (int)((gain_abs - mGainMin) * 100.f / range);
		   }
	   }
	   return result;
	}

    /**
     * @return gain[%]
     */
	public synchronized int getGain() {
    	return getGain(nativeGetGain(mNativePtr));
    }

	public synchronized void resetGain() {
    	if (mNativePtr != 0) {
    		nativeSetGain(mNativePtr, mGainDef);
    	}
    }

//================================================================================
    /**
     * @param gamma [%]
     */
	public synchronized void setGamma(final int gamma) {
    	if (mNativePtr != 0) {
 		   final float range = Math.abs(mGammaMax - mGammaMin);
 		   if (range > 0)
 			   nativeSetGamma(mNativePtr, (int)(gamma / 100.f * range) + mGammaMin);
    	}
    }

    /**
     * @param gamma_abs
     * @return gamma[%]
     */
	public synchronized int getGamma(final int gamma_abs) {
	   int result = 0;
	   if (mNativePtr != 0) {
		   nativeUpdateGammaLimit(mNativePtr);
		   final float range = Math.abs(mGammaMax - mGammaMin);
		   if (range > 0) {
			   result = (int)((gamma_abs - mGammaMin) * 100.f / range);
		   }
	   }
	   return result;
	}

    /**
     * @return gamma[%]
     */
	public synchronized int getGamma() {
    	return getGamma(nativeGetGamma(mNativePtr));
    }

	public synchronized void resetGamma() {
    	if (mNativePtr != 0) {
    		nativeSetGamma(mNativePtr, mGammaDef);
    	}
    }

//================================================================================
    /**
     * @param saturation [%]
     */
	public synchronized void setSaturation(final int saturation) {
    	if (mNativePtr != 0) {
 		   final float range = Math.abs(mSaturationMax - mSaturationMin);
 		   if (range > 0)
 			   nativeSetSaturation(mNativePtr, (int)(saturation / 100.f * range) + mSaturationMin);
    	}
    }

    /**
     * @param saturation_abs
     * @return saturation[%]
     */
	public synchronized int getSaturation(final int saturation_abs) {
	   int result = 0;
	   if (mNativePtr != 0) {
		   nativeUpdateSaturationLimit(mNativePtr);
		   final float range = Math.abs(mSaturationMax - mSaturationMin);
		   if (range > 0) {
			   result = (int)((saturation_abs - mSaturationMin) * 100.f / range);
		   }
	   }
	   return result;
	}

    /**
     * @return saturation[%]
     */
	public synchronized int getSaturation() {
    	return getSaturation(nativeGetSaturation(mNativePtr));
    }

	public synchronized void resetSaturation() {
    	if (mNativePtr != 0) {
    		nativeSetSaturation(mNativePtr, mSaturationDef);
    	}
    }
//================================================================================
    /**
     * @param hue [%]
     */
	public synchronized void setHue(final int hue) {
    	if (mNativePtr != 0) {
 		   final float range = Math.abs(mHueMax - mHueMin);
 		   if (range > 0)
 			   nativeSetHue(mNativePtr, (int)(hue / 100.f * range) + mHueMin);
    	}
    }

    /**
     * @param hue_abs
     * @return hue[%]
     */
	public synchronized int getHue(final int hue_abs) {
	   int result = 0;
	   if (mNativePtr != 0) {
		   nativeUpdateHueLimit(mNativePtr);
		   final float range = Math.abs(mHueMax - mHueMin);
		   if (range > 0) {
			   result = (int)((hue_abs - mHueMin) * 100.f / range);
		   }
	   }
	   return result;
	}

    /**
     * @return hue[%]
     */
	public synchronized int getHue() {
    	return getHue(nativeGetHue(mNativePtr));
    }

	public synchronized void resetHue() {
    	if (mNativePtr != 0) {
    		nativeSetHue(mNativePtr, mSaturationDef);
    	}
    }

//================================================================================
	public void setPowerlineFrequency(final int frequency) {
    	if (mNativePtr != 0)
    		nativeSetPowerlineFrequency(mNativePtr, frequency);
    }

	public int getPowerlineFrequency() {
    	return nativeGetPowerlineFrequency(mNativePtr);
    }

//================================================================================
    /**
     * this may not work well with some combination of camera and device
     * @param zoom [%]
     */
	public synchronized void setZoom(final int zoom) {
    	if (mNativePtr != 0) {
 		   final float range = Math.abs(mZoomMax - mZoomMin);
 		   if (range > 0) {
 			   final int z = (int)(zoom / 100.f * range) + mZoomMin;
// 			   Log.d(TAG, "setZoom:zoom=" + zoom + " ,value=" + z);
 			   nativeSetZoom(mNativePtr, z);
 		   }
    	}
    }

    /**
     * @param zoom_abs
     * @return zoom[%]
     */
	public synchronized int getZoom(final int zoom_abs) {
	   int result = 0;
	   if (mNativePtr != 0) {
		   nativeUpdateZoomLimit(mNativePtr);
		   final float range = Math.abs(mZoomMax - mZoomMin);
		   if (range > 0) {
			   result = (int)((zoom_abs - mZoomMin) * 100.f / range);
		   }
	   }
	   return result;
	}

    /**
     * @return zoom[%]
     */
	public synchronized int getZoom() {
    	return getZoom(nativeGetZoom(mNativePtr));
    }

	public synchronized void resetZoom() {
    	if (mNativePtr != 0) {
    		nativeSetZoom(mNativePtr, mZoomDef);
    	}
    }

//================================================================================
	public synchronized void updateCameraParams() {
    	if (mNativePtr != 0) {
    		if ((mControlSupports == 0) || (mProcSupports == 0)) {
        		// サポートしている機能フラグを取得
    			if (mControlSupports == 0)
    				mControlSupports = nativeGetCtrlSupports(mNativePtr);
    			if (mProcSupports == 0)
    				mProcSupports = nativeGetProcSupports(mNativePtr);
    	    	// 設定値を取得
    	    	if ((mControlSupports != 0) && (mProcSupports != 0)) {
	    	    	nativeUpdateBrightnessLimit(mNativePtr);
	    	    	nativeUpdateContrastLimit(mNativePtr);
	    	    	nativeUpdateSharpnessLimit(mNativePtr);
	    	    	nativeUpdateGainLimit(mNativePtr);
	    	    	nativeUpdateGammaLimit(mNativePtr);
	    	    	nativeUpdateSaturationLimit(mNativePtr);
	    	    	nativeUpdateHueLimit(mNativePtr);
	    	    	nativeUpdateZoomLimit(mNativePtr);
	    	    	nativeUpdateWhiteBlanceLimit(mNativePtr);
	    	    	nativeUpdateFocusLimit(mNativePtr);
    	    	}
    	    	if (DEBUG) {
					dumpControls(mControlSupports);
					dumpProc(mProcSupports);
					Log.v(TAG, String.format("Brightness:min=%d,max=%d,def=%d", mBrightnessMin, mBrightnessMax, mBrightnessDef));
					Log.v(TAG, String.format("Contrast:min=%d,max=%d,def=%d", mContrastMin, mContrastMax, mContrastDef));
					Log.v(TAG, String.format("Sharpness:min=%d,max=%d,def=%d", mSharpnessMin, mSharpnessMax, mSharpnessDef));
					Log.v(TAG, String.format("Gain:min=%d,max=%d,def=%d", mGainMin, mGainMax, mGainDef));
					Log.v(TAG, String.format("Gamma:min=%d,max=%d,def=%d", mGammaMin, mGammaMax, mGammaDef));
					Log.v(TAG, String.format("Saturation:min=%d,max=%d,def=%d", mSaturationMin, mSaturationMax, mSaturationDef));
					Log.v(TAG, String.format("Hue:min=%d,max=%d,def=%d", mHueMin, mHueMax, mHueDef));
					Log.v(TAG, String.format("Zoom:min=%d,max=%d,def=%d", mZoomMin, mZoomMax, mZoomDef));
					Log.v(TAG, String.format("WhiteBlance:min=%d,max=%d,def=%d", mWhiteBlanceMin, mWhiteBlanceMax, mWhiteBlanceDef));
					Log.v(TAG, String.format("Focus:min=%d,max=%d,def=%d", mFocusMin, mFocusMax, mFocusDef));
				}
			}
    	} else {
    		mControlSupports = mProcSupports = 0;
    	}
    }

    private static final String[] SUPPORTS_CTRL = {
    	"D0:  Scanning Mode",
    	"D1:  Auto-Exposure Mode",
    	"D2:  Auto-Exposure Priority",
    	"D3:  Exposure Time (Absolute)",
    	"D4:  Exposure Time (Relative)",
    	"D5:  Focus (Absolute)",
    	"D6:  Focus (Relative)",
    	"D7:  Iris (Absolute)",
    	"D8:  Iris (Relative)",
    	"D9:  Zoom (Absolute)",
    	"D10: Zoom (Relative)",
    	"D11: PanTilt (Absolute)",
    	"D12: PanTilt (Relative)",
    	"D13: Roll (Absolute)",
    	"D14: Roll (Relative)",
		"D15: Reserved",
		"D16: Reserved",
		"D17: Focus, Auto",
		"D18: Privacy",
		"D19: Focus, Simple",
		"D20: Window",
		"D21: Region of Interest",
		"D22: Reserved, set to zero",
		"D23: Reserved, set to zero",
    };

    private static final String[] SUPPORTS_PROC = {
		"D0: Brightness",
		"D1: Contrast",
		"D2: Hue",
		"D3: Saturation",
		"D4: Sharpness",
		"D5: Gamma",
		"D6: White Balance Temperature",
		"D7: White Balance Component",
		"D8: Backlight Compensation",
		"D9: Gain",
		"D10: Power Line Frequency",
		"D11: Hue, Auto",
		"D12: White Balance Temperature, Auto",
		"D13: White Balance Component, Auto",
		"D14: Digital Multiplier",
		"D15: Digital Multiplier Limit",
		"D16: Analog Video Standard",
		"D17: Analog Video Lock Status",
		"D18: Contrast, Auto",
		"D19: Reserved. Set to zero",
		"D20: Reserved. Set to zero",
		"D21: Reserved. Set to zero",
		"D22: Reserved. Set to zero",
		"D23: Reserved. Set to zero",
	};

    private static final void dumpControls(final long controlSupports) {
    	Log.i(TAG, String.format("controlSupports=%x", controlSupports));
    	for (int i = 0; i < SUPPORTS_CTRL.length; i++) {
    		Log.i(TAG, SUPPORTS_CTRL[i] + ((controlSupports & (0x1 << i)) != 0 ? "=enabled" : "=disabled"));
    	}
    }

	private static final void dumpProc(final long procSupports) {
    	Log.i(TAG, String.format("procSupports=%x", procSupports));
    	for (int i = 0; i < SUPPORTS_PROC.length; i++) {
    		Log.i(TAG, SUPPORTS_PROC[i] + ((procSupports & (0x1 << i)) != 0 ? "=enabled" : "=disabled"));
    	}
    }

	private final String getUSBFSName(final UsbControlBlock ctrlBlock) {
		String result = null;
		final String name = ctrlBlock.getDeviceName();
		final String[] v = !TextUtils.isEmpty(name) ? name.split("/") : null;
		if ((v != null) && (v.length > 2)) {
			final StringBuilder sb = new StringBuilder(v[0]);
			for (int i = 1; i < v.length - 2; i++)
				sb.append("/").append(v[i]);
			result = sb.toString();
		}
		if (TextUtils.isEmpty(result)) {
			Log.w(TAG, "failed to get USBFS path, try to use default path:" + name);
			result = DEFAULT_USBFS;
		}
		return result;
	}

    // #nativeCreate and #nativeDestroy are not static methods.
    private final native long nativeCreate();
    private final native void nativeDestroy(final long id_camera);

    private final native int nativeConnect(long id_camera, int venderId, int productId, int fileDescriptor, int busNum, int devAddr, String usbfs);
    private final native int nativeConnectSimple(long id_camera, int fileDescriptor, String usbfs);
    private static final native int nativeRelease(final long id_camera);

	private static final native int nativeSetStatusCallback(final long mNativePtr, final IStatusCallback callback);
	private static final native int nativeSetButtonCallback(final long mNativePtr, final IButtonCallback callback);
	private static final native int nativeSetReadinessCallback(final long mNativePtr, final IReadinessCallback callback);
	private static final native boolean nativeIsReady(final long mNativePtr);

	private static final native int nativeCleanup(final long mNativePtr, final int level);
	private static final native int nativeReleaseInterface(final long mNativePtr);
	private static final native int nativeHardReset(final long mNativePtr);

    private static final native int nativeSetPreviewSize(final long id_camera, final int width, final int height, final int min_fps, final int max_fps, final int mode, final float bandwidth);
    private static final native String nativeGetSupportedSize(final long id_camera);
    private static final native int nativeStartPreview(final long id_camera);
    private static final native int nativeStopPreview(final long id_camera);
    private static final native int nativeSetPreviewDisplay(final long id_camera, final Surface surface);
    private static final native int nativeSetFrameCallback(final long mNativePtr, final IFrameCallback callback, final int pixelFormat);

//**********************************************************************
    /**
     * start movie capturing(this should call while previewing)
     * @param surface
     */
    public void startCapture(final Surface surface) {
    	// V2-HANDLE-002: Check mNativePtr instead of mCtrlBlock
    	if (mNativePtr != 0 && surface != null) {
    		nativeSetCaptureDisplay(mNativePtr, surface);
    	} else
    		throw new NullPointerException("startCapture");
    }

    /**
     * stop movie capturing
     */
    public void stopCapture() {
    	// V2-HANDLE-002: Check mNativePtr instead of mCtrlBlock
    	if (mNativePtr != 0) {
    		nativeSetCaptureDisplay(mNativePtr, null);
    	}
    }
    private static final native int nativeSetCaptureDisplay(final long id_camera, final Surface surface);

//**********************************************************************
// Ring buffer support (Phase 4) - Zero-copy AHardwareBuffer streaming
//**********************************************************************

    /**
     * Enable ring buffer mode for decoupled frame streaming.
     * When enabled, frames are written to an AHardwareBuffer ring instead of ANativeWindow.
     * The ring buffer must be allocated first via {@link #allocateRingBuffer(int, int)}.
     *
     * <p><b>Thread Safety:</b> Call from main thread only.</p>
     *
     * @param use true to enable ring buffer mode, false for legacy ANativeWindow mode
     * @return 0 on success, -1 if ring buffer not allocated
     */
    public int setUseRingBuffer(final boolean use) {
        // V2-HANDLE-002: Check mNativePtr instead of mCtrlBlock
        if (mNativePtr != 0) {
            return nativeSetUseRingBuffer(mNativePtr, use);
        }
        return -1;
    }

    /**
     * Allocate the ring buffer with specified dimensions.
     * Call after {@link #setPreviewSize(int, int, int, int, int, float)} with matching dimensions.
     *
     * <p><b>Thread Safety:</b> Call from main thread only.</p>
     * <p><b>Lifecycle:</b> Automatically destroyed when camera is released or explicitly
     * via {@link #destroyRingBuffer()}.</p>
     *
     * @param width Frame width in pixels (must match preview size)
     * @param height Frame height in pixels (must match preview size)
     * @return 0 on success, error code on failure
     */
    public int allocateRingBuffer(final int width, final int height) {
        // V2-HANDLE-002: Check mNativePtr instead of mCtrlBlock
        if (mNativePtr != 0) {
            return nativeAllocateRingBuffer(mNativePtr, width, height);
        }
        return -1;
    }

    /**
     * Destroy the ring buffer and release all resources.
     * Automatically disables ring buffer mode.
     *
     * <p><b>Thread Safety:</b> Call from main thread only. Safe to call even if preview is running.</p>
     */
    public void destroyRingBuffer() {
        // V2-HANDLE-002: Check mNativePtr instead of mCtrlBlock
        if (mNativePtr != 0) {
            nativeDestroyRingBuffer(mNativePtr);
        }
    }

    /**
     * Get the native handle to the ring buffer for consumer access.
     * Use this handle with the nativeFrameBuffer* methods to acquire frames.
     *
     * <p><b>Usage Pattern:</b></p>
     * <pre>{@code
     * long handle = camera.getRingBufferHandle();
     * HardwareBuffer buffer = nativeFrameBufferAcquireBuffer(handle);
     * // ... render buffer ...
     * nativeFrameBufferReleaseBuffer(handle);
     * }</pre>
     *
     * @return Native pointer to FrameBufferRing, or 0 if not allocated
     * @since v2.2 - Updated to work with openSimple() (no mCtrlBlock required)
     */
    public long getRingBufferHandle() {
        // V2-HANDLE-001: Check mNativePtr instead of mCtrlBlock
        // openSimple() doesn't set mCtrlBlock, but still initializes mNativePtr
        if (mNativePtr != 0) {
            return nativeGetRingBufferHandle(mNativePtr);
        }
        return 0;
    }

    /**
     * Invalidates the ring buffer handle, preventing further use-after-free access.
     * Call this during navigation/cleanup before the ring buffer is destroyed.
     */
    public void invalidateRingBufferHandle() {
        if (mNativePtr != 0) {
            nativeInvalidateRingBufferHandle(mNativePtr);
        }
    }

    /**
     * Checks if the ring buffer is still valid (not poisoned or destroyed).
     * @return true if the ring buffer exists and has valid magic headers
     */
    public boolean isRingBufferValid() {
        return mNativePtr != 0 && nativeIsRingBufferValid(mNativePtr);
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Ring Buffer Injection (v2.2 Handle Alignment)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * V2-HANDLE-001: Injects a Kotlin-created FrameBufferRing into the native camera.
     *
     * <p>This enables the 2026 "Kotlin-First" architecture where:</p>
     * <ul>
     *   <li>Kotlin creates and owns the FrameBufferRing</li>
     *   <li>Native produces frames into the ring</li>
     *   <li>Kotlin consumes frames and telemetry from the same ring</li>
     * </ul>
     *
     * <p><b>CRITICAL:</b> This method MUST be called after {@link #open} or
     * {@link #openSimple} and BEFORE {@link #startPreview} to ensure producer
     * and consumer use the same ring buffer.</p>
     *
     * <p>Without this call, native creates its own internal ring buffer, causing:</p>
     * <ul>
     *   <li>Black screen (consumer reads from empty Kotlin ring)</li>
     *   <li>Telemetry shows frames produced but none rendered</li>
     *   <li>Handle alignment mismatch between layers</li>
     * </ul>
     *
     * <p><b>Call Sequence:</b></p>
     * <pre>{@code
     * // 1. Open camera
     * camera.openSimple(fd, usbfsPath);
     *
     * // 2. Create ring buffer in Kotlin
     * long ringHandle = frameBufferManager.createRing(width, height);
     *
     * // 3. INJECT ring into native (required for handle alignment)
     * camera.setFrameBufferRing(ringHandle);
     *
     * // 4. Start preview (frames now flow through shared ring)
     * camera.startPreview();
     * }</pre>
     *
     * @param ringHandle Native pointer to FrameBufferRing created by Kotlin
     * @return 0 on success, negative error code on failure
     * @throws IllegalStateException if camera not initialized
     * @throws IllegalArgumentException if ringHandle is 0
     *
     * @since v2.2 (2026-01-04)
     */
    public synchronized int setFrameBufferRing(long ringHandle) {
        if (mNativePtr == 0) {
            throw new IllegalStateException("Camera not initialized - call open() or openSimple() first");
        }
        if (ringHandle == 0) {
            throw new IllegalArgumentException("Invalid ringHandle: 0");
        }
        if (DEBUG) Log.d(TAG, "setFrameBufferRing: ringHandle=" + ringHandle);
        return nativeSetFrameBufferRing(mNativePtr, ringHandle);
    }

    //================================================================================
    // Telemetry for native layer diagnostics
    //================================================================================

    /**
     * Get the count of frames dropped because the surface was not ready.
     * This metric is useful for diagnosing navigation-related preview issues.
     *
     * @return Number of frames dropped due to surface unavailable
     */
    public long getDroppedNoSurface() {
        return mNativePtr != 0 ? nativeGetDroppedNoSurface(mNativePtr) : 0;
    }

    /**
     * Get the count of frames dropped because the queue was full.
     * This indicates frame conversion thread is too slow for USB data rate.
     *
     * @return Number of frames dropped due to queue overflow
     */
    public long getDroppedQueueFull() {
        return mNativePtr != 0 ? nativeGetDroppedQueueFull(mNativePtr) : 0;
    }

    /**
     * Get the total count of frames processed through preview.
     * This can be used to calculate effective frame rate.
     *
     * @return Total number of frames successfully rendered to surface
     */
    public long getTotalFramesProcessed() {
        return mNativePtr != 0 ? nativeGetTotalFramesProcessed(mNativePtr) : 0;
    }

    /**
     * Check if the native surface is configured and ready for rendering.
     * If true but screen is black, the issue is in Fragment/View layer.
     *
     * @return true if surface is ready, false otherwise
     */
    public boolean isSurfaceReady() {
        return mNativePtr != 0 && nativeIsSurfaceReady(mNativePtr);
    }

    /**
     * Check if the USB file descriptor is still valid.
     * Returns false if the OS has reclaimed the USB connection.
     * Useful for diagnosing disconnections during navigation.
     *
     * @return true if USB FD is valid, false if OS reclaimed it
     */
    public boolean isUsbFdValid() {
        return mNativePtr != 0 && nativeIsUsbFdValid(mNativePtr);
    }

    //======================================================================
    // Capture Callback API (Dual-Emit Architecture)
    //======================================================================

    /**
     * Set the capture callback for dual-emit architecture.
     * This callback receives frames on the conversion thread, independent of the display path.
     * 
     * @param callback ICaptureFrameCallback implementation, or null to clear
     * @return 0 on success, error code on failure
     */
    public int setCaptureCallback(final ICaptureFrameCallback callback) {
        if (mNativePtr != 0) {
            return nativeSetCaptureCallback(mNativePtr, callback);
        }
        return -1;
    }

    /**
     * Set the pixel format for capture callback.
     * 
     * @param format One of ICaptureFrameCallback.CAPTURE_FORMAT_* constants
     * @return 0 on success, error code on failure
     */
    public int setCaptureFormat(final int format) {
        if (mNativePtr != 0) {
            return nativeSetCaptureFormat(mNativePtr, format);
        }
        return -1;
    }

    /**
     * Set target frame rate for capture callback (frame rate decimation).
     * Frames will be dropped to achieve the target rate.
     * 
     * @param targetFps Target frames per second (1-120)
     * @return 0 on success, error code on failure
     */
    public int setCaptureFrameRate(final int targetFps) {
        if (mNativePtr != 0) {
            return nativeSetCaptureFrameRate(mNativePtr, targetFps);
        }
        return -1;
    }

    /**
     * Enable or disable the capture callback.
     * 
     * @param enable true to enable, false to disable
     * @return 0 on success, error code on failure
     */
    public int enableCaptureCallback(final boolean enable) {
        if (mNativePtr != 0) {
            return nativeEnableCaptureCallback(mNativePtr, enable);
        }
        return -1;
    }

    /**
     * Get the number of frames emitted to the capture callback.
     * 
     * @return frame count
     */
    public long getCaptureFramesEmitted() {
        return mNativePtr != 0 ? nativeGetCaptureFramesEmitted(mNativePtr) : 0;
    }

    /**
     * Get the number of frames dropped by the capture callback.
     * 
     * @return dropped frame count
     */
    public long getCaptureFramesDropped() {
        return mNativePtr != 0 ? nativeGetCaptureFramesDropped(mNativePtr) : 0;
    }

    /**
     * Get the number of frames dropped because the callback was busy (slow consumer).
     * 
     * @return busy drop count
     */
    public long getCaptureCallbackBusy() {
        return mNativePtr != 0 ? nativeGetCaptureCallbackBusy(mNativePtr) : 0;
    }

    // Ring buffer support (Phase 4)
    private static final native int nativeSetUseRingBuffer(final long id_camera, final boolean use);
    private static final native int nativeAllocateRingBuffer(final long id_camera, final int width, final int height);
    private static final native void nativeDestroyRingBuffer(final long id_camera);
    private static final native long nativeGetRingBufferHandle(final long id_camera);
    private static final native void nativeInvalidateRingBufferHandle(final long id_camera);
    private static final native boolean nativeIsRingBufferValid(final long id_camera);

    // V2-HANDLE-001: Ring buffer injection for handle alignment
    // JNI: serenegiant_usb_UVCCamera.cpp:2350 - Signature: (JJ)I
    private static final native int nativeSetFrameBufferRing(final long id_camera, final long ringHandle);

    // ═══════════════════════════════════════════════════════════════════════════
    // Preview State Machine - Native Declarations (2026-01-09)
    // ═══════════════════════════════════════════════════════════════════════════
    private static final native int nativeGetPreviewState(final long id_camera);
    private static final native int nativeGetInternalDiagnosticState(final long id_camera);
    private static final native void nativeDetachSurface(final long id_camera);
    private static final native void nativeAttachSurface(final long id_camera, final Surface surface);

    /**
     * Get the negotiated preview FPS from the native layer.
     * @return FPS value (e.g. 30), or 0 if not available
     */
    public int getPreviewFps() {
        return mNativePtr != 0 ? nativeGetPreviewFps(mNativePtr) : 0;
    }

    //================================================================================
    // Telemetry for native layer diagnostics
    //================================================================================
    private static final native long nativeGetDroppedNoSurface(final long id_camera);
    private static final native long nativeGetDroppedQueueFull(final long id_camera);
    private static final native long nativeGetTotalFramesProcessed(final long id_camera);
    private static final native boolean nativeIsSurfaceReady(final long id_camera);
    private static final native boolean nativeIsUsbFdValid(final long id_camera);

    // Capture Callback API (Dual-Emit Architecture)
    private static final native int nativeSetCaptureCallback(final long id_camera, final ICaptureFrameCallback callback);
    private static final native int nativeSetCaptureFormat(final long id_camera, final int format);
    private static final native int nativeSetCaptureFrameRate(final long id_camera, final int targetFps);
    private static final native int nativeEnableCaptureCallback(final long id_camera, final boolean enable);
    private static final native long nativeGetCaptureFramesEmitted(final long id_camera);
    private static final native long nativeGetCaptureFramesDropped(final long id_camera);
    private static final native long nativeGetCaptureCallbackBusy(final long id_camera);
    private static final native int nativeGetPreviewFps(final long id_camera);

    private static final native long nativeGetCtrlSupports(final long id_camera);
    private static final native long nativeGetProcSupports(final long id_camera);

    private final native int nativeUpdateScanningModeLimit(final long id_camera);
    private static final native int nativeSetScanningMode(final long id_camera, final int scanning_mode);
    private static final native int nativeGetScanningMode(final long id_camera);

	private final native int nativeUpdateExposureModeLimit(final long id_camera);
    private static final native int nativeSetExposureMode(final long id_camera, final int exposureMode);
    private static final native int nativeGetExposureMode(final long id_camera);

	private final native int nativeUpdateExposurePriorityLimit(final long id_camera);
    private static final native int nativeSetExposurePriority(final long id_camera, final int priority);
    private static final native int nativeGetExposurePriority(final long id_camera);

	private final native int nativeUpdateExposureLimit(final long id_camera);
    private static final native int nativeSetExposure(final long id_camera, final int exposure);
    private static final native int nativeGetExposure(final long id_camera);

	private final native int nativeUpdateExposureRelLimit(final long id_camera);
    private static final native int nativeSetExposureRel(final long id_camera, final int exposure_rel);
    private static final native int nativeGetExposureRel(final long id_camera);

	private final native int nativeUpdateAutoFocusLimit(final long id_camera);
    private static final native int nativeSetAutoFocus(final long id_camera, final boolean autofocus);
    private static final native int nativeGetAutoFocus(final long id_camera);

    private final native int nativeUpdateFocusLimit(final long id_camera);
    private static final native int nativeSetFocus(final long id_camera, final int focus);
    private static final native int nativeGetFocus(final long id_camera);

    private final native int nativeUpdateFocusRelLimit(final long id_camera);
    private static final native int nativeSetFocusRel(final long id_camera, final int focus_rel);
    private static final native int nativeGetFocusRel(final long id_camera);

    private final native int nativeUpdateIrisLimit(final long id_camera);
    private static final native int nativeSetIris(final long id_camera, final int iris);
    private static final native int nativeGetIris(final long id_camera);

    private final native int nativeUpdateIrisRelLimit(final long id_camera);
    private static final native int nativeSetIrisRel(final long id_camera, final int iris_rel);
    private static final native int nativeGetIrisRel(final long id_camera);

    private final native int nativeUpdatePanLimit(final long id_camera);
    private static final native int nativeSetPan(final long id_camera, final int pan);
    private static final native int nativeGetPan(final long id_camera);

    private final native int nativeUpdatePanRelLimit(final long id_camera);
    private static final native int nativeSetPanRel(final long id_camera, final int pan_rel);
    private static final native int nativeGetPanRel(final long id_camera);

    private final native int nativeUpdateTiltLimit(final long id_camera);
    private static final native int nativeSetTilt(final long id_camera, final int tilt);
    private static final native int nativeGetTilt(final long id_camera);

    private final native int nativeUpdateTiltRelLimit(final long id_camera);
    private static final native int nativeSetTiltRel(final long id_camera, final int tilt_rel);
    private static final native int nativeGetTiltRel(final long id_camera);

    private final native int nativeUpdateRollLimit(final long id_camera);
    private static final native int nativeSetRoll(final long id_camera, final int roll);
    private static final native int nativeGetRoll(final long id_camera);

    private final native int nativeUpdateRollRelLimit(final long id_camera);
    private static final native int nativeSetRollRel(final long id_camera, final int roll_rel);
    private static final native int nativeGetRollRel(final long id_camera);

	private final native int nativeUpdateAutoWhiteBlanceLimit(final long id_camera);
    private static final native int nativeSetAutoWhiteBlance(final long id_camera, final boolean autoWhiteBlance);
    private static final native int nativeGetAutoWhiteBlance(final long id_camera);

    private final native int nativeUpdateAutoWhiteBlanceCompoLimit(final long id_camera);
    private static final native int nativeSetAutoWhiteBlanceCompo(final long id_camera, final boolean autoWhiteBlanceCompo);
    private static final native int nativeGetAutoWhiteBlanceCompo(final long id_camera);

	private final native int nativeUpdateWhiteBlanceLimit(final long id_camera);
    private static final native int nativeSetWhiteBlance(final long id_camera, final int whiteBlance);
    private static final native int nativeGetWhiteBlance(final long id_camera);

	private final native int nativeUpdateWhiteBlanceCompoLimit(final long id_camera);
    private static final native int nativeSetWhiteBlanceCompo(final long id_camera, final int whiteBlance_compo);
    private static final native int nativeGetWhiteBlanceCompo(final long id_camera);

	private final native int nativeUpdateBacklightCompLimit(final long id_camera);
    private static final native int nativeSetBacklightComp(final long id_camera, final int backlight_comp);
    private static final native int nativeGetBacklightComp(final long id_camera);

	private final native int nativeUpdateBrightnessLimit(final long id_camera);
    private static final native int nativeSetBrightness(final long id_camera, final int brightness);
    private static final native int nativeGetBrightness(final long id_camera);

    private final native int nativeUpdateContrastLimit(final long id_camera);
    private static final native int nativeSetContrast(final long id_camera, final int contrast);
    private static final native int nativeGetContrast(final long id_camera);

	private final native int nativeUpdateAutoContrastLimit(final long id_camera);
    private static final native int nativeSetAutoContrast(final long id_camera, final boolean autocontrast);
    private static final native int nativeGetAutoContrast(final long id_camera);

	private final native int nativeUpdateSharpnessLimit(final long id_camera);
    private static final native int nativeSetSharpness(final long id_camera, final int sharpness);
    private static final native int nativeGetSharpness(final long id_camera);

    private final native int nativeUpdateGainLimit(final long id_camera);
    private static final native int nativeSetGain(final long id_camera, final int gain);
    private static final native int nativeGetGain(final long id_camera);

    private final native int nativeUpdateGammaLimit(final long id_camera);
    private static final native int nativeSetGamma(final long id_camera, final int gamma);
    private static final native int nativeGetGamma(final long id_camera);

    private final native int nativeUpdateSaturationLimit(final long id_camera);
    private static final native int nativeSetSaturation(final long id_camera, final int saturation);
    private static final native int nativeGetSaturation(final long id_camera);

    private final native int nativeUpdateHueLimit(final long id_camera);
    private static final native int nativeSetHue(final long id_camera, final int hue);
    private static final native int nativeGetHue(final long id_camera);

	private final native int nativeUpdateAutoHueLimit(final long id_camera);
	private static final native int nativeSetAutoHue(final long id_camera, final boolean autohue);
	private static final native int nativeGetAutoHue(final long id_camera);

	private final native int nativeUpdatePowerlineFrequencyLimit(final long id_camera);
    private static final native int nativeSetPowerlineFrequency(final long id_camera, final int frequency);
    private static final native int nativeGetPowerlineFrequency(final long id_camera);

    private final native int nativeUpdateZoomLimit(final long id_camera);
    private static final native int nativeSetZoom(final long id_camera, final int zoom);
    private static final native int nativeGetZoom(final long id_camera);

    private final native int nativeUpdateZoomRelLimit(final long id_camera);
    private static final native int nativeSetZoomRel(final long id_camera, final int zoom_rel);
    private static final native int nativeGetZoomRel(final long id_camera);

    private final native int nativeUpdateDigitalMultiplierLimit(final long id_camera);
    private static final native int nativeSetDigitalMultiplier(final long id_camera, final int multiplier);
    private static final native int nativeGetDigitalMultiplier(final long id_camera);

	private final native int nativeUpdateDigitalMultiplierLimitLimit(final long id_camera);
    private static final native int nativeSetDigitalMultiplierLimit(final long id_camera, final int multiplier_limit);
    private static final native int nativeGetDigitalMultiplierLimit(final long id_camera);

	private final native int nativeUpdateAnalogVideoStandardLimit(final long id_camera);
    private static final native int nativeSetAnalogVideoStandard(final long id_camera, final int standard);
    private static final native int nativeGetAnalogVideoStandard(final long id_camera);

	private final native int nativeUpdateAnalogVideoLockStateLimit(final long id_camera);
    private static final native int nativeSetAnalogVideoLoackState(final long id_camera, final int state);
    private static final native int nativeGetAnalogVideoLoackState(final long id_camera);

	private final native int nativeUpdatePrivacyLimit(final long id_camera);
    private static final native int nativeSetPrivacy(final long id_camera, final boolean privacy);
    private static final native int nativeGetPrivacy(final long id_camera);

    //**********************************************************************
    // FrameBufferRing native methods (AHardwareBuffer-based streaming)
    //**********************************************************************

    // Lifecycle
    private static native long nativeFrameBufferAllocate(int width, int height, int format);
    private static native void nativeFrameBufferDestroy(long handle);

    // Consumer API
    private static native android.hardware.HardwareBuffer nativeFrameBufferAcquireBuffer(long handle);
    private static native void nativeFrameBufferReleaseBuffer(long handle);

    // Bidirectional Fence API (Phase 4)
    private static native int nativeFrameBufferGetAcquireFence(long handle);
    private static native long nativeFrameBufferGetFrameNumber(long handle);
    private static native void nativeFrameBufferReleaseWithFence(long handle, long frameNumber, int gpuReleaseFenceFd);

    // State queries
    private static native boolean nativeFrameBufferIsAllocated(long handle);
    private static native int nativeFrameBufferGetWidth(long handle);
    private static native int nativeFrameBufferGetHeight(long handle);

    // Telemetry - Basic Counters
    private static native long nativeFrameBufferGetFramesReceived(long handle);
    private static native long nativeFrameBufferGetFramesRendered(long handle);
    private static native long nativeFrameBufferGetFramesDropped(long handle);
    private static native long nativeFrameBufferGetFramesCorrupted(long handle);

    // Telemetry - Ring Buffer State
    private static native long nativeFrameBufferGetProducerStalls(long handle);
    private static native long nativeFrameBufferGetConsumerStarves(long handle);
    private static native int[] nativeFrameBufferGetSlotStates(long handle);

    // Telemetry - Timing
    private static native long nativeFrameBufferGetAvgDecodeTimeUs(long handle);
    private static native long nativeFrameBufferGetFenceWaitTimeNs(long handle);
    private static native long nativeFrameBufferGetTotalFenceWaits(long handle);

    // Telemetry - Error Recording
    private static native void nativeFrameBufferRecordError(long handle, int errorCode, String source);
    private static native int nativeFrameBufferGetErrorCount(long handle);

    // Telemetry - Stream Negotiation
    private static native void nativeFrameBufferSetUsbProtocol(long handle, int endpoint, int altSetting, boolean isIsochronous, int maxPacketSize);
    private static native void nativeFrameBufferSetNegotiatedParams(long handle, int width, int height, int fps);
    private static native int nativeFrameBufferGetFallbackLevel(long handle);
    private static native void nativeFrameBufferSetFallbackLevel(long handle, int level, String reason);

    // Telemetry - USB Layer
    private static native void nativeFrameBufferOnUsbPacket(long handle);
    private static native void nativeFrameBufferOnUsbOverflow(long handle);
    private static native void nativeFrameBufferOnUsbTimeout(long handle);
    private static native long nativeFrameBufferGetUsbPacketsReceived(long handle);
    private static native long nativeFrameBufferGetUsbOverflowErrors(long handle);
    private static native long nativeFrameBufferGetUsbTimeoutErrors(long handle);
}
