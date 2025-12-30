# ScopeCam Native Integration Guide

This document provides integration instructions for the ScopeCam Kotlin agent to wire up the native stability APIs from UVCCamera library.

## Overview

The UVCCamera library (v1.x.x+) now exposes:
- **Readiness Callback**: Know when native preview thread is ready
- **Graduated Cleanup**: Cleanup at 4 different levels
- **Hard Reset**: Nuclear option for DeviceBusy recovery

---

## 1. Library Update Required

The ScopeCam app must use the updated UVCCamera library. Update the dependency in your app's `build.gradle`:

```kotlin
implementation("org.uvccamera:lib:1.x.x")  // Use version with stability fixes
```

Or for local development:
```bash
cd /path/to/UVCCamera
./gradlew :lib:publishToMavenLocal
```

---

## 2. Readiness Callback Integration

### Java API (UVCCamera.java)

```java
// Set callback before startPreview()
camera.setReadinessCallback(new IReadinessCallback() {
    @Override
    public void onNativeReady() {
        // Native preview thread is now running
        // Safe to call stopPreview() from now on
    }
});

// Query readiness state (polling alternative)
boolean ready = camera.isReady();
```

### Kotlin Integration Pattern

```kotlin
// In your camera manager or wrapper class
class NativeReadinessCallbackImpl(
    private val onReady: () -> Unit
) : IReadinessCallback {
    override fun onNativeReady() {
        onReady()
    }
}

// Usage in CameraConnectionManager or equivalent:
fun startPreviewWithReadiness(camera: UVCCamera) {
    val readyLatch = CountDownLatch(1)

    camera.setReadinessCallback(NativeReadinessCallbackImpl {
        readyLatch.countDown()
        emitTelemetry("native_ready", mapOf("source" to "callback"))
    })

    camera.startPreview()

    // Wait for readiness with timeout
    val receivedCallback = readyLatch.await(500, TimeUnit.MILLISECONDS)
    if (!receivedCallback) {
        // Fallback: poll isReady()
        emitTelemetry("native_ready", mapOf("source" to "timeout_fallback"))
    }
}
```

### Telemetry Events for Readiness

| Event | Properties | Description |
|-------|------------|-------------|
| `native_ready` | `source: "callback"` | Readiness via JNI callback |
| `native_ready` | `source: "timeout_fallback"` | Readiness via polling after timeout |
| `native_ready_timeout` | `timeout_ms: 500` | No readiness within timeout |

---

## 3. Cleanup Levels Integration

### Java API (UVCCamera.java)

```java
// Cleanup level constants
public static final int CLEANUP_PREVIEW_ONLY = 0;  // Stop preview, keep USB
public static final int CLEANUP_CAMERA = 1;        // Close camera handle
public static final int CLEANUP_INTERFACE = 2;     // Release USB interface
public static final int CLEANUP_FULL = 3;          // Release everything

// Usage
int result = camera.cleanup(UVCCamera.CLEANUP_PREVIEW_ONLY);
int result = camera.cleanup(UVCCamera.CLEANUP_INTERFACE);
```

### Cleanup Level Details

| Level | Constant | Actions | Use Case |
|-------|----------|---------|----------|
| 0 | `CLEANUP_PREVIEW_ONLY` | Stop preview thread | Quick pause, resume expected |
| 1 | `CLEANUP_CAMERA` | Close UVC handle, delete helpers | Camera error recovery |
| 2 | `CLEANUP_INTERFACE` | Release USB interfaces (BEFORE camera close) | DeviceBusy recovery |
| 3 | `CLEANUP_FULL` | Release device, close fd | Full disconnect |

**CRITICAL ORDERING**: Level 2 (INTERFACE) is executed BEFORE Level 1 (CAMERA) internally because releasing USB interfaces requires the camera handle to still be valid.

### Kotlin Integration Pattern

```kotlin
enum class CleanupLevel(val value: Int) {
    PREVIEW_ONLY(0),
    CAMERA(1),
    INTERFACE(2),
    FULL(3)
}

fun cleanup(camera: UVCCamera, level: CleanupLevel): Result<Unit> {
    val startTime = System.currentTimeMillis()

    return try {
        val result = camera.cleanup(level.value)
        val duration = System.currentTimeMillis() - startTime

        emitTelemetry("cleanup_complete", mapOf(
            "level" to level.name,
            "result" to result,
            "duration_ms" to duration
        ))

        if (result == 0) Result.success(Unit)
        else Result.failure(CleanupException(result))
    } catch (e: Exception) {
        emitTelemetry("cleanup_error", mapOf(
            "level" to level.name,
            "error" to e.message
        ))
        Result.failure(e)
    }
}
```

### Telemetry Events for Cleanup

| Event | Properties | Description |
|-------|------------|-------------|
| `cleanup_start` | `level: String` | Cleanup initiated |
| `cleanup_complete` | `level: String, result: Int, duration_ms: Long` | Cleanup finished |
| `cleanup_error` | `level: String, error: String` | Cleanup failed |

---

## 4. Hard Reset Integration

### Java API (UVCCamera.java)

```java
// Nuclear option - forces cleanup without thread join
int result = camera.hardReset();
```

### What hardReset() Does

1. Calls `forceStop()` on preview (sets atomic flags, broadcasts condition variables)
2. Waits 50ms for threads to notice
3. Deletes all callback helpers
4. Force closes camera handle
5. Resets USB device via `libusb_reset_device()`
6. Releases device reference
7. Clears all state

### Kotlin Integration Pattern

```kotlin
suspend fun hardReset(camera: UVCCamera): Result<Unit> {
    emitTelemetry("hard_reset_start", emptyMap())
    val startTime = System.currentTimeMillis()

    return withContext(Dispatchers.IO) {
        try {
            val result = camera.hardReset()
            val duration = System.currentTimeMillis() - startTime

            emitTelemetry("hard_reset_complete", mapOf(
                "result" to result,
                "duration_ms" to duration
            ))

            // After hard reset, device needs re-permission and re-connect
            connectionState.value = ConnectionState.DISCONNECTED

            if (result == 0) Result.success(Unit)
            else Result.failure(HardResetException(result))
        } catch (e: Exception) {
            emitTelemetry("hard_reset_error", mapOf("error" to e.message))
            Result.failure(e)
        }
    }
}
```

### Telemetry Events for Hard Reset

| Event | Properties | Description |
|-------|------------|-------------|
| `hard_reset_start` | - | Hard reset initiated |
| `hard_reset_complete` | `result: Int, duration_ms: Long` | Hard reset finished |
| `hard_reset_error` | `error: String` | Hard reset failed |

---

## 5. Recovery Strategy Pattern

Recommended recovery escalation:

```kotlin
suspend fun attemptRecovery(camera: UVCCamera, error: CameraError): RecoveryResult {
    return when (error) {
        is PreviewError -> {
            // Level 0: Just stop preview
            emitTelemetry("recovery_attempt", mapOf("strategy" to "preview_restart"))
            cleanup(camera, CleanupLevel.PREVIEW_ONLY)
            delay(100)
            camera.startPreview()
            RecoveryResult.Success
        }

        is CameraHandleError -> {
            // Level 1: Close camera, reconnect
            emitTelemetry("recovery_attempt", mapOf("strategy" to "camera_reconnect"))
            cleanup(camera, CleanupLevel.CAMERA)
            delay(200)
            reconnectCamera()
        }

        is DeviceBusyError -> {
            // Level 2: Release interfaces
            emitTelemetry("recovery_attempt", mapOf("strategy" to "interface_release"))
            cleanup(camera, CleanupLevel.INTERFACE)
            delay(300)
            reconnectCamera()
        }

        is UnrecoverableError -> {
            // Level 3+: Hard reset
            emitTelemetry("recovery_attempt", mapOf("strategy" to "hard_reset"))
            hardReset(camera)
            delay(500)
            // User must re-grant permission
            RecoveryResult.RequiresPermission
        }
    }
}
```

---

## 6. Native Logging

The native layer logs to Android logcat with tag `UVCCamera`:

| Level | Message | Meaning |
|-------|---------|---------|
| D | `cleanup called with level N` | Cleanup initiated |
| D | `Releasing USB interfaces` | Interface release in progress |
| D | `Cleaning up camera resources` | Camera handles being closed |
| D | `Full cleanup - releasing device` | Complete teardown |
| W | `stopPreview returned N` | Non-zero return from stopPreview |
| W | `Hard reset initiated` | Hard reset started |
| W | `libusb_release_interface returned X, Y` | Interface release failed |
| D | `Native readiness signaled to Kotlin` | Callback fired |

Filter logcat:
```bash
adb logcat -s UVCCamera:D
```

---

## 7. Complete Telemetry Event Catalog

### Connection Lifecycle
- `native_ready` - Native layer ready for commands
- `native_ready_timeout` - Readiness timeout occurred

### Cleanup Operations
- `cleanup_start` - Cleanup initiated
- `cleanup_complete` - Cleanup successful
- `cleanup_error` - Cleanup failed

### Hard Reset
- `hard_reset_start` - Hard reset initiated
- `hard_reset_complete` - Hard reset successful
- `hard_reset_error` - Hard reset failed

### Recovery
- `recovery_attempt` - Recovery strategy initiated
- `recovery_success` - Recovery successful
- `recovery_failed` - Recovery failed, escalating

### Recommended Telemetry Properties

All events should include:
- `timestamp_ms: Long` - Event timestamp
- `session_id: String` - Camera session identifier
- `device_id: String` - USB device identifier (if available)

---

## 8. Migration Checklist

- [ ] Update UVCCamera library dependency to version with stability fixes
- [ ] Wire up `IReadinessCallback` in camera connection flow
- [ ] Implement `awaitNativeReady()` with timeout fallback
- [ ] Map `CleanupLevel` enum to native constants
- [ ] Wire up `cleanup(level)` calls to native
- [ ] Wire up `hardReset()` call to native
- [ ] Add telemetry events for all operations
- [ ] Implement recovery escalation strategy
- [ ] Test rapid connect/disconnect cycles
- [ ] Test DeviceBusy recovery scenarios

---

## Version History

| Version | Changes |
|---------|---------|
| N1 | Thread safety - atomic flags, guarded joins |
| N2 | Readiness callback - IReadinessCallback, isReady() |
| N3 | Cleanup levels - cleanup(level), releaseInterface() |
| N4 | Hard reset - hardReset(), USB device reset |
