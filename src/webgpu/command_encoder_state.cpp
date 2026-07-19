#include "webgpu/command_encoder_state.h"

namespace mystral::webgpu::bridge {

void CommandEncoderState::trackCommandEncoder(WGPUCommandEncoder encoder) {
    liveCommandEncoders_.insert(encoder);
    ++stats_.commandEncodersCreated;
}

bool CommandEncoderState::isCommandEncoderLive(WGPUCommandEncoder encoder) const {
    return liveCommandEncoders_.find(encoder) != liveCommandEncoders_.end();
}

void CommandEncoderState::finishCommandEncoder(WGPUCommandEncoder encoder) {
    if (liveCommandEncoders_.erase(encoder) == 0) return;
    wgpuCommandEncoderRelease(encoder);
}

void CommandEncoderState::recordCommandBuffer(WGPUCommandBuffer commandBuffer) {
    if (commandBuffer) ++stats_.commandBuffersCreated;
}

void CommandEncoderState::trackRenderPass(
    WGPUCommandEncoder encoder,
    WGPURenderPassEncoder pass) {
    liveRenderPasses_.insert(pass);
    encoderRenderPasses_[encoder] = pass;
    renderPassOwners_[pass] = encoder;
    ++stats_.renderPassesCreated;
}

void CommandEncoderState::trackComputePass(
    WGPUCommandEncoder encoder,
    WGPUComputePassEncoder pass) {
    liveComputePasses_.insert(pass);
    encoderComputePasses_[encoder] = pass;
    computePassOwners_[pass] = encoder;
    ++stats_.computePassesCreated;
}

WGPURenderPassEncoder CommandEncoderState::renderPassFor(WGPUCommandEncoder encoder) const {
    auto found = encoderRenderPasses_.find(encoder);
    return found == encoderRenderPasses_.end() ? nullptr : found->second;
}

WGPUComputePassEncoder CommandEncoderState::computePassFor(WGPUCommandEncoder encoder) const {
    auto found = encoderComputePasses_.find(encoder);
    return found == encoderComputePasses_.end() ? nullptr : found->second;
}

void CommandEncoderState::closeRenderPass(WGPURenderPassEncoder pass) {
    if (!pass || liveRenderPasses_.erase(pass) == 0) return;
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    auto owner = renderPassOwners_.find(pass);
    if (owner == renderPassOwners_.end()) return;
    encoderRenderPasses_.erase(owner->second);
    if (surfaceRenderEncoder_ == owner->second) surfaceRenderPassEnded_ = true;
    renderPassOwners_.erase(owner);
}

void CommandEncoderState::closeComputePass(WGPUComputePassEncoder pass) {
    if (!pass || liveComputePasses_.erase(pass) == 0) return;
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);

    auto owner = computePassOwners_.find(pass);
    if (owner == computePassOwners_.end()) return;
    encoderComputePasses_.erase(owner->second);
    computePassOwners_.erase(owner);
}

void CommandEncoderState::markSurfaceRenderPass(WGPUCommandEncoder encoder) {
    surfaceRenderEncoder_ = encoder;
    surfaceRenderPassEnded_ = false;
}

void CommandEncoderState::markSurfacePresented() {
    surfaceRenderEncoder_ = nullptr;
    surfaceRenderPassEnded_ = false;
}

} // namespace mystral::webgpu::bridge
