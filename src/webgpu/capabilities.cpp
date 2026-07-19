#include "webgpu/capabilities.h"

#include <cstdint>
#include <string>

namespace mystral::webgpu::bridge {

namespace {

struct FeatureNameEntry {
    const char* name;
    WGPUFeatureName feature;
};

constexpr FeatureNameEntry kFeatureNames[] = {
    {"depth-clip-control", WGPUFeatureName_DepthClipControl},
    {"depth32float-stencil8", WGPUFeatureName_Depth32FloatStencil8},
    {"timestamp-query", WGPUFeatureName_TimestampQuery},
    {"texture-compression-bc", WGPUFeatureName_TextureCompressionBC},
    {"texture-compression-etc2", WGPUFeatureName_TextureCompressionETC2},
    {"texture-compression-astc", WGPUFeatureName_TextureCompressionASTC},
    {"indirect-first-instance", WGPUFeatureName_IndirectFirstInstance},
    {"shader-f16", WGPUFeatureName_ShaderF16},
    {"rg11b10ufloat-renderable", WGPUFeatureName_RG11B10UfloatRenderable},
    {"bgra8unorm-storage", WGPUFeatureName_BGRA8UnormStorage},
    {"float32-filterable", WGPUFeatureName_Float32Filterable},
#if defined(MYSTRAL_WEBGPU_DAWN)
    {"float32-blendable", WGPUFeatureName_Float32Blendable},
    {"clip-distances", WGPUFeatureName_ClipDistances},
    {"dual-source-blending", WGPUFeatureName_DualSourceBlending},
    {"subgroups", WGPUFeatureName_Subgroups},
    {"texture-formats-tier1", WGPUFeatureName_TextureFormatsTier1},
#endif
};

} // namespace

void Capabilities::configure(js::Engine* engine, WGPUAdapter adapter, WGPUDevice device) {
    engine_ = engine;
    adapter_ = adapter;
    device_ = device;
}

void Capabilities::applyLimits(js::JSValueHandle target, const WGPULimits& limits) const {
    auto set = [&](const char* name, double value) {
        engine_->setProperty(target, name, engine_->newNumber(value));
    };
    set("maxTextureDimension1D", limits.maxTextureDimension1D);
    set("maxTextureDimension2D", limits.maxTextureDimension2D);
    set("maxTextureDimension3D", limits.maxTextureDimension3D);
    set("maxTextureArrayLayers", limits.maxTextureArrayLayers);
    set("maxBindGroups", limits.maxBindGroups);
    set("maxBindGroupsPlusVertexBuffers", limits.maxBindGroupsPlusVertexBuffers);
    set("maxBindingsPerBindGroup", limits.maxBindingsPerBindGroup);
    set("maxDynamicUniformBuffersPerPipelineLayout", limits.maxDynamicUniformBuffersPerPipelineLayout);
    set("maxDynamicStorageBuffersPerPipelineLayout", limits.maxDynamicStorageBuffersPerPipelineLayout);
    set("maxSampledTexturesPerShaderStage", limits.maxSampledTexturesPerShaderStage);
    set("maxSamplersPerShaderStage", limits.maxSamplersPerShaderStage);
    set("maxStorageBuffersPerShaderStage", limits.maxStorageBuffersPerShaderStage);
    set("maxStorageTexturesPerShaderStage", limits.maxStorageTexturesPerShaderStage);
    set("maxUniformBuffersPerShaderStage", limits.maxUniformBuffersPerShaderStage);
    set("maxUniformBufferBindingSize", static_cast<double>(limits.maxUniformBufferBindingSize));
    set("maxStorageBufferBindingSize", static_cast<double>(limits.maxStorageBufferBindingSize));
    set("minUniformBufferOffsetAlignment", limits.minUniformBufferOffsetAlignment);
    set("minStorageBufferOffsetAlignment", limits.minStorageBufferOffsetAlignment);
    set("maxVertexBuffers", limits.maxVertexBuffers);
    set("maxBufferSize", static_cast<double>(limits.maxBufferSize));
    set("maxVertexAttributes", limits.maxVertexAttributes);
    set("maxVertexBufferArrayStride", limits.maxVertexBufferArrayStride);
    set("maxColorAttachments", limits.maxColorAttachments);
    set("maxColorAttachmentBytesPerSample", limits.maxColorAttachmentBytesPerSample);
    set("maxComputeWorkgroupStorageSize", limits.maxComputeWorkgroupStorageSize);
    set("maxComputeInvocationsPerWorkgroup", limits.maxComputeInvocationsPerWorkgroup);
    set("maxComputeWorkgroupSizeX", limits.maxComputeWorkgroupSizeX);
    set("maxComputeWorkgroupSizeY", limits.maxComputeWorkgroupSizeY);
    set("maxComputeWorkgroupSizeZ", limits.maxComputeWorkgroupSizeZ);
    set("maxComputeWorkgroupsPerDimension", limits.maxComputeWorkgroupsPerDimension);
}

bool Capabilities::queryDeviceLimits(WGPULimits* out) const {
    if (!device_) return false;
#if defined(MYSTRAL_WEBGPU_DAWN)
    WGPULimits limits = {};
    wgpuDeviceGetLimits(device_, &limits);
    *out = limits;
#else
    WGPUSupportedLimits supported = {};
    wgpuDeviceGetLimits(device_, &supported);
    *out = supported.limits;
#endif
    return true;
}

bool Capabilities::queryAdapterLimits(WGPULimits* out) const {
    if (!adapter_) return queryDeviceLimits(out);
#if defined(MYSTRAL_WEBGPU_DAWN)
    WGPULimits limits = {};
    wgpuAdapterGetLimits(adapter_, &limits);
    *out = limits;
#else
    WGPUSupportedLimits supported = {};
    wgpuAdapterGetLimits(adapter_, &supported);
    *out = supported.limits;
#endif
    return true;
}

bool Capabilities::applyDeviceLimits(js::JSValueHandle target) const {
    WGPULimits limits = {};
    if (!queryDeviceLimits(&limits)) return false;
    applyLimits(target, limits);
    return true;
}

bool Capabilities::applyAdapterLimits(js::JSValueHandle target) const {
    WGPULimits limits = {};
    if (!queryAdapterLimits(&limits)) return false;
    applyLimits(target, limits);
    return true;
}

js::JSValueHandle Capabilities::buildFeatures(bool adapter) const {
    auto features = engine_->newArray(0);
    uint32_t count = 0;
    for (const auto& entry : kFeatureNames) {
        const bool enabled = adapter && adapter_
            ? wgpuAdapterHasFeature(adapter_, entry.feature)
            : device_ && wgpuDeviceHasFeature(device_, entry.feature);
        if (enabled) {
            engine_->setPropertyIndex(features, count++, engine_->newString(entry.name));
        }
    }
    engine_->setProperty(features, "size", engine_->newNumber(count));
    engine_->setProperty(features, "has",
        engine_->newFunction("has", [this, adapter](void*, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) return engine_->newBoolean(false);
            const std::string name = engine_->toString(args[0]);
            for (const auto& entry : kFeatureNames) {
                if (name != entry.name) continue;
                const bool enabled = adapter && adapter_
                    ? wgpuAdapterHasFeature(adapter_, entry.feature)
                    : device_ && wgpuDeviceHasFeature(device_, entry.feature);
                return engine_->newBoolean(enabled);
            }
            return engine_->newBoolean(false);
        }));
    return features;
}

js::JSValueHandle Capabilities::deviceFeatures() const {
    return buildFeatures(false);
}

js::JSValueHandle Capabilities::adapterFeatures() const {
    return buildFeatures(true);
}

} // namespace mystral::webgpu::bridge
