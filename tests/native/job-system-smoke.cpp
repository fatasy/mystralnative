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

    {
        configureCpuBudget(2);
        JobSystem jobs;
        jobs.start({4, 16});
        std::atomic<int> active{0};
        std::atomic<int> peak{0};
        for (int index = 0; index < 8; index++) {
            jobs.submit(JobPriority::FrameCritical, 0,
                [&](const JobContext&) {
                    const int current = active.fetch_add(1) + 1;
                    int observed = peak.load();
                    while (current > observed && !peak.compare_exchange_weak(observed, current)) {}
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    active.fetch_sub(1);
                });
        }
        jobs.waitIdle();
        const auto budget = cpuBudgetStats();
        jobs.shutdown();
        if (peak.load() != 2 || budget.limit != 2 || budget.peakActive > 2 ||
            budget.active != 0) {
            std::cerr << "Global CPU execution budget validation failed" << std::endl;
            return 11;
        }
    }

    {
        configureCpuBudget(1);
        auto heldBudget = acquireCpuBudget();
        JobSystem jobs;
        jobs.start({1, 4});
        std::atomic_bool jobRan{false};
        JobStatus completionStatus = JobStatus::Completed;
        jobs.submit(JobPriority::FrameCritical, 0,
            [&](const JobContext&) { jobRan.store(true, std::memory_order_release); },
            [&](JobStatus status) { completionStatus = status; });

        const auto runningDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (jobs.stats().running != 1 &&
            std::chrono::steady_clock::now() < runningDeadline) {
            std::this_thread::yield();
        }
        const bool waitingOnBudget = jobs.stats().running == 1;
        const auto shutdownStarted = std::chrono::steady_clock::now();
        jobs.shutdown();
        const auto shutdownElapsed = std::chrono::steady_clock::now() - shutdownStarted;
        if (!waitingOnBudget || jobRan.load(std::memory_order_acquire) ||
            completionStatus != JobStatus::Cancelled ||
            shutdownElapsed > std::chrono::seconds(1)) {
            std::cerr << "CPU-budget waiter did not cancel during shutdown" << std::endl;
            return 12;
        }
    }

    std::cout << "Job system smoke test passed" << std::endl;
    return 0;
}
