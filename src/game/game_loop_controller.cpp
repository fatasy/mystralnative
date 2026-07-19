#include "game/game_loop_controller.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace mystral::game {

double GameLoopController::monotonicNowMs() {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
}

void GameLoopController::reset() {
    if (engine_ && tickHandler_.ptr) engine_->unprotect(tickHandler_);
    tickHandler_ = {};
    loop_ = GameLoop{};
    engine_ = nullptr;
}

void GameLoopController::install(js::Engine* engine) {
    reset();
    engine_ = engine;
    if (!engine_) return;

    auto api = engine_->newObject();
    engine_->setProperty(api, "configure",
        engine_->newFunction("configure", [this](void*, const std::vector<js::JSValueHandle>& args) {
            GameLoopConfig config = loop_.config();
            if (!args.empty() && !engine_->isUndefined(args[0]) && !engine_->isNull(args[0])) {
                if (!engine_->isObject(args[0])) {
                    engine_->throwException("gameLoop.configure(options) requires an object");
                    return engine_->newUndefined();
                }

                const auto readNumber = [this, &args](const char* name, double& target) {
                    auto value = engine_->getProperty(args[0], name);
                    if (!engine_->isUndefined(value)) target = engine_->toNumber(value);
                };
                readNumber("simulationHz", config.simulationHz);
                readNumber("maxFrameDeltaMs", config.maxFrameDeltaMs);

                auto maxCatchUp = engine_->getProperty(args[0], "maxCatchUpTicks");
                if (!engine_->isUndefined(maxCatchUp)) {
                    const double value = engine_->toNumber(maxCatchUp);
                    if (!std::isfinite(value) || value < 0.0 || std::floor(value) != value ||
                        value > static_cast<double>((std::numeric_limits<uint32_t>::max)())) {
                        engine_->throwException("maxCatchUpTicks must be a positive integer");
                        return engine_->newUndefined();
                    }
                    config.maxCatchUpTicks = static_cast<uint32_t>(value);
                }
            }

            std::string error;
            if (!loop_.configure(config, &error)) engine_->throwException(error.c_str());
            return engine_->newUndefined();
        }));

    engine_->setProperty(api, "setTickHandler",
        engine_->newFunction("setTickHandler", [this](void*, const std::vector<js::JSValueHandle>& args) {
            if (args.empty() || engine_->isUndefined(args[0]) || engine_->isNull(args[0])) {
                if (tickHandler_.ptr) engine_->unprotect(tickHandler_);
                tickHandler_ = {};
                loop_.stop();
                return engine_->newUndefined();
            }
            if (!engine_->isFunction(args[0])) {
                engine_->throwException("gameLoop.setTickHandler(handler) requires a function or null");
                return engine_->newUndefined();
            }
            if (tickHandler_.ptr) engine_->unprotect(tickHandler_);
            tickHandler_ = args[0];
            engine_->protect(tickHandler_);
            return engine_->newUndefined();
        }));

    engine_->setProperty(api, "start",
        engine_->newFunction("start", [this](void*, const std::vector<js::JSValueHandle>&) {
            if (!tickHandler_.ptr) {
                engine_->throwException("gameLoop.start() requires a tick handler");
                return engine_->newUndefined();
            }
            loop_.start(monotonicNowMs());
            return engine_->newUndefined();
        }));
    engine_->setProperty(api, "stop",
        engine_->newFunction("stop", [this](void*, const std::vector<js::JSValueHandle>&) {
            loop_.stop();
            return engine_->newUndefined();
        }));
    engine_->setProperty(api, "pause",
        engine_->newFunction("pause", [this](void*, const std::vector<js::JSValueHandle>&) {
            loop_.pause();
            return engine_->newUndefined();
        }));
    engine_->setProperty(api, "resume",
        engine_->newFunction("resume", [this](void*, const std::vector<js::JSValueHandle>&) {
            loop_.resume(monotonicNowMs());
            return engine_->newUndefined();
        }));
    engine_->setProperty(api, "step",
        engine_->newFunction("step", [this](void*, const std::vector<js::JSValueHandle>& args) {
            const double countValue = args.empty() ? 1.0 : engine_->toNumber(args[0]);
            if (!std::isfinite(countValue) || countValue <= 0.0 ||
                std::floor(countValue) != countValue ||
                countValue > static_cast<double>((std::numeric_limits<uint32_t>::max)())) {
                engine_->throwException("gameLoop.step(count) requires a positive integer");
                return engine_->newUndefined();
            }
            std::string error;
            if (!loop_.step(static_cast<uint32_t>(countValue), &error)) {
                engine_->throwException(error.c_str());
            }
            return engine_->newUndefined();
        }));
    engine_->setProperty(api, "setTimeScale",
        engine_->newFunction("setTimeScale", [this](void*, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                engine_->throwException("gameLoop.setTimeScale(scale) requires a number");
                return engine_->newUndefined();
            }
            std::string error;
            if (!loop_.setTimeScale(engine_->toNumber(args[0]), &error)) {
                engine_->throwException(error.c_str());
            }
            return engine_->newUndefined();
        }));
    engine_->setProperty(api, "getState",
        engine_->newFunction("getState", [this](void*, const std::vector<js::JSValueHandle>&) {
            const auto state = loop_.state();
            const auto& config = loop_.config();
            auto result = engine_->newObject();
            engine_->setProperty(result, "running", engine_->newBoolean(state.running));
            engine_->setProperty(result, "paused", engine_->newBoolean(state.paused));
            engine_->setProperty(result, "tickCount", engine_->newNumber(static_cast<double>(state.tickCount)));
            engine_->setProperty(result, "simulationTimeMs", engine_->newNumber(state.simulationTimeMs));
            engine_->setProperty(result, "interpolationAlpha", engine_->newNumber(state.interpolationAlpha));
            engine_->setProperty(result, "timeScale", engine_->newNumber(state.timeScale));
            engine_->setProperty(result, "pendingStepTicks", engine_->newNumber(state.pendingStepTicks));
            engine_->setProperty(result, "simulationHz", engine_->newNumber(config.simulationHz));
            engine_->setProperty(result, "tickDeltaMs", engine_->newNumber(1000.0 / config.simulationHz));
            engine_->setProperty(result, "maxCatchUpTicks", engine_->newNumber(config.maxCatchUpTicks));
            engine_->setProperty(result, "maxFrameDeltaMs", engine_->newNumber(config.maxFrameDeltaMs));
            return result;
        }));
    engine_->setProperty(api, "getStats",
        engine_->newFunction("getStats", [this](void*, const std::vector<js::JSValueHandle>&) {
            const auto& stats = loop_.stats();
            auto result = engine_->newObject();
            engine_->setProperty(result, "ticksExecuted", engine_->newNumber(static_cast<double>(stats.ticksExecuted)));
            engine_->setProperty(result, "ticksDropped", engine_->newNumber(static_cast<double>(stats.ticksDropped)));
            engine_->setProperty(result, "catchUpFrames", engine_->newNumber(static_cast<double>(stats.catchUpFrames)));
            engine_->setProperty(result, "clockClampFrames", engine_->newNumber(static_cast<double>(stats.clockClampFrames)));
            engine_->setProperty(result, "handlerFailures", engine_->newNumber(static_cast<double>(stats.handlerFailures)));
            engine_->setProperty(result, "maxTicksPerFrame", engine_->newNumber(stats.maxTicksPerFrame));
            return result;
        }));

    engine_->setGlobalProperty("__mystralGameLoop", api);
    engine_->setGlobalProperty("gameLoop", api);
}

void GameLoopController::advance() {
    if (!engine_ || !loop_.state().running || !tickHandler_.ptr) return;
    const auto result = loop_.advance(monotonicNowMs(), [this](const GameTick& tick) {
        auto tickObject = engine_->newObject();
        engine_->setProperty(tickObject, "tick", engine_->newNumber(static_cast<double>(tick.tick)));
        engine_->setProperty(tickObject, "deltaMs", engine_->newNumber(tick.deltaMs));
        engine_->setProperty(tickObject, "deltaSeconds", engine_->newNumber(tick.deltaMs / 1000.0));
        engine_->setProperty(tickObject, "simulationTimeMs", engine_->newNumber(tick.simulationTimeMs));
        auto callbackResult = engine_->call(tickHandler_, engine_->newUndefined(), {tickObject});
        if (engine_->hasException()) {
            std::cerr << "[GameLoop] Tick handler failed: " << engine_->getException() << std::endl;
            return false;
        }
        if (callbackResult.ptr && engine_->isObject(callbackResult)) {
            auto then = engine_->getProperty(callbackResult, "then");
            if (engine_->isFunction(then)) {
                std::cerr << "[GameLoop] Tick handler returned a Promise; ticks must be synchronous. "
                          << "Post work to a Worker without awaiting it inside the handler." << std::endl;
                return false;
            }
        }
        return true;
    });
    if (result.handlerFailed) {
        std::cerr << "[GameLoop] Scheduler paused after a tick handler failure" << std::endl;
    }
}

void GameLoopController::rebaseClock() {
    loop_.rebaseClock(monotonicNowMs());
}

}  // namespace mystral::game
