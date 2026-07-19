#pragma once

#include <cstddef>
#include <cstdint>

#include <webgpu/webgpu.h>

namespace mystral::webgpu::bridge {

class ScreenshotState {
public:
    WGPUBuffer ensureBuffer(WGPUDevice device, uint32_t width, uint32_t height);

    WGPUBuffer buffer() const { return buffer_; }
    size_t bufferSize() const { return bufferSize_; }
    uint32_t bytesPerRow() const { return bytesPerRow_; }

    bool ready() const { return ready_; }
    void markReady() { ready_ = true; }
    void clearReady() { ready_ = false; }

    bool capturedThisFrame() const { return capturedThisFrame_; }
    void markCapturedThisFrame() { capturedThisFrame_ = true; }
    void beginPresentedFrame() { capturedThisFrame_ = false; }

private:
    WGPUBuffer buffer_ = nullptr;
    size_t bufferSize_ = 0;
    uint32_t bytesPerRow_ = 0;
    bool ready_ = false;
    bool capturedThisFrame_ = false;
};

} // namespace mystral::webgpu::bridge
