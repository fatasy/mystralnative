#include "mystral/workers/native_task.h"

#include <atomic>
#include <exception>
#include <utility>

namespace mystral::workers {
namespace {

std::atomic<uint64_t> nextMailboxGeneration{uint64_t{1} << 63};

}  // namespace

NativeTaskMailbox::NativeTaskMailbox(ActivityCallback activityCallback)
    : generation_(nextMailboxGeneration.fetch_add(1))
    , activityCallback_(std::move(activityCallback)) {}

NativeTaskMailbox::~NativeTaskMailbox() {
    close();
}

void NativeTaskMailbox::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        closed_ = true;
        activityCallback_ = {};
        completions_.clear();
    }
    async::getJobSystem().cancelGeneration(generation_);
}

std::vector<NativeTaskCompletion> NativeTaskMailbox::drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NativeTaskCompletion> result;
    result.swap(completions_);
    return result;
}

void NativeTaskMailbox::publish(NativeTaskCompletion completion) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    completions_.push_back(std::move(completion));
    if (activityCallback_) activityCallback_();
}

bool NativeTaskRegistry::registerTask(std::string name, NativeTaskHandler handler) {
    if (name.empty() || !handler) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.emplace(std::move(name), std::move(handler)).second;
}

bool NativeTaskRegistry::unregisterTask(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.erase(name) > 0;
}

uint64_t NativeTaskRegistry::submit(
    const std::string& name,
    std::string payload,
    const std::shared_ptr<NativeTaskMailbox>& mailbox,
    std::string* error) {
    if (!mailbox) {
        if (error) *error = "Native task mailbox is unavailable";
        return 0;
    }

    NativeTaskHandler handler;
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = handlers_.find(name);
        if (iterator == handlers_.end()) {
            if (error) *error = "Unknown native task: " + name;
            return 0;
        }
        handler = iterator->second;
        id = nextTaskId_++;
    }

    if (!async::getJobSystem().isRunning()) {
        if (error) *error = "Native task JobSystem is not running";
        return 0;
    }

    const std::weak_ptr<NativeTaskMailbox> weakMailbox = mailbox;
    const auto handle = async::getJobSystem().submit(
        async::JobPriority::FrameCritical,
        mailbox->generation(),
        [id, handler = std::move(handler), payload = std::move(payload), weakMailbox](
            const async::JobContext& context) mutable {
            NativeTaskResult result;
            if (context.isCancelled()) {
                result = NativeTaskResult::failure("Native task was cancelled");
            } else {
                try {
                    result = handler(payload, context);
                    if (context.isCancelled()) {
                        result = NativeTaskResult::failure("Native task was cancelled");
                    }
                } catch (const std::exception& exception) {
                    result = NativeTaskResult::failure(exception.what());
                } catch (...) {
                    result = NativeTaskResult::failure("Native task failed with an unknown exception");
                }
            }
            if (auto destination = weakMailbox.lock()) {
                destination->publish({id, std::move(result)});
            }
        });
    if (!handle) {
        if (error) *error = "Native task queue is full";
        return 0;
    }
    return id;
}

NativeTaskRegistry& getNativeTaskRegistry() {
    static NativeTaskRegistry registry;
    return registry;
}

bool installNativeTaskBindings(
    js::Engine* engine,
    const std::shared_ptr<NativeTaskMailbox>& mailbox) {
    if (!engine || !mailbox) return false;
    engine->setGlobalProperty("__mystralNativeTaskSubmit",
        engine->newFunction("__mystralNativeTaskSubmit",
            [engine, mailbox](void*, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2) {
                    engine->throwException("runNativeTask requires a task name and payload");
                    return engine->newNumber(0);
                }
                std::string error;
                const uint64_t id = getNativeTaskRegistry().submit(
                    engine->toString(args[0]),
                    engine->toString(args[1]),
                    mailbox,
                    &error);
                if (id == 0 && !error.empty()) engine->throwException(error.c_str());
                return engine->newNumber(static_cast<double>(id));
            }));
    return !engine->hasException();
}

bool dispatchNativeTaskCompletions(
    js::Engine* engine,
    const std::shared_ptr<NativeTaskMailbox>& mailbox,
    std::string* error) {
    if (!engine || !mailbox) return true;
    const auto dispatch = engine->getGlobalProperty("__mystralDispatchNativeTask");
    if (!engine->isFunction(dispatch)) {
        engine->releaseValue(dispatch);
        return true;
    }

    bool succeeded = true;
    for (const auto& completion : mailbox->drain()) {
        auto id = engine->newNumber(static_cast<double>(completion.id));
        auto ok = engine->newBoolean(completion.result.ok);
        auto payload = engine->newString(completion.result.payload.c_str());
        auto taskError = engine->newString(completion.result.error.c_str());
        auto thisArg = engine->newUndefined();
        auto result = engine->call(dispatch, thisArg, {id, ok, payload, taskError});
        engine->releaseValue(result);
        engine->releaseValue(thisArg);
        engine->releaseValue(taskError);
        engine->releaseValue(payload);
        engine->releaseValue(ok);
        engine->releaseValue(id);
        if (engine->hasException()) {
            const std::string currentError = engine->getException();
            if (error && error->empty()) *error = currentError;
            succeeded = false;
        }
    }
    engine->releaseValue(dispatch);
    return succeeded;
}

const char* nativeTaskApiSource() {
    return R"JS(
(function() {
    const pending = new Map();

    globalThis.__mystralDispatchNativeTask = function(id, ok, payload, error) {
        const request = pending.get(id);
        if (!request) return;
        pending.delete(id);
        if (ok) {
            request.resolve(__mystralParseMessage(payload, []));
        } else {
            request.reject(new Error(error || 'Native task failed'));
        }
    };

    function runNativeTask(name, data) {
        if (typeof name !== 'string' || !name) {
            return Promise.reject(new TypeError('Native task name is required'));
        }
        let prepared;
        try {
            prepared = __mystralPrepareMessage(data);
            if (prepared.transfers.length !== 0) {
                throw new TypeError('Native tasks accept shared handles and value data, not ArrayBuffer attachments');
            }
        } catch (error) {
            return Promise.reject(error);
        }
        return new Promise((resolve, reject) => {
            const id = __mystralNativeTaskSubmit(name, prepared.payload);
            pending.set(id, { resolve, reject });
        });
    }

    globalThis.__mystralNativeTasks = Object.freeze({ runNativeTask });
    if (typeof globalThis.runNativeTask === 'undefined') {
        globalThis.runNativeTask = runNativeTask;
    }
})();
)JS";
}

}  // namespace mystral::workers
