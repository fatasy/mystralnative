#pragma once

#include "mystral/js/engine.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>

#include <webgpu/webgpu.h>

namespace mystral::webgpu::bridge {

class AsyncBridge {
public:
    struct PendingPromise {
        js::Engine* engine = nullptr;
        js::JSValueHandle resolve;
        js::JSValueHandle reject;
        uint64_t session = 0;
        bool active = true;
    };

    void configure(js::Engine* engine, WGPUInstance instance, WGPUDevice device);
    void detach();

    PendingPromise* createPromise(js::JSValueHandle& promise);
    js::JSValueHandle rejectedPromise(const std::string& message);
    js::JSValueHandle resolvedPromise(js::JSValueHandle value);
    void settle(PendingPromise* pending, bool success,
                js::JSValueHandle value, const std::string& error = {});
    bool isCurrent(const PendingPromise* pending) const;

    void enqueue(std::function<void()> completion);
    size_t process(size_t maxCount = static_cast<size_t>(-1));
    void invalidateSession();

    size_t pendingCount() const { return pendingPromises_.size(); }
    size_t queuedCount() const;
    uint64_t session() const { return session_; }

private:
    js::JSValueHandle makeError(const std::string& message);
    void abandonPendingPromises();

    js::Engine* engine_ = nullptr;
    WGPUInstance instance_ = nullptr;
    WGPUDevice device_ = nullptr;
    mutable std::mutex completionMutex_;
    std::deque<std::function<void()>> completions_;
    std::unordered_set<PendingPromise*> pendingPromises_;
    uint64_t session_ = 1;
};

} // namespace mystral::webgpu::bridge
