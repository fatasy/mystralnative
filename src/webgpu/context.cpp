/**
 * WebGPU Context Implementation
 *
 * Handles WebGPU initialization using Dawn.
 */

#include "mystral/webgpu/context.h"
#include <iostream>
#include <vector>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <cstdlib>



#include "webgpu/webgpu.h"
#include "mystral/webgpu_compat.h"

// Dawn-specific native API used for adapter/device discovery.
#if defined(MYSTRAL_WEBGPU_DAWN)
#include "dawn/native/DawnNative.h"
#if defined(MYSTRAL_DAWN_USE_PROC_TABLE)
#include "dawn/dawn_proc.h"
#endif
#endif

#include "context_device_helpers.h"

namespace mystral {
namespace webgpu {

using namespace detail;

Context::Context() = default;

Context::~Context() {
    // Clean up offscreen resources
    if (offscreenTextureView_) {
        wgpuTextureViewRelease((WGPUTextureView)offscreenTextureView_);
        offscreenTextureView_ = nullptr;
    }
    if (offscreenTexture_) {
        wgpuTextureRelease((WGPUTexture)offscreenTexture_);
        offscreenTexture_ = nullptr;
    }
    if (device_) {
        wgpuDeviceRelease(device_);
        device_ = nullptr;
    }
    if (adapter_) {
        wgpuAdapterRelease(adapter_);
        adapter_ = nullptr;
    }
    if (surface_) {
        wgpuSurfaceRelease(surface_);
        surface_ = nullptr;
    }
    if (instance_) {
        wgpuInstanceRelease(instance_);
        instance_ = nullptr;
    }
    std::cout << "[WebGPU] Context destroyed" << std::endl;
}

bool Context::initialize() {
    std::cout << "[WebGPU] Initializing..." << std::endl;

#if defined(MYSTRAL_DAWN_USE_PROC_TABLE)
    dawnProcSetProcs(&dawn::native::GetProcs());
    std::cout << "[WebGPU] Dawn proc table initialized" << std::endl;
#endif

    WGPUInstanceDescriptor instanceDesc = {};

    instance_ = wgpuCreateInstance(&instanceDesc);
    if (!instance_) {
        std::cerr << "[WebGPU] Failed to create instance" << std::endl;
        return false;
    }
    std::cout << "[WebGPU] Instance created" << std::endl;

    initialized_ = true;
    return true;
}

bool Context::initializeHeadless() {
    std::cout << "[WebGPU] Initializing headless mode (no SDL)..." << std::endl;

    // First initialize the instance
    if (!initialize()) {
        return false;
    }

    headless_ = true;

    // Request adapter WITHOUT a compatible surface
    WGPURequestAdapterOptions adapterOptions = {};
    adapterOptions.compatibleSurface = nullptr;  // No surface required
    adapterOptions.powerPreference = WGPUPowerPreference_HighPerformance;

    AdapterRequestData adapterData;

    WGPURequestAdapterCallbackInfo callbackInfo = {};
    callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    callbackInfo.callback = onAdapterRequestEnded;
    callbackInfo.userdata1 = &adapterData;
    callbackInfo.userdata2 = nullptr;
    wgpuInstanceRequestAdapter(instance_, &adapterOptions, callbackInfo);

    while (!adapterData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }

    if (!adapterData.adapter) {
        std::cerr << "[WebGPU] Failed to get adapter in headless mode" << std::endl;
        return false;
    }
    adapter_ = adapterData.adapter;

    // Print adapter info
    WGPUAdapterInfo adapterInfo = {};
    wgpuAdapterGetInfo(adapter_, &adapterInfo);
    std::cout << "[WebGPU] Headless adapter: " << WGPU_PRINT_STRING_VIEW(adapterInfo.device) << std::endl;
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

    // Request device
    WGPUDeviceDescriptor deviceDesc = {};
    WGPU_SET_LABEL(deviceDesc, "Mystral Headless Device");
    WGPUDawnCacheDeviceDescriptor cacheDesc;
    attachDawnCache(deviceDesc, cacheDesc);

    WGPULimits adapterLimits = {};
    wgpuAdapterGetLimits(adapter_, &adapterLimits);
    WGPULimits requiredLimits = adapterLimits;
    deviceDesc.requiredLimits = &requiredLimits;

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

    WGPURequestDeviceCallbackInfo deviceCallbackInfo = {};
    deviceCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    deviceCallbackInfo.callback = onDeviceRequestEnded;
    deviceCallbackInfo.userdata1 = &deviceData;
    deviceCallbackInfo.userdata2 = nullptr;
    wgpuAdapterRequestDevice(adapter_, &deviceDesc, deviceCallbackInfo);

    while (!deviceData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }

    if (!deviceData.device) {
        std::cerr << "[WebGPU] Failed to get device in headless mode" << std::endl;
        return false;
    }
    device_ = deviceData.device;

    queue_ = wgpuDeviceGetQueue(device_);
    if (!queue_) {
        std::cerr << "[WebGPU] Failed to get queue in headless mode" << std::endl;
        return false;
    }

    std::cout << "[WebGPU] Headless mode initialized successfully" << std::endl;
    return true;
}

bool Context::createOffscreenTarget(uint32_t width, uint32_t height) {
    if (!device_) {
        std::cerr << "[WebGPU] Cannot create offscreen target: no device" << std::endl;
        return false;
    }

    std::cout << "[WebGPU] Creating offscreen render target: " << width << "x" << height << std::endl;

    surfaceWidth_ = width;
    surfaceHeight_ = height;

    // Use BGRA8Unorm format (same as surface format for compatibility)
    preferredFormat_ = WGPUTextureFormat_BGRA8Unorm;

    // Create offscreen texture
    WGPUTextureDescriptor textureDesc = {};
    WGPU_SET_LABEL(textureDesc, "Offscreen Render Target");
    textureDesc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    textureDesc.dimension = WGPUTextureDimension_2D;
    textureDesc.size = {width, height, 1};
    textureDesc.format = (WGPUTextureFormat)preferredFormat_;
    textureDesc.mipLevelCount = 1;
    textureDesc.sampleCount = 1;

    WGPUTexture texture = wgpuDeviceCreateTexture(device_, &textureDesc);
    if (!texture) {
        std::cerr << "[WebGPU] Failed to create offscreen texture" << std::endl;
        return false;
    }
    offscreenTexture_ = texture;

    // Create texture view
    WGPUTextureViewDescriptor viewDesc = {};
    viewDesc.format = (WGPUTextureFormat)preferredFormat_;
    viewDesc.dimension = WGPUTextureViewDimension_2D;
    viewDesc.baseMipLevel = 0;
    viewDesc.mipLevelCount = 1;
    viewDesc.baseArrayLayer = 0;
    viewDesc.arrayLayerCount = 1;
    viewDesc.aspect = WGPUTextureAspect_All;

    WGPUTextureView view = wgpuTextureCreateView(texture, &viewDesc);
    if (!view) {
        std::cerr << "[WebGPU] Failed to create offscreen texture view" << std::endl;
        return false;
    }
    offscreenTextureView_ = view;

    std::cout << "[WebGPU] Offscreen render target created" << std::endl;
    return true;
}


}  // namespace webgpu
}  // namespace mystral
