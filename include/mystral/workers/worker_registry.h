#pragma once

#include "mystral/workers/worker_thread.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mystral::workers {

struct WorkerRegistryStats {
    uint64_t createdWorkers = 0;
    uint64_t nestedCreatedWorkers = 0;
    uint64_t activeWorkers = 0;
    uint32_t maxDepth = 0;
    uint64_t processedMessages = 0;
    uint64_t processedTimerCallbacks = 0;
    uint64_t busyNanoseconds = 0;
    uint64_t rejectedInputMessages = 0;
    uint64_t rejectedOutputMessages = 0;
    uint64_t rejectedInputTooLarge = 0;
    uint64_t rejectedInputQueueFull = 0;
    uint64_t rejectedOutputTooLarge = 0;
    uint64_t rejectedOutputQueueFull = 0;
    uint64_t queuedInputMessages = 0;
    uint64_t queuedInputBytes = 0;
    uint64_t queuedOutputMessages = 0;
    uint64_t queuedOutputBytes = 0;
    uint64_t peakQueuedInputBytes = 0;
    uint64_t peakQueuedOutputBytes = 0;
    uint64_t largestInputMessageBytes = 0;
    uint64_t largestOutputMessageBytes = 0;
};

class WorkerRegistry {
public:
    using MessageCallback = std::function<void(int, const WorkerMessage&)>;
    using ActivityCallback = std::function<void()>;

    WorkerRegistry(js::EngineType engineType,
                   std::string rootDir,
                   std::shared_ptr<SharedBufferRegistry> sharedBuffers,
                   std::shared_ptr<WorkerRuntimeState> runtimeState = {},
                   uint32_t ownerDepth = 0,
                   ActivityCallback activityCallback = {});
    ~WorkerRegistry();

    int createWorker(WorkerSourceKind sourceKind,
                     const std::string& source,
                     const std::string& name = {},
                     WorkerQueueLimits queueLimits = {},
                     std::string* error = nullptr);
    WorkerPostStatus postToWorker(
        int id,
        std::string payload,
        std::vector<js::TransferredArrayBuffer> transfers = {});
    WorkerPostStatus postToWorkers(
        const std::vector<int>& ids,
        const std::string& payload);
    void terminateWorker(int id);
    bool drainMessages(const MessageCallback& callback,
                       size_t maxCount = static_cast<size_t>(-1));
    void shutdown();
    size_t size() const;
    uint32_t suggestedWorkerCount() const;
    WorkerRegistryStats stats() const;

    WorkerRegistry(const WorkerRegistry&) = delete;
    WorkerRegistry& operator=(const WorkerRegistry&) = delete;

private:
    js::EngineType engineType_ = js::EngineType::Unknown;
    std::string rootDir_;
    std::shared_ptr<SharedBufferRegistry> sharedBuffers_;
    std::shared_ptr<WorkerRuntimeState> runtimeState_;
    uint32_t ownerDepth_ = 0;
    ActivityCallback activityCallback_;
    std::unordered_map<int, std::unique_ptr<WorkerThread>> workers_;
    int nextId_ = 1;
    uint64_t createdWorkers_ = 0;
    uint64_t completedMessages_ = 0;
    uint64_t completedTimerCallbacks_ = 0;
    uint64_t completedBusyNanoseconds_ = 0;
    uint64_t completedRejectedInputMessages_ = 0;
    uint64_t completedRejectedOutputMessages_ = 0;
    uint64_t completedRejectedInputTooLarge_ = 0;
    uint64_t completedRejectedInputQueueFull_ = 0;
    uint64_t completedRejectedOutputTooLarge_ = 0;
    uint64_t completedRejectedOutputQueueFull_ = 0;
    uint64_t peakQueuedInputBytes_ = 0;
    uint64_t peakQueuedOutputBytes_ = 0;
    uint64_t largestInputMessageBytes_ = 0;
    uint64_t largestOutputMessageBytes_ = 0;
    uint64_t completedDescendantWorkers_ = 0;
    uint32_t maxObservedDepth_ = 0;
    int lastDrainWorkerId_ = 0;
    bool shuttingDown_ = false;
    mutable std::mutex mutex_;
};

bool installWorkerRegistryBindings(js::Engine* engine, WorkerRegistry* registry);
bool dispatchWorkerRegistryMessages(
    js::Engine* engine,
    WorkerRegistry* registry,
    std::string* error = nullptr,
    size_t maxCount = static_cast<size_t>(-1));
const char* nestedWorkerFacadeSource();

}  // namespace mystral::workers
