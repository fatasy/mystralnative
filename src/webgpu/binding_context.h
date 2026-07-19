#pragma once

#include "mystral/js/engine.h"
#include "webgpu/async_bridge.h"
#include "webgpu/canvas_compositor.h"
#include "webgpu/capabilities.h"
#include "webgpu/command_encoder_state.h"
#include "webgpu/frame_queue.h"
#include "webgpu/image_decoder.h"
#include "webgpu/screenshot_state.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace mystral::webgpu::bridge {

struct OffscreenCanvasState {
    int width = 300;
    int height = 150;
    js::JSValueHandle context2d;
    bool hasContext2d = false;
};

struct TextureInfo {
    WGPUTexture texture = nullptr;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depthOrArrayLayers = 0;
    uint32_t mipLevelCount = 0;
    WGPUTextureDimension dimension = WGPUTextureDimension_2D;
    bool ownsReference = false;
    bool destroyOnReload = false;
    uint64_t estimatedBytes = 0;
};

struct BufferInfo {
    WGPUBuffer buffer = nullptr;
    uint64_t size = 0;
    WGPUBufferUsage usage = WGPUBufferUsage_None;
    bool isMapped = false;
    void* mappedData = nullptr;
    uint64_t mappedSize = 0;
    WGPUMapMode mapMode = WGPUMapMode_None;
    bool mapPending = false;
};

struct TransientMethodCache {
    js::JSValueHandle surfaceTextureCreateView;
    js::JSValueHandle surfaceTextureDestroy;
    js::JSValueHandle encoderBeginRenderPass;
    js::JSValueHandle encoderBeginComputePass;
    js::JSValueHandle encoderCopyBufferToBuffer;
    js::JSValueHandle encoderCopyBufferToTexture;
    js::JSValueHandle encoderCopyTextureToBuffer;
    js::JSValueHandle encoderCopyTextureToTexture;
    js::JSValueHandle encoderClearBuffer;
    js::JSValueHandle encoderWriteTimestamp;
    js::JSValueHandle encoderResolveQuerySet;
    js::JSValueHandle encoderFinish;
    js::JSValueHandle renderSetPipeline;
    js::JSValueHandle renderSetBindGroup;
    js::JSValueHandle renderDraw;
    js::JSValueHandle renderSetVertexBuffer;
    js::JSValueHandle renderSetIndexBuffer;
    js::JSValueHandle renderDrawIndexed;
    js::JSValueHandle renderDrawIndirect;
    js::JSValueHandle renderDrawIndexedIndirect;
    js::JSValueHandle renderSetViewport;
    js::JSValueHandle renderSetScissorRect;
    js::JSValueHandle renderSetBlendConstant;
    js::JSValueHandle renderSetStencilReference;
    js::JSValueHandle renderExecuteBundles;
    js::JSValueHandle renderWriteTimestamp;
    js::JSValueHandle renderBeginOcclusionQuery;
    js::JSValueHandle renderEndOcclusionQuery;
    js::JSValueHandle renderEnd;
    js::JSValueHandle computeSetPipeline;
    js::JSValueHandle computeSetBindGroup;
    js::JSValueHandle computeDispatchWorkgroups;
    js::JSValueHandle computeWriteTimestamp;
    js::JSValueHandle computeEnd;
};

class BindingContext {
public:
    BindingContext() : canvasCompositor(screenshot) {}

    bool verboseLogging = false;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPUSurface surface = nullptr;
    WGPUInstance instance = nullptr;
    js::Engine* engine = nullptr;

    AsyncBridge asyncBridge;
    Capabilities capabilities;
    ImageDecoderBindings imageDecoder;

    WGPUTexture offscreenTexture = nullptr;
    WGPUTextureView offscreenTextureView = nullptr;
    WGPUTextureFormat surfaceFormat = WGPUTextureFormat_BGRA8UnormSrgb;
    uint32_t canvasWidth = 800;
    uint32_t canvasHeight = 600;
    bool contextConfigured = false;
    WGPUTexture currentTexture = nullptr;
    WGPUTextureView currentTextureView = nullptr;
    WGPUTexture currentViewSourceTexture = nullptr;

    ScreenshotState screenshot;
    CanvasCompositor canvasCompositor;
    CommandEncoderState commandEncoders;
    FrameQueue frameQueue;
    TransientMethodCache transientMethods;

    std::unordered_map<int, std::unique_ptr<OffscreenCanvasState>> offscreenCanvases;
    int nextOffscreenCanvasId = 0;
    std::unordered_map<uint64_t, TextureInfo> textureRegistry;
    uint64_t nextTextureId = 1;
    uint64_t currentTextureId = 0;
    std::unordered_map<uint64_t, BufferInfo> bufferRegistry;
    uint64_t nextBufferId = 1;
    std::unordered_map<uint64_t, WGPUComputePipeline> computePipelineRegistry;
    uint64_t nextComputePipelineId = 1;
    std::unordered_map<uint64_t, WGPURenderPipeline> renderPipelineRegistry;
    uint64_t nextRenderPipelineId = 1;
    uint64_t trackedBufferBytes = 0;
    uint64_t estimatedTextureBytes = 0;
    uint64_t peakTrackedGpuBytes = 0;
    uint64_t maxTrackedGpuMemoryBytes = 0;
};

inline BindingContext& bindingContext() {
    static BindingContext context;
    return context;
}

} // namespace mystral::webgpu::bridge
