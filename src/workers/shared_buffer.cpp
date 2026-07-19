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

    if (typeof globalThis.AbortSignal === 'undefined') {
        const abortError = (name, message) => {
            const error = new Error(message);
            error.name = name;
            return error;
        };
        class AbortSignal {
            constructor() {
                this.aborted = false;
                this.reason = undefined;
                this.onabort = null;
                this._listeners = [];
            }
            addEventListener(type, listener) {
                if (type === 'abort' && typeof listener === 'function') {
                    this._listeners.push(listener);
                }
            }
            removeEventListener(type, listener) {
                if (type === 'abort') {
                    this._listeners = this._listeners.filter(value => value !== listener);
                }
            }
            dispatchEvent(event) {
                if (!event || event.type !== 'abort') return true;
                if (typeof this.onabort === 'function') this.onabort(event);
                for (const listener of this._listeners.slice()) listener(event);
                return true;
            }
            throwIfAborted() {
                if (this.aborted) throw this.reason;
            }
            _fireAbort(reason) {
                if (this.aborted) return;
                this.aborted = true;
                this.reason = reason === undefined
                    ? abortError('AbortError', 'The operation was aborted')
                    : reason;
                this.dispatchEvent({ type: 'abort', target: this });
            }
            static abort(reason) {
                const signal = new AbortSignal();
                signal._fireAbort(reason);
                return signal;
            }
            static timeout(milliseconds) {
                const signal = new AbortSignal();
                setTimeout(() => signal._fireAbort(
                    abortError('TimeoutError', 'The operation timed out')), milliseconds);
                return signal;
            }
            static any(signals) {
                const result = new AbortSignal();
                for (const signal of Array.from(signals || [])) {
                    if (!signal) continue;
                    if (signal.aborted) {
                        result._fireAbort(signal.reason);
                        break;
                    }
                    signal.addEventListener('abort', () => result._fireAbort(signal.reason));
                }
                return result;
            }
        }
        globalThis.AbortSignal = AbortSignal;
    }

    if (typeof globalThis.AbortController === 'undefined') {
        class AbortController {
            constructor() { this.signal = new AbortSignal(); }
            abort(reason) { this.signal._fireAbort(reason); }
        }
        globalThis.AbortController = AbortController;
    }

)JS";
        code += R"JS(

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

    const WIRE_VERSION = 1;
    const TYPED_ARRAY_NAMES = [
        'Int8Array', 'Uint8Array', 'Uint8ClampedArray',
        'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array',
        'Float16Array', 'Float32Array', 'Float64Array',
        'BigInt64Array', 'BigUint64Array',
    ];
    const TYPED_ARRAY_CTORS = Object.create(null);
    for (const name of TYPED_ARRAY_NAMES) {
        const ctor = globalThis[name];
        if (typeof ctor === 'function') TYPED_ARRAY_CTORS[name] = ctor;
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

        const attachments = transfers.slice();
        const stack = new WeakSet();

        function bufferIndex(buffer) {
            if (!(buffer instanceof ArrayBuffer)) {
                throw new TypeError('Raw SharedArrayBuffer views are not message values; send a SharedBuffer handle');
            }
            const existing = indexes.get(buffer);
            if (existing !== undefined) return existing;
            const copy = buffer.slice(0);
            const index = attachments.length;
            attachments.push(copy);
            indexes.set(buffer, index);
            return index;
        }

        function encode(item, arrayElement = false) {
            if (item === null || typeof item === 'string' || typeof item === 'boolean') return item;
            if (typeof item === 'number') return Number.isFinite(item) ? item : null;
            if (typeof item === 'undefined' || typeof item === 'function' || typeof item === 'symbol') {
                if (arrayElement) return null;
                return undefined;
            }
            if (typeof item === 'bigint') {
                throw new TypeError('Worker message does not support BigInt values');
            }
            if (item instanceof ArrayBuffer) {
                return ['arraybuffer', bufferIndex(item)];
            }
            if (ArrayBuffer.isView(item)) {
                const index = bufferIndex(item.buffer);
                if (item instanceof DataView) {
                    return ['view', 'DataView', index, item.byteOffset, item.byteLength];
                }
                const name = item.constructor && item.constructor.name;
                if (!name || TYPED_ARRAY_CTORS[name] !== item.constructor) {
                    throw new TypeError('Worker message contains an unsupported TypedArray');
                }
                return ['view', name, index, item.byteOffset, item.length];
            }
            if (typeof SharedArrayBuffer !== 'undefined' && item instanceof SharedArrayBuffer) {
                throw new TypeError('Raw SharedArrayBuffer is not a message value; send a SharedBuffer handle');
            }

            const jsonValue = typeof item.toJSON === 'function' ? item.toJSON() : item;
            if (jsonValue !== item) return encode(jsonValue, arrayElement);
            if (stack.has(item)) throw new TypeError('Worker message contains a circular value');
            stack.add(item);
            let encoded;
            if (Array.isArray(item)) {
                const values = new Array(item.length);
                for (let index = 0; index < item.length; index++) {
                    values[index] = encode(item[index], true);
                }
                encoded = ['array', values];
            } else {
                const entries = [];
                for (const key of Object.keys(item)) {
                    const child = encode(item[key], false);
                    if (child !== undefined) entries.push([key, child]);
                }
                encoded = ['object', entries];
            }
            stack.delete(item);
            return encoded;
        }

        const encoded = encode(value, false);
        if (encoded === undefined) throw new TypeError('Worker message must be JSON-compatible');
        const payload = JSON.stringify({ $mystralWire: WIRE_VERSION, value: encoded });
        for (const buffer of transfers) transferredBuffers.add(buffer);
        return { payload, transfers: attachments };
    }

    function parseMessage(value, transfers = []) {
        const envelope = JSON.parse(value);
        if (!envelope || envelope.$mystralWire !== WIRE_VERSION || !('value' in envelope)) {
            return JSON.parse(value, reviveShared);
        }

        function transferredBuffer(index) {
            if (!Number.isInteger(index) || index < 0 || index >= transfers.length) {
                throw new TypeError('Worker message contains an invalid buffer index');
            }
            const buffer = transfers[index];
            if (!(buffer instanceof ArrayBuffer)) {
                throw new TypeError('Worker message attachment is not an ArrayBuffer');
            }
            return buffer;
        }

        function decode(encoded) {
            if (encoded === null || typeof encoded === 'string' ||
                typeof encoded === 'boolean' || typeof encoded === 'number') return encoded;
            if (!Array.isArray(encoded) || typeof encoded[0] !== 'string') {
                throw new TypeError('Worker message contains an invalid encoded value');
            }
            const tag = encoded[0];
            if (tag === 'arraybuffer') return transferredBuffer(encoded[1]);
            if (tag === 'view') {
                const name = encoded[1];
                const buffer = transferredBuffer(encoded[2]);
                const byteOffset = encoded[3];
                const length = encoded[4];
                if (!Number.isSafeInteger(byteOffset) || byteOffset < 0 ||
                    !Number.isSafeInteger(length) || length < 0) {
                    throw new TypeError('Worker message contains invalid view bounds');
                }
                try {
                    if (name === 'DataView') return new DataView(buffer, byteOffset, length);
                    const ctor = TYPED_ARRAY_CTORS[name];
                    if (typeof ctor !== 'function') {
                        throw new TypeError('Worker message contains an unsupported TypedArray: ' + name);
                    }
                    return new ctor(buffer, byteOffset, length);
                } catch (error) {
                    if (error instanceof TypeError && String(error.message).startsWith('Worker message')) throw error;
                    throw new TypeError('Worker message view is outside its backing buffer');
                }
            }
            if (tag === 'array') {
                if (!Array.isArray(encoded[1])) throw new TypeError('Worker message contains an invalid array');
                return encoded[1].map(decode);
            }
            if (tag === 'object') {
                if (!Array.isArray(encoded[1])) throw new TypeError('Worker message contains an invalid object');
                const result = {};
                for (const entry of encoded[1]) {
                    if (!Array.isArray(entry) || entry.length !== 2 || typeof entry[0] !== 'string') {
                        throw new TypeError('Worker message contains an invalid object entry');
                    }
                    Object.defineProperty(result, entry[0], {
                        value: decode(entry[1]),
                        enumerable: true,
                        configurable: true,
                        writable: true,
                    });
                }
                return reviveShared('', result);
            }
            throw new TypeError('Worker message contains an unknown value tag');
        }

        return decode(envelope.value);
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

    function buildCommandLayout(fields, workerCount, laneCapacity) {
        const entries = validateFields(fields);
        const controlWords = 4 + workerCount;
        let offset = align(controlWords * 4, HEADER_BYTES);
        const slots = workerCount * laneCapacity;
        const layout = [];
        for (const [name, type] of entries) {
            const info = TYPE_INFO[type];
            offset = align(offset, info.bytes);
            layout.push({ name, type, offset, info });
            offset += info.bytes * slots;
        }
        return { fields: layout, controlWords, slots, byteLength: align(offset, 8) };
    }

    class SharedCommandBufferInstance {
        constructor(definition, sharedBuffer, workerCount, laneCapacity, initialize) {
            this.schemaId = definition.schemaId;
            this.schemaHash = definition.schemaHash;
            this.workerCount = workerCount;
            this.laneCapacity = laneCapacity;
            this.sharedBuffer = sharedBuffer;
            this.buffer = sharedBuffer.buffer;
            this.layout = buildCommandLayout(definition.fields, workerCount, laneCapacity);
            if (this.layout.byteLength !== sharedBuffer.byteLength) {
                throw new Error('SharedCommandBuffer buffer size mismatch');
            }
            this.control = new Int32Array(this.buffer, 0, this.layout.controlWords);
            this.views = {};
            for (const field of this.layout.fields) {
                this.views[field.name] = new field.info.ctor(
                    this.buffer, field.offset, this.layout.slots);
            }
            if (initialize) {
                Atomics.store(this.control, 0, definition.schemaHash | 0);
                Atomics.store(this.control, 1, workerCount);
                Atomics.store(this.control, 2, laneCapacity);
                Atomics.store(this.control, 3, 0);
                for (let workerIndex = 0; workerIndex < workerCount; workerIndex++) {
                    Atomics.store(this.control, 4 + workerIndex, 0);
                }
            } else if ((Atomics.load(this.control, 0) >>> 0) !== definition.schemaHash ||
                       Atomics.load(this.control, 1) !== workerCount ||
                       Atomics.load(this.control, 2) !== laneCapacity) {
                throw new Error('SharedCommandBuffer header does not match its schema handle');
            }
        }

        push(workerIndex, value) {
            if (!Number.isInteger(workerIndex) || workerIndex < 0 || workerIndex >= this.workerCount) {
                throw new RangeError('SharedCommandBuffer workerIndex is outside its lane range');
            }
            const lengthSlot = 4 + workerIndex;
            const length = Atomics.load(this.control, lengthSlot);
            if (length >= this.laneCapacity) return false;
            const slot = workerIndex * this.laneCapacity + length;
            for (const field of this.layout.fields) {
                this.views[field.name][slot] = value[field.name];
            }
            Atomics.store(this.control, lengthSlot, length + 1);
            Atomics.add(this.control, 3, 1);
            return true;
        }

        drain(options = {}) {
            let sortKeys = null;
            if (options && options.sortBy !== undefined) {
                sortKeys = Array.isArray(options.sortBy)
                    ? options.sortBy
                    : [options.sortBy];
                if (sortKeys.length === 0 || sortKeys.some(
                    key => typeof key !== 'string' || !key || !this.views[key])) {
                    throw new TypeError('SharedCommandBuffer sortBy must name one or more schema fields');
                }
            }
            const result = [];
            for (let workerIndex = 0; workerIndex < this.workerCount; workerIndex++) {
                const lengthSlot = 4 + workerIndex;
                const length = Atomics.load(this.control, lengthSlot);
                const laneStart = workerIndex * this.laneCapacity;
                for (let index = 0; index < length; index++) {
                    const value = {};
                    for (const field of this.layout.fields) {
                        value[field.name] = this.views[field.name][laneStart + index];
                    }
                    result.push(value);
                }
                Atomics.store(this.control, lengthSlot, 0);
            }
            Atomics.store(this.control, 3, 0);
            if (sortKeys) {
                result.sort((left, right) => {
                    for (const key of sortKeys) {
                        if (left[key] < right[key]) return -1;
                        if (left[key] > right[key]) return 1;
                    }
                    return 0;
                });
            }
            return result;
        }

        clear() {
            for (let workerIndex = 0; workerIndex < this.workerCount; workerIndex++) {
                Atomics.store(this.control, 4 + workerIndex, 0);
            }
            Atomics.store(this.control, 3, 0);
        }

        get size() { return Atomics.load(this.control, 3); }

        handle() {
            return {
                $mystralSharedCommandBuffer: 1,
                schemaId: this.schemaId,
                schemaHash: this.schemaHash,
                workerCount: this.workerCount,
                laneCapacity: this.laneCapacity,
                buffer: this.sharedBuffer,
            };
        }

        release() { return this.sharedBuffer.release(); }
    }

    class SharedCommandBuffer {
        static define(schemaId, fields) {
            if (typeof schemaId !== 'string' || !schemaId) {
                throw new TypeError('SharedCommandBuffer schemaId is required');
            }
            validateFields(fields);
            const definition = {
                schemaId,
                fields: Object.freeze({ ...fields }),
                schemaHash: schemaHash(schemaId, fields),
            };
            return Object.freeze({
                schemaId,
                schemaHash: definition.schemaHash,
                create({ workerCount, laneCapacity }) {
                    if (!Number.isInteger(workerCount) || workerCount < 1 || workerCount > 64) {
                        throw new RangeError('SharedCommandBuffer workerCount must be between 1 and 64');
                    }
                    if (!Number.isInteger(laneCapacity) || laneCapacity < 1) {
                        throw new RangeError('SharedCommandBuffer laneCapacity must be positive');
                    }
                    const layout = buildCommandLayout(fields, workerCount, laneCapacity);
                    return new SharedCommandBufferInstance(
                        definition,
                        SharedBuffer.allocate(layout.byteLength),
                        workerCount,
                        laneCapacity,
                        true,
                    );
                },
                attach(handle) {
                    if (!handle || handle.$mystralSharedCommandBuffer !== 1 ||
                        handle.schemaId !== schemaId ||
                        handle.schemaHash !== definition.schemaHash ||
                        !Number.isInteger(handle.workerCount) || handle.workerCount < 1 ||
                        handle.workerCount > 64 || !Number.isInteger(handle.laneCapacity) ||
                        handle.laneCapacity < 1) {
                        throw new Error('SharedCommandBuffer schema mismatch for ' + schemaId);
                    }
                    return new SharedCommandBufferInstance(
                        definition,
                        SharedBuffer.attach(handle.buffer),
                        handle.workerCount,
                        handle.laneCapacity,
                        false,
                    );
                },
            });
        }
    }

)JS";
        code += R"JS(

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

    function exposeWorkerTasks(handlers) {
        if (!handlers || typeof handlers !== 'object') {
            throw new TypeError('WorkerPool task handlers must be an object');
        }
        const taskHandlers = new Map();
        for (const [name, handler] of Object.entries(handlers)) {
            if (!name || typeof handler !== 'function') {
                throw new TypeError('WorkerPool task handler must be a named function');
            }
            taskHandlers.set(name, handler);
        }
        if (taskHandlers.size === 0) throw new TypeError('WorkerPool needs at least one task handler');

        let poolWorkerIndex = -1;
        let poolWorkerCount = 0;
        globalThis.onmessage = event => {
            const task = event.data;
            if (task && task.$mystralWorkerPoolInit === 1) {
                poolWorkerIndex = task.workerIndex;
                poolWorkerCount = task.workerCount;
                postMessage({
                    $mystralWorkerPoolReady: 1,
                    workerIndex: poolWorkerIndex,
                });
                return;
            }
            if (!task || task.$mystralWorkerPoolTask !== 1) return;

            if (poolWorkerIndex < 0 && Number.isInteger(task.workerIndex)) {
                poolWorkerIndex = task.workerIndex;
                poolWorkerCount = task.workerCount;
            }

            const handler = taskHandlers.get(task.taskName);
            const execute = async () => {
                if (!handler) throw new Error('Unknown WorkerPool task: ' + task.taskName);
                if (poolWorkerIndex < 0) throw new Error('WorkerPool Worker was not initialized');

                const sharedControl = task.control
                    ? task.control instanceof SharedBuffer
                        ? task.control
                        : SharedBuffer.attach(task.control)
                    : null;
                const control = sharedControl
                    ? new Int32Array(sharedControl.buffer, 0, 4)
                    : null;
                const isCancelled = () => Boolean(
                    task.cancellable && control !== null && Atomics.load(control, 2) !== 0);

                if (task.schedule === 'dynamic') {
                    if (!control) throw new Error('WorkerPool dynamic task is missing its control buffer');
                    const counter = control;
                    const chunks = task.resultMode === 'discard' ? null : [];
                    for (;;) {
                        if (isCancelled()) break;
                        const chunkIndex = Atomics.add(counter, 0, 1);
                        if (chunkIndex >= task.chunkCount) break;
                        const begin = task.start + chunkIndex * task.grainSize;
                        const end = Math.min(task.end, begin + task.grainSize);
                        const value = await handler({
                            data: task.data,
                            round: task.round,
                            workerIndex: poolWorkerIndex,
                            workerCount: poolWorkerCount,
                            chunkIndex,
                            begin,
                            end,
                            isCancelled,
                        });
                        if (value && value.$mystralWorkerPoolTransferResult === 1) {
                            throw new Error('Dynamic WorkerPool tasks cannot transfer per-chunk buffers');
                        }
                        if (chunks) chunks.push([chunkIndex, value]);
                    }
                    if (!chunks) {
                        const previous = Atomics.sub(counter, 1, 1);
                        return {
                            $mystralWorkerPoolBarrier: 1,
                            publish: previous === 1,
                        };
                    }
                    return { $mystralWorkerPoolChunks: 1, chunks };
                }

                if (isCancelled()) return undefined;

                return handler({
                    data: task.data,
                    round: task.round,
                    workerIndex: poolWorkerIndex,
                    workerCount: poolWorkerCount,
                    chunkIndex: task.chunkIndex,
                    begin: task.begin,
                    end: task.end,
                    isCancelled,
                });
            };

            Promise.resolve().then(execute).then(result => {
                if (result && result.$mystralWorkerPoolBarrier === 1 && !result.publish) return;
                let value = result;
                let transfers = [];
                if (result && result.$mystralWorkerPoolTransferResult === 1) {
                    value = result.value;
                    transfers = result.transferList;
                }
                postMessage({
                    $mystralWorkerPoolResult: 1,
                    round: task.round,
                    workerIndex: poolWorkerIndex,
                    ok: true,
                    value,
                }, transfers);
            }).catch(error => {
                postMessage({
                    $mystralWorkerPoolResult: 1,
                    round: task.round,
                    workerIndex: poolWorkerIndex,
                    ok: false,
                    error: error && error.message ? error.message : String(error),
                    stack: error && error.stack ? String(error.stack) : '',
                });
            });
        };
    }

    function deterministicReduce(values, reducer = 'sum', initialValue) {
        if (!Array.isArray(values)) throw new TypeError('deterministicReduce values must be an array');
        const hasInitial = arguments.length >= 3;
        let reduce;
        let accumulator = initialValue;
        if (typeof reducer === 'function') {
            reduce = reducer;
            if (!hasInitial) {
                if (values.length === 0) throw new TypeError('Empty reduction requires an initial value');
                accumulator = values[0];
            }
        } else if (reducer === 'sum') {
            reduce = (left, right) => left + right;
            if (!hasInitial) accumulator = 0;
        } else if (reducer === 'count') {
            reduce = (left, right) => left + (right ? 1 : 0);
            if (!hasInitial) accumulator = 0;
        } else if (reducer === 'min') {
            reduce = (left, right) => Math.min(left, right);
            if (!hasInitial) accumulator = Infinity;
        } else if (reducer === 'max') {
            reduce = (left, right) => Math.max(left, right);
            if (!hasInitial) accumulator = -Infinity;
        } else {
            throw new TypeError('Unsupported deterministic reducer: ' + reducer);
        }
        const start = typeof reducer === 'function' && !hasInitial ? 1 : 0;
        for (let index = start; index < values.length; index++) {
            accumulator = reduce(accumulator, values[index], index, values);
        }
        return accumulator;
    }

    function createAbortError(reason) {
        if (reason instanceof Error) return reason;
        const error = new Error(
            reason === undefined ? 'WorkerPool round was aborted' : String(reason));
        error.name = 'AbortError';
        return error;
    }

)JS";
        code += R"JS(
    const workerPoolConstructionKey = Object.freeze({});

    class WorkerPool {
        static async create(workerUrl, options = {}) {
            const pool = new WorkerPool(workerPoolConstructionKey, workerUrl, options);
            await pool._readyPromise;
            return pool;
        }

        constructor(constructionKey, workerUrl, options = {}) {
            if (constructionKey !== workerPoolConstructionKey) {
                throw new TypeError('Use await WorkerPool.create(workerUrl, options)');
            }
            if (typeof Worker !== 'function') {
                throw new Error('WorkerPool cannot create Workers at this runtime nesting depth');
            }
            const runtimeSize = globalThis.__mystralWorkerPoolConcurrency;
            const defaultSize = Number.isInteger(runtimeSize)
                ? runtimeSize
                : globalThis.navigator && Number.isInteger(globalThis.navigator.hardwareConcurrency)
                    ? globalThis.navigator.hardwareConcurrency
                    : 4;
            const size = options.size === undefined ? Math.min(64, defaultSize) : options.size;
            if (!Number.isInteger(size) || size < 1 || size > 64) {
                throw new RangeError('WorkerPool size must be between 1 and 64');
            }
            const maxQueuedRounds = options.maxQueuedRounds === undefined
                ? 64
                : options.maxQueuedRounds;
            if (!Number.isInteger(maxQueuedRounds) || maxQueuedRounds < 1 || maxQueuedRounds > 4096) {
                throw new RangeError('WorkerPool maxQueuedRounds must be between 1 and 4096');
            }
            const startupTimeoutMs = options.startupTimeoutMs === undefined
                ? 30_000
                : options.startupTimeoutMs;
            if (!Number.isFinite(startupTimeoutMs) || startupTimeoutMs < 1 ||
                startupTimeoutMs > 300_000) {
                throw new RangeError('WorkerPool startupTimeoutMs must be between 1 and 300000');
            }
            this.size = size;
            this.maxQueuedRounds = maxQueuedRounds;
            this.startupTimeoutMs = startupTimeoutMs;
            this._round = 0;
            this._active = null;
            this._failure = null;
            this._terminated = false;
            this._workers = [];
            this._queue = [];
            this._broadcastRounds = 0;
            this._barrierRounds = 0;
            this._readyWorkers = 0;
            this._workerReady = new Array(size).fill(false);
            this._isReady = false;
            this._readySettled = false;
            this._startupTimer = null;
            this._readyPromise = new Promise((resolve, reject) => {
                this._readyResolve = resolve;
                this._readyReject = reject;
            });
            this._readyPromise.catch(() => {});
            try {
                for (let index = 0; index < size; index++) {
                    const worker = new Worker(workerUrl, {
                        type: 'module',
                        name: (options.name || 'worker-pool') + '-' + index,
                    });
                    worker.onmessage = event => this._handleMessage(index, event.data);
                    worker.onerror = event => this._fail(new Error(event.message || 'WorkerPool Worker failed'));
                    this._workers.push(worker);
                    worker.postMessage({
                        $mystralWorkerPoolInit: 1,
                        workerIndex: index,
                        workerCount: size,
                    });
                }
                if (!this._isReady) {
                    this._startupTimer = setTimeout(() => this._fail(new Error(
                        'WorkerPool startup timed out before every task Worker became ready')),
                    startupTimeoutMs);
                }
            } catch (error) {
                for (const worker of this._workers) worker.terminate();
                this._workers.length = 0;
                this._terminated = true;
                this._settleReady(error);
                throw error;
            }
        }

        get busy() { return this._active !== null || this._queue.length > 0; }
        get queuedRounds() { return this._queue.length; }

        parallelFor(taskName, data, options = {}) {
            const schedule = options.schedule === undefined ? 'dynamic' : options.schedule;
            if (schedule !== 'dynamic' && schedule !== 'static') {
                return Promise.reject(new RangeError('WorkerPool schedule must be dynamic or static'));
            }
            return this._enqueue(taskName, data, options, schedule, 'chunks');
        }

        forEach(taskName, data, options = {}) {
            return this._enqueue(taskName, data, options, 'dynamic', 'discard');
        }

        resizeTable(definition, table, options) {
            if (this.busy) throw new Error('SharedTable can only be resized while WorkerPool is idle');
            if (!definition || typeof definition.resize !== 'function') {
                throw new TypeError('WorkerPool resizeTable requires a SharedTable definition');
            }
            return definition.resize(table, options);
        }

        stats() {
            return Object.freeze({
                size: this.size,
                ready: this._isReady,
                busy: this.busy,
                queuedRounds: this._queue.length,
                broadcastRounds: this._broadcastRounds,
                barrierRounds: this._barrierRounds,
                completedRounds: this._round - (this._active ? 1 : 0),
            });
        }

        _terminate() {
            if (this._terminated) return;
            this._terminated = true;
            this._settleReady(new Error('WorkerPool terminated before startup completed'));
            if (this._active) {
                if (this._active.control) this._active.control.release();
                this._cleanupAbort(this._active);
                this._active.reject(new Error('WorkerPool terminated during an active round'));
                this._active = null;
            }
            const error = new Error('WorkerPool terminated before a queued round started');
            for (const request of this._queue.splice(0)) {
                this._cleanupAbort(request);
                request.reject(error);
            }
            for (const worker of this._workers) worker.terminate();
            this._workers.length = 0;
        }

        async close() {
            this._terminate();
        }

)JS";
        code += R"JS(
        _settleReady(error) {
            if (this._readySettled) return;
            this._readySettled = true;
            if (this._startupTimer !== null) {
                clearTimeout(this._startupTimer);
                this._startupTimer = null;
            }
            if (error) {
                this._readyReject(error);
            } else {
                this._isReady = true;
                this._readyResolve(this);
                this._pump();
            }
            this._readyResolve = null;
            this._readyReject = null;
        }

        _cleanupAbort(request) {
            if (!request || !request.signal || !request.abortHandler) return;
            request.signal.removeEventListener('abort', request.abortHandler);
            request.abortHandler = null;
        }

        _abortRequest(request) {
            const error = createAbortError(request.signal && request.signal.reason);
            if (this._active && this._active.request === request) {
                if (this._active.aborted) return;
                this._active.aborted = true;
                this._active.abortError = error;
                if (this._active.control) {
                    const control = new Int32Array(this._active.control.buffer, 0, 4);
                    Atomics.store(control, 2, 1);
                }
                return;
            }

            const index = this._queue.indexOf(request);
            if (index < 0) return;
            this._queue.splice(index, 1);
            this._cleanupAbort(request);
            request.reject(error);
        }

        _enqueue(taskName, data, options, schedule, resultMode) {
            if (this._terminated) return Promise.reject(new Error('WorkerPool is terminated'));
            if (this._failure) return Promise.reject(this._failure);
            if (typeof taskName !== 'string' || !taskName) {
                return Promise.reject(new TypeError('WorkerPool taskName is required'));
            }
            if (!options || typeof options !== 'object') {
                return Promise.reject(new TypeError('WorkerPool options must be an object'));
            }
            const signal = options.signal;
            if (signal !== undefined && (!signal || typeof signal !== 'object' ||
                typeof signal.aborted !== 'boolean' ||
                typeof signal.addEventListener !== 'function' ||
                typeof signal.removeEventListener !== 'function')) {
                return Promise.reject(new TypeError(
                    'WorkerPool signal must implement the AbortSignal contract'));
            }
            if (signal && signal.aborted) {
                return Promise.reject(createAbortError(signal.reason));
            }
            if (this._queue.length >= this.maxQueuedRounds) {
                return Promise.reject(new RangeError('WorkerPool round queue is full'));
            }

            const start = options.start === undefined ? 0 : options.start;
            const end = options.end === undefined ? options.length : options.end;
            if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) ||
                start < 0 || end < start) {
                return Promise.reject(new RangeError(
                    'WorkerPool requires a valid start/end or length'));
            }

            let grainSize = 0;
            let chunkCount = this.size;
            if (resultMode === 'discard' &&
                Object.prototype.hasOwnProperty.call(options, 'reducer')) {
                return Promise.reject(new RangeError(
                    'WorkerPool forEach cannot use a reducer'));
            }
            if (schedule === 'dynamic') {
                const length = end - start;
                const automaticGrain = Math.max(1, Math.ceil(length / (this.size * 8)));
                grainSize = options.grainSize === undefined ? automaticGrain : options.grainSize;
                if (!Number.isSafeInteger(grainSize) || grainSize < 1) {
                    return Promise.reject(new RangeError('WorkerPool grainSize must be positive'));
                }
                chunkCount = Math.ceil(length / grainSize);
                if (chunkCount > 0x7fffffff) {
                    return Promise.reject(new RangeError('WorkerPool dynamic chunk count is too large'));
                }
            }

            return new Promise((resolve, reject) => {
                const request = {
                    taskName,
                    data,
                    start,
                    end,
                    schedule,
                    grainSize,
                    chunkCount,
                    resultMode,
                    reducer: options.reducer,
                    hasReducer: Object.prototype.hasOwnProperty.call(options, 'reducer'),
                    initialValue: options.initialValue,
                    hasInitialValue: Object.prototype.hasOwnProperty.call(options, 'initialValue'),
                    signal,
                    abortHandler: null,
                    resolve,
                    reject,
                };
                this._queue.push(request);
                if (signal) {
                    request.abortHandler = () => this._abortRequest(request);
                    try {
                        signal.addEventListener('abort', request.abortHandler, { once: true });
                    } catch (error) {
                        const index = this._queue.indexOf(request);
                        if (index >= 0) this._queue.splice(index, 1);
                        request.abortHandler = null;
                        reject(error);
                        return;
                    }
                    if (signal.aborted) {
                        this._abortRequest(request);
                        return;
                    }
                }
                this._pump();
            });
        }

)JS";
        code += R"JS(
        _pump() {
            if (!this._isReady || this._active || this._terminated ||
                this._failure || this._queue.length === 0) return;
            const request = this._queue.shift();
            if (request.signal && request.signal.aborted) {
                this._cleanupAbort(request);
                request.reject(createAbortError(request.signal.reason));
                this._pump();
                return;
            }
            const round = ++this._round;
            const active = {
                ...request,
                request,
                round,
                remaining: this.size,
                completed: new Array(this.size).fill(false),
                results: new Array(this.size),
                chunkResults: new Array(request.chunkCount),
                chunkCompleted: new Array(request.chunkCount).fill(false),
                receivedChunks: 0,
                control: null,
                barrierOnly: request.resultMode === 'discard',
                aborted: false,
                abortError: null,
            };
            if (active.barrierOnly) active.remaining = 1;
            this._active = active;

            try {
                if (request.schedule === 'dynamic' || request.signal) {
                    active.control = SharedBuffer.allocate(64);
                    const control = new Int32Array(active.control.buffer, 0, 4);
                    Atomics.store(control, 0, 0);
                    Atomics.store(control, 1, this.size);
                    Atomics.store(control, 2, 0);
                }
                if (request.schedule === 'dynamic') {
                    const task = {
                        $mystralWorkerPoolTask: 1,
                        round,
                        taskName: request.taskName,
                        workerCount: this.size,
                        schedule: 'dynamic',
                        start: request.start,
                        end: request.end,
                        grainSize: request.grainSize,
                        chunkCount: request.chunkCount,
                        resultMode: request.resultMode,
                        cancellable: Boolean(request.signal),
                        control: active.control,
                        data: request.data,
                    };
                    if (!this._broadcast(task)) {
                        for (const worker of this._workers) worker.postMessage(task);
                    }
                } else {
                    const partitions = deterministicPartitions(
                        request.start, request.end, this.size);
                    for (let index = 0; index < this.size; index++) {
                        this._workers[index].postMessage({
                            $mystralWorkerPoolTask: 1,
                            round,
                            taskName: request.taskName,
                            workerIndex: index,
                            workerCount: this.size,
                            schedule: 'static',
                            cancellable: Boolean(request.signal),
                            chunkIndex: index,
                            begin: partitions[index].begin,
                            end: partitions[index].end,
                            control: active.control,
                            data: request.data,
                        });
                    }
                }
            } catch (error) {
                this._fail(error);
            }
        }

        _broadcast(task) {
            if (typeof globalThis.__mystralWorkerBroadcast !== 'function') return false;
            const ids = this._workers.map(worker => worker._id);
            if (!ids.every(Number.isInteger)) return false;
            const prepared = __mystralPrepareMessage(task);
            if (prepared.transfers.length !== 0) return false;
            const status = globalThis.__mystralWorkerBroadcast(ids, prepared.payload);
            if (status === 0) {
                this._broadcastRounds++;
                return true;
            }
            if (status === 2) throw new RangeError('Worker input queue is full');
            if (status === 3) throw new RangeError('Worker message exceeds the input queue byte limit');
            throw new Error('Worker is no longer running');
        }

        _handleMessage(workerIndex, message) {
            if (!message) return;
            if (message.$mystralWorkerPoolReady === 1) {
                if (this._terminated || this._workerReady[workerIndex]) return;
                if (message.workerIndex !== workerIndex) {
                    this._fail(new Error('WorkerPool Worker reported an invalid startup index'));
                    return;
                }
                this._workerReady[workerIndex] = true;
                this._readyWorkers++;
                if (this._readyWorkers === this.size) this._settleReady();
                return;
            }
            const active = this._active;
            if (!active || message.$mystralWorkerPoolResult !== 1 ||
                message.round !== active.round || message.workerIndex !== workerIndex) return;
            if (active.completed[workerIndex]) return;
            if (!message.ok) {
                const error = new Error(message.error || 'WorkerPool task failed');
                if (message.stack) error.stack = message.stack;
                this._fail(error);
                return;
            }
            active.completed[workerIndex] = true;
            if (active.schedule === 'dynamic') {
                const chunkMessage = message.value;
                if (active.barrierOnly) {
                    if (!chunkMessage || chunkMessage.$mystralWorkerPoolBarrier !== 1) {
                        this._fail(new Error('WorkerPool barrier returned an invalid result'));
                        return;
                    }
                } else if (!chunkMessage || chunkMessage.$mystralWorkerPoolChunks !== 1 ||
                    !Array.isArray(chunkMessage.chunks)) {
                    this._fail(new Error('WorkerPool dynamic task returned an invalid chunk result'));
                    return;
                } else {
                    for (const entry of chunkMessage.chunks) {
                        const chunkIndex = entry && entry[0];
                        if (!Array.isArray(entry) || !Number.isInteger(chunkIndex) ||
                            chunkIndex < 0 || chunkIndex >= active.chunkCount ||
                            active.chunkCompleted[chunkIndex]) {
                            this._fail(new Error('WorkerPool dynamic task returned an invalid chunk index'));
                            return;
                        }
                        active.chunkCompleted[chunkIndex] = true;
                        active.chunkResults[chunkIndex] = entry[1];
                        active.receivedChunks++;
                    }
                }
            } else {
                active.results[workerIndex] = message.value;
            }
            active.remaining--;
            if (active.remaining === 0) {
                if (!active.aborted && active.schedule === 'dynamic' && !active.barrierOnly &&
                    active.receivedChunks !== active.chunkCount) {
                    this._fail(new Error('WorkerPool dynamic task did not complete every chunk'));
                    return;
                }
                this._active = null;
                if (active.control) active.control.release();
                this._cleanupAbort(active);
                try {
                    if (active.barrierOnly) this._barrierRounds++;
                    if (active.aborted) {
                        active.reject(active.abortError || createAbortError());
                    } else {
                        let result = active.schedule === 'dynamic'
                            ? (active.barrierOnly ? undefined : active.chunkResults)
                            : active.results;
                        if (active.hasReducer) {
                            result = active.hasInitialValue
                                ? deterministicReduce(result, active.reducer, active.initialValue)
                                : deterministicReduce(result, active.reducer);
                        }
                        active.resolve(result);
                    }
                } catch (error) {
                    active.reject(error);
                }
                this._pump();
            }
        }

        _fail(error) {
            const failure = error instanceof Error ? error : new Error(String(error));
            this._failure = failure;
            this._settleReady(failure);
            if (this._active) {
                if (this._active.control) this._active.control.release();
                this._cleanupAbort(this._active);
                this._active.reject(failure);
                this._active = null;
            }
            for (const request of this._queue.splice(0)) {
                this._cleanupAbort(request);
                request.reject(failure);
            }
            for (const worker of this._workers) worker.terminate();
            this._workers.length = 0;
            this._terminated = true;
        }
    }

    globalThis.__mystralSerializeMessage = serializeMessage;
    globalThis.__mystralPrepareMessage = prepareMessage;
    globalThis.__mystralParseMessage = parseMessage;
    globalThis.__mystralShared = Object.freeze({
        SharedBuffer,
        SharedTable,
        SharedQueue,
        SharedCommandBuffer,
    });
    globalThis.__mystralWorkerPool = Object.freeze({
        WorkerPool,
        exposeWorkerTasks,
        transferResult,
    });
    if (typeof globalThis.WorkerPool === 'undefined') globalThis.WorkerPool = WorkerPool;
})();
)JS";
        return code;
    }();
    return source.c_str();
}

}  // namespace mystral::workers
