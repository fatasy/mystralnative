/**
 * QuickJS JavaScript Engine Implementation
 *
 * QuickJS is a tiny (~600KB) JavaScript engine with no JIT,
 * making it ideal for consoles, embedded systems, and fallback on all platforms.
 */

#include "mystral/js/engine.h"
#include "mystral/js/module_system.h"
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <atomic>
#include <cstring>
#include <sstream>

#ifdef __ANDROID__
#include <android/log.h>
#define ANDROID_LOG_TAG "MystralJS"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, ANDROID_LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, ANDROID_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, ANDROID_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, ANDROID_LOG_TAG, __VA_ARGS__)
#else
#define LOGD(...) do { } while(0)
#define LOGI(...) do { } while(0)
#define LOGW(...) do { } while(0)
#define LOGE(...) do { } while(0)
#endif

#if defined(MYSTRAL_JS_QUICKJS)
#include "quickjs.h"

namespace mystral {
namespace js {

// Global set of protected handles that should not be deleted by nativeCallback cleanup
thread_local std::unordered_set<void*> g_protectedHandles;

struct QuickJSTransferredBacking {
    std::shared_ptr<void> owner;
    bool detaching = false;
    bool finalized = false;
};

static void freeQuickJSTransferredBacking(JSRuntime*, void* opaque, void*) {
    auto* backing = static_cast<QuickJSTransferredBacking*>(opaque);
    if (!backing) return;
    backing->owner.reset();
    if (!backing->detaching) backing->finalized = true;
}

static char* quickjsModuleNormalize(JSContext* ctx,
                                    const char* module_base_name,
                                    const char* module_name,
                                    void* opaque) {
    (void)opaque;
    auto* moduleSystem = getModuleSystem();
    if (!moduleSystem) {
        return js_strdup(ctx, module_name);
    }

    ResolvedModule resolved;
    std::string error;
    std::string referrer = module_base_name ? module_base_name : "";
    if (!moduleSystem->resolveForImport(module_name, referrer, resolved, error)) {
        JS_ThrowReferenceError(ctx, "%s", error.c_str());
        return nullptr;
    }
    return js_strdup(ctx, resolved.resolved.path.c_str());
}

static JSModuleDef* quickjsModuleLoader(JSContext* ctx,
                                        const char* module_name,
                                        void* opaque) {
    (void)opaque;
    auto* moduleSystem = getModuleSystem();
    if (!moduleSystem) {
        JS_ThrowReferenceError(ctx, "Module system not initialized");
        return nullptr;
    }

    ResolvedModule resolved;
    std::string error;
    if (!moduleSystem->resolver().resolveResolvedPath(module_name, resolved, error)) {
        JS_ThrowReferenceError(ctx, "%s", error.c_str());
        return nullptr;
    }

    std::string source;
    std::string filename;
    if (!moduleSystem->getEsmSource(resolved, module_name, source, filename, error)) {
        JS_ThrowReferenceError(ctx, "%s", error.c_str());
        return nullptr;
    }

    JSValue result = JS_Eval(ctx, source.c_str(), source.size(),
        filename.c_str(), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(result)) {
        return nullptr;
    }

    JSModuleDef* module = (JSModuleDef*)JS_VALUE_GET_PTR(result);
    JS_FreeValue(ctx, result);
    return module;
}

class QuickJSEngine : public Engine {
public:
    QuickJSEngine() {
        std::cout << "[QuickJS] Creating engine..." << std::endl;

        runtime_ = JS_NewRuntime();
        if (!runtime_) {
            std::cerr << "[QuickJS] Failed to create runtime" << std::endl;
            return;
        }

        JS_SetInterruptHandler(runtime_, &QuickJSEngine::interruptHandler, this);

        context_ = JS_NewContext(runtime_);
        if (!context_) {
            std::cerr << "[QuickJS] Failed to create context" << std::endl;
            return;
        }

        JS_SetModuleLoaderFunc(runtime_, quickjsModuleNormalize, quickjsModuleLoader, nullptr);

        // Set up standard globals
        setupGlobals();

        std::cout << "[QuickJS] Engine created successfully" << std::endl;
    }

    ~QuickJSEngine() override {
        std::cout << "[QuickJS] Destroying engine..." << std::endl;

        if (context_ && runtime_) {
            // Execute all pending promise jobs
            JSContext* ctx;
            while (JS_ExecutePendingJob(runtime_, &ctx) > 0) {
                // Keep running until no more jobs
            }

            // Free all remaining protected handles
            for (void* ptr : g_protectedHandles) {
                JSValue* val = (JSValue*)ptr;
                JS_FreeValue(context_, *val);
                allocatedHandles_.erase(ptr);
                delete val;
            }
            g_protectedHandles.clear();

            for (void* ptr : allocatedHandles_) {
                auto* value = static_cast<JSValue*>(ptr);
                JS_FreeValue(context_, *value);
                delete value;
            }
            allocatedHandles_.clear();

            // Delete all allocated function objects
            for (auto* fn : allocatedFunctions_) {
                delete fn;
            }
            allocatedFunctions_.clear();
            for (auto* method : allocatedMethods_) {
                delete method;
            }
            allocatedMethods_.clear();

            // Clear private data map
            privateDataMap_.clear();

            // Run garbage collection multiple times to clean up cycles
            JS_RunGC(runtime_);
            JS_RunGC(runtime_);
            JS_RunGC(runtime_);
        }

        if (context_) {
            JS_FreeContext(context_);
        }
        if (runtime_) {
            JS_FreeRuntime(runtime_);
        }

        engineInstance_ = nullptr;
    }

    EngineType getType() const override { return EngineType::QuickJS; }
    const char* getName() const override { return "QuickJS"; }

    // ========================================================================
    // Script Evaluation
    // ========================================================================

    bool eval(const char* code, const char* filename) override {
        // Use JS_EVAL_TYPE_MODULE to support import.meta
        JSValue result = JS_Eval(context_, code, strlen(code), filename, JS_EVAL_TYPE_MODULE);

        if (JS_IsException(result)) {
            JSValue exception = JS_GetException(context_);
            reportException(exception);
            JS_FreeValue(context_, exception);
            JS_FreeValue(context_, result);
            return false;
        }

        JS_FreeValue(context_, result);

        // Execute any pending Promise jobs (microtasks)
        executePendingJobs();

        return true;
    }

    // Execute pending Promise jobs (microtasks)
    void executePendingJobs() {
        JSContext* ctx;
        int ret;
        while ((ret = JS_ExecutePendingJob(runtime_, &ctx)) > 0) {
            // Job executed successfully
        }
        if (ret < 0) {
            // An exception occurred during job execution
            JSValue exception = JS_GetException(ctx);
            reportException(exception);
            JS_FreeValue(ctx, exception);
        }
    }

    JSValueHandle evalWithResult(const char* code, const char* filename) override {
        // Use JS_EVAL_TYPE_MODULE to support import.meta
        JSValue result = JS_Eval(context_, code, strlen(code), filename, JS_EVAL_TYPE_MODULE);

        if (JS_IsException(result)) {
            JSValue exception = JS_GetException(context_);
            reportException(exception);
            lastException_ = exception;
            JS_FreeValue(context_, result);
            return {nullptr, context_};
        }

        // Store the result (caller must free)
        JSValue* stored = new JSValue(result);
        allocatedHandles_.insert(stored);
        return {stored, context_};
    }

    bool evalScript(const char* code, const char* filename) override {
        JSValue result = JS_Eval(context_, code, strlen(code), filename, JS_EVAL_TYPE_GLOBAL);

        if (JS_IsException(result)) {
            JSValue exception = JS_GetException(context_);
            reportException(exception);
            JS_FreeValue(context_, exception);
            JS_FreeValue(context_, result);
            return false;
        }

        JS_FreeValue(context_, result);
        executePendingJobs();
        return true;
    }

    JSValueHandle evalScriptWithResult(const char* code, const char* filename) override {
        JSValue result = JS_Eval(context_, code, strlen(code), filename, JS_EVAL_TYPE_GLOBAL);

        if (JS_IsException(result)) {
            JSValue exception = JS_GetException(context_);
            reportException(exception);
            lastException_ = exception;
            JS_FreeValue(context_, result);
            return {nullptr, context_};
        }

        executePendingJobs();
        JSValue* stored = new JSValue(result);
        allocatedHandles_.insert(stored);
        return {stored, context_};
    }

    // ========================================================================
    // Global Object Access
    // ========================================================================

    JSValueHandle getGlobal() override {
        JSValue global = JS_GetGlobalObject(context_);
        JSValue* stored = new JSValue(global);
        allocatedHandles_.insert(stored);
        return {stored, context_};
    }

    bool setGlobalProperty(const char* name, JSValueHandle value) override {
        JSValue global = JS_GetGlobalObject(context_);
        JSValue* val = (JSValue*)value.ptr;
        JS_SetPropertyStr(context_, global, name, JS_DupValue(context_, *val));
        JS_FreeValue(context_, global);
        return true;
    }

    JSValueHandle getGlobalProperty(const char* name) override {
        JSValue global = JS_GetGlobalObject(context_);
        JSValue result = JS_GetPropertyStr(context_, global, name);
        JS_FreeValue(context_, global);
        JSValue* stored = new JSValue(result);
        allocatedHandles_.insert(stored);
        return {stored, context_};
    }

    // ========================================================================
    // Value Creation
    // ========================================================================

    JSValueHandle newUndefined() override {
        JSValue* val = new JSValue(JS_UNDEFINED);
        allocatedHandles_.insert(val);
        return {val, context_};
    }

    JSValueHandle newNull() override {
        JSValue* val = new JSValue(JS_NULL);
        allocatedHandles_.insert(val);
        return {val, context_};
    }

    JSValueHandle newBoolean(bool value) override {
        JSValue* val = new JSValue(JS_NewBool(context_, value));
        allocatedHandles_.insert(val);
        return {val, context_};
    }

    JSValueHandle newNumber(double value) override {
        JSValue* val = new JSValue(JS_NewFloat64(context_, value));
        allocatedHandles_.insert(val);
        return {val, context_};
    }

    JSValueHandle newString(const char* value) override {
        JSValue* val = new JSValue(JS_NewString(context_, value));
        allocatedHandles_.insert(val);
        return {val, context_};
    }

    JSValueHandle newObject() override {
        JSValue* val = new JSValue(JS_NewObject(context_));
        allocatedHandles_.insert(val);
        return {val, context_};
    }

    JSValueHandle newArray(size_t length) override {
        JSValue* val = new JSValue(JS_NewArray(context_));
        allocatedHandles_.insert(val);
        return {val, context_};
    }

    JSValueHandle newArrayBuffer(const uint8_t* data, size_t length) override {
        JSValue* val = new JSValue(JS_NewArrayBufferCopy(context_, data, length));
        allocatedHandles_.insert(val);
        return {val, context_};
    }

    JSValueHandle newArrayBufferExternal(void* data, size_t length) override {
        // Create an ArrayBuffer that directly references external memory (no copy)
        // Pass nullptr for free_func since we don't own this memory (GPU manages it)
        JSValue* val = new JSValue(JS_NewArrayBuffer(context_, (uint8_t*)data, length, nullptr, nullptr, false));
        allocatedHandles_.insert(val);
        return {val, context_};
    }

    bool transferArrayBuffer(JSValueHandle value, TransferredArrayBuffer& result) override {
        auto* stored = static_cast<JSValue*>(value.ptr);
        if (!stored || !JS_IsArrayBuffer(*stored)) return false;
        size_t length = 0;
        uint8_t* data = JS_GetArrayBuffer(context_, &length, *stored);
        if (!data && length != 0) return false;
        auto bytes = std::make_shared<std::vector<uint8_t>>();
        if (length > 0) bytes->assign(data, data + length);
        result.data = bytes->data();
        result.size = bytes->size();
        result.owner = std::static_pointer_cast<void>(bytes);
        auto backingIt = transferredBackingOwners_.find(JS_VALUE_GET_PTR(*stored));
        std::shared_ptr<QuickJSTransferredBacking> backing;
        if (backingIt != transferredBackingOwners_.end()) {
            backing = backingIt->second;
            backing->detaching = true;
        }
        JS_DetachArrayBuffer(context_, *stored);
        if (backing) backing->detaching = false;
        return true;
    }

    JSValueHandle newTransferredArrayBuffer(const TransferredArrayBuffer& buffer) override {
        pruneTransferredBackingOwners();
        auto backing = std::make_shared<QuickJSTransferredBacking>();
        backing->owner = buffer.owner;
        JSValue value = JS_NewArrayBuffer(
            context_,
            static_cast<uint8_t*>(buffer.data),
            buffer.size,
            freeQuickJSTransferredBacking,
            backing.get(),
            false);
        if (JS_IsException(value)) {
            return {};
        }
        transferredBackingOwners_[JS_VALUE_GET_PTR(value)] = std::move(backing);
        auto* stored = new JSValue(value);
        allocatedHandles_.insert(stored);
        return {stored, context_};
    }

    JSValueHandle newSharedArrayBuffer(void* data,
                                       size_t length,
                                       std::shared_ptr<void> owner) override {
        auto* retainedOwner = new std::shared_ptr<void>(std::move(owner));
        JSValue value = JS_NewArrayBuffer(
            context_,
            static_cast<uint8_t*>(data),
            length,
            [](JSRuntime*, void* opaque, void*) {
                delete static_cast<std::shared_ptr<void>*>(opaque);
            },
            retainedOwner,
            true);
        if (JS_IsException(value)) {
            delete retainedOwner;
            return {};
        }
        auto* stored = new JSValue(value);
        allocatedHandles_.insert(stored);
        return {stored, context_};
    }

    bool supportsSharedArrayBuffer() const override { return true; }

    void* getArrayBufferData(JSValueHandle value, size_t* size) override {
        JSValue* val = (JSValue*)value.ptr;
        if (!val) return nullptr;

        size_t len = 0;
        uint8_t* data = JS_GetArrayBuffer(context_, &len, *val);

        if (!data) {
            // Try getting from TypedArray
            // Note: JS_GetTypedArrayBuffer returns byte_offset and byte_length, NOT element count!
            size_t byteOffset = 0;
            size_t byteLength = 0;
            size_t bytesPerElement = 0;
            JSValue buffer = JS_GetTypedArrayBuffer(context_, *val, &byteOffset, &byteLength, &bytesPerElement);
            if (!JS_IsException(buffer)) {
                size_t bufferLen = 0;
                data = JS_GetArrayBuffer(context_, &bufferLen, buffer);
                JS_FreeValue(context_, buffer);
                if (data) {
                    data += byteOffset;
                    len = byteLength;  // byteLength is already the byte count, don't multiply!
                }
            }
        }

        if (size) *size = len;
        return data;
    }

    JSValueHandle createFloat32Array(const float* data, size_t count) override {
        // Create ArrayBuffer with the data
        size_t byteLength = count * sizeof(float);
        JSValue buffer = JS_NewArrayBufferCopy(context_, (const uint8_t*)data, byteLength);

        // Create Float32Array from the buffer using JS_NewTypedArray
        // Signature: JS_NewTypedArray(ctx, count, buffer, byte_offset, buffer_provided)
        // We need to get the Float32Array constructor and call it manually
        JSValue global = JS_GetGlobalObject(context_);
        JSValue float32ArrayCtor = JS_GetPropertyStr(context_, global, "Float32Array");
        JS_FreeValue(context_, global);

        JSValue args[1] = { buffer };
        JSValue typedArray = JS_CallConstructor(context_, float32ArrayCtor, 1, args);

        JS_FreeValue(context_, float32ArrayCtor);
        JS_FreeValue(context_, buffer);

        JSValue* val = new JSValue(typedArray);
        allocatedHandles_.insert(val);
        return {val, context_};
    }

    JSValueHandle createFloat32ArrayView(float* data, size_t count) override {
        // Create ArrayBuffer backed by external memory (no copy)
        // Pass NULL for free_func since the caller (AudioBuffer) manages lifetime
        size_t byteLength = count * sizeof(float);
        JSValue buffer = JS_NewArrayBuffer(context_, (uint8_t*)data, byteLength, nullptr, nullptr, 0);

        JSValue global = JS_GetGlobalObject(context_);
        JSValue float32ArrayCtor = JS_GetPropertyStr(context_, global, "Float32Array");
        JS_FreeValue(context_, global);

        JSValue args[1] = { buffer };
        JSValue typedArray = JS_CallConstructor(context_, float32ArrayCtor, 1, args);

        JS_FreeValue(context_, float32ArrayCtor);
        JS_FreeValue(context_, buffer);

        JSValue* val = new JSValue(typedArray);
        allocatedHandles_.insert(val);
        return {val, context_};
    }

    JSValueHandle createUint32Array(const uint32_t* data, size_t count) override {
        size_t byteLength = count * sizeof(uint32_t);
        JSValue buffer = JS_NewArrayBufferCopy(context_, (const uint8_t*)data, byteLength);

        JSValue global = JS_GetGlobalObject(context_);
        JSValue uint32ArrayCtor = JS_GetPropertyStr(context_, global, "Uint32Array");
        JS_FreeValue(context_, global);

        JSValue args[1] = { buffer };
        JSValue typedArray = JS_CallConstructor(context_, uint32ArrayCtor, 1, args);

        JS_FreeValue(context_, uint32ArrayCtor);
        JS_FreeValue(context_, buffer);

        JSValue* val = new JSValue(typedArray);
        allocatedHandles_.insert(val);
        return {val, context_};
    }

    JSValueHandle createUint8Array(const uint8_t* data, size_t count) override {
        JSValue buffer = JS_NewArrayBufferCopy(context_, data, count);

        JSValue global = JS_GetGlobalObject(context_);
        JSValue uint8ArrayCtor = JS_GetPropertyStr(context_, global, "Uint8Array");
        JS_FreeValue(context_, global);

        JSValue args[1] = { buffer };
        JSValue typedArray = JS_CallConstructor(context_, uint8ArrayCtor, 1, args);

        JS_FreeValue(context_, uint8ArrayCtor);
        JS_FreeValue(context_, buffer);

        JSValue* val = new JSValue(typedArray);
        allocatedHandles_.insert(val);
        return {val, context_};
    }

    JSValueHandle newFunction(const char* name, NativeFunction fn) override {
        // Store the callback as a heap-allocated function
        auto* fnPtr = new NativeFunction(fn);
        allocatedFunctions_.push_back(fnPtr);  // Track for cleanup

        // Wrap the pointer in a BigInt64 JSValue so it can be passed through JS_NewCFunctionData
        JSValue ptrValue = JS_NewBigInt64(context_, (int64_t)(uintptr_t)fnPtr);

        JSValue func = JS_NewCFunctionData(context_, &nativeCallback, 0, 0, 1, &ptrValue);
        JS_FreeValue(context_, ptrValue);  // JS_NewCFunctionData dups the values

        JSValue* stored = new JSValue(func);
        allocatedHandles_.insert(stored);
        return {stored, context_};
    }

    JSValueHandle newMethod(const char* name, NativeMethod fn) override {
        auto* fnPtr = new NativeMethod(std::move(fn));
        allocatedMethods_.push_back(fnPtr);
        JSValue ptrValue = JS_NewBigInt64(context_, (int64_t)(uintptr_t)fnPtr);
        JSValue func = JS_NewCFunctionData(context_, &nativeMethodCallback, 0, 0, 1, &ptrValue);
        JS_FreeValue(context_, ptrValue);
        JSValue* stored = new JSValue(func);
        allocatedHandles_.insert(stored);
        return {stored, context_};
    }

    // ========================================================================
    // Value Conversion
    // ========================================================================

    bool toBoolean(JSValueHandle value) override {
        JSValue* val = (JSValue*)value.ptr;
        return JS_ToBool(context_, *val);
    }

    double toNumber(JSValueHandle value) override {
        JSValue* val = (JSValue*)value.ptr;
        double result;
        JS_ToFloat64(context_, &result, *val);
        return result;
    }

    std::string toString(JSValueHandle value) override {
        JSValue* val = (JSValue*)value.ptr;
        const char* str = JS_ToCString(context_, *val);
        if (!str) return "";
        std::string result(str);
        JS_FreeCString(context_, str);
        return result;
    }

    bool isUndefined(JSValueHandle value) override {
        JSValue* val = (JSValue*)value.ptr;
        return JS_IsUndefined(*val);
    }

    bool isNull(JSValueHandle value) override {
        JSValue* val = (JSValue*)value.ptr;
        return JS_IsNull(*val);
    }

    bool isBoolean(JSValueHandle value) override {
        JSValue* val = (JSValue*)value.ptr;
        return JS_IsBool(*val);
    }

    bool isNumber(JSValueHandle value) override {
        JSValue* val = (JSValue*)value.ptr;
        return JS_IsNumber(*val);
    }

    bool isString(JSValueHandle value) override {
        JSValue* val = (JSValue*)value.ptr;
        return JS_IsString(*val);
    }

    bool isObject(JSValueHandle value) override {
        JSValue* val = (JSValue*)value.ptr;
        return JS_IsObject(*val);
    }

    bool isArray(JSValueHandle value) override {
        JSValue* val = (JSValue*)value.ptr;
        return JS_IsArray(*val);  // quickjs-ng: only takes value, not context
    }

    bool isFunction(JSValueHandle value) override {
        JSValue* val = (JSValue*)value.ptr;
        return JS_IsFunction(context_, *val);
    }

    // ========================================================================
    // Object Operations
    // ========================================================================

    bool setProperty(JSValueHandle obj, const char* name, JSValueHandle value) override {
        JSValue* objVal = (JSValue*)obj.ptr;
        JSValue* val = (JSValue*)value.ptr;
        JS_SetPropertyStr(context_, *objVal, name, JS_DupValue(context_, *val));
        return true;
    }

    JSValueHandle getProperty(JSValueHandle obj, const char* name) override {
        JSValue* objVal = (JSValue*)obj.ptr;
        JSValue result = JS_GetPropertyStr(context_, *objVal, name);
        JSValue* stored = new JSValue(result);
        allocatedHandles_.insert(stored);
        return {stored, context_};
    }

    bool setPropertyIndex(JSValueHandle arr, uint32_t index, JSValueHandle value) override {
        JSValue* arrVal = (JSValue*)arr.ptr;
        JSValue* val = (JSValue*)value.ptr;
        JS_SetPropertyUint32(context_, *arrVal, index, JS_DupValue(context_, *val));
        return true;
    }

    JSValueHandle getPropertyIndex(JSValueHandle arr, uint32_t index) override {
        JSValue* arrVal = (JSValue*)arr.ptr;
        JSValue result = JS_GetPropertyUint32(context_, *arrVal, index);
        JSValue* stored = new JSValue(result);
        allocatedHandles_.insert(stored);
        return {stored, context_};
    }

    JSValueHandle call(JSValueHandle func, JSValueHandle thisArg, const std::vector<JSValueHandle>& args) override {
        JSValue* funcVal = (JSValue*)func.ptr;
        JSValue* thisVal = thisArg.ptr ? (JSValue*)thisArg.ptr : nullptr;

        std::vector<JSValue> jsArgs;
        jsArgs.reserve(args.size());
        for (const auto& arg : args) {
            jsArgs.push_back(*(JSValue*)arg.ptr);
        }

        JSValue result = JS_Call(context_, *funcVal,
            thisVal ? *thisVal : JS_UNDEFINED,
            (int)jsArgs.size(), jsArgs.data());

        if (JS_IsException(result)) {
            JSValue exception = JS_GetException(context_);
            reportException(exception);
            lastException_ = exception;
            return {nullptr, context_};
        }

        // Execute any pending Promise jobs (microtasks)
        executePendingJobs();

        JSValue* stored = new JSValue(result);
        allocatedHandles_.insert(stored);
        return {stored, context_};
    }

    // ========================================================================
    // Memory Management
    // ========================================================================

    void protect(JSValueHandle value) override {
        // The handle already owns a QuickJS reference. Mark callback arguments
        // so nativeCallback leaves that reference alive until unprotect().
        g_protectedHandles.insert(value.ptr);
    }

    void unprotect(JSValueHandle value) override {
        JSValue* val = (JSValue*)value.ptr;
        JS_FreeValue(context_, *val);
        // Remove from protected set and clean up
        g_protectedHandles.erase(value.ptr);
        allocatedHandles_.erase(value.ptr);
        delete val;
    }

    void releaseValue(JSValueHandle value) override {
        if (!value.ptr) return;
        JSValue* val = (JSValue*)value.ptr;
        JS_FreeValue(context_, *val);
        g_protectedHandles.erase(value.ptr);
        allocatedHandles_.erase(value.ptr);
        delete val;
    }

    void setConsoleCallback(ConsoleCallback callback) override {
        consoleCallback_ = std::move(callback);
    }

    void gc() override {
        JS_RunGC(runtime_);
    }

    MemoryStats getMemoryStats() const override {
        JSMemoryUsage usage = {};
        JS_ComputeMemoryUsage(runtime_, &usage);
        MemoryStats stats;
        stats.heapUsedBytes = usage.memory_used_size;
        stats.heapTotalBytes = usage.malloc_size;
        stats.nativeFunctions = allocatedFunctions_.size() + allocatedMethods_.size();
        return stats;
    }

    bool requestTermination() override {
        terminationRequested_.store(true, std::memory_order_release);
        return runtime_ != nullptr;
    }

    // ========================================================================
    // Error Handling
    // ========================================================================

    bool hasException() override {
        return !JS_IsNull(lastException_) && !JS_IsUndefined(lastException_);
    }

    std::string getException() override {
        if (JS_IsNull(lastException_) || JS_IsUndefined(lastException_)) {
            return "";
        }

        const char* str = JS_ToCString(context_, lastException_);
        std::string result = str ? str : "";
        if (str) JS_FreeCString(context_, str);

        JS_FreeValue(context_, lastException_);
        lastException_ = JS_UNDEFINED;
        return result;
    }

    void throwException(const char* message) override {
        lastException_ = JS_ThrowInternalError(context_, "%s", message);
    }

    // ========================================================================
    // Private Data
    // ========================================================================

    void setPrivateData(JSValueHandle obj, void* data) override {
        JSValue* val = (JSValue*)obj.ptr;
        // Use JS_VALUE_GET_PTR to get the actual object pointer as a unique key
        void* objPtr = JS_VALUE_GET_PTR(*val);
        privateDataMap_[objPtr] = data;
    }

    void* getPrivateData(JSValueHandle obj) override {
        JSValue* val = (JSValue*)obj.ptr;
        void* objPtr = JS_VALUE_GET_PTR(*val);
        auto it = privateDataMap_.find(objPtr);
        return it != privateDataMap_.end() ? it->second : nullptr;
    }

    // ========================================================================
    // Raw Context Access
    // ========================================================================

    void* getRawContext() override {
        return context_;
    }

private:
    void pruneTransferredBackingOwners() {
        for (auto it = transferredBackingOwners_.begin();
             it != transferredBackingOwners_.end();) {
            if (it->second->finalized) {
                it = transferredBackingOwners_.erase(it);
            } else {
                ++it;
            }
        }
    }

    static int interruptHandler(JSRuntime*, void* opaque) {
        auto* engine = static_cast<QuickJSEngine*>(opaque);
        return engine && engine->terminationRequested_.load(std::memory_order_acquire) ? 1 : 0;
    }

    void setupGlobals() {
        JSValue global = JS_GetGlobalObject(context_);

        // console object
        JSValue console = JS_NewObject(context_);
        JS_SetPropertyStr(context_, console, "log",
            JS_NewCFunction(context_, js_console_log, "log", 1));
        JS_SetPropertyStr(context_, console, "warn",
            JS_NewCFunction(context_, js_console_warn, "warn", 1));
        JS_SetPropertyStr(context_, console, "error",
            JS_NewCFunction(context_, js_console_error, "error", 1));
        JS_SetPropertyStr(context_, console, "info",
            JS_NewCFunction(context_, js_console_info, "info", 1));
        JS_SetPropertyStr(context_, console, "debug",
            JS_NewCFunction(context_, js_console_debug, "debug", 1));
        JS_SetPropertyStr(context_, global, "console", console);

        // performance.now()
        startTime_ = std::chrono::high_resolution_clock::now();
        JSValue performance = JS_NewObject(context_);

        // Store engine pointer for performance.now
        engineInstance_ = this;
        JS_SetPropertyStr(context_, performance, "now",
            JS_NewCFunction(context_, js_performance_now, "now", 0));
        JS_SetPropertyStr(context_, global, "performance", performance);

        // setTimeout/clearTimeout (basic)
        JS_SetPropertyStr(context_, global, "setTimeout",
            JS_NewCFunction(context_, js_set_timeout, "setTimeout", 2));
        JS_SetPropertyStr(context_, global, "clearTimeout",
            JS_NewCFunction(context_, js_clear_timeout, "clearTimeout", 1));

        JS_FreeValue(context_, global);
    }

    void reportException(JSValue exception) {
        if (terminationRequested_.load(std::memory_order_acquire)) return;
        const char* str = JS_ToCString(context_, exception);
        std::cerr << "[QuickJS] Error: " << (str ? str : "unknown") << std::endl;
        if (str) JS_FreeCString(context_, str);

        // Also try to get stack trace
        JSValue stack = JS_GetPropertyStr(context_, exception, "stack");
        if (!JS_IsUndefined(stack)) {
            const char* stackStr = JS_ToCString(context_, stack);
            if (stackStr) {
                std::cerr << "[QuickJS] Stack:\n" << stackStr << std::endl;
                JS_FreeCString(context_, stackStr);
            }
            JS_FreeValue(context_, stack);
        }
    }

    static JSValue nativeCallback(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv, int magic, JSValue* func_data) {
        // Extract the NativeFunction pointer from the BigInt64 stored in func_data[0]
        int64_t ptrVal;
        if (JS_ToBigInt64(ctx, &ptrVal, func_data[0]) < 0) {
            return JS_UNDEFINED;
        }
        NativeFunction* fn = (NativeFunction*)(uintptr_t)ptrVal;
        if (!fn) {
            return JS_UNDEFINED;
        }

        // Convert arguments
        std::vector<JSValueHandle> args;
        args.reserve(argc);
        for (int i = 0; i < argc; i++) {
            JSValue* stored = new JSValue(JS_DupValue(ctx, argv[i]));
            args.push_back({stored, ctx});
        }

        // Call the native function
        JSValueHandle result = (*fn)(ctx, args);

        // Clean up argument copies (skip protected handles)
        for (auto& arg : args) {
            // Skip handles that were protected during the callback
            if (g_protectedHandles.find(arg.ptr) != g_protectedHandles.end()) {
                continue;
            }
            JSValue* val = (JSValue*)arg.ptr;
            JS_FreeValue(ctx, *val);
            delete val;
        }

        if (result.ptr) {
            JSValue* val = (JSValue*)result.ptr;
            JSValue returned = JS_DupValue(ctx, *val);
            if (engineInstance_ &&
                engineInstance_->allocatedHandles_.find(result.ptr) != engineInstance_->allocatedHandles_.end() &&
                g_protectedHandles.find(result.ptr) == g_protectedHandles.end()) {
                engineInstance_->releaseValue(result);
            }
            return returned;
        }
        return JS_UNDEFINED;
    }

    static JSValue nativeMethodCallback(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv, int magic,
                                        JSValue* func_data) {
        int64_t ptrVal;
        if (JS_ToBigInt64(ctx, &ptrVal, func_data[0]) < 0) return JS_UNDEFINED;
        auto* method = reinterpret_cast<NativeMethod*>(static_cast<uintptr_t>(ptrVal));
        if (!method) return JS_UNDEFINED;

        std::vector<JSValueHandle> args;
        args.reserve(argc);
        for (int i = 0; i < argc; i++) {
            auto* stored = new JSValue(JS_DupValue(ctx, argv[i]));
            args.push_back({stored, ctx});
        }
        auto* receiverValue = new JSValue(JS_DupValue(ctx, this_val));
        JSValueHandle receiver{receiverValue, ctx};
        JSValueHandle result = (*method)(ctx, receiver, args);

        for (auto& arg : args) {
            if (g_protectedHandles.find(arg.ptr) != g_protectedHandles.end() ||
                arg.ptr == result.ptr) continue;
            auto* value = static_cast<JSValue*>(arg.ptr);
            JS_FreeValue(ctx, *value);
            delete value;
        }
        if (receiver.ptr != result.ptr &&
            g_protectedHandles.find(receiver.ptr) == g_protectedHandles.end()) {
            JS_FreeValue(ctx, *receiverValue);
            delete receiverValue;
        }
        if (result.ptr) {
            JSValue returned = JS_DupValue(ctx, *static_cast<JSValue*>(result.ptr));
            if (engineInstance_ &&
                engineInstance_->allocatedHandles_.find(result.ptr) != engineInstance_->allocatedHandles_.end() &&
                g_protectedHandles.find(result.ptr) == g_protectedHandles.end()) {
                engineInstance_->releaseValue(result);
            }
            return returned;
        }
        return JS_UNDEFINED;
    }

    // Console functions - helper to build message string
    static std::string buildConsoleMessage(JSContext* ctx, int argc, JSValueConst* argv) {
        std::ostringstream oss;
        for (int i = 0; i < argc; i++) {
            const char* str = JS_ToCString(ctx, argv[i]);
            if (str) {
                oss << str;
                if (i < argc - 1) oss << " ";
                JS_FreeCString(ctx, str);
            }
        }
        return oss.str();
    }

    static JSValue js_console_log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
        std::string msg = buildConsoleMessage(ctx, argc, argv);
        std::cout << "[log] " << msg << std::endl;
        if (engineInstance_ && engineInstance_->consoleCallback_) {
            engineInstance_->consoleCallback_("log", msg);
        }
#ifdef __ANDROID__
        LOGI("[log] %s", msg.c_str());
#endif
        return JS_UNDEFINED;
    }

    static JSValue js_console_warn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
        std::string msg = buildConsoleMessage(ctx, argc, argv);
        std::cout << "[warn] " << msg << std::endl;
        if (engineInstance_ && engineInstance_->consoleCallback_) {
            engineInstance_->consoleCallback_("warn", msg);
        }
#ifdef __ANDROID__
        LOGW("[warn] %s", msg.c_str());
#endif
        return JS_UNDEFINED;
    }

    static JSValue js_console_info(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
        std::string msg = buildConsoleMessage(ctx, argc, argv);
        std::cout << "[info] " << msg << std::endl;
        if (engineInstance_ && engineInstance_->consoleCallback_) {
            engineInstance_->consoleCallback_("info", msg);
        }
        return JS_UNDEFINED;
    }

    static JSValue js_console_debug(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
        std::string msg = buildConsoleMessage(ctx, argc, argv);
        std::cout << "[debug] " << msg << std::endl;
        if (engineInstance_ && engineInstance_->consoleCallback_) {
            engineInstance_->consoleCallback_("debug", msg);
        }
        return JS_UNDEFINED;
    }

    static JSValue js_console_error(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
        std::string msg = buildConsoleMessage(ctx, argc, argv);
        std::cerr << "[error] " << msg << std::endl;
        if (engineInstance_ && engineInstance_->consoleCallback_) {
            engineInstance_->consoleCallback_("error", msg);
        }
#ifdef __ANDROID__
        LOGE("[error] %s", msg.c_str());
#endif
        return JS_UNDEFINED;
    }

    static JSValue js_performance_now(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
        if (!engineInstance_) return JS_NewFloat64(ctx, 0);
        auto now = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - engineInstance_->startTime_).count();
        return JS_NewFloat64(ctx, ms);
    }

    static JSValue js_set_timeout(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
        // TODO: Proper timer implementation with event loop integration
        static int nextId = 1;
        return JS_NewInt32(ctx, nextId++);
    }

    static JSValue js_clear_timeout(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
        return JS_UNDEFINED;
    }

    JSRuntime* runtime_ = nullptr;
    JSContext* context_ = nullptr;
    std::atomic<bool> terminationRequested_{false};
    JSValue lastException_ = JS_UNDEFINED;
    ConsoleCallback consoleCallback_;
    std::chrono::high_resolution_clock::time_point startTime_;
    std::unordered_map<void*, void*> privateDataMap_;  // Map JS object ptr to native data
    std::vector<NativeFunction*> allocatedFunctions_;  // Track allocated function pointers
    std::vector<NativeMethod*> allocatedMethods_;  // Track receiver-aware callbacks
    std::unordered_set<void*> allocatedHandles_;
    std::unordered_map<void*, std::shared_ptr<QuickJSTransferredBacking>> transferredBackingOwners_;

    static thread_local QuickJSEngine* engineInstance_;  // For performance.now access
};

thread_local QuickJSEngine* QuickJSEngine::engineInstance_ = nullptr;

// Factory function
std::unique_ptr<Engine> createQuickJSEngine() {
    return std::make_unique<QuickJSEngine>();
}

}  // namespace js
}  // namespace mystral

#endif  // MYSTRAL_JS_QUICKJS
