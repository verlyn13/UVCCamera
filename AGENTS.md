# Repository Guidelines

## Project Structure & Modules
- Root Gradle (Kotlin DSL) with multiple Android modules.
- `lib/`: core library (AAR) sources and manifest.
- `usbCameraCommon/`: shared camera, GL, and utility code used by samples.
- `usbCameraTest6|7|8/`: sample apps (activities, resources, manifests, tests).
- `gh-pages/`: site assets; `upstreams/`: patch set for upstream sync.
- Typical Android layout: `module/src/main/java`, `module/src/main/res`, `module/src/test`, `module/src/androidTest`.

## Build, Test, and Development
- Build all: `./gradlew build` — compiles, runs unit tests, and checks lint where configured.
- Lint: `./gradlew lint` or `./gradlew :usbCameraCommon:lint` — Android Lint reports per module.
- Unit tests: `./gradlew test` — JUnit tests under `src/test`.
- Instrumented tests: `./gradlew connectedAndroidTest` — requires a device/emulator.
- Library only: `./gradlew :lib:assembleDebug`.
- Sample app (example): `./gradlew :usbCameraTest8:installDebug` — installs to a connected device.

## Coding Style & Naming
- Languages: Java (primary) + Gradle Kotlin DSL.
- Indentation: 4 spaces (Java), 2 spaces (Markdown/JSON/YAML).
- Names: Classes `UpperCamelCase`, methods/fields `lowerCamelCase`, constants `UPPER_SNAKE_CASE`.
- Packages follow existing `com.serenegiant...` hierarchy; keep module boundaries intact.
- Prefer readability over cleverness; add Javadoc for public APIs and complex logic.

## Testing Guidelines
- Frameworks: JUnit 4 for unit tests; AndroidX Instrumentation/Espresso for `androidTest`.
- Place tests alongside modules they cover (e.g., `usbCameraCommon/src/test`).
- Aim for meaningful coverage (≈80% where feasible); test error paths and threading-sensitive code.
- Naming: mirror class under test, e.g., `FpsCounterTest` for `FpsCounter`.

## Commits & Pull Requests
- Branches: `feature/<short-desc>` or `fix/<issue-id>-<desc>`.
- Commit messages: Conventional Commits preferred (e.g., `feat: add multi-surface handler`).
- Before pushing: `git pull --rebase origin main` and `./gradlew build` must pass.
- PRs include: concise summary, linked issues, test plan, screenshots or logs for UI/device changes, and affected modules.

## Security & Configuration
- Do not commit secrets/keystores. Debug builds use default signing; handle release signing externally.
- Validate external inputs and respect Android permission flows (see `usbCameraCommon` helpers).

## Agent Notes (Optional)
- Keep changes minimal and localized; follow existing patterns before adding dependencies.
- When automating, run Gradle with `--no-daemon --stacktrace` for clearer CI logs.
