#pragma once

#include <cstddef>
#include <cstdint>

#include <webgpu/webgpu.h>

namespace mystral::webgpu::bridge {

struct ScreenshotStats {
    uint64_t requests = 0;
    uint64_t capturedFrames = 0;
    uint64_t capturedBytes = 0;
};

class ScreenshotState {
public:
    WGPUBuffer ensureBuffer(WGPUDevice device, uint32_t width, uint32_t height);

    WGPUBuffer buffer() const { return buffer_; }
    size_t bufferSize() const { return bufferSize_; }
    uint32_t bytesPerRow() const { return bytesPerRow_; }

    void requestCapture() {
        if (captureRequested_ || ready_) return;
        captureRequested_ = true;
        stats_.requests++;
    }
    bool shouldCapture() const {
        return captureRequested_ && !ready_ && !capturedThisFrame_;
    }

    bool ready() const { return ready_; }
    void clearReady() { ready_ = false; }
    void markCaptured(uint32_t width, uint32_t height) {
        captureRequested_ = false;
        ready_ = true;
        capturedThisFrame_ = true;
        capturedWidth_ = width;
        capturedHeight_ = height;
        stats_.capturedFrames++;
        stats_.capturedBytes += bufferSize_;
    }

    uint32_t capturedWidth() const { return capturedWidth_; }
    uint32_t capturedHeight() const { return capturedHeight_; }
    const ScreenshotStats& stats() const { return stats_; }

    void beginPresentedFrame() { capturedThisFrame_ = false; }

private:
    WGPUBuffer buffer_ = nullptr;
    size_t bufferSize_ = 0;
    uint32_t bytesPerRow_ = 0;
    uint32_t capturedWidth_ = 0;
    uint32_t capturedHeight_ = 0;
    bool captureRequested_ = false;
    bool ready_ = false;
    bool capturedThisFrame_ = false;
    ScreenshotStats stats_;
};

} // namespace mystral::webgpu::bridge
