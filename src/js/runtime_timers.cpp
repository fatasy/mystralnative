#include "js/runtime_timers.h"

#include "mystral/async/event_loop.h"

#include <iostream>
#include <utility>

namespace mystral::js {

void RuntimeTimers::install(Engine* engine) {
    engine_ = engine;
    setupTimers();
}

void RuntimeTimers::finishShutdown() {
#ifdef MYSTRAL_TIMERS_USE_LIBUV
    uvTimers_.clear();
#endif
    cancelledTimerIds_.clear();
    engine_ = nullptr;
}

void RuntimeTimers::clear() {
#ifdef MYSTRAL_TIMERS_USE_LIBUV
        // Stop and clean up all libuv timers
        for (auto& [id, ctx] : uvTimers_) {
            if (ctx) {
                ctx->cancelled = true;
                uv_timer_stop(&ctx->handle);
                if (ctx->callbackProtected) {
                    engine_->unprotect(ctx->callback);
                    ctx->callbackProtected = false;
                }
                if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&ctx->handle))) {
                    uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle), onTimerClose);
                }
            }
        }
        // Note: Don't clear uvTimers_ here - onTimerClose will do that
        cancelledTimerIds_.clear();
        {
            std::lock_guard<std::mutex> lock(timerMutex_);
            while (!pendingTimerCallbacks_.empty()) {
                pendingTimerCallbacks_.pop();
            }
        }
#else
        // Clear std::chrono-based timers
        for (auto& timer : timerCallbacks_) {
            if (!timer.cancelled) {
                engine_->unprotect(timer.callback);
            }
        }
        timerCallbacks_.clear();
        cancelledTimerIds_.clear();
#endif
        // IDs remain monotonic: libuv close callbacks erase old timers on a
        // later loop turn, so reusing IDs here could erase a new timer.
    }

bool RuntimeTimers::hasActive() const {
#ifdef MYSTRAL_TIMERS_USE_LIBUV
        for (const auto& [id, ctx] : uvTimers_) {
            if (ctx && !ctx->cancelled) {
                return true;
            }
        }
        return false;
#else
        for (const auto& timer : timerCallbacks_) {
            if (!timer.cancelled) {
                return true;
            }
        }
        return false;
#endif
    }

void RuntimeTimers::setupTimers() {
        if (!engine_) return;

#ifdef MYSTRAL_TIMERS_USE_LIBUV
        // libuv-based timers for precise timing
        setupLibuvTimers();
#else
        // Fallback to std::chrono-based timers
        setupChronoTimers();
#endif
    }

#ifdef MYSTRAL_TIMERS_USE_LIBUV
    // libuv timer callback - queues the JS callback for main thread processing
void RuntimeTimers::onUvTimerCallback(uv_timer_t* handle) {
        auto* ctx = static_cast<UvTimerContext*>(handle->data);
        if (!ctx || ctx->cancelled) return;

        // Queue the callback for processing on the main thread
        {
            std::lock_guard<std::mutex> lock(ctx->runtime->timerMutex_);
            ctx->runtime->pendingTimerCallbacks_.push({
                ctx->id,
                ctx->callback,
                ctx->intervalMs
            });
        }

        // For setTimeout (intervalMs == 0), mark as cancelled so we don't fire again
        if (ctx->intervalMs == 0) {
            ctx->cancelled = true;
        }
    }

    // Close callback for timer handles
void RuntimeTimers::onTimerClose(uv_handle_t* handle) {
        auto* ctx = static_cast<UvTimerContext*>(handle->data);
        if (ctx && ctx->runtime) {
            // Now it's safe to remove from the map - handle is fully closed
            ctx->runtime->uvTimers_.erase(ctx->id);
        }
    }

int RuntimeTimers::createUvTimer(js::JSValueHandle callback, int delayMs, int intervalMs) {
        uv_loop_t* loop = async::EventLoop::instance().handle();
        if (!loop) {
            std::cerr << "[Timer] EventLoop not available" << std::endl;
            return -1;
        }

        int id = nextTimerId_++;
        engine_->protect(callback);

        auto ctx = std::make_unique<UvTimerContext>();
        ctx->id = id;
        ctx->callback = callback;
        ctx->intervalMs = intervalMs;
        ctx->cancelled = false;
        ctx->callbackProtected = true;
        ctx->runtime = this;
        ctx->handle.data = ctx.get();

        int result = uv_timer_init(loop, &ctx->handle);
        if (result != 0) {
            std::cerr << "[Timer] Failed to init timer: " << uv_strerror(result) << std::endl;
            engine_->unprotect(callback);
            return -1;
        }

        // Start the timer
        // For setInterval, use repeat; for setTimeout, use 0 repeat
        uint64_t repeat = (intervalMs > 0) ? (uint64_t)intervalMs : 0;
        result = uv_timer_start(&ctx->handle, onUvTimerCallback, (uint64_t)delayMs, repeat);
        if (result != 0) {
            std::cerr << "[Timer] Failed to start timer: " << uv_strerror(result) << std::endl;
            uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle), nullptr);
            engine_->unprotect(callback);
            return -1;
        }

        uvTimers_[id] = std::move(ctx);
        return id;
    }

void RuntimeTimers::cancelUvTimer(int id) {
        auto it = uvTimers_.find(id);
        if (it == uvTimers_.end()) return;

        auto& ctx = it->second;
        if (ctx && !ctx->cancelled) {
            ctx->cancelled = true;
            cancelledTimerIds_.insert(id);
            uv_timer_stop(&ctx->handle);
            if (ctx->callbackProtected) {
                engine_->unprotect(ctx->callback);
                ctx->callbackProtected = false;
            }
            uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle), onTimerClose);
        }
    }

void RuntimeTimers::setupLibuvTimers() {
        // setTimeout
        engine_->setGlobalProperty("setTimeout",
            engine_->newFunction("setTimeout", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return engine_->newNumber(-1);
                }

                int delay = 0;
                if (args.size() > 1) {
                    delay = (int)engine_->toNumber(args[1]);
                }
                if (delay < 0) delay = 0;

                int id = createUvTimer(args[0], delay, 0);
                return engine_->newNumber(id);
            })
        );

        // clearTimeout
        engine_->setGlobalProperty("clearTimeout",
            engine_->newFunction("clearTimeout", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return engine_->newUndefined();
                }

                int id = (int)engine_->toNumber(args[0]);
                cancelUvTimer(id);
                return engine_->newUndefined();
            })
        );

        // setInterval
        engine_->setGlobalProperty("setInterval",
            engine_->newFunction("setInterval", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return engine_->newNumber(-1);
                }

                int delay = 0;
                if (args.size() > 1) {
                    delay = (int)engine_->toNumber(args[1]);
                }
                if (delay < 1) delay = 1;  // Minimum 1ms for intervals

                int id = createUvTimer(args[0], delay, delay);
                return engine_->newNumber(id);
            })
        );

        // clearInterval
        engine_->setGlobalProperty("clearInterval",
            engine_->newFunction("clearInterval", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return engine_->newUndefined();
                }

                int id = (int)engine_->toNumber(args[0]);
                cancelUvTimer(id);
                return engine_->newUndefined();
            })
        );
    }
#endif // MYSTRAL_TIMERS_USE_LIBUV

#ifndef MYSTRAL_TIMERS_USE_LIBUV
void RuntimeTimers::setupChronoTimers() {
        // setTimeout (fallback using std::chrono)
        engine_->setGlobalProperty("setTimeout",
            engine_->newFunction("setTimeout", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return engine_->newNumber(-1);
                }

                int delay = 0;
                if (args.size() > 1) {
                    delay = (int)engine_->toNumber(args[1]);
                }

                int id = nextTimerId_++;
                auto callback = args[0];
                engine_->protect(callback);

                auto targetTime = std::chrono::high_resolution_clock::now() +
                                  std::chrono::milliseconds(delay);

                timerCallbacks_.push_back({id, callback, targetTime, 0, false});

                return engine_->newNumber(id);
            })
        );

        // clearTimeout (fallback)
        engine_->setGlobalProperty("clearTimeout",
            engine_->newFunction("clearTimeout", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return engine_->newUndefined();
                }

                int id = (int)engine_->toNumber(args[0]);

                for (auto& timer : timerCallbacks_) {
                    if (timer.id == id && !timer.cancelled) {
                        timer.cancelled = true;
                        engine_->unprotect(timer.callback);
                        break;
                    }
                }

                return engine_->newUndefined();
            })
        );

        // setInterval (fallback)
        engine_->setGlobalProperty("setInterval",
            engine_->newFunction("setInterval", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return engine_->newNumber(-1);
                }

                int delay = 0;
                if (args.size() > 1) {
                    delay = (int)engine_->toNumber(args[1]);
                }
                if (delay < 1) delay = 1;

                int id = nextTimerId_++;
                auto callback = args[0];
                engine_->protect(callback);

                auto targetTime = std::chrono::high_resolution_clock::now() +
                                  std::chrono::milliseconds(delay);

                timerCallbacks_.push_back({id, callback, targetTime, delay, false});

                return engine_->newNumber(id);
            })
        );

        // clearInterval (fallback)
        engine_->setGlobalProperty("clearInterval",
            engine_->newFunction("clearInterval", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return engine_->newUndefined();
                }

                int id = (int)engine_->toNumber(args[0]);
                cancelledTimerIds_.insert(id);

                for (auto& timer : timerCallbacks_) {
                    if (timer.id == id && !timer.cancelled) {
                        timer.cancelled = true;
                        engine_->unprotect(timer.callback);
                        break;
                    }
                }

                return engine_->newUndefined();
            })
        );
    }
#endif // !MYSTRAL_TIMERS_USE_LIBUV

size_t RuntimeTimers::executeCallbacks(size_t maxCount) {
#ifdef MYSTRAL_TIMERS_USE_LIBUV
        // Process pending timer callbacks from libuv
        std::queue<PendingTimerCallback> toProcess;
        {
            std::lock_guard<std::mutex> lock(timerMutex_);
            size_t count = 0;
            while (!pendingTimerCallbacks_.empty() && count++ < maxCount) {
                toProcess.push(std::move(pendingTimerCallbacks_.front()));
                pendingTimerCallbacks_.pop();
            }
        }

        size_t executed = 0;
        while (!toProcess.empty()) {
            auto pending = std::move(toProcess.front());
            toProcess.pop();

            // Check if cancelled while waiting in queue
            if (cancelledTimerIds_.count(pending.id) > 0) {
                cancelledTimerIds_.erase(pending.id);
                continue;
            }

            // Call the callback
            std::vector<js::JSValueHandle> args;
            engine_->call(pending.callback, engine_->newUndefined(), args);
            ++executed;

            // For setTimeout (intervalMs == 0), clean up the timer
            if (pending.intervalMs == 0) {
                auto it = uvTimers_.find(pending.id);
                if (it != uvTimers_.end()) {
                    if (it->second->callbackProtected) {
                        engine_->unprotect(it->second->callback);
                        it->second->callbackProtected = false;
                    }
                    // uv_close is async - onTimerClose will erase from map when done
                    uv_close(reinterpret_cast<uv_handle_t*>(&it->second->handle), onTimerClose);
                }
            }
            // For setInterval, libuv automatically repeats - nothing to do
        }
        return executed;
#else
        // Fallback: std::chrono-based timer processing
        if (timerCallbacks_.empty()) return 0;

        auto now = std::chrono::high_resolution_clock::now();

        // Process timers - collect expired ones
        std::vector<TimerCallback> toExecute;
        std::vector<TimerCallback> remaining;

        for (auto& timer : timerCallbacks_) {
            if (timer.cancelled) {
                continue;  // Skip cancelled timers
            }

            if (now >= timer.targetTime && toExecute.size() < maxCount) {
                toExecute.push_back(timer);
            } else {
                remaining.push_back(timer);
            }
        }
        const size_t executed = toExecute.size();

        timerCallbacks_ = std::move(remaining);

        // Execute expired timers
        for (auto& timer : toExecute) {
            // Call the callback
            std::vector<js::JSValueHandle> args;
            engine_->call(timer.callback, engine_->newUndefined(), args);

            if (timer.intervalMs > 0) {
                // Check if interval was cancelled during callback execution
                bool wasCancelled = false;
                for (const auto& t : timerCallbacks_) {
                    if (t.id == timer.id && t.cancelled) {
                        wasCancelled = true;
                        break;
                    }
                }
                // Also check cancelledTimerIds_ set
                if (cancelledTimerIds_.count(timer.id) > 0) {
                    wasCancelled = true;
                }

                if (!wasCancelled) {
                    // Re-schedule interval
                    timer.targetTime = now + std::chrono::milliseconds(timer.intervalMs);
                    timerCallbacks_.push_back(timer);
                } else {
                    engine_->unprotect(timer.callback);
                    cancelledTimerIds_.erase(timer.id);
                }
            } else {
                // setTimeout - unprotect and done
                engine_->unprotect(timer.callback);
            }
        }
        return executed;
#endif
    }

}  // namespace mystral::js
