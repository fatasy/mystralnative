#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace mystral::webgpu::bridge {

enum class Operation : size_t {
    CreateCommandEncoder,
    BeginRenderPass,
    BeginComputePass,
    FinishCommandEncoder,
    QueueSubmit,
    NativeQueueSubmit,
    QueueWriteBuffer,
    QueueWriteBufferBatch,
    QueueWriteTexture,
    RenderSetPipeline,
    RenderSetBindGroup,
    RenderSetVertexBuffer,
    RenderSetIndexBuffer,
    RenderDraw,
    RenderDrawIndexed,
    RenderDrawIndirect,
    RenderDrawIndexedIndirect,
    RenderExecuteBundles,
    RenderEnd,
    ComputeSetPipeline,
    ComputeSetBindGroup,
    ComputeDispatchWorkgroups,
    ComputeEnd,
    Count,
};

struct Metric {
    uint64_t calls = 0;
    uint64_t totalNanoseconds = 0;
    uint64_t bytes = 0;
};

class ScopedMeasurement {
public:
    explicit ScopedMeasurement(Operation operation);
    ~ScopedMeasurement();

    ScopedMeasurement(const ScopedMeasurement&) = delete;
    ScopedMeasurement& operator=(const ScopedMeasurement&) = delete;

private:
    Operation operation_;
    std::chrono::steady_clock::time_point startedAt_;
};

void record(Operation operation, uint64_t nanoseconds);
void recordBytes(Operation operation, uint64_t bytes);

const std::array<Metric, static_cast<size_t>(Operation::Count)>& metrics();
const std::array<const char*, static_cast<size_t>(Operation::Count)>& names();

} // namespace mystral::webgpu::bridge
