# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository shape

ViroCore is a cross-platform 3D/AR/VR engine. One C++ core (`ViroRenderer/`) is compiled against four platform shells:

- `android/` — Gradle/NDK build producing `.aar` artifacts consumed by React Native and standalone Android apps
- `ios/` — CocoaPods + Xcode build producing `ViroKit.framework`
- `macos/` — Xcode build mirroring iOS, separate `ViroRenderer.xcodeproj`
- `wasm/` — Emscripten/CMake build for browser test harnesses

The C++ core in `ViroRenderer/` is flat (~200 `.cpp` files at the top level). JNI bindings for Android live in `ViroRenderer/capi/` (one `*_JNI.cpp` per exposed Java class). Android-specific C++ glue (scene renderers for GVR/OVR/ARCore/SceneView, JNI entrypoints) lives in `android/sharedCode/src/main/cpp/`.

## Android build

Java 17, NDK `25.2.9519653`, `compileSdkVersion 33`, `minSdkVersion 24`, Kotlin 1.8.21, AGP 8.2.0. ABIs: `arm64-v8a` and `armeabi-v7a` only (no x86, no x86_64 in shipped libs).

```bash
cd android
./gradlew :viroreact:assembleDebug      # debug AAR
./gradlew :viroreact:assembleRelease    # release AAR (viroreact-release.aar)
./gradlew :viroreact:check              # lint + checks (CI runs this)
./gradlew clean
```

### Module layout — the non-obvious part

`android/` has four modules and they are deliberately not what they appear:

- **`sharedCode/`** — holds all actual Java/Kotlin, resources, `AndroidManifest.xml`, `jniLibs/`, C++ (`src/main/cpp/`), and the authoritative `CMakeLists.txt`. But `sharedCode` itself is **IDE-only** — its `build.gradle` exists purely so Android Studio gives code completion. It is not used as a Gradle dependency and produces nothing useful.
- **`viroreact/`** — thin wrapper whose `sourceSets` point at `../sharedCode`. Produces `viroreact-release.aar`, the canonical AAR. Sets `VIRO_PLATFORM = "VIRO_REACT"` in `BuildConfig`.
- **`virocore/`** — same wrapper pattern pointing at `../sharedCode`. Sets `VIRO_PLATFORM = "VIRO_CORE"` and runs the `preprocessor.gradle` IFDEF pass (see below). **`assembleRelease` / `assembleDebug` are explicitly disabled** (see `virocore/build.gradle` bottom). Build `:viroreact` instead. The Fastlane `virorenderer_virocore_aar` lane is stale; if you need a virocore AAR, re-enable the tasks.
- **`viroar/`** — separate AAR for the ARCore-dependent JNI (`viro_arcore` library). `viroreact`/`virocore` depend on it: `assembleRelease` on either of them runs `:viroar:assembleRelease` first, which extracts `.so` files into `viroar/build/natives/jni/`, which the wrapper modules then package via `jniLibs.srcDirs`.

Consequence: editing Java/C++ in `sharedCode` and running `:sharedCode:assembleRelease` does nothing useful. Always build via `:viroreact`.

### Java preprocessor (virocore-only)

`android/virocore/preprocessor.gradle` runs a custom IFDEF preprocessor that edits Java files **in place** in `sharedCode/src/main/java/` when building `:virocore:assembleRelease`. It uncomments lines between `//#IFDEF 'release'` / `//#ELSE` / `//#ENDIF` markers and comments out the other branch. Implications:

- Only runs for `:virocore` release builds (if you re-enable them). Does **not** run for `:viroreact`.
- Edits are persistent — running it dirties your working tree. Revert with `git checkout` before committing.
- When reading code in `sharedCode`, treat both IFDEF branches as live.

### Optional native dependencies

- **ReactVisionCCA** (proprietary cloud anchor lib): CMake auto-detects `libreactvisioncca.so` in `android/sharedCode/src/main/jniLibs/{abi}/` plus header at `../../../reactvisioncca/include/ReactVisionCCA/RVCCACloudAnchorProvider.h`. Found → `RVCCA_AVAILABLE=1`; missing → stubs compile that report the feature as unavailable. Open-source builds are expected to skip this.
- **ARCore** (Android): real pod dependency (`com.google.ar:core:1.51.0`). Not optional here.
- The `copyReleaseAAR` task in `viroreact/build.gradle` copies the output to `../../../viro/android/viro_renderer/viro_renderer-release.aar` — a sibling `viro/` repo expects the AAR there. Harmless if the path doesn't exist (task will fail; delete or fix if you don't have that sibling).

## iOS build

Requires CocoaPods, Xcode 16.4 (per CI), iOS deployment target 13.0. Mac Catalyst is disabled (`ViroKit` uses GLKit + ARCore).

```bash
cd ios
pod install --verbose
xcodebuild -workspace ViroRenderer.xcworkspace -scheme ViroKit -sdk iphoneos -configuration Release
# Fastlane wrappers (both framework and static lib, device + simulator):
cd ios && fastlane virorender_viroreact_virokit
```

ARCore is an **optional** dependency on iOS: the `post_install` hook in `ios/Podfile` rewrites `-framework ARCore*` to `-weak_framework ARCore*` across ViroKit's xcconfigs. Downstream consumers that don't include ARCore pods get `isAvailable = false` at runtime rather than a linker failure. The same hook also weak-links the Firebase, GoogleToolbox, GTMSessionFetcher, nanopb, and PromisesObjC frameworks that ARCore transitively depends on — when adding ARCore-related dependencies, add the new framework name to the `arcore_frameworks` list in the hook or linking will be wrong.

## WASM build

Separate CMake project at `wasm/CMakeLists.txt`, driven by Emscripten.

```bash
source ~/Source/emsdk/emsdk_env.sh   # set your emsdk path
cd wasm
./build.sh viro_test_fbx             # or any test target from CMakeLists.txt
./run.sh                              # serves ./products/build/ on :8080
```

Detailed CLion setup is in `wasm/README.md`.

## CI

- `.github/workflows/pull-request.yml` — on every PR: `:viroreact:assembleDebug`, `:viroreact:check`, and an iOS `xcodebuild` of the `ViroKit` scheme. Artifacts are uploaded.
- `.github/workflows/release.yml` — triggered by git tag push; tags matching `*alpha*`/`*beta*`/`*rc*` are marked prerelease. Builds `:viroreact:assembleRelease` and publishes the four AARs (`viroreact`, `viroar`, `virocore`, `sharedCode`) to a GitHub Release.
- `Jenkinsfile` and `android/fastlane/Fastfile` describe the legacy Viro Media Jenkins pipeline (Slack hooks, S3 artifact sync); not active in this fork but kept for reference.

Version comes from `git describe --tags` (see `android/build.gradle`'s `getTagVersion`), so a tagged commit is required for meaningful version strings.
