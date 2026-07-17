#include "mystral/workers/shared_buffer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace mystral::workers {

namespace {

js::JSValueHandle makeDescriptor(js::Engine* engine,
                                 const std::shared_ptr<SharedBufferStorage>& storage) {
    auto buffer = engine->newSharedArrayBuffer(
        storage->bytes.get(),
        storage->size,
        std::static_pointer_cast<void>(storage));
    if (!buffer.ptr) {
        engine->throwException("SharedArrayBuffer is not supported by this JavaScript engine");
        return engine->newUndefined();
    }

    auto descriptor = engine->newObject();
    engine->setProperty(descriptor, "id", engine->newNumber(static_cast<double>(storage->id)));
    engine->setProperty(descriptor, "byteLength", engine->newNumber(static_cast<double>(storage->size)));
    engine->setProperty(descriptor, "buffer", buffer);
    return descriptor;
}

enum class AtomicOperation {
    Load,
    Store,
    Add,
    Sub,
    Exchange,
    CompareExchange,
    And,
    Or,
    Xor
};

int32_t atomicLoad(int32_t* value) {
#if defined(_MSC_VER)
    return static_cast<int32_t>(_InterlockedCompareExchange(
        reinterpret_cast<volatile long*>(value), 0, 0));
#else
    return __atomic_load_n(value, __ATOMIC_SEQ_CST);
#endif
}

int32_t atomicExchange(int32_t* value, int32_t replacement) {
#if defined(_MSC_VER)
    return static_cast<int32_t>(_InterlockedExchange(
        reinterpret_cast<volatile long*>(value), static_cast<long>(replacement)));
#else
    return __atomic_exchange_n(value, replacement, __ATOMIC_SEQ_CST);
#endif
}

int32_t atomicCompareExchange(int32_t* value, int32_t expected, int32_t replacement) {
#if defined(_MSC_VER)
    return static_cast<int32_t>(_InterlockedCompareExchange(
        reinterpret_cast<volatile long*>(value),
        static_cast<long>(replacement),
        static_cast<long>(expected)));
#else
    __atomic_compare_exchange_n(
        value, &expected, replacement, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
#endif
}

int32_t atomicAdd(int32_t* value, int32_t amount) {
#if defined(_MSC_VER)
    return static_cast<int32_t>(_InterlockedExchangeAdd(
        reinterpret_cast<volatile long*>(value), static_cast<long>(amount)));
#else
    return __atomic_fetch_add(value, amount, __ATOMIC_SEQ_CST);
#endif
}

int32_t atomicBitwise(int32_t* value, int32_t operand, AtomicOperation operation) {
#if defined(_MSC_VER)
    auto* target = reinterpret_cast<volatile long*>(value);
    if (operation == AtomicOperation::And) {
        return static_cast<int32_t>(_InterlockedAnd(target, static_cast<long>(operand)));
    }
    if (operation == AtomicOperation::Or) {
        return static_cast<int32_t>(_InterlockedOr(target, static_cast<long>(operand)));
    }
    return static_cast<int32_t>(_InterlockedXor(target, static_cast<long>(operand)));
#else
    if (operation == AtomicOperation::And) return __atomic_fetch_and(value, operand, __ATOMIC_SEQ_CST);
    if (operation == AtomicOperation::Or) return __atomic_fetch_or(value, operand, __ATOMIC_SEQ_CST);
    return __atomic_fetch_xor(value, operand, __ATOMIC_SEQ_CST);
#endif
}

void installAtomicBinding(js::Engine* engine, const char* name, AtomicOperation operation) {
    engine->setGlobalProperty(name,
        engine->newFunction(name,
            [engine, operation](void*, const std::vector<js::JSValueHandle>& args) {
                const size_t requiredArgs = operation == AtomicOperation::Load ? 2
                    : operation == AtomicOperation::CompareExchange ? 4
                    : 3;
                if (args.size() < requiredArgs) {
                    engine->throwException("Invalid native Atomics call");
                    return engine->newUndefined();
                }

                size_t byteLength = 0;
                auto* data = static_cast<uint8_t*>(engine->getArrayBufferData(args[0], &byteLength));
                const double numericIndex = engine->toNumber(args[1]);
                if (!data || !std::isfinite(numericIndex) || numericIndex < 0 ||
                    numericIndex != static_cast<size_t>(numericIndex) ||
                    static_cast<size_t>(numericIndex) >= byteLength / sizeof(int32_t)) {
                    engine->throwException("Atomics index is outside the shared Int32Array");
                    return engine->newUndefined();
                }

                auto* target = reinterpret_cast<int32_t*>(data) + static_cast<size_t>(numericIndex);
                int32_t result = 0;
                if (operation == AtomicOperation::Load) {
                    result = atomicLoad(target);
                } else {
                    const int32_t value = static_cast<int32_t>(engine->toNumber(args[2]));
                    switch (operation) {
                        case AtomicOperation::Store:
                            atomicExchange(target, value);
                            result = value;
                            break;
                        case AtomicOperation::Add: result = atomicAdd(target, value); break;
                        case AtomicOperation::Sub: result = atomicAdd(target, -value); break;
                        case AtomicOperation::Exchange: result = atomicExchange(target, value); break;
                        case AtomicOperation::CompareExchange:
                            result = atomicCompareExchange(
                                target, value, static_cast<int32_t>(engine->toNumber(args[3])));
                            break;
                        case AtomicOperation::And:
                        case AtomicOperation::Or:
                        case AtomicOperation::Xor:
                            result = atomicBitwise(target, value, operation);
                            break;
                        case AtomicOperation::Load: break;
                    }
                }
                return engine->newNumber(result);
            }));
}

}  // namespace

SharedBufferStorage::SharedBufferStorage(uint64_t bufferId, size_t byteLength)
    : id(bufferId)
    , size(byteLength)
    , bytes(std::make_unique<uint8_t[]>(byteLength)) {
    std::fill_n(bytes.get(), size, uint8_t{0});
}

std::shared_ptr<SharedBufferStorage> SharedBufferRegistry::create(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto storage = std::make_shared<SharedBufferStorage>(nextId_++, size);
    buffers_.emplace(storage->id, storage);
    return storage;
}

std::shared_ptr<SharedBufferStorage> SharedBufferRegistry::attach(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buffers_.find(id);
    return it == buffers_.end() ? nullptr : it->second;
}

bool SharedBufferRegistry::release(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffers_.erase(id) > 0;
}

void SharedBufferRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    buffers_.clear();
}

size_t SharedBufferRegistry::allocatedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& entry : buffers_) {
        total += entry.second->size;
    }
    return total;
}

void installSharedBufferBindings(
    js::Engine* engine,
    const std::shared_ptr<SharedBufferRegistry>& registry) {
    if (!engine || !registry) return;

    engine->setGlobalProperty("__mystralSharedSupported",
        engine->newBoolean(engine->supportsSharedArrayBuffer()));

    installAtomicBinding(engine, "__mystralAtomicLoad", AtomicOperation::Load);
    installAtomicBinding(engine, "__mystralAtomicStore", AtomicOperation::Store);
    installAtomicBinding(engine, "__mystralAtomicAdd", AtomicOperation::Add);
    installAtomicBinding(engine, "__mystralAtomicSub", AtomicOperation::Sub);
    installAtomicBinding(engine, "__mystralAtomicExchange", AtomicOperation::Exchange);
    installAtomicBinding(engine, "__mystralAtomicCompareExchange", AtomicOperation::CompareExchange);
    installAtomicBinding(engine, "__mystralAtomicAnd", AtomicOperation::And);
    installAtomicBinding(engine, "__mystralAtomicOr", AtomicOperation::Or);
    installAtomicBinding(engine, "__mystralAtomicXor", AtomicOperation::Xor);

    engine->setGlobalProperty("__mystralSharedCreate",
        engine->newFunction("__mystralSharedCreate",
            [engine, registry](void*, const std::vector<js::JSValueHandle>& args) {
                if (!engine->supportsSharedArrayBuffer()) {
                    engine->throwException("SharedArrayBuffer is not supported by this JavaScript engine");
                    return engine->newUndefined();
                }
                if (args.empty() || !engine->isNumber(args[0])) {
                    engine->throwException("SharedBuffer.allocate requires a byte length");
                    return engine->newUndefined();
                }
                double requested = engine->toNumber(args[0]);
                constexpr double maxSafeBufferSize = 2147483647.0;
                if (requested <= 0 || requested > maxSafeBufferSize || requested != static_cast<size_t>(requested)) {
                    engine->throwException("SharedBuffer byte length must be an integer between 1 and 2147483647");
                    return engine->newUndefined();
                }
                try {
                    return makeDescriptor(engine, registry->create(static_cast<size_t>(requested)));
                } catch (const std::bad_alloc&) {
                    engine->throwException("SharedBuffer allocation failed");
                    return engine->newUndefined();
                }
            }));

    engine->setGlobalProperty("__mystralSharedAttach",
        engine->newFunction("__mystralSharedAttach",
            [engine, registry](void*, const std::vector<js::JSValueHandle>& args) {
                if (args.empty() || !engine->isNumber(args[0])) {
                    engine->throwException("SharedBuffer.attach requires a buffer id");
                    return engine->newUndefined();
                }
                double numericId = engine->toNumber(args[0]);
                if (numericId <= 0 || numericId > 9007199254740991.0) {
                    engine->throwException("Invalid SharedBuffer id");
                    return engine->newUndefined();
                }
                auto storage = registry->attach(static_cast<uint64_t>(numericId));
                if (!storage) {
                    engine->throwException("SharedBuffer is no longer available");
                    return engine->newUndefined();
                }
                return makeDescriptor(engine, storage);
            }));

    engine->setGlobalProperty("__mystralSharedRelease",
        engine->newFunction("__mystralSharedRelease",
            [engine, registry](void*, const std::vector<js::JSValueHandle>& args) {
                if (args.empty() || !engine->isNumber(args[0])) {
                    return engine->newBoolean(false);
                }
                return engine->newBoolean(
                    registry->release(static_cast<uint64_t>(engine->toNumber(args[0]))));
            }));
}

const char* sharedApiSource() {
    static const std::string source = [] {
        std::string code;
        code.reserve(24 * 1024);
        code += R"JS(
(function() {
    if (globalThis.__mystralShared) return;

    if (typeof globalThis.Atomics === 'undefined') {
        function assertAtomicArray(array) {
            if (!(array instanceof Int32Array)) {
                throw new TypeError('Mystral Atomics fallback requires an Int32Array');
            }
            return array;
        }
        globalThis.Atomics = Object.freeze({
            load: (array, index) => __mystralAtomicLoad(assertAtomicArray(array), index),
            store: (array, index, value) => __mystralAtomicStore(assertAtomicArray(array), index, value),
            add: (array, index, value) => __mystralAtomicAdd(assertAtomicArray(array), index, value),
            sub: (array, index, value) => __mystralAtomicSub(assertAtomicArray(array), index, value),
            exchange: (array, index, value) => __mystralAtomicExchange(assertAtomicArray(array), index, value),
            compareExchange: (array, index, expected, value) =>
                __mystralAtomicCompareExchange(assertAtomicArray(array), index, expected, value),
            and: (array, index, value) => __mystralAtomicAnd(assertAtomicArray(array), index, value),
            or: (array, index, value) => __mystralAtomicOr(assertAtomicArray(array), index, value),
            xor: (array, index, value) => __mystralAtomicXor(assertAtomicArray(array), index, value),
            isLockFree: size => size === 4,
            notify: () => 0,
        });
    }

    const TYPE_INFO = Object.freeze({
        u8:  { bytes: 1, ctor: Uint8Array },
        i8:  { bytes: 1, ctor: Int8Array },
        u16: { bytes: 2, ctor: Uint16Array },
        i16: { bytes: 2, ctor: Int16Array },
        u32: { bytes: 4, ctor: Uint32Array },
        i32: { bytes: 4, ctor: Int32Array },
        f32: { bytes: 4, ctor: Float32Array },
        f64: { bytes: 8, ctor: Float64Array },
    });
    const HEADER_BYTES = 64;

    function assertSharedAvailable() {
        if (!globalThis.__mystralSharedSupported) {
            throw new Error('Shared memory is not supported by this JavaScript engine');
        }
    }

    function align(value, alignment) {
        return Math.ceil(value / alignment) * alignment;
    }

    function schemaHash(name, fields) {
        let hash = 0x811c9dc5;
        const text = name + '|' + Object.entries(fields).map(([key, type]) => key + ':' + type).join('|');
        for (let i = 0; i < text.length; i++) {
            hash ^= text.charCodeAt(i);
            hash = Math.imul(hash, 0x01000193);
        }
        return hash >>> 0;
    }

    function validateFields(fields) {
        const entries = Object.entries(fields || {});
        if (entries.length === 0) throw new TypeError('A shared schema needs at least one field');
        for (const [name, type] of entries) {
            if (!name || !TYPE_INFO[type]) throw new TypeError('Unsupported shared field: ' + name + ':' + type);
        }
        return entries;
    }

    class SharedBuffer {
        constructor(descriptor) {
            this.id = descriptor.id;
            this.byteLength = descriptor.byteLength;
            this.buffer = descriptor.buffer;
        }

        static allocate(byteLength) {
            assertSharedAvailable();
            return new SharedBuffer(__mystralSharedCreate(byteLength));
        }

        static attach(handle) {
            if (handle instanceof SharedBuffer) return handle;
            const id = handle && (handle.$mystralSharedBuffer || handle.id);
            if (!Number.isSafeInteger(id) || id <= 0) throw new TypeError('Invalid SharedBuffer handle');
            return new SharedBuffer(__mystralSharedAttach(id));
        }

        handle() { return { $mystralSharedBuffer: this.id }; }
        toJSON() { return this.handle(); }
        release() { return __mystralSharedRelease(this.id); }
    }

    function reviveShared(_key, value) {
        if (value && Number.isSafeInteger(value.$mystralSharedBuffer)) {
            return SharedBuffer.attach(value);
        }
        return value;
    }

    function serializeMessage(value) {
        return JSON.stringify(value);
    }

    const transferredBuffers = new WeakSet();

    function prepareMessage(value, transferList = []) {
        const transfers = Array.from(transferList || []);
        const indexes = new Map();
        for (let index = 0; index < transfers.length; index++) {
            const buffer = transfers[index];
            if (!(buffer instanceof ArrayBuffer)) {
                throw new TypeError('Worker transfer list accepts only ArrayBuffer');
            }
            if (indexes.has(buffer)) throw new TypeError('Worker transfer list contains a duplicate ArrayBuffer');
            if (transferredBuffers.has(buffer)) throw new TypeError('Worker transfer list contains a detached ArrayBuffer');
            indexes.set(buffer, index);
        }
        const payload = JSON.stringify(value, (_key, item) => {
            const index = indexes.get(item);
            return index === undefined ? item : { $mystralTransferredBuffer: index };
        });
        if (typeof payload !== 'string') throw new TypeError('Worker message must be JSON-compatible');
        for (const buffer of transfers) transferredBuffers.add(buffer);
        return { payload, transfers };
    }

    function parseMessage(value, transfers = []) {
        return JSON.parse(value, (key, item) => {
            if (item && Number.isInteger(item.$mystralTransferredBuffer)) {
                const buffer = transfers[item.$mystralTransferredBuffer];
                if (!(buffer instanceof ArrayBuffer)) throw new TypeError('Invalid transferred ArrayBuffer marker');
                return buffer;
            }
            return reviveShared(key, item);
        });
    }

    function buildLayout(fields, capacity) {
        const entries = validateFields(fields);
        let offset = HEADER_BYTES;
        const layout = [];
        for (const [name, type] of entries) {
            const info = TYPE_INFO[type];
            offset = align(offset, info.bytes);
            layout.push({ name, type, offset, info });
            offset += info.bytes * capacity;
        }
        return { fields: layout, byteLength: align(offset, 8) };
    }

    class SharedTableInstance {
        constructor(definition, sharedBuffer, capacity, initialize) {
            this.schemaId = definition.schemaId;
            this.schemaHash = definition.schemaHash;
            this.capacity = capacity;
            this.sharedBuffer = sharedBuffer;
            this.buffer = sharedBuffer.buffer;
            this.control = new Int32Array(this.buffer, 0, HEADER_BYTES / 4);
            const layout = buildLayout(definition.fields, capacity);
            if (layout.byteLength !== sharedBuffer.byteLength) throw new Error('SharedTable buffer size mismatch');
            for (const field of layout.fields) {
                this[field.name] = new field.info.ctor(this.buffer, field.offset, capacity);
            }
            if (initialize) {
                Atomics.store(this.control, 0, definition.schemaHash | 0);
                Atomics.store(this.control, 1, capacity);
                Atomics.store(this.control, 2, 0);
            } else if ((Atomics.load(this.control, 0) >>> 0) !== definition.schemaHash ||
                       Atomics.load(this.control, 1) !== capacity) {
                throw new Error('SharedTable header does not match its schema handle');
            }
        }

        get length() { return Atomics.load(this.control, 2); }
        set length(value) {
            if (!Number.isInteger(value) || value < 0 || value > this.capacity) {
                throw new RangeError('SharedTable length is outside its capacity');
            }
            Atomics.store(this.control, 2, value);
        }
        handle() {
            return {
                $mystralSharedTable: 1,
                schemaId: this.schemaId,
                schemaHash: this.schemaHash,
                capacity: this.capacity,
                buffer: this.sharedBuffer,
            };
        }
        release() { return this.sharedBuffer.release(); }
    }

)JS";
        code += R"JS(
    class SharedTable {
        static define(schemaId, fields) {
            if (typeof schemaId !== 'string' || !schemaId) throw new TypeError('SharedTable schemaId is required');
            validateFields(fields);
            const definition = { schemaId, fields: Object.freeze({ ...fields }), schemaHash: schemaHash(schemaId, fields) };
            return Object.freeze({
                schemaId,
                schemaHash: definition.schemaHash,
                create({ capacity }) {
                    if (!Number.isInteger(capacity) || capacity <= 0) throw new RangeError('SharedTable capacity must be positive');
                    const layout = buildLayout(fields, capacity);
                    return new SharedTableInstance(definition, SharedBuffer.allocate(layout.byteLength), capacity, true);
                },
                attach(handle) {
                    if (!handle || handle.$mystralSharedTable !== 1 || handle.schemaId !== schemaId ||
                        handle.schemaHash !== definition.schemaHash || !Number.isInteger(handle.capacity)) {
                        throw new Error('SharedTable schema mismatch for ' + schemaId);
                    }
                    return new SharedTableInstance(definition, SharedBuffer.attach(handle.buffer), handle.capacity, false);
                },
                resize(table, { capacity }) {
                    if (!(table instanceof SharedTableInstance) || table.schemaHash !== definition.schemaHash) {
                        throw new TypeError('SharedTable resize requires an instance of ' + schemaId);
                    }
                    if (!Number.isInteger(capacity) || capacity <= 0) {
                        throw new RangeError('SharedTable capacity must be positive');
                    }
                    const layout = buildLayout(fields, capacity);
                    const resized = new SharedTableInstance(
                        definition, SharedBuffer.allocate(layout.byteLength), capacity, true);
                    const copiedLength = Math.min(table.length, capacity);
                    for (const [fieldName] of Object.entries(fields)) {
                        resized[fieldName].set(table[fieldName].subarray(0, copiedLength));
                    }
                    resized.length = copiedLength;
                    return resized;
                },
            });
        }
    }

    class SharedQueueInstance {
        constructor(definition, sharedBuffer, capacity, initialize) {
            this.schemaId = definition.schemaId;
            this.schemaHash = definition.schemaHash;
            this.capacity = capacity;
            this.ringCapacity = capacity + 1;
            this.sharedBuffer = sharedBuffer;
            this.buffer = sharedBuffer.buffer;
            this.control = new Int32Array(this.buffer, 0, HEADER_BYTES / 4);
            this.layout = buildLayout(definition.fields, this.ringCapacity);
            if (this.layout.byteLength !== sharedBuffer.byteLength) throw new Error('SharedQueue buffer size mismatch');
            this.views = {};
            for (const field of this.layout.fields) {
                this.views[field.name] = new field.info.ctor(this.buffer, field.offset, this.ringCapacity);
            }
            if (initialize) {
                Atomics.store(this.control, 0, definition.schemaHash | 0);
                Atomics.store(this.control, 1, capacity);
                Atomics.store(this.control, 2, 0);
                Atomics.store(this.control, 3, 0);
            } else if ((Atomics.load(this.control, 0) >>> 0) !== definition.schemaHash ||
                       Atomics.load(this.control, 1) !== capacity) {
                throw new Error('SharedQueue header does not match its schema handle');
            }
        }

        push(value) {
            const head = Atomics.load(this.control, 2);
            const tail = Atomics.load(this.control, 3);
            const next = (head + 1) % this.ringCapacity;
            if (next === tail) return false;
            for (const field of this.layout.fields) this.views[field.name][head] = value[field.name];
            Atomics.store(this.control, 2, next);
            Atomics.notify(this.control, 2, 1);
            return true;
        }

        pop(target = {}) {
            const tail = Atomics.load(this.control, 3);
            const head = Atomics.load(this.control, 2);
            if (tail === head) return null;
            for (const field of this.layout.fields) target[field.name] = this.views[field.name][tail];
            Atomics.store(this.control, 3, (tail + 1) % this.ringCapacity);
            return target;
        }

        get size() {
            const head = Atomics.load(this.control, 2);
            const tail = Atomics.load(this.control, 3);
            return head >= tail ? head - tail : this.ringCapacity - tail + head;
        }

        handle() {
            return {
                $mystralSharedQueue: 1,
                schemaId: this.schemaId,
                schemaHash: this.schemaHash,
                capacity: this.capacity,
                buffer: this.sharedBuffer,
            };
        }
    }

    class SharedQueue {
        static define(schemaId, fields) {
            if (typeof schemaId !== 'string' || !schemaId) throw new TypeError('SharedQueue schemaId is required');
            validateFields(fields);
            const definition = { schemaId, fields: Object.freeze({ ...fields }), schemaHash: schemaHash(schemaId, fields) };
            return Object.freeze({
                schemaId,
                schemaHash: definition.schemaHash,
                create({ capacity }) {
                    if (!Number.isInteger(capacity) || capacity < 2) throw new RangeError('SharedQueue capacity must be at least 2');
                    const layout = buildLayout(fields, capacity + 1);
                    return new SharedQueueInstance(definition, SharedBuffer.allocate(layout.byteLength), capacity, true);
                },
                attach(handle) {
                    if (!handle || handle.$mystralSharedQueue !== 1 || handle.schemaId !== schemaId ||
                        handle.schemaHash !== definition.schemaHash || !Number.isInteger(handle.capacity)) {
                        throw new Error('SharedQueue schema mismatch for ' + schemaId);
                    }
                    return new SharedQueueInstance(definition, SharedBuffer.attach(handle.buffer), handle.capacity, false);
                },
            });
        }
    }

    function deterministicPartitions(start, end, count) {
        if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) ||
            !Number.isSafeInteger(count) || start < 0 || end < start || count < 1) {
            throw new RangeError('WorkerPool partitions require valid start/end/count values');
        }
        const length = end - start;
        const base = Math.floor(length / count);
        const remainder = length % count;
        const partitions = [];
        let cursor = start;
        for (let index = 0; index < count; index++) {
            const partitionLength = base + (index < remainder ? 1 : 0);
            partitions.push({ begin: cursor, end: cursor + partitionLength });
            cursor += partitionLength;
        }
        return partitions;
    }

    function transferResult(value, transferList = []) {
        return { $mystralWorkerPoolTransferResult: 1, value, transferList: Array.from(transferList || []) };
    }

    function exposeWorkerTask(handler) {
        if (typeof handler !== 'function') throw new TypeError('WorkerPool task handler must be a function');
        globalThis.onmessage = event => {
            const task = event.data;
            if (!task || task.$mystralWorkerPoolTask !== 1) return;
            Promise.resolve().then(() => handler({
                data: task.data,
                round: task.round,
                workerIndex: task.workerIndex,
                workerCount: task.workerCount,
                begin: task.begin,
                end: task.end,
            })).then(result => {
                let value = result;
                let transfers = [];
                if (result && result.$mystralWorkerPoolTransferResult === 1) {
                    value = result.value;
                    transfers = result.transferList;
                }
                postMessage({
                    $mystralWorkerPoolResult: 1,
                    round: task.round,
                    workerIndex: task.workerIndex,
                    ok: true,
                    value,
                }, transfers);
            }).catch(error => {
                postMessage({
                    $mystralWorkerPoolResult: 1,
                    round: task.round,
                    workerIndex: task.workerIndex,
                    ok: false,
                    error: error && error.message ? error.message : String(error),
                    stack: error && error.stack ? String(error.stack) : '',
                });
            });
        };
    }

)JS";
        code += R"JS(
    class WorkerPool {
        constructor(workerUrl, options = {}) {
            if (typeof Worker !== 'function') throw new Error('WorkerPool can only be created on the main runtime thread');
            const defaultSize = globalThis.navigator && Number.isInteger(globalThis.navigator.hardwareConcurrency)
                ? globalThis.navigator.hardwareConcurrency
                : 4;
            const size = options.size === undefined ? defaultSize : options.size;
            if (!Number.isInteger(size) || size < 1 || size > 64) {
                throw new RangeError('WorkerPool size must be between 1 and 64');
            }
            this.size = size;
            this._round = 0;
            this._active = null;
            this._failure = null;
            this._terminated = false;
            this._workers = [];
            for (let index = 0; index < size; index++) {
                const worker = new Worker(workerUrl, {
                    type: 'module',
                    name: (options.name || 'worker-pool') + '-' + index,
                });
                worker.onmessage = event => this._handleMessage(index, event.data);
                worker.onerror = event => this._fail(new Error(event.message || 'WorkerPool Worker failed'));
                this._workers.push(worker);
            }
        }

        get busy() { return this._active !== null; }

        run(data, options = {}) {
            if (this._terminated) return Promise.reject(new Error('WorkerPool is terminated'));
            if (this._failure) return Promise.reject(this._failure);
            if (this._active) return Promise.reject(new Error('WorkerPool already has an active round'));
            const start = options.start === undefined ? 0 : options.start;
            const end = options.end === undefined ? options.length : options.end;
            if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start < 0 || end < start) {
                return Promise.reject(new RangeError('WorkerPool run requires a valid start/end or length'));
            }
            const round = ++this._round;
            const partitions = deterministicPartitions(start, end, this.size);
            return new Promise((resolve, reject) => {
                this._active = {
                    round,
                    remaining: this.size,
                    results: new Array(this.size),
                    completed: new Array(this.size).fill(false),
                    resolve,
                    reject,
                };
                try {
                    for (let index = 0; index < this.size; index++) {
                        this._workers[index].postMessage({
                            $mystralWorkerPoolTask: 1,
                            round,
                            workerIndex: index,
                            workerCount: this.size,
                            begin: partitions[index].begin,
                            end: partitions[index].end,
                            data,
                        });
                    }
                } catch (error) {
                    this._fail(error);
                }
            });
        }

        resizeTable(definition, table, options) {
            if (this._active) throw new Error('SharedTable can only be resized while WorkerPool is idle');
            if (!definition || typeof definition.resize !== 'function') {
                throw new TypeError('WorkerPool resizeTable requires a SharedTable definition');
            }
            return definition.resize(table, options);
        }

        terminate() {
            if (this._terminated) return;
            this._terminated = true;
            if (this._active) {
                this._active.reject(new Error('WorkerPool terminated during an active round'));
                this._active = null;
            }
            for (const worker of this._workers) worker.terminate();
            this._workers.length = 0;
        }

        _handleMessage(workerIndex, message) {
            const active = this._active;
            if (!active || !message || message.$mystralWorkerPoolResult !== 1 ||
                message.round !== active.round || message.workerIndex !== workerIndex) return;
            if (active.completed[workerIndex]) return;
            if (!message.ok) {
                const error = new Error(message.error || 'WorkerPool task failed');
                if (message.stack) error.stack = message.stack;
                this._fail(error);
                return;
            }
            active.completed[workerIndex] = true;
            active.results[workerIndex] = message.value;
            active.remaining--;
            if (active.remaining === 0) {
                this._active = null;
                active.resolve(active.results);
            }
        }

        _fail(error) {
            const failure = error instanceof Error ? error : new Error(String(error));
            this._failure = failure;
            if (this._active) {
                this._active.reject(failure);
                this._active = null;
            }
            for (const worker of this._workers) worker.terminate();
            this._workers.length = 0;
            this._terminated = true;
        }
    }

    globalThis.__mystralSerializeMessage = serializeMessage;
    globalThis.__mystralPrepareMessage = prepareMessage;
    globalThis.__mystralParseMessage = parseMessage;
    globalThis.__mystralShared = Object.freeze({ SharedBuffer, SharedTable, SharedQueue });
    globalThis.__mystralWorkerPool = Object.freeze({
        WorkerPool,
        exposeWorkerTask,
        transferResult,
        deterministicPartitions,
    });
    if (typeof globalThis.WorkerPool === 'undefined') globalThis.WorkerPool = WorkerPool;
})();
)JS";
        return code;
    }();
    return source.c_str();
}

}  // namespace mystral::workers
