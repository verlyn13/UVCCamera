
# Phase 0 — Setup & Guardrails (∼30–45 min)

1. **Create a short-lived branch**

```bash
git checkout -b feat/thermal-xu-probe
```

2. **Folder scaffolding (app module)**

```
app/
 └─ src/main/
    ├─ cpp/thermal/            # minimal native module
    │   ├─ CMakeLists.txt
    │   ├─ thermal_xu.cpp
    │   └─ extension_unit_discovery.cpp
    └─ java/.../thermal/
        └─ ThermalVerificationActivity.kt
```

3. **Debug symbols (so logs/addr2line work) — `app/build.gradle.kts`**

```kotlin
android {
  buildTypes {
    getByName("debug") {
      packaging {
        jniLibs.keepDebugSymbols += setOf("**/*.so")
      }
    }
  }
}
```

4. **Crash-safe logging macro for native**

```cpp
#include <android/log.h>
#define THERMAL_LOG(...) __android_log_print(ANDROID_LOG_DEBUG, "ThermalXU", __VA_ARGS__)
```

---

# Phase 1 — “Today” Unblock (unsafe handle + probe harness)

## 1.1 Unsafe device handle extraction (tactical hack)

* File: `UVCCamera/libuvccamera/src/main/jni/ExtensionUnitEnumerator_simple.cpp` (or any native file compiled into the existing .so)
* Add once and **comment with TODO to remove by end of week**:

```cpp
extern "C" JNIEXPORT jlong JNICALL
Java_com_serenegiant_usb_UVCCamera_nativeGetDeviceHandleUnsafe(
    JNIEnv *env, jobject /*thiz*/, jlong id_camera) {
    // DANGER: transient hack for probing XUs; remove after POC.
    auto *camera = reinterpret_cast<void **>(id_camera);
    // Assumes: [vptr][mDeviceHandle] layout; verify once via debugger
    auto device_handle_ptr = *(reinterpret_cast<void **>(reinterpret_cast<char *>(camera) + sizeof(void *)));
    return reinterpret_cast<jlong>(device_handle_ptr);
}
```

**Acceptance check (stepping):**

* Attach debugger, set a breakpoint after call, confirm non-zero `jlong` and stable across several opens.
* If offset is wrong, quickly tweak (log address ranges and bail if null).

## 1.2 Fast path build (skip full Gradle at first)

```bash
cd UVCCamera/libuvccamera/src/main/jni
$ANDROID_NDK_HOME/ndk-build APP_ABI="arm64-v8a armeabi-v7a" -B V=1
# Copy only what you need into app
cp -v libs/arm64-v8a/*.so ../../../../../app/src/main/jniLibs/arm64-v8a/
cp -v libs/armeabi-v7a/*.so ../../../../../app/src/main/jniLibs/armeabi-v7a/
```

## 1.3 Minimal CMake sidecar for your thermal probe

**`app/src/main/cpp/thermal/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.18.1)
project(thermal_xu)

add_library(thermal_xu SHARED
  thermal_xu.cpp
  extension_unit_discovery.cpp
)

add_library(uvccamera SHARED IMPORTED)
set_target_properties(uvccamera PROPERTIES
  IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../../jniLibs/${ANDROID_ABI}/libuvccamera.so"
)

target_include_directories(thermal_xu PRIVATE
  ${CMAKE_SOURCE_DIR}/../../../../UVCCamera/libuvccamera/src/main/jni/libuvc/include
  ${CMAKE_SOURCE_DIR}/../../../../UVCCamera/libuvccamera/src/main/jni/UVCCamera
)

target_link_libraries(thermal_xu uvccamera log)
```

**`app/build.gradle.kts`**

```kotlin
android {
  defaultConfig {
    externalNativeBuild { cmake { cppFlags += listOf("-fno-exceptions", "-fno-rtti") } }
  }
  externalNativeBuild { cmake { path = file("src/main/cpp/thermal/CMakeLists.txt") } }
}
```

## 1.4 Native probe (`thermal_xu.cpp`) — quick scan JSON

* Keep STL out; use C + snprintf.
* Return 2-byte reads for selectors 1..10, unitIds 3..6, and dump hex + LE int.

Skeleton:

```cpp
#include <jni.h>
#include <string.h>
#include <stdio.h>
#include "libuvc/libuvc.h"

extern "C" JNIEXPORT jstring JNICALL
Java_your_pkg_thermal_NativeProbe_nativeQuickThermalTest(JNIEnv* env, jclass, jlong idCamera) {
  char out[2048]; size_t p=0;
  p += snprintf(out+p, sizeof(out)-p, "{");

  // (1) enumerate candidate unitIds/selectors; (2) try GET_CUR; (3) write compact JSON
  // Pseudocode:
  // for unitId in [3..6]:
  //   for sel in [1..10]:
  //     uint8_t buf[2]={0};
  //     int ok = try_xu_get(idCamera, unitId, sel, buf, 2);
  //     if (ok >= 0) { append hex/int16/le/”deciC” }

  p += snprintf(out+p, sizeof(out)-p, "}");
  return env->NewStringUTF(out);
}
```

## 1.5 Kotlin test harness (single Activity)

**`ThermalVerificationActivity.kt`**

```kotlin
class ThermalVerificationActivity : AppCompatActivity() {
  companion object {
    init { System.loadLibrary("thermal_xu") }
    @JvmStatic external fun nativeQuickThermalTest(id: Long): String
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_thermal_verification)

    val cam = UVCCamera()
    // reuse your existing open path
    cam.open(usbDevice)

    val id = UVCCamera::class.java.getDeclaredField("mNativePtr").apply { isAccessible = true }.getLong(cam)
    val res = nativeQuickThermalTest(id)
    findViewById<TextView>(R.id.results).text = res
  }
}
```

**Acceptance criteria (end of “Today”):**

* ✅ App launches the verification activity, prints non-zero JSON with at least one selector reading.
* ✅ You see plausible values (e.g., `int16_le=235` → `23.5 °C` if deciC).
* ✅ No preview regressions.

---

# Phase 2 — “Tomorrow” Hardened Module (no STL, clean build, API seam)

## 2.1 Promote the probe into a minimal reusable API

Create a tiny `ThermalXuApi` wrapper:

```kotlin
object ThermalXuApi {
  init { System.loadLibrary("thermal_xu") }
  external fun tryRead(unitId: Int, selector: Int, cameraId: Long): ByteArray?
}
```

* JNI returns exactly 2 bytes or null.
* **Contract**: “If deci-C, convert as `shortLE / 10.0`.”

## 2.2 Kotlin adapter to your camera manager

```kotlin
class UvcThermalReader(private val uvc: UVCCamera) {
  fun readDeciC(unitId: Int, selector: Int): Int? {
    val id = UVCCamera::class.java.getDeclaredField("mNativePtr")
      .apply { isAccessible = true }.getLong(uvc)
    val b = ThermalXuApi.tryRead(unitId, selector, id) ?: return null
    val v = (b[0].toInt() and 0xFF) or ((b[1].toInt() and 0xFF) shl 8)
    return (v.toShort()).toInt() // int16
  }
  fun readCelsius(unitId: Int, selector: Int): Double? = readDeciC(unitId, selector)?.let { it / 10.0 }
}
```

## 2.3 Identify the winning `(unitId, selector)` pair

* Persist the pair in app `SharedPreferences` once a stable reading is found (fallback to quick scan on first boot/new camera VID\:PID).

**Acceptance criteria (tomorrow):**

* ✅ A single call `readCelsius(savedUnit, savedSel)` returns steady values for 60s.
* ✅ UI field shows °C with 1 decimal; updates at 1–2 Hz.
* ✅ If no thermal XU present, UI shows “N/A” without crashing.

---

# Phase 3 — “This Week” Safe Integration & Debt Paydown

## 3.1 Replace the unsafe handle path

* **Goal**: remove pointer arithmetic breach.
* Options (pick one this week, keep the other as backlog):

  * **A. Tactical fork (preferred this week)**: copy the minimal UVCCamera C++ files that expose `uvc_device_handle_t*` behind a new friend/bridge with a legitimate accessor (same ABI for rest).
  * **B. Pure-USB path**: read XU via `UsbDeviceConnection.controlTransfer` from Kotlin (no native), using UVC class requests. (Keep for next sprint if time is tight.)

**Acceptance criteria:**

* ✅ App compiles and runs with **no** `nativeGetDeviceHandleUnsafe` references.
* ✅ Same readings as Phase 2.

## 3.2 Build system convergence (no mixed “mystery” builds)

* Keep **CMake** sidecar as the authoritative build for `thermal_xu`.
* Document “How to update libuvccamera.so” (one README with the exact ndk-build command and copy steps).
* Add `./gradlew :app:externalNativeBuildDebug` CI step to validate the thermal module always compiles.

## 3.3 Verification Matrix (automated quick check)

Add an instrumentation test (or a developer action) that:

* Scans unitIds 3–6, selectors 1–10 once.
* Writes a compact report to `Logcat` and a JSON file in `getExternalFilesDir("diagnostics")/thermal_scan.json`.
* Confirms chosen pair is within the discovered set.

**Pass/fail gates:**

* ✅ File exists and contains at least one non-zero selector result for supported cameras.
* ✅ The chosen pair reproduces within 1% across 10 reads (basic stability test).

## 3.4 UI/UX hooks

* Small status row in your preview screen:

  * “Thermal: 23.7 °C” (updates every 500–1000 ms, debounce to avoid GC churn).
  * If absent: “Thermal: Not available.”

## 3.5 Observability

* Add a guarded logger:

```kotlin
if (BuildConfig.DEBUG) {
  Log.d("Thermal", "unit=$unit sel=$sel deciC=$v")
}
```

* **No logs** in release except first-boot detection.

## 3.6 Rollback levers

* Feature flag `thermal.enabled` (bool in `local.properties` read at runtime or in app settings).
* “Reset detection” button to force re-scan (clears saved (unit,selector)).

---

# Reference Checklists

## A) Daily DoD (Definition of Done) — You

* [ ] App opens camera preview with **no regression** to frame rate/latency.
* [ ] Thermal test activity returns JSON with plausible values.
* [ ] Stable `(unit, selector)` stored per camera VID\:PID.
* [ ] Crash-free disconnect/reconnect cycles ×3.
* [ ] Release build strips logs and symbols as usual.

## B) Artifacts to Commit

* [ ] `app/src/main/cpp/thermal/**`
* [ ] `ThermalVerificationActivity.kt` (dev-only; wire behind a debug menu)
* [ ] `UvcThermalReader.kt` (production path)
* [ ] `README_thermal.md` with:

  * ndk-build one-liner for updating libuvccamera.so
  * How to run the verification activity
  * Known good `(unit,selector)` for your test camera(s)
* [ ] CI job: `./gradlew :app:externalNativeBuildDebug`

## C) Known Risks & Mitigations

* **ABI mismatch / offsets** → *Limit* the unsafe hack to Phase 1; delete it in Phase 3. Validate with debugger.
* **Vendor XU variability** → Scan 3–6 unitIds, 1–10 selectors; persist the winner per VID\:PID.
* **Throughput contention** → Poll at ≤2 Hz; never on UI thread. Use a single worker (shared with stats).
* **STL/NDK headaches** → Keep C codepaths, avoid `<string>`, `<vector>`, exceptions/RTTI disabled.

---

# “Do These Next” (copy/paste into an issue)

**Epic: Thermal XU Proof → Integration (Week 1)**

* [ ] **P1** Add unsafe handle JNI + verification activity; prove non-zero thermal read on target camera
  *Acceptance*: JSON shows at least one selector with non-zero bytes; human-readable °C plausible.
* [ ] **P1** Create `thermal_xu` CMake module; expose `tryRead(unit,sel,cameraId)` JNI
  *Acceptance*: Kotlin can read bytes from selected pair on demand.
* [ ] **P1** Wire `UvcThermalReader` into preview screen; 1–2 Hz updates; graceful “N/A” fallback
  *Acceptance*: No jank; GC stable; FPS unaffected.
* [ ] **P2** Replace unsafe accessor by either (A) tiny fork with proper accessor or (B) pure-USB controlTransfer path
  *Acceptance*: No references to `nativeGetDeviceHandleUnsafe`.
* [ ] **P2** Add verification matrix output to `externalFilesDir/diagnostics/thermal_scan.json`
  *Acceptance*: File produced on first run; includes chosen pair.
* [ ] **P3** CI: build native module; document ndk-build refresh path for libuvccamera.so
  *Acceptance*: CI green; README\_thermal.md complete.

---

# Quick Commands Recap

```bash
# Build UVCCamera native fast (Phase 1)
cd UVCCamera/libuvccamera/src/main/jni
$ANDROID_NDK_HOME/ndk-build APP_ABI="arm64-v8a armeabi-v7a" -B V=1
cp -v libs/arm64-v8a/*.so ../../../../../app/src/main/jniLibs/arm64-v8a/
cp -v libs/armeabi-v7a/*.so ../../../../../app/src/main/jniLibs/armeabi-v7a/

# Build your thermal module via Gradle
cd <project-root>
./gradlew :app:externalNativeBuildDebug :app:installDebug

# Logcat filter
adb logcat | grep ThermalXU
```

---

# Success Criteria for “This Week”

* **Functional**: Live temperature visible in preview UI (or “N/A” if absent), stable over multiple connects, no preview regression.
* **Technical Debt Controlled**: Unsafe accessor **removed** or fully quarantined behind a clean forked accessor.
* **Repeatable**: CI validates native build; README documents update flow; a JSON diagnostic is saved on first run.
* **User-Facing**: Minimal, clear thermal status; non-blocking; opt-out flag present.

If you want, I can draft the exact `thermal_xu.cpp` and the tiny `try_xu_get(...)` helper next, using only C + `uvc_get_proc_units` style calls and a 2-byte GET\_CUR.

