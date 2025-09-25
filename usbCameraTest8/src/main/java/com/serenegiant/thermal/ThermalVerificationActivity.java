package com.serenegiant.thermal;

import android.app.Activity;
import android.hardware.usb.UsbDevice;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import com.serenegiant.usb.DeviceFilter;
import com.serenegiant.usb.USBMonitor;
import com.serenegiant.usb.UVCCamera;
import com.serenegiant.usbcameratest8.R;
import com.serenegiant.widget.UVCCameraTextureView;

import java.lang.reflect.Field;
import java.util.List;

/**
 * Thermal Extension Unit Verification Activity
 * Tests thermal data reading from UVC cameras
 */
public class ThermalVerificationActivity extends Activity {
    private static final String TAG = "ThermalVerification";
    
    static {
        System.loadLibrary("thermal_xu");
    }
    
    // Native methods
    private static native String nativeQuickThermalTest(long idCamera);
    private static native String nativeEnumerateExtensionUnits(long deviceHandle);
    
    private USBMonitor mUSBMonitor;
    private UVCCamera mUVCCamera;
    private UVCCameraTextureView mUVCCameraView;
    private Surface mPreviewSurface;
    
    private TextView mResultText;
    private Button mScanButton;
    private Button mConnectButton;
    
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_thermal_verification);
        
        mResultText = findViewById(R.id.result_text);
        mScanButton = findViewById(R.id.scan_button);
        mConnectButton = findViewById(R.id.connect_button);
        mUVCCameraView = findViewById(R.id.camera_view);
        
        mScanButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                scanThermalData();
            }
        });
        
        mConnectButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                connectCamera();
            }
        });
        
        mUSBMonitor = new USBMonitor(this, mOnDeviceConnectListener);
    }
    
    @Override
    protected void onStart() {
        super.onStart();
        mUSBMonitor.register();
        mUVCCameraView.onResume();
        
        // Check for already connected devices
        checkConnectedDevices();
    }
    
    @Override
    protected void onStop() {
        mUSBMonitor.unregister();
        mUVCCameraView.onPause();
        releaseCamera();
        super.onStop();
    }
    
    @Override
    protected void onDestroy() {
        if (mUSBMonitor != null) {
            mUSBMonitor.destroy();
            mUSBMonitor = null;
        }
        super.onDestroy();
    }
    
    private void checkConnectedDevices() {
        final List<DeviceFilter> filters = DeviceFilter.getDeviceFilters(this, R.xml.device_filter);
        final List<UsbDevice> devices = mUSBMonitor.getDeviceList(filters);
        
        if (devices.size() > 0) {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    mResultText.append("Found " + devices.size() + " USB camera(s)\n");
                    mConnectButton.setEnabled(true);
                }
            });
        }
    }
    
    private void connectCamera() {
        final List<DeviceFilter> filters = DeviceFilter.getDeviceFilters(this, R.xml.device_filter);
        final List<UsbDevice> devices = mUSBMonitor.getDeviceList(filters);
        
        if (devices.size() > 0) {
            mUSBMonitor.requestPermission(devices.get(0));
        } else {
            Toast.makeText(this, "No USB camera found", Toast.LENGTH_SHORT).show();
        }
    }
    
    private void scanThermalData() {
        if (mUVCCamera == null) {
            mResultText.append("Camera not connected\n");
            return;
        }
        
        try {
            // Use reflection to get the native camera pointer
            Field mNativePtrField = UVCCamera.class.getDeclaredField("mNativePtr");
            mNativePtrField.setAccessible(true);
            long cameraId = mNativePtrField.getLong(mUVCCamera);
            
            Log.d(TAG, "Starting thermal scan with camera ID: " + cameraId);
            mResultText.append("Starting thermal scan...\n");
            
            // Call native thermal test
            String result = nativeQuickThermalTest(cameraId);
            
            Log.d(TAG, "Thermal scan result: " + result);
            mResultText.append("Result: " + result + "\n\n");
            
        } catch (Exception e) {
            Log.e(TAG, "Error during thermal scan", e);
            mResultText.append("Error: " + e.getMessage() + "\n");
        }
    }
    
    private void releaseCamera() {
        if (mUVCCamera != null) {
            try {
                mUVCCamera.setStatusCallback(null);
                mUVCCamera.setButtonCallback(null);
                mUVCCamera.close();
                mUVCCamera.destroy();
            } catch (Exception e) {
                Log.e(TAG, "Error releasing camera", e);
            }
            mUVCCamera = null;
        }
    }
    
    private final USBMonitor.OnDeviceConnectListener mOnDeviceConnectListener = new USBMonitor.OnDeviceConnectListener() {
        @Override
        public void onAttach(final UsbDevice device) {
            Log.d(TAG, "USB device attached: " + device.getDeviceName());
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    mResultText.append("USB device attached: " + device.getDeviceName() + "\n");
                }
            });
        }
        
        @Override
        public void onConnect(final UsbDevice device, final USBMonitor.UsbControlBlock ctrlBlock, final boolean createNew) {
            Log.d(TAG, "USB device connected: " + device.getDeviceName());
            
            releaseCamera();
            
            try {
                mUVCCamera = new UVCCamera();
                mUVCCamera.open(ctrlBlock);
                
                Log.d(TAG, "Camera opened successfully");
                
                // Try to set preview size
                try {
                    mUVCCamera.setPreviewSize(UVCCamera.DEFAULT_PREVIEW_WIDTH, UVCCamera.DEFAULT_PREVIEW_HEIGHT,
                        UVCCamera.FRAME_FORMAT_MJPEG);
                } catch (IllegalArgumentException e) {
                    try {
                        mUVCCamera.setPreviewSize(UVCCamera.DEFAULT_PREVIEW_WIDTH, UVCCamera.DEFAULT_PREVIEW_HEIGHT,
                            UVCCamera.FRAME_FORMAT_YUYV);
                    } catch (IllegalArgumentException e2) {
                        Log.e(TAG, "Failed to set preview size", e2);
                    }
                }
                
                // Start preview
                if (mPreviewSurface != null) {
                    mPreviewSurface.release();
                    mPreviewSurface = null;
                }
                
                mPreviewSurface = mUVCCameraView.getSurface();
                if (mPreviewSurface != null) {
                    mUVCCamera.setPreviewDisplay(mPreviewSurface);
                    mUVCCamera.startPreview();
                }
                
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        mResultText.append("Camera connected and preview started\n");
                        mScanButton.setEnabled(true);
                    }
                });
                
            } catch (Exception e) {
                Log.e(TAG, "Error opening camera", e);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        mResultText.append("Error opening camera: " + e.getMessage() + "\n");
                    }
                });
            }
        }
        
        @Override
        public void onDisconnect(final UsbDevice device, final USBMonitor.UsbControlBlock ctrlBlock) {
            Log.d(TAG, "USB device disconnected: " + device.getDeviceName());
            releaseCamera();
            
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    mResultText.append("Camera disconnected\n");
                    mScanButton.setEnabled(false);
                }
            });
        }
        
        @Override
        public void onDettach(final UsbDevice device) {
            Log.d(TAG, "USB device detached: " + device.getDeviceName());
        }
        
        @Override
        public void onCancel(final UsbDevice device) {
            Log.d(TAG, "USB permission cancelled for: " + device.getDeviceName());
        }
    };
}