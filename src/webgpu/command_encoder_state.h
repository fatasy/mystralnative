#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include <webgpu/webgpu.h>

namespace mystral::webgpu::bridge {

struct CommandEncoderStats {
    uint64_t commandEncodersCreated = 0;
    uint64_t renderPassesCreated = 0;
    uint64_t computePassesCreated = 0;
    uint64_t commandBuffersCreated = 0;
};

class CommandEncoderState {
public:
    void trackCommandEncoder(WGPUCommandEncoder encoder);
    bool isCommandEncoderLive(WGPUCommandEncoder encoder) const;
    void finishCommandEncoder(WGPUCommandEncoder encoder);
    void recordCommandBuffer(WGPUCommandBuffer commandBuffer);

    void trackRenderPass(WGPUCommandEncoder encoder, WGPURenderPassEncoder pass);
    void trackComputePass(WGPUCommandEncoder encoder, WGPUComputePassEncoder pass);
    WGPURenderPassEncoder renderPassFor(WGPUCommandEncoder encoder) const;
    WGPUComputePassEncoder computePassFor(WGPUCommandEncoder encoder) const;
    void closeRenderPass(WGPURenderPassEncoder pass);
    void closeComputePass(WGPUComputePassEncoder pass);

    void markSurfaceRenderPass(WGPUCommandEncoder encoder);
    bool surfaceRenderPassEnded() const { return surfaceRenderPassEnded_; }
    void markSurfacePresented();

    const CommandEncoderStats& stats() const { return stats_; }
    size_t liveCommandEncoderCount() const { return liveCommandEncoders_.size(); }
    size_t liveRenderPassCount() const { return liveRenderPasses_.size(); }
    size_t liveComputePassCount() const { return liveComputePasses_.size(); }

private:
    std::unordered_map<WGPUCommandEncoder, WGPURenderPassEncoder> encoderRenderPasses_;
    std::unordered_map<WGPUCommandEncoder, WGPUComputePassEncoder> encoderComputePasses_;
    std::unordered_map<WGPURenderPassEncoder, WGPUCommandEncoder> renderPassOwners_;
    std::unordered_map<WGPUComputePassEncoder, WGPUCommandEncoder> computePassOwners_;
    std::unordered_set<WGPUCommandEncoder> liveCommandEncoders_;
    std::unordered_set<WGPURenderPassEncoder> liveRenderPasses_;
    std::unordered_set<WGPUComputePassEncoder> liveComputePasses_;
    WGPUCommandEncoder surfaceRenderEncoder_ = nullptr;
    bool surfaceRenderPassEnded_ = false;
    CommandEncoderStats stats_;
};

} // namespace mystral::webgpu::bridge
