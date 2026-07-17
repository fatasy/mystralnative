# iOS GLTF Viewer Example
#
# This builds an iOS app that embeds the mystral-runtime library.
#
# Build for device:
#   cmake -B build-ios -G Xcode \
#     -DCMAKE_TOOLCHAIN_FILE=../../cmake/ios.toolchain.cmake \
#     -DPLATFORM=OS64 \
#     -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-ios --config Release
#
# Build for simulator (Apple Silicon):
#   cmake -B build-ios-sim -G Xcode \
#     -DCMAKE_TOOLCHAIN_FILE=../../cmake/ios.toolchain.cmake \
#     -DPLATFORM=SIMULATORARM64 \
#     -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-ios-sim --config Release

cmake_minimum_required(VERSION 3.20)
project(MystralGLTFViewer LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# iOS-specific settings
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0")
set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.mystral.gltfviewer.dev")
set(MACOSX_BUNDLE_BUNDLE_VERSION "1")
set(MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0")

# Code signing configuration (optional - set your team ID here or via -DMYSTRAL_IOS_DEVELOPMENT_TEAM=XXXXXXXXXX)
set(MYSTRAL_IOS_DEVELOPMENT_TEAM "" CACHE STRING "Apple Development Team ID for code signing (leave empty to configure in Xcode)")

# Path to mystral-runtime build
set(MYSTRAL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(MYSTRAL_BUILD_DIR "${MYSTRAL_ROOT}/build-ios" CACHE PATH "Path to mystral-runtime iOS build")

# Source files
set(APP_SOURCES
    main.mm
)

# Resources to bundle (Info.plist is set separately via MACOSX_BUNDLE_INFO_PLIST)
set(APP_RESOURCES
    gltf-viewer.js
    LaunchScreen.storyboard
)

# Asset files (need to be copied)
set(ASSET_DIR "${MYSTRAL_ROOT}/../../apps/mystral/static/assets")
set(ASSET_FILES
    "${ASSET_DIR}/DamagedHelmet/glTF-Binary/DamagedHelmet.glb"
    "${ASSET_DIR}/Skyboxes/sunny_rose_garden_2k.hdr"
)

# Create the iOS app bundle
add_executable(MystralGLTFViewer MACOSX_BUNDLE
    ${APP_SOURCES}
    ${APP_RESOURCES}
    ${ASSET_FILES}
)

# Set resource file properties
foreach(RESOURCE ${APP_RESOURCES})
    set_source_files_properties(${RESOURCE} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
    )
endforeach()

# Copy asset files to bundle
foreach(ASSET ${ASSET_FILES})
    get_filename_component(ASSET_NAME ${ASSET} NAME)
    set_source_files_properties(${ASSET} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
    )
endforeach()

# Include directories (SDL3 added later after SDL3_FRAMEWORK_DIR is defined)
target_include_directories(MystralGLTFViewer PRIVATE
    ${MYSTRAL_ROOT}/include
)

# Link mystral-runtime library
# The library should be built with the iOS toolchain first
if(CMAKE_OSX_SYSROOT MATCHES "iphoneos")
    set(MYSTRAL_RUNTIME_LIB "${MYSTRAL_BUILD_DIR}/Release-iphoneos/libmystral-runtime.a")
else()
    set(MYSTRAL_RUNTIME_LIB "${MYSTRAL_BUILD_DIR}/Release-iphonesimulator/libmystral-runtime.a")
endif()

if(NOT EXISTS "${MYSTRAL_RUNTIME_LIB}")
    message(WARNING "mystral-runtime library not found at ${MYSTRAL_RUNTIME_LIB}")
    message(STATUS "Please build mystral-runtime for iOS first:")
    message(STATUS "  cd ${MYSTRAL_ROOT}")
    message(STATUS "  cmake -B build-ios-sim -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake -DPLATFORM=SIMULATORARM64")
    message(STATUS "  cmake --build build-ios-sim --config Release")
endif()

message(STATUS "mystral-runtime: ${MYSTRAL_RUNTIME_LIB}")

# Find dependencies in third_party directory
set(DEPS_DIR "${MYSTRAL_ROOT}/third_party")

# SDL3 - use xcframework (link as framework, not binary)
if(CMAKE_OSX_SYSROOT MATCHES "iphoneos")
    set(SDL3_FRAMEWORK_DIR "${DEPS_DIR}/sdl3/SDL3.xcframework/ios-arm64")
else()
    set(SDL3_FRAMEWORK_DIR "${DEPS_DIR}/sdl3/SDL3.xcframework/ios-arm64_x86_64-simulator")
endif()

# Add SDL3 include directories (must be after SDL3_FRAMEWORK_DIR is defined)
# SDL3 headers use <SDL3/...> includes, so we need to create a symlink structure
# We create a local include/SDL3 symlink pointing to the framework Headers
set(SDL3_INCLUDE_SETUP_DIR "${CMAKE_BINARY_DIR}/sdl3-include")
file(MAKE_DIRECTORY "${SDL3_INCLUDE_SETUP_DIR}")
# Create a symlink SDL3 -> actual Headers directory
execute_process(
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        "${SDL3_FRAMEWORK_DIR}/SDL3.framework/Headers"
        "${SDL3_INCLUDE_SETUP_DIR}/SDL3"
)
target_include_directories(MystralGLTFViewer PRIVATE
    ${SDL3_INCLUDE_SETUP_DIR}
)

# wgpu-native
if(CMAKE_OSX_SYSROOT MATCHES "iphoneos")
    set(WGPU_LIB "${DEPS_DIR}/wgpu-ios/device/lib/libwgpu_native.a")
else()
    # Simulator - use arm64 for Apple Silicon
    set(WGPU_LIB "${DEPS_DIR}/wgpu-ios/simulatorArm64/lib/libwgpu_native.a")
endif()

# Skia
if(CMAKE_OSX_SYSROOT MATCHES "iphoneos")
    set(SKIA_LIB "${DEPS_DIR}/skia-ios/device/build/ios-gpu/lib/Release/device-arm64/libskia.a")
else()
    set(SKIA_LIB "${DEPS_DIR}/skia-ios/simulator/build/ios-gpu/lib/Release/simulator-arm64/libskia.a")
endif()

# Add framework search path for SDL3
target_link_options(MystralGLTFViewer PRIVATE "-F${SDL3_FRAMEWORK_DIR}")

# Embed SDL3 framework using Xcode's native embedding (handles signing automatically)
set(SDL3_FRAMEWORK_PATH "${SDL3_FRAMEWORK_DIR}/SDL3.framework")
set_target_properties(MystralGLTFViewer PROPERTIES
    XCODE_EMBED_FRAMEWORKS "${SDL3_FRAMEWORK_PATH}"
    XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY YES
    XCODE_EMBED_FRAMEWORKS_REMOVE_HEADERS_ON_COPY YES
)

# Set rpath to find embedded frameworks
set_target_properties(MystralGLTFViewer PROPERTIES
    XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks"
)

# Link libraries and frameworks
target_link_libraries(MystralGLTFViewer PRIVATE
    ${MYSTRAL_RUNTIME_LIB}
    "-framework SDL3"
    ${WGPU_LIB}
    ${SKIA_LIB}
)

# iOS frameworks
target_link_libraries(MystralGLTFViewer PRIVATE
    "-framework UIKit"
    "-framework Foundation"
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework CoreServices"
    "-framework CoreText"
    "-framework CoreFoundation"
    "-framework ImageIO"
    "-framework IOSurface"
    "-framework GameController"
    "-framework CoreHaptics"
    "-framework AVFAudio"
    "-framework AudioToolbox"
)

# System libraries
target_link_libraries(MystralGLTFViewer PRIVATE
    "-lc++"
)

# Xcode-specific settings
set_target_properties(MystralGLTFViewer PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.mystral.gltfviewer.dev"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_INFOPLIST_KEY_UIApplicationSupportsIndirectInputEvents "YES"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
    XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH "YES"
    # Force arm64 only for Apple Silicon simulators
    XCODE_ATTRIBUTE_ARCHS "arm64"
    XCODE_ATTRIBUTE_VALID_ARCHS "arm64"
    XCODE_ATTRIBUTE_EXCLUDED_ARCHS "i386 x86_64"
)

# Set development team if provided (otherwise user configures in Xcode)
if(MYSTRAL_IOS_DEVELOPMENT_TEAM)
    set_target_properties(MystralGLTFViewer PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${MYSTRAL_IOS_DEVELOPMENT_TEAM}"
    )
    message(STATUS "iOS Development Team: ${MYSTRAL_IOS_DEVELOPMENT_TEAM}")
else()
    message(STATUS "iOS Development Team: Not set (configure in Xcode or pass -DMYSTRAL_IOS_DEVELOPMENT_TEAM=...)")
endif()
