#pragma once

#include <cstddef>
#include <vector>

#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
#include "webgpu/webgpu.h"
#include "mystral/webgpu_compat.h"
#endif

#if defined(MYSTRAL_WEBGPU_DAWN)
#include "dawn/native/DawnNative.h"
#endif

namespace mystral {
namespace webgpu {
namespace detail {

struct AdapterRequestData {
    WGPUAdapter adapter = nullptr;
    bool completed = false;
};

struct DeviceRequestData {
    WGPUDevice device = nullptr;
    bool completed = false;
};

#if defined(MYSTRAL_WEBGPU_DAWN)
void attachDawnCache(
    WGPUDeviceDescriptor& deviceDesc,
    WGPUDawnCacheDeviceDescriptor& cacheDesc);
#endif

#if WGPU_USES_CALLBACK_INFO_PATTERN
void onAdapterRequestEnded(
    WGPURequestAdapterStatus status,
    WGPUAdapter adapter,
    WGPUStringView message,
    void* userdata1,
    void* userdata2);
void onDeviceRequestEnded(
    WGPURequestDeviceStatus status,
    WGPUDevice device,
    WGPUStringView message,
    void* userdata1,
    void* userdata2);
void onDeviceError(
    WGPUDevice const* device,
    WGPUErrorType type,
    WGPUStringView message,
    void* userdata1,
    void* userdata2);
#else
void onAdapterRequestEnded(
    WGPURequestAdapterStatus status,
    WGPUAdapter adapter,
    char const* message,
    void* userdata);
void onDeviceRequestEnded(
    WGPURequestDeviceStatus status,
    WGPUDevice device,
    char const* message,
    void* userdata);
void onDeviceError(
    WGPUErrorType type,
    char const* message,
    void* userdata);
#endif

std::vector<WGPUFeatureName> enumerateAdapterFeatures(WGPUAdapter adapter);

}  // namespace detail
}  // namespace webgpu
}  // namespace mystral
