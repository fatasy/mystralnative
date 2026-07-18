#include "mystral/workers/worker_registry.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace mystral::workers {

WorkerRegistry::WorkerRegistry(js::EngineType engineType,
                               std::string rootDir,
                               std::shared_ptr<SharedBufferRegistry> sharedBuffers)
    : engineType_(engineType)
    , rootDir_(std::move(rootDir))
    , sharedBuffers_(std::move(sharedBuffers)) {}

WorkerRegistry::~WorkerRegistry() {
    shutdown();
}

int WorkerRegistry::createWorker(WorkerSourceKind sourceKind,
                                 const std::string& source,
                                 const std::string& name,
                                 WorkerQueueLimits queueLimits) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int id = nextId_++;
    createdWorkers_++;
    auto worker = std::make_unique<WorkerThread>(
        id, engineType_, sourceKind, source, rootDir_, name, sharedBuffers_, queueLimits);
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
    }
}

bool WorkerRegistry::drainMessages(const MessageCallback& callback) {
    std::vector<std::pair<int, WorkerMessage>> messages;
    std::vector<int> stopped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : workers_) {
            for (auto& message : entry.second->drainMessages()) {
                messages.emplace_back(entry.first, std::move(message));
            }
            if (entry.second->isFinished()) {
                // finishThread publishes Exited before the terminal state. Drain
                // once more after observing that state so the facade cannot miss
                // an exit published between the first drain and this check.
                for (auto& message : entry.second->drainMessages()) {
                    messages.emplace_back(entry.first, std::move(message));
                }
                stopped.push_back(entry.first);
            }
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
    for (auto& entry : workers) {
        entry.second->terminate();
        const auto stats = entry.second->stats();
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
}

size_t WorkerRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return workers_.size();
}

WorkerRegistryStats WorkerRegistry::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    WorkerRegistryStats result;
    result.createdWorkers = createdWorkers_;
    result.activeWorkers = workers_.size();
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

}  // namespace mystral::workers
