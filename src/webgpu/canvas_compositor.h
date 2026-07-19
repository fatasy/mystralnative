#pragma once

#include "mystral/canvas/canvas2d.h"
#include "webgpu/screenshot_state.h"

#include <webgpu/webgpu.h>

namespace mystral::webgpu::bridge {

class CanvasCompositor {
public:
    explicit CanvasCompositor(ScreenshotState& screenshot) : screenshot_(screenshot) {}

    void configure(WGPUDevice device, WGPUQueue queue,
                   WGPUSurface surface, WGPUTextureFormat surfaceFormat);
    void setSurfaceFormat(WGPUTextureFormat surfaceFormat) { surfaceFormat_ = surfaceFormat; }
    void setContext(canvas::Canvas2DContext* context) { context_ = context; }
    WGPUTexture composite(uint32_t canvasWidth, uint32_t canvasHeight);

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
};

} // namespace mystral::webgpu::bridge
