#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace mystral::async {

enum class JobPriority : uint8_t {
    FrameCritical = 0,
    Streaming = 1,
    Background = 2,
};

enum class JobStatus : uint8_t {
    Completed,
    Cancelled,
    Failed,
};

struct JobHandle {
    uint64_t id = 0;
    explicit operator bool() const { return id != 0; }
};

class JobContext {
public:
    bool isCancelled() const {
        return cancelled_ && cancelled_->load(std::memory_order_relaxed);
    }

private:
    friend class JobSystem;
    explicit JobContext(const std::atomic_bool* cancelled) : cancelled_(cancelled) {}
    const std::atomic_bool* cancelled_ = nullptr;
};

using JobWork = std::function<void(const JobContext&)>;
using JobCompletion = std::function<void(JobStatus)>;

struct JobSystemConfig {
    uint32_t workerCount = 0;
    size_t maxInFlightJobs = 1024;
};

struct JobLatencyHistogram {
    static constexpr size_t BucketCount = 32;
    uint64_t samples = 0;
    uint64_t totalNanoseconds = 0;
    std::array<uint64_t, BucketCount> buckets{};
};

struct JobLatencySummary {
    double minMs = 0.0;
    double meanMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
};

struct JobSystemStats {
    uint64_t submitted = 0;
    uint64_t completed = 0;
    uint64_t cancelled = 0;
    uint64_t failed = 0;
    uint64_t rejected = 0;
    uint64_t inFlight = 0;
    uint64_t queued = 0;
    uint64_t running = 0;
    uint64_t completionQueueDepth = 0;
    uint64_t queueHighWater = 0;
    uint64_t inFlightHighWater = 0;
    uint32_t workerCount = 0;
    JobLatencyHistogram queueWait;
    JobLatencyHistogram execution;
};

struct CpuBudgetStats {
    uint32_t limit = 1;
    uint32_t active = 0;
    uint32_t peakActive = 0;
};

class CpuBudgetLease {
public:
    CpuBudgetLease() = default;
    ~CpuBudgetLease();
    CpuBudgetLease(CpuBudgetLease&& other) noexcept;
    CpuBudgetLease& operator=(CpuBudgetLease&& other) noexcept;
    CpuBudgetLease(const CpuBudgetLease&) = delete;
    CpuBudgetLease& operator=(const CpuBudgetLease&) = delete;
    explicit operator bool() const { return held_; }

private:
    friend CpuBudgetLease acquireCpuBudget(const std::atomic_bool* cancelled);
    explicit CpuBudgetLease(bool held) : held_(held) {}
    bool held_ = false;
};

void configureCpuBudget(uint32_t limit);
CpuBudgetLease acquireCpuBudget(const std::atomic_bool* cancelled = nullptr);
void notifyCpuBudgetWaiters();
CpuBudgetStats cpuBudgetStats();

JobLatencySummary summarizeJobLatency(
    const JobLatencyHistogram& start,
    const JobLatencyHistogram& end);

class JobSystem {
public:
    JobSystem();
    ~JobSystem();

    bool start(const JobSystemConfig& config = {});
    void shutdown();
    bool isRunning() const;

    JobHandle submit(
        JobPriority priority,
        uint64_t generation,
        JobWork work,
        JobCompletion completion = {});

    size_t cancelGeneration(uint64_t generation);
    size_t processCompletions(size_t maxCount = static_cast<size_t>(-1));
    void waitIdle();
    JobSystemStats stats() const;
    void resetHighWaterMarks();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

JobSystem& getJobSystem();

}  // namespace mystral::async
