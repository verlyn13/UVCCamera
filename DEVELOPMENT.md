# Development Workflow

This document describes the branch strategy and development workflow for the verlyn13/UVCCamera fork.

## Repository Structure

```
upstream:  alexey-pelykh/UVCCamera  (source of truth for general UVC library)
origin:    verlyn13/UVCCamera       (our fork for ScopeCam development)
```

## Branch Strategy

```
upstream/main ──────────────────────────────────────────────────►
       │                                          (sync periodically)
       ▼
origin/main ────────────────────────────────────────────────────►
       │                              (tracks upstream, PRs go here)
       │
       │ merge main → develop (pick up upstream changes)
       ▼
origin/develop ─────┬───────────┬───────────┬──────────────────►
                    │           │           │
                    ▼           ▼           ▼
              feat/phase-1  feat/phase-2  fix/xxx
                    │           │           │
                    └────PR─────┴────PR─────┴────PR──► develop
```

### Branch Purposes

| Branch | Purpose | Merges From | Merges To |
|--------|---------|-------------|-----------|
| `main` | Synced with upstream alexey-pelykh | upstream/main | develop |
| `develop` | Integration branch for ScopeCam work | main, feature branches | (tagged releases) |
| `feat/*` | Feature development | develop | develop (via PR) |
| `fix/*` | Bug fixes | develop | develop (via PR) |

### Upstream Contributions

For fixes/features intended for upstream (alexey-pelykh):

1. Branch from `main` (not develop)
2. Create PR to `upstream/main`
3. After merge, sync `main` then merge to `develop`

```bash
git checkout main
git pull upstream main
git checkout -b fix/upstream-bug-description
# ... make changes ...
git push -u origin fix/upstream-bug-description
# Create PR to alexey-pelykh/UVCCamera
```

## Development Workflow

### Starting New Work

```bash
# Ensure develop is up to date
git checkout develop
git pull origin develop

# Create feature branch
git checkout -b feat/descriptive-name

# ... develop ...

# Push and create PR to develop
git push -u origin feat/descriptive-name
```

### Feature Branch Naming

Use descriptive names based on the work:

- `feat/frame-buffer-core` - Phase 1: Core ring buffer implementation
- `feat/jni-bridge` - Phase 2: JNI bridge for Kotlin
- `feat/preview-integration` - Phase 3: UVCPreview.cpp modifications
- `feat/hardware-renderer` - Phase 4: EGL/GL rendering pipeline
- `feat/usb-hardening` - Phase 5: USB fallback and resilience
- `fix/memory-leak-description` - Bug fixes

### Syncing with Upstream

Periodically sync `main` with upstream and merge to `develop`:

```bash
# Sync main with upstream
git checkout main
git fetch upstream
git merge upstream/main  # or rebase if preferred
git push origin main

# Merge upstream changes to develop
git checkout develop
git merge main
git push origin develop
```

## CI/CD

### Continuous Integration

CI runs on:
- Push to `main`, `develop`, `feat/**`, `fix/**`
- Pull requests to `main` and `develop`

CI validates:
- Android library build (NDK/JNI compilation)
- Flutter plugin analysis and lint
- Flutter example app build

### Artifact Publishing

| Trigger | Action |
|---------|--------|
| Push to `main` | Publish SNAPSHOT to Maven Central |
| Version tag on `main` | Release to Maven Central + pub.dev |
| Push to `develop` | Build only (no publish) |
| Feature branches | Build only (no publish) |

## ScopeCam Integration

ScopeCam consumes this library via:
- Local Maven repository during development
- Maven Central releases for production

### Local Development

```bash
# In UVCCamera repo
./gradlew :lib:publishToMavenLocal

# In ScopeCam repo - uses mavenLocal() repository
```

## Phase-Based Development Plan

The AHardwareBuffer frame buffer implementation follows these phases:

| Phase | Branch | Deliverables |
|-------|--------|--------------|
| 1 | `feat/frame-buffer-core` | FrameBufferRing.cpp/h, FrameSlotMetadata.h, StreamTelemetry.h |
| 2 | `feat/jni-bridge` | FrameBufferJNI.cpp, FrameBufferManager.kt |
| 3 | `feat/preview-integration` | UVCPreview.cpp modifications |
| 4 | `feat/hardware-renderer` | EGLImage → GL texture pipeline |
| 5 | `feat/usb-hardening` | USB fallback ladder, stress testing |

Each phase:
1. Branches from `develop`
2. PRs back to `develop` when complete
3. CI validates before merge

## Branch Protection (Recommended)

Configure via GitHub Settings > Branches:

**For `main`:**
- Require PR reviews
- Require CI to pass
- No force push

**For `develop`:**
- Require CI to pass
- Allow maintainer force push (for rebasing)
