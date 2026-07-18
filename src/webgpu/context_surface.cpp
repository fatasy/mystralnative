/**
 * WebGPU surface creation and presentation.
 */

#include "mystral/webgpu/context.h"
#include "context_device_helpers.h"

#include <iostream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
#include "webgpu/webgpu.h"
#include "mystral/webgpu_compat.h"
#endif

namespace mystral {
namespace webgpu {

using namespace detail;

bool Context::createSurface(void* nativeHandle, int platformType) {
    if (!instance_) {
        std::cerr << "[WebGPU] Cannot create surface: no instance" << std::endl;
        return false;
    }

    std::cout << "[WebGPU] Creating surface for platform type " << platformType << std::endl;

    WGPUSurfaceDescriptor surfaceDesc = {};

    // Declare platform-specific descriptors outside the switch to avoid use-after-free
    // (the pointer in nextInChain must remain valid until wgpuInstanceCreateSurface returns)
#if defined(__APPLE__)
    WGPUSurfaceDescriptorFromMetalLayer_Compat metalDesc = {};
#endif
#if defined(_WIN32)
    WGPUSurfaceDescriptorFromWindowsHWND_Compat windowsDesc = {};
#endif
#if defined(__ANDROID__)
    WGPUSurfaceDescriptorFromAndroidNativeWindow_Compat androidDesc = {};
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    WGPUSurfaceDescriptorFromXlibWindow_Compat xlibDesc = {};
#endif

    switch (platformType) {
#if defined(__APPLE__)
        case PLATFORM_METAL:
            metalDesc.chain.sType = WGPUSType_SurfaceDescriptorFromMetalLayer_Compat;
            metalDesc.layer = nativeHandle;
            surfaceDesc.nextInChain = (WGPUChainedStruct*)&metalDesc;
            break;
#endif
#if defined(_WIN32)
        case PLATFORM_WINDOWS:
            windowsDesc.chain.sType = WGPUSType_SurfaceDescriptorFromWindowsHWND_Compat;
            windowsDesc.hinstance = GetModuleHandle(NULL);
            windowsDesc.hwnd = nativeHandle;
            surfaceDesc.nextInChain = (WGPUChainedStruct*)&windowsDesc;
            break;
#endif
#if defined(__ANDROID__)
        case PLATFORM_ANDROID:
            androidDesc.chain.sType = WGPUSType_SurfaceDescriptorFromAndroidNativeWindow_Compat;
            androidDesc.window = nativeHandle;
            surfaceDesc.nextInChain = (WGPUChainedStruct*)&androidDesc;
            std::cout << "[WebGPU] Creating Android surface with ANativeWindow: " << nativeHandle << std::endl;
            break;
#endif
#if defined(__linux__) && !defined(__ANDROID__)
        case PLATFORM_XLIB:
            xlibDesc.chain.sType = WGPUSType_SurfaceDescriptorFromXlibWindow_Compat;
            xlibDesc.display = nullptr;  // Will be set by wgpu from the environment
            xlibDesc.window = reinterpret_cast<uint64_t>(nativeHandle);
            surfaceDesc.nextInChain = (WGPUChainedStruct*)&xlibDesc;
            std::cout << "[WebGPU] Creating X11 surface with window: " << nativeHandle << std::endl;
            break;
#endif
        default:
            std::cerr << "[WebGPU] Unsupported platform type: " << platformType << std::endl;
            return false;
    }

    surface_ = wgpuInstanceCreateSurface(instance_, &surfaceDesc);
    if (!surface_) {
        std::cerr << "[WebGPU] Failed to create surface" << std::endl;
        return false;
    }
    std::cout << "[WebGPU] Surface created" << std::endl;

    // Now request adapter with surface compatibility
    WGPURequestAdapterOptions adapterOptions = {};
    adapterOptions.compatibleSurface = surface_;
    adapterOptions.powerPreference = WGPUPowerPreference_HighPerformance;

    AdapterRequestData adapterData;

#if WGPU_USES_CALLBACK_INFO_PATTERN
    // Dawn uses CallbackInfo struct with required callback mode
    WGPURequestAdapterCallbackInfo callbackInfo = {};
    callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    callbackInfo.callback = onAdapterRequestEnded;
    callbackInfo.userdata1 = &adapterData;
    callbackInfo.userdata2 = nullptr;
    wgpuInstanceRequestAdapter(instance_, &adapterOptions, callbackInfo);
#else
    // wgpu-native uses separate callback and userdata
    wgpuInstanceRequestAdapter(instance_, &adapterOptions, onAdapterRequestEnded, &adapterData);
#endif

    // wgpu-native is synchronous for requestAdapter, but we should poll just in case
#if defined(MYSTRAL_WEBGPU_WGPU)
    while (!adapterData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#elif defined(MYSTRAL_WEBGPU_DAWN)
    // Dawn also needs event processing
    while (!adapterData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#endif

    if (!adapterData.adapter) {
        std::cerr << "[WebGPU] Failed to get adapter" << std::endl;
        return false;
    }
    adapter_ = adapterData.adapter;

    // Print adapter info
    WGPUAdapterInfo adapterInfo = {};
    wgpuAdapterGetInfo(adapter_, &adapterInfo);
    std::cout << "[WebGPU] Adapter: " << WGPU_PRINT_STRING_VIEW(adapterInfo.device) << std::endl;
    std::cout << "[WebGPU] Vendor: " << WGPU_PRINT_STRING_VIEW(adapterInfo.vendor) << std::endl;
    std::cout << "[WebGPU] Backend: ";
    switch (adapterInfo.backendType) {
        case WGPUBackendType_Null: std::cout << "Null"; break;
        case WGPUBackendType_WebGPU: std::cout << "WebGPU"; break;
        case WGPUBackendType_D3D11: std::cout << "D3D11"; break;
        case WGPUBackendType_D3D12: std::cout << "D3D12"; break;
        case WGPUBackendType_Metal: std::cout << "Metal"; break;
        case WGPUBackendType_Vulkan: std::cout << "Vulkan"; break;
        case WGPUBackendType_OpenGL: std::cout << "OpenGL"; break;
        case WGPUBackendType_OpenGLES: std::cout << "OpenGLES"; break;
        default: std::cout << "Unknown"; break;
    }
    std::cout << std::endl;
    wgpuAdapterInfoFreeMembers(adapterInfo);

    // Request device with required limits
    WGPUDeviceDescriptor deviceDesc = {};
    WGPU_SET_LABEL(deviceDesc, "Mystral Device");
#if defined(MYSTRAL_WEBGPU_DAWN)
    WGPUDawnCacheDeviceDescriptor cacheDesc;
    attachDawnCache(deviceDesc, cacheDesc);
#endif

    // Set up required limits - copy adapter limits and override what we need
    // WebGPU default is 32 bytes per sample, but deferred rendering needs ~40
    // Chrome defaults: https://www.w3.org/TR/webgpu/#limits
#if defined(MYSTRAL_WEBGPU_DAWN)
    // Dawn uses WGPULimits directly
    WGPULimits adapterLimits = {};
    wgpuAdapterGetLimits(adapter_, &adapterLimits);

    // Start with adapter limits as baseline (avoids minimum limit validation errors)
    WGPULimits requiredLimits = adapterLimits;

    // Request higher maxColorAttachmentBytesPerSample for deferred rendering
    uint32_t neededBytesPerSample = 64;  // 4 RGBA16Float + 1 BGRA8 = 40, round up
    if (adapterLimits.maxColorAttachmentBytesPerSample >= neededBytesPerSample) {
        requiredLimits.maxColorAttachmentBytesPerSample = neededBytesPerSample;
        std::cout << "[WebGPU] Requesting maxColorAttachmentBytesPerSample: " << neededBytesPerSample << std::endl;
    }

    deviceDesc.requiredLimits = &requiredLimits;
#elif defined(MYSTRAL_WEBGPU_WGPU)
    // wgpu-native uses WGPURequiredLimits wrapper
    WGPUSupportedLimits adapterLimits = {};
    wgpuAdapterGetLimits(adapter_, &adapterLimits);

    // Start with adapter limits as baseline
    WGPURequiredLimits requiredLimits = {};
    requiredLimits.limits = adapterLimits.limits;

    uint32_t neededBytesPerSample = 64;
    if (adapterLimits.limits.maxColorAttachmentBytesPerSample >= neededBytesPerSample) {
        requiredLimits.limits.maxColorAttachmentBytesPerSample = neededBytesPerSample;
        std::cout << "[WebGPU] Requesting maxColorAttachmentBytesPerSample: " << neededBytesPerSample << std::endl;
    }

    deviceDesc.requiredLimits = &requiredLimits;
#endif

    // Request every adapter feature (see enumerateAdapterFeatures)
    std::vector<WGPUFeatureName> requiredFeatures = enumerateAdapterFeatures(adapter_);
    hasIndirectFirstInstance_ =
        wgpuAdapterHasFeature(adapter_, WGPUFeatureName_IndirectFirstInstance);
    std::cout << "[WebGPU] IndirectFirstInstance "
              << (hasIndirectFirstInstance_ ? "supported" : "NOT supported") << std::endl;
    deviceDesc.requiredFeatureCount = requiredFeatures.size();
    deviceDesc.requiredFeatures = requiredFeatures.empty() ? nullptr : requiredFeatures.data();

    // Set up error callback
    WGPUUncapturedErrorCallbackInfo errorCallbackInfo = {};
    errorCallbackInfo.callback = onDeviceError;
    deviceDesc.uncapturedErrorCallbackInfo = errorCallbackInfo;

    DeviceRequestData deviceData;

#if WGPU_USES_CALLBACK_INFO_PATTERN
    // Dawn uses CallbackInfo struct with required callback mode
    WGPURequestDeviceCallbackInfo deviceCallbackInfo = {};
    deviceCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    deviceCallbackInfo.callback = onDeviceRequestEnded;
    deviceCallbackInfo.userdata1 = &deviceData;
    deviceCallbackInfo.userdata2 = nullptr;
    wgpuAdapterRequestDevice(adapter_, &deviceDesc, deviceCallbackInfo);
#else
    // wgpu-native uses separate callback and userdata
    wgpuAdapterRequestDevice(adapter_, &deviceDesc, onDeviceRequestEnded, &deviceData);
#endif

#if defined(MYSTRAL_WEBGPU_WGPU)
    while (!deviceData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#elif defined(MYSTRAL_WEBGPU_DAWN)
    while (!deviceData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#endif

    if (!deviceData.device) {
        std::cerr << "[WebGPU] Failed to get device" << std::endl;
        return false;
    }
    device_ = deviceData.device;

    // Get queue
    queue_ = wgpuDeviceGetQueue(device_);
    if (!queue_) {
        std::cerr << "[WebGPU] Failed to get queue" << std::endl;
        return false;
    }
    std::cout << "[WebGPU] Queue acquired" << std::endl;

    return true;
}

bool Context::createSurfaceWithDisplay(void* display, void* window, int platformType) {
    if (!instance_) {
        std::cerr << "[WebGPU] Cannot create surface: no instance" << std::endl;
        return false;
    }

    std::cout << "[WebGPU] Creating surface for platform type " << platformType << " with display pointer" << std::endl;

    WGPUSurfaceDescriptor surfaceDesc = {};

#if defined(__linux__) && !defined(__ANDROID__)
    WGPUSurfaceDescriptorFromXlibWindow_Compat xlibDesc = {};

    if (platformType == PLATFORM_XLIB) {
        xlibDesc.chain.sType = WGPUSType_SurfaceDescriptorFromXlibWindow_Compat;
        xlibDesc.display = display;  // Pass the actual X11 Display pointer
        xlibDesc.window = reinterpret_cast<uint64_t>(window);
        surfaceDesc.nextInChain = (WGPUChainedStruct*)&xlibDesc;
        std::cout << "[WebGPU] Creating X11 surface with display: " << display << " window: " << window << std::endl;
    } else {
        std::cerr << "[WebGPU] createSurfaceWithDisplay only supports PLATFORM_XLIB on Linux" << std::endl;
        return false;
    }
#else
    (void)display;
    (void)window;
    (void)platformType;
    std::cerr << "[WebGPU] createSurfaceWithDisplay is only available on Linux" << std::endl;
    return false;
#endif

    surface_ = wgpuInstanceCreateSurface(instance_, &surfaceDesc);
    if (!surface_) {
        std::cerr << "[WebGPU] Failed to create surface" << std::endl;
        return false;
    }
    std::cout << "[WebGPU] Surface created" << std::endl;

    // Now request adapter with surface compatibility
    WGPURequestAdapterOptions adapterOptions = {};
    adapterOptions.compatibleSurface = surface_;
    adapterOptions.powerPreference = WGPUPowerPreference_HighPerformance;

    AdapterRequestData adapterData;

#if WGPU_USES_CALLBACK_INFO_PATTERN
    WGPURequestAdapterCallbackInfo callbackInfo = {};
    callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    callbackInfo.callback = onAdapterRequestEnded;
    callbackInfo.userdata1 = &adapterData;
    callbackInfo.userdata2 = nullptr;
    wgpuInstanceRequestAdapter(instance_, &adapterOptions, callbackInfo);
#else
    wgpuInstanceRequestAdapter(instance_, &adapterOptions, onAdapterRequestEnded, &adapterData);
#endif

#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
    while (!adapterData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#endif

    if (!adapterData.adapter) {
        std::cerr << "[WebGPU] Failed to get adapter" << std::endl;
        return false;
    }
    adapter_ = adapterData.adapter;

    // Print adapter info
    WGPUAdapterInfo adapterInfo = {};
    wgpuAdapterGetInfo(adapter_, &adapterInfo);
    std::cout << "[WebGPU] Adapter: " << WGPU_PRINT_STRING_VIEW(adapterInfo.device) << std::endl;
    std::cout << "[WebGPU] Vendor: " << WGPU_PRINT_STRING_VIEW(adapterInfo.vendor) << std::endl;
    std::cout << "[WebGPU] Backend: ";
    switch (adapterInfo.backendType) {
        case WGPUBackendType_Null: std::cout << "Null"; break;
        case WGPUBackendType_WebGPU: std::cout << "WebGPU"; break;
        case WGPUBackendType_D3D11: std::cout << "D3D11"; break;
        case WGPUBackendType_D3D12: std::cout << "D3D12"; break;
        case WGPUBackendType_Metal: std::cout << "Metal"; break;
        case WGPUBackendType_Vulkan: std::cout << "Vulkan"; break;
        case WGPUBackendType_OpenGL: std::cout << "OpenGL"; break;
        case WGPUBackendType_OpenGLES: std::cout << "OpenGLES"; break;
        default: std::cout << "Unknown"; break;
    }
    std::cout << std::endl;
    wgpuAdapterInfoFreeMembers(adapterInfo);

    // Request device with required limits - same as createSurface
    WGPUDeviceDescriptor deviceDesc = {};
    WGPU_SET_LABEL(deviceDesc, "Mystral Device");
#if defined(MYSTRAL_WEBGPU_DAWN)
    WGPUDawnCacheDeviceDescriptor cacheDesc;
    attachDawnCache(deviceDesc, cacheDesc);
#endif

#if defined(MYSTRAL_WEBGPU_DAWN)
    WGPULimits adapterLimits = {};
    wgpuAdapterGetLimits(adapter_, &adapterLimits);
    WGPULimits requiredLimits = adapterLimits;
    uint32_t neededBytesPerSample = 64;
    if (adapterLimits.maxColorAttachmentBytesPerSample >= neededBytesPerSample) {
        requiredLimits.maxColorAttachmentBytesPerSample = neededBytesPerSample;
    }
    deviceDesc.requiredLimits = &requiredLimits;
#elif defined(MYSTRAL_WEBGPU_WGPU)
    WGPUSupportedLimits adapterLimits = {};
    wgpuAdapterGetLimits(adapter_, &adapterLimits);
    WGPURequiredLimits requiredLimits = {};
    requiredLimits.limits = adapterLimits.limits;
    uint32_t neededBytesPerSample = 64;
    if (adapterLimits.limits.maxColorAttachmentBytesPerSample >= neededBytesPerSample) {
        requiredLimits.limits.maxColorAttachmentBytesPerSample = neededBytesPerSample;
    }
    deviceDesc.requiredLimits = &requiredLimits;
#endif

    // Request every adapter feature (see enumerateAdapterFeatures)
    std::vector<WGPUFeatureName> requiredFeatures = enumerateAdapterFeatures(adapter_);
    hasIndirectFirstInstance_ =
        wgpuAdapterHasFeature(adapter_, WGPUFeatureName_IndirectFirstInstance);
    deviceDesc.requiredFeatureCount = requiredFeatures.size();
    deviceDesc.requiredFeatures = requiredFeatures.empty() ? nullptr : requiredFeatures.data();

    WGPUUncapturedErrorCallbackInfo errorCallbackInfo = {};
    errorCallbackInfo.callback = onDeviceError;
    deviceDesc.uncapturedErrorCallbackInfo = errorCallbackInfo;

    DeviceRequestData deviceData;

#if WGPU_USES_CALLBACK_INFO_PATTERN
    WGPURequestDeviceCallbackInfo deviceCallbackInfo = {};
    deviceCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    deviceCallbackInfo.callback = onDeviceRequestEnded;
    deviceCallbackInfo.userdata1 = &deviceData;
    deviceCallbackInfo.userdata2 = nullptr;
    wgpuAdapterRequestDevice(adapter_, &deviceDesc, deviceCallbackInfo);
#else
    wgpuAdapterRequestDevice(adapter_, &deviceDesc, onDeviceRequestEnded, &deviceData);
#endif

#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
    while (!deviceData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#endif

    if (!deviceData.device) {
        std::cerr << "[WebGPU] Failed to get device" << std::endl;
        return false;
    }
    device_ = deviceData.device;

    queue_ = wgpuDeviceGetQueue(device_);
    if (!queue_) {
        std::cerr << "[WebGPU] Failed to get queue" << std::endl;
        return false;
    }
    std::cout << "[WebGPU] Queue acquired" << std::endl;

    return true;
}

bool Context::configureSurface(uint32_t width, uint32_t height) {
    std::cout << "[WebGPU] configureSurface called: " << width << "x" << height << std::endl;
    std::cout << "[WebGPU] surface_=" << (void*)surface_ << ", device_=" << (void*)device_ << ", adapter_=" << (void*)adapter_ << std::endl;

    if (!surface_ || !device_) {
        std::cerr << "[WebGPU] Cannot configure surface: missing surface or device" << std::endl;
        return false;
    }

    if (!adapter_) {
        std::cerr << "[WebGPU] Cannot configure surface: missing adapter" << std::endl;
        return false;
    }

    surfaceWidth_ = width;
    surfaceHeight_ = height;

    // Get surface capabilities
    std::cout << "[WebGPU] Getting surface capabilities..." << std::endl;
    WGPUSurfaceCapabilities capabilities = {};
    wgpuSurfaceGetCapabilities(surface_, adapter_, &capabilities);
    std::cout << "[WebGPU] Got capabilities: formatCount=" << capabilities.formatCount << std::endl;

    if (capabilities.formatCount == 0) {
        std::cerr << "[WebGPU] No surface formats available" << std::endl;
        return false;
    }

    // List all available formats
    std::cout << "[WebGPU] Available surface formats:" << std::endl;
    for (uint32_t i = 0; i < capabilities.formatCount; i++) {
        std::cout << "  [" << i << "] = " << capabilities.formats[i] << std::endl;
    }

    // Prefer BGRA8Unorm (non-sRGB) to match browser behavior
    // Fall back to first format if not available
    preferredFormat_ = capabilities.formats[0];
    for (uint32_t i = 0; i < capabilities.formatCount; i++) {
        if (capabilities.formats[i] == WGPUTextureFormat_BGRA8Unorm) {
            preferredFormat_ = WGPUTextureFormat_BGRA8Unorm;
            break;
        }
    }
    std::cout << "[WebGPU] Using surface format: " << preferredFormat_ << std::endl;

    // Configure surface
    WGPUSurfaceConfiguration config = {};
    config.device = device_;
    config.format = (WGPUTextureFormat)preferredFormat_;
    config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    config.alphaMode = WGPUCompositeAlphaMode_Auto;
    config.width = width;
    config.height = height;
    config.presentMode = WGPUPresentMode_Fifo;  // VSync

    wgpuSurfaceConfigure(surface_, &config);
    std::cout << "[WebGPU] Surface configured: " << width << "x" << height << std::endl;

    wgpuSurfaceCapabilitiesFreeMembers(capabilities);
    return true;
}

void Context::resizeSurface(uint32_t width, uint32_t height) {
    if (width != surfaceWidth_ || height != surfaceHeight_) {
        configureSurface(width, height);
    }
}

void* Context::getCurrentTextureView() {
    if (!surface_) {
        return nullptr;
    }

    WGPUSurfaceTexture surfaceTexture;
    wgpuSurfaceGetCurrentTexture(surface_, &surfaceTexture);

    if (!wgpuSurfaceTextureStatusIsSuccess(surfaceTexture.status)) {
        std::cerr << "[WebGPU] Failed to get current texture, status: " << surfaceTexture.status << std::endl;
        return nullptr;
    }

    WGPUTextureViewDescriptor viewDesc = {};
    viewDesc.format = (WGPUTextureFormat)preferredFormat_;
    viewDesc.dimension = WGPUTextureViewDimension_2D;
    viewDesc.baseMipLevel = 0;
    viewDesc.mipLevelCount = 1;
    viewDesc.baseArrayLayer = 0;
    viewDesc.arrayLayerCount = 1;
    viewDesc.aspect = WGPUTextureAspect_All;

    return wgpuTextureCreateView(surfaceTexture.texture, &viewDesc);
}

void Context::present() {
    if (surface_) {
        wgpuSurfacePresent(surface_);
    }
}

}  // namespace webgpu
}  // namespace mystral
