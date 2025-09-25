# Thermal XU Integration Guide

This guide documents how to execute the plan in `uvccamera-update.md` within this repository’s structure. It maps the plan’s generic “app” layout to our modules, outlines build commands, and defines acceptance criteria and checkpoints.

## Module Mapping
- Library (native/JNI): `lib/src/main/jni/**` builds `libUVCCamera.so` via ndk-build.
- Common code: `usbCameraCommon/**` (Java helpers, views, handlers).
- App harness (choose one sample): use `usbCameraTest8/` as the verification app.

## Phase 0 — Setup
- Branch: `git checkout -b feat/thermal-xu-probe`.
- App scaffolding (mapped): `usbCameraTest8/src/main/cpp/thermal/` with `CMakeLists.txt`, `thermal_xu.cpp`, `extension_unit_discovery.cpp`.
- Debug symbols (Groovy): in `usbCameraTest8/build.gradle` add
  - `android { buildTypes { debug { packagingOptions { doNotStrip '**/*.so' } } } }`.

## Phase 1 — Unblock POC
- Unsafe accessor (temporary): add JNI to `lib/src/main/jni/UVCCamera/ExtensionUnitEnumerator_simple.cpp` (as in the plan) and clearly mark TODO to remove.
- Fast native build (either):
  - Gradle: `./gradlew :lib:externalNativeBuildDebug`.
  - Direct NDK: `cd lib/src/main/jni && $ANDROID_NDK_HOME/ndk-build APP_ABI="arm64-v8a armeabi-v7a" -B V=1`.
- Copy .so for app-side import (sidecar CMake):
  - `usbCameraTest8/src/main/jniLibs/arm64-v8a/libUVCCamera.so`
  - `usbCameraTest8/src/main/jniLibs/armeabi-v7a/libUVCCamera.so`
- Sidecar CMake in `usbCameraTest8/src/main/cpp/thermal/CMakeLists.txt` that builds `thermal_xu` and IMPORTs `libUVCCamera.so` from `jniLibs/${ANDROID_ABI}`. Link with `log`.
- App wiring:
  - Add a simple Verification Activity (Java is fine in this module; Kotlin optional) that loads `thermal_xu` and calls the native probe.

Acceptance (Day 1):
- Activity prints non-empty JSON with at least one selector reading; no preview regressions.

## Phase 2 — Harden
- API seam: expose `tryRead(unitId, selector, cameraId)` JNI that returns 2 bytes or null.
- Adapter: `UvcThermalReader` converts LE-int16 deciC → Celsius, updates UI 1–2 Hz.
- Persist winning `(unit, selector)` per VID:PID, fallback to quick scan on first run.

## Phase 3 — Integrate & Clean
- Remove unsafe accessor by either a minimal accessor fork in `UVCCamera` JNI or a pure USB `controlTransfer` path.
- Add a verification scan that logs to Logcat and writes `externalFilesDir/diagnostics/thermal_scan.json`.
- CI guard: `./gradlew :usbCameraTest8:externalNativeBuildDebug` and unit tests pass.

## Notes & Paths
- Include dirs for CMake: `lib/src/main/jni/libuvc/include` and `lib/src/main/jni/UVCCamera`.
- Shared lib name in this repo is `libUVCCamera.so` (not `libuvccamera.so`). Adjust IMPORTED_LOCATION accordingly.
- Keep NDK code C/C-only (no STL, no exceptions, no RTTI).

## Current Implementation Status (Updated: Phase 1 Complete)

### ✅ Completed
- **Phase 0**: Branch created, folder structure set up, debug symbols configured
- **Phase 1**: Unsafe accessor implemented (ExtensionUnitEnumerator_unsafe.cpp), thermal probe functional
- **Native Implementation**: thermal_xu.cpp scans unit IDs 3-6, selectors 1-10
- **Cross-Architecture Support**: Works on arm64-v8a, armeabi-v7a, gracefully handles x86/x86_64
- **Verification Activity**: ThermalVerificationActivity.java with live preview and scan button
- **Test Integration**: test_thermal.sh script for standalone testing

### 🔄 In Progress
- Integration with unified test framework (unified_test_session.sh)
- Documentation updates for consistency

### 📋 Pending
- **Phase 2**: Hardened API with ThermalXuApi wrapper
- **Phase 3**: Remove unsafe accessor, implement clean solution

## Quick Commands
```bash
# Build native core with unsafe accessor
cd lib/src/main/jni && ~/Android/Sdk/ndk/28.0.12674087/ndk-build APP_ABI="arm64-v8a armeabi-v7a" -B V=1

# Copy libraries to test app
cp -v lib/src/main/libs/arm64-v8a/*.so usbCameraTest8/src/main/jniLibs/arm64-v8a/
cp -v lib/src/main/libs/armeabi-v7a/*.so usbCameraTest8/src/main/jniLibs/armeabi-v7a/

# Build & install verification app
./gradlew :usbCameraTest8:assembleDebug
adb install -r usbCameraTest8/build/outputs/apk/debug/usbCameraTest8-debug.apk

# Run thermal test
./test_thermal.sh

# Monitor thermal logs
adb logcat | grep -E "ThermalXU|XUDiscovery|ThermalVerification"
```

## Integration with Unified Testing

The thermal XU probe is integrated with the unified test framework:

### Standalone Thermal Testing
```bash
./test_thermal.sh  # Quick thermal-only test
```

### Integrated Testing (with ScopeCam)
```bash
cd /home/verlyn13/Projects/ScopeTechGtHb/scopecam
./scripts/unified_test_session.sh  # Includes thermal monitoring
```

## Test Output Format

The thermal probe returns JSON with discovered temperature data:
```json
{
  "scan_result": {
    "units": [
      {
        "unit_id": 3,
        "selectors": [
          {
            "selector": 1,
            "hex": "ea00",
            "int16_le": 234,
            "deciC": 234,
            "celsius": 23.4
          }
        ]
      }
    ],
    "total_reads": 1,
    "status": "success"
  }
}
```
