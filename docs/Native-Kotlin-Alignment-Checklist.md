---
title: Native ↔ Kotlin Integration Alignment Checklist
category: integration
component: jni
status: active
version: "1.0"
last_updated: 2024-12-29
tags: [jni, contract, verification, scopecam, integration]
priority: critical
---

# Native ↔ Kotlin Integration Alignment Checklist

## Cross-Project Verification Document v1.0

---

### Purpose

This document serves as the **single source of truth** for interface contracts between the Native (UVCCamera) and Kotlin (ScopeCam) layers. Both agents must verify their implementations against this checklist before integration testing.

**Workflow**:
1. Native Agent verifies all "Native Side" columns
2. ScopeCam Agent verifies all "Kotlin Side" columns
3. Any mismatch must be resolved before proceeding

---

## Section 1: JNI Method Signatures

### 1.1 Camera Connection Methods

| ID | Method Name | Native JNI Signature | Java Declaration | Kotlin Usage | Native Status | Kotlin Status |
|----|-------------|---------------------|------------------|--------------|---------------|---------------|
| JNI-001 | nativeConnect (legacy) | `(JIIIIILjava/lang/String;)I` | `private native int nativeConnect(long id, int vid, int pid, int fd, int busNum, int devAddr, String usbfs)` | Via `UVCCamera.open(UsbControlBlock)` | ✅ Verified (line 2334) | ☐ Verify |
| JNI-002 | nativeConnectSimple | `(JILjava/lang/String;)I` | `private native int nativeConnectSimple(long id, int fd, String usbfsPath)` | `uvcCamera.openSimple(fd, path)` | ✅ Verified (line 2335) | ☐ Verify |
| JNI-003 | nativeRelease | `(J)I` | `private native int nativeRelease(long id)` | `uvcCamera.close()` | ✅ Verified (line 2336) | ☐ Verify |

**Verification Commands**:

```bash
# Native Agent - Verify JNI registration
grep -n "nativeConnectSimple" lib/src/main/jni/UVCCamera/serenegiant_usb_UVCCamera.cpp

# Expected output should include:
# Line XXX: { "nativeConnectSimple", "(JILjava/lang/String;)I", (void *) nativeConnectSimple },

# ScopeCam Agent - Verify Java declaration
grep -n "nativeConnectSimple" */UVCCamera.java

# Expected output should include:
# private native int nativeConnectSimple(long id_camera, int fd, String usbfsPath);
```

### 1.2 Telemetry Methods

| ID | Method Name | Native JNI Signature | Return Type | Kotlin Usage | Native Status | Kotlin Status |
|----|-------------|---------------------|-------------|--------------|---------------|---------------|
| JNI-010 | nativeGetTelemetryBuffer | `(J)Ljava/nio/ByteBuffer;` | `ByteBuffer` (direct) | `NativeTelemetry.fromByteBuffer()` | ✅ Verified (FrameBufferJNI.cpp:731) | ☐ Verify |
| JNI-011 | nativeSetFrameBufferRing | `(JJ)I` | `int` (0=success) | Via FrameBufferManager | ✅ Verified (line 2361) | ☐ Verify |

> **Note:** JNI-010 returns a direct `ByteBuffer` (more efficient than `byte[]`). The Kotlin side should use `buffer.order(ByteOrder.LITTLE_ENDIAN)` before reading.

**Verification Commands**:

```bash
# Native Agent
grep -n "nativeGetTelemetryBuffer\|nativeSetFrameBufferRing" lib/src/main/jni/UVCCamera/*.cpp

# ScopeCam Agent
grep -n "nativeGetTelemetryBuffer\|nativeSetFrameBufferRing" */UVCCamera.java
```

---

## Section 2: Enum Mappings

### 2.1 ConnectionReadiness (Native) ↔ CameraConnectionState (Kotlin)

| Native Enum Value | Int Value | Kotlin State Equivalent | Mapping Correct |
|-------------------|-----------|------------------------|-----------------|
| `DISCONNECTED` | 0 | `CameraConnectionState.Disconnected` | ✅ Native ☐ Kotlin |
| `INITIALIZING` | 1 | `CameraConnectionState.Connecting` | ✅ Native ☐ Kotlin |
| `READY` | 2 | `CameraConnectionState.Connected` | ✅ Native ☐ Kotlin |
| `STREAMING` | 3 | `CameraConnectionState.Streaming` | ✅ Native ☐ Kotlin |
| `ERROR` | 4 | `CameraConnectionState.Error` | ✅ Native ☐ Kotlin |

**Native Declaration** (StreamTelemetry.h):
```cpp
enum class ConnectionReadiness : int32_t {
    DISCONNECTED = 0,
    INITIALIZING = 1,
    READY = 2,
    STREAMING = 3,
    ERROR = 4
};
```

**Kotlin Declaration** (if reading from packed buffer - future):
```kotlin
enum class NativeConnectionReadiness(val value: Int) {
    DISCONNECTED(0),
    INITIALIZING(1),
    READY(2),
    STREAMING(3),
    ERROR(4);

    companion object {
        fun fromValue(value: Int): NativeConnectionReadiness =
            entries.find { it.value == value } ?: DISCONNECTED
    }
}
```

**Verification Commands**:

```bash
# Native Agent
grep -A6 "enum class ConnectionReadiness" lib/src/main/jni/UVCCamera/StreamTelemetry.h

# ScopeCam Agent - Verify state machine has equivalent states
grep -n "object Disconnected\|class DeviceAttached\|class AwaitingPermission\|class PermissionGranted\|class Connecting\|class Connected\|class Streaming\|class Error" */CameraConnectionState.kt
```

### 2.2 SlotState (Ring Buffer)

| Native Enum Value | Int Value | Kotlin Enum | Mapping Correct |
|-------------------|-----------|-------------|-----------------|
| `EMPTY` | 0 | `SlotState.EMPTY` | ✅ Native ☐ Kotlin |
| `WRITING` | 1 | `SlotState.WRITING` | ✅ Native ☐ Kotlin |
| `READY` | 2 | `SlotState.READY` | ✅ Native ☐ Kotlin |
| `READING` | 3 | `SlotState.READING` | ✅ Native ☐ Kotlin |

**Verification Commands**:

```bash
# Native Agent
grep -A5 "enum class SlotState" lib/src/main/jni/UVCCamera/StreamTelemetry.h

# ScopeCam Agent
grep -A5 "enum class SlotState" */NativeTelemetry.kt
```

### 2.3 StagnationPoint (Pipeline Diagnosis)

> **Note:** This enum exists only in Kotlin (NativeTelemetry.kt) for Watchdog heuristics.
> The Kotlin side derives stagnation from telemetry metrics; native does not emit stagnation points directly.

| Native Enum Value | Int Value | Kotlin Enum | Mapping Correct |
|-------------------|-----------|-------------|-----------------|
| N/A | 0 | `StagnationPoint.NONE` | N/A (Kotlin-only) |
| N/A | 1 | `StagnationPoint.STREAMING_FAILED` | N/A (Kotlin-only) |
| N/A | 2 | `StagnationPoint.NO_USB_CALLBACKS` | N/A (Kotlin-only) |
| N/A | 3 | `StagnationPoint.CONVERSION_FAILED` | N/A (Kotlin-only) |
| N/A | 4 | `StagnationPoint.CONSUMER_NOT_POLLING` | N/A (Kotlin-only) |
| N/A | 5 | `StagnationPoint.PRODUCER_TOO_SLOW` | N/A (Kotlin-only) |

**Status**: Kotlin-only heuristic enum. CameraWatchdog derives stagnation from:
- `framesProduced == 0` after timeout → STREAMING_FAILED
- `usbPacketsReceived == 0` → NO_USB_CALLBACKS
- `framesRendered == 0` but `framesProduced > 0` → CONVERSION_FAILED
- Consumer starve ratio > threshold → CONSUMER_NOT_POLLING

---

## Section 3: Telemetry Struct Fields

### 3.1 StreamTelemetry Packed Buffer Layout

> **IMPORTANT**: The packed buffer uses **uniform `int64_t` fields** (8 bytes each) for simplicity.
> Total: 37 fields × 8 bytes = **296 bytes**. Version = 2.

The packed buffer is transferred via JNI as a **direct ByteBuffer** (not byte array).

| Field Index | Field Name | Native Type | Byte Offset | Kotlin Access | Native Status |
|-------------|------------|-------------|-------------|---------------|---------------|
| 0 | VERSION | `int64_t` | 0 | `buffer.getLong(0)` | ✅ Verified |
| 1 | FRAMES_PRODUCED | `int64_t` | 8 | `buffer.getLong(8)` | ✅ Verified |
| 2 | FRAMES_DROPPED_MAILBOX | `int64_t` | 16 | `buffer.getLong(16)` | ✅ Verified |
| 3 | FRAMES_DROPPED_NO_SURFACE | `int64_t` | 24 | `buffer.getLong(24)` | ✅ Verified |
| 4 | FRAMES_DROPPED_QUEUE_FULL | `int64_t` | 32 | `buffer.getLong(32)` | ✅ Verified |
| 5 | FRAMES_CORRUPTED | `int64_t` | 40 | `buffer.getLong(40)` | ✅ Verified |
| 6 | FRAMES_RENDERED | `int64_t` | 48 | `buffer.getLong(48)` | ✅ Verified |
| 7-13 | USB_* fields | `int64_t` | 56-104 | `buffer.getLong(n*8)` | ✅ Verified |
| 14-17 | IN_PIPE_LATENCY_* | `int64_t` | 112-136 | `buffer.getLong(n*8)` | ✅ Verified |
| 18-22 | LEGACY_TIMING_* | `int64_t` | 144-176 | `buffer.getLong(n*8)` | ✅ Verified |
| 23-29 | RING_BUFFER_STATE_* | `int64_t` | 184-232 | `buffer.getLong(n*8)` | ✅ Verified |
| 30-32 | STREAM_PARAMS_* | `int64_t` | 240-256 | `buffer.getLong(n*8)` | ✅ Verified |
| 33-35 | ERROR_TRACKING_* | `int64_t` | 264-280 | `buffer.getLong(n*8)` | ✅ Verified |
| 36 | TIMESTAMP_NS | `int64_t` | 288 | `buffer.getLong(288)` | ✅ Verified |

**Full field list**: See `TelemetryPackedBuffer::Field` enum in `StreamTelemetry.h:108-166`

**Kotlin access pattern**:
```kotlin
buffer.order(ByteOrder.LITTLE_ENDIAN)
val version = buffer.getLong(0)
val framesProduced = buffer.getLong(8)
// ... or use Field enum ordinal * 8
```

**Verification Commands**:

```bash
# Native Agent - Check struct layout
grep -A50 "struct StreamTelemetry" lib/src/main/jni/UVCCamera/StreamTelemetry.h | head -60

# Native Agent - Check pack function
grep -A30 "packForJni" lib/src/main/jni/UVCCamera/StreamTelemetry.h

# ScopeCam Agent - Check unpacking
grep -A50 "fun fromByteBuffer" */NativeTelemetry.kt | head -60
```

### 3.2 Connection Readiness Fields (New - E1-N-004)

| Field Name | Native Type | In Packed Buffer? | Kotlin Access | Native Status | Kotlin Status |
|------------|-------------|-------------------|---------------|---------------|---------------|
| `readinessState` | `atomic<ConnectionReadiness>` | No (TBD v3) | Future: `nativeGetReadinessState()` | ✅ Verified (line 268) | N/A |
| `readinessTimestamp` | `atomic<int64_t>` | No (TBD v3) | Future | ✅ Verified (line 269) | N/A |
| `activeFdCount` | `atomic<int32_t>` | No (TBD v3) | Future: `nativeGetActiveFdCount()` | ✅ Verified (line 270) | N/A |
| `lastConnectionError` | `atomic<int32_t>` | No (TBD v3) | Future | ✅ Verified (line 271) | N/A |

**Note**: These fields exist in native (`StreamTelemetry.h:268-271`) but are NOT in the packed buffer.
Kotlin Watchdog uses Kotlin state machine as primary source; these native fields are for future direct correlation.

---

## Section 4: USBMonitor Passive Mode

### 4.1 Constructor Signature

| ID | Constructor | Parameters | Kotlin Usage | Native Status | Kotlin Status |
|----|-------------|------------|--------------|---------------|---------------|
| USM-001 | Active (legacy) | `(Context, OnDeviceConnectListener)` | Legacy compatibility | ✅ Verified (line 132) | ☐ Verify |
| USM-002 | Passive (new) | `(Context, OnDeviceConnectListener, boolean passiveMode)` | `USBMonitor(ctx, listener, true)` | ✅ Verified (line 146) | ☐ Verify |

**Verification Commands**:

```bash
# Native Agent (USBMonitor.java is in native lib)
grep -n "public USBMonitor" lib/src/main/java/com/serenegiant/usb/USBMonitor.java

# Expected: Two constructors - one with 2 params, one with 3 params (passiveMode)

# ScopeCam Agent
grep -n "USBMonitor(" */UsbConnectivityManager.kt
# Expected: USBMonitor(context, listener, true)  // passive mode
```

### 4.2 Passive Mode Behavior

| Behavior | Active Mode | Passive Mode | Kotlin Expectation | Verified |
|----------|-------------|--------------|-------------------|----------|
| `onAttach()` fires | Yes | Yes | Receives device attach events | ✅ Native (line 559) |
| `onDettach()` fires | Yes | Yes | Receives device detach events | ✅ Native (line 560) |
| `onConnect()` fires | Yes | No | Never fires in passive mode | ✅ Native (guard 539-542) |
| `onCancel()` fires | Yes | No | Never fires in passive mode | ✅ Native (guard 539-542) |
| `requestPermission()` shows dialog | Yes | No-op | Returns without action | ✅ Native (guard 477-480) |
| `openDevice()` works | Yes | Yes | Creates UsbControlBlock | ✅ Native (lines 515-526) |

---

## Section 5: UVCCamera.openSimple() Contract

### 5.1 Method Signature

| Language | Declaration | Location |
|----------|-------------|----------|
| Java | `public synchronized void openSimple(int fd, String usbfsPath)` | UVCCamera.java |
| Native | `static jint nativeConnectSimple(JNIEnv*, jobject, ID_TYPE, jint fd, jstring usbfs_path)` | serenegiant_usb_UVCCamera.cpp |
| Kotlin | `uvcCamera.openSimple(fd, path)` | UsbConnectivityManager.kt |

### 5.2 Parameter Contract

| Parameter | Type | Valid Values | Validation | Verified |
|-----------|------|--------------|------------|----------|
| `fd` | `int` | > 0 | Native checks `fd <= 0` returns -2 (line 183-186), Java checks `fd <= 0` throws IllegalArgumentException (line 247-249) | ✅ Native ☐ Kotlin |
| `usbfsPath` | `String` | `/dev/bus/usb/XXX/YYY` or `/proc/bus/usb/XXX/YYY` | Native parses with sscanf (line 198-202) | ✅ Native ☐ Kotlin |

### 5.3 Return/Exception Contract

| Outcome | Native Return | Java Behavior | Kotlin Handling |
|---------|---------------|---------------|-----------------|
| Success | `0` | Returns normally | Transition to `Connected` state |
| Invalid FD | `-1` (JNI_ERR) | Throws `UnsupportedOperationException` | Catch → `Error` state |
| Parse failure | `-1` | Throws `UnsupportedOperationException` | Catch → `Error` state |
| libuvc error | `< 0` | Throws `UnsupportedOperationException` | Catch → `Error` state |

---

## Section 6: State Emission Points

### 6.1 Native Readiness Emissions

| Location | File:Line | State Emitted | Trigger Condition | Verified |
|----------|-----------|---------------|-------------------|----------|
| connect() start | UVCCamera.cpp:146 | `INITIALIZING` | Entry to connect() | ✅ |
| libuvc init fail | UVCCamera.cpp:160 | `ERROR` | `uvc_init2()` fails | ✅ |
| connect success | UVCCamera.cpp:190-191 | `incrementFdCount()` + `READY` | `uvc_open()` succeeds | ✅ |
| uvc_open fail | UVCCamera.cpp:200 | `ERROR` | `uvc_open()` fails | ✅ |
| device not found | UVCCamera.cpp:213 | `ERROR` | `uvc_get_device_with_fd()` fails | ✅ |
| release FD | UVCCamera.cpp:252 | `decrementFdCount()` | Before `close(mFd)` | ✅ |
| release complete | UVCCamera.cpp:264 | `DISCONNECTED` | End of `release()` | ✅ |
| preview start | UVCCamera.cpp:477 | `STREAMING` | `startPreview()` succeeds | ✅ |
| preview stop | UVCCamera.cpp:491 | `READY` | `stopPreview()` called | ✅ |

**Verification Command**:

```bash
# Native Agent - Full verification
grep -n "setReadinessState\|incrementFdCount\|decrementFdCount" lib/src/main/jni/UVCCamera/UVCCamera.cpp

# Expected: 10 matches at the lines listed above
```

### 6.2 Kotlin State Transitions

| Trigger | From State | To State | Location | Verified |
|---------|------------|----------|----------|----------|
| Device attached | `Disconnected` | `DeviceAttached` | UsbConnectivityManager | ☐ |
| Already has permission | `DeviceAttached` | `PermissionGranted` | UsbConnectivityManager | ☐ |
| Request permission | `DeviceAttached` | `AwaitingPermission` | UsbConnectivityManager | ☐ |
| Permission granted | `AwaitingPermission` | `PermissionGranted` | UsbConnectivityManager | ☐ |
| Permission denied | `AwaitingPermission` | `Error` | UsbConnectivityManager | ☐ |
| FD obtained | `PermissionGranted` | `Connecting` | UsbConnectivityManager | ☐ |
| Native ready | `Connecting` | `Connected` | UsbConnectivityManager | ☐ |
| Preview started | `Connected` | `Streaming` | `notifyStreamingStarted()` | ☐ |
| Preview stopped | `Streaming` | `Connected` | `notifyStreamingStopped()` | ☐ |
| Device detached | Any | `Disconnected` | UsbConnectivityManager | ☐ |
| Error occurred | Any | `Error` | UsbConnectivityManager | ☐ |

---

## Section 7: Error Code Mapping

### 7.1 libuvc Error Codes (Reference)

| Error Code | Native Constant | Meaning | Kotlin RecoveryAction |
|------------|-----------------|---------|----------------------|
| 0 | `UVC_SUCCESS` | Success | N/A |
| -1 | `UVC_ERROR_IO` | I/O error | `REPLUG_DEVICE` |
| -2 | `UVC_ERROR_INVALID_PARAM` | Invalid parameter | `RESTART_APP` |
| -3 | `UVC_ERROR_ACCESS` | Access denied | `WAIT_AND_RETRY` |
| -4 | `UVC_ERROR_NO_DEVICE` | Device not found | `REPLUG_DEVICE` |
| -5 | `UVC_ERROR_NOT_FOUND` | Entity not found | `REPLUG_DEVICE` |
| -6 | `UVC_ERROR_BUSY` | Device busy | `WAIT_AND_RETRY` |
| -7 | `UVC_ERROR_TIMEOUT` | Timeout | `RESET_USB_BUS` |
| -9 | `UVC_ERROR_NO_MEM` | Out of memory | `RESTART_APP` |

**Note**: Error code to RecoveryAction mapping should be implemented in `UsbRecoveryCoordinator.kt`

---

## Section 8: File Descriptor Contract

### 8.1 FD Lifecycle

| Stage | Owner | Action | Verified |
|-------|-------|--------|----------|
| Obtain FD | Kotlin | `usbManager.openDevice(device).fileDescriptor` | ☐ Kotlin |
| Pass to native | Kotlin → Native | `openSimple(fd, path)` | ✅ Native (line 171) |
| Duplicate FD | Native | `dup(fd)` in `connect()` | ✅ Native (line 168) |
| Store FD | Native | `mFd = fd` | ✅ Native (line 180) |
| Decrement count | Native | `decrementFdCount()` before close | ✅ Native (line 252) |
| Close FD | Native | `close(mFd)` in `release()` | ✅ Native (line 256) |
| Close connection | Kotlin | `usbDeviceConnection.close()` | ☐ Kotlin |

### 8.2 FD Count Invariants

| Invariant | Check | Expected Value |
|-----------|-------|----------------|
| After connect success | `activeFdCount` | 1 |
| After release | `activeFdCount` | 0 |
| After 50 reconnect cycles | `activeFdCount` | 0 |
| While streaming | `activeFdCount` | 1 |

---

## Section 9: Integration Test Checklist

### 9.1 Pre-Test Verification

| Check | Command | Expected Result | Status |
|-------|---------|-----------------|--------|
| Native lib builds | `./gradlew :lib:build` | BUILD SUCCESSFUL | ☐ |
| Native lib published | `./gradlew :lib:publishToMavenLocal` | Published to ~/.m2 | ☐ |
| ScopeCam builds | `./gradlew :app:assembleDebug` | BUILD SUCCESSFUL | ☐ |
| ScopeCam references correct lib version | Check `build.gradle.kts` | Matches published version | ☐ |

### 9.2 Runtime Flow Test

| Step | Action | Expected Log | Status |
|------|--------|--------------|--------|
| 1 | Launch app | `UsbConnectivityManager registered (passive mode)` | ☐ |
| 2 | Attach USB camera | `E2-K-004 State Observer: DeviceAttached` | ☐ |
| 3 | Permission dialog appears | `E2-K-004 State Observer: AwaitingPermission` | ☐ |
| 4 | Grant permission | `E2-K-004 State Observer: PermissionGranted` | ☐ |
| 5 | Native connecting | `setReadinessState(INITIALIZING)` | ☐ |
| 6 | Native ready | `setReadinessState(READY)` | ☐ |
| 7 | Kotlin connected | `E2-K-004 State Observer: Connected` | ☐ |
| 8 | Start preview | `setReadinessState(STREAMING)` | ☐ |
| 9 | Kotlin streaming | `E2-K-004 State Observer: Streaming` | ☐ |
| 10 | Detach device | `setReadinessState(DISCONNECTED)` | ☐ |
| 11 | Kotlin disconnected | `E2-K-004 State Observer: Disconnected` | ☐ |

**Logcat Filter**:

```bash
adb logcat -s "UsbConnectivityManager:D" "UVCCamera:D" "E2-K-004:D" "StreamTelemetry:D"
```

### 9.3 Stress Test Protocol

| Test | Procedure | Success Criteria | Status |
|------|-----------|------------------|--------|
| Rapid reconnect | Attach/detach 10x in 30 seconds | No stuck states, `activeFdCount == 0` at end | ☐ |
| Permission denial | Deny permission, retry | State returns to `Disconnected`, retry works | ☐ |
| Cold plug | Launch app with device attached | Detects device, requests permission | ☐ |
| Hot plug | Attach device while app running | State transitions correctly | ☐ |
| Realtek reset | Trigger error on Realtek device | `Resetting` state, 500ms silence, retry | ☐ |

---

## Section 10: Sign-Off

### Native Agent Verification

| Section | All Items Verified | Agent Signature | Date |
|---------|-------------------|-----------------|------|
| Section 1: JNI Signatures | ✅ | Claude Code (UVCCamera) | 2026-01-03 |
| Section 2: Enum Mappings | ⚠️ (StagnationPoint N/A) | Claude Code (UVCCamera) | 2026-01-03 |
| Section 3: Telemetry Struct | ✅ | Claude Code (UVCCamera) | 2026-01-03 |
| Section 4: USBMonitor Passive | ✅ | Claude Code (UVCCamera) | 2026-01-03 |
| Section 5: openSimple Contract | ✅ | Claude Code (UVCCamera) | 2026-01-03 |
| Section 6.1: Native Emissions | ✅ | Claude Code (UVCCamera) | 2026-01-03 |
| Section 8: FD Contract | ✅ | Claude Code (UVCCamera) | 2026-01-03 |

**Notes:**
- StagnationPoint enum is Kotlin-only (by design) - native does not emit stagnation states
- JNI-010 returns ByteBuffer (not byte[]) - more efficient
- Packed buffer uses uniform int64_t fields (corrected from original checklist)

### ScopeCam Agent Verification

| Section | All Items Verified | Agent Signature | Date |
|---------|-------------------|-----------------|------|
| Section 1: JNI Signatures | ☐ | | |
| Section 2: Enum Mappings | ☐ | | |
| Section 3: Telemetry Struct | ☐ | | |
| Section 4: USBMonitor Passive | ☐ | | |
| Section 5: openSimple Contract | ☐ | | |
| Section 6.2: Kotlin Transitions | ☐ | | |
| Section 8: FD Contract | ☐ | | |
| Section 9: Integration Tests | ☐ | | |

---

## Appendix A: Quick Reference Commands

```bash
# === NATIVE AGENT (UVCCamera repo) ===

# Verify all JNI registrations
grep -n "{ \"native" lib/src/main/jni/UVCCamera/serenegiant_usb_UVCCamera.cpp

# Verify ConnectionReadiness enum
grep -A6 "enum class ConnectionReadiness" lib/src/main/jni/UVCCamera/StreamTelemetry.h

# Verify readiness emissions
grep -n "setReadinessState\|incrementFdCount\|decrementFdCount" lib/src/main/jni/UVCCamera/UVCCamera.cpp

# Verify passive mode guards
grep -n "mPassiveMode" lib/src/main/java/com/serenegiant/usb/USBMonitor.java

# Verify openSimple implementation
grep -A25 "nativeConnectSimple" lib/src/main/jni/UVCCamera/serenegiant_usb_UVCCamera.cpp

# Verify packed buffer layout
grep -A60 "enum Field" lib/src/main/jni/UVCCamera/StreamTelemetry.h


# === SCOPECAM AGENT ===

# Verify state observer setup
grep -n "setupConnectionStateObserver\|handleConnectionStateChange" app/src/main/java/**/MainActivity.kt

# Verify lifecycle registration
grep -n "usbConnectivityManager.register\|usbConnectivityManager.unregister" app/src/main/java/**/MainActivity.kt

# Verify state transitions
grep -n "_state.value = CameraConnectionState" app/src/main/java/**/UsbConnectivityManager.kt

# Verify no legacy permission calls
grep -rn "\.requestPermission(" app/src/main/java/ --include="*.kt" | grep -v "@Deprecated\|// REMOVED"

# Verify enum mappings
grep -A10 "enum class SlotState\|enum class StagnationPoint" app/src/main/java/**/NativeTelemetry.kt
```
