#pragma once

#include "mystral/async/job_system.h"
#include "mystral/js/engine.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mystral::workers {

struct NativeTaskResult {
    bool ok = true;
    std::string payload;
    std::string error;

    static NativeTaskResult success(std::string payloadValue) {
        return {true, std::move(payloadValue), {}};
    }

    static NativeTaskResult failure(std::string errorValue) {
        return {false, {}, std::move(errorValue)};
    }
};

using NativeTaskHandler = std::function<NativeTaskResult(
    std::string_view,
    const async::JobContext&)>;

struct NativeTaskCompletion {
    uint64_t id = 0;
    NativeTaskResult result;
};

class NativeTaskMailbox {
public:
    using ActivityCallback = std::function<void()>;

    explicit NativeTaskMailbox(ActivityCallback activityCallback = {});
    ~NativeTaskMailbox();

    uint64_t generation() const { return generation_; }
    void close();
    std::vector<NativeTaskCompletion> drain();

private:
    friend class NativeTaskRegistry;
    void publish(NativeTaskCompletion completion);

    uint64_t generation_ = 0;
    ActivityCallback activityCallback_;
    std::vector<NativeTaskCompletion> completions_;
    bool closed_ = false;
    std::mutex mutex_;
};

class NativeTaskRegistry {
public:
    bool registerTask(std::string name, NativeTaskHandler handler);
    bool unregisterTask(const std::string& name);
    uint64_t submit(
        const std::string& name,
        std::string payload,
        const std::shared_ptr<NativeTaskMailbox>& mailbox,
        std::string* error = nullptr);

private:
    std::unordered_map<std::string, NativeTaskHandler> handlers_;
    uint64_t nextTaskId_ = 1;
    std::mutex mutex_;
};

NativeTaskRegistry& getNativeTaskRegistry();

bool installNativeTaskBindings(
    js::Engine* engine,
    const std::shared_ptr<NativeTaskMailbox>& mailbox);
bool dispatchNativeTaskCompletions(
    js::Engine* engine,
    const std::shared_ptr<NativeTaskMailbox>& mailbox,
    std::string* error = nullptr);
const char* nativeTaskApiSource();

}  // namespace mystral::workers
