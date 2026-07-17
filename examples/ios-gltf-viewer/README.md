# iOS GLTF Viewer

An iOS example app that demonstrates embedding the Mystral Native Runtime to render 3D GLTF models with PBR materials and touch-based rotation.

## Features

- Loads the DamagedHelmet GLTF model
- PBR rendering with IBL (Image-Based Lighting)
- HDR environment map for realistic reflections
- Touch-to-rotate: drag to orbit the camera around the model
- Works on both iOS Simulator and physical devices

## Prerequisites

1. **Xcode** (version 14 or later)
2. **iOS dependencies downloaded**:
   ```bash
   cd ../../
   node scripts/download-deps.mjs --ios
   ```

3. **mystral-runtime built for iOS**:
   ```bash
   # Build for device (arm64)
   cmake -B build-ios -G Xcode \
     -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
     -DPLATFORM=OS64 \
     -DCMAKE_BUILD_TYPE=Release
   cmake --build build-ios --config Release

   # OR build for simulator (Apple Silicon)
   cmake -B build-ios-sim -G Xcode \
     -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
     -DPLATFORM=SIMULATORARM64 \
     -DCMAKE_BUILD_TYPE=Release
   cmake --build build-ios-sim --config Release
   ```

## Building the Example

### Option 1: Using CMake (Recommended)

```bash
# For device
cmake -B build -G Xcode \
  -DCMAKE_TOOLCHAIN_FILE=../../cmake/ios.toolchain.cmake \
  -DPLATFORM=OS64 \
  -DMYSTRAL_BUILD_DIR=../../build-ios

# For simulator (Apple Silicon)
cmake -B build-sim -G Xcode \
  -DCMAKE_TOOLCHAIN_FILE=../../cmake/ios.toolchain.cmake \
  -DPLATFORM=SIMULATORARM64 \
  -DMYSTRAL_BUILD_DIR=../../build-ios-sim

# Open in Xcode
open build/MystralGLTFViewer.xcodeproj
# OR
open build-sim/MystralGLTFViewer.xcodeproj
```

### Option 2: Manual Xcode Project

1. Create a new iOS App project in Xcode
2. Add `main.mm` as the main source file
3. Remove the default AppDelegate and SceneDelegate
4. Add these resources to the bundle:
   - `gltf-viewer.js`
   - `DamagedHelmet.glb` (from `apps/mystral/static/assets/DamagedHelmet/glTF-Binary/`)
   - `sunny_rose_garden_2k.hdr` (from `apps/mystral/static/assets/Skyboxes/`)
5. Link against the mystral-runtime static library
6. Link required iOS frameworks (see CMakeLists.txt for the full list)

## Running

1. Select your target device or simulator in Xcode
2. For physical devices: set your Development Team in signing settings
3. Click Run (Cmd+R)

The app will:
1. Initialize the Mystral runtime with WebGPU
2. Load the GLTF model and HDR environment map
3. Start rendering with PBR materials
4. Respond to touch gestures for rotation

## Files

- `main.mm` - iOS entry point, sets up the runtime and loads JS code
- `gltf-viewer.js` - JavaScript rendering code with PBR shaders
- `Info.plist` - iOS app configuration
- `LaunchScreen.storyboard` - Launch screen
- `CMakeLists.txt` - CMake build configuration

## Troubleshooting

### "Could not find DamagedHelmet.glb in bundle"
Make sure the asset files are properly added to the Xcode project's "Copy Bundle Resources" build phase.

### Linker errors with mystral-runtime
Ensure you've built mystral-runtime for the correct platform (device vs simulator) and architecture (arm64).

### Metal validation errors
Enable Metal API validation in Xcode's scheme settings for debugging graphics issues.

## Architecture

```
┌──────────────────────────────────────────────────┐
│                  iOS App                          │
├──────────────────────────────────────────────────┤
│  main.mm                                          │
│    └── Sets up paths, loads gltf-viewer.js       │
├──────────────────────────────────────────────────┤
│  mystral-runtime (C++ library)                   │
│    ├── SDL3 (windowing, input, Metal surface)    │
│    ├── wgpu-native (WebGPU via Metal)            │
│    ├── Skia (Canvas 2D)                          │
│    └── QuickJS (JS engine)                       │
├──────────────────────────────────────────────────┤
│  gltf-viewer.js                                   │
│    ├── WebGPU rendering code                     │
│    ├── PBR shaders                               │
│    ├── Touch handlers                            │
│    └── loadGLTF() for model loading              │
└──────────────────────────────────────────────────┘
```
