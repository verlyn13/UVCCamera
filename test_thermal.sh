#!/bin/bash

# Thermal XU Testing Script
# Tests the thermal extension unit probe implementation

echo "==================================="
echo "Thermal XU Testing Script"
echo "==================================="

# Check for connected device
echo "Checking for connected Android device..."
DEVICE=$(adb devices | grep -E "device$" | head -1 | awk '{print $1}')

if [ -z "$DEVICE" ]; then
    echo "❌ No Android device connected"
    echo "Please connect your Android device with USB debugging enabled"
    exit 1
fi

echo "✅ Device found: $DEVICE"

# Install the app
echo -e "\nInstalling usbCameraTest8 app..."
adb install -r usbCameraTest8/build/outputs/apk/debug/usbCameraTest8-debug.apk

if [ $? -eq 0 ]; then
    echo "✅ App installed successfully"
else
    echo "❌ Failed to install app"
    exit 1
fi

# Launch the thermal verification activity
echo -e "\nLaunching Thermal Verification Activity..."
adb shell am start -n com.serenegiant.usbcameratest8/com.serenegiant.thermal.ThermalVerificationActivity

# Monitor logs
echo -e "\nMonitoring thermal logs (Ctrl+C to stop)..."
echo "Look for the following:"
echo "  - 'ThermalXU' tags for thermal probe logs"
echo "  - 'XUDiscovery' tags for extension unit discovery"
echo "  - Temperature readings in deci-Celsius format"
echo ""
echo "Logs:"
echo "-----"
adb logcat -c  # Clear old logs
adb logcat | grep -E "ThermalXU|XUDiscovery|ThermalVerification"