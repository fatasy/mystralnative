#include "mystral/async/job_system.h"
#include "mystral/js/engine.h"
#include "mystral/js/module_system.h"
#include "mystral/workers/native_task.h"
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

mystral::js::JSValueHandle decodeWorkerMessage(
    mystral::js::Engine* engine,
    const mystral::workers::WorkerMessage& message) {
    auto transfers = engine->newArray(message.transfers.size());
    for (uint32_t index = 0; index < message.transfers.size(); index++) {
        auto buffer = engine->newTransferredArrayBuffer(message.transfers[index]);
        engine->setPropertyIndex(transfers, index, buffer);
        engine->releaseValue(buffer);
    }
    auto parser = engine->getGlobalProperty("__mystralParseMessage");
    auto payload = engine->newString(message.payload.c_str());
    auto thisArg = engine->newUndefined();
    auto result = engine->call(parser, thisArg, {payload, transfers});
    engine->releaseValue(thisArg);
    engine->releaseValue(payload);
    engine->releaseValue(parser);
    engine->releaseValue(transfers);
    return result;
}

std::string decodedMessageJson(
    mystral::js::Engine* engine,
    const mystral::workers::WorkerMessage& message) {
    auto decoded = decodeWorkerMessage(engine, message);
    auto serializer = engine->getGlobalProperty("__mystralSerializeMessage");
    auto thisArg = engine->newUndefined();
    auto serialized = engine->call(serializer, thisArg, {decoded});
    const std::string result = engine->toString(serialized);
    engine->releaseValue(serialized);
    engine->releaseValue(thisArg);
    engine->releaseValue(serializer);
    engine->releaseValue(decoded);
    return result;
}

}  // namespace

int main() {
    using namespace mystral;

#if defined(MYSTRAL_JS_V8)
    constexpr js::EngineType engineType = js::EngineType::V8;
#else
#error "worker-shared-smoke requires V8"
#endif

    auto mainEngine = js::createEngine(engineType);
    if (!mainEngine) return 1;

    auto sharedBuffers = std::make_shared<workers::SharedBufferRegistry>();
    workers::installSharedBufferBindings(mainEngine.get(), sharedBuffers);
    if (!mainEngine->evalScript(workers::sharedApiSource(), "mystral-shared.js")) return 2;

    js::ModuleSystem mainModules(mainEngine.get(), std::filesystem::current_path().string());
    js::setModuleSystem(&mainModules);
    if (!mainEngine->eval(
            "import { SharedCommandBuffer, SharedTable } from 'mystral/shared';\n"
            "import { WorkerPool, exposeWorkerTasks, transferResult } from 'mystral/worker-pool';\n"
            "globalThis.__sharedModuleLoaded = typeof SharedTable === 'function' && "
            "typeof SharedCommandBuffer === 'function' && "
            "typeof WorkerPool === 'function' && typeof exposeWorkerTasks === 'function' && "
            "typeof transferResult === 'function' && "
            "typeof WorkerPool.prototype.run === 'undefined' && "
            "typeof WorkerPool.prototype.runTask === 'undefined' && "
            "typeof WorkerPool.prototype.terminate === 'undefined' && "
            "!('exposeWorkerTask' in globalThis.__mystralWorkerPool) && "
            "!('deterministicPartitions' in globalThis.__mystralWorkerPool) && "
            "!('deterministicReduce' in globalThis.__mystralWorkerPool);\n",
            "worker-shared-module-smoke.mjs")) {
        return 6;
    }
    auto moduleLoaded = mainEngine->getGlobalProperty("__sharedModuleLoaded");
    if (!mainEngine->toBoolean(moduleLoaded)) return 7;

    if (!mainEngine->evalScript(R"JS(
(() => {
    globalThis.__workerPoolResult = 'pending';
    const fakeWorkers = [];
    globalThis.Worker = class FakeWorker {
        constructor() {
            this.index = fakeWorkers.length;
            this.task = null;
            fakeWorkers.push(this);
        }
        postMessage(task) {
            if (task.$mystralWorkerPoolInit === 1) {
                this.onmessage({ data: {
                    $mystralWorkerPoolReady: 1,
                    workerIndex: this.index,
                }});
                return;
            }
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

    let directConstructionRejected = false;
    try {
        new __mystralWorkerPool.WorkerPool('fake-worker.mjs', { size: 1 });
    } catch (error) {
        directConstructionRejected = String(error.message).includes('WorkerPool.create');
    }
    if (!directConstructionRejected) {
        throw new Error('WorkerPool direct construction remains public');
    }

    (async () => {
        const Rows = __mystralShared.SharedTable.define('test.pool-resize/v1', { value: 'i32' });
        const rows = Rows.create({ capacity: 1 });
        rows.length = 1;
        rows.value[0] = 77;
        const pool = await __mystralWorkerPool.WorkerPool.create(
            'fake-worker.mjs', { size: 3 });
        if (typeof pool.ready !== 'undefined') {
            throw new Error('WorkerPool ready Promise remains public');
        }
        const pending = pool.parallelFor(
            'partition', { tag: 'round-1' }, { length: 10, schedule: 'static' });
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
        const results = await pending;
        await pool.close();
        resized.release();
        globalThis.__workerPoolResult = JSON.stringify(results);
    })().catch(error => {
        globalThis.__workerPoolResult = 'error:' + error.message;
    });
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

    if (!mainEngine->evalScript(R"JS(
(() => {
    const transferred = new ArrayBuffer(16);
    new Uint8Array(transferred).set([1, 2, 3, 4, 5, 6, 7, 8]);
    const cloned = new ArrayBuffer(64);
    const names = [
        'Int8Array', 'Uint8Array', 'Uint8ClampedArray',
        'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array',
        'Float16Array', 'Float32Array', 'Float64Array',
        'BigInt64Array', 'BigUint64Array',
    ].filter(name => typeof globalThis[name] === 'function');
    globalThis.__wireTransferred = transferred;
    globalThis.__wireCloned = cloned;
    globalThis.__wireTypedArrayNames = names;
    let duplicateRejected = false;
    let circularRejected = false;
    let rawSharedRejected = false;
    let invalidBoundsRejected = false;
    try { __mystralPrepareMessage({}, [transferred, transferred]); } catch (_) { duplicateRejected = true; }
    const circular = {};
    circular.self = circular;
    try { __mystralPrepareMessage(circular); } catch (_) { circularRejected = true; }
    const shared = __mystralShared.SharedBuffer.allocate(8);
    try { __mystralPrepareMessage({ raw: new Uint8Array(shared.buffer) }); } catch (_) { rawSharedRejected = true; }
    shared.release();
    try {
        __mystralParseMessage(
            JSON.stringify({ $mystralWire: 1, value: ['view', 'Uint32Array', 0, 2, 1] }),
            [new ArrayBuffer(4)],
        );
    } catch (_) { invalidBoundsRejected = true; }
    if (!duplicateRejected || !circularRejected || !rawSharedRejected || !invalidBoundsRejected) {
        throw new Error('Structured wire validation contract failed');
    }
    globalThis.__wirePrepared = __mystralPrepareMessage({
        kind: 'wire-views',
        bytes: new Uint8Array(transferred, 4, 4),
        words: new Uint16Array(transferred, 4, 2),
        window: new DataView(transferred, 5, 2),
        clonedViews: names.map(name => new globalThis[name](cloned, 0, 1)),
        marker: { $mystralTransferredBuffer: 0, note: 'user-data' },
        envelopeLookalike: { $mystralWire: 1, value: ['arraybuffer', 0] },
    }, [transferred]);
})()
)JS", "worker-wire-prepare.js")) {
        std::cerr << "Main isolate could not prepare structured binary views" << std::endl;
        worker.terminate();
        return 31;
    }

    auto prepared = mainEngine->getGlobalProperty("__wirePrepared");
    const std::string binaryPayload = mainEngine->toString(
        mainEngine->getProperty(prepared, "payload"));
    auto preparedTransfers = mainEngine->getProperty(prepared, "transfers");
    const auto preparedTransferCount = static_cast<uint32_t>(mainEngine->toNumber(
        mainEngine->getProperty(preparedTransfers, "length")));
    std::vector<js::TransferredArrayBuffer> outboundTransfers;
    outboundTransfers.reserve(preparedTransferCount);
    for (uint32_t index = 0; index < preparedTransferCount; index++) {
        js::TransferredArrayBuffer transfer;
        if (!mainEngine->transferArrayBuffer(
                mainEngine->getPropertyIndex(preparedTransfers, index), transfer)) {
            std::cerr << "Prepared binary attachment could not be transferred" << std::endl;
            worker.terminate();
            return 32;
        }
        outboundTransfers.push_back(std::move(transfer));
    }
    const auto senderState = mainEngine->evalScriptWithResult(
        "__wireTransferred.byteLength === 0 && __wireCloned.byteLength === 64",
        "worker-wire-sender-state.js");
    if (!mainEngine->toBoolean(senderState) ||
        worker.postMessage(binaryPayload, std::move(outboundTransfers)) !=
            workers::WorkerPostStatus::Posted) {
        std::cerr << "Transfer detached the wrong backing or the message was rejected" << std::endl;
        worker.terminate();
        return 33;
    }

    bool binaryReceived = false;
    const auto binaryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!binaryReceived && std::chrono::steady_clock::now() < binaryDeadline) {
        for (const auto& message : worker.drainMessages()) {
            if (message.type == workers::WorkerMessage::Type::Error) {
                std::cerr << message.payload << std::endl;
                worker.terminate();
                return 34;
            }
            if (message.type != workers::WorkerMessage::Type::Message || message.transfers.size() != 2) {
                continue;
            }
            auto returnedTransfers = mainEngine->newArray(message.transfers.size());
            for (uint32_t index = 0; index < message.transfers.size(); index++) {
                auto returnedBuffer = mainEngine->newTransferredArrayBuffer(message.transfers[index]);
                mainEngine->setPropertyIndex(returnedTransfers, index, returnedBuffer);
                mainEngine->releaseValue(returnedBuffer);
            }
            auto parser = mainEngine->getGlobalProperty("__mystralParseMessage");
            auto serialized = mainEngine->newString(message.payload.c_str());
            auto thisArg = mainEngine->newUndefined();
            auto roundTrip = mainEngine->call(parser, thisArg, {serialized, returnedTransfers});
            mainEngine->setGlobalProperty("__wireRoundTrip", roundTrip);
            const auto validRoundTrip = mainEngine->evalScriptWithResult(R"JS(
(() => {
    const value = __wireRoundTrip;
    const names = __wireTypedArrayNames;
    return value.kind === 'wire-views-result' &&
        value.bytes instanceof Uint8Array && value.bytes.byteOffset === 4 && value.bytes.length === 4 &&
        value.bytes[0] === 6 && value.words.buffer === value.bytes.buffer &&
        value.window instanceof DataView && value.window.buffer === value.bytes.buffer &&
        value.marker.$mystralTransferredBuffer === 0 && value.marker.note === 'user-data' &&
        value.envelopeLookalike.$mystralWire === 1 &&
        value.envelopeLookalike.value[0] === 'arraybuffer' &&
        value.clonedViews.length === names.length &&
        value.clonedViews.every((view, index) =>
            view.constructor.name === names[index] &&
            view.buffer === value.clonedViews[0].buffer) &&
        new Uint8Array(value.clonedViews[0].buffer)[0] === 91 &&
        __wireCloned.byteLength === 64 && new Uint8Array(__wireCloned)[0] === 0;
})()
)JS", "worker-wire-round-trip.js");
            binaryReceived = mainEngine->toBoolean(validRoundTrip);
            mainEngine->releaseValue(validRoundTrip);
            mainEngine->releaseValue(roundTrip);
            mainEngine->releaseValue(thisArg);
            mainEngine->releaseValue(serialized);
            mainEngine->releaseValue(parser);
            mainEngine->releaseValue(returnedTransfers);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.terminate();
    if (!binaryReceived) {
        std::cerr << "Structured binary views did not round-trip through the Worker" << std::endl;
        return 35;
    }

    workers::WorkerQueueLimits tightLimits;
    tightLimits.maxMessages = 3;
    tightLimits.maxMessageBytes = 128;
    tightLimits.maxQueuedBytes = 200;
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
    if (blockedWorker.postMessage(std::string(100, 'x')) != workers::WorkerPostStatus::Posted ||
        blockedWorker.postMessage(std::string(100, 'x')) != workers::WorkerPostStatus::Posted ||
        blockedWorker.postMessage(std::string(100, 'x')) != workers::WorkerPostStatus::QueueFull ||
        blockedWorker.postMessage(std::string(129, 'x')) != workers::WorkerPostStatus::MessageTooLarge) {
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
    if (blockedStats.rejectedInputMessages != 2 ||
        blockedStats.rejectedInputQueueFull != 1 ||
        blockedStats.rejectedInputTooLarge != 1 ||
        blockedStats.peakQueuedInputBytes == 0 ||
        blockedStats.largestInputMessageBytes != 129) {
        std::cerr << "Worker input backpressure metrics were not recorded" << std::endl;
        return 12;
    }

    constexpr size_t largeMessageBytes = 20 * 1024 * 1024;
    std::string largePayload(largeMessageBytes, 'x');
    largePayload.front() = '"';
    largePayload.back() = '"';
    workers::WorkerThread defaultLimitWorker(
        8,
        engineType,
        workers::WorkerSourceKind::Script,
        "self.onmessage = ({data}) => postMessage({length:data.length});",
        std::filesystem::current_path().string(),
        "worker-default-limit-smoke.js",
        sharedBuffers);
    defaultLimitWorker.start();
    if (!waitForWorkerMessage(defaultLimitWorker, [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Ready;
        }) || defaultLimitWorker.postMessage(largePayload) != workers::WorkerPostStatus::MessageTooLarge ||
        defaultLimitWorker.stats().rejectedInputTooLarge != 1) {
        std::cerr << "Default per-message limit was not enforced" << std::endl;
        defaultLimitWorker.terminate();
        return 36;
    }
    defaultLimitWorker.terminate();

    workers::WorkerQueueLimits largeLimits;
    largeLimits.maxMessages = 4;
    largeLimits.maxMessageBytes = 24 * 1024 * 1024;
    largeLimits.maxQueuedBytes = 32 * 1024 * 1024;
    workers::WorkerThread largeWorker(
        9,
        engineType,
        workers::WorkerSourceKind::Script,
        "self.onmessage = ({data}) => postMessage({length:data.length});",
        std::filesystem::current_path().string(),
        "worker-large-message-smoke.js",
        sharedBuffers,
        largeLimits);
    largeWorker.start();
    if (!waitForWorkerMessage(largeWorker, [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Ready;
        }) || largeWorker.postMessage(std::move(largePayload)) != workers::WorkerPostStatus::Posted) {
        std::cerr << "Configured Worker rejected a message above the default limit" << std::endl;
        largeWorker.terminate();
        return 37;
    }
    const bool largeMessageReceived = waitForWorkerMessage(
        largeWorker,
        [mainEngine = mainEngine.get()](const workers::WorkerMessage& message) {
            if (message.type != workers::WorkerMessage::Type::Message) return false;
            return decodedMessageJson(mainEngine, message).find("\"length\":20971518") !=
                std::string::npos;
        });
    const auto largeStats = largeWorker.stats();
    largeWorker.terminate();
    if (!largeMessageReceived || largeStats.largestInputMessageBytes != largeMessageBytes ||
        largeStats.rejectedInputMessages != 0) {
        std::cerr << "Configured large-message round-trip or metrics failed" << std::endl;
        return 38;
    }

    workers::WorkerQueueLimits outputLimits;
    outputLimits.maxMessages = 2;
    outputLimits.maxMessageBytes = 1024;
    outputLimits.maxQueuedBytes = 1024;
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
    if (outputStats.rejectedOutputMessages == 0 || outputStats.rejectedOutputQueueFull == 0 ||
        outputStats.rejectedOutputTooLarge != 0 || outputStats.peakQueuedOutputBytes == 0 ||
        outputStats.largestOutputMessageBytes == 0) {
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
            if (message.type == workers::WorkerMessage::Type::Error) {
                std::cerr << "Worker timer failed: " << message.payload << std::endl;
                timerWorker.terminate();
                return 25;
            }
            if (message.type != workers::WorkerMessage::Type::Message) continue;
            const std::string decoded = decodedMessageJson(mainEngine.get(), message);
            if (decoded.find("cancelledTimerRan") != std::string::npos) {
                std::cerr << "Worker timer failed: " << decoded << std::endl;
                timerWorker.terminate();
                return 25;
            }
            timerMessageReceived = message.type == workers::WorkerMessage::Type::Message &&
                decoded.find("\"ticks\":2") != std::string::npos &&
                decoded.find("\"microtaskRan\":true") != std::string::npos;
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
            R"JSON({"$mystralWorkerPoolTask":1,"round":9,"taskName":"partition","workerIndex":2,"workerCount":4,"begin":5,"end":8,"data":{"tag":"native"}})JSON") !=
            workers::WorkerPostStatus::Posted) {
        std::cerr << "WorkerPool task helper did not start" << std::endl;
        poolTaskWorker.terminate();
        return 27;
    }
    const bool poolTaskReceived = waitForWorkerMessage(
        poolTaskWorker,
        [mainEngine = mainEngine.get()](const workers::WorkerMessage& message) {
            if (message.type != workers::WorkerMessage::Type::Message) return false;
            const std::string decoded = decodedMessageJson(mainEngine, message);
            return decoded.find("\"round\":9") != std::string::npos &&
                decoded.find("\"workerIndex\":2") != std::string::npos &&
                decoded.find("\"begin\":5") != std::string::npos &&
                decoded.find("\"end\":8") != std::string::npos &&
                decoded.find("\"tag\":\"native\"") != std::string::npos;
        });
    if (!poolTaskReceived) {
        std::cerr << "WorkerPool task helper did not complete its partition" << std::endl;
        poolTaskWorker.terminate();
        return 28;
    }

    if (!mainEngine->evalScript(R"JS(
(() => {
    const binary = new Uint8Array([9, 8]).buffer;
    globalThis.__poolBinaryPrepared = __mystralPrepareMessage({
        $mystralWorkerPoolTask: 1,
        round: 10,
        taskName: 'partition',
        workerIndex: 2,
        workerCount: 4,
        begin: 8,
        end: 10,
        data: { tag: 'binary', binary },
    }, [binary]);
})()
)JS", "worker-pool-binary-prepare.js")) {
        std::cerr << "WorkerPool transferable result task was rejected" << std::endl;
        poolTaskWorker.terminate();
        return 29;
    }
    auto poolPrepared = mainEngine->getGlobalProperty("__poolBinaryPrepared");
    const std::string poolPayload = mainEngine->toString(
        mainEngine->getProperty(poolPrepared, "payload"));
    auto poolPreparedTransfers = mainEngine->getProperty(poolPrepared, "transfers");
    js::TransferredArrayBuffer poolBinaryTransfer;
    if (!mainEngine->transferArrayBuffer(
            mainEngine->getPropertyIndex(poolPreparedTransfers, 0), poolBinaryTransfer) ||
        poolTaskWorker.postMessage(poolPayload, {poolBinaryTransfer}) !=
            workers::WorkerPostStatus::Posted) {
        std::cerr << "WorkerPool transferable result task was rejected" << std::endl;
        poolTaskWorker.terminate();
        return 29;
    }
    bool poolBinaryReceived = false;
    const auto poolBinaryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!poolBinaryReceived && std::chrono::steady_clock::now() < poolBinaryDeadline) {
        for (const auto& message : poolTaskWorker.drainMessages()) {
            if (message.type != workers::WorkerMessage::Type::Message || message.transfers.size() != 1) continue;
            auto decoded = decodeWorkerMessage(mainEngine.get(), message);
            auto value = mainEngine->getProperty(decoded, "value");
            auto returnedBuffer = mainEngine->getProperty(value, "binary");
            size_t returnedSize = 0;
            auto* returnedBytes = static_cast<uint8_t*>(
                mainEngine->getArrayBufferData(returnedBuffer, &returnedSize));
            poolBinaryReceived = returnedBytes && returnedSize == 2 &&
                returnedBytes[0] == 10 && returnedBytes[1] == 8;
            mainEngine->releaseValue(returnedBuffer);
            mainEngine->releaseValue(value);
            mainEngine->releaseValue(decoded);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    poolTaskWorker.terminate();
    if (!poolBinaryReceived) {
        std::cerr << "WorkerPool transferResult did not return its ArrayBuffer" << std::endl;
        return 30;
    }

    if (!mainEngine->eval(R"JS(
import { NestedRows } from './tests/native/fixtures/nested-worker-schema.mjs';
globalThis.__nestedRows = NestedRows.create({ capacity: 10 });
globalThis.__nestedRows.length = 10;
globalThis.__nestedRows.energy.fill(1);
globalThis.__nestedPrepared = __mystralPrepareMessage({
    rows: globalThis.__nestedRows.handle(),
    length: globalThis.__nestedRows.length,
});
)JS", "nested-worker-prepare.mjs")) {
        std::cerr << "Nested WorkerPool shared state setup failed" << std::endl;
        return 39;
    }
    auto nestedPrepared = mainEngine->getGlobalProperty("__nestedPrepared");
    const std::string nestedPayload = mainEngine->toString(
        mainEngine->getProperty(nestedPrepared, "payload"));
    mainEngine->releaseValue(nestedPrepared);

    auto nestedRuntime = std::make_shared<workers::WorkerRuntimeState>();
    nestedRuntime->maxDepth = 2;
    nestedRuntime->maxWorkers = 8;
    workers::WorkerThread nestedOwner(
        8,
        engineType,
        workers::WorkerSourceKind::Module,
        "./tests/native/fixtures/nested-worker-owner.mjs",
        std::filesystem::current_path().string(),
        "nested-worker-owner-smoke.js",
        sharedBuffers,
        {},
        nestedRuntime,
        1);
    nestedOwner.start();
    if (!waitForWorkerMessage(nestedOwner, [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Ready;
        }) || nestedOwner.postMessage(nestedPayload) != workers::WorkerPostStatus::Posted) {
        std::cerr << "Nested WorkerPool owner did not start" << std::endl;
        nestedOwner.terminate();
        return 40;
    }
    const bool nestedResultReceived = waitForWorkerMessage(
        nestedOwner,
        [mainEngine = mainEngine.get()](const workers::WorkerMessage& message) {
            if (message.type != workers::WorkerMessage::Type::Message) return false;
            const std::string decoded = decodedMessageJson(mainEngine, message);
            return decoded.find("\"kind\":\"nested-result\"") != std::string::npos &&
                decoded.find("\"begin\":0,\"end\":4,\"workerIndex\":0") != std::string::npos &&
                decoded.find("\"begin\":4,\"end\":7,\"workerIndex\":1") != std::string::npos &&
                decoded.find("\"begin\":7,\"end\":10,\"workerIndex\":2") != std::string::npos;
        });
    if (!nestedResultReceived || !mainEngine->evalScript(R"JS(
globalThis.__nestedRowsMatch =
    Array.from(__nestedRows.energy).join(',') === '2,2,2,2,3,3,3,4,4,4';
)JS", "nested-worker-verify.js")) {
        std::cerr << "Nested WorkerPool deterministic shared round failed" << std::endl;
        nestedOwner.terminate();
        return 41;
    }
    auto nestedRowsMatch = mainEngine->getGlobalProperty("__nestedRowsMatch");
    const auto nestedStats = nestedOwner.stats();
    const bool nestedMetricsMatch = nestedStats.descendantCreatedWorkers == 3 &&
        nestedStats.descendantActiveWorkers == 3 && nestedStats.maxDepth == 2;
    if (!mainEngine->toBoolean(nestedRowsMatch) || !nestedMetricsMatch) {
        std::cerr << "Nested WorkerPool shared data or metrics failed" << std::endl;
        mainEngine->releaseValue(nestedRowsMatch);
        nestedOwner.terminate();
        return 42;
    }
    mainEngine->releaseValue(nestedRowsMatch);
    nestedOwner.terminate();
    if (nestedRuntime->activeWorkers.load() != 0) {
        std::cerr << "Nested WorkerPool cascade shutdown leaked Workers" << std::endl;
        return 43;
    }

    if (!mainEngine->evalScript(R"JS(
globalThis.__nestedFailurePrepared = __mystralPrepareMessage({
    rows: globalThis.__nestedRows.handle(),
    length: globalThis.__nestedRows.length,
    fail: true,
});
)JS", "nested-worker-failure-prepare.js")) {
        return 44;
    }
    auto nestedFailurePrepared = mainEngine->getGlobalProperty("__nestedFailurePrepared");
    const std::string nestedFailurePayload = mainEngine->toString(
        mainEngine->getProperty(nestedFailurePrepared, "payload"));
    mainEngine->releaseValue(nestedFailurePrepared);
    workers::WorkerThread nestedFailureOwner(
        9,
        engineType,
        workers::WorkerSourceKind::Module,
        "./tests/native/fixtures/nested-worker-owner.mjs",
        std::filesystem::current_path().string(),
        "nested-worker-failure-owner-smoke.js",
        sharedBuffers,
        {},
        nestedRuntime,
        1);
    nestedFailureOwner.start();
    if (!waitForWorkerMessage(nestedFailureOwner, [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Ready;
        }) || nestedFailureOwner.postMessage(nestedFailurePayload) !=
            workers::WorkerPostStatus::Posted ||
        !waitForWorkerMessage(
            nestedFailureOwner,
            [mainEngine = mainEngine.get()](const workers::WorkerMessage& message) {
                if (message.type != workers::WorkerMessage::Type::Message) return false;
                const std::string decoded = decodedMessageJson(mainEngine, message);
                return decoded.find("\"kind\":\"nested-error\"") != std::string::npos &&
                    decoded.find("intentional nested Worker failure") != std::string::npos;
            })) {
        std::cerr << "Nested WorkerPool failure propagation failed" << std::endl;
        nestedFailureOwner.terminate();
        return 45;
    }
    nestedFailureOwner.terminate();
    if (nestedRuntime->activeWorkers.load() != 0) {
        std::cerr << "Failed nested WorkerPool leaked siblings" << std::endl;
        return 46;
    }

    if (!mainEngine->evalScript(R"JS(
globalThis.__nestedHangPrepared = __mystralPrepareMessage({
    rows: globalThis.__nestedRows.handle(),
    length: globalThis.__nestedRows.length,
    hang: true,
});
)JS", "nested-worker-hang-prepare.js")) {
        return 49;
    }
    auto nestedHangPrepared = mainEngine->getGlobalProperty("__nestedHangPrepared");
    const std::string nestedHangPayload = mainEngine->toString(
        mainEngine->getProperty(nestedHangPrepared, "payload"));
    mainEngine->releaseValue(nestedHangPrepared);
    workers::WorkerThread nestedHangOwner(
        10,
        engineType,
        workers::WorkerSourceKind::Module,
        "./tests/native/fixtures/nested-worker-owner.mjs",
        std::filesystem::current_path().string(),
        "nested-worker-hang-owner-smoke.js",
        sharedBuffers,
        {},
        nestedRuntime,
        1);
    nestedHangOwner.start();
    if (!waitForWorkerMessage(nestedHangOwner, [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Ready;
        }) || nestedHangOwner.postMessage(nestedHangPayload) !=
            workers::WorkerPostStatus::Posted) {
        std::cerr << "Nested WorkerPool hang scenario did not start" << std::endl;
        nestedHangOwner.terminate();
        return 50;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto nestedShutdownStarted = std::chrono::steady_clock::now();
    nestedHangOwner.terminate();
    const auto nestedShutdownElapsed =
        std::chrono::steady_clock::now() - nestedShutdownStarted;
    if (nestedShutdownElapsed > std::chrono::seconds(5) ||
        nestedRuntime->activeWorkers.load() != 0) {
        std::cerr << "Nested WorkerPool cascade interruption failed" << std::endl;
        return 51;
    }

    auto depthLimitedRuntime = std::make_shared<workers::WorkerRuntimeState>();
    depthLimitedRuntime->maxDepth = 1;
    workers::WorkerThread depthLimitedOwner(
        10,
        engineType,
        workers::WorkerSourceKind::Script,
        R"JS(
let poolError = '';
(async () => {
    try {
        await WorkerPool.create('./tests/native/fixtures/nested-worker-task.mjs', { size: 1 });
    } catch (error) {
        poolError = error && error.message ? error.message : String(error);
    }
    postMessage({ kind: 'depth-limit', workerType: typeof Worker, poolError });
    close();
})();
)JS",
        std::filesystem::current_path().string(),
        "nested-worker-depth-limit-smoke.js",
        sharedBuffers,
        {},
        depthLimitedRuntime,
        1);
    depthLimitedOwner.start();
    const bool depthLimitReported = waitForWorkerMessage(
        depthLimitedOwner,
        [mainEngine = mainEngine.get()](const workers::WorkerMessage& message) {
            if (message.type != workers::WorkerMessage::Type::Message) return false;
            const std::string decoded = decodedMessageJson(mainEngine, message);
            return decoded.find("\"workerType\":\"undefined\"") != std::string::npos &&
                decoded.find("nesting depth") != std::string::npos;
        });
    depthLimitedOwner.terminate();
    if (!depthLimitReported) {
        std::cerr << "Nested Worker depth limit was not enforced" << std::endl;
        return 47;
    }

    auto workerLimitedRuntime = std::make_shared<workers::WorkerRuntimeState>();
    workerLimitedRuntime->maxDepth = 2;
    workerLimitedRuntime->maxWorkers = 2;
    workers::WorkerRegistry workerLimitedRegistry(
        engineType,
        std::filesystem::current_path().string(),
        sharedBuffers,
        workerLimitedRuntime,
        1);
    std::string workerLimitError;
    const int limitedWorker0 = workerLimitedRegistry.createWorker(
        workers::WorkerSourceKind::Script,
        "self.onmessage = () => {};",
        "nested-worker-limit-0.js",
        {},
        &workerLimitError);
    const int limitedWorker1 = workerLimitedRegistry.createWorker(
        workers::WorkerSourceKind::Script,
        "self.onmessage = () => {};",
        "nested-worker-limit-1.js",
        {},
        &workerLimitError);
    const int rejectedWorker = workerLimitedRegistry.createWorker(
        workers::WorkerSourceKind::Script,
        "self.onmessage = () => {};",
        "nested-worker-limit-rejected.js",
        {},
        &workerLimitError);
    workerLimitedRegistry.shutdown();
    if (limitedWorker0 < 0 || limitedWorker1 < 0 || rejectedWorker >= 0 ||
        workerLimitError.find("runtime worker limit") == std::string::npos ||
        workerLimitedRuntime->activeWorkers.load() != 0) {
        std::cerr << "Nested Worker global limit or cleanup failed" << std::endl;
        return 48;
    }

    if (!mainEngine->eval(R"JS(
import { V2Commands, V2Rows } from './tests/native/fixtures/worker-pool-v2-schema.mjs';
globalThis.__v2Rows = V2Rows.create({ capacity: 10 });
globalThis.__v2Rows.length = 10;
globalThis.__v2Rows.energy.fill(1);
globalThis.__v2Commands = V2Commands.create({ workerCount: 3, laneCapacity: 8 });
globalThis.__v2Prepared = __mystralPrepareMessage({
    rows: globalThis.__v2Rows.handle(),
    commands: globalThis.__v2Commands.handle(),
    length: globalThis.__v2Rows.length,
});

const UnitCommands = __mystralShared.SharedCommandBuffer.define(
    'test.worker-pool-v2.unit-commands/v1', { order: 'u32' });
const unitCommands = UnitCommands.create({ workerCount: 2, laneCapacity: 1 });
const unitAccepted = unitCommands.push(1, { order: 9 }) &&
    unitCommands.push(0, { order: 3 }) &&
    !unitCommands.push(0, { order: 4 });
const unitDrained = unitCommands.drain({ sortBy: 'order' });
globalThis.__v2CommandUnitMatch = unitAccepted && unitCommands.size === 0 &&
    unitDrained.map(command => command.order).join(',') === '3,9';
unitCommands.release();
)JS", "worker-pool-v2-prepare.mjs")) {
        std::cerr << "WorkerPool v2 shared setup failed" << std::endl;
        return 52;
    }
    auto commandUnitMatch = mainEngine->getGlobalProperty("__v2CommandUnitMatch");
    if (!mainEngine->toBoolean(commandUnitMatch)) {
        mainEngine->releaseValue(commandUnitMatch);
        std::cerr << "SharedCommandBuffer deterministic drain failed" << std::endl;
        return 53;
    }
    mainEngine->releaseValue(commandUnitMatch);
    auto v2Prepared = mainEngine->getGlobalProperty("__v2Prepared");
    const std::string v2Payload = mainEngine->toString(
        mainEngine->getProperty(v2Prepared, "payload"));
    mainEngine->releaseValue(v2Prepared);

    auto v2Runtime = std::make_shared<workers::WorkerRuntimeState>();
    v2Runtime->maxDepth = 2;
    v2Runtime->maxWorkers = 8;
    v2Runtime->maxParallelism = 3;
    workers::WorkerThread v2Owner(
        11,
        engineType,
        workers::WorkerSourceKind::Module,
        "./tests/native/fixtures/worker-pool-v2-owner.mjs",
        std::filesystem::current_path().string(),
        "worker-pool-v2-owner-smoke.js",
        sharedBuffers,
        {},
        v2Runtime,
        1);
    v2Owner.start();
    const bool v2Ready = waitForWorkerMessage(v2Owner, [](const workers::WorkerMessage& message) {
        return message.type == workers::WorkerMessage::Type::Ready;
    });
    const bool v2Result = v2Ready &&
        v2Owner.postMessage(v2Payload) == workers::WorkerPostStatus::Posted &&
        waitForWorkerMessage(
            v2Owner,
            [mainEngine = mainEngine.get()](const workers::WorkerMessage& message) {
                if (message.type != workers::WorkerMessage::Type::Message) return false;
                const std::string decoded = decodedMessageJson(mainEngine, message);
                return decoded.find("\"kind\":\"worker-pool-v2\"") != std::string::npos &&
                    decoded.find("\"updated\":10") != std::string::npos &&
                    decoded.find("\"queuedAbort\":\"AbortError\"") != std::string::npos &&
                    decoded.find("\"activeAbort\":\"AbortError\"") != std::string::npos &&
                    decoded.find("\"staticAbort\":\"AbortError\"") != std::string::npos &&
                    decoded.find("\"startupTimeout\":true") != std::string::npos &&
                    decoded.find("\"ready\":true") != std::string::npos &&
                    decoded.find("\"broadcastRounds\":3") != std::string::npos &&
                    decoded.find("\"barrierRounds\":2") != std::string::npos &&
                    decoded.find("\"completedRounds\":5") != std::string::npos &&
                    decoded.find("\"begin\":0,\"end\":3,\"workerIndex\":0") != std::string::npos &&
                    decoded.find("\"begin\":3,\"end\":6,\"workerIndex\":1") != std::string::npos &&
                    decoded.find("\"begin\":6,\"end\":8,\"workerIndex\":2") != std::string::npos &&
                    decoded.find("\"order\":0") != std::string::npos &&
                    decoded.find("\"order\":8") != std::string::npos;
            });
    v2Owner.terminate();
    if (!v2Result || v2Runtime->activeWorkers.load() != 0 ||
        !mainEngine->evalScript(R"JS(
globalThis.__v2RowsMatch = Array.from(__v2Rows.energy).every(value => value === 2);
)JS", "worker-pool-v2-verify.js")) {
        std::cerr << "WorkerPool v2 scheduling, broadcast, or queueing failed" << std::endl;
        return 54;
    }
    auto v2RowsMatch = mainEngine->getGlobalProperty("__v2RowsMatch");
    if (!mainEngine->toBoolean(v2RowsMatch)) {
        mainEngine->releaseValue(v2RowsMatch);
        std::cerr << "WorkerPool v2 dynamic chunks missed shared rows" << std::endl;
        return 55;
    }
    mainEngine->releaseValue(v2RowsMatch);

    async::configureCpuBudget(1);
    workers::WorkerThread budgetBlocker(
        13,
        engineType,
        workers::WorkerSourceKind::Script,
        R"JS(
self.onmessage = () => {
    postMessage({ kind: 'cpu-budget-blocker-started' });
    while (true) {}
};
)JS",
        std::filesystem::current_path().string(),
        "cpu-budget-blocker-smoke.js",
        sharedBuffers);
    budgetBlocker.start();
    const bool budgetBlockerReady = waitForWorkerMessage(
        budgetBlocker,
        [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Ready;
        });
    const bool budgetBlockerStarted = budgetBlockerReady &&
        budgetBlocker.postMessage("null") == workers::WorkerPostStatus::Posted &&
        waitForWorkerMessage(
            budgetBlocker,
            [](const workers::WorkerMessage& message) {
                return message.type == workers::WorkerMessage::Type::Message;
            });
    workers::WorkerThread budgetWaiter(
        14,
        engineType,
        workers::WorkerSourceKind::Script,
        "self.onmessage = () => {};",
        std::filesystem::current_path().string(),
        "cpu-budget-waiter-smoke.js",
        sharedBuffers);
    budgetWaiter.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto budgetCancelStarted = std::chrono::steady_clock::now();
    budgetWaiter.terminate();
    const auto budgetCancelElapsed = std::chrono::steady_clock::now() - budgetCancelStarted;
    budgetBlocker.terminate();
    if (!budgetBlockerStarted || budgetCancelElapsed > std::chrono::seconds(1) ||
        async::cpuBudgetStats().active != 0) {
        std::cerr << "CPU budget cancellation blocked Worker shutdown" << std::endl;
        return 59;
    }

    async::configureCpuBudget(2);
    if (!async::getJobSystem().start({2, 16}) ||
        !workers::getNativeTaskRegistry().registerTask(
            "test.native.echo",
            [](std::string_view payload, const async::JobContext&) {
                return workers::NativeTaskResult::success(std::string(payload));
            }) ||
        !workers::getNativeTaskRegistry().registerTask(
            "test.native.failure",
            [](std::string_view, const async::JobContext&) {
                return workers::NativeTaskResult::failure("intentional native task failure");
            })) {
        std::cerr << "Native task registry setup failed" << std::endl;
        return 56;
    }
    if (!mainEngine->evalScript(R"JS(
globalThis.__nativeTaskPrepared = __mystralPrepareMessage({ value: 37, label: 'echo' });
)JS", "native-task-prepare.js")) {
        return 57;
    }
    auto nativePrepared = mainEngine->getGlobalProperty("__nativeTaskPrepared");
    const std::string nativePayload = mainEngine->toString(
        mainEngine->getProperty(nativePrepared, "payload"));
    mainEngine->releaseValue(nativePrepared);
    workers::WorkerThread nativeTaskWorker(
        12,
        engineType,
        workers::WorkerSourceKind::Module,
        "./tests/native/fixtures/native-task-worker.mjs",
        std::filesystem::current_path().string(),
        "native-task-worker-smoke.js",
        sharedBuffers);
    nativeTaskWorker.start();
    const bool nativeReady = waitForWorkerMessage(
        nativeTaskWorker,
        [](const workers::WorkerMessage& message) {
            return message.type == workers::WorkerMessage::Type::Ready;
        });
    const bool nativeResult = nativeReady &&
        nativeTaskWorker.postMessage(nativePayload) == workers::WorkerPostStatus::Posted &&
        waitForWorkerMessage(
            nativeTaskWorker,
            [mainEngine = mainEngine.get()](const workers::WorkerMessage& message) {
                if (message.type != workers::WorkerMessage::Type::Message) return false;
                const std::string decoded = decodedMessageJson(mainEngine, message);
                return decoded.find("\"kind\":\"native-task-result\"") != std::string::npos &&
                    decoded.find("\"value\":37") != std::string::npos &&
                    decoded.find("intentional native task failure") != std::string::npos;
            });
    nativeTaskWorker.terminate();
    workers::getNativeTaskRegistry().unregisterTask("test.native.echo");
    workers::getNativeTaskRegistry().unregisterTask("test.native.failure");
    async::getJobSystem().shutdown();
    if (!nativeResult) {
        std::cerr << "Isolate-aware native task bridge failed" << std::endl;
        return 58;
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
