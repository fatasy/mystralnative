#include "mystral/workers/worker_registry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace mystral::workers {

WorkerRegistry::WorkerRegistry(js::EngineType engineType,
                               std::string rootDir,
                               std::shared_ptr<SharedBufferRegistry> sharedBuffers,
                               std::shared_ptr<WorkerRuntimeState> runtimeState,
                               uint32_t ownerDepth,
                               ActivityCallback activityCallback)
    : engineType_(engineType)
    , rootDir_(std::move(rootDir))
    , sharedBuffers_(std::move(sharedBuffers))
    , runtimeState_(std::move(runtimeState))
    , ownerDepth_(ownerDepth)
    , activityCallback_(std::move(activityCallback)) {
    if (!runtimeState_) runtimeState_ = std::make_shared<WorkerRuntimeState>();
    maxObservedDepth_ = ownerDepth_;
}

WorkerRegistry::~WorkerRegistry() {
    shutdown();
}

int WorkerRegistry::createWorker(WorkerSourceKind sourceKind,
                                 const std::string& source,
                                 const std::string& name,
                                 WorkerQueueLimits queueLimits,
                                 std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shuttingDown_) {
        if (error) *error = "Worker registry is shutting down";
        return -1;
    }
    if (ownerDepth_ >= runtimeState_->maxDepth) {
        if (error) *error = "Worker creation exceeds the runtime nesting limit";
        return -1;
    }
    if (!runtimeState_->tryAcquireWorker()) {
        if (error) *error = "Worker creation exceeds the runtime worker limit";
        return -1;
    }
    const int id = nextId_++;
    std::unique_ptr<WorkerThread> worker;
    try {
        worker = std::make_unique<WorkerThread>(
            id,
            engineType_,
            sourceKind,
            source,
            rootDir_,
            name,
            sharedBuffers_,
            queueLimits,
            runtimeState_,
            ownerDepth_ + 1,
            [callback = activityCallback_]() {
                if (callback) callback();
            });
    } catch (...) {
        runtimeState_->releaseWorker();
        throw;
    }
    createdWorkers_++;
    maxObservedDepth_ = std::max(maxObservedDepth_, ownerDepth_ + 1);
    worker->start();
    workers_.emplace(id, std::move(worker));
    return id;
}

WorkerPostStatus WorkerRegistry::postToWorker(
    int id,
    std::string payload,
    std::vector<js::TransferredArrayBuffer> transfers) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(id);
    if (it == workers_.end()) return WorkerPostStatus::NotFound;
    return it->second->postMessage(std::move(payload), std::move(transfers));
}

WorkerPostStatus WorkerRegistry::postToWorkers(
    const std::vector<int>& ids,
    const std::string& payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int id : ids) {
        if (workers_.find(id) == workers_.end()) return WorkerPostStatus::NotFound;
    }
    for (int id : ids) {
        const auto status = workers_.at(id)->postMessage(payload);
        if (status != WorkerPostStatus::Posted) return status;
    }
    return WorkerPostStatus::Posted;
}

void WorkerRegistry::terminateWorker(int id) {
    std::unique_ptr<WorkerThread> worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = workers_.find(id);
        if (it == workers_.end()) return;
        worker = std::move(it->second);
        workers_.erase(it);
    }
    worker->terminate();
    const auto stats = worker->stats();
    runtimeState_->releaseWorker();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completedMessages_ += stats.processedMessages;
        completedTimerCallbacks_ += stats.processedTimerCallbacks;
        completedBusyNanoseconds_ += stats.busyNanoseconds;
        completedRejectedInputMessages_ += stats.rejectedInputMessages;
        completedRejectedOutputMessages_ += stats.rejectedOutputMessages;
        completedRejectedInputTooLarge_ += stats.rejectedInputTooLarge;
        completedRejectedInputQueueFull_ += stats.rejectedInputQueueFull;
        completedRejectedOutputTooLarge_ += stats.rejectedOutputTooLarge;
        completedRejectedOutputQueueFull_ += stats.rejectedOutputQueueFull;
        peakQueuedInputBytes_ = std::max(peakQueuedInputBytes_,
            static_cast<uint64_t>(stats.peakQueuedInputBytes));
        peakQueuedOutputBytes_ = std::max(peakQueuedOutputBytes_,
            static_cast<uint64_t>(stats.peakQueuedOutputBytes));
        largestInputMessageBytes_ = std::max(largestInputMessageBytes_,
            static_cast<uint64_t>(stats.largestInputMessageBytes));
        largestOutputMessageBytes_ = std::max(largestOutputMessageBytes_,
            static_cast<uint64_t>(stats.largestOutputMessageBytes));
        completedDescendantWorkers_ += stats.descendantCreatedWorkers;
        maxObservedDepth_ = std::max(maxObservedDepth_, stats.maxDepth);
    }
}

bool WorkerRegistry::drainMessages(const MessageCallback& callback, size_t maxCount) {
    if (maxCount == 0) return false;
    std::vector<std::pair<int, WorkerMessage>> messages;
    std::vector<int> stopped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int> workerIds;
        workerIds.reserve(workers_.size());
        for (const auto& entry : workers_) workerIds.push_back(entry.first);
        std::sort(workerIds.begin(), workerIds.end());
        auto start = std::upper_bound(workerIds.begin(), workerIds.end(), lastDrainWorkerId_);
        if (start == workerIds.end()) start = workerIds.begin();

        size_t remaining = maxCount;
        for (size_t offset = 0; offset < workerIds.size(); ++offset) {
            const size_t startIndex = static_cast<size_t>(start - workerIds.begin());
            const int id = workerIds[(startIndex + offset) % workerIds.size()];
            auto entry = workers_.find(id);
            if (entry == workers_.end()) continue;
            lastDrainWorkerId_ = id;
            for (auto& message : entry->second->drainMessages(remaining)) {
                messages.emplace_back(id, std::move(message));
                --remaining;
            }
            if (entry->second->isFinished() && remaining > 0) {
                // finishThread publishes Exited before the terminal state. Drain
                // once more after observing that state so the facade cannot miss
                // an exit published between the first drain and this check.
                for (auto& message : entry->second->drainMessages(remaining)) {
                    messages.emplace_back(id, std::move(message));
                    --remaining;
                }
            }
            if (entry->second->isFinished() && !entry->second->hasPendingMessages()) {
                stopped.push_back(id);
            }
            if (remaining == 0) break;
        }
    }

    for (const auto& entry : messages) callback(entry.first, entry.second);
    for (int id : stopped) terminateWorker(id);
    return !messages.empty();
}

void WorkerRegistry::shutdown() {
    std::unordered_map<int, std::unique_ptr<WorkerThread>> workers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shuttingDown_ = true;
        workers.swap(workers_);
    }
    uint64_t messages = 0;
    uint64_t timerCallbacks = 0;
    uint64_t busyNanoseconds = 0;
    uint64_t rejectedInputMessages = 0;
    uint64_t rejectedOutputMessages = 0;
    uint64_t rejectedInputTooLarge = 0;
    uint64_t rejectedInputQueueFull = 0;
    uint64_t rejectedOutputTooLarge = 0;
    uint64_t rejectedOutputQueueFull = 0;
    uint64_t peakInputBytes = 0;
    uint64_t peakOutputBytes = 0;
    uint64_t largestInputMessageBytes = 0;
    uint64_t largestOutputMessageBytes = 0;
    uint64_t descendantWorkers = 0;
    uint32_t maxDepth = maxObservedDepth_;
    for (auto& entry : workers) entry.second->requestStop();
    for (auto& entry : workers) {
        entry.second->terminate();
        const auto stats = entry.second->stats();
        runtimeState_->releaseWorker();
        messages += stats.processedMessages;
        timerCallbacks += stats.processedTimerCallbacks;
        busyNanoseconds += stats.busyNanoseconds;
        rejectedInputMessages += stats.rejectedInputMessages;
        rejectedOutputMessages += stats.rejectedOutputMessages;
        rejectedInputTooLarge += stats.rejectedInputTooLarge;
        rejectedInputQueueFull += stats.rejectedInputQueueFull;
        rejectedOutputTooLarge += stats.rejectedOutputTooLarge;
        rejectedOutputQueueFull += stats.rejectedOutputQueueFull;
        peakInputBytes = std::max(peakInputBytes,
            static_cast<uint64_t>(stats.peakQueuedInputBytes));
        peakOutputBytes = std::max(peakOutputBytes,
            static_cast<uint64_t>(stats.peakQueuedOutputBytes));
        largestInputMessageBytes = std::max(largestInputMessageBytes,
            static_cast<uint64_t>(stats.largestInputMessageBytes));
        largestOutputMessageBytes = std::max(largestOutputMessageBytes,
            static_cast<uint64_t>(stats.largestOutputMessageBytes));
        descendantWorkers += stats.descendantCreatedWorkers;
        maxDepth = std::max(maxDepth, stats.maxDepth);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    completedMessages_ += messages;
    completedTimerCallbacks_ += timerCallbacks;
    completedBusyNanoseconds_ += busyNanoseconds;
    completedRejectedInputMessages_ += rejectedInputMessages;
    completedRejectedOutputMessages_ += rejectedOutputMessages;
    completedRejectedInputTooLarge_ += rejectedInputTooLarge;
    completedRejectedInputQueueFull_ += rejectedInputQueueFull;
    completedRejectedOutputTooLarge_ += rejectedOutputTooLarge;
    completedRejectedOutputQueueFull_ += rejectedOutputQueueFull;
    peakQueuedInputBytes_ = std::max(peakQueuedInputBytes_, peakInputBytes);
    peakQueuedOutputBytes_ = std::max(peakQueuedOutputBytes_, peakOutputBytes);
    largestInputMessageBytes_ = std::max(largestInputMessageBytes_, largestInputMessageBytes);
    largestOutputMessageBytes_ = std::max(largestOutputMessageBytes_, largestOutputMessageBytes);
    completedDescendantWorkers_ += descendantWorkers;
    maxObservedDepth_ = std::max(maxObservedDepth_, maxDepth);
}

size_t WorkerRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return workers_.size();
}

uint32_t WorkerRegistry::suggestedWorkerCount() const {
    const uint32_t active = runtimeState_->activeWorkers.load();
    const uint32_t remaining = active < runtimeState_->maxWorkers
        ? runtimeState_->maxWorkers - active
        : 0;
    return std::max(1u, std::min(runtimeState_->maxParallelism, remaining));
}

WorkerRegistryStats WorkerRegistry::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    WorkerRegistryStats result;
    result.createdWorkers = createdWorkers_ + completedDescendantWorkers_;
    result.nestedCreatedWorkers = completedDescendantWorkers_;
    result.activeWorkers = workers_.size();
    result.maxDepth = maxObservedDepth_;
    result.processedMessages = completedMessages_;
    result.processedTimerCallbacks = completedTimerCallbacks_;
    result.busyNanoseconds = completedBusyNanoseconds_;
    result.rejectedInputMessages = completedRejectedInputMessages_;
    result.rejectedOutputMessages = completedRejectedOutputMessages_;
    result.rejectedInputTooLarge = completedRejectedInputTooLarge_;
    result.rejectedInputQueueFull = completedRejectedInputQueueFull_;
    result.rejectedOutputTooLarge = completedRejectedOutputTooLarge_;
    result.rejectedOutputQueueFull = completedRejectedOutputQueueFull_;
    result.peakQueuedInputBytes = peakQueuedInputBytes_;
    result.peakQueuedOutputBytes = peakQueuedOutputBytes_;
    result.largestInputMessageBytes = largestInputMessageBytes_;
    result.largestOutputMessageBytes = largestOutputMessageBytes_;
    for (const auto& entry : workers_) {
        const auto stats = entry.second->stats();
        result.createdWorkers += stats.descendantCreatedWorkers;
        result.nestedCreatedWorkers += stats.descendantCreatedWorkers;
        result.activeWorkers += stats.descendantActiveWorkers;
        result.maxDepth = std::max(result.maxDepth, stats.maxDepth);
        result.processedMessages += stats.processedMessages;
        result.processedTimerCallbacks += stats.processedTimerCallbacks;
        result.busyNanoseconds += stats.busyNanoseconds;
        result.rejectedInputMessages += stats.rejectedInputMessages;
        result.rejectedOutputMessages += stats.rejectedOutputMessages;
        result.rejectedInputTooLarge += stats.rejectedInputTooLarge;
        result.rejectedInputQueueFull += stats.rejectedInputQueueFull;
        result.rejectedOutputTooLarge += stats.rejectedOutputTooLarge;
        result.rejectedOutputQueueFull += stats.rejectedOutputQueueFull;
        result.queuedInputMessages += stats.queuedInputMessages;
        result.queuedInputBytes += stats.queuedInputBytes;
        result.queuedOutputMessages += stats.queuedOutputMessages;
        result.queuedOutputBytes += stats.queuedOutputBytes;
        result.peakQueuedInputBytes = std::max(result.peakQueuedInputBytes,
            static_cast<uint64_t>(stats.peakQueuedInputBytes));
        result.peakQueuedOutputBytes = std::max(result.peakQueuedOutputBytes,
            static_cast<uint64_t>(stats.peakQueuedOutputBytes));
        result.largestInputMessageBytes = std::max(result.largestInputMessageBytes,
            static_cast<uint64_t>(stats.largestInputMessageBytes));
        result.largestOutputMessageBytes = std::max(result.largestOutputMessageBytes,
            static_cast<uint64_t>(stats.largestOutputMessageBytes));
    }
    return result;
}

bool installWorkerRegistryBindings(js::Engine* engine, WorkerRegistry* registry) {
    if (!engine || !registry) return false;

    engine->setGlobalProperty("__mystralWorkerHardwareConcurrency",
        engine->newNumber(std::max(1u, std::thread::hardware_concurrency())));
    engine->setGlobalProperty("__mystralWorkerPoolConcurrency",
        engine->newNumber(registry->suggestedWorkerCount()));

    engine->setGlobalProperty("__mystralWorkerCreate",
        engine->newFunction("__mystralWorkerCreate",
            [engine, registry](void*, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2) {
                    engine->throwException("Worker requires a script source");
                    return engine->newNumber(-1);
                }
                const auto kind = engine->toNumber(args[0]) == 0
                    ? WorkerSourceKind::Script
                    : WorkerSourceKind::Module;
                const std::string source = engine->toString(args[1]);
                std::string name;
                if (args.size() > 2 && !engine->isUndefined(args[2])) {
                    name = engine->toString(args[2]);
                }
                if (source.empty()) {
                    engine->throwException("Worker script source is empty");
                    return engine->newNumber(-1);
                }

                WorkerQueueLimits queueLimits;
                const auto readLimit = [engine, &args](
                    size_t index, size_t& target, const char* option) {
                    if (args.size() <= index || engine->isUndefined(args[index])) return true;
                    constexpr double maxSafeInteger = 9'007'199'254'740'991.0;
                    const double value = engine->toNumber(args[index]);
                    if (!std::isfinite(value) || value < 1.0 || value > maxSafeInteger ||
                        std::floor(value) != value) {
                        const std::string message =
                            std::string("Worker ") + option + " must be a positive safe integer";
                        engine->throwException(message.c_str());
                        return false;
                    }
                    target = static_cast<size_t>(value);
                    return true;
                };
                if (!readLimit(3, queueLimits.maxMessages, "maxMessages") ||
                    !readLimit(4, queueLimits.maxMessageBytes, "maxMessageBytes") ||
                    !readLimit(5, queueLimits.maxQueuedBytes, "maxQueuedBytes")) {
                    return engine->newNumber(-1);
                }
                if (queueLimits.maxMessageBytes > queueLimits.maxQueuedBytes) {
                    engine->throwException("Worker maxMessageBytes cannot exceed maxQueuedBytes");
                    return engine->newNumber(-1);
                }

                std::string error;
                const int id = registry->createWorker(
                    kind, source, name, queueLimits, &error);
                if (id < 0 && !error.empty()) engine->throwException(error.c_str());
                return engine->newNumber(id);
            }));

    engine->setGlobalProperty("__mystralWorkerPostMessage",
        engine->newFunction("__mystralWorkerPostMessage",
            [engine, registry](void*, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2) {
                    return engine->newNumber(static_cast<int>(WorkerPostStatus::NotFound));
                }
                const int id = static_cast<int>(engine->toNumber(args[0]));
                std::vector<js::TransferredArrayBuffer> transfers;
                if (args.size() > 2 && !engine->isUndefined(args[2]) &&
                    !engine->isNull(args[2])) {
                    if (!engine->isArray(args[2])) {
                        return engine->newNumber(
                            static_cast<int>(WorkerPostStatus::InvalidTransfer));
                    }
                    const auto lengthValue = engine->getProperty(args[2], "length");
                    const auto length = static_cast<uint32_t>(engine->toNumber(lengthValue));
                    transfers.reserve(length);
                    for (uint32_t index = 0; index < length; index++) {
                        js::TransferredArrayBuffer transferred;
                        if (!engine->transferArrayBuffer(
                                engine->getPropertyIndex(args[2], index), transferred)) {
                            return engine->newNumber(
                                static_cast<int>(WorkerPostStatus::InvalidTransfer));
                        }
                        transfers.push_back(std::move(transferred));
                    }
                }
                return engine->newNumber(static_cast<int>(registry->postToWorker(
                    id, engine->toString(args[1]), std::move(transfers))));
            }));

    engine->setGlobalProperty("__mystralWorkerBroadcast",
        engine->newFunction("__mystralWorkerBroadcast",
            [engine, registry](void*, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2 || !engine->isArray(args[0])) {
                    return engine->newNumber(
                        static_cast<int>(WorkerPostStatus::NotFound));
                }
                const auto lengthValue = engine->getProperty(args[0], "length");
                const auto length = static_cast<uint32_t>(engine->toNumber(lengthValue));
                std::vector<int> ids;
                ids.reserve(length);
                for (uint32_t index = 0; index < length; index++) {
                    ids.push_back(static_cast<int>(
                        engine->toNumber(engine->getPropertyIndex(args[0], index))));
                }
                return engine->newNumber(static_cast<int>(registry->postToWorkers(
                    ids, engine->toString(args[1]))));
            }));

    engine->setGlobalProperty("__mystralWorkerTerminate",
        engine->newFunction("__mystralWorkerTerminate",
            [engine, registry](void*, const std::vector<js::JSValueHandle>& args) {
                if (!args.empty()) {
                    registry->terminateWorker(static_cast<int>(engine->toNumber(args[0])));
                }
                return engine->newUndefined();
            }));
    return !engine->hasException();
}

bool dispatchWorkerRegistryMessages(
    js::Engine* engine,
    WorkerRegistry* registry,
    std::string* error,
    size_t maxCount) {
    if (!engine || !registry) return true;
    auto dispatch = engine->getGlobalProperty("__mystralDispatchWorkerMessage");
    if (!engine->isFunction(dispatch)) {
        engine->releaseValue(dispatch);
        return true;
    }

    bool succeeded = true;
    registry->drainMessages(
        [engine, dispatch, &succeeded, error](int id, const WorkerMessage& message) {
            const char* type = "message";
            if (message.type == WorkerMessage::Type::Error) type = "error";
            else if (message.type == WorkerMessage::Type::Ready) type = "ready";
            else if (message.type == WorkerMessage::Type::Exited) type = "exit";

            auto transfers = engine->newArray(message.transfers.size());
            for (uint32_t index = 0; index < message.transfers.size(); index++) {
                auto transferredBuffer =
                    engine->newTransferredArrayBuffer(message.transfers[index]);
                engine->setPropertyIndex(transfers, index, transferredBuffer);
                engine->releaseValue(transferredBuffer);
            }
            auto idValue = engine->newNumber(id);
            auto typeValue = engine->newString(type);
            auto payloadValue = engine->newString(message.payload.c_str());
            auto thisArg = engine->newUndefined();
            auto result = engine->call(dispatch, thisArg, {
                idValue,
                typeValue,
                payloadValue,
                transfers,
            });
            engine->releaseValue(result);
            engine->releaseValue(thisArg);
            engine->releaseValue(payloadValue);
            engine->releaseValue(typeValue);
            engine->releaseValue(idValue);
            engine->releaseValue(transfers);
            if (engine->hasException()) {
                const std::string currentError = engine->getException();
                if (error && error->empty()) *error = currentError;
                succeeded = false;
            }
        }, maxCount);
    engine->releaseValue(dispatch);
    return succeeded;
}

const char* nestedWorkerFacadeSource() {
    return R"JS(
(function() {
    const nativeWorkers = new Map();

    if (typeof globalThis.navigator === 'undefined') globalThis.navigator = {};
    if (!Number.isInteger(globalThis.navigator.hardwareConcurrency)) {
        globalThis.navigator.hardwareConcurrency = __mystralWorkerHardwareConcurrency;
    }

    globalThis.__mystralDispatchWorkerMessage = function(id, type, payload, transfers) {
        const worker = nativeWorkers.get(id);
        if (!worker || worker._terminated) return;
        if (type === 'ready') {
            worker._ready = true;
            return;
        }
        if (type === 'exit') {
            worker._terminated = true;
            nativeWorkers.delete(id);
            return;
        }
        if (type === 'error') {
            const event = { type: 'error', message: payload, error: new Error(payload), target: worker };
            if (worker.onerror) worker.onerror.call(worker, event);
            for (const listener of worker._errorListeners.slice()) listener.call(worker, event);
            return;
        }
        const event = { type: 'message', data: __mystralParseMessage(payload, transfers), target: worker };
        if (worker.onmessage) worker.onmessage.call(worker, event);
        for (const listener of worker._messageListeners.slice()) listener.call(worker, event);
    };

    class NestedWorker {
        constructor(url, options = {}) {
            this.onmessage = null;
            this.onerror = null;
            this._terminated = false;
            this._ready = false;
            this._messageListeners = [];
            this._errorListeners = [];

            const source = String(url);
            if (!source) throw new TypeError('Worker requires a script URL');
            const limit = name => {
                const value = options && options[name];
                if (value === undefined) return undefined;
                if (!Number.isSafeInteger(value) || value <= 0) {
                    throw new RangeError('Worker ' + name + ' must be a positive safe integer');
                }
                return value;
            };
            const maxMessages = limit('maxMessages');
            const maxMessageBytes = limit('maxMessageBytes');
            const maxQueuedBytes = limit('maxQueuedBytes');
            if (maxMessageBytes !== undefined && maxQueuedBytes !== undefined &&
                maxMessageBytes > maxQueuedBytes) {
                throw new RangeError('Worker maxMessageBytes cannot exceed maxQueuedBytes');
            }
            this._id = __mystralWorkerCreate(
                1,
                source,
                options && options.name ? String(options.name) : '',
                maxMessages,
                maxMessageBytes,
                maxQueuedBytes,
            );
            if (this._id < 0) throw new Error('Failed to create Worker');
            nativeWorkers.set(this._id, this);
        }

        postMessage(data, transferList = []) {
            if (this._terminated) return;
            const prepared = __mystralPrepareMessage(data, transferList);
            const status = __mystralWorkerPostMessage(this._id, prepared.payload, prepared.transfers);
            if (status === 0) return;
            if (status === 2) throw new RangeError('Worker input queue is full');
            if (status === 3) throw new RangeError('Worker message exceeds the input queue byte limit');
            if (status === 5) throw new TypeError('Worker transfer list contains an invalid ArrayBuffer');
            throw new Error('Worker is no longer running');
        }

        terminate() {
            if (this._terminated) return;
            this._terminated = true;
            nativeWorkers.delete(this._id);
            __mystralWorkerTerminate(this._id);
        }

        addEventListener(type, handler) {
            if (typeof handler !== 'function') return;
            if (type === 'message') this._messageListeners.push(handler);
            else if (type === 'error') this._errorListeners.push(handler);
        }

        removeEventListener(type, handler) {
            const listeners = type === 'message'
                ? this._messageListeners
                : type === 'error' ? this._errorListeners : null;
            if (!listeners) return;
            const index = listeners.indexOf(handler);
            if (index >= 0) listeners.splice(index, 1);
        }
    }

    globalThis.Worker = NestedWorker;
})();
)JS";
}

}  // namespace mystral::workers
