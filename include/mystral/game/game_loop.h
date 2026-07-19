#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace mystral::game {

struct GameLoopConfig {
    double simulationHz = 60.0;
    uint32_t maxCatchUpTicks = 5;
    double maxFrameDeltaMs = 250.0;
};

struct GameTick {
    uint64_t tick = 0;
    double deltaMs = 0.0;
    double simulationTimeMs = 0.0;
};

struct GameLoopState {
    bool running = false;
    bool paused = false;
    uint64_t tickCount = 0;
    double simulationTimeMs = 0.0;
    double interpolationAlpha = 0.0;
    double timeScale = 1.0;
    uint32_t pendingStepTicks = 0;
};

struct GameLoopStats {
    uint64_t ticksExecuted = 0;
    uint64_t ticksDropped = 0;
    uint64_t catchUpFrames = 0;
    uint64_t clockClampFrames = 0;
    uint64_t handlerFailures = 0;
    uint32_t maxTicksPerFrame = 0;
};

struct GameLoopAdvanceResult {
    uint32_t ticksExecuted = 0;
    uint64_t ticksDropped = 0;
    bool clockClamped = false;
    bool handlerFailed = false;
};

class GameLoop {
public:
    using TickHandler = std::function<bool(const GameTick&)>;

    static bool validateConfig(const GameLoopConfig& config, std::string* error = nullptr);
    static bool validateTimeScale(double timeScale, std::string* error = nullptr);

    bool configure(const GameLoopConfig& config, std::string* error = nullptr);
    bool setTimeScale(double timeScale, std::string* error = nullptr);

    void start(double nowMs);
    void stop();
    void pause();
    void resume(double nowMs);
    void rebaseClock(double nowMs);
    bool step(uint32_t count, std::string* error = nullptr);

    GameLoopAdvanceResult advance(double nowMs, const TickHandler& handler);

    const GameLoopConfig& config() const { return config_; }
    GameLoopState state() const;
    const GameLoopStats& stats() const { return stats_; }
    void resetHighWaterMarks() { stats_.maxTicksPerFrame = 0; }

private:
    bool executeTick(const TickHandler& handler);

    GameLoopConfig config_;
    GameLoopStats stats_;
    bool running_ = false;
    bool paused_ = false;
    bool clockInitialized_ = false;
    double lastNowMs_ = 0.0;
    double accumulatorMs_ = 0.0;
    double simulationTimeMs_ = 0.0;
    double timeScale_ = 1.0;
    uint64_t tickCount_ = 0;
    uint32_t pendingStepTicks_ = 0;
};

}  // namespace mystral::game
