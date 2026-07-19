#include "webgpu/screenshot_state.h"

namespace mystral::webgpu::bridge {

WGPUBuffer ScreenshotState::ensureBuffer(
    WGPUDevice device, uint32_t width, uint32_t height) {
    const uint32_t requiredBytesPerRow = ((width * 4 + 255) / 256) * 256;
    const size_t requiredSize = static_cast<size_t>(requiredBytesPerRow) * height;

    if (!buffer_ || bufferSize_ < requiredSize) {
        if (buffer_) {
            wgpuBufferDestroy(buffer_);
            wgpuBufferRelease(buffer_);
        }

        WGPUBufferDescriptor descriptor = {};
        descriptor.size = requiredSize;
        descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        descriptor.mappedAtCreation = false;

        buffer_ = wgpuDeviceCreateBuffer(device, &descriptor);
        bufferSize_ = requiredSize;
        bytesPerRow_ = requiredBytesPerRow;
    }
    return buffer_;
}

} // namespace mystral::webgpu::bridge
