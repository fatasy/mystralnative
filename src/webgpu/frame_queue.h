#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include <webgpu/webgpu.h>

namespace mystral::webgpu::bridge {

struct FrameQueueStats {
    uint64_t nativeQueueSubmits = 0;
    uint64_t nativeCommandBuffersSubmitted = 0;
    uint64_t nativeQueueSubmitNanoseconds = 0;
    uint64_t deferredUploadCommandBuffers = 0;
    uint64_t forcedFrameFlushes = 0;
    uint64_t maxCommandBuffersPerNativeSubmit = 0;
};

class FrameQueue {
public:
    void configure(WGPUDevice device, WGPUQueue queue);

    void beginFrame();
    void endFrame();
    bool frameActive() const { return frameActive_; }
    bool hasPendingWork() const { return !work_.empty(); }

    void submit(std::vector<WGPUCommandBuffer>&& commandBuffers);
    void writeBuffer(WGPUBuffer buffer, uint64_t offset, const void* data, size_t size);
    void flush();
    void flushBeforeImmediateOperation();

    const FrameQueueStats& stats() const { return stats_; }

private:
    struct BufferCopy {
        WGPUBuffer destination = nullptr;
        uint64_t destinationOffset = 0;
        uint64_t sourceOffset = 0;
        uint64_t size = 0;
    };

    struct Work {
        enum class Kind { BufferCopies, CommandBuffers };
        Kind kind = Kind::CommandBuffers;
        std::vector<BufferCopy> bufferCopies;
        std::vector<WGPUCommandBuffer> commandBuffers;
    };

    static uint64_t alignToCopyOffset(uint64_t value);
    void recordNativeSubmit(const std::vector<WGPUCommandBuffer>& commandBuffers);
    void releaseDeferredCommandBuffers();
    void clearWork();
    void fallbackFlush();

    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    bool frameActive_ = false;
    std::vector<Work> work_;
    std::vector<uint8_t> uploadBytes_;
    std::unordered_set<WGPUBuffer> retainedUploadBuffers_;
    FrameQueueStats stats_;
};

} // namespace mystral::webgpu::bridge
