This is the **Ideal Architecture Specification** for the ScopeCam Ring Buffer system.

This architecture is designed to solve the "Split-Brain" problem by explicitly defining the responsibilities of the Native Layer (Performance/Data) vs. the Kotlin Layer (Control/Safety), and introducing a "Warm State" to handle the Gallery transition seamlessly.

---

# 1. The Ideal Native Architecture (C++)
**Goal:** Robustness, Zero-Copy, and "Surface Independence".

The current native layer is high-performance but fragile because it ties the **USB Data Pump** too tightly to the **Display Surface**. We need to separate them.

### A. The Core Components

1.  **The USB Pump (Producer)**
    *   **Role:** Reads isochronous packets from libuvc.
    *   **Lifecycle:** Bound to the USB File Descriptor (FD).
    *   **Mechanism:** SPSC Queue.
    *   **Change:** This thread should *keep running* even if the Surface is destroyed, discarding frames or pausing the pump if the queue fills up, but maintaining the USB interface claim.

2.  **The Render Loop (Consumer)**
    *   **Role:** Consumes frames from SPSC, converts YUV→RGB, renders to `ANativeWindow`.
    *   **Lifecycle:** Bound to the `Surface`.
    *   **Mechanism:** `AHardwareBuffer` Ring.
    *   **Change:** Needs to support "Hot Swapping" the output window without tearing down the USB Pump.

3.  **The Handle Manager (Safety)**
    *   **Role:** Prevent use-after-free.
    *   **Mechanism:** Instead of returning raw pointers (`jlong`), return a **Generation ID**.
    *   **Logic:** A map of `std::map<int64_t, Context*>` protected by a reader/writer lock. If Kotlin passes an old ID after a teardown, Native returns `INVALID_HANDLE` instead of crashing.

### B. Ideal Native Interface (JNI)

```cpp
// 1. Initialize the USB Pump (The "Warm" State)
// Returns a session handle (Generation ID). Starts reading USB but not rendering.
jlong startCameraSession(int fd, int vendorId, int width, int height);

// 2. Attach the Output Surface (The "Hot" State)
// Can be called/recalled multiple times on the same Session.
// Zero overhead resume.
void setPreviewSurface(jlong sessionHandle, jobject surface);

// 3. Detach Surface (Back to "Warm" State)
// Stops rendering, halts the Ring Buffer consumer, but keeps USB claimed.
// Called when navigating to Gallery.
void detachSurface(jlong sessionHandle);

// 4. Teardown (The "Cold" State)
// Stops USB, joins threads, frees memory.
void stopCameraSession(jlong sessionHandle);

// 5. Zero-Copy Capture
// Trigger a snapshot directly from the Ring Buffer to a JPEG file or Callback
void captureSnapshot(jlong sessionHandle, jstring path);
```

---

# 2. The Ideal Kotlin Architecture
**Goal:** Control Plane Authority and State Persistence.

Kotlin must act as the "Orchestrator," ensuring safety and managing the Android Lifecycle events, while letting Native handle the heavy lifting.

### A. The Three-Tier State Machine

We move away from binary "Connected/Disconnected" to a ternary model:

1.  **COLD (Disconnected):** No USB FD, no Native memory.
2.  **WARM (Session Active):** USB FD valid, Native Threads running, SPSC Queue active (dropping frames or pausing), **No Surface**.
    *   *Context:* This is the state during Gallery navigation.
3.  **HOT (Previewing):** WARM + Surface Attached. Frames flowing to screen.

### B. Architecture Diagram

```
[ UI Layer (Compose) ]
       │
       ▼
[ CameraViewModel ] <--- Observes Telemetry (FPS, Temp)
       │
       ▼
[ SafeUvcCameraManager (The Orchestrator) ]
       │
    (Mutex)
       │
       ├── State Machine (Cold <-> Warm <-> Hot)
       ├── Safety Monitor (Thermal/Power)
       │
       ▼
[ NativeBridge (JNI) ]
       │
       ├── Session Handle (Generation ID)
       │
       ▼
[ C++ Native Layer ]
    ├── USB Pump Thread (Alive in Warm/Hot)
    └── Render Thread (Alive only in Hot)
```

### C. Implementation Strategy for Specific Features

#### 1. Preview (The Ring Buffer)
*   **Kotlin:** does *not* touch frames. It only provides the `Surface`.
*   **Flow:** `openSimpleWithSafety` -> Enters **WARM** -> `setPreviewSurface` -> Enters **HOT**.

#### 2. Smooth Back-and-Forth (Gallery Navigation)
This is the critical fix for your current crash.

*   **User goes to Gallery:**
    1.  `onPause` / `surfaceDestroyed` fires.
    2.  Kotlin calls `detachSurface(handle)`.
    3.  Native layer stops the *Render Thread* but keeps the *USB Pump Thread* alive.
    4.  State moves to **WARM**.
    *   *Result:* Zero latency, USB permissions preserved.

*   **User returns to Camera:**
    1.  `onResume` / `surfaceCreated` fires.
    2.  Kotlin checks state. If **WARM**, calls `setPreviewSurface(surface)`.
    3.  Native layer restarts *Render Thread*.
    4.  State moves to **HOT**.
    *   *Result:* Instant preview, no "connecting..." spinner.

#### 3. Telemetry (Zero Allocation)
*   **Native:** Updates a specific `struct NativeTelemetry` in C++ memory atomicly.
*   **JNI:** Exposes a `getTelemetry(handle, float[] outArray)` method.
*   **Kotlin:** Polling loop (500ms) passes a pre-allocated array to JNI. JNI fills it. No object allocation (Garbage Collection friendly).
*   **ViewModel:** Updates a `StateFlow<FpsState>` derived from that array.

#### 4. Video Capture (Zero Copy)
*   **Concept:** Do not pull frames to Java to encode.
*   **Native:** The Ring Buffer already holds `AHardwareBuffer`.
*   **Implementation:**
    1.  Kotlin configures `MediaCodec` and gets an Input Surface.
    2.  Kotlin passes this Surface to Native via `startRecording(handle, encoderSurface)`.
    3.  Native Render Thread blits the `AHardwareBuffer` to *both* the Preview Window and the Encoder Window using OpenGL ES (glBlitFramebuffer).
    *   *Result:* 1080p60 recording with almost zero CPU load.

#### 5. Photo Capture
*   **Concept:** "Pick" the latest frame from the Ring Buffer.
*   **Implementation:**
    1.  Kotlin calls `capture(handle)`.
    2.  Native locks the "Latest Completed" index in the Ring Buffer.
    3.  Native performs JPEG compression on a background thread.
    4.  Native calls back to Kotlin with the filepath or bytes.

---

# 3. Migration Steps (From Current to Ideal)

You don't need to rewrite C++ immediately. You can simulate the **WARM** state in Kotlin now to fix the crashes.

### Step 1: Implement the "Handle Guard" (Immediate)
Use the `NativeRingBufferHandle` wrapper I provided in the previous V2 code. This simulates the safety of Generation IDs by invalidating the handle on the Kotlin side immediately upon `surfaceDestroyed`.

### Step 2: Decouple Disconnect from Surface Destroy (Immediate)
Modify `SafeUvcCameraManager`:
*   **Current:** `surfaceDestroyed` -> calls `safeDisconnect` (kills everything).
*   **New:** `surfaceDestroyed` -> calls `cameraManager.stopPreview()` (native) but **NOT** `cameraManager.release()`.
*   **New:** `onPause` (Activity) -> calls `safeDisconnect` (full cleanup).
*   *Note:* This requires your Native `stopPreview` to be robust enough to stop rendering without killing the USB handle. If your current native code couples them, you must stick to the full disconnect/reconnect flow but optimize the reconnection speed.

### Step 3: Telemetry Loop
Remove the `ViewModel` dependency on `frames()`. Implement the polling loop in `TelemetryCollector` that reads directly from JNI.

### Step 4: The "Warm" State logic
Implement the logic:
```kotlin
fun onSurfaceDestroyed() {
   // Don't kill the session, just stop rendering
   hardwareMutex.withLock {
       cameraManager.stopPreview() // Native: Stop render thread
       state = Warm
   }
}

fun onSurfaceCreated(surface: Surface) {
   hardwareMutex.withLock {
       if (state == Warm) {
           cameraManager.setPreviewSurface(surface) // Native: Restart render thread
           state = Hot
       } else {
           fullConnect()
       }
   }
}
```

This architecture aligns your Kotlin Control Plane with the physical realities of the Hardware and the Native Data Plane.
