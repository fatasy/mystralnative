#include "mystral/workers/worker_thread.h"

#include "mystral/async/job_system.h"
#include "mystral/js/module_system.h"
#include "mystral/workers/native_task.h"
#include "mystral/workers/worker_registry.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

namespace mystral::workers {

bool WorkerRuntimeState::tryAcquireWorker() {
    uint32_t current = activeWorkers.load();
    while (current < maxWorkers) {
        if (activeWorkers.compare_exchange_weak(current, current + 1)) return true;
    }
    return false;
}

void WorkerRuntimeState::releaseWorker() {
    uint32_t current = activeWorkers.load();
    while (current > 0 && !activeWorkers.compare_exchange_weak(current, current - 1)) {}
}

namespace {

thread_local js::Engine* currentWorkerEngine = nullptr;
thread_local WorkerThread* currentWorker = nullptr;

struct MessageByteSize {
    size_t value = 0;
    bool overflow = false;
};

MessageByteSize messageBytes(
    const std::string& payload,
    const std::vector<js::TransferredArrayBuffer>& transfers) {
    size_t total = payload.size();
    for (const auto& transfer : transfers) {
        if (transfer.size > std::numeric_limits<size_t>::max() - total) {
            return {std::numeric_limits<size_t>::max(), true};
        }
        total += transfer.size;
    }
    return {total, false};
}

void recordMaximum(std::atomic<size_t>& target, size_t value) {
    size_t current = target.load();
    while (current < value && !target.compare_exchange_weak(current, value)) {}
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
    if (normalized.rfind("file://", 0) == 0) {
        normalized.erase(0, 7);
#ifdef _WIN32
        if (normalized.size() > 3 && normalized[0] == '/' &&
            std::isalpha(static_cast<unsigned char>(normalized[1])) && normalized[2] == ':') {
            normalized.erase(0, 1);
        }
#endif
    }
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
                           WorkerQueueLimits queueLimits,
                           std::shared_ptr<WorkerRuntimeState> runtimeState,
                           uint32_t depth,
                           std::function<void()> outputReadyCallback)
    : id_(id)
    , engineType_(engineType)
    , sourceKind_(sourceKind)
    , source_(std::move(source))
    , rootDir_(std::move(rootDir))
    , name_(std::move(name))
    , sharedBuffers_(std::move(sharedBuffers))
    , runtimeState_(std::move(runtimeState))
    , queueLimits_(queueLimits)
    , depth_(depth)
    , outputReadyCallback_(std::move(outputReadyCallback)) {
    if (!runtimeState_) runtimeState_ = std::make_shared<WorkerRuntimeState>();
    if (depth_ < runtimeState_->maxDepth) {
        childRegistry_ = std::make_unique<WorkerRegistry>(
            engineType_,
            rootDir_,
            sharedBuffers_,
            runtimeState_,
            depth_,
            [this]() {
                {
                    std::lock_guard<std::mutex> lock(inputMutex_);
                    childActivity_.store(true);
                }
                inputCondition_.notify_one();
            });
    }
    nativeTaskMailbox_ = std::make_shared<NativeTaskMailbox>([this]() {
        {
            std::lock_guard<std::mutex> lock(inputMutex_);
            nativeTaskActivity_.store(true);
        }
        inputCondition_.notify_one();
    });
}

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
    const auto measured = messageBytes(payload, transfers);
    recordMaximum(largestInputMessageBytes_, measured.value);
    if (measured.overflow || measured.value > queueLimits_.maxMessageBytes) {
        rejectedInputMessages_.fetch_add(1);
        rejectedInputTooLarge_.fetch_add(1);
        return WorkerPostStatus::MessageTooLarge;
    }
    const size_t byteSize = measured.value;
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        const WorkerState lockedState = state_.load();
        if (terminated_.load() ||
            (lockedState != WorkerState::Starting && lockedState != WorkerState::Running)) {
            return WorkerPostStatus::NotRunning;
        }
        if (inputQueue_.size() >= queueLimits_.maxMessages ||
            inputQueuedBytes_ > queueLimits_.maxQueuedBytes ||
            byteSize > queueLimits_.maxQueuedBytes - inputQueuedBytes_) {
            rejectedInputMessages_.fetch_add(1);
            rejectedInputQueueFull_.fetch_add(1);
            return WorkerPostStatus::QueueFull;
        }
        inputQueuedBytes_ += byteSize;
        inputQueue_.push({std::move(payload), std::move(transfers)});
        peakInputQueuedBytes_ = std::max(peakInputQueuedBytes_, inputQueuedBytes_);
    }
    inputCondition_.notify_one();
    return WorkerPostStatus::Posted;
}

void WorkerThread::requestStop() {
    terminated_ = true;
    if (nativeTaskMailbox_) nativeTaskMailbox_->close();
    WorkerState currentState = state_.load();
    while (currentState != WorkerState::Stopped && currentState != WorkerState::Failed &&
           !state_.compare_exchange_weak(currentState, WorkerState::Stopping)) {}
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        if (engine_) engine_->requestTermination();
    }
    inputCondition_.notify_all();
    async::notifyCpuBudgetWaiters();
}

void WorkerThread::terminate() {
    requestStop();
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
    result.rejectedInputTooLarge = rejectedInputTooLarge_.load();
    result.rejectedInputQueueFull = rejectedInputQueueFull_.load();
    result.rejectedOutputTooLarge = rejectedOutputTooLarge_.load();
    result.rejectedOutputQueueFull = rejectedOutputQueueFull_.load();
    result.largestInputMessageBytes = largestInputMessageBytes_.load();
    result.largestOutputMessageBytes = largestOutputMessageBytes_.load();
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
    result.maxDepth = depth_;
    if (childRegistry_) {
        const auto child = childRegistry_->stats();
        result.descendantCreatedWorkers = child.createdWorkers;
        result.descendantActiveWorkers = child.activeWorkers;
        result.maxDepth = std::max(result.maxDepth, child.maxDepth);
        result.processedMessages += child.processedMessages;
        result.processedTimerCallbacks += child.processedTimerCallbacks;
        result.busyNanoseconds += child.busyNanoseconds;
        result.rejectedInputMessages += child.rejectedInputMessages;
        result.rejectedOutputMessages += child.rejectedOutputMessages;
        result.rejectedInputTooLarge += child.rejectedInputTooLarge;
        result.rejectedInputQueueFull += child.rejectedInputQueueFull;
        result.rejectedOutputTooLarge += child.rejectedOutputTooLarge;
        result.rejectedOutputQueueFull += child.rejectedOutputQueueFull;
        result.queuedInputMessages += child.queuedInputMessages;
        result.queuedInputBytes += child.queuedInputBytes;
        result.queuedOutputMessages += child.queuedOutputMessages;
        result.queuedOutputBytes += child.queuedOutputBytes;
        result.peakQueuedInputBytes = std::max(
            result.peakQueuedInputBytes, static_cast<size_t>(child.peakQueuedInputBytes));
        result.peakQueuedOutputBytes = std::max(
            result.peakQueuedOutputBytes, static_cast<size_t>(child.peakQueuedOutputBytes));
        result.largestInputMessageBytes = std::max(
            result.largestInputMessageBytes, static_cast<size_t>(child.largestInputMessageBytes));
        result.largestOutputMessageBytes = std::max(
            result.largestOutputMessageBytes, static_cast<size_t>(child.largestOutputMessageBytes));
    }
    return result;
}

std::vector<WorkerMessage> WorkerThread::drainMessages() {
    std::vector<WorkerMessage> messages;
    std::lock_guard<std::mutex> lock(outputMutex_);
    messages.reserve(outputQueue_.size());
    while (!outputQueue_.empty()) {
        const auto measured = messageBytes(
            outputQueue_.front().payload, outputQueue_.front().transfers);
        outputQueuedBytes_ = measured.overflow || measured.value > outputQueuedBytes_
            ? 0
            : outputQueuedBytes_ - measured.value;
        messages.push_back(std::move(outputQueue_.front()));
        outputQueue_.pop();
    }
    return messages;
}

WorkerPostStatus WorkerThread::enqueueOutput(
    WorkerMessage::Type type,
    std::string payload,
    std::vector<js::TransferredArrayBuffer> transfers) {
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        const auto measured = messageBytes(payload, transfers);
        recordMaximum(largestOutputMessageBytes_, measured.value);
        const size_t byteSize = measured.value;
        if (type != WorkerMessage::Type::Ready && type != WorkerMessage::Type::Exited) {
            if (measured.overflow || byteSize > queueLimits_.maxMessageBytes) {
                rejectedOutputMessages_.fetch_add(1);
                rejectedOutputTooLarge_.fetch_add(1);
                return WorkerPostStatus::MessageTooLarge;
            }
            if (outputQueue_.size() >= queueLimits_.maxMessages ||
                outputQueuedBytes_ > queueLimits_.maxQueuedBytes ||
                byteSize > queueLimits_.maxQueuedBytes - outputQueuedBytes_) {
                rejectedOutputMessages_.fetch_add(1);
                rejectedOutputQueueFull_.fetch_add(1);
                return WorkerPostStatus::QueueFull;
            }
        }
        outputQueuedBytes_ += byteSize;
        outputQueue_.push({type, std::move(payload), std::move(transfers)});
        peakOutputQueuedBytes_ = std::max(peakOutputQueuedBytes_, outputQueuedBytes_);
    }
    if (outputReadyCallback_) outputReadyCallback_();
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
    auto cpuLease = async::acquireCpuBudget(&terminated_);
    if (!cpuLease) return true;
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
    auto cpuLease = async::acquireCpuBudget(&terminated_);
    if (!cpuLease) return true;
    const auto startedAt = std::chrono::steady_clock::now();
    engine->beginFrame();
    auto dispatch = engine->getGlobalProperty("__mystralWorkerDispatch");
    auto payload = engine->newString(message.serialized.c_str());
    auto transferred = engine->newArray(message.transfers.size());
    for (uint32_t index = 0; index < message.transfers.size(); index++) {
        auto transferredBuffer = engine->newTransferredArrayBuffer(message.transfers[index]);
        engine->setPropertyIndex(transferred, index, transferredBuffer);
        engine->releaseValue(transferredBuffer);
    }
    auto thisArg = engine->newUndefined();
    auto result = engine->call(dispatch, thisArg, {payload, transferred});
    engine->releaseValue(result);
    engine->releaseValue(thisArg);
    engine->releaseValue(transferred);
    engine->releaseValue(payload);
    engine->releaseValue(dispatch);
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

bool WorkerThread::dispatchChildMessages(js::Engine* engine) {
    if (!childRegistry_) return true;
    auto cpuLease = async::acquireCpuBudget(&terminated_);
    if (!cpuLease) return true;
    std::string error;
    engine->beginFrame();
    const bool succeeded = dispatchWorkerRegistryMessages(engine, childRegistry_.get(), &error);
    engine->clearFrameHandles();
    if (!succeeded && !terminated_.load()) {
        enqueueOutput(WorkerMessage::Type::Error,
            error.empty() ? "Nested Worker message handler failed" : std::move(error));
    }
    return succeeded;
}

bool WorkerThread::dispatchNativeTaskMessages(js::Engine* engine) {
    if (!nativeTaskMailbox_) return true;
    auto cpuLease = async::acquireCpuBudget(&terminated_);
    if (!cpuLease) return true;
    std::string error;
    engine->beginFrame();
    const bool succeeded = dispatchNativeTaskCompletions(
        engine, nativeTaskMailbox_, &error);
    engine->clearFrameHandles();
    if (!succeeded && !terminated_.load()) {
        enqueueOutput(WorkerMessage::Type::Error,
            error.empty() ? "Native task completion handler failed" : std::move(error));
    }
    return succeeded;
}

bool WorkerThread::setupWorkerGlobals(js::Engine* engine) {
    installSharedBufferBindings(engine, sharedBuffers_);
    if (!engine->evalScript(sharedApiSource(), "mystral-shared.js")) return false;
    if (!installNativeTaskBindings(engine, nativeTaskMailbox_)) return false;
    if (!engine->evalScript(nativeTaskApiSource(), "mystral-native-tasks.js")) return false;
    if (childRegistry_) {
        if (!installWorkerRegistryBindings(engine, childRegistry_.get())) return false;
        if (!engine->evalScript(nestedWorkerFacadeSource(), "nested-worker-global.js")) return false;
    }

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

    auto startupCpuLease = async::acquireCpuBudget(&terminated_);
    if (!startupCpuLease) {
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
        if (nativeTaskMailbox_) nativeTaskMailbox_->close();
        if (childRegistry_) childRegistry_->shutdown();
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

    startupCpuLease = {};

    while (loaded && !terminated_.load()) {
        childActivity_.store(false);
        nativeTaskActivity_.store(false);
        dispatchChildMessages(engine.get());
        dispatchNativeTaskMessages(engine.get());

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
                        return terminated_.load() || childActivity_.load() ||
                            nativeTaskActivity_.load() || !inputQueue_.empty();
                    });
                } else {
                    auto nextDue = timers_.begin()->second.due;
                    for (const auto& [_, timer] : timers_) nextDue = std::min(nextDue, timer.due);
                    inputCondition_.wait_until(lock, nextDue, [this]() {
                        return terminated_.load() || childActivity_.load() ||
                            nativeTaskActivity_.load() || !inputQueue_.empty();
                    });
                }
            }
            if (terminated_.load()) break;
            if ((childActivity_.load() || nativeTaskActivity_.load()) && inputQueue_.empty()) continue;
            if (inputQueue_.empty()) continue;
            pending = std::move(inputQueue_.front());
            const auto measured = messageBytes(pending.serialized, pending.transfers);
            inputQueuedBytes_ = measured.overflow || measured.value > inputQueuedBytes_
                ? 0
                : inputQueuedBytes_ - measured.value;
            inputQueue_.pop();
        }
        dispatchMessage(engine.get(), pending);
    }

    timers_.clear();
    if (nativeTaskMailbox_) nativeTaskMailbox_->close();
    if (childRegistry_) childRegistry_->shutdown();
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
