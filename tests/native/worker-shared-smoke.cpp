#include "mystral/js/engine.h"
#include "mystral/js/module_system.h"
#include "mystral/workers/shared_buffer.h"
#include "mystral/workers/worker_registry.h"
#include "mystral/workers/worker_thread.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

bool waitForWorkerMessage(
    mystral::workers::WorkerThread& worker,
    const std::function<bool(const mystral::workers::WorkerMessage&)>& predicate,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& message : worker.drainMessages()) {
            if (predicate(message)) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}  // namespace

int main() {
    using namespace mystral;

#if defined(MYSTRAL_JS_V8)
    constexpr js::EngineType engineType = js::EngineType::V8;
#elif defined(MYSTRAL_JS_QUICKJS)
    constexpr js::EngineType engineType = js::EngineType::QuickJS;
#else
#error "worker-shared-smoke requires V8 or QuickJS"
#endif

    auto mainEngine = js::createEngine(engineType);
    if (!mainEngine) return 1;

    auto sharedBuffers = std::make_shared<workers::SharedBufferRegistry>();
    workers::installSharedBufferBindings(mainEngine.get(), sharedBuffers);
    if (!mainEngine->evalScript(workers::sharedApiSource(), "mystral-shared.js")) return 2;

    js::ModuleSystem mainModules(mainEngine.get(), std::filesystem::current_path().string());
    js::setModuleSystem(&mainModules);
    if (!mainEngine->eval(
            "import { SharedTable } from 'mystral/shared';\n"
            "import { WorkerPool, exposeWorkerTask, transferResult } from 'mystral/worker-pool';\n"
            "globalThis.__sharedModuleLoaded = typeof SharedTable === 'function' && "
            "typeof WorkerPool === 'function' && typeof exposeWorkerTask === 'function' && "
            "typeof transferResult === 'function';\n",
            "worker-shared-module-smoke.mjs")) {
        return 6;
    }
    auto moduleLoaded = mainEngine->getGlobalProperty("__sharedModuleLoaded");
    if (!mainEngine->toBoolean(moduleLoaded)) return 7;

    if (!mainEngine->evalScript(R"JS(
(() => {
    const fakeWorkers = [];
    globalThis.Worker = class FakeWorker {
        constructor() {
            this.index = fakeWorkers.length;
            this.task = null;
            fakeWorkers.push(this);
        }
        postMessage(task) {
            this.task = task;
            if (this.index < 2) {
                this.respond();
                if (this.index === 0) this.respond();
            }
        }
        respond() {
            const task = this.task;
            this.onmessage({ data: {
                $mystralWorkerPoolResult: 1,
                round: task.round,
                workerIndex: task.workerIndex,
                ok: true,
                value: [task.begin, task.end, task.workerIndex],
            }});
        }
        terminate() {}
    };

    const Rows = __mystralShared.SharedTable.define('test.pool-resize/v1', { value: 'i32' });
    const rows = Rows.create({ capacity: 1 });
    rows.length = 1;
    rows.value[0] = 77;
    const pool = new __mystralWorkerPool.WorkerPool('fake-worker.mjs', { size: 3 });
    const pending = pool.run({ tag: 'round-1' }, { length: 10 });
    if (!pool.busy) throw new Error('duplicate WorkerPool result completed the barrier early');
    let resizeBlocked = false;
    try { pool.resizeTable(Rows, rows, { capacity: 2 }); } catch (_) { resizeBlocked = true; }
    if (!resizeBlocked) throw new Error('WorkerPool resized shared state during an active round');
    fakeWorkers[2].respond();
    if (pool.busy) throw new Error('WorkerPool barrier did not complete');
    const resized = pool.resizeTable(Rows, rows, { capacity: 2 });
    if (resized.length !== 1 || resized.value[0] !== 77) {
        throw new Error('WorkerPool safe resize lost table data');
    }
    globalThis.__workerPoolResult = 'pending';
    pending.then(
        results => { globalThis.__workerPoolResult = JSON.stringify(results); },
        error => { globalThis.__workerPoolResult = 'error:' + error.message; });
    pool.terminate();
    resized.release();
})()
)JS", "worker-pool-smoke.js")) {
        return 23;
    }
    const auto poolDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::string poolResult;
    while (std::chrono::steady_clock::now() < poolDeadline) {
        mainEngine->evalScript("void 0", "worker-pool-microtask.js");
        poolResult = mainEngine->toString(mainEngine->getGlobalProperty("__workerPoolResult"));
        if (poolResult != "pending") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (poolResult != "[[0,4,0],[4,7,1],[7,10,2]]") {
        std::cerr << "WorkerPool deterministic barrier failed: " << poolResult << std::endl;
        return 24;
    }

    auto payloadValue = mainEngine->evalScriptWithResult(R"JS(
(() => {
    const { SharedQueue, SharedTable } = globalThis.__mystralShared;
    const Rows = SharedTable.define('test.rows/v1', { energy: 'i32' });
    const rows = Rows.create({ capacity: 4 });
    globalThis.__sharedRowsId = rows.sharedBuffer.id;
    rows.length = 1;
    rows.energy[0] = 1;
    let mismatchRejected = false;
    try {
        SharedTable.define('test.rows/v2', { energy: 'u32' }).attach(rows.handle());
    } catch (_) {
        mismatchRejected = true;
    }
    if (!mismatchRejected) throw new Error('table schema mismatch was accepted');

    const ResizeRows = SharedTable.define('test.resize-rows/v1', { energy: 'i32', x: 'f32' });
    const resizeRows = ResizeRows.create({ capacity: 2 });
    resizeRows.length = 2;
    resizeRows.energy[0] = 11;
    resizeRows.energy[1] = 22;
    resizeRows.x[1] = 3.5;
    const grownRows = ResizeRows.resize(resizeRows, { capacity: 4 });
    if (grownRows.capacity !== 4 || grownRows.length !== 2 ||
        grownRows.energy[0] !== 11 || grownRows.energy[1] !== 22 || grownRows.x[1] !== 3.5) {
        throw new Error('SharedTable growth migration failed');
    }
    const shrunkRows = ResizeRows.resize(grownRows, { capacity: 1 });
    if (shrunkRows.capacity !== 1 || shrunkRows.length !== 1 || shrunkRows.energy[0] !== 11) {
        throw new Error('SharedTable shrink migration failed');
    }
    grownRows.release();
    shrunkRows.release();

    const Commands = SharedQueue.define('test.commands/v1', { kind: 'u32', value: 'f32' });
    const commands = Commands.create({ capacity: 2 });
    if (!commands.push({ kind: 7, value: 3.5 })) throw new Error('queue push failed');
    if (!commands.push({ kind: 8, value: 4.5 })) throw new Error('queue second push failed');
    if (commands.push({ kind: 99, value: 99 })) throw new Error('queue overflow was accepted');
    const firstCommand = commands.pop();
    if (!firstCommand || firstCommand.kind !== 7 || firstCommand.value !== 3.5) throw new Error('queue pop failed');
    if (!commands.push({ kind: 9, value: 5.5 })) throw new Error('queue wrap push failed');
    const secondCommand = commands.pop();
    const wrappedCommand = commands.pop();
    if (!secondCommand || secondCommand.kind !== 8 ||
        !wrappedCommand || wrappedCommand.kind !== 9 || commands.pop() !== null) {
        throw new Error('queue wrap order failed');
    }

    return globalThis.__mystralSerializeMessage({ characters: rows.handle() });
})()
)JS", "worker-shared-smoke-main.js");
    if (!payloadValue.ptr) return 3;
    std::string payload = mainEngine->toString(payloadValue);

    workers::WorkerThread worker(
        1,
        engineType,
        workers::WorkerSourceKind::Module,
        "./tests/native/fixtures/worker-shared-module.mjs",
        std::filesystem::current_path().string(),
        "worker-shared-smoke-worker.js",
        sharedBuffers);
    worker.start();
    worker.postMessage(payload);

    bool received = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !received) {
        for (const auto& message : worker.drainMessages()) {
            if (message.type == workers::WorkerMessage::Type::Error) {
                std::cerr << message.payload << std::endl;
                worker.terminate();
                return 4;
            }
            if (message.type == workers::WorkerMessage::Type::Message &&
                message.payload.find("42") != std::string::npos) {
                received = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto rowsId = static_cast<uint64_t>(
        mainEngine->toNumber(mainEngine->getGlobalProperty("__sharedRowsId")));
    auto storage = sharedBuffers->attach(rowsId);
    int32_t value = 0;
    constexpr size_t tableHeaderBytes = 64;
    if (storage && storage->size >= tableHeaderBytes + sizeof(value)) {
        std::memcpy(&value, storage->bytes.get() + tableHeaderBytes, sizeof(value));
    }
    if (!received || value != 42) {
        std::cerr << "Expected shared value 42, got " << value << std::endl;
        worker.terminate();
        return 5;
    }

    const uint8_t sourceBytes[] = {4, 5, 6, 7};
    auto sourceBuffer = mainEngine->newArrayBuffer(sourceBytes, sizeof(sourceBytes));
    js::TransferredArrayBuffer outboundTransfer;
    if (!mainEngine->transferArrayBuffer(sourceBuffer, outboundTransfer)) {
        std::cerr << "Main isolate could not detach a transferable ArrayBuffer" << std::endl;
        worker.terminate();
        return 19;
    }
    size_t detachedSize = 999;
    mainEngine->getArrayBufferData(sourceBuffer, &detachedSize);
    if (detachedSize != 0 ||
        worker.postMessage(
            R"JSON({"binary":{"$mystralTransferredBuffer":0}})JSON",
            {outboundTransfer}) != workers::WorkerPostStatus::Posted) {
        std::cerr << "Transfer sender was not detached or message was rejected" << std::endl;
        worker.terminate();
        return 20;
    }

    bool binaryReceived = false;
    const auto binaryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!binaryReceived && std::chrono::steady_clock::now() < binaryDeadline) {
        for (const auto& message : worker.drainMessages()) {
            if (message.type == workers::WorkerMessage::Type::Error) {
                std::cerr << message.payload << std::endl;
                worker.terminate();
                return 21;
            }
            if (message.type != workers::WorkerMessage::Type::Message || message.transfers.size() != 1) {
                continue;
            }
            auto returnedBuffer = mainEngine->newTransferredArrayBuffer(message.transfers[0]);
            size_t returnedSize = 0;
            auto* returnedBytes = static_cast<uint8_t*>(
                mainEngine->getArrayBufferData(returnedBuffer, &returnedSize));
            binaryReceived = returnedBytes && returnedSize == sizeof(sourceBytes) &&
                returnedBytes[0] == 5 && returnedBytes[1] == 5;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.terminate();
    if (!binaryReceived) {
        std::cerr << "Transferred ArrayBuffer did not round-trip through the Worker" << std::endl;
        return 22;
    }

    workers::WorkerQueueLimits tightLimits;
    tightLimits.maxMessages = 2;
    tightLimits.maxBytes = 64;
    workers::WorkerThread blockedWorker(
        2,
        engineType,
        workers::WorkerSourceKind::Script,
        "self.onmessage = () => { postMessage({started:true}); while (true) {} };",
        std::filesystem::current_path().string(),
        "worker-blocked-smoke.js",
        sharedBuffers,
        tightLimits);
    blockedWorker.start();
    if (!waitForWorkerMessage(blockedWorker, [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Ready;
        })) {
        std::cerr << "Blocked Worker did not become ready" << std::endl;
        blockedWorker.terminate();
        return 8;
    }
    if (blockedWorker.postMessage("{}") != workers::WorkerPostStatus::Posted ||
        !waitForWorkerMessage(blockedWorker, [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Message &&
                message.payload.find("started") != std::string::npos;
        })) {
        std::cerr << "Blocked Worker did not enter its message handler" << std::endl;
        blockedWorker.terminate();
        return 9;
    }
    if (blockedWorker.postMessage("{\"id\":1}") != workers::WorkerPostStatus::Posted ||
        blockedWorker.postMessage("{\"id\":2}") != workers::WorkerPostStatus::Posted ||
        blockedWorker.postMessage("{\"id\":3}") != workers::WorkerPostStatus::QueueFull ||
        blockedWorker.postMessage(std::string(65, 'x')) != workers::WorkerPostStatus::MessageTooLarge) {
        std::cerr << "Worker input backpressure contract failed" << std::endl;
        blockedWorker.terminate();
        return 10;
    }
    const auto terminateStartedAt = std::chrono::steady_clock::now();
    blockedWorker.terminate();
    const auto terminateElapsed = std::chrono::steady_clock::now() - terminateStartedAt;
    if (terminateElapsed > std::chrono::seconds(2) ||
        blockedWorker.state() != workers::WorkerState::Stopped ||
        blockedWorker.postMessage("{}") != workers::WorkerPostStatus::NotRunning) {
        std::cerr << "Worker execution was not interrupted cleanly" << std::endl;
        return 11;
    }
    const auto blockedStats = blockedWorker.stats();
    if (blockedStats.rejectedInputMessages != 2 || blockedStats.peakQueuedInputBytes == 0) {
        std::cerr << "Worker input backpressure metrics were not recorded" << std::endl;
        return 12;
    }

    workers::WorkerQueueLimits outputLimits;
    outputLimits.maxMessages = 2;
    outputLimits.maxBytes = 1024;
    workers::WorkerThread outputWorker(
        3,
        engineType,
        workers::WorkerSourceKind::Script,
        "self.onmessage = () => { postMessage({id:1}); postMessage({id:2}); try { postMessage({id:3}); } catch (_) {} };",
        std::filesystem::current_path().string(),
        "worker-output-backpressure-smoke.js",
        sharedBuffers,
        outputLimits);
    outputWorker.start();
    if (!waitForWorkerMessage(outputWorker, [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Ready;
        }) || outputWorker.postMessage("{}") != workers::WorkerPostStatus::Posted) {
        std::cerr << "Output backpressure Worker did not start" << std::endl;
        outputWorker.terminate();
        return 13;
    }
    const auto outputDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < outputDeadline &&
           outputWorker.stats().rejectedOutputMessages == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto outputStats = outputWorker.stats();
    outputWorker.terminate();
    if (outputStats.rejectedOutputMessages == 0 || outputStats.peakQueuedOutputBytes == 0) {
        std::cerr << "Worker output backpressure contract failed" << std::endl;
        return 14;
    }

    workers::WorkerThread failedWorker(
        4,
        engineType,
        workers::WorkerSourceKind::Module,
        "./tests/native/fixtures/does-not-exist.mjs",
        std::filesystem::current_path().string(),
        "worker-startup-failure-smoke.js",
        sharedBuffers);
    failedWorker.start();
    bool sawStartupError = false;
    bool sawStartupExit = false;
    const auto startupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < startupDeadline &&
           (!sawStartupError || !sawStartupExit)) {
        for (const auto& message : failedWorker.drainMessages()) {
            sawStartupError = sawStartupError || message.type == workers::WorkerMessage::Type::Error;
            sawStartupExit = sawStartupExit || message.type == workers::WorkerMessage::Type::Exited;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!sawStartupError || !sawStartupExit ||
        failedWorker.state() != workers::WorkerState::Failed) {
        std::cerr << "Worker startup failure lifecycle was not reported" << std::endl;
        failedWorker.terminate();
        return 15;
    }
    failedWorker.terminate();

    workers::WorkerThread closingWorker(
        5,
        engineType,
        workers::WorkerSourceKind::Script,
        "self.onmessage = () => close();",
        std::filesystem::current_path().string(),
        "worker-close-smoke.js",
        sharedBuffers);
    closingWorker.start();
    const bool closingReady = waitForWorkerMessage(closingWorker, [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Ready;
        });
    const bool closingPosted = closingWorker.postMessage("{}") == workers::WorkerPostStatus::Posted;
    const bool closingExited = waitForWorkerMessage(closingWorker, [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Exited;
        });
    const auto closingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!closingWorker.isFinished() && std::chrono::steady_clock::now() < closingDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!closingReady || !closingPosted || !closingExited ||
        closingWorker.state() != workers::WorkerState::Stopped) {
        std::cerr << "Worker close lifecycle failed" << std::endl;
        closingWorker.terminate();
        return 16;
    }
    closingWorker.terminate();

    workers::WorkerThread timerWorker(
        6,
        engineType,
        workers::WorkerSourceKind::Script,
        R"JS(
let ticks = 0;
let microtaskRan = false;
queueMicrotask(() => { microtaskRan = true; });
const cancelled = setTimeout(() => postMessage({ cancelledTimerRan: true }), 0);
clearTimeout(cancelled);
const interval = setInterval(() => {
    ticks++;
    if (ticks === 2) {
        clearInterval(interval);
        setTimeout(() => postMessage({ ticks, microtaskRan }), 1);
    }
}, 1);
)JS",
        std::filesystem::current_path().string(),
        "worker-timer-smoke.js",
        sharedBuffers);
    timerWorker.start();
    bool timerMessageReceived = false;
    const auto timerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!timerMessageReceived && std::chrono::steady_clock::now() < timerDeadline) {
        for (const auto& message : timerWorker.drainMessages()) {
            if (message.type == workers::WorkerMessage::Type::Error ||
                message.payload.find("cancelledTimerRan") != std::string::npos) {
                std::cerr << "Worker timer failed: " << message.payload << std::endl;
                timerWorker.terminate();
                return 25;
            }
            timerMessageReceived = message.type == workers::WorkerMessage::Type::Message &&
                message.payload.find("\"ticks\":2") != std::string::npos &&
                message.payload.find("\"microtaskRan\":true") != std::string::npos;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto timerStats = timerWorker.stats();
    timerWorker.terminate();
    if (!timerMessageReceived || timerStats.processedTimerCallbacks < 3) {
        std::cerr << "Worker timer event loop or metrics failed" << std::endl;
        return 26;
    }

    workers::WorkerThread poolTaskWorker(
        7,
        engineType,
        workers::WorkerSourceKind::Module,
        "./tests/native/fixtures/worker-pool-module.mjs",
        std::filesystem::current_path().string(),
        "worker-pool-task-smoke.js",
        sharedBuffers);
    poolTaskWorker.start();
    if (!waitForWorkerMessage(poolTaskWorker, [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Ready;
        }) || poolTaskWorker.postMessage(
            R"JSON({"$mystralWorkerPoolTask":1,"round":9,"workerIndex":2,"workerCount":4,"begin":5,"end":8,"data":{"tag":"native"}})JSON") !=
            workers::WorkerPostStatus::Posted) {
        std::cerr << "WorkerPool task helper did not start" << std::endl;
        poolTaskWorker.terminate();
        return 27;
    }
    const bool poolTaskReceived = waitForWorkerMessage(
        poolTaskWorker,
        [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Message &&
                message.payload.find("\"round\":9") != std::string::npos &&
                message.payload.find("\"workerIndex\":2") != std::string::npos &&
                message.payload.find("\"begin\":5") != std::string::npos &&
                message.payload.find("\"end\":8") != std::string::npos &&
                message.payload.find("\"tag\":\"native\"") != std::string::npos;
        });
    if (!poolTaskReceived) {
        std::cerr << "WorkerPool task helper did not complete its partition" << std::endl;
        poolTaskWorker.terminate();
        return 28;
    }

    const uint8_t poolBinarySource[] = {9, 8};
    auto poolBinaryBuffer = mainEngine->newArrayBuffer(poolBinarySource, sizeof(poolBinarySource));
    js::TransferredArrayBuffer poolBinaryTransfer;
    if (!mainEngine->transferArrayBuffer(poolBinaryBuffer, poolBinaryTransfer) ||
        poolTaskWorker.postMessage(
            R"JSON({"$mystralWorkerPoolTask":1,"round":10,"workerIndex":2,"workerCount":4,"begin":8,"end":10,"data":{"tag":"binary","binary":{"$mystralTransferredBuffer":0}}})JSON",
            {poolBinaryTransfer}) != workers::WorkerPostStatus::Posted) {
        std::cerr << "WorkerPool transferable result task was rejected" << std::endl;
        poolTaskWorker.terminate();
        return 29;
    }
    bool poolBinaryReceived = false;
    const auto poolBinaryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!poolBinaryReceived && std::chrono::steady_clock::now() < poolBinaryDeadline) {
        for (const auto& message : poolTaskWorker.drainMessages()) {
            if (message.type != workers::WorkerMessage::Type::Message ||
                message.payload.find("\"round\":10") == std::string::npos ||
                message.transfers.size() != 1) {
                continue;
            }
            auto returnedBuffer = mainEngine->newTransferredArrayBuffer(message.transfers[0]);
            size_t returnedSize = 0;
            auto* returnedBytes = static_cast<uint8_t*>(
                mainEngine->getArrayBufferData(returnedBuffer, &returnedSize));
            poolBinaryReceived = returnedBytes && returnedSize == sizeof(poolBinarySource) &&
                returnedBytes[0] == 10 && returnedBytes[1] == 8;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    poolTaskWorker.terminate();
    if (!poolBinaryReceived) {
        std::cerr << "WorkerPool transferResult did not return its ArrayBuffer" << std::endl;
        return 30;
    }

    workers::WorkerRegistry exitRegistry(
        engineType,
        std::filesystem::current_path().string(),
        sharedBuffers);
    exitRegistry.createWorker(
        workers::WorkerSourceKind::Script,
        "close();",
        "worker-registry-exit-smoke.js");
    bool registrySawExit = false;
    const auto registryExitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!registrySawExit && std::chrono::steady_clock::now() < registryExitDeadline) {
        exitRegistry.drainMessages([&registrySawExit](int, const workers::WorkerMessage& message) {
            registrySawExit = registrySawExit || message.type == workers::WorkerMessage::Type::Exited;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!registrySawExit || exitRegistry.size() != 0) {
        std::cerr << "Worker registry lost the exit lifecycle message" << std::endl;
        exitRegistry.shutdown();
        return 17;
    }

    workers::WorkerRegistry shutdownRegistry(
        engineType,
        std::filesystem::current_path().string(),
        sharedBuffers);
    constexpr int rapidWorkerCount = 4;
    for (int index = 0; index < rapidWorkerCount; index++) {
        shutdownRegistry.createWorker(
            workers::WorkerSourceKind::Script,
            "self.onmessage = () => {};",
            "worker-rapid-shutdown-smoke.js");
    }
    const auto shutdownStartedAt = std::chrono::steady_clock::now();
    shutdownRegistry.shutdown();
    const auto shutdownElapsed = std::chrono::steady_clock::now() - shutdownStartedAt;
    const auto shutdownStats = shutdownRegistry.stats();
    if (shutdownElapsed > std::chrono::seconds(5) || shutdownRegistry.size() != 0 ||
        shutdownStats.createdWorkers != rapidWorkerCount || shutdownStats.activeWorkers != 0) {
        std::cerr << "Worker registry rapid shutdown failed" << std::endl;
        return 18;
    }

    std::cout << "worker shared-memory and lifecycle smoke tests passed" << std::endl;
    mainModules.clearCaches();
    js::setModuleSystem(nullptr);
    return 0;
}
