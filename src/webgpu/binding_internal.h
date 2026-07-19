#pragma once

#include "webgpu/binding_context.h"
#include "webgpu/bridge_profiler.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mystral::webgpu {

extern bool& g_verboseLogging;
extern WGPUAdapter& g_adapter;
extern WGPUDevice& g_device;
extern WGPUQueue& g_queue;
extern WGPUSurface& g_surface;
extern WGPUInstance& g_instance;
extern js::Engine*& g_engine;
extern bridge::AsyncBridge& g_asyncBridge;
extern bridge::Capabilities& g_capabilities;
extern bridge::ImageDecoderBindings& g_imageDecoder;
extern WGPUTexture& g_offscreenTexture;
extern WGPUTextureView& g_offscreenTextureView;
extern WGPUTextureFormat& g_surfaceFormat;
extern uint32_t& g_canvasWidth;
extern uint32_t& g_canvasHeight;
extern bool& g_contextConfigured;
extern WGPUTexture& g_currentTexture;
extern WGPUTextureView& g_currentTextureView;
extern WGPUTexture& g_currentViewSourceTexture;
extern bridge::ScreenshotState& g_screenshot;
extern bridge::CanvasCompositor& g_canvasCompositor;
extern bridge::CommandEncoderState& g_commandEncoders;
extern bridge::FrameQueue& g_frameQueue;
extern bridge::TransientMethodCache& g_transientMethods;
extern std::unordered_map<int, std::unique_ptr<bridge::OffscreenCanvasState>>& g_offscreenCanvases;
extern int& g_nextOffscreenCanvasId;
extern std::unordered_map<uint64_t, bridge::TextureInfo>& g_textureRegistry;
extern uint64_t& g_nextTextureId;
extern uint64_t& g_currentTextureId;
extern std::unordered_map<uint64_t, bridge::BufferInfo>& g_bufferRegistry;
extern uint64_t& g_nextBufferId;
extern std::unordered_map<uint64_t, WGPUComputePipeline>& g_computePipelineRegistry;
extern uint64_t& g_nextComputePipelineId;
extern std::unordered_map<uint64_t, WGPURenderPipeline>& g_renderPipelineRegistry;
extern uint64_t& g_nextRenderPipelineId;
extern uint64_t& g_trackedBufferBytes;
extern uint64_t& g_estimatedTextureBytes;
extern uint64_t& g_peakTrackedGpuBytes;
extern uint64_t& g_maxTrackedGpuMemoryBytes;

bool gcReleaseEnabled();
uint64_t estimateTextureBytes(WGPUTextureFormat format, uint32_t width, uint32_t height,
                              uint32_t depthOrLayers, uint32_t mipLevels, uint32_t samples);
bool canAllocateTrackedGpuBytes(uint64_t additionalBytes);
void updatePeakTrackedGpuBytes();
bool writeQueueBuffer(
    js::JSValueHandle bufferHandle,
    js::JSValueHandle offsetHandle,
    js::JSValueHandle data,
    const js::JSValueHandle* dataOffsetHandle,
    const js::JSValueHandle* sizeHandle,
    bridge::Operation operation,
    const char* apiName);
bool parseDynamicOffsets(
    const std::vector<js::JSValueHandle>& args,
    std::vector<uint32_t>& offsets,
    const char* apiName);
js::JSValueHandle sharedMethod(
    js::JSValueHandle& slot,
    const char* name,
    js::NativeMethod method);
void releaseBuffer(uint64_t id, bool destroy);
void releaseTexture(uint64_t id, bool destroy);
void releaseRenderPipeline(uint64_t id);
void releaseComputePipeline(uint64_t id);
js::JSValueHandle wrapRenderPipeline(WGPURenderPipeline pipeline);
js::JSValueHandle wrapComputePipeline(WGPUComputePipeline pipeline);
void presentSurfaceIfReady();

} // namespace mystral::webgpu
