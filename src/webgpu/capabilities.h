#pragma once

#include "mystral/js/engine.h"

#include <webgpu/webgpu.h>

namespace mystral::webgpu::bridge {

class Capabilities {
public:
    void configure(js::Engine* engine, WGPUAdapter adapter, WGPUDevice device);

    bool applyDeviceLimits(js::JSValueHandle target) const;
    bool applyAdapterLimits(js::JSValueHandle target) const;
    js::JSValueHandle deviceFeatures() const;
    js::JSValueHandle adapterFeatures() const;

private:
    bool queryDeviceLimits(WGPULimits* out) const;
    bool queryAdapterLimits(WGPULimits* out) const;
    void applyLimits(js::JSValueHandle target, const WGPULimits& limits) const;
    js::JSValueHandle buildFeatures(bool adapter) const;

    js::Engine* engine_ = nullptr;
    WGPUAdapter adapter_ = nullptr;
    WGPUDevice device_ = nullptr;
};

} // namespace mystral::webgpu::bridge
