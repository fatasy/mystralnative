#include "webgpu/frame_queue.h"

#include "webgpu/bridge_profiler.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace mystral::webgpu::bridge {

namespace {

void onFrameWorkDone(WGPUQueueWorkDoneStatus, WGPUStringView, void*, void*) {}

}  // namespace

void FrameQueue::configure(WGPUInstance instance, WGPUDevice device, WGPUQueue queue,
                           uint32_t maxFrameLatency) {
    if (device_ && device_ != device) reset();
    instance_ = instance;
    device_ = device;
    queue_ = queue;
    maxFrameLatency_ = std::max<uint32_t>(1, maxFrameLatency);
}

void FrameQueue::reset() {
    if (hasPendingWork()) flush();
    while (!inFlightSubmissions_.empty()) waitForOldestSubmission();
    for (auto& slot : uploadRing_) {
        if (slot.buffer) wgpuBufferRelease(slot.buffer);
        slot = {};
    }
    nextUploadSlot_ = 0;
    instance_ = nullptr;
    device_ = nullptr;
    queue_ = nullptr;
    frameActive_ = false;
}

void FrameQueue::beginFrame() {
    while (inFlightSubmissions_.size() >= maxFrameLatency_) {
        waitForOldestSubmission();
    }
    if (hasPendingWork()) {
        stats_.forcedFrameFlushes++;
        flush();
    }
    frameActive_ = true;
}

void FrameQueue::waitForOldestSubmission() {
    if (inFlightSubmissions_.empty()) return;
    if (!instance_) {
        inFlightSubmissions_.pop_front();
        return;
    }

    WGPUFutureWaitInfo waitInfo = WGPU_FUTURE_WAIT_INFO_INIT;
    waitInfo.future = inFlightSubmissions_.front();
    const auto startedAt = std::chrono::steady_clock::now();
    wgpuInstanceWaitAny(instance_, 1, &waitInfo, (std::numeric_limits<uint64_t>::max)());
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;
    stats_.frameLatencyWaits++;
    stats_.frameLatencyWaitNanoseconds += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    inFlightSubmissions_.pop_front();
}

void FrameQueue::endFrame() {
    flush();
    frameActive_ = false;
}

uint64_t FrameQueue::alignToCopyOffset(uint64_t value) {
    return (value + 3u) & ~uint64_t(3u);
}

void FrameQueue::recordNativeSubmit(const std::vector<WGPUCommandBuffer>& commandBuffers) {
    if (!queue_ || commandBuffers.empty()) return;

    const auto startedAt = std::chrono::steady_clock::now();
    wgpuQueueSubmit(queue_, commandBuffers.size(), commandBuffers.data());
    if (instance_) {
        WGPUQueueWorkDoneCallbackInfo callbackInfo = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
        callbackInfo.mode = WGPUCallbackMode_WaitAnyOnly;
        callbackInfo.callback = onFrameWorkDone;
        inFlightSubmissions_.push_back(wgpuQueueOnSubmittedWorkDone(queue_, callbackInfo));
        stats_.maxInFlightSubmissions = std::max<uint64_t>(
            stats_.maxInFlightSubmissions, inFlightSubmissions_.size());
    }
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;
    const uint64_t elapsedNanoseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());

    stats_.nativeQueueSubmits++;
    stats_.nativeCommandBuffersSubmitted += commandBuffers.size();
    stats_.maxCommandBuffersPerNativeSubmit = std::max<uint64_t>(
        stats_.maxCommandBuffersPerNativeSubmit, commandBuffers.size());
    stats_.nativeQueueSubmitNanoseconds += elapsedNanoseconds;
    record(Operation::NativeQueueSubmit, elapsedNanoseconds);
}

void FrameQueue::releaseDeferredCommandBuffers() {
    for (auto& work : work_) {
        for (auto commandBuffer : work.commandBuffers) {
            if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
        }
    }
}

void FrameQueue::clearWork() {
    for (auto buffer : retainedUploadBuffers_) {
        wgpuBufferRelease(buffer);
    }
    retainedUploadBuffers_.clear();
    work_.clear();
    uploadBytes_.clear();
}

void FrameQueue::fallbackFlush() {
    for (auto& work : work_) {
        if (work.kind == Work::Kind::BufferCopies) {
            for (const auto& copy : work.bufferCopies) {
                wgpuQueueWriteBuffer(queue_, copy.destination, copy.destinationOffset,
                    uploadBytes_.data() + copy.sourceOffset, copy.size);
            }
            continue;
        }

        recordNativeSubmit(work.commandBuffers);
        for (auto commandBuffer : work.commandBuffers) {
            if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
        }
    }
    clearWork();
}

WGPUBuffer FrameQueue::acquireUploadBuffer(size_t requiredSize) {
    if (!device_ || !queue_ || requiredSize == 0) return nullptr;
    auto& slot = uploadRing_[nextUploadSlot_++ % uploadRing_.size()];
    const uint64_t alignedSize = alignToCopyOffset(requiredSize);
    if (!slot.buffer || slot.capacity < alignedSize) {
        uint64_t capacity = 64 * 1024;
        while (capacity < alignedSize) capacity *= 2;

        WGPUBufferDescriptor descriptor = {};
        descriptor.size = capacity;
        descriptor.usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
        descriptor.mappedAtCreation = false;
        WGPUBuffer replacement = wgpuDeviceCreateBuffer(device_, &descriptor);
        if (!replacement) return nullptr;
        if (slot.buffer) wgpuBufferRelease(slot.buffer);
        slot.buffer = replacement;
        slot.capacity = capacity;
        stats_.uploadBufferAllocations++;
        stats_.uploadBufferCapacityBytes = 0;
        for (const auto& current : uploadRing_) {
            stats_.uploadBufferCapacityBytes += current.capacity;
        }
    }

    wgpuQueueWriteBuffer(queue_, slot.buffer, 0, uploadBytes_.data(), uploadBytes_.size());
    stats_.peakUploadBytesPerFlush = std::max<uint64_t>(
        stats_.peakUploadBytesPerFlush, uploadBytes_.size());
    return slot.buffer;
}

void FrameQueue::flush() {
    if (work_.empty()) return;
    if (!queue_ || !device_) {
        releaseDeferredCommandBuffers();
        clearWork();
        return;
    }

    WGPUBuffer stagingBuffer = nullptr;
    if (!uploadBytes_.empty()) {
        stagingBuffer = acquireUploadBuffer(uploadBytes_.size());
        if (!stagingBuffer) {
            fallbackFlush();
            return;
        }
    }

    std::vector<WGPUCommandBuffer> submissionBuffers;
    std::vector<WGPUCommandBuffer> uploadCommandBuffers;
    bool encodingFailed = false;

    for (const auto& work : work_) {
        if (work.kind == Work::Kind::CommandBuffers) {
            submissionBuffers.insert(submissionBuffers.end(),
                work.commandBuffers.begin(), work.commandBuffers.end());
            continue;
        }

        WGPUCommandEncoderDescriptor encoderDescriptor = {};
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, &encoderDescriptor);
        if (!encoder) {
            encodingFailed = true;
            break;
        }

        for (const auto& copy : work.bufferCopies) {
            wgpuCommandEncoderCopyBufferToBuffer(encoder,
                stagingBuffer, copy.sourceOffset,
                copy.destination, copy.destinationOffset,
                copy.size);
        }

        WGPUCommandBufferDescriptor commandBufferDescriptor = {};
        WGPUCommandBuffer commandBuffer =
            wgpuCommandEncoderFinish(encoder, &commandBufferDescriptor);
        wgpuCommandEncoderRelease(encoder);
        if (!commandBuffer) {
            encodingFailed = true;
            break;
        }

        uploadCommandBuffers.push_back(commandBuffer);
        submissionBuffers.push_back(commandBuffer);
        stats_.deferredUploadCommandBuffers++;
    }

    if (encodingFailed) {
        for (auto commandBuffer : uploadCommandBuffers) {
            wgpuCommandBufferRelease(commandBuffer);
        }
        fallbackFlush();
        return;
    }

    recordNativeSubmit(submissionBuffers);
    releaseDeferredCommandBuffers();
    for (auto commandBuffer : uploadCommandBuffers) {
        wgpuCommandBufferRelease(commandBuffer);
    }
    clearWork();
}

void FrameQueue::submit(std::vector<WGPUCommandBuffer>&& commandBuffers) {
    if (commandBuffers.empty()) return;
    if (!frameActive_) {
        recordNativeSubmit(commandBuffers);
        for (auto commandBuffer : commandBuffers) {
            wgpuCommandBufferRelease(commandBuffer);
        }
        return;
    }

    if (!work_.empty() && work_.back().kind == Work::Kind::CommandBuffers) {
        auto& destination = work_.back().commandBuffers;
        destination.insert(destination.end(), commandBuffers.begin(), commandBuffers.end());
        return;
    }

    Work work;
    work.kind = Work::Kind::CommandBuffers;
    work.commandBuffers = std::move(commandBuffers);
    work_.push_back(std::move(work));
}

void FrameQueue::flushBeforeImmediateOperation() {
    if (!frameActive_ || !hasPendingWork()) return;
    stats_.forcedFrameFlushes++;
    flush();
}

void FrameQueue::writeBuffer(WGPUBuffer buffer, uint64_t offset,
                             const void* data, size_t size) {
    if (!buffer || !queue_ || !data || size == 0) return;

    if (!frameActive_ || (offset & 3u) != 0 || (size & 3u) != 0) {
        flushBeforeImmediateOperation();
        wgpuQueueWriteBuffer(queue_, buffer, offset, data, size);
        return;
    }

    const uint64_t sourceOffset = alignToCopyOffset(uploadBytes_.size());
    uploadBytes_.resize(static_cast<size_t>(sourceOffset + size));
    std::memcpy(uploadBytes_.data() + sourceOffset, data, size);
    if (retainedUploadBuffers_.insert(buffer).second) {
        wgpuBufferAddRef(buffer);
    }

    if (work_.empty() || work_.back().kind != Work::Kind::BufferCopies) {
        Work work;
        work.kind = Work::Kind::BufferCopies;
        work_.push_back(std::move(work));
    }

    work_.back().bufferCopies.push_back(
        {buffer, offset, sourceOffset, static_cast<uint64_t>(size)});
}

} // namespace mystral::webgpu::bridge
