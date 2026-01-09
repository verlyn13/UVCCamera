# ScopeCam / UVCCamera 2026 "Scientific-Grade" Imaging Pipeline

> **Status**: FINALIZED — All major architectural decisions locked.  
> **Last Updated**: 2026-01-07

This architecture proposal matches the existing implementation (UYVY→RGBX conversion thread + AHardwareBuffer triple-buffer ring + acquire/release fences) and incorporates the "2026 bullet-proof" decisions (GPU surface-to-surface recording, single GL owner compositor, drop-on-fence-timeout, low-jitter signaling, error containment). Optimized for **deterministic behavior, data integrity, and graceful failure** on **flagship Android devices**.

---

## Finalized Architectural Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| **minSdk** | 26 | Enables AHardwareBuffer, ImageReader with usage flags, native fences |
| **Fence Timeout Policy** | Drop frame | Scientific-grade integrity; no corrupted data reaches any consumer |
| **GL Context Model** | Single Owner (RendererHolder) | Centralizes fence policy; ensures Preview, Recording, AI Analysis never desync |
| **Snapshot Mechanism** | ImageReader surface | Zero-copy, async, no GPU pipeline stalls (API 26+) |
| **Color Space** | BT.601 limited range (assumed) | Cameras provide NO metadata; use standard USB camera coefficients |
| **Thermal Policy** | Drop frames at source | Preserve data integrity over continuity under thermal pressure |

### Color Space Details (Camera Testing Results)

| Camera | Pixel Format | Colorspace Metadata |
|--------|--------------|---------------------|
| 0BDA:5880 | UYVY (YUV 4:2:2) | ❌ None provided |
| 0BDA:5830 | UYVY (YUV 4:2:2) | ❌ None provided |

**Assumed values** (de facto standard for generic USB cameras):
- Color Primaries: BT.601 (SMPTE 170M)
- Transfer Function: ~gamma 2.2
- Matrix Coefficients: BT.601 (Rec.601)
- Range: Limited (16-235 for Y, 16-240 for UV)

**Conversion coefficients**:
```
R = Y + 1.402 × (V - 128)
G = Y - 0.344 × (U - 128) - 0.714 × (V - 128)
B = Y + 1.772 × (U - 128)
```

---

## Core principles (explicit design constraints)

1. **USB ingest must never block** (hard realtime constraint).
2. **No undefined frame state**: never overwrite a buffer still in use by GPU (no tearing/corruption).
3. **Single authoritative frame bus**: one canonical GPU-visible representation (RGBX in AHardwareBuffer).
4. **Single GL owner**: one compositor thread owns EGL + all output passes (display/encode/snapshot).
5. **Fail by dropping frames, not by stalling**: preserve temporal integrity over continuity.
6. **Every stall/drop is measurable**: telemetry is part of correctness.
---

## High-level topology

**USB callback (native)**
→ **Pending SPSC queue (native, CPU)**
→ **Conversion thread (native, CPU)**
→ **AHardwareBuffer triple-buffer ring (native, GPU-shared)**
→ **GL compositor thread (Kotlin/NDK, single EGL owner)**
→ outputs:

* **Preview Surface** (display)
* **MediaCodec input Surface** (recording)
* **Snapshot surface** (optional: ImageReader/Surface)

---

## Stage-by-stage design

### Stage 1 — USB Callback (native, RT-safe)

**Responsibilities**

* Copy UYVY bytes into a reusable pending-slot buffer
* enqueue into SPSC queue
* signal conversion thread via eventfd

**Hard rules**

* no locks (other than atomics)
* no JNI
* no allocations in steady state
* drop immediately if queue full

**Telemetry**

* `framesReceived`
* `framesDroppedQueueFull`

✅ This matches your current implementation.

---

### Stage 2 — Pending SPSC Queue (native, CPU)

**Responsibilities**

* buffer reuse and slot canaries (debug safety)
* bounded queue depth (8+)

**Policy**

* overflow drops newest frame (current behavior) OR newest enqueue fails (effectively same)
* keep it SPSC and keep it simple

**Telemetry**

* `framesDroppedQueueFull`
* optional: queue depth watermark / histogram

✅ This matches your current implementation.

---

### Stage 3 — Conversion Thread (native, CPU) → “Producer”

**Responsibilities**

* dequeue pending UYVY
* acquire a writable AHardwareBuffer slot from ring
* perform UYVY→RGBX conversion into that buffer (stride-aware)
* unlock AHardwareBuffer to produce **CPU→GPU acquire fence**
* publish as “latest completed”

#### Key updated policy (quality-first)

**Fence wait timeout must NOT proceed.**

* Today you “proceed anyway” after 33ms, risking corruption.
* Updated rule:

  * if GPU release fence isn’t signaled by timeout → **do not write this slot**
  * **drop** this pending frame and move on
  * record `framesDroppedFenceTimeout` and `producerStalls` separately

This one change is the biggest “scientific-grade” correctness improvement.

**Telemetry additions**

* `framesDroppedFenceTimeout`
* `fenceWaitNs` histogram
* `framesDroppedLockFailed`

---

### Stage 4 — AHardwareBuffer Triple-Buffer Ring (native, GPU-shared)

**Format**

* canonical bus format: **RGBA_8888 / RGBX** in AHardwareBuffer

**Semantics**

* MAILBOX (latest completed) + triple buffering
* safe overwrite only after GPU release fence indicates all GPU reads are done

**Metadata per slot**

* `acquireFenceFd` (CPU→GPU)
* `gpuReleaseFenceFd` (GPU→CPU)
* `frameNumber`, timestamp, valid flag
* (optional) `colorSpace`/`range` metadata if you want auditing

**Producer acquire policy (updated)**

* wait for release fence up to *budget* (e.g., 33ms at 30fps)
* if not signaled: **skip this slot** and **drop this pending frame** (integrity)
* never “overwrite anyway”

---

### Stage 5 — GL Compositor (single EGL owner, Kotlin+NDK) → “Consumer”

This is where your “B” choice becomes real.

#### Ownership

One dedicated thread owns:

* EGLDisplay/EGLContext/EGLSurfaces
* all GL objects (textures, FBOs, shaders)
* all output surfaces (preview surface, encoder surface, snapshot surface)

No other thread touches GL.

#### Per-frame loop (deterministic “one acquire, N outputs”)

For each tick:

1. `acquireReadBuffer()` → AHardwareBuffer* + `acquireFenceFd` + `frameNumber`
2. import & GPU-wait on acquire fence:

   * create EGLSync from native fence FD
   * `eglWaitSyncKHR(...)`
3. create/bind EGLImage to GL texture (cache per slot to avoid churn)
4. **render passes**:

   * Pass A: draw to display surface
   * Pass B: if recording enabled, draw to encoder input surface
   * Pass C: if snapshot requested, draw to snapshot surface/FBO
5. `glFlush()` (or `eglSwapBuffers` does implicit flush but keep it explicit for fence placement)
6. create **one** GPU release fence after all passes:

   * `eglCreateSyncKHR(EGL_SYNC_NATIVE_FENCE_ANDROID)`
   * `glFlush()` then `eglDupNativeFenceFDANDROID`
7. `releaseWithFence(frameNumber, fenceFd)`

#### Output strategy (stable + simple)

* **Preview**: render with vsync scheduling (Choreographer / fixed tick)
* **Encoder**: render-to-surface; if encoder surface is absent or error → skip pass
* **Snapshot**: render into an ImageReader surface or an RGBA FBO + readback (prefer ImageReader surface path if you want to avoid readPixels stalls)

#### Error containment

* Any failure in encoder pass → remove encoder surface from output list; emit `RecordingError`; preview continues.
* Any EGL surface recreation → compositor recreates surface-specific objects; ring buffer remains stable.

**Telemetry**

* `consumerStarves`
* `renderTimeNs`
* `encoderRenderSkipped`
* `eglErrors`
* `framesRenderedPreview`, `framesRenderedEncoder`, `framesRenderedSnapshot`

---

## Recording pipeline (GPU surface-to-surface)

### Components

* `UvcCameraService` (Kotlin): owns MediaCodec lifecycle + muxer (MediaMuxer)
* `HardwareBufferRenderer` (compositor): owns the “draw into encoder surface” pass

### Flow

1. Service configures MediaCodec encoder:

   * input: `createInputSurface()`
   * output: ByteBuffers → MediaMuxer
2. Service passes encoder input surface to compositor as an “OutputTarget”
3. Compositor includes encoder pass in its per-frame render loop
4. If encoder stalls:

   * compositor can drop encoder rendering for that frame
   * or keep rendering but tolerate queueing inside codec
   * (for integrity, prefer “skip encoder pass if backpressured” once you can detect it reliably)
5. If codec errors:

   * service notifies renderer to detach encoder surface
   * stop muxer cleanly
   * preview unaffected

---

## Snapshot capture (quality-safe)

Two stable options:

### Option S1 (preferred): snapshot surface

* add an ImageReader surface (or similar) as an output target
* compositor draws same texture to it when requested
* avoids `glReadPixels` stalls on the compositor thread

### Option S2: FBO + readback

* compositor draws to an FBO and reads back
* simplest but can introduce GPU→CPU sync stalls (less deterministic)

---

## Scheduling and determinism

### Conversion thread wake strategy (updated)

Your current 100ms poll is too jittery for “scientific-grade.”

**Proposed**:

* `poll(eventfd, timeout = 8–16ms)` OR
* `poll(eventfd, timeout = -1)` plus a dedicated shutdown FD (eventfd/pipe) so you can wake on shutdown immediately

This eliminates the “100ms worst-case latency” behavior.

### Compositor pacing

* preview should be vsync-paced (or fixed cadence for lab mode)
* recording can be decoupled by optionally skipping encoder pass when necessary

---

## Data integrity guarantees (what you can claim, concretely)

With “drop-on-fence-timeout” and a single compositor thread:

* A buffer is never overwritten until the GPU has signaled it finished reading it.
* A frame’s pixel content is either:

  * fully valid and coherently rendered/encoded, or
  * dropped (never partially used).
* Encoder failures cannot corrupt preview pipeline.

This is the right story for “scientific-grade.”

---

## Architecture Decision Records (ADRs)

### ADR-1: Fence Timeout Budget ✅ DECIDED

**Decision**: `min(frameIntervalMs, 33ms)` - one frame period, max 33ms

| Frame Rate | Timeout |
|------------|---------|
| 30 fps | 33ms |
| 60 fps | 16ms |

**Rationale**: Timeout should never exceed one frame period. At 30fps, 33ms gives the GPU exactly one frame to complete. At 60fps, tighten to 16ms.

---

### ADR-2: Frame Rate Modes ⏳ PENDING

**Open questions**:
- Fixed vs variable frame rate support?
- Preview fps independent of recording fps?
- Telemetry sampling rate?

---

### ADR-3: Color Accuracy Contract ✅ DECIDED

**Decision**: BT.601 limited range, no runtime detection

| Property | Value |
|----------|-------|
| Color Primaries | BT.601 (SMPTE 170M) |
| Transfer Function | ~gamma 2.2 (assumed) |
| Matrix Coefficients | BT.601 (Rec.601) |
| Range | Limited (Y: 16-235, UV: 16-240) |

**Rationale**: Target cameras (0BDA:5880, 0BDA:5830) provide NO colorspace metadata via UVC Color Matching Descriptor. BT.601 limited range is the de facto standard for generic USB cameras.

**Conversion**: Use fixed-point BT.601 coefficients already in `frame.c`.

---

### ADR-4: Encoder Settings ⏳ PENDING

**Open questions**:
- Bitrate mode: CBR vs VBR vs CQ?
- Keyframe interval?
- H.264 profile/level?
- Timestamp source: frame timestamp vs system time?

---

### ADR-5: Thermal Policy ✅ DECIDED

**Decision**: Drop frames at source under thermal pressure

**Rationale**: Preserves data integrity over continuity. A dropped frame is acceptable; a corrupted frame is not.

**Implementation**: Monitor thermal state via `PowerManager.THERMAL_STATUS_*`. When throttling detected:
1. Increase SPSC queue drop threshold
2. Log `thermal_throttle_drops` telemetry
3. Continue normal operation (no resolution/fps changes)

---

### ADR-6: Snapshot Mechanism ✅ DECIDED

**Decision**: ImageReader surface (Option S1)

**Configuration**:
```java
ImageReader.newInstance(
    width, height,
    ImageFormat.PRIVATE,  // Or RGBA_8888 if CPU access needed
    2,  // maxImages
    HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE | HardwareBuffer.USAGE_GPU_COLOR_OUTPUT
);
```

**Rationale**: Zero-copy, async, no GPU pipeline stalls. `acquireLatestImage()` for real-time capture. See `imagereader.md` for full API documentation.

---

## Concrete "module map" (what lives where)

### Native (C++)

* `UvcIngest` (callback + SPSC)
* `FrameConverter` (conversion loop)
* `FrameBufferRing` (AHardwareBuffer triple buffer + metadata + fences)
* `FenceUtils` (poll/import/export helpers)

### Kotlin/Android

* `UvcCameraService`

  * camera lifecycle, controls, telemetry aggregation
  * MediaCodec + MediaMuxer lifecycle
* `HardwareBufferRenderer` (single compositor thread)

  * EGL init, surfaces, render passes, fence creation
  * output target list management (preview/encoder/snapshot)
* `TelemetryReporter`

  * histograms, percentiles, "drop reasons," remote streaming

---

## Implementation Priority

### Phase 1: Critical (Must Have)
1. **Fence timeout → drop frame** (`FrameBufferRing.cpp:265-270`)
2. **Poll timeout reduction** (100ms → 8-16ms)
3. **`framesDroppedFenceTimeout` telemetry metric**

### Phase 2: Single Owner Compositor
1. Singleton `RendererHolder` pattern
2. Multi-surface render pass (preview → encoder → snapshot)
3. Single release fence after all passes

### Phase 3: ImageReader Integration
1. Add ImageReader surface to compositor
2. Frame-accurate snapshot trigger
3. Preserve original frame timestamp

### Phase 4: Recording Integration  
1. MediaCodec surface as compositor output
2. Async drain with timeout
3. Encoder stall detection and recovery

---

## Related Documentation

- `bus-plan-report.md` — Official invariants and canonical code patterns
- `bus-plan-feedback.md` — Implementation gap analysis with code references
- `imagereader.md` — ImageReader API documentation
