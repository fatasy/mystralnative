#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
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
    uint64_t uploadBufferAllocations = 0;
    uint64_t uploadBufferCapacityBytes = 0;
    uint64_t peakUploadBytesPerFlush = 0;
    uint64_t frameLatencyWaits = 0;
    uint64_t frameLatencyWaitNanoseconds = 0;
    uint64_t maxInFlightSubmissions = 0;
};

class FrameQueue {
public:
    void configure(WGPUInstance instance, WGPUDevice device, WGPUQueue queue,
                   uint32_t maxFrameLatency);
    void reset();

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

    struct UploadSlot {
        WGPUBuffer buffer = nullptr;
        uint64_t capacity = 0;
    };

    static uint64_t alignToCopyOffset(uint64_t value);
    void recordNativeSubmit(const std::vector<WGPUCommandBuffer>& commandBuffers);
    void releaseDeferredCommandBuffers();
    void clearWork();
    void fallbackFlush();
    WGPUBuffer acquireUploadBuffer(size_t requiredSize);
    void waitForOldestSubmission();

    WGPUInstance instance_ = nullptr;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    uint32_t maxFrameLatency_ = 2;
    bool frameActive_ = false;
    std::vector<Work> work_;
    std::vector<uint8_t> uploadBytes_;
    std::array<UploadSlot, 3> uploadRing_{};
    size_t nextUploadSlot_ = 0;
    std::deque<WGPUFuture> inFlightSubmissions_;
    std::unordered_set<WGPUBuffer> retainedUploadBuffers_;
    FrameQueueStats stats_;
};

} // namespace mystral::webgpu::bridge
