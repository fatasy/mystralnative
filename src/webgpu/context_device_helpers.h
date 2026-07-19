#pragma once

#include <cstddef>
#include <vector>

#include "webgpu/webgpu.h"
#include "mystral/webgpu_compat.h"
#include "dawn/native/DawnNative.h"

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

void attachDawnCache(
    WGPUDeviceDescriptor& deviceDesc,
    WGPUDawnCacheDeviceDescriptor& cacheDesc);
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

std::vector<WGPUFeatureName> enumerateAdapterFeatures(WGPUAdapter adapter);

}  // namespace detail
}  // namespace webgpu
}  // namespace mystral
