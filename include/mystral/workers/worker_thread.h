#pragma once

#include "mystral/js/engine.h"
#include "mystral/workers/shared_buffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mystral::workers {

class WorkerRegistry;
class NativeTaskMailbox;

enum class WorkerSourceKind {
    Script,
    Module
};

enum class WorkerState {
    Created,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed
};

enum class WorkerPostStatus {
    Posted = 0,
    NotRunning = 1,
    QueueFull = 2,
    MessageTooLarge = 3,
    NotFound = 4,
    InvalidTransfer = 5
};

struct WorkerQueueLimits {
    size_t maxMessages = 1024;
    size_t maxMessageBytes = 16 * 1024 * 1024;
    size_t maxQueuedBytes = 64 * 1024 * 1024;
};

struct WorkerRuntimeState {
    uint32_t maxDepth = 2;
    uint32_t maxWorkers = 64;
    uint32_t maxParallelism = 1;
    std::atomic<uint32_t> activeWorkers{0};

    bool tryAcquireWorker();
    void releaseWorker();
};

struct WorkerThreadStats {
    WorkerState state = WorkerState::Created;
    uint64_t processedMessages = 0;
    uint64_t processedTimerCallbacks = 0;
    uint64_t busyNanoseconds = 0;
    uint64_t rejectedInputMessages = 0;
    uint64_t rejectedOutputMessages = 0;
    uint64_t rejectedInputTooLarge = 0;
    uint64_t rejectedInputQueueFull = 0;
    uint64_t rejectedOutputTooLarge = 0;
    uint64_t rejectedOutputQueueFull = 0;
    size_t queuedInputMessages = 0;
    size_t queuedInputBytes = 0;
    size_t queuedOutputMessages = 0;
    size_t queuedOutputBytes = 0;
    size_t peakQueuedInputBytes = 0;
    size_t peakQueuedOutputBytes = 0;
    size_t largestInputMessageBytes = 0;
    size_t largestOutputMessageBytes = 0;
    uint64_t descendantCreatedWorkers = 0;
    uint64_t descendantActiveWorkers = 0;
    uint32_t maxDepth = 0;
};

struct WorkerMessage {
    enum class Type {
        Message,
        Error,
        Ready,
        Exited
    };

    Type type = Type::Message;
    std::string payload;
    std::vector<js::TransferredArrayBuffer> transfers;
};

struct WorkerPayload {
    std::string serialized;
    std::vector<js::TransferredArrayBuffer> transfers;
};

class WorkerThread {
public:
    WorkerThread(int id,
                 js::EngineType engineType,
                 WorkerSourceKind sourceKind,
                 std::string source,
                 std::string rootDir,
                 std::string name,
                 std::shared_ptr<SharedBufferRegistry> sharedBuffers,
                 WorkerQueueLimits queueLimits = {},
                 std::shared_ptr<WorkerRuntimeState> runtimeState = {},
                 uint32_t depth = 1,
                 std::function<void()> outputReadyCallback = {});
    ~WorkerThread();

    void start();
    void requestStop();
    WorkerPostStatus postMessage(
        std::string payload,
        std::vector<js::TransferredArrayBuffer> transfers = {});
    void terminate();
    std::vector<WorkerMessage> drainMessages(size_t maxCount = static_cast<size_t>(-1));
    bool hasPendingMessages() const;

    bool isFinished() const;
    int id() const { return id_; }
    WorkerState state() const { return state_.load(); }
    WorkerThreadStats stats() const;

private:
    struct WorkerTimer {
        std::chrono::steady_clock::time_point due;
        std::chrono::milliseconds interval{0};
        bool repeat = false;
    };

    void threadMain();
    bool setupWorkerGlobals(js::Engine* engine);
    int scheduleTimer(double delayMs, bool repeat);
    void cancelTimer(int id);
    std::vector<int> collectDueTimers();
    bool dispatchTimer(js::Engine* engine, int id);
    bool dispatchMessage(js::Engine* engine, const WorkerPayload& payload);
    bool dispatchChildMessages(js::Engine* engine);
    bool dispatchNativeTaskMessages(js::Engine* engine);
    WorkerPostStatus enqueueOutput(
        WorkerMessage::Type type,
        std::string payload,
        std::vector<js::TransferredArrayBuffer> transfers = {});
    void finishThread(js::Engine* engine, WorkerState finalState);

    int id_ = 0;
    js::EngineType engineType_ = js::EngineType::Unknown;
    WorkerSourceKind sourceKind_ = WorkerSourceKind::Script;
    std::string source_;
    std::string rootDir_;
    std::string name_;
    std::shared_ptr<SharedBufferRegistry> sharedBuffers_;
    std::shared_ptr<WorkerRuntimeState> runtimeState_;
    WorkerQueueLimits queueLimits_;
    uint32_t depth_ = 1;
    std::function<void()> outputReadyCallback_;
    std::unique_ptr<WorkerRegistry> childRegistry_;
    std::shared_ptr<NativeTaskMailbox> nativeTaskMailbox_;
    std::thread thread_;

    mutable std::mutex engineMutex_;
    js::Engine* engine_ = nullptr;
    mutable std::mutex inputMutex_;
    mutable std::mutex outputMutex_;
    std::queue<WorkerPayload> inputQueue_;
    std::queue<WorkerMessage> outputQueue_;
    size_t inputQueuedBytes_ = 0;
    size_t outputQueuedBytes_ = 0;
    size_t peakInputQueuedBytes_ = 0;
    size_t peakOutputQueuedBytes_ = 0;
    std::condition_variable inputCondition_;
    std::atomic<bool> childActivity_{false};
    std::atomic<bool> nativeTaskActivity_{false};
    std::unordered_map<int, WorkerTimer> timers_;
    int nextTimerId_ = 1;
    std::atomic<bool> terminated_{false};
    std::atomic<WorkerState> state_{WorkerState::Created};
    std::atomic<uint64_t> processedMessages_{0};
    std::atomic<uint64_t> processedTimerCallbacks_{0};
    std::atomic<uint64_t> busyNanoseconds_{0};
    std::atomic<uint64_t> rejectedInputMessages_{0};
    std::atomic<uint64_t> rejectedOutputMessages_{0};
    std::atomic<uint64_t> rejectedInputTooLarge_{0};
    std::atomic<uint64_t> rejectedInputQueueFull_{0};
    std::atomic<uint64_t> rejectedOutputTooLarge_{0};
    std::atomic<uint64_t> rejectedOutputQueueFull_{0};
    std::atomic<size_t> largestInputMessageBytes_{0};
    std::atomic<size_t> largestOutputMessageBytes_{0};
};

}  // namespace mystral::workers
