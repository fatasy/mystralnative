/**
 * Shared WebGPU adapter and device request helpers.
 */

#include "context_device_helpers.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>

namespace mystral {
namespace webgpu {
namespace detail {

#if defined(MYSTRAL_WEBGPU_DAWN)

namespace {

struct DawnDiskCache {
    std::filesystem::path directory;
    std::mutex mutex;
};

DawnDiskCache& dawnDiskCache() {
    static DawnDiskCache cache;
    static std::once_flag initialized;
    std::call_once(initialized, [] {
#ifdef _WIN32
        const char* base = std::getenv("LOCALAPPDATA");
        cache.directory = base && *base
            ? std::filesystem::path(base) / "Mystral" / "cache" / "dawn"
            : std::filesystem::temp_directory_path() / "Mystral" / "cache" / "dawn";
#else
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        const char* home = std::getenv("HOME");
        if (xdg && *xdg) cache.directory = std::filesystem::path(xdg) / "mystral" / "dawn";
        else if (home && *home) cache.directory = std::filesystem::path(home) / ".cache" / "mystral" / "dawn";
        else cache.directory = std::filesystem::temp_directory_path() / "mystral" / "dawn";
#endif
        std::error_code error;
        std::filesystem::create_directories(cache.directory, error);
        std::cout << "[WebGPU] Dawn pipeline cache: " << cache.directory.string() << std::endl;
    });
    return cache;
}

std::string cacheKeyHex(const void* key, size_t keySize) {
    static constexpr char hex[] = "0123456789abcdef";
    const auto* bytes = static_cast<const uint8_t*>(key);
    std::string value;
    value.resize(keySize * 2);
    for (size_t i = 0; i < keySize; ++i) {
        value[i * 2] = hex[bytes[i] >> 4];
        value[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    return value;
}

std::filesystem::path cachePath(DawnDiskCache& cache, const void* key, size_t keySize) {
    return cache.directory / (cacheKeyHex(key, keySize) + ".bin");
}

size_t loadDawnCacheData(
    const void* key, size_t keySize, void* value, size_t valueSize, void* userdata) {
    auto& cache = *static_cast<DawnDiskCache*>(userdata);
    std::lock_guard<std::mutex> lock(cache.mutex);
    const auto path = cachePath(cache, key, keySize);
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return 0;
    if (!value || valueSize < size) return static_cast<size_t>(size);
    std::ifstream input(path, std::ios::binary);
    if (!input.read(static_cast<char*>(value), static_cast<std::streamsize>(size))) return 0;
    return static_cast<size_t>(size);
}

void storeDawnCacheData(
    const void* key, size_t keySize, const void* value, size_t valueSize, void* userdata) {
    auto& cache = *static_cast<DawnDiskCache*>(userdata);
    std::lock_guard<std::mutex> lock(cache.mutex);
    std::error_code error;
    std::filesystem::create_directories(cache.directory, error);
    const auto path = cachePath(cache, key, keySize);
    if (!value || valueSize == 0) {
        std::filesystem::remove(path, error);
        return;
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output.write(static_cast<const char*>(value), static_cast<std::streamsize>(valueSize))) {
            std::filesystem::remove(temporary, error);
            return;
        }
    }
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) std::filesystem::remove(temporary, error);
}

}  // namespace

void attachDawnCache(WGPUDeviceDescriptor& deviceDesc,
                     WGPUDawnCacheDeviceDescriptor& cacheDesc) {
    cacheDesc = WGPU_DAWN_CACHE_DEVICE_DESCRIPTOR_INIT;
    cacheDesc.isolationKey = WGPU_STRING_VIEW("mystralnative-dawn-v1");
    cacheDesc.loadDataFunction = loadDawnCacheData;
    cacheDesc.storeDataFunction = storeDawnCacheData;
    cacheDesc.functionUserdata = &dawnDiskCache();
    deviceDesc.nextInChain = &cacheDesc.chain;
}

#endif

// Callbacks - different signatures for Dawn vs wgpu-native
#if WGPU_USES_CALLBACK_INFO_PATTERN
// Dawn callback signatures
void onAdapterRequestEnded(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* userdata1, void* userdata2) {
    auto* data = static_cast<AdapterRequestData*>(userdata1);
    if (status == WGPURequestAdapterStatus_Success) {
        data->adapter = adapter;
        std::cout << "[WebGPU] Adapter acquired successfully" << std::endl;
    } else {
        std::cerr << "[WebGPU] Failed to request adapter: " << WGPU_PRINT_STRING_VIEW(message) << std::endl;
    }
    data->completed = true;
}

void onDeviceRequestEnded(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void* userdata2) {
    auto* data = static_cast<DeviceRequestData*>(userdata1);
    if (status == WGPURequestDeviceStatus_Success) {
        data->device = device;
        std::cout << "[WebGPU] Device acquired successfully" << std::endl;
    } else {
        std::cerr << "[WebGPU] Failed to request device: " << WGPU_PRINT_STRING_VIEW(message) << std::endl;
    }
    data->completed = true;
}

void onDeviceError(WGPUDevice const* device, WGPUErrorType type, WGPUStringView message, void* userdata1, void* userdata2) {
    const char* typeStr = "Unknown";
    switch (type) {
        case WGPUErrorType_NoError: typeStr = "NoError"; break;
        case WGPUErrorType_Validation: typeStr = "Validation"; break;
        case WGPUErrorType_OutOfMemory: typeStr = "OutOfMemory"; break;
        case WGPUErrorType_Internal: typeStr = "Internal"; break;
        case WGPUErrorType_Unknown: typeStr = "Unknown"; break;
        // Note: DeviceLost is not a separate error type in Dawn (maps to Unknown)
        default: break;
    }
    std::cerr << "[WebGPU] Device error (" << typeStr << "): " << WGPU_PRINT_STRING_VIEW(message) << std::endl;
}
#else
// wgpu-native callback signatures
void onAdapterRequestEnded(WGPURequestAdapterStatus status, WGPUAdapter adapter, char const* message, void* userdata) {
    auto* data = static_cast<AdapterRequestData*>(userdata);
    if (status == WGPURequestAdapterStatus_Success) {
        data->adapter = adapter;
        std::cout << "[WebGPU] Adapter acquired successfully" << std::endl;
    } else {
        std::cerr << "[WebGPU] Failed to request adapter: " << (message ? message : "unknown error") << std::endl;
    }
    data->completed = true;
}

void onDeviceRequestEnded(WGPURequestDeviceStatus status, WGPUDevice device, char const* message, void* userdata) {
    auto* data = static_cast<DeviceRequestData*>(userdata);
    if (status == WGPURequestDeviceStatus_Success) {
        data->device = device;
        std::cout << "[WebGPU] Device acquired successfully" << std::endl;
    } else {
        std::cerr << "[WebGPU] Failed to request device: " << (message ? message : "unknown error") << std::endl;
    }
    data->completed = true;
}

void onDeviceError(WGPUErrorType type, char const* message, void* userdata) {
    const char* typeStr = "Unknown";
    switch (type) {
        case WGPUErrorType_NoError: typeStr = "NoError"; break;
        case WGPUErrorType_Validation: typeStr = "Validation"; break;
        case WGPUErrorType_OutOfMemory: typeStr = "OutOfMemory"; break;
        case WGPUErrorType_Internal: typeStr = "Internal"; break;
        case WGPUErrorType_Unknown: typeStr = "Unknown"; break;
        case WGPUErrorType_DeviceLost_Compat: typeStr = "DeviceLost"; break;
        default: break;
    }
    std::cerr << "[WebGPU] Device error (" << typeStr << "): " << (message ? message : "no message") << std::endl;
}
#endif

#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
/**
 * Enumerate every feature the adapter exposes so the device can be created
 * with all of them — matching what a browser page can opt into (three.js,
 * for example, requests every available adapter feature when creating its
 * device). Previously only IndirectFirstInstance was requested, so standard
 * capabilities such as float32-filterable or the texture-format tiers were
 * silently missing from the device even when the hardware supports them.
 */
std::vector<WGPUFeatureName> enumerateAdapterFeatures(WGPUAdapter adapter) {
    std::vector<WGPUFeatureName> features;
#if defined(MYSTRAL_WEBGPU_DAWN)
    WGPUSupportedFeatures supported = {};
    wgpuAdapterGetFeatures(adapter, &supported);
    features.assign(supported.features, supported.features + supported.featureCount);
    wgpuSupportedFeaturesFreeMembers(supported);
#else
    size_t count = wgpuAdapterEnumerateFeatures(adapter, nullptr);
    features.resize(count);
    if (count > 0) {
        wgpuAdapterEnumerateFeatures(adapter, features.data());
    }
#endif
    std::cout << "[WebGPU] Requesting " << features.size() << " adapter features" << std::endl;
    return features;
}
#endif

}  // namespace detail
}  // namespace webgpu
}  // namespace mystral
