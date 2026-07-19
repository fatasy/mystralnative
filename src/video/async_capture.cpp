/**
 * Async Video Capture Implementation
 *
 * Uses a pool of GPU readback buffers with async mapping to capture
 * frames without blocking the render loop.
 */

#include "mystral/video/async_capture.h"
#include <webgpu/webgpu.h>
#include "mystral/webgpu_compat.h"
#include <iostream>
#include <cstring>

namespace mystral {
namespace video {

// Calculate bytes per row with 256-byte alignment (WebGPU requirement)
static uint32_t alignedBytesPerRow(uint32_t width) {
    uint32_t bytesPerRow = width * 4;  // RGBA
    return (bytesPerRow + 255) & ~255;  // Align to 256 bytes
}

// Dawn buffer map callback.
static void onBufferMappedInfo(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void* userdata2) {
    auto* buffer = static_cast<ReadbackBuffer*>(userdata1);
    buffer->mapStatus = (status == WGPUMapAsyncStatus_Success) ? 0 : static_cast<int>(status);
    buffer->mapComplete.store(true, std::memory_order_release);
    if (buffer->frameNumber == 0) {
        std::cout << "[AsyncCapture] Buffer map callback for frame " << buffer->frameNumber
                  << ": status=" << static_cast<int>(status) << std::endl;
    }
}

AsyncCapture::AsyncCapture() = default;

AsyncCapture::~AsyncCapture() {
    shutdown();
}

bool AsyncCapture::initialize(WGPUDevice device, WGPUQueue queue, WGPUInstance instance,
                               const AsyncCaptureConfig& config) {
    if (initialized_) {
        return true;
    }

    device_ = device;
    queue_ = queue;
    instance_ = instance;
    config_ = config;

    // Pre-allocate buffer pool (buffers are created lazily with proper dimensions)
    bufferPool_.reserve(config_.maxBufferCount);

    initialized_ = true;
    std::cout << "[AsyncCapture] Initialized with pool capacity " << config_.maxBufferCount
              << ", queue capacity " << config_.maxQueuedFrames << std::endl;

    return true;
}

void AsyncCapture::shutdown() {
    if (!initialized_) {
        return;
    }

    // Release all GPU buffers
    for (auto& bufferPtr : bufferPool_) {
        if (bufferPtr && bufferPtr->buffer) {
            if (bufferPtr->state == BufferState::Mapped) {
                wgpuBufferUnmap(bufferPtr->buffer);
            }
            wgpuBufferRelease(bufferPtr->buffer);
            bufferPtr->buffer = nullptr;
        }
    }
    bufferPool_.clear();

    // Clear frame queue
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        while (!frameQueue_.empty()) {
            frameQueue_.pop();
        }
    }

    initialized_ = false;
    std::cout << "[AsyncCapture] Shutdown complete. Captured: " << capturedFrames_.load()
              << ", Dropped: " << droppedFrames_.load() << std::endl;
}

bool AsyncCapture::createBuffer(ReadbackBuffer& buffer, uint32_t width, uint32_t height) {
    buffer.width = width;
    buffer.height = height;
    buffer.bytesPerRow = alignedBytesPerRow(width);
    buffer.size = buffer.bytesPerRow * height;

    WGPUBufferDescriptor desc = {};
    desc.size = buffer.size;
    desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    desc.mappedAtCreation = false;

    buffer.buffer = wgpuDeviceCreateBuffer(device_, &desc);
    if (!buffer.buffer) {
        std::cerr << "[AsyncCapture] Failed to create readback buffer" << std::endl;
        return false;
    }

    buffer.state = BufferState::Free;
    return true;
}

ReadbackBuffer* AsyncCapture::acquireBuffer() {
    // First, try to find a free buffer with matching dimensions
    for (auto& bufferPtr : bufferPool_) {
        if (bufferPtr && bufferPtr->state == BufferState::Free) {
            return bufferPtr.get();
        }
    }

    // No free buffer - try to grow the pool
    if (static_cast<int>(bufferPool_.size()) < config_.maxBufferCount) {
        auto newBuffer = std::make_unique<ReadbackBuffer>();
        bufferPool_.push_back(std::move(newBuffer));
        return bufferPool_.back().get();
    }

    // Pool is full - no buffer available
    return nullptr;
}

void AsyncCapture::releaseBuffer(ReadbackBuffer* buffer) {
    if (buffer) {
        buffer->state = BufferState::Free;
        buffer->frameNumber = -1;
        buffer->mapComplete.store(false, std::memory_order_release);
    }
}

bool AsyncCapture::submitCapture(WGPUTexture sourceTexture, uint32_t width, uint32_t height, int frameNumber) {
    if (!initialized_ || !sourceTexture) {
        std::cerr << "[AsyncCapture] submitCapture failed: initialized=" << initialized_ << ", texture=" << (void*)sourceTexture << std::endl;
        return false;
    }

    if (frameNumber == 0) {
        std::cout << "[AsyncCapture] First frame: " << width << "x" << height << ", texture=" << (void*)sourceTexture << std::endl;
    }

    // Acquire a buffer
    ReadbackBuffer* buffer = acquireBuffer();
    if (!buffer) {
        droppedFrames_++;
        return false;
    }

    // Create or resize buffer if needed
    if (!buffer->buffer || buffer->width != width || buffer->height != height) {
        if (buffer->buffer) {
            wgpuBufferRelease(buffer->buffer);
            buffer->buffer = nullptr;
        }
        if (!createBuffer(*buffer, width, height)) {
            return false;
        }
    }

    // Create command encoder for the copy
    WGPUCommandEncoderDescriptor encDesc = {};
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, &encDesc);
    if (!encoder) {
        return false;
    }

    // Copy texture to buffer
    WGPUImageCopyTexture_Compat srcCopy = {};
    srcCopy.texture = sourceTexture;
    srcCopy.mipLevel = 0;
    srcCopy.origin = {0, 0, 0};
    srcCopy.aspect = WGPUTextureAspect_All;

    WGPUImageCopyBuffer_Compat dstCopy = {};
    dstCopy.buffer = buffer->buffer;
    dstCopy.layout.offset = 0;
    dstCopy.layout.bytesPerRow = buffer->bytesPerRow;
    dstCopy.layout.rowsPerImage = height;

    WGPUExtent3D copySize = {width, height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &srcCopy, &dstCopy, &copySize);

    // Submit the copy command
    WGPUCommandBufferDescriptor cmdDesc = {};
    WGPUCommandBuffer cmdBuffer = wgpuCommandEncoderFinish(encoder, &cmdDesc);
    wgpuQueueSubmit(queue_, 1, &cmdBuffer);
    wgpuCommandBufferRelease(cmdBuffer);
    wgpuCommandEncoderRelease(encoder);

    // Start async buffer map
    buffer->frameNumber = frameNumber;
    buffer->mapComplete.store(false, std::memory_order_release);
    buffer->state = BufferState::MapPending;

    WGPUBufferMapCallbackInfo mapCallbackInfo = {};
    mapCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    mapCallbackInfo.callback = onBufferMappedInfo;
    mapCallbackInfo.userdata1 = buffer;
    mapCallbackInfo.userdata2 = nullptr;
    wgpuBufferMapAsync(buffer->buffer, WGPUMapMode_Read, 0, buffer->size, mapCallbackInfo);

    return true;
}

bool AsyncCapture::submitCaptureSync(WGPUTexture sourceTexture, uint32_t width, uint32_t height, int frameNumber) {
    if (!initialized_ || !sourceTexture) {
        return false;
    }

    // For sync capture, create a fresh buffer each time to avoid mapping state issues
    // WebGPU buffer unmapping can be async, so reusing buffers immediately causes errors
    ReadbackBuffer tempBuffer;
    if (!createBuffer(tempBuffer, width, height)) {
        droppedFrames_++;
        return false;
    }
    ReadbackBuffer* buffer = &tempBuffer;

    // Create command encoder for the copy
    WGPUCommandEncoderDescriptor encDesc = {};
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, &encDesc);
    if (!encoder) {
        return false;
    }

    // Copy texture to buffer
    WGPUImageCopyTexture_Compat srcCopy = {};
    srcCopy.texture = sourceTexture;
    srcCopy.mipLevel = 0;
    srcCopy.origin = {0, 0, 0};
    srcCopy.aspect = WGPUTextureAspect_All;

    WGPUImageCopyBuffer_Compat dstCopy = {};
    dstCopy.buffer = buffer->buffer;
    dstCopy.layout.offset = 0;
    dstCopy.layout.bytesPerRow = buffer->bytesPerRow;
    dstCopy.layout.rowsPerImage = height;

    WGPUExtent3D copySize = {width, height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &srcCopy, &dstCopy, &copySize);

    // Submit the copy command
    WGPUCommandBufferDescriptor cmdDesc = {};
    WGPUCommandBuffer cmdBuffer = wgpuCommandEncoderFinish(encoder, &cmdDesc);
    wgpuQueueSubmit(queue_, 1, &cmdBuffer);
    wgpuCommandBufferRelease(cmdBuffer);
    wgpuCommandEncoderRelease(encoder);

    // Synchronously wait for the copy to complete by mapping the buffer synchronously
    buffer->frameNumber = frameNumber;
    buffer->mapComplete.store(false, std::memory_order_release);
    buffer->state = BufferState::MapPending;

    WGPUBufferMapCallbackInfo mapCallbackInfo = {};
    mapCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    mapCallbackInfo.callback = onBufferMappedInfo;
    mapCallbackInfo.userdata1 = buffer;
    mapCallbackInfo.userdata2 = nullptr;
    wgpuBufferMapAsync(buffer->buffer, WGPUMapMode_Read, 0, buffer->size, mapCallbackInfo);

    // Wait for the map to complete (synchronous wait)
    int maxIterations = 1000;  // Prevent infinite loop
    while (!buffer->mapComplete.load(std::memory_order_acquire) && maxIterations > 0) {
#if defined(MYSTRAL_WEBGPU_DAWN)
        wgpuDeviceTick(device_);
#endif
        if (instance_) {
            wgpuInstanceProcessEvents(instance_);
        }
        maxIterations--;
    }

    if (maxIterations == 0) {
        std::cerr << "[AsyncCapture] Sync capture timeout waiting for buffer map" << std::endl;
        wgpuBufferDestroy(buffer->buffer);
        wgpuBufferRelease(buffer->buffer);
        droppedFrames_++;
        return false;
    }

    if (buffer->mapStatus != static_cast<int>(WGPUMapAsyncStatus_Success)) {
        std::cerr << "[AsyncCapture] Sync capture map failed with status " << buffer->mapStatus << std::endl;
        wgpuBufferDestroy(buffer->buffer);
        wgpuBufferRelease(buffer->buffer);
        droppedFrames_++;
        return false;
    }

    // Read data and queue the frame
    buffer->state = BufferState::Mapped;

    // Get mapped data
    const void* mappedData = wgpuBufferGetConstMappedRange(buffer->buffer, 0, buffer->size);
    if (!mappedData) {
        std::cerr << "[AsyncCapture] Sync capture failed to get mapped range" << std::endl;
        wgpuBufferUnmap(buffer->buffer);
        wgpuBufferDestroy(buffer->buffer);
        wgpuBufferRelease(buffer->buffer);
        droppedFrames_++;
        return false;
    }

    if (frameNumber == 0) {
        std::cout << "[AsyncCapture] Sync capture frame 0: mappedData=" << mappedData << std::endl;
        // Check pixel data
        const uint8_t* src = static_cast<const uint8_t*>(mappedData);
        int nonZeroCount = 0;
        for (size_t i = 0; i < std::min(buffer->size, (size_t)10000); i++) {
            if (src[i] != 0) nonZeroCount++;
        }
        std::cout << "[AsyncCapture] First 10000 bytes: " << nonZeroCount << " non-zero" << std::endl;
    }

    // Copy data to frame queue (BGRA -> RGBA conversion)
    {
        std::lock_guard<std::mutex> lock(queueMutex_);

        CapturedFrame frame;
        frame.width = buffer->width;
        frame.height = buffer->height;
        frame.frameNumber = frameNumber;
        frame.timestamp = frameNumber / 60.0;
        frame.pixels.resize(buffer->width * buffer->height * 4);

        const uint8_t* src = static_cast<const uint8_t*>(mappedData);
        uint8_t* dst = frame.pixels.data();

        for (uint32_t y = 0; y < buffer->height; y++) {
            const uint8_t* srcRow = src + y * buffer->bytesPerRow;
            uint8_t* dstRow = dst + y * buffer->width * 4;
            for (uint32_t x = 0; x < buffer->width; x++) {
                // BGRA -> RGBA
                dstRow[x * 4 + 0] = srcRow[x * 4 + 2];  // R <- B
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1];  // G
                dstRow[x * 4 + 2] = srcRow[x * 4 + 0];  // B <- R
                dstRow[x * 4 + 3] = srcRow[x * 4 + 3];  // A
            }
        }

        frameQueue_.push(std::move(frame));
    }

    // Cleanup temp buffer
    wgpuBufferUnmap(buffer->buffer);
    wgpuBufferDestroy(buffer->buffer);
    wgpuBufferRelease(buffer->buffer);
    capturedFrames_++;

    return true;
}

void AsyncCapture::processBuffer(ReadbackBuffer& buffer) {
    if (buffer.state != BufferState::MapPending) {
        return;
    }

    // Check if map is complete (non-blocking)
    if (!buffer.mapComplete.load(std::memory_order_acquire)) {
        return;
    }

    if (buffer.mapStatus != static_cast<int>(WGPUMapAsyncStatus_Success)) {
        std::cerr << "[AsyncCapture] Buffer map failed with status " << buffer.mapStatus << std::endl;
        releaseBuffer(&buffer);
        droppedFrames_++;
        return;
    }

    buffer.state = BufferState::Mapped;
    copyToFrameQueue(&buffer);
}

void AsyncCapture::copyToFrameQueue(ReadbackBuffer* buffer) {
    if (!buffer || buffer->state != BufferState::Mapped) {
        return;
    }

    // Get mapped data
    const void* mappedData = wgpuBufferGetConstMappedRange(buffer->buffer, 0, buffer->size);
    if (!mappedData) {
        std::cerr << "[AsyncCapture] Failed to get mapped range for frame " << buffer->frameNumber << std::endl;
        wgpuBufferUnmap(buffer->buffer);
        releaseBuffer(buffer);
        droppedFrames_++;
        return;
    }

    if (buffer->frameNumber == 0) {
        std::cout << "[AsyncCapture] mappedData=" << mappedData << ", size=" << buffer->size << std::endl;
    }

    // Check queue capacity with backpressure
    {
        std::lock_guard<std::mutex> lock(queueMutex_);

        if (config_.dropFramesOnBackpressure &&
            static_cast<int>(frameQueue_.size()) >= config_.maxQueuedFrames) {
            // Drop oldest frame
            frameQueue_.pop();
            droppedFrames_++;
        }

        // Create frame and copy data (BGRA -> RGBA conversion)
        CapturedFrame frame;
        frame.width = buffer->width;
        frame.height = buffer->height;
        frame.frameNumber = buffer->frameNumber;
        frame.timestamp = buffer->frameNumber / 60.0;  // Assuming 60 FPS
        frame.pixels.resize(buffer->width * buffer->height * 4);

        if (buffer->frameNumber == 0) {
            std::cout << "[AsyncCapture] copyToFrameQueue: frame " << buffer->frameNumber
                      << " with " << buffer->width << "x" << buffer->height
                      << " (bytesPerRow=" << buffer->bytesPerRow << ", size=" << buffer->size << ")" << std::endl;

            // Check if pixel data is valid (not all zeros)
            const uint8_t* src = static_cast<const uint8_t*>(mappedData);
            int nonZeroCount = 0;
            for (size_t i = 0; i < std::min(buffer->size, (size_t)10000); i++) {
                if (src[i] != 0) nonZeroCount++;
            }
            std::cout << "[AsyncCapture] First 10000 bytes: " << nonZeroCount << " non-zero" << std::endl;
        }

        const uint8_t* src = static_cast<const uint8_t*>(mappedData);
        uint8_t* dst = frame.pixels.data();

        for (uint32_t y = 0; y < buffer->height; y++) {
            const uint8_t* srcRow = src + y * buffer->bytesPerRow;
            uint8_t* dstRow = dst + y * buffer->width * 4;
            for (uint32_t x = 0; x < buffer->width; x++) {
                // BGRA -> RGBA
                dstRow[x * 4 + 0] = srcRow[x * 4 + 2];  // R <- B
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1];  // G
                dstRow[x * 4 + 2] = srcRow[x * 4 + 0];  // B <- R
                dstRow[x * 4 + 3] = srcRow[x * 4 + 3];  // A
            }
        }

        frameQueue_.push(std::move(frame));
        capturedFrames_++;
    }

    // Unmap and release buffer
    wgpuBufferUnmap(buffer->buffer);
    releaseBuffer(buffer);
}

void AsyncCapture::processAsync() {
    if (!initialized_) {
        return;
    }

    // Process events to trigger callbacks
    if (instance_) {
        wgpuInstanceProcessEvents(instance_);
    }
#if defined(MYSTRAL_WEBGPU_DAWN)
    wgpuDeviceTick(device_);
#endif

    // Check all buffers for completed maps
    for (auto& bufferPtr : bufferPool_) {
        if (bufferPtr) {
            processBuffer(*bufferPtr);
        }
    }
}

bool AsyncCapture::tryGetFrame(CapturedFrame& outFrame) {
    std::lock_guard<std::mutex> lock(queueMutex_);

    if (frameQueue_.empty()) {
        return false;
    }

    outFrame = std::move(frameQueue_.front());
    frameQueue_.pop();
    return true;
}

size_t AsyncCapture::getQueuedFrameCount() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return frameQueue_.size();
}

int AsyncCapture::getActiveBufferCount() const {
    int count = 0;
    for (const auto& bufferPtr : bufferPool_) {
        if (bufferPtr && bufferPtr->state != BufferState::Free) {
            count++;
        }
    }
    return count;
}

AsyncCapture::Stats AsyncCapture::getStats() const {
    Stats stats;
    stats.capturedFrames = capturedFrames_.load();
    stats.droppedFrames = droppedFrames_.load();
    stats.bufferPoolSize = static_cast<int>(bufferPool_.size());
    stats.activeBuffers = getActiveBufferCount();
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stats.queuedFrames = static_cast<int>(frameQueue_.size());
    }
    return stats;
}

}  // namespace video
}  // namespace mystral
