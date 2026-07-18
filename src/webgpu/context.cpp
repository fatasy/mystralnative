/**
 * WebGPU Context Implementation
 *
 * Handles WebGPU initialization using wgpu-native (or Dawn).
 * Both backends implement the same webgpu.h C API.
 */

#include "mystral/webgpu/context.h"
#include <iostream>
#include <vector>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <cstdlib>



#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
#include "webgpu/webgpu.h"
#include "mystral/webgpu_compat.h"
#endif

// Dawn-specific native API used for adapter/device discovery.
#if defined(MYSTRAL_WEBGPU_DAWN)
#include "dawn/native/DawnNative.h"
#if defined(MYSTRAL_DAWN_USE_PROC_TABLE)
#include "dawn/dawn_proc.h"
#endif
#endif

#include "context_device_helpers.h"

// wgpu-native specific declarations (avoiding wgpu.h include path issues)
#if defined(MYSTRAL_WEBGPU_WGPU)
extern "C" {
// Log level enum
typedef enum WGPULogLevel {
    WGPULogLevel_Off = 0x00000000,
    WGPULogLevel_Error = 0x00000001,
    WGPULogLevel_Warn = 0x00000002,
    WGPULogLevel_Info = 0x00000003,
    WGPULogLevel_Debug = 0x00000004,
    WGPULogLevel_Trace = 0x00000005,
} WGPULogLevel;

// Instance backend flags
typedef enum WGPUInstanceBackend {
    WGPUInstanceBackend_All = 0x00000000,
    WGPUInstanceBackend_Vulkan = 1 << 0,
    WGPUInstanceBackend_GL = 1 << 1,
    WGPUInstanceBackend_Metal = 1 << 2,
    WGPUInstanceBackend_DX12 = 1 << 3,
    WGPUInstanceBackend_DX11 = 1 << 4,
    WGPUInstanceBackend_BrowserWebGPU = 1 << 5,
} WGPUInstanceBackend;

typedef enum WGPUInstanceFlag {
    WGPUInstanceFlag_Default = 0x00000000,
    WGPUInstanceFlag_Debug = 1 << 0,
    WGPUInstanceFlag_Validation = 1 << 1,
} WGPUInstanceFlag;

// Native sType for instance extras
#define WGPUSType_InstanceExtras 0x00030006

typedef struct WGPUInstanceExtras {
    WGPUChainedStruct chain;
    WGPUFlags backends;
    WGPUFlags flags;
    uint32_t dx12ShaderCompiler;
    uint32_t gles3MinorVersion;
    const char* dxilPath;
    const char* dxcPath;
} WGPUInstanceExtras;

typedef void (*WGPULogCallback)(WGPULogLevel level, char const* message, void* userdata);

// Wrapped submission index for device poll
typedef struct WGPUWrappedSubmissionIndex {
    WGPUQueue queue;
    uint64_t submissionIndex;
} WGPUWrappedSubmissionIndex;

void wgpuSetLogCallback(WGPULogCallback callback, void* userdata);
void wgpuSetLogLevel(WGPULogLevel level);

// Device poll - blocks until all GPU work is done
WGPUBool wgpuDevicePoll(WGPUDevice device, WGPUBool wait, WGPUWrappedSubmissionIndex const* wrappedSubmissionIndex);
}
#endif

namespace mystral {
namespace webgpu {

using namespace detail;

#if defined(MYSTRAL_WEBGPU_WGPU)
static void onWgpuLog(WGPULogLevel level, char const* message, void* userdata) {
    const char* levelStr = "???";
    switch (level) {
        case WGPULogLevel_Error: levelStr = "ERROR"; break;
        case WGPULogLevel_Warn: levelStr = "WARN"; break;
        case WGPULogLevel_Info: levelStr = "INFO"; break;
        case WGPULogLevel_Debug: levelStr = "DEBUG"; break;
        case WGPULogLevel_Trace: levelStr = "TRACE"; break;
        default: break;
    }
    std::cout << "[wgpu " << levelStr << "] " << (message ? message : "") << std::endl;
}
#endif

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

#if defined(MYSTRAL_WEBGPU_WGPU)
    // Set up wgpu-native logging
    // Use Error level to suppress noisy warnings like "Depth slice on color attachments is not implemented"
    wgpuSetLogCallback(onWgpuLog, nullptr);
    wgpuSetLogLevel(WGPULogLevel_Error);

    // Create instance with Metal backend on macOS
    WGPUInstanceExtras instanceExtras = {};
    instanceExtras.chain.sType = (WGPUSType)WGPUSType_InstanceExtras;
#if defined(__APPLE__)
    instanceExtras.backends = WGPUInstanceBackend_Metal;
#elif defined(_WIN32)
    instanceExtras.backends = WGPUInstanceBackend_DX12 | WGPUInstanceBackend_Vulkan;
#else
    instanceExtras.backends = WGPUInstanceBackend_Vulkan;
#endif
    instanceExtras.flags = WGPUInstanceFlag_Validation;

    WGPUInstanceDescriptor instanceDesc = {};
    instanceDesc.nextInChain = (WGPUChainedStruct*)&instanceExtras;
#else
    WGPUInstanceDescriptor instanceDesc = {};
#endif

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

#if defined(MYSTRAL_WEBGPU_WGPU)
    while (!adapterData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#elif defined(MYSTRAL_WEBGPU_DAWN)
    while (!adapterData.completed) {
        wgpuInstanceProcessEvents(instance_);
    }
#endif

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
#if defined(MYSTRAL_WEBGPU_DAWN)
    WGPUDawnCacheDeviceDescriptor cacheDesc;
    attachDawnCache(deviceDesc, cacheDesc);
#endif

#if defined(MYSTRAL_WEBGPU_DAWN)
    WGPULimits adapterLimits = {};
    wgpuAdapterGetLimits(adapter_, &adapterLimits);
    WGPULimits requiredLimits = adapterLimits;
    deviceDesc.requiredLimits = &requiredLimits;
#elif defined(MYSTRAL_WEBGPU_WGPU)
    WGPUSupportedLimits adapterLimits = {};
    wgpuAdapterGetLimits(adapter_, &adapterLimits);
    WGPURequiredLimits requiredLimits = {};
    requiredLimits.limits = adapterLimits.limits;
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
