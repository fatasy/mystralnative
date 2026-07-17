#include "mystral/async/job_system.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

bool waitUntil(const std::atomic_bool& value) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!value.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

}  // namespace

int main() {
    using namespace mystral::async;
    const std::thread::id mainThread = std::this_thread::get_id();

    {
        JobSystem jobs;
        jobs.start({1, 8});
        std::atomic_bool blockerStarted{false};
        std::atomic_bool releaseBlocker{false};
        std::vector<std::string> completionOrder;
        bool completionsOnMainThread = true;

        jobs.submit(JobPriority::FrameCritical, 1,
            [&](const JobContext&) {
                blockerStarted.store(true, std::memory_order_release);
                while (!releaseBlocker.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            },
            [&](JobStatus status) {
                completionsOnMainThread &= std::this_thread::get_id() == mainThread;
                if (status == JobStatus::Completed) completionOrder.push_back("blocker");
            });
        if (!waitUntil(blockerStarted)) return 1;

        jobs.submit(JobPriority::Background, 1, [](const JobContext&) {},
            [&](JobStatus) { completionOrder.push_back("background"); });
        jobs.submit(JobPriority::Streaming, 1, [](const JobContext&) {},
            [&](JobStatus) { completionOrder.push_back("streaming"); });
        jobs.submit(JobPriority::FrameCritical, 1, [](const JobContext&) {},
            [&](JobStatus) { completionOrder.push_back("critical"); });

        releaseBlocker.store(true, std::memory_order_release);
        jobs.waitIdle();
        jobs.processCompletions();

        const std::vector<std::string> expected = {
            "blocker", "critical", "streaming", "background"};
        if (!completionsOnMainThread || completionOrder != expected) {
            std::cerr << "Job priority or completion-thread validation failed" << std::endl;
            return 2;
        }
        const auto stats = jobs.stats();
        if (stats.completed != 4 || stats.queueWait.samples != 4 ||
            stats.execution.samples != 4 || stats.inFlight != 0) {
            std::cerr << "Job metrics validation failed" << std::endl;
            return 3;
        }
        jobs.resetHighWaterMarks();
        if (jobs.stats().queueHighWater != 0 || jobs.stats().inFlightHighWater != 0) {
            std::cerr << "Job high-water reset validation failed" << std::endl;
            return 3;
        }
        jobs.shutdown();
    }

    {
        JobSystem jobs;
        jobs.start({1, 2});
        std::atomic_bool blockerStarted{false};
        std::atomic_bool releaseBlocker{false};
        jobs.submit(JobPriority::Streaming, 2,
            [&](const JobContext&) {
                blockerStarted.store(true, std::memory_order_release);
                while (!releaseBlocker.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            });
        if (!waitUntil(blockerStarted)) return 4;
        if (!jobs.submit(JobPriority::Streaming, 2, [](const JobContext&) {})) return 5;
        if (jobs.submit(JobPriority::Streaming, 2, [](const JobContext&) {})) {
            std::cerr << "Job backpressure validation failed" << std::endl;
            return 6;
        }
        releaseBlocker.store(true, std::memory_order_release);
        jobs.waitIdle();
        jobs.processCompletions();
        if (jobs.stats().rejected != 1) return 7;
        jobs.shutdown();
    }

    {
        JobSystem jobs;
        jobs.start({1, 4});
        std::atomic_bool runningStarted{false};
        std::atomic_bool queuedRan{false};
        std::vector<JobStatus> statuses;
        jobs.submit(JobPriority::Streaming, 42,
            [&](const JobContext& context) {
                runningStarted.store(true, std::memory_order_release);
                while (!context.isCancelled()) std::this_thread::yield();
            },
            [&](JobStatus status) { statuses.push_back(status); });
        if (!waitUntil(runningStarted)) return 8;
        jobs.submit(JobPriority::Background, 42,
            [&](const JobContext&) { queuedRan.store(true, std::memory_order_release); },
            [&](JobStatus status) { statuses.push_back(status); });

        if (jobs.cancelGeneration(42) != 1) return 9;
        jobs.waitIdle();
        jobs.processCompletions();
        if (queuedRan.load(std::memory_order_acquire) || statuses.size() != 2 ||
            statuses[0] != JobStatus::Cancelled || statuses[1] != JobStatus::Cancelled ||
            jobs.stats().cancelled != 2) {
            std::cerr << "Job cancellation validation failed" << std::endl;
            return 10;
        }
        jobs.shutdown();
    }

    std::cout << "Job system smoke test passed" << std::endl;
    return 0;
}
