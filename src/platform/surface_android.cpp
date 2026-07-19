/**
 * Android Surface Creation (Vulkan via ANativeWindow)
 *
 * On Android, Dawn creates Vulkan surfaces from ANativeWindow
 * handles obtained through SDL3's Android backend.
 */

#include <iostream>

#if defined(__ANDROID__)

#include <android/native_window.h>

namespace mystral {
namespace platform {

/**
 * Get the ANativeWindow from SDL window for WebGPU surface creation.
 * This is called internally by the WebGPU bindings when creating a surface.
 *
 * Note: SDL3 handles most of the Android-specific work. The ANativeWindow
 * is obtained from the SDL_Window using SDL_GetProperty with
 * SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER.
 */
void* getAndroidNativeWindow(void* sdlWindow) {
    // The actual ANativeWindow extraction happens in webgpu/context.cpp
    // using SDL_GetProperty. This file exists for platform-specific
    // surface utilities if needed in the future.
    std::cout << "[Surface] Android native window requested" << std::endl;
    return nullptr;
}

}  // namespace platform
}  // namespace mystral

#endif  // __ANDROID__
