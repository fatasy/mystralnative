#pragma once

#include "mystral/canvas/canvas2d.h"
#include "webgpu/frame_queue.h"
#include "webgpu/screenshot_state.h"

#include <cstdint>
#include <vector>
#include <webgpu/webgpu.h>

namespace mystral::webgpu::bridge {

struct CanvasCompositorStats {
    uint64_t framesComposited = 0;
    uint64_t framesWithoutUpload = 0;
    uint64_t textureUploads = 0;
    uint64_t textureUploadBytes = 0;
};

class CanvasCompositor {
public:
    explicit CanvasCompositor(ScreenshotState& screenshot) : screenshot_(screenshot) {}

    void configure(WGPUDevice device, WGPUQueue queue,
                   WGPUSurface surface, WGPUTextureFormat surfaceFormat);
    void setSurfaceFormat(WGPUTextureFormat surfaceFormat);
    void setContext(canvas::Canvas2DContext* context) { context_ = context; }
    bool composite(uint32_t canvasWidth, uint32_t canvasHeight, FrameQueue& frameQueue);
    void present();
    const CanvasCompositorStats& stats() const { return stats_; }

private:
    ScreenshotState& screenshot_;
    canvas::Canvas2DContext* context_ = nullptr;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUSurface surface_ = nullptr;
    WGPUTextureFormat surfaceFormat_ = WGPUTextureFormat_BGRA8UnormSrgb;
    WGPUTexture texture_ = nullptr;
    WGPURenderPipeline pipeline_ = nullptr;
    WGPUBindGroup bindGroup_ = nullptr;
    WGPUSampler sampler_ = nullptr;
    uint32_t textureWidth_ = 0;
    uint32_t textureHeight_ = 0;
    WGPUTexture surfaceTexture_ = nullptr;
    WGPUTextureView surfaceView_ = nullptr;
    std::vector<uint8_t> uploadScratch_;
    CanvasCompositorStats stats_;
};

} // namespace mystral::webgpu::bridge
