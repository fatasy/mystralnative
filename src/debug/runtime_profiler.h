#pragma once

#include "mystral/runtime.h"
#include "mystral/async/job_system.h"
#include "mystral/game/game_loop.h"
#include "mystral/workers/worker_registry.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace mystral::debug {

struct RuntimeProfilerSnapshot {
    RuntimeMemorySnapshot memory;
    workers::WorkerRegistryStats workers;
    uint64_t sharedMemoryBytes = 0;
    async::JobSystemStats jobs;
    game::GameLoopStats gameLoop;
};

class RuntimeProfiler {
public:
    struct FrameSample {
        double frameMs = 0.0;
        double eventsMs = 0.0;
        double asyncWorkMs = 0.0;
        double callbacksMs = 0.0;
        double simulationMs = 0.0;
        double animationFrameMs = 0.0;
        double cleanupMs = 0.0;
    };

    void start(uint64_t expectedFrames, RuntimeProfilerSnapshot snapshot);
    RuntimeProfileReport stop(RuntimeProfilerSnapshot snapshot);

    bool active() const { return active_; }
    bool shouldSampleFrame() const;
    void addFrameSample(FrameSample sample);

    template <typename Clock, typename DurationA, typename DurationB>
    static double millisecondsBetween(
        const std::chrono::time_point<Clock, DurationA>& start,
        const std::chrono::time_point<Clock, DurationB>& end) {
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

private:
    static RuntimeProfileStatistics summarizeValues(std::vector<double>& values);

    bool active_ = false;
    uint64_t expectedFrames_ = 0;
    std::chrono::steady_clock::time_point startedAt_;
    RuntimeProfilerSnapshot startSnapshot_;
    std::vector<FrameSample> samples_;
};

} // namespace mystral::debug
