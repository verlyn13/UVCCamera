# UVCCamera Modernization Plan

**Objective:** Transform UVCCamera into a general-purpose, modern Android library suitable for upstream contribution while maintaining ScopeCam compatibility.

**Total Estimated Effort:** 3-4 weeks
**Breaking Changes:** None (additive modernization)

---

## Phase 1: Decouple ScopeCam References

**Effort:** 1-2 days
**Priority:** HIGH (unblocks general release)
**Breaking Changes:** None

### 1.1 Make JNI Class Names Configurable

Currently hardcoded in 2 locations:
- `lib/src/main/jni/UVCCamera/FrameBufferJNI.cpp:790`
- `lib/src/main/jni/UVCCamera/EGLImageHelperJNI.cpp:534`

**Tasks:**
- [ ] Create `JNIConfig.h` with default class name constants
- [ ] Add `UVCCamera.setAdvancedFrameManagerClass(String)` Java method
- [ ] Add `UVCCamera.setEGLImageHelperClass(String)` Java method
- [ ] Modify JNI registration to use configured class names
- [ ] Default to `com/serenegiant/usb/FrameBufferManager` and `com/serenegiant/usb/EGLImageHelper`
- [ ] Add stub Java classes as opt-in templates (can be empty/no-op)

**Files to modify:**
```
lib/src/main/jni/UVCCamera/FrameBufferJNI.cpp
lib/src/main/jni/UVCCamera/EGLImageHelperJNI.cpp
lib/src/main/jni/UVCCamera/_onload.cpp (if needed for config)
lib/src/main/java/com/serenegiant/usb/UVCCamera.java
```

### 1.2 Update Copyright Headers

Replace ScopeCam-specific copyright with general contributor attribution.

**Tasks:**
- [ ] Update ring buffer files to "UVCCamera Contributors" copyright
- [ ] Keep original saki4510t attribution where it exists
- [ ] Add your contributor attribution

**Files to update (native):**
```
lib/src/main/jni/UVCCamera/FrameBufferRing.cpp
lib/src/main/jni/UVCCamera/FrameBufferRing.h
lib/src/main/jni/UVCCamera/FrameBufferJNI.cpp
lib/src/main/jni/UVCCamera/EGLImageHelperJNI.cpp
lib/src/main/jni/UVCCamera/LayoutContract.cpp
lib/src/main/jni/UVCCamera/LayoutContract.h
lib/src/main/jni/UVCCamera/StreamTelemetry.h
lib/src/main/jni/UVCCamera/FrameSlotMetadata.h
```

### 1.3 Reorganize Documentation

**Tasks:**
- [ ] Create `docs/integrations/` directory
- [ ] Move `docs/ScopeCam-Integration-Guide.md` to `docs/integrations/scopecam.md`
- [ ] Create generic `docs/Advanced-Features.md` covering ring buffer, telemetry
- [ ] Update `docs/README.md` to be library-focused (remove ScopeCam specifics)
- [ ] Review and update mentions in:
  - `DEVELOPMENT.md` (5 mentions)
  - `docs/Native-Testing.md` (6 mentions)
  - `docs/Native-Kotlin-Alignment-Checklist.md` (12 mentions)
  - Other docs with 1-2 mentions each

**Documentation structure after:**
```
docs/
├── README.md                    # Library overview
├── api-reference.md             # API docs
├── architecture.md              # Architecture docs
├── Advanced-Features.md         # NEW: Ring buffer, telemetry, GPU pipeline
├── integrations/                # NEW: Consumer-specific guides
│   └── scopecam.md              # Moved ScopeCam guide
└── archive/                     # Historical planning docs
```

### 1.4 Update DEVELOPMENT.md

- [ ] Remove ScopeCam-specific workflow references
- [ ] Generalize branch strategy documentation
- [ ] Keep technical accuracy for contributors

---

## Phase 2: Kotlin Migration

**Effort:** 1-2 weeks
**Priority:** MEDIUM
**Breaking Changes:** None (additive layer)

### 2.1 Add Kotlin Plugin

**Tasks:**
- [ ] Add Kotlin plugin to `lib/build.gradle.kts`
- [ ] Add Kotlin to version catalog (`libs.versions.toml`)
- [ ] Add coroutines dependency
- [ ] Verify build still works

**build.gradle.kts additions:**
```kotlin
plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)  // NEW
}

dependencies {
    implementation(libs.kotlinx.coroutines.android)  // NEW
}

android {
    kotlinOptions {
        jvmTarget = "11"
    }
}
```

**libs.versions.toml additions:**
```toml
[versions]
kotlin = "2.0.21"
kotlinxCoroutines = "1.9.0"

[libraries]
kotlinx-coroutines-android = { module = "org.jetbrains.kotlinx:kotlinx-coroutines-android", version.ref = "kotlinxCoroutines" }

[plugins]
kotlin-android = { id = "org.jetbrains.kotlin.android", version.ref = "kotlin" }
```

### 2.2 Create Kotlin Wrapper Classes

**New file structure:**
```
lib/src/main/kotlin/org/uvccamera/
├── UvcCamera.kt              # Main camera class (wraps UVCCamera.java)
├── UvcCameraSession.kt       # Camera session lifecycle
├── UvcDevice.kt              # Device representation
├── UvcMonitor.kt             # Wraps USBMonitor
├── events/
│   ├── DeviceEvent.kt        # Sealed class for device events
│   ├── StatusEvent.kt        # Sealed class for status events
│   └── ButtonEvent.kt        # Sealed class for button events
├── settings/
│   └── CameraControls.kt     # Type-safe camera controls
└── extensions/
    └── FlowExtensions.kt     # callbackFlow builders
```

**Tasks:**
- [ ] Create `UvcCamera.kt` - main entry point with suspend functions
- [ ] Create `UvcCameraSession.kt` - represents open camera session
- [ ] Create `UvcDevice.kt` - immutable device representation
- [ ] Create `UvcMonitor.kt` - Flow-based device monitoring
- [ ] Create event sealed classes
- [ ] Create `CameraControls.kt` - type-safe control settings
- [ ] Add callbackFlow builders for event streams
- [ ] Write unit tests for Kotlin wrappers

**Example API:**
```kotlin
class UvcCamera(context: Context) {
    val deviceEvents: Flow<DeviceEvent>
    val statusEvents: Flow<StatusEvent>

    suspend fun open(device: UvcDevice): UvcCameraSession
    suspend fun close()
}

class UvcCameraSession internal constructor(
    private val camera: UVCCamera  // Java class
) {
    val isOpen: Boolean
    val frames: Flow<FrameData>

    suspend fun setPreviewSize(width: Int, height: Int)
    suspend fun startPreview(surface: Surface)
    suspend fun stopPreview()
    suspend fun captureStillImage(): Bitmap

    val controls: CameraControls
}

sealed class DeviceEvent {
    data class Attached(val device: UvcDevice) : DeviceEvent()
    data class Detached(val device: UvcDevice) : DeviceEvent()
    data class PermissionGranted(val device: UvcDevice) : DeviceEvent()
    data class PermissionDenied(val device: UvcDevice) : DeviceEvent()
}
```

### 2.3 Add Coroutine Support

**Tasks:**
- [ ] Wrap blocking operations in `withContext(Dispatchers.IO)`
- [ ] Create `suspendCancellableCoroutine` wrappers for callbacks
- [ ] Add proper cancellation handling
- [ ] Add timeout support for long operations

### 2.4 Add Flow-Based Event Streams

**Tasks:**
- [ ] Create `callbackFlow` for device attach/detach events
- [ ] Create `callbackFlow` for status events
- [ ] Create `callbackFlow` for button events
- [ ] Create `channelFlow` for frame streaming (advanced path)
- [ ] Ensure proper cleanup on Flow cancellation

---

## Phase 3: Modern Android Patterns

**Effort:** 1 week
**Priority:** MEDIUM
**Breaking Changes:** Minor (pure AndroidX)

### 3.1 Lifecycle-Aware Components

**Tasks:**
- [ ] Create `LifecycleUvcCamera` that auto-manages start/stop
- [ ] Implement `DefaultLifecycleObserver` for automatic cleanup
- [ ] Add lifecycle state checks before operations
- [ ] Document lifecycle integration

**Example:**
```kotlin
class LifecycleUvcCamera(
    lifecycle: Lifecycle,
    context: Context
) : DefaultLifecycleObserver {

    private val camera = UvcCamera(context)

    init {
        lifecycle.addObserver(this)
    }

    override fun onDestroy(owner: LifecycleOwner) {
        runBlocking { camera.close() }
    }
}
```

### 3.2 ViewModel Integration Examples

**Tasks:**
- [ ] Create sample `UvcCameraViewModel` in sample app
- [ ] Show proper state management with `StateFlow`
- [ ] Demonstrate error handling patterns
- [ ] Add to documentation

### 3.3 Jetpack Compose Preview Widget

**New files:**
```
lib/src/main/kotlin/org/uvccamera/compose/
├── UvcCameraPreview.kt       # Composable preview
└── rememberUvcCamera.kt      # State holder
```

**Tasks:**
- [ ] Add Compose dependencies to version catalog
- [ ] Create `UvcCameraPreview` composable using `AndroidView`
- [ ] Create `rememberUvcCamera()` state holder
- [ ] Add preview size configuration
- [ ] Handle orientation changes
- [ ] Add sample in compose sample app

### 3.4 Migrate to Pure AndroidX

**Tasks:**
- [ ] Remove `com.android.support` dependencies from version catalog
- [ ] Update test apps to use AndroidX equivalents
- [ ] Verify no Android Support Library remains
- [ ] Update min SDK if needed (currently 26, which is fine)

**libs.versions.toml cleanup:**
Remove:
```toml
appcompat7 = "28.0.0"  # Old support lib
supportTestRunner = "1.0.2"  # Old test runner
supportEspressoCore = "3.0.2"  # Old espresso
appcompat-v7 = { ... }
support-v4 = { ... }
support-annotations = { ... }
support-test-runner = { ... }
support-espresso-core = { ... }
```

Keep/Use:
```toml
appcompat = "1.7.0"  # AndroidX
androidx-junit = { ... }
androidx-espresso-core = { ... }
```

---

## Phase 4: Documentation & Samples

**Effort:** 3-5 days
**Priority:** HIGH (for adoption)

### 4.1 Getting Started Guide

**Tasks:**
- [ ] Create `docs/Getting-Started.md`
- [ ] Cover Gradle dependency setup
- [ ] Show minimal working example (Java)
- [ ] Show minimal working example (Kotlin)
- [ ] Document USB permission flow
- [ ] Add troubleshooting section

### 4.2 Kotlin Sample App

**New module:** `samples/kotlin-app/`

**Tasks:**
- [ ] Create new Android app module
- [ ] Implement device discovery UI
- [ ] Implement camera preview with controls
- [ ] Show proper lifecycle integration
- [ ] Use coroutines throughout
- [ ] Add to settings.gradle.kts

### 4.3 Compose Sample App

**New module:** `samples/compose-app/`

**Tasks:**
- [ ] Create new Compose Android app module
- [ ] Implement Material 3 UI
- [ ] Show `UvcCameraPreview` composable usage
- [ ] Demonstrate state management
- [ ] Add camera controls UI

### 4.4 API Documentation (KDoc)

**Tasks:**
- [ ] Add KDoc to all public Kotlin classes
- [ ] Add KDoc to all public functions
- [ ] Document parameters and return values
- [ ] Add code examples in documentation
- [ ] Configure Dokka for doc generation
- [ ] Add Dokka to CI workflow

---

## Implementation Order & Dependencies

```
Phase 1.1 (JNI config) ─┬─► Phase 1.2 (copyright) ─► Phase 1.3 (docs) ─► Phase 1.4 (DEVELOPMENT.md)
                        │
                        └─► Phase 2.1 (Kotlin plugin) ─► Phase 2.2 (wrappers) ─► Phase 2.3 (coroutines)
                                                                                        │
                                                                                        ▼
                                                        Phase 2.4 (Flow) ◄─────────────┘
                                                              │
                                                              ▼
                             Phase 3.1 (lifecycle) ─► Phase 3.2 (ViewModel) ─► Phase 3.3 (Compose)
                                                              │
                                                              ▼
                                                     Phase 3.4 (AndroidX)
                                                              │
                                                              ▼
                           Phase 4.1 (Getting Started) ─► Phase 4.2 (Kotlin sample) ─► Phase 4.3 (Compose sample)
                                                              │
                                                              ▼
                                                       Phase 4.4 (KDoc)
```

**Recommended execution:**
1. Phase 1 complete (unblock general release)
2. Phase 2.1 + 2.2 (Kotlin foundation)
3. Phase 4.1 (docs for early adopters)
4. Phase 2.3 + 2.4 (complete Kotlin API)
5. Phase 3 (modern patterns)
6. Phase 4.2 + 4.3 + 4.4 (samples and docs)

---

## Success Criteria

- [ ] Library builds and publishes without ScopeCam dependencies
- [ ] Existing Java API unchanged (backward compatible)
- [ ] Kotlin API available alongside Java
- [ ] All test apps build successfully
- [ ] Documentation covers both Java and Kotlin usage
- [ ] CI/CD validates all modules
- [ ] Published to Maven Central under `org.uvccamera:lib`

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Breaking Java API | Kotlin wrapper only, Java API unchanged |
| NDK build issues | Phase 1 only touches JNI registration, not core native code |
| Test app failures | Update incrementally, test after each change |
| Compose version conflicts | Use BOM, make Compose optional (separate artifact if needed) |

---

## Next Steps

1. **Commit this plan** to `docs/MODERNIZATION-PLAN.md`
2. **Create GitHub issues** for each phase (or use as single epic)
3. **Start Phase 1** - lowest risk, highest impact
4. **Parallel track** - Begin Phase 2.1 (Kotlin plugin) while finishing Phase 1
