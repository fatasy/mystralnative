#include "webgpu/bridge_profiler.h"

#include <atomic>

namespace mystral::webgpu::bridge {

namespace {

constexpr std::array<const char*, static_cast<size_t>(Operation::Count)> kNames = {
    "createCommandEncoder",
    "beginRenderPass",
    "beginComputePass",
    "finishCommandEncoder",
    "queueSubmit",
    "nativeQueueSubmit",
    "queueWriteBuffer",
    "queueWriteBufferBatch",
    "queueWriteTexture",
    "renderSetPipeline",
    "renderSetBindGroup",
    "renderSetVertexBuffer",
    "renderSetIndexBuffer",
    "renderDraw",
    "renderDrawIndexed",
    "renderDrawIndirect",
    "renderDrawIndexedIndirect",
    "renderExecuteBundles",
    "renderEnd",
    "computeSetPipeline",
    "computeSetBindGroup",
    "computeDispatchWorkgroups",
    "computeEnd",
};

std::array<Metric, static_cast<size_t>(Operation::Count)> g_metrics;
std::atomic<bool> g_enabled{false};

} // namespace

ScopedMeasurement::ScopedMeasurement(Operation operation)
    : operation_(operation), enabled_(g_enabled.load(std::memory_order_relaxed)) {
    if (enabled_) startedAt_ = std::chrono::steady_clock::now();
}

ScopedMeasurement::~ScopedMeasurement() {
    if (!enabled_) return;
    const auto elapsed = std::chrono::steady_clock::now() - startedAt_;
    record(operation_, static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
}

void setEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
}

bool isEnabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void reset() {
    g_metrics.fill({});
}

void record(Operation operation, uint64_t nanoseconds) {
    if (!isEnabled()) return;
    auto& metric = g_metrics[static_cast<size_t>(operation)];
    metric.calls++;
    metric.totalNanoseconds += nanoseconds;
}

void recordBytes(Operation operation, uint64_t bytes) {
    if (!isEnabled()) return;
    g_metrics[static_cast<size_t>(operation)].bytes += bytes;
}

const std::array<Metric, static_cast<size_t>(Operation::Count)>& metrics() {
    return g_metrics;
}

const std::array<const char*, static_cast<size_t>(Operation::Count)>& names() {
    return kNames;
}

} // namespace mystral::webgpu::bridge
