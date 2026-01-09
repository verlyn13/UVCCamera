2026 Scientific-Grade UVC Camera Architecture Evolution Report

1.0 Executive Summary

The strategic evolution of our UVC camera architecture is critical to supporting scientific-grade imaging applications, which demand uncompromising data integrity and deterministic performance. This report outlines a shift away from conventional, best-effort media pipelines toward a robust, data-integrity-first model. The proposed architecture is designed around a single-producer, multi-consumer pattern that guarantees resource ownership, explicit synchronization, and predictable behavior under contention, thereby eliminating common sources of data corruption and pipeline stalls.

The core data flow is designed for maximum efficiency and control, minimizing memory copies and leveraging native hardware capabilities. The pipeline proceeds as follows:

[USB Frame Ingest] -> [SPSC Queue (UYVY)] -> [CPU Conversion Thread] -> [AHardwareBuffer Ring (RGBX)] -> [Single GL Compositor] -> [Fan-out to Consumers]


From the single GL Compositor, a unified source frame is rendered without modification to three distinct consumer endpoints, enabling simultaneous operations from a single, synchronized data stream:

1. Live Preview: Rendering to a SurfaceView or TextureView via its ANativeWindow for real-time visualization.
2. Video Encoding: Rendering to a MediaCodec input Surface for efficient, zero-copy hardware video encoding.
3. High-Resolution Snapshot: Rendering to an ImageReader input Surface for non-blocking, full-quality still image capture.

This architecture is governed by a set of strict invariants that ensure its stability and correctness. These principles are not guidelines but foundational rules of the system.

* Governing Invariants:
  * Single Ownership: A single EGL context, managed on a dedicated thread, owns and controls all graphics resources, including AHardwareBuffer objects, textures, and EGL surfaces. This eliminates an entire class of concurrency-related defects.
  * Synchronization Integrity: All data handoffs between system components (e.g., CPU-to-GPU, GPU-to-CPU) are explicitly synchronized using native fence file descriptors. This provides a clear, verifiable chain of dependencies for every frame.
  * Deterministic Behavior: The system is designed to never enter an undefined state. In the event of resource contention or synchronization timeouts, frames are decisively and atomically dropped. This action is logged via telemetry, preventing pipeline stalls, data corruption, or deadlocks.
  * Zero-Copy Compositing: Once frame data is uploaded to the GPU via the AHardwareBuffer ring, it flows to all consumers through GPU-native operations. This avoids slow, pipeline-stalling operations like glReadPixels or redundant CPU-side memory copies.

By adhering to these principles, the architecture provides a stable and verifiable foundation for scientific imaging. The following sections detail the technical invariants, derived directly from official Android and Khronos documentation, that make this model possible.

2.0 Official Documentation-Derived Invariants

The stability and correctness of the entire architecture depend on strictly adhering to a set of non-obvious constraints derived from the authoritative Android NDK and Khronos EGL specifications. These invariants are not recommendations but are mandatory implementation rules. Deviating from these rules will lead to undefined behavior, instability, and difficult-to-diagnose bugs. The following tables codify these rules, their failure modes, and how they must be enforced and validated.

2.1 AHardwareBuffer Lifecycle and CPU Access

These invariants govern the interaction between the CPU (the producer) and the shared AHardwareBuffer memory pool. They are derived from the official Android NDK documentation for Native Hardware Buffers.

Invariant Statement	Failure Mode	Enforcement Point	Validation Method
Buffers must be allocated with a union of all required usage flags (e.g., AHARDWAREBUFFER_USAGE_CPU_WRITE_*, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, AHARDWAREBUFFER_USAGE_VIDEO_ENCODE).	AHardwareBuffer_allocate() fails, or a downstream consumer (like eglCreateImageKHR or MediaCodec) rejects the buffer, returning an error.	Buffer pool allocation logic.	Unit tests that verify allocation succeeds with combined flags and fails with insufficient flags using AHardwareBuffer_isSupported().
CPU access must follow the strict AHardwareBuffer_lock(), memory write, AHardwareBuffer_unlock() sequence. The unlock call yields the CPU->GPU acquire fence.	Data corruption, visual artifacts, or GPU driver crashes due to reading partially written data.	CPU conversion thread's main loop.	Code review and static analysis to ensure the lock/unlock pair brackets all CPU writes.
A buffer locked for writing must not be accessed by any other thread or hardware unit until it is unlocked.	Undefined behavior. This can manifest as data corruption, race conditions, or hard crashes.	Concurrency model and buffer management logic.	Stress testing and thread-sanitizer analysis to detect concurrent access violations.
The caller is responsible for closing the fence file descriptor returned by AHardwareBuffer_unlock().	File descriptor exhaustion, leading to a process crash or inability to open new files or sockets.	Any code path that calls AHardwareBuffer_unlock().	Long-duration stress testing while monitoring /proc/<pid>/fd count for stability.

2.2 EGLImage and GPU Texture Creation

These invariants are derived from the EGL_ANDROID_image_native_buffer specification and define the non-negotiable rules for importing an AHardwareBuffer into the GPU domain as a texture.

Invariant Statement	Failure Mode	Enforcement Point	Validation Method
The target parameter of eglCreateImageKHR must be EGL_NATIVE_BUFFER_ANDROID when creating an image from an AHardwareBuffer.	eglCreateImageKHR fails, returning EGL_NO_IMAGE_KHR and generating an EGL_BAD_PARAMETER error.	GL Compositor thread's texture import logic.	Code review. Verify eglGetError() returns EGL_SUCCESS after the call.
The EGLContext parameter for eglCreateImageKHR must be EGL_NO_CONTEXT. This is because the EGLImage is being created from a shareable AHardwareBuffer handle, not from a resource owned by a specific context.	eglCreateImageKHR fails, returning EGL_NO_IMAGE_KHR and generating an EGL_BAD_CONTEXT error.	GL Compositor thread's texture import logic.	Code review. Verify eglGetError() returns EGL_SUCCESS after the call. This is a critical and non-obvious requirement.
The AHardwareBuffer's properties (format, usage, dimensions) must be supported by the EGL implementation.	eglCreateImageKHR fails, returning EGL_NO_IMAGE_KHR and generating an EGL_BAD_PARAMETER error.	Buffer allocation logic and GL compositor initialization.	Pre-flight checks during startup using AHardwareBuffer_isSupported() for the desired format/usage combination.

2.3 Synchronization with Native Fences

This model relies on explicit synchronization objects (fences) to manage the pipeline. These rules are synthesized from the AHardwareBuffer documentation and the principles of the EGL_ANDROID_native_fence_sync extension.

Invariant Statement	Failure Mode	Enforcement Point	Validation Method
The producer (CPU thread) must generate a CPU->GPU acquire fence via AHardwareBuffer_unlock().	The GPU may read from the buffer before the CPU has finished writing, resulting in torn or corrupted frames.	CPU conversion thread after writing pixel data.	Instrumented testing to confirm a valid fence FD is generated and passed to the consumer.
The GPU consumer (GL thread) must wait on the acquire fence before sampling the associated texture.	Visual artifacts (tearing, corruption) as the GPU samples a texture that is still being written by the CPU.	GL compositor thread, immediately before any draw calls that use the imported texture.	Visual inspection during testing and validation that render latency telemetry includes fence wait times.
The GPU consumer must generate a GPU->CPU release fence after all rendering operations for a frame are complete.	The CPU producer may overwrite the buffer while it is still in use by the GPU, causing severe visual corruption and driver instability.	GL compositor thread, after the last eglSwapBuffers call for a given frame.	Code review. Telemetry for producer fence-wait times should show non-zero values, confirming it waits on a valid fence.
The CPU producer must wait on the GPU->CPU release fence before reusing an AHardwareBuffer for a new frame.	Race condition where the CPU overwrites a buffer still being read by the GPU for a previous frame's composition.	CPU conversion thread, before acquiring a buffer from the ring for writing.	Stress testing. A failure here often manifests as flickering or corrupted frames under heavy load.

2.4 Native Window (ANativeWindow) and Surface Consumers

These invariants clarify the correct way to interact with consumer endpoints like MediaCodec and ImageReader in a GPU-driven pipeline.

Invariant Statement	Failure Mode	Enforcement Point	Validation Method
Consumer Surface objects (from MediaCodec, ImageReader, SurfaceView) are treated as the producer endpoint (ANativeWindow) for dequeuing buffers, which are managed by EGL via eglSwapBuffers.	The application has no mechanism to render frames to the consumer.	GL Compositor thread's render target setup.	Successful rendering to all three consumer types (preview, video file, snapshot image).
The functions ANativeWindow_lock() and ANativeWindow_unlockAndPost() must not be used to draw frames.	Conflicts with EGL's management of the buffer queue, leading to undefined behavior. ANativeWindow_lock implies CPU-based drawing, which would contend with the GPU for buffer ownership, risking deadlocks, visual corruption, or crashes.	The entire native codebase.	Static analysis and code review to prohibit calls to these functions in the rendering pipeline.

2.5 EGL Context Management

This invariant, derived from the core EGL specification, underpins the decision to use a single-threaded graphics owner.

Invariant Statement	Failure Mode	Enforcement Point	Validation Method
EGL contexts can only share data via the share_context parameter if they exist in the same process address space.	eglCreateContext will succeed, but attempts to access resources from another context will fail unpredictably or crash.	EGL initialization logic.	Architectural review. The design enforces this by using a single context, making cross-thread sharing a non-issue.

The practical application of these foundational rules is demonstrated in the canonical code patterns that follow.

3.0 Canonical Code Patterns

This section provides minimal, audit-ready C++ and Java/Kotlin code snippets that correctly implement the invariants defined in the previous section. These patterns focus on the core API interactions and omit boilerplate such as thread management and queue initialization for brevity, but the logic shown should be considered canonical. These patterns should be treated as the canonical implementation for their respective pipeline stages, forming the basis for production code.

3.1 Producer: Writing to AHardwareBuffer

This C++ snippet demonstrates the producer thread's core loop. This loop operates in a feedback cycle with the GL Compositor: it waits to acquire a buffer that the GPU has finished using (signaled by a release fence), locks it for CPU access, writes new pixel data, and then unlocks it to obtain an acquire fence for the GPU.

// C++ (Producer Thread)
void producer_thread_loop(RingBuffer& ring, SPSCQueue& queue) {
    while (is_running) {
        // 1. Acquire a buffer slot from the ring. This call blocks until a buffer is available,
        //    returned by the GL thread after its previous use, complete with a release fence.
        BufferSlot& slot = ring.acquire_for_producer();
        if (slot.release_fence_fd >= 0) {
            wait_on_fence(slot.release_fence_fd, /* timeout_ms= */ 100);
            close(slot.release_fence_fd);
            slot.release_fence_fd = -1;
        }

        // 2. Lock the buffer for CPU writing. The fence param is -1 because
        //    we already waited on the release fence.
        void* data = nullptr;
        int lock_status = AHardwareBuffer_lock(
            slot.buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
            /* fence= */ -1, /* rect= */ nullptr, &data);

        if (lock_status == 0 && data) {
            // 3. Get new frame data and perform CPU-side conversion.
            uyvy_frame = queue.pop();
            convert_uyvy_to_rgbx(uyvy_frame.data, data, WIDTH, HEIGHT);

            // 4. Unlock, generating the acquire fence for the GPU.
            int acquire_fence_fd = -1;
            AHardwareBuffer_unlock(slot.buffer, &acquire_fence_fd);

            // 5. Enqueue the buffer and its acquire fence for the GL thread.
            gl_queue.push({slot.buffer, acquire_fence_fd});
        }
    }
}


* Thread Ownership: This code runs exclusively on the dedicated CPU producer/conversion thread.
* Fence FD Ownership/Closure Rules: It waits on and closes the release_fence_fd received from the GL compositor. It creates a new acquire_fence_fd via AHardwareBuffer_unlock, the ownership of which is transferred to the GL thread.
* Critical Error Handling: Production code must check the return status of AHardwareBuffer_lock and unlock. A failure should be logged, and the buffer should be returned to the pool without being sent to the GL thread.
* Required API/Extension Checks: This relies on AHardwareBuffer APIs available since API level 26.

3.2 GL Compositor: Fence Import and Texture Sampling

This C++ snippet shows how the GL thread receives a buffer, waits on its acquire fence, and imports it as an external texture.

// C++ (GL Compositor Thread)

// One-time setup:
GLuint input_texture_id;
glGenTextures(1, &input_texture_id);
glBindTexture(GL_TEXTURE_EXTERNAL_OES, input_texture_id);
// ... set texture parameters ...

// In the render loop, for each new frame:
Frame new_frame = gl_queue.pop();

// 1. Create an EGLSync object from the acquire fence FD.
EGLSyncKHR fence_sync = eglCreateSyncKHR(
    display, EGL_SYNC_NATIVE_FENCE_ANDROID,
    {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, new_frame.acquire_fence_fd});
close(new_frame.acquire_fence_fd); // EGL has imported the fence by duplicating the FD, so we must close the original.

// 2. Block the GPU command stream until the fence signals.
eglWaitSyncKHR(display, fence_sync, 0);
eglDestroySyncKHR(display, fence_sync);

// 3. Create an EGLImage from the AHardwareBuffer.
//    NOTE: The context parameter MUST be EGL_NO_CONTEXT.
EGLImageKHR egl_image = eglCreateImageKHR(
    display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
    (EGLClientBuffer)new_frame.buffer, nullptr);

// 4. Bind the EGLImage to the external texture.
glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)egl_image);

// ... proceed to render with 'input_texture_id' ...

// 5. Clean up the EGLImage after rendering.
eglDestroyImageKHR(display, egl_image);


* Thread Ownership: Runs exclusively on the single GL Compositor thread.
* Fence FD Ownership/Closure Rules: It takes ownership of the acquire_fence_fd, passes it to EGL, and closes the FD immediately as EGL has now duplicated it.
* Critical Error Handling: All EGL/GLES calls must be checked for errors. A failure in eglCreateImageKHR is critical and should result in the frame being dropped.
* Required API/Extension Checks: Requires EGL_KHR_image_base, EGL_ANDROID_image_native_buffer, EGL_ANDROID_native_fence_sync, and GL_OES_EGL_image_external. These must be checked at initialization.

3.3 GL Compositor: Fan-Out Rendering

This snippet demonstrates rendering the single input texture to multiple render targets and then creating a single release fence.

// C++ (GL Compositor Thread, after texture is bound)

// 1. Bind the single input texture.
glActiveTexture(GL_TEXTURE0);
glBindTexture(GL_TEXTURE_EXTERNAL_OES, input_texture_id);

// 2. Iterate through N render targets.
for (const auto& target : render_targets) {
    // 3. Bind the FBO for the consumer's EGLSurface.
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo_id);
    glViewport(0, 0, target.width, target.height);

    // 4. Execute a simple draw call (e.g., a textured quad).
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // 5. Present the buffer to the consumer's Surface.
    eglSwapBuffers(display, target.egl_surface);
}

// 6. After all drawing is complete, create a single release fence.
EGLSyncKHR release_sync = eglCreateSyncKHR(
    display, EGL_SYNC_NATIVE_FENCE_ANDROID, {EGL_NONE});

int release_fence_fd = eglDupNativeFenceFDANDROID(display, release_sync);
eglDestroySyncKHR(display, release_sync);

// 7. Send the buffer handle and release fence back to the producer.
producer_feedback_queue.push({frame.buffer, release_fence_fd});


* Thread Ownership: Runs exclusively on the single GL Compositor thread.
* Fence FD Ownership/Closure Rules: It creates a new release_fence_fd and transfers its ownership to the producer thread.
* Critical Error Handling: An invalid EGL surface (e.g., from a destroyed view) will cause eglSwapBuffers to fail. This should be handled by removing that target from the render list for subsequent frames.
* Required API/Extension Checks: Requires EGL_ANDROID_native_fence_sync.

3.4 Fence Timeout and Frame Dropping Logic

This C++ snippet shows how to robustly wait on a fence with a timeout, implementing the deterministic frame-dropping invariant.

// C++
#include <poll.h>

// Waits on a fence FD, returns true if signaled, false on timeout.
bool wait_on_fence(int fence_fd, int timeout_ms) {
    if (fence_fd < 0) return true; // Invalid fence is considered signaled.

    struct pollfd pfd = { .fd = fence_fd, .events = POLLIN };
    int result = poll(&pfd, 1, timeout_ms);

    if (result > 0) {
        return true; // Fence signaled.
    } else {
        // result == 0 means timeout, result < 0 means error.
        return false; // Fence timed out or error occurred.
    }
}

// Example usage in producer before locking a buffer:
if (!wait_on_fence(slot.release_fence_fd, 100 /* ms */)) {
    // TIMEOUT: GPU is stalled or holding the buffer too long.
    telemetry.increment("fence_wait_timeout_drops");
    // Do NOT proceed. Immediately release the buffer back to the pool
    // and try to acquire the next one.
    return_buffer_to_pool(slot);
    continue; // Skip to next iteration of producer loop.
}


* Thread Ownership: This logic is critical in any thread that waits on a fence, primarily the producer thread waiting on the GPU release fence.
* Fence FD Ownership/Closure Rules: This function does not consume or close the FD; the caller retains ownership.
* Critical Error Handling: If wait_on_fence returns false, it is a critical error to proceed with using the associated buffer. The frame must be dropped to prevent a pipeline stall or deadlock.

3.5 Consumer Surface Integration (Java/Kotlin)

These Java/Kotlin snippets show how to create the Surface objects that act as endpoints for the native GL compositor.

// Java/Kotlin (UI/Main Thread)

// 1. MediaCodec setup for video encoding
MediaFormat format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, 1920, 1080);
// ... configure bitrate, color format, etc. ...
MediaCodec encoder = MediaCodec.createEncoderByType(format.getString(MediaFormat.KEY_MIME));
encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
Surface encoderSurface = encoder.createInputSurface(); // Pass this to JNI
encoder.start();

// 2. ImageReader setup for high-resolution snapshots
ImageReader snapshotReader = ImageReader.newInstance(
    3840, 2160,
    ImageFormat.PRIVATE, // Or a specific format if needed
    2, // maxImages
    HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE | HardwareBuffer.USAGE_GPU_COLOR_OUTPUT
);
Surface snapshotSurface = snapshotReader.getSurface(); // Pass this to JNI
snapshotReader.setOnImageAvailableListener(this::onJpegImageAvailable, backgroundHandler);

// 3. SurfaceView setup for live preview
SurfaceView previewView = findViewById(R.id.preview_surface);
Surface previewSurface = previewView.getHolder().getSurface(); // Pass this to JNI

// 4. Pass all surfaces to the native layer
nativeInitialize(previewSurface, encoderSurface, snapshotSurface);


* Annotations: The Surface objects are Parcelable and can be passed safely across the JNI boundary. On the native side, these Surface objects are received as ANativeWindow* pointers. For each ANativeWindow, an EGL window surface (EGLSurface) is created using eglCreateWindowSurface. These EGL surfaces are the render targets for the compositor's fan-out logic. The native layer uses these surfaces for rendering but does not control their lifecycle; they are owned by their respective Java objects.

These low-level patterns provide the building blocks for the architecture. The high-level decisions that justify their use are documented next.

4.0 Architectural Decision Records

The following records document the key architectural choices that define this system. Each decision was made to maximize stability, data integrity, and performance, in that order of priority.

4.1 Data Format: Unified RGBX Ring Buffer

* Decision: A dedicated CPU thread will ingest UYVY frames from the USB source, convert them to AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, and place them into a shared AHardwareBuffer ring buffer.
* Alternatives: Pass the raw YUV frames (AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420) directly to the GL compositor and perform color conversion in a GLSL shader using an external sampler (samplerExternalOES).
* Rationale: This decision prioritizes simplicity and correctness in the most complex part of the pipeline: the GL compositor.
  1. Simplified Compositing: Rendering from a standard RGBX texture is trivial and requires no special shaders. It guarantees that the data sent to the preview, encoder, and snapshot consumers is pixel-identical.
  2. Decoupling: It decouples the image ingest format from the rendering format. Future camera sources with different formats (e.g., Bayer, P010) can be supported by only changing the CPU conversion function, leaving the entire GPU pipeline unmodified.
  3. Unified Buffer: As documented in the AHardwareBuffer API, a single buffer can be allocated with a union of usage flags like AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE and AHARDWAREBUFFER_USAGE_VIDEO_ENCODE. Using a common RGBX format ensures maximum compatibility with all potential hardware consumers. A YUV buffer would be less flexible for general-purpose GPU rendering tasks.
* Risks: The CPU conversion step introduces latency and consumes CPU cycles. This must be monitored with the cpu_conversion_latency_us telemetry metric to ensure it does not become a bottleneck, especially at high frame rates.

4.2 Encoding Path: Surface-to-Surface Encoding

* Decision: Video encoding will be implemented by rendering frames from the GL compositor directly onto the input Surface provided by MediaCodec.createInputSurface().
* Alternatives: Use glReadPixels to synchronously copy the composed frame from the GPU framebuffer back to a CPU-side ByteBuffer, and then pass this buffer to the MediaCodec's input queue.
* Rationale: This decision enables a zero-copy encoding path. When rendering to a MediaCodec surface, the GPU driver can pass the buffer handle directly to the hardware video encoder without involving the main system CPU or copying pixel data to main memory. The alternative, glReadPixels, is a notoriously slow and inefficient operation that stalls the entire GPU pipeline, drains performance, and introduces an extra, power-hungry memory copy. The surface-to-surface path is the standard, high-performance method for GPU-accelerated encoding on Android.
* Risks: MediaCodec and its underlying drivers can have device-specific bugs. This path requires extensive testing across a range of target hardware to ensure stability. An encoder stall could block the return of buffers, which must be handled gracefully by the pipeline (see Section 5.2).

4.3 Concurrency Model: Single EGL Context Owner

* Decision: A single, dedicated thread will be responsible for creating, managing, and using a single EGLContext. All OpenGL and EGL calls will be confined to this thread.
* Alternatives: Use multiple EGL contexts, potentially shared between threads using the share_context attribute of eglCreateContext.
* Rationale: This model prioritizes stability and simplicity. The eglCreateContext documentation states that shared contexts must exist within the same process address space. While this makes multi-threaded GL possible, it introduces significant complexity for synchronizing access to shared resources like textures and buffer objects. A single-threaded owner model completely eliminates this class of race conditions. It creates a clear, serial flow of commands to the GPU driver, making the system easier to debug, reason about, and prove correct. This aligns with the primary architectural priority of stability.
* Risks: The single GL thread could become a bottleneck if rendering work is too complex or if it is blocked waiting on fences for extended periods. Telemetry on render latency and fence wait times is crucial to monitor this risk.

4.4 Snapshot Path: ImageReader Surface

* Decision: High-resolution snapshots will be captured by rendering the composed frame onto the input Surface provided by an ImageReader instance.
* Alternatives: Use glReadPixels to synchronously copy the composed frame from the GPU framebuffer to a CPU buffer, then create a Bitmap or encode a JPEG from that buffer.
* Rationale: The rationale is identical to that of the MediaCodec path. ImageReader provides a Surface-based interface that allows the GPU to render to a buffer asynchronously. This is a non-stalling operation that does not block the main rendering pipeline. The ImageReader manages a queue of buffers, and the resulting image data can be accessed on a background thread via the OnImageAvailableListener for processing (e.g., JPEG encoding and saving to disk) without impacting the real-time preview or video encoding streams.
* Risks: If snapshots are requested at a very high frequency, the ImageReader's buffer queue could fill up, causing it to stop accepting new frames. The application logic must manage the rate of snapshot requests.

These architectural decisions form a cohesive strategy that must be rigorously tested and monitored in production.

5.0 Validation and Observability

To guarantee scientific-grade reliability, the architecture must be paired with a comprehensive validation strategy and runtime observability. This section defines the required telemetry and testing scenarios to ensure the system's invariants are never violated and that its performance characteristics are well understood.

5.1 Required Telemetry

The following metrics must be implemented to provide continuous insight into the health and performance of the pipeline. Anomalous readings are early indicators of system degradation or bugs.

* spsc_queue_overflow_drops: A counter for frames dropped at ingest because the CPU processing queue is full. A consistently high value indicates the CPU conversion thread cannot keep up with the USB frame rate.
* fence_wait_timeout_drops: A counter for frames dropped by the producer or compositor due to a synchronization fence timing out. This is a critical health signal, indicating a severe stall in the GPU, a consumer (like MediaCodec), or a driver bug.
* cpu_conversion_latency_us: Microsecond timing for the UYVY -> RGBX conversion function. Used to monitor the performance of the CPU-bound portion of the pipeline.
* gpu_acquire_fence_wait_latency_us: Microsecond timing for how long the GL thread waits on the CPU->GPU acquire fence. High values suggest the GPU is waiting on the CPU, which is expected; near-zero values might indicate a problem.
* gpu_render_latency_us: Microsecond timing for the full compositor draw pass, from the first glBindFramebuffer to the creation of the release fence. Measures the total GPU workload per frame.
* producer_release_fence_wait_latency_us: Microsecond timing for how long the producer thread waits on the GPU->CPU release fence. A spike in this metric points to a stall in the GPU pipeline, which may be caused by any slow consumer (e.g., the video encoder, preview surface, or snapshot reader).

5.2 System Test Scenarios

The following critical test cases must be implemented and passed to validate the architecture's resilience and correctness.

1. Encoder Stall: Simulate the MediaCodec consumer holding onto a buffer for an extended period (e.g., by not calling releaseOutputBuffer). The system must not deadlock. It should continue servicing the Live Preview and Snapshot consumers. The producer_release_fence_wait_latency_us metric should spike, and eventually, the producer should time out waiting for the encoder's buffers and drop frames intended for it, while the rest of the system remains operational.
2. Consumer Surface Destruction: Simulate a consumer Surface being destroyed and recreated mid-stream (e.g., rotating the screen, which destroys and recreates the SurfaceView). The GL compositor must handle the resulting EGL_BAD_SURFACE error from eglSwapBuffers gracefully without crashing. It should stop rendering to the invalid target and seamlessly resume when a new, valid Surface is provided.
3. Forced Fence Timeout: Inject an invalid fence file descriptor or a fence that never signals into the pipeline. The system must correctly timeout based on the logic in Section 3.4. The fence_wait_timeout_drops telemetry must increment, and the associated frame must be dropped without stalling the entire pipeline indefinitely.
4. File Descriptor Leak Detection: Run the pipeline under a sustained, high-load condition for several hours. The process's file descriptor count (monitored via /proc/<pid>/fd) must remain stable. Any steady increase indicates a leak where fence FDs are not being closed correctly.
5. Thermal Throttling Event: Operate the pipeline on a device under heavy thermal load, forcing the CPU and GPU to downclock. The system must gracefully degrade by dropping frames (indicated by increasing spsc_queue_overflow_drops and fence_wait_timeout_drops) rather than crashing, producing corrupted data, or triggering "Application Not Responding" (ANR) errors.

5.3 Definition of Done

A production-ready implementation of this architecture must meet the following exit criteria:

* [ ] All invariants from Section 2.0 are enforced by static analysis or mandatory code review checklists.
* [ ] All canonical code patterns from Section 3.0 are implemented as the foundational logic for their respective pipeline stages.
* [ ] All telemetry from Section 5.1 is implemented and integrated with a metrics backend for monitoring.
* [ ] All system test scenarios from Section 5.2 pass consistently on all target hardware.
* [ ] The process file descriptor count remains stable under a 24-hour stress test.
* [ ] No graphics-related ANRs or native crashes (tombstones) occur during the 24-hour stress test.

This rigorous validation process ensures the final implementation is truly scientific-grade.

6.0 Appendix: Invariant to Source Mapping

This appendix provides an audit trail mapping each core architectural invariant to its authoritative source document, ensuring all design constraints are grounded in official specifications.

Invariant ID	Invariant Summary	Authoritative Source(s)
AHB-01	AHardwareBuffer must be allocated with a union of all usage flags required by its consumers.	Native Hardware Buffer | Android NDK | Android Developers
AHB-02	AHardwareBuffer_unlock is the sole source of the CPU->GPU acquire fence.	Native Hardware Buffer | Android NDK | Android Developers
AHB-03	The caller is responsible for closing all native fence file descriptors to prevent resource leaks.	Native Hardware Buffer | Android NDK | Android Developers
EGL-01	eglCreateImageKHR for native buffers must use EGL_NO_CONTEXT as the context parameter.	EGL Extension 49: Android Native Buffer
EGL-02	EGL contexts using share_context for resource sharing must exist in the same process address space.	EGL Context Initialization and Configuration Reference
ANW-01	ANativeWindow_lock and unlockAndPost are for CPU-based drawing and are incorrect for a GPU-driven pipeline.	Native Window | Android NDK | Android Developers
SYNC-01	The pipeline relies on a strict producer-consumer model where acquire/release fences manage all transitions.	Synthesized from Native Hardware Buffer fence behavior and EGL_ANDROID_native_fence_sync extension principles.

