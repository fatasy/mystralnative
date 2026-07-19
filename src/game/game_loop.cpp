#include "mystral/game/game_loop.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mystral::game {
namespace {

constexpr double kMinimumSimulationHz = 0.1;
constexpr double kMaximumSimulationHz = 1000.0;
constexpr uint32_t kMaximumCatchUpTicks = 1000;
constexpr double kMinimumFrameDeltaMs = 1.0;
constexpr double kMaximumFrameDeltaMs = 60'000.0;
constexpr double kMaximumTimeScale = 1000.0;

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

}  // namespace

bool GameLoop::validateConfig(const GameLoopConfig& config, std::string* error) {
    if (!std::isfinite(config.simulationHz) ||
        config.simulationHz < kMinimumSimulationHz ||
        config.simulationHz > kMaximumSimulationHz) {
        return fail(error, "simulationHz must be between 0.1 and 1000");
    }
    if (config.maxCatchUpTicks == 0 || config.maxCatchUpTicks > kMaximumCatchUpTicks) {
        return fail(error, "maxCatchUpTicks must be between 1 and 1000");
    }
    if (!std::isfinite(config.maxFrameDeltaMs) ||
        config.maxFrameDeltaMs < kMinimumFrameDeltaMs ||
        config.maxFrameDeltaMs > kMaximumFrameDeltaMs) {
        return fail(error, "maxFrameDeltaMs must be between 1 and 60000");
    }
    return true;
}

bool GameLoop::validateTimeScale(double timeScale, std::string* error) {
    if (!std::isfinite(timeScale) || timeScale <= 0.0 || timeScale > kMaximumTimeScale) {
        return fail(error, "timeScale must be greater than 0 and at most 1000");
    }
    return true;
}

bool GameLoop::configure(const GameLoopConfig& config, std::string* error) {
    if (!validateConfig(config, error)) return false;

    const double oldTickMs = 1000.0 / config_.simulationHz;
    const double interpolationAlpha = oldTickMs > 0.0
        ? std::clamp(accumulatorMs_ / oldTickMs, 0.0, 1.0)
        : 0.0;
    config_ = config;
    accumulatorMs_ = interpolationAlpha * (1000.0 / config_.simulationHz);
    return true;
}

bool GameLoop::setTimeScale(double timeScale, std::string* error) {
    if (!validateTimeScale(timeScale, error)) return false;
    timeScale_ = timeScale;
    return true;
}

void GameLoop::start(double nowMs) {
    running_ = true;
    paused_ = false;
    clockInitialized_ = std::isfinite(nowMs);
    lastNowMs_ = clockInitialized_ ? nowMs : 0.0;
    accumulatorMs_ = 0.0;
    simulationTimeMs_ = 0.0;
    tickCount_ = 0;
    pendingStepTicks_ = 0;
}

void GameLoop::stop() {
    running_ = false;
    paused_ = false;
    clockInitialized_ = false;
    accumulatorMs_ = 0.0;
    pendingStepTicks_ = 0;
}

void GameLoop::pause() {
    if (running_) paused_ = true;
}

void GameLoop::resume(double nowMs) {
    if (!running_) return;
    paused_ = false;
    rebaseClock(nowMs);
}

void GameLoop::rebaseClock(double nowMs) {
    if (!running_) return;
    clockInitialized_ = std::isfinite(nowMs);
    lastNowMs_ = clockInitialized_ ? nowMs : 0.0;
}

bool GameLoop::step(uint32_t count, std::string* error) {
    if (!running_) return fail(error, "game loop must be started before stepping");
    if (!paused_) return fail(error, "game loop must be paused before stepping");
    if (count == 0) return fail(error, "step count must be greater than 0");
    if (count > std::numeric_limits<uint32_t>::max() - pendingStepTicks_) {
        return fail(error, "too many pending step ticks");
    }
    pendingStepTicks_ += count;
    return true;
}

bool GameLoop::executeTick(const TickHandler& handler) {
    const double tickDeltaMs = 1000.0 / config_.simulationHz;
    GameTick tick;
    tick.tick = tickCount_;
    tick.deltaMs = tickDeltaMs;
    tick.simulationTimeMs = simulationTimeMs_ + tickDeltaMs;
    if (handler && !handler(tick)) {
        paused_ = true;
        accumulatorMs_ = 0.0;
        pendingStepTicks_ = 0;
        stats_.handlerFailures++;
        return false;
    }
    simulationTimeMs_ = tick.simulationTimeMs;
    tickCount_++;
    stats_.ticksExecuted++;
    return true;
}

GameLoopAdvanceResult GameLoop::advance(double nowMs, const TickHandler& handler) {
    GameLoopAdvanceResult result;
    if (!running_) return result;
    const bool advancingPausedSteps = paused_;

    if (!std::isfinite(nowMs)) nowMs = lastNowMs_;
    if (!clockInitialized_) {
        clockInitialized_ = true;
        lastNowMs_ = nowMs;
    }

    uint32_t ticksToExecute = 0;
    if (paused_) {
        lastNowMs_ = nowMs;
        ticksToExecute = (std::min)(pendingStepTicks_, config_.maxCatchUpTicks);
        pendingStepTicks_ -= ticksToExecute;
    } else {
        double frameDeltaMs = (std::max)(0.0, nowMs - lastNowMs_);
        lastNowMs_ = nowMs;
        if (frameDeltaMs > config_.maxFrameDeltaMs) {
            frameDeltaMs = config_.maxFrameDeltaMs;
            result.clockClamped = true;
            stats_.clockClampFrames++;
        }

        const double tickDeltaMs = 1000.0 / config_.simulationHz;
        accumulatorMs_ += frameDeltaMs * timeScale_;
        const auto availableTicks = static_cast<uint64_t>(accumulatorMs_ / tickDeltaMs);
        const uint64_t executableTicks = (std::min)(
            availableTicks, static_cast<uint64_t>(config_.maxCatchUpTicks));
        ticksToExecute = static_cast<uint32_t>(executableTicks);

        if (availableTicks > executableTicks) {
            result.ticksDropped = availableTicks - executableTicks;
            stats_.ticksDropped += result.ticksDropped;
            accumulatorMs_ -= static_cast<double>(result.ticksDropped) * tickDeltaMs;
        }
        accumulatorMs_ -= static_cast<double>(ticksToExecute) * tickDeltaMs;
        accumulatorMs_ = (std::max)(0.0, accumulatorMs_);
    }

    if (ticksToExecute > 1) stats_.catchUpFrames++;
    for (uint32_t index = 0; index < ticksToExecute; ++index) {
        if (!executeTick(handler)) {
            result.handlerFailed = true;
            break;
        }
        result.ticksExecuted++;
        if (!running_ || paused_ != advancingPausedSteps) break;
    }
    stats_.maxTicksPerFrame = (std::max)(stats_.maxTicksPerFrame, result.ticksExecuted);
    return result;
}

GameLoopState GameLoop::state() const {
    GameLoopState state;
    state.running = running_;
    state.paused = paused_;
    state.tickCount = tickCount_;
    state.simulationTimeMs = simulationTimeMs_;
    const double tickDeltaMs = 1000.0 / config_.simulationHz;
    state.interpolationAlpha = paused_ || tickDeltaMs <= 0.0
        ? 0.0
        : std::clamp(accumulatorMs_ / tickDeltaMs, 0.0, 1.0);
    state.timeScale = timeScale_;
    state.pendingStepTicks = pendingStepTicks_;
    return state;
}

}  // namespace mystral::game
