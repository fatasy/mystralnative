#include "js/animation_frame.h"

#include <chrono>
#include <utility>

namespace mystral::js {

void AnimationFrameScheduler::reset() {
    if (engine_) {
        for (auto& callback : callbacks_) engine_->unprotect(callback.function);
    }
    callbacks_.clear();
    engine_ = nullptr;
    nextId_ = 1;
}

void AnimationFrameScheduler::install(Engine* engine) {
    reset();
    engine_ = engine;
    if (!engine_) return;

    engine_->setGlobalProperty("requestAnimationFrame",
        engine_->newFunction("requestAnimationFrame", [this](void*, const std::vector<JSValueHandle>& args) {
            if (args.empty()) return engine_->newNumber(-1);
            const int id = nextId_++;
            engine_->protect(args[0]);
            callbacks_.push_back({id, args[0]});
            return engine_->newNumber(id);
        }));

    engine_->setGlobalProperty("cancelAnimationFrame",
        engine_->newFunction("cancelAnimationFrame", [this](void*, const std::vector<JSValueHandle>& args) {
            if (args.empty()) return engine_->newUndefined();
            const int id = static_cast<int>(engine_->toNumber(args[0]));
            for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
                if (it->id != id) continue;
                engine_->unprotect(it->function);
                callbacks_.erase(it);
                break;
            }
            return engine_->newUndefined();
        }));
}

void AnimationFrameScheduler::execute() {
    if (!engine_ || callbacks_.empty()) return;

    const auto now = std::chrono::high_resolution_clock::now();
    const double timestamp =
        std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
    auto callbacks = std::move(callbacks_);
    callbacks_.clear();
    for (auto& callback : callbacks) {
        engine_->call(
            callback.function,
            engine_->newUndefined(),
            {engine_->newNumber(timestamp)});
        engine_->unprotect(callback.function);
    }
}

}  // namespace mystral::js
