#include "webgpu/async_bridge.h"

namespace mystral::webgpu::bridge {

void AsyncBridge::configure(js::Engine* engine, WGPUInstance instance, WGPUDevice device) {
    engine_ = engine;
    instance_ = instance;
    device_ = device;
}

void AsyncBridge::detach() {
    engine_ = nullptr;
    instance_ = nullptr;
    device_ = nullptr;
}

AsyncBridge::PendingPromise* AsyncBridge::createPromise(js::JSValueHandle& promise) {
    auto factory = engine_->getGlobalProperty("__mystralCreateDeferred");
    auto deferred = engine_->call(factory, engine_->newUndefined(), {});
    if (!deferred.ptr) return nullptr;

    auto* pending = new PendingPromise{
        engine_,
        engine_->getProperty(deferred, "resolve"),
        engine_->getProperty(deferred, "reject"),
        session_,
        true,
    };
    promise = engine_->getProperty(deferred, "promise");
    pending->engine->protect(pending->resolve);
    pending->engine->protect(pending->reject);
    pendingPromises_.insert(pending);
    return pending;
}

js::JSValueHandle AsyncBridge::makeError(const std::string& message) {
    auto constructor = engine_->getGlobalProperty("Error");
    return engine_->call(
        constructor,
        engine_->newUndefined(),
        {engine_->newString(message.c_str())});
}

void AsyncBridge::settle(PendingPromise* pending, bool success,
                         js::JSValueHandle value, const std::string& error) {
    if (!pending) return;
    if (isCurrent(pending)) {
        auto* engine = pending->engine;
        if (success) {
            engine->call(pending->resolve, engine->newUndefined(), {value});
        } else {
            engine->call(
                pending->reject,
                engine->newUndefined(),
                {makeError(error.empty() ? "WebGPU async operation failed" : error)});
        }
        engine->unprotect(pending->resolve);
        engine->unprotect(pending->reject);
        pending->active = false;
    }
    pendingPromises_.erase(pending);
    delete pending;
}

bool AsyncBridge::isCurrent(const PendingPromise* pending) const {
    return pending && pending->active && pending->session == session_ &&
        pending->engine == engine_;
}

js::JSValueHandle AsyncBridge::rejectedPromise(const std::string& message) {
    js::JSValueHandle promise;
    auto* pending = createPromise(promise);
    if (!pending) {
        engine_->throwException(message.c_str());
        return engine_->newUndefined();
    }
    settle(pending, false, {}, message);
    return promise;
}

js::JSValueHandle AsyncBridge::resolvedPromise(js::JSValueHandle value) {
    js::JSValueHandle promise;
    auto* pending = createPromise(promise);
    if (!pending) return engine_->newUndefined();
    settle(pending, true, value);
    return promise;
}

void AsyncBridge::enqueue(std::function<void()> completion) {
    std::lock_guard<std::mutex> lock(completionMutex_);
    completions_.push_back(std::move(completion));
}

void AsyncBridge::process() {
    if (!pendingPromises_.empty()) {
        if (instance_) wgpuInstanceProcessEvents(instance_);
        if (device_) wgpuDeviceTick(device_);
    }

    std::deque<std::function<void()>> completions;
    {
        std::lock_guard<std::mutex> lock(completionMutex_);
        completions.swap(completions_);
    }
    for (auto& completion : completions) completion();
}

void AsyncBridge::abandonPendingPromises() {
    for (auto* pending : pendingPromises_) {
        if (!pending->active) continue;
        pending->engine->unprotect(pending->resolve);
        pending->engine->unprotect(pending->reject);
        pending->active = false;
    }
}

void AsyncBridge::invalidateSession() {
    abandonPendingPromises();
    session_++;
}

size_t AsyncBridge::queuedCount() const {
    std::lock_guard<std::mutex> lock(completionMutex_);
    return completions_.size();
}

} // namespace mystral::webgpu::bridge
