#include "mystral/workers/worker_thread.h"

#include "mystral/js/module_system.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

namespace mystral::workers {

namespace {

thread_local js::Engine* currentWorkerEngine = nullptr;
thread_local WorkerThread* currentWorker = nullptr;

size_t transferBytes(const std::vector<js::TransferredArrayBuffer>& transfers) {
    size_t total = 0;
    for (const auto& transfer : transfers) total += transfer.size;
    return total;
}

size_t messageBytes(const std::string& payload,
                    const std::vector<js::TransferredArrayBuffer>& transfers) {
    return payload.size() + transferBytes(transfers);
}

bool takeTransferList(js::Engine* engine,
                      js::JSValueHandle value,
                      std::vector<js::TransferredArrayBuffer>& result) {
    if (!value.ptr || engine->isUndefined(value) || engine->isNull(value)) return true;
    if (!engine->isArray(value)) return false;
    const auto lengthValue = engine->getProperty(value, "length");
    const auto length = static_cast<uint32_t>(engine->toNumber(lengthValue));
    result.reserve(length);
    for (uint32_t index = 0; index < length; index++) {
        auto item = engine->getPropertyIndex(value, index);
        js::TransferredArrayBuffer transferred;
        if (!engine->transferArrayBuffer(item, transferred)) return false;
        result.push_back(std::move(transferred));
    }
    return true;
}

std::string readTextFile(const std::string& rootDir, const std::string& requestedPath) {
    std::string normalized = requestedPath;
    if (normalized.rfind("file://", 0) == 0) normalized.erase(0, 7);
    std::filesystem::path path(normalized);
    if (!path.is_absolute()) path = std::filesystem::path(rootDir) / path;
    path = std::filesystem::absolute(path).lexically_normal();

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {};
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

}  // namespace

WorkerThread::WorkerThread(int id,
                           js::EngineType engineType,
                           WorkerSourceKind sourceKind,
                           std::string source,
                           std::string rootDir,
                           std::string name,
                           std::shared_ptr<SharedBufferRegistry> sharedBuffers,
                           WorkerQueueLimits queueLimits)
    : id_(id)
    , engineType_(engineType)
    , sourceKind_(sourceKind)
    , source_(std::move(source))
    , rootDir_(std::move(rootDir))
    , name_(std::move(name))
    , sharedBuffers_(std::move(sharedBuffers))
    , queueLimits_(queueLimits) {}

WorkerThread::~WorkerThread() {
    terminate();
}

void WorkerThread::start() {
    WorkerState expected = WorkerState::Created;
    if (!state_.compare_exchange_strong(expected, WorkerState::Starting)) return;
    terminated_ = false;
    try {
        thread_ = std::thread(&WorkerThread::threadMain, this);
    } catch (const std::exception& error) {
        state_ = WorkerState::Failed;
        enqueueOutput(WorkerMessage::Type::Error, error.what());
        enqueueOutput(WorkerMessage::Type::Exited, {});
    }
}

WorkerPostStatus WorkerThread::postMessage(
    std::string payload,
    std::vector<js::TransferredArrayBuffer> transfers) {
    const WorkerState currentState = state_.load();
    if (terminated_.load() || (currentState != WorkerState::Starting && currentState != WorkerState::Running)) {
        return WorkerPostStatus::NotRunning;
    }
    const size_t byteSize = messageBytes(payload, transfers);
    if (byteSize > queueLimits_.maxBytes) {
        rejectedInputMessages_.fetch_add(1);
        return WorkerPostStatus::MessageTooLarge;
    }
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        const WorkerState lockedState = state_.load();
        if (terminated_.load() ||
            (lockedState != WorkerState::Starting && lockedState != WorkerState::Running)) {
            return WorkerPostStatus::NotRunning;
        }
        if (inputQueue_.size() >= queueLimits_.maxMessages ||
            byteSize > queueLimits_.maxBytes - inputQueuedBytes_) {
            rejectedInputMessages_.fetch_add(1);
            return WorkerPostStatus::QueueFull;
        }
        inputQueuedBytes_ += byteSize;
        inputQueue_.push({std::move(payload), std::move(transfers)});
        peakInputQueuedBytes_ = std::max(peakInputQueuedBytes_, inputQueuedBytes_);
    }
    inputCondition_.notify_one();
    return WorkerPostStatus::Posted;
}

void WorkerThread::terminate() {
    terminated_ = true;
    WorkerState currentState = state_.load();
    while (currentState != WorkerState::Stopped && currentState != WorkerState::Failed &&
           !state_.compare_exchange_weak(currentState, WorkerState::Stopping)) {}
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        if (engine_) engine_->requestTermination();
    }
    inputCondition_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
    if (state_.load() != WorkerState::Failed) state_ = WorkerState::Stopped;
}

bool WorkerThread::isFinished() const {
    const WorkerState currentState = state_.load();
    return currentState == WorkerState::Stopped || currentState == WorkerState::Failed;
}

WorkerThreadStats WorkerThread::stats() const {
    WorkerThreadStats result;
    result.state = state_.load();
    result.processedMessages = processedMessages_.load();
    result.processedTimerCallbacks = processedTimerCallbacks_.load();
    result.busyNanoseconds = busyNanoseconds_.load();
    result.rejectedInputMessages = rejectedInputMessages_.load();
    result.rejectedOutputMessages = rejectedOutputMessages_.load();
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        result.queuedInputMessages = inputQueue_.size();
        result.queuedInputBytes = inputQueuedBytes_;
        result.peakQueuedInputBytes = peakInputQueuedBytes_;
    }
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        result.queuedOutputMessages = outputQueue_.size();
        result.queuedOutputBytes = outputQueuedBytes_;
        result.peakQueuedOutputBytes = peakOutputQueuedBytes_;
    }
    return result;
}

std::vector<WorkerMessage> WorkerThread::drainMessages() {
    std::vector<WorkerMessage> messages;
    std::lock_guard<std::mutex> lock(outputMutex_);
    messages.reserve(outputQueue_.size());
    while (!outputQueue_.empty()) {
        outputQueuedBytes_ -= messageBytes(
            outputQueue_.front().payload, outputQueue_.front().transfers);
        messages.push_back(std::move(outputQueue_.front()));
        outputQueue_.pop();
    }
    return messages;
}

WorkerPostStatus WorkerThread::enqueueOutput(
    WorkerMessage::Type type,
    std::string payload,
    std::vector<js::TransferredArrayBuffer> transfers) {
    std::lock_guard<std::mutex> lock(outputMutex_);
    const size_t byteSize = messageBytes(payload, transfers);
    if (type != WorkerMessage::Type::Ready && type != WorkerMessage::Type::Exited) {
        if (byteSize > queueLimits_.maxBytes) {
            rejectedOutputMessages_.fetch_add(1);
            return WorkerPostStatus::MessageTooLarge;
        }
        if (outputQueue_.size() >= queueLimits_.maxMessages ||
            outputQueuedBytes_ > queueLimits_.maxBytes ||
            byteSize > queueLimits_.maxBytes - outputQueuedBytes_) {
            rejectedOutputMessages_.fetch_add(1);
            return WorkerPostStatus::QueueFull;
        }
    }
    outputQueuedBytes_ += byteSize;
    outputQueue_.push({type, std::move(payload), std::move(transfers)});
    peakOutputQueuedBytes_ = std::max(peakOutputQueuedBytes_, outputQueuedBytes_);
    return WorkerPostStatus::Posted;
}

void WorkerThread::finishThread(js::Engine* engine, WorkerState finalState) {
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        if (engine_ == engine) engine_ = nullptr;
    }
    enqueueOutput(WorkerMessage::Type::Exited, {});
    state_ = finalState;
}

int WorkerThread::scheduleTimer(double delayMs, bool repeat) {
    if (!std::isfinite(delayMs) || delayMs < 0.0) delayMs = 0.0;
    const auto interval = std::chrono::milliseconds(
        static_cast<int64_t>(std::min(delayMs, static_cast<double>(std::numeric_limits<int32_t>::max()))));
    const auto effectiveDelay = repeat && interval.count() == 0
        ? std::chrono::milliseconds(1)
        : interval;
    const int id = nextTimerId_++;
    timers_[id] = {std::chrono::steady_clock::now() + effectiveDelay, effectiveDelay, repeat};
    return id;
}

void WorkerThread::cancelTimer(int id) {
    timers_.erase(id);
}

std::vector<int> WorkerThread::collectDueTimers() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<std::chrono::steady_clock::time_point, int>> due;
    for (const auto& [id, timer] : timers_) {
        if (timer.due <= now) due.emplace_back(timer.due, id);
    }
    std::sort(due.begin(), due.end(), [](const auto& left, const auto& right) {
        return left.first == right.first ? left.second < right.second : left.first < right.first;
    });

    std::vector<int> result;
    result.reserve(due.size());
    for (const auto& [_, id] : due) {
        auto it = timers_.find(id);
        if (it == timers_.end()) continue;
        result.push_back(id);
        if (it->second.repeat) {
            const auto interval = std::max(it->second.interval, std::chrono::milliseconds(1));
            do {
                it->second.due += interval;
            } while (it->second.due <= now);
        } else {
            timers_.erase(it);
        }
    }
    return result;
}

bool WorkerThread::dispatchTimer(js::Engine* engine, int id) {
    const auto startedAt = std::chrono::steady_clock::now();
    engine->beginFrame();
    auto dispatch = engine->getGlobalProperty("__mystralWorkerDispatchTimer");
    auto result = engine->call(dispatch, engine->newUndefined(), {engine->newNumber(id)});
    (void)result;
    bool succeeded = true;
    if (engine->hasException()) {
        const std::string error = engine->getException();
        if (!terminated_.load()) enqueueOutput(WorkerMessage::Type::Error, error);
        succeeded = false;
    }
    busyNanoseconds_.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - startedAt).count()));
    processedTimerCallbacks_.fetch_add(1);
    engine->clearFrameHandles();
    return succeeded;
}

bool WorkerThread::dispatchMessage(js::Engine* engine, const WorkerPayload& message) {
    const auto startedAt = std::chrono::steady_clock::now();
    engine->beginFrame();
    auto dispatch = engine->getGlobalProperty("__mystralWorkerDispatch");
    auto payload = engine->newString(message.serialized.c_str());
    auto transferred = engine->newArray(message.transfers.size());
    for (uint32_t index = 0; index < message.transfers.size(); index++) {
        engine->setPropertyIndex(
            transferred, index, engine->newTransferredArrayBuffer(message.transfers[index]));
    }
    auto result = engine->call(dispatch, engine->newUndefined(), {payload, transferred});
    (void)result;
    bool succeeded = true;
    if (engine->hasException()) {
        const std::string error = engine->getException();
        if (!terminated_.load()) enqueueOutput(WorkerMessage::Type::Error, error);
        succeeded = false;
    }
    busyNanoseconds_.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - startedAt).count()));
    processedMessages_.fetch_add(1);
    engine->clearFrameHandles();
    return succeeded;
}

bool WorkerThread::setupWorkerGlobals(js::Engine* engine) {
    installSharedBufferBindings(engine, sharedBuffers_);
    if (!engine->evalScript(sharedApiSource(), "mystral-shared.js")) return false;

    engine->setGlobalProperty("__workerPostMessage",
        engine->newFunction("__workerPostMessage",
            [](void*, const std::vector<js::JSValueHandle>& args) {
                if (!currentWorkerEngine || !currentWorker) return js::JSValueHandle{};
                if (!args.empty()) {
                    std::vector<js::TransferredArrayBuffer> transfers;
                    if (args.size() > 1 &&
                        !takeTransferList(currentWorkerEngine, args[1], transfers)) {
                        currentWorkerEngine->throwException("Worker transfer list contains an invalid ArrayBuffer");
                        return currentWorkerEngine->newUndefined();
                    }
                    const auto status = currentWorker->enqueueOutput(
                        WorkerMessage::Type::Message,
                        currentWorkerEngine->toString(args[0]),
                        std::move(transfers));
                    if (status == WorkerPostStatus::QueueFull) {
                        currentWorkerEngine->throwException("Worker output queue is full");
                    } else if (status == WorkerPostStatus::MessageTooLarge) {
                        currentWorkerEngine->throwException("Worker message exceeds the output queue byte limit");
                    }
                }
                return currentWorkerEngine->newUndefined();
            }));

    engine->setGlobalProperty("__workerClose",
        engine->newFunction("__workerClose",
            [](void*, const std::vector<js::JSValueHandle>&) {
                if (!currentWorkerEngine || !currentWorker) return js::JSValueHandle{};
                currentWorker->terminated_ = true;
                currentWorker->state_ = WorkerState::Stopping;
                currentWorker->inputCondition_.notify_all();
                return currentWorkerEngine->newUndefined();
            }));

    engine->setGlobalProperty("__workerSetTimer",
        engine->newFunction("__workerSetTimer",
            [](void*, const std::vector<js::JSValueHandle>& args) {
                if (!currentWorkerEngine || !currentWorker) return js::JSValueHandle{};
                const double delay = args.empty() ? 0.0 : currentWorkerEngine->toNumber(args[0]);
                const bool repeat = args.size() > 1 && currentWorkerEngine->toBoolean(args[1]);
                return currentWorkerEngine->newNumber(currentWorker->scheduleTimer(delay, repeat));
            }));

    engine->setGlobalProperty("__workerClearTimer",
        engine->newFunction("__workerClearTimer",
            [](void*, const std::vector<js::JSValueHandle>& args) {
                if (!currentWorkerEngine || !currentWorker) return js::JSValueHandle{};
                if (!args.empty()) currentWorker->cancelTimer(
                    static_cast<int>(currentWorkerEngine->toNumber(args[0])));
                return currentWorkerEngine->newUndefined();
            }));

    engine->setGlobalProperty("__workerReadText",
        engine->newFunction("__workerReadText",
            [this, engine](void*, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) return engine->newNull();
                auto source = readTextFile(rootDir_, engine->toString(args[0]));
                return source.empty() ? engine->newNull() : engine->newString(source.c_str());
            }));

    return engine->evalScript(R"JS(
globalThis.self = globalThis;
(function() {
    let messageHandler = null;
    let errorHandler = null;
    const messageListeners = [];
    const errorListeners = [];
    const timerCallbacks = new Map();

    function dispatchError(error) {
        const errorEvent = { error, message: error && error.message ? error.message : String(error), target: globalThis };
        if (errorHandler) errorHandler.call(globalThis, errorEvent);
        for (const listener of errorListeners.slice()) listener.call(globalThis, errorEvent);
    }

    Object.defineProperty(globalThis, 'onmessage', {
        get: () => messageHandler,
        set: value => { messageHandler = typeof value === 'function' ? value : null; },
        configurable: true,
    });
    Object.defineProperty(globalThis, 'onerror', {
        get: () => errorHandler,
        set: value => { errorHandler = typeof value === 'function' ? value : null; },
        configurable: true,
    });

    globalThis.addEventListener = function(type, handler) {
        if (typeof handler !== 'function') return;
        if (type === 'message') messageListeners.push(handler);
        else if (type === 'error') errorListeners.push(handler);
    };
    globalThis.removeEventListener = function(type, handler) {
        const listeners = type === 'message' ? messageListeners : type === 'error' ? errorListeners : null;
        if (!listeners) return;
        const index = listeners.indexOf(handler);
        if (index >= 0) listeners.splice(index, 1);
    };
    globalThis.postMessage = function(data, transferList = []) {
        const prepared = __mystralPrepareMessage(data, transferList);
        __workerPostMessage(prepared.payload, prepared.transfers);
    };
    globalThis.close = function() { __workerClose(); };
    function createTimer(callback, delay, repeat, args) {
        if (typeof callback !== 'function') throw new TypeError('Worker timer callback must be a function');
        const numericDelay = Number(delay);
        const id = __workerSetTimer(Number.isFinite(numericDelay) ? Math.max(0, numericDelay) : 0, repeat);
        timerCallbacks.set(id, { callback, args, repeat });
        return id;
    }
    globalThis.setTimeout = function(callback, delay = 0, ...args) {
        return createTimer(callback, delay, false, args);
    };
    globalThis.clearTimeout = function(id) {
        timerCallbacks.delete(Number(id));
        __workerClearTimer(Number(id));
    };
    globalThis.setInterval = function(callback, delay = 0, ...args) {
        return createTimer(callback, delay, true, args);
    };
    globalThis.clearInterval = globalThis.clearTimeout;
    globalThis.queueMicrotask = function(callback) {
        if (typeof callback !== 'function') throw new TypeError('queueMicrotask callback must be a function');
        Promise.resolve().then(callback);
    };
    globalThis.importScripts = function(...paths) {
        for (const path of paths) {
            const source = __workerReadText(String(path));
            if (source === null) throw new Error('importScripts failed to load: ' + path);
            (0, eval)(source);
        }
    };
    globalThis.__mystralWorkerDispatch = function(payload, transfers) {
        const event = { data: __mystralParseMessage(payload, transfers), target: globalThis };
        try {
            if (messageHandler) messageHandler.call(globalThis, event);
            for (const listener of messageListeners.slice()) listener.call(globalThis, event);
        } catch (error) {
            dispatchError(error);
            throw error;
        }
    };
    globalThis.__mystralWorkerDispatchTimer = function(id) {
        const timer = timerCallbacks.get(id);
        if (!timer) return;
        if (!timer.repeat) timerCallbacks.delete(id);
        try {
            timer.callback(...timer.args);
        } catch (error) {
            dispatchError(error);
            throw error;
        }
    };
})();
)JS", "worker-global.js");
}

void WorkerThread::threadMain() {
    if (terminated_.load()) {
        finishThread(nullptr, WorkerState::Stopped);
        return;
    }

    auto engine = js::createEngine(engineType_);
    if (!engine) {
        enqueueOutput(WorkerMessage::Type::Error, "Failed to create JavaScript engine for Worker");
        finishThread(nullptr, WorkerState::Failed);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        engine_ = engine.get();
        if (terminated_.load()) engine_->requestTermination();
    }

    currentWorkerEngine = engine.get();
    currentWorker = this;
    js::ModuleSystem moduleSystem(engine.get(), rootDir_);
    js::setModuleSystem(&moduleSystem);

    if (!setupWorkerGlobals(engine.get())) {
        const std::string error = engine->getException();
        if (!terminated_.load()) enqueueOutput(WorkerMessage::Type::Error, error);
        js::setModuleSystem(nullptr);
        currentWorkerEngine = nullptr;
        currentWorker = nullptr;
        finishThread(engine.get(), terminated_.load() ? WorkerState::Stopped : WorkerState::Failed);
        return;
    }

    bool loaded = sourceKind_ == WorkerSourceKind::Module
        ? moduleSystem.loadEntry(source_)
        : engine->evalScript(source_.c_str(), name_.empty() ? "worker.js" : name_.c_str());
    if (!loaded) {
        std::string error = engine->getException();
        if (!terminated_.load()) {
            enqueueOutput(WorkerMessage::Type::Error,
                error.empty() ? "Failed to load Worker script" : std::move(error));
            state_ = WorkerState::Failed;
        }
    } else {
        if (terminated_.load()) {
            state_ = WorkerState::Stopping;
        } else {
            state_ = WorkerState::Running;
            enqueueOutput(WorkerMessage::Type::Ready, {});
        }
    }

    while (loaded && !terminated_.load()) {
        const auto dueTimers = collectDueTimers();
        if (!dueTimers.empty()) {
            for (int timerId : dueTimers) {
                if (terminated_.load()) break;
                dispatchTimer(engine.get(), timerId);
            }
        }

        WorkerPayload pending;
        {
            std::unique_lock<std::mutex> lock(inputMutex_);
            if (inputQueue_.empty()) {
                if (timers_.empty()) {
                    inputCondition_.wait(lock, [this]() {
                        return terminated_.load() || !inputQueue_.empty();
                    });
                } else {
                    auto nextDue = timers_.begin()->second.due;
                    for (const auto& [_, timer] : timers_) nextDue = std::min(nextDue, timer.due);
                    inputCondition_.wait_until(lock, nextDue, [this]() {
                        return terminated_.load() || !inputQueue_.empty();
                    });
                }
            }
            if (terminated_.load()) break;
            if (inputQueue_.empty()) continue;
            pending = std::move(inputQueue_.front());
            inputQueuedBytes_ -= messageBytes(pending.serialized, pending.transfers);
            inputQueue_.pop();
        }
        dispatchMessage(engine.get(), pending);
    }

    timers_.clear();
    moduleSystem.clearCaches();
    js::setModuleSystem(nullptr);
    currentWorkerEngine = nullptr;
    currentWorker = nullptr;
    const WorkerState finalState = state_.load() == WorkerState::Failed
        ? WorkerState::Failed
        : WorkerState::Stopped;
    finishThread(engine.get(), finalState);
}

}  // namespace mystral::workers
