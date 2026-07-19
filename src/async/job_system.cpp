#include "mystral/async/job_system.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mystral::async {
namespace {

using Clock = std::chrono::steady_clock;

uint32_t defaultCpuBudgetLimit() {
    const uint32_t hardwareThreads = (std::max)(1u, std::thread::hardware_concurrency());
    return (std::max)(1u, hardwareThreads - 1);
}

struct CpuBudgetState {
    std::mutex mutex;
    std::condition_variable available;
    uint32_t limit = defaultCpuBudgetLimit();
    uint32_t active = 0;
    uint32_t peakActive = 0;
};

CpuBudgetState& cpuBudgetState() {
    static CpuBudgetState state;
    return state;
}

void releaseCpuBudget() {
    auto& state = cpuBudgetState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.active > 0) state.active--;
    }
    state.available.notify_one();
}

size_t priorityIndex(JobPriority priority) {
    return static_cast<size_t>(priority);
}

void recordLatency(JobLatencyHistogram& histogram, uint64_t nanoseconds) {
    histogram.samples++;
    histogram.totalNanoseconds += nanoseconds;

    uint64_t microseconds = std::max<uint64_t>(1, (nanoseconds + 999) / 1000);
    uint64_t upperBound = 1;
    size_t bucket = 0;
    while (upperBound < microseconds && bucket + 1 < JobLatencyHistogram::BucketCount) {
        upperBound <<= 1;
        bucket++;
    }
    histogram.buckets[bucket]++;
}

double bucketUpperBoundMs(size_t bucket) {
    return static_cast<double>(uint64_t{1} << bucket) / 1000.0;
}

uint64_t counterDelta(uint64_t start, uint64_t end) {
    return end >= start ? end - start : 0;
}

}  // namespace

CpuBudgetLease::~CpuBudgetLease() {
    if (held_) releaseCpuBudget();
}

CpuBudgetLease::CpuBudgetLease(CpuBudgetLease&& other) noexcept
    : held_(other.held_) {
    other.held_ = false;
}

CpuBudgetLease& CpuBudgetLease::operator=(CpuBudgetLease&& other) noexcept {
    if (this == &other) return *this;
    if (held_) releaseCpuBudget();
    held_ = other.held_;
    other.held_ = false;
    return *this;
}

void configureCpuBudget(uint32_t limit) {
    auto& state = cpuBudgetState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.limit = std::max(1u, limit);
        state.peakActive = state.active;
    }
    state.available.notify_all();
}

CpuBudgetLease acquireCpuBudget(const std::atomic_bool* cancelled) {
    auto& state = cpuBudgetState();
    std::unique_lock<std::mutex> lock(state.mutex);
    state.available.wait(lock, [&state, cancelled] {
        return state.active < state.limit ||
            (cancelled && cancelled->load(std::memory_order_relaxed));
    });
    if (cancelled && cancelled->load(std::memory_order_relaxed)) return {};
    state.active++;
    state.peakActive = std::max(state.peakActive, state.active);
    return CpuBudgetLease(true);
}

void notifyCpuBudgetWaiters() {
    cpuBudgetState().available.notify_all();
}

CpuBudgetStats cpuBudgetStats() {
    auto& state = cpuBudgetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return {state.limit, state.active, state.peakActive};
}

JobLatencySummary summarizeJobLatency(
    const JobLatencyHistogram& start,
    const JobLatencyHistogram& end) {
    JobLatencySummary summary;
    const uint64_t samples = counterDelta(start.samples, end.samples);
    if (samples == 0) return summary;

    const uint64_t totalNanoseconds =
        counterDelta(start.totalNanoseconds, end.totalNanoseconds);
    summary.meanMs = static_cast<double>(totalNanoseconds) /
        static_cast<double>(samples) / 1'000'000.0;

    std::array<uint64_t, JobLatencyHistogram::BucketCount> buckets{};
    size_t firstBucket = 0;
    size_t lastBucket = 0;
    bool foundSample = false;
    for (size_t index = 0; index < buckets.size(); index++) {
        buckets[index] = counterDelta(start.buckets[index], end.buckets[index]);
        if (buckets[index] > 0) {
            if (!foundSample) firstBucket = index;
            lastBucket = index;
            foundSample = true;
        }
    }

    auto percentile = [&buckets, samples](uint64_t numerator, uint64_t denominator) {
        const uint64_t target = (samples * numerator + denominator - 1) / denominator;
        uint64_t cumulative = 0;
        for (size_t index = 0; index < buckets.size(); index++) {
            cumulative += buckets[index];
            if (cumulative >= target) return bucketUpperBoundMs(index);
        }
        return bucketUpperBoundMs(buckets.size() - 1);
    };

    summary.minMs = firstBucket == 0 ? 0.0 : bucketUpperBoundMs(firstBucket - 1);
    summary.p50Ms = percentile(50, 100);
    summary.p95Ms = percentile(95, 100);
    summary.p99Ms = percentile(99, 100);
    summary.maxMs = bucketUpperBoundMs(lastBucket);
    return summary;
}

struct JobSystem::Impl {
    struct QueuedJob {
        uint64_t id = 0;
        uint64_t generation = 0;
        Clock::time_point submittedAt;
        std::shared_ptr<std::atomic_bool> cancelToken;
        JobWork work;
        JobCompletion completion;
    };

    struct CompletedJob {
        JobStatus status = JobStatus::Completed;
        JobCompletion completion;
    };

    mutable std::mutex mutex;
    std::condition_variable workAvailable;
    std::condition_variable idle;
    std::array<std::deque<QueuedJob>, 3> queues;
    std::deque<CompletedJob> completions;
    std::vector<std::thread> workers;
    std::unordered_map<uint64_t, std::shared_ptr<std::atomic_bool>> jobTokens;
    std::unordered_map<uint64_t, std::shared_ptr<std::atomic_bool>> generationTokens;
    std::unordered_set<uint64_t> cancelledGenerations;
    JobSystemStats counters;
    size_t maxInFlightJobs = 1024;
    uint64_t nextJobId = 1;
    bool running = false;
    bool accepting = false;
    bool stopping = false;

    bool hasQueuedJobs() const {
        return counters.queued > 0;
    }

    QueuedJob popNextJob() {
        for (auto& queue : queues) {
            if (!queue.empty()) {
                QueuedJob job = std::move(queue.front());
                queue.pop_front();
                counters.queued--;
                return job;
            }
        }
        return {};
    }

    void queueCompletion(QueuedJob&& job, JobStatus status) {
        jobTokens.erase(job.id);
        if (job.completion) {
            completions.push_back({status, std::move(job.completion)});
        } else {
            counters.inFlight--;
        }
    }

    void runWorker() {
        for (;;) {
            QueuedJob job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                workAvailable.wait(lock, [this] { return stopping || hasQueuedJobs(); });
                if (stopping && !hasQueuedJobs()) return;
                job = popNextJob();
                counters.running++;
            }

            auto cpuLease = acquireCpuBudget(job.cancelToken.get());
            const auto startedAt = Clock::now();
            JobStatus status = JobStatus::Completed;
            bool executed = false;
            if (job.cancelToken->load(std::memory_order_relaxed)) {
                status = JobStatus::Cancelled;
            } else {
                try {
                    executed = true;
                    JobContext context(job.cancelToken.get());
                    job.work(context);
                    if (context.isCancelled()) status = JobStatus::Cancelled;
                } catch (...) {
                    status = JobStatus::Failed;
                }
            }
            const auto finishedAt = Clock::now();

            const uint64_t waitNanoseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    startedAt - job.submittedAt).count());
            const uint64_t executionNanoseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    finishedAt - startedAt).count());

            // Publish the job as idle only after its global execution slot is
            // available to Workers or other native jobs.
            cpuLease = {};

            {
                std::lock_guard<std::mutex> lock(mutex);
                counters.running--;
                recordLatency(counters.queueWait, waitNanoseconds);
                if (executed) recordLatency(counters.execution, executionNanoseconds);
                if (status == JobStatus::Completed) counters.completed++;
                else if (status == JobStatus::Cancelled) counters.cancelled++;
                else counters.failed++;
                queueCompletion(std::move(job), status);
                if (!hasQueuedJobs() && counters.running == 0) idle.notify_all();
            }
        }
    }
};

JobSystem::JobSystem() : impl_(std::make_unique<Impl>()) {}

JobSystem::~JobSystem() {
    shutdown();
}

bool JobSystem::start(const JobSystemConfig& config) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->running) return true;

    const uint32_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t workerCount = config.workerCount > 0
        ? config.workerCount
        : std::max(1u, hardwareThreads - 1);

    impl_->maxInFlightJobs = std::max<size_t>(workerCount, config.maxInFlightJobs);
    impl_->nextJobId = 1;
    impl_->queues = {};
    impl_->completions.clear();
    impl_->jobTokens.clear();
    impl_->generationTokens.clear();
    impl_->cancelledGenerations.clear();
    impl_->counters = {};
    impl_->counters.workerCount = workerCount;
    impl_->running = true;
    impl_->accepting = true;
    impl_->stopping = false;
    impl_->workers.reserve(workerCount);
    for (uint32_t index = 0; index < workerCount; index++) {
        impl_->workers.emplace_back([this] { impl_->runWorker(); });
    }
    return true;
}

void JobSystem::shutdown() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running) return;
        impl_->accepting = false;
        impl_->stopping = true;
        for (auto& entry : impl_->jobTokens) {
            entry.second->store(true, std::memory_order_relaxed);
        }
        for (auto& queue : impl_->queues) {
            while (!queue.empty()) {
                auto job = std::move(queue.front());
                queue.pop_front();
                impl_->counters.queued--;
                impl_->counters.cancelled++;
                impl_->queueCompletion(std::move(job), JobStatus::Cancelled);
            }
        }
    }
    impl_->workAvailable.notify_all();
    notifyCpuBudgetWaiters();
    for (auto& worker : impl_->workers) {
        if (worker.joinable()) worker.join();
    }
    impl_->workers.clear();

    processCompletions();

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->jobTokens.clear();
    impl_->generationTokens.clear();
    impl_->cancelledGenerations.clear();
    impl_->running = false;
    impl_->stopping = false;
    impl_->counters.workerCount = 0;
}

bool JobSystem::isRunning() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->running;
}

JobHandle JobSystem::submit(
    JobPriority priority,
    uint64_t generation,
    JobWork work,
    JobCompletion completion) {
    if (!work) return {};

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->accepting || impl_->counters.inFlight >= impl_->maxInFlightJobs ||
        (generation != 0 && impl_->cancelledGenerations.contains(generation))) {
        impl_->counters.rejected++;
        return {};
    }

    std::shared_ptr<std::atomic_bool> token;
    if (generation == 0) {
        token = std::make_shared<std::atomic_bool>(false);
    } else {
        auto& generationToken = impl_->generationTokens[generation];
        if (!generationToken) generationToken = std::make_shared<std::atomic_bool>(false);
        token = generationToken;
    }

    const uint64_t id = impl_->nextJobId++;
    impl_->jobTokens.emplace(id, token);
    auto& queue = impl_->queues[priorityIndex(priority)];
    queue.push_back({
        id,
        generation,
        Clock::now(),
        std::move(token),
        std::move(work),
        std::move(completion),
    });
    impl_->counters.submitted++;
    impl_->counters.inFlight++;
    impl_->counters.queued++;
    impl_->counters.queueHighWater =
        std::max(impl_->counters.queueHighWater, impl_->counters.queued);
    impl_->counters.inFlightHighWater =
        std::max(impl_->counters.inFlightHighWater, impl_->counters.inFlight);
    impl_->workAvailable.notify_one();
    return {id};
}

size_t JobSystem::cancelGeneration(uint64_t generation) {
    if (generation == 0) return 0;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cancelledGenerations.insert(generation);
    auto token = impl_->generationTokens.find(generation);
    if (token != impl_->generationTokens.end()) {
        token->second->store(true, std::memory_order_relaxed);
    }

    size_t cancelled = 0;
    for (auto& queue : impl_->queues) {
        for (auto iterator = queue.begin(); iterator != queue.end();) {
            if (iterator->generation != generation) {
                ++iterator;
                continue;
            }
            auto job = std::move(*iterator);
            iterator = queue.erase(iterator);
            impl_->counters.queued--;
            impl_->counters.cancelled++;
            impl_->queueCompletion(std::move(job), JobStatus::Cancelled);
            cancelled++;
        }
    }
    if (!impl_->hasQueuedJobs() && impl_->counters.running == 0) impl_->idle.notify_all();
    notifyCpuBudgetWaiters();
    return cancelled;
}

size_t JobSystem::processCompletions(size_t maxCount) {
    std::deque<Impl::CompletedJob> ready;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const size_t count = std::min(maxCount, impl_->completions.size());
        for (size_t index = 0; index < count; index++) {
            ready.push_back(std::move(impl_->completions.front()));
            impl_->completions.pop_front();
            impl_->counters.inFlight--;
        }
    }

    for (auto& completed : ready) {
        try {
            completed.completion(completed.status);
        } catch (...) {
            // Completion callbacks are isolated so one failed consumer cannot
            // prevent the remaining main-thread completions from running.
        }
    }
    return ready.size();
}

void JobSystem::waitIdle() {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->idle.wait(lock, [this] {
        return !impl_->hasQueuedJobs() && impl_->counters.running == 0;
    });
}

JobSystemStats JobSystem::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    JobSystemStats result = impl_->counters;
    result.completionQueueDepth = impl_->completions.size();
    return result;
}

void JobSystem::resetHighWaterMarks() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->counters.queueHighWater = impl_->counters.queued;
    impl_->counters.inFlightHighWater = impl_->counters.inFlight;
}

JobSystem& getJobSystem() {
    static JobSystem instance;
    return instance;
}

}  // namespace mystral::async
