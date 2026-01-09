# UVCCamera Documentation

This is a maintained fork of UVCCamera with enhanced ring buffer support for zero-copy GPU rendering.

## Quick Start

| If you want to... | Read this |
|-------------------|-----------|
| Understand the architecture | [architecture.md](./architecture.md) |
| Use the public API | [api-reference.md](./api-reference.md) |
| Integrate with an app | [ScopeCam-Integration-Guide.md](./ScopeCam-Integration-Guide.md) |

---

## Core Documentation

| Document | Description |
|----------|-------------|
| [architecture.md](./architecture.md) | System architecture, pipeline topology, frame routing, threading model |
| [api-reference.md](./api-reference.md) | Public API reference with method signatures and usage examples |

---

## Integration & Contracts

| Document | Description |
|----------|-------------|
| [ScopeCam-Integration-Guide.md](./ScopeCam-Integration-Guide.md) | Integration guide for consuming applications |
| [Native-Kotlin-Alignment-Checklist.md](./Native-Kotlin-Alignment-Checklist.md) | JNI contract verification checklist |
| [Native-Ground-Truth.md](./Native-Ground-Truth.md) | ScopeCam-specific integration context |

---

## Implementation Details

| Document | Description |
|----------|-------------|
| [Phase4-Bidirectional-Fence-Implementation.md](./Phase4-Bidirectional-Fence-Implementation.md) | GPU/CPU fence synchronization |
| [Producer-Consumer-Handshake-Trace.md](./Producer-Consumer-Handshake-Trace.md) | Frame handoff trace between threads |

---

## Planning & Future Work

| Document | Description |
|----------|-------------|
| [MODERNIZATION-PLAN.md](./MODERNIZATION-PLAN.md) | Roadmap for upstream contribution |
| [SafeUvcCameraManager-Audit-Feasibility-Report.md](./SafeUvcCameraManager-Audit-Feasibility-Report.md) | Kotlin wrapper feasibility analysis |

---

## Testing (Future)

| Document | Description |
|----------|-------------|
| [Native-Testing.md](./Native-Testing.md) | Native test infrastructure plan |
| [Native-Testing-Addition.md](./Native-Testing-Addition.md) | Test implementation supplements |

---

## Archive

Historical planning documents, bug reports, and superseded designs are preserved in [archive/](./archive/).

| Document | Description |
|----------|-------------|
| [archive/JNI_SIGNATURE_MISMATCH_REPORT.md](./archive/JNI_SIGNATURE_MISMATCH_REPORT.md) | JNI signature bug fix (2026-01-08) |
| [archive/architecture-upgrade.md](./archive/architecture-upgrade.md) | Original WARM/HOT state design notes |
| [archive/bus-plan*.md](./archive/) | Historical bus factor planning |

---

## Key Concepts

### Frame Routing (P0 Fix - 2026-01-09)

The conversion thread routes frames based on **consumer availability**:

```cpp
bool hasConsumer = surfaceReady || ringConsumerActive;

if (hasConsumer) {
    ring->unlockWriteBuffer();   // Commit frame
} else {
    ring->cancelWriteBuffer();   // Active drain
}
```

- `surfaceReady`: ANativeWindow display path active
- `ringConsumerActive`: Ring buffer mode with Kotlin GL consumer

### State Machine

| State | Description |
|-------|-------------|
| COLD | No USB streaming, preview thread stopped |
| WARM | USB streaming active, no display consumer (capture-only) |
| HOT | USB streaming + display consumer active |

---

## Build & Test

```bash
# Build AAR
./gradlew :lib:assembleRelease

# Publish to local Maven
./gradlew :lib:publishToMavenLocal

# Verify frame routing (after deployment)
adb logcat | grep -E "STATE_TRACE|RING_WRITE|latest="
```
