#include "debug/runtime_profiler.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace mystral::debug {

namespace {

uint64_t counterDelta(uint64_t start, uint64_t end) {
    return end >= start ? end - start : 0;
}

RuntimeProfileStatistics copyJobLatency(const async::JobLatencySummary& source) {
    RuntimeProfileStatistics result;
    result.minMs = source.minMs;
    result.meanMs = source.meanMs;
    result.p50Ms = source.p50Ms;
    result.p95Ms = source.p95Ms;
    result.p99Ms = source.p99Ms;
    result.maxMs = source.maxMs;
    return result;
}

} // namespace

void RuntimeProfiler::start(uint64_t expectedFrames, RuntimeProfilerSnapshot snapshot) {
    samples_.clear();
    expectedFrames_ = expectedFrames;
    samples_.reserve(expectedFrames > 0 ? static_cast<size_t>(expectedFrames) : 1024);
    startSnapshot_ = std::move(snapshot);
    startedAt_ = std::chrono::steady_clock::now();
    active_ = true;
}

bool RuntimeProfiler::shouldSampleFrame() const {
    return active_ && (expectedFrames_ == 0 || samples_.size() < expectedFrames_);
}

void RuntimeProfiler::addFrameSample(FrameSample sample) {
    if (active_ && (expectedFrames_ == 0 || samples_.size() < expectedFrames_)) {
        samples_.push_back(std::move(sample));
    }
}

RuntimeProfileReport RuntimeProfiler::stop(RuntimeProfilerSnapshot endSnapshot) {
    RuntimeProfileReport report;
    if (!active_) {
        return report;
    }

    const auto stoppedAt = std::chrono::steady_clock::now();
    active_ = false;

    report.sampledFrames = samples_.size();
    report.wallTimeMs = millisecondsBetween(startedAt_, stoppedAt);
    report.memoryStart = startSnapshot_.memory;
    report.memoryEnd = endSnapshot.memory;

    const auto& workerStart = startSnapshot_.workers;
    const auto& workerEnd = endSnapshot.workers;
    report.workersActiveStart = workerStart.activeWorkers;
    report.workersActiveEnd = workerEnd.activeWorkers;
    report.workersCreated = counterDelta(workerStart.createdWorkers, workerEnd.createdWorkers);
    report.nestedWorkersCreated = counterDelta(
        workerStart.nestedCreatedWorkers, workerEnd.nestedCreatedWorkers);
    report.workerMessagesProcessed = counterDelta(
        workerStart.processedMessages, workerEnd.processedMessages);
    report.workerTimerCallbacks = counterDelta(
        workerStart.processedTimerCallbacks, workerEnd.processedTimerCallbacks);
    const uint64_t rejectedAtStart =
        workerStart.rejectedInputMessages + workerStart.rejectedOutputMessages;
    const uint64_t rejectedAtEnd =
        workerEnd.rejectedInputMessages + workerEnd.rejectedOutputMessages;
    report.workerMessagesRejected = counterDelta(rejectedAtStart, rejectedAtEnd);
    report.workerInputMessagesTooLarge = counterDelta(
        workerStart.rejectedInputTooLarge, workerEnd.rejectedInputTooLarge);
    report.workerInputQueueFull = counterDelta(
        workerStart.rejectedInputQueueFull, workerEnd.rejectedInputQueueFull);
    report.workerOutputMessagesTooLarge = counterDelta(
        workerStart.rejectedOutputTooLarge, workerEnd.rejectedOutputTooLarge);
    report.workerOutputQueueFull = counterDelta(
        workerStart.rejectedOutputQueueFull, workerEnd.rejectedOutputQueueFull);
    report.workerBusyTimeMs = static_cast<double>(counterDelta(
        workerStart.busyNanoseconds, workerEnd.busyNanoseconds)) / 1'000'000.0;
    report.workerInputQueueEndBytes = workerEnd.queuedInputBytes;
    report.workerOutputQueueEndBytes = workerEnd.queuedOutputBytes;
    report.workerInputQueuePeakBytes = workerEnd.peakQueuedInputBytes;
    report.workerOutputQueuePeakBytes = workerEnd.peakQueuedOutputBytes;
    report.workerLargestInputMessageBytes = workerEnd.largestInputMessageBytes;
    report.workerLargestOutputMessageBytes = workerEnd.largestOutputMessageBytes;
    report.workerMaxDepth = workerEnd.maxDepth;
    report.sharedMemoryStartBytes = startSnapshot_.sharedMemoryBytes;
    report.sharedMemoryEndBytes = endSnapshot.sharedMemoryBytes;

    const auto& jobStart = startSnapshot_.jobs;
    const auto& jobEnd = endSnapshot.jobs;
    report.jobsSubmitted = counterDelta(jobStart.submitted, jobEnd.submitted);
    report.jobsCompleted = counterDelta(jobStart.completed, jobEnd.completed);
    report.jobsCancelled = counterDelta(jobStart.cancelled, jobEnd.cancelled);
    report.jobsFailed = counterDelta(jobStart.failed, jobEnd.failed);
    report.jobsRejected = counterDelta(jobStart.rejected, jobEnd.rejected);
    report.jobQueueEnd = jobEnd.queued;
    report.jobQueuePeak = jobEnd.queueHighWater;
    report.jobInFlightEnd = jobEnd.inFlight;
    report.jobInFlightPeak = jobEnd.inFlightHighWater;
    report.jobWorkerCount = jobEnd.workerCount;
    const auto cpuBudget = async::cpuBudgetStats();
    report.cpuBudgetThreads = cpuBudget.limit;
    report.cpuBudgetActiveEnd = cpuBudget.active;
    report.cpuBudgetPeakActive = cpuBudget.peakActive;
    report.jobQueueWait = copyJobLatency(async::summarizeJobLatency(
        jobStart.queueWait, jobEnd.queueWait));
    report.jobExecution = copyJobLatency(async::summarizeJobLatency(
        jobStart.execution, jobEnd.execution));

    const auto& gameLoopStart = startSnapshot_.gameLoop;
    const auto& gameLoopEnd = endSnapshot.gameLoop;
    report.gameTicksExecuted = counterDelta(
        gameLoopStart.ticksExecuted, gameLoopEnd.ticksExecuted);
    report.gameTicksDropped = counterDelta(
        gameLoopStart.ticksDropped, gameLoopEnd.ticksDropped);
    report.gameCatchUpFrames = counterDelta(
        gameLoopStart.catchUpFrames, gameLoopEnd.catchUpFrames);
    report.gameClockClampFrames = counterDelta(
        gameLoopStart.clockClampFrames, gameLoopEnd.clockClampFrames);
    report.gameTickHandlerFailures = counterDelta(
        gameLoopStart.handlerFailures, gameLoopEnd.handlerFailures);
    report.gameMaxTicksPerFrame = gameLoopEnd.maxTicksPerFrame;

    std::vector<double> values;
    values.reserve(samples_.size());
    auto summarize = [this, &values](auto member) {
        values.clear();
        for (const auto& sample : samples_) {
            values.push_back(sample.*member);
        }
        return summarizeValues(values);
    };

    report.frame = summarize(&FrameSample::frameMs);
    report.events = summarize(&FrameSample::eventsMs);
    report.asyncWork = summarize(&FrameSample::asyncWorkMs);
    report.callbacks = summarize(&FrameSample::callbacksMs);
    report.simulation = summarize(&FrameSample::simulationMs);
    report.animationFrame = summarize(&FrameSample::animationFrameMs);
    report.cleanup = summarize(&FrameSample::cleanupMs);

    for (const auto& sample : samples_) {
        if (sample.frameMs > 8.333333) report.framesOver8_33Ms++;
        if (sample.frameMs > 16.666667) report.framesOver16_67Ms++;
        if (sample.frameMs > 33.333333) report.framesOver33_33Ms++;
    }

    return report;
}

RuntimeProfileStatistics RuntimeProfiler::summarizeValues(std::vector<double>& values) {
    RuntimeProfileStatistics stats;
    if (values.empty()) {
        return stats;
    }

    std::sort(values.begin(), values.end());
    const double total = std::accumulate(values.begin(), values.end(), 0.0);
    auto percentile = [&values](double fraction) {
        const size_t rank = static_cast<size_t>(std::ceil(fraction * values.size()));
        return values[std::max<size_t>(1, rank) - 1];
    };

    stats.minMs = values.front();
    stats.meanMs = total / static_cast<double>(values.size());
    stats.p50Ms = percentile(0.50);
    stats.p95Ms = percentile(0.95);
    stats.p99Ms = percentile(0.99);
    stats.maxMs = values.back();
    return stats;
}

} // namespace mystral::debug
