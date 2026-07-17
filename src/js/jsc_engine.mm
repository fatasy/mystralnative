/**
 * JavaScriptCore Engine Implementation
 *
 * Uses Apple's JavaScriptCore framework for JavaScript execution.
 * JSC is available for free on macOS and iOS.
 */

#include "mystral/js/engine.h"
#include <iostream>
#include <unordered_map>
#include <chrono>
#include <sstream>

#if defined(MYSTRAL_JS_JSC) && defined(__APPLE__)

#import <JavaScriptCore/JavaScriptCore.h>

namespace mystral {
namespace js {

// Store native function callbacks (since we can't capture lambdas in JSC callbacks)
static std::unordered_map<void*, NativeFunction> g_nativeFunctions;
static std::unordered_map<void*, NativeMethod> g_nativeMethods;

class JSCEngine : public Engine {
public:
    JSCEngine() {
        std::cout << "[JSC] Creating JavaScriptCore engine..." << std::endl;

        // Create a new JavaScript context group and context
        contextGroup_ = JSContextGroupCreate();
        context_ = JSGlobalContextCreateInGroup(contextGroup_, nullptr);

        if (!context_) {
            std::cerr << "[JSC] Failed to create context" << std::endl;
            return;
        }

        // Set up standard globals
        setupGlobals();

        std::cout << "[JSC] Engine created successfully" << std::endl;
    }

    ~JSCEngine() override {
        std::cout << "[JSC] Destroying engine..." << std::endl;

        g_nativeFunctions.clear();
        g_nativeMethods.clear();

        if (context_) {
            JSGlobalContextRelease(context_);
        }
        if (contextGroup_) {
            JSContextGroupRelease(contextGroup_);
        }
    }

    EngineType getType() const override { return EngineType::JavaScriptCore; }
    const char* getName() const override { return "JavaScriptCore"; }

    // ========================================================================
    // Script Evaluation
    // ========================================================================

    bool eval(const char* code, const char* filename) override {
        JSValueHandle result = evalWithResult(code, filename);
        return !hasException();
    }

    JSValueHandle evalWithResult(const char* code, const char* filename) override {
        JSStringRef scriptStr = JSStringCreateWithUTF8CString(code);
        JSStringRef sourceURL = filename ? JSStringCreateWithUTF8CString(filename) : nullptr;

        JSValueRef exception = nullptr;
        JSValueRef result = JSEvaluateScript(context_, scriptStr, nullptr, sourceURL, 0, &exception);

        JSStringRelease(scriptStr);
        if (sourceURL) JSStringRelease(sourceURL);

        if (exception) {
            lastException_ = exception;
            reportException(exception);
            return {nullptr, context_};
        }

        return {(void*)result, context_};
    }

    bool evalScript(const char* code, const char* filename) override {
        return eval(code, filename);
    }

    JSValueHandle evalScriptWithResult(const char* code, const char* filename) override {
        return evalWithResult(code, filename);
    }

    // ========================================================================
    // Global Object Access
    // ========================================================================

    JSValueHandle getGlobal() override {
        JSObjectRef global = JSContextGetGlobalObject(context_);
        return {(void*)global, context_};
    }

    bool setGlobalProperty(const char* name, JSValueHandle value) override {
        JSObjectRef global = JSContextGetGlobalObject(context_);
        JSStringRef nameStr = JSStringCreateWithUTF8CString(name);

        JSValueRef exception = nullptr;
        JSObjectSetProperty(context_, global, nameStr, (JSValueRef)value.ptr, 0, &exception);

        JSStringRelease(nameStr);
        return exception == nullptr;
    }

    JSValueHandle getGlobalProperty(const char* name) override {
        JSObjectRef global = JSContextGetGlobalObject(context_);
        JSStringRef nameStr = JSStringCreateWithUTF8CString(name);

        JSValueRef exception = nullptr;
        JSValueRef result = JSObjectGetProperty(context_, global, nameStr, &exception);

        JSStringRelease(nameStr);
        return {(void*)result, context_};
    }

    // ========================================================================
    // Value Creation
    // ========================================================================

    JSValueHandle newUndefined() override {
        return {(void*)JSValueMakeUndefined(context_), context_};
    }

    JSValueHandle newNull() override {
        return {(void*)JSValueMakeNull(context_), context_};
    }

    JSValueHandle newBoolean(bool value) override {
        return {(void*)JSValueMakeBoolean(context_, value), context_};
    }

    JSValueHandle newNumber(double value) override {
        return {(void*)JSValueMakeNumber(context_, value), context_};
    }

    JSValueHandle newString(const char* value) override {
        JSStringRef str = JSStringCreateWithUTF8CString(value);
        JSValueRef result = JSValueMakeString(context_, str);
        JSStringRelease(str);
        return {(void*)result, context_};
    }

    JSValueHandle newObject() override {
        return {(void*)JSObjectMake(context_, nullptr, nullptr), context_};
    }

    JSValueHandle newArray(size_t length) override {
        JSValueRef exception = nullptr;
        JSObjectRef arr = JSObjectMakeArray(context_, 0, nullptr, &exception);
        return {(void*)arr, context_};
    }

    JSValueHandle newArrayBuffer(const uint8_t* data, size_t length) override {
        // Allocate memory for the data copy
        void* dataCopy = nullptr;
        if (data && length > 0) {
            dataCopy = malloc(length);
            memcpy(dataCopy, data, length);
        } else {
            // Even for empty buffers, we need valid memory
            dataCopy = malloc(1);
            length = 0;
        }

        // Deallocator callback to free the memory when ArrayBuffer is GC'd
        auto deallocator = [](void* bytes, void* deallocatorContext) {
            free(bytes);
        };

        JSValueRef exception = nullptr;
        JSObjectRef arrayBuffer = JSObjectMakeArrayBufferWithBytesNoCopy(
            context_,
            dataCopy,
            length,
            deallocator,
            nullptr,  // deallocatorContext
            &exception
        );

        if (exception) {
            // Clean up if creation failed
            free(dataCopy);
            return {nullptr, context_};
        }

        return {(void*)arrayBuffer, context_};
    }

    JSValueHandle newArrayBufferExternal(void* data, size_t length) override {
        // Create an ArrayBuffer that directly references external memory (no copy)
        // Pass nullptr for deallocator since we don't own this memory (GPU manages it)
        JSValueRef exception = nullptr;
        JSObjectRef arrayBuffer = JSObjectMakeArrayBufferWithBytesNoCopy(
            context_,
            data,
            length,
            nullptr,  // No deallocator - memory is managed by WebGPU
            nullptr,  // deallocatorContext
            &exception
        );

        if (exception) {
            return {nullptr, context_};
        }

        return {(void*)arrayBuffer, context_};
    }

    void* getArrayBufferData(JSValueHandle value, size_t* size) override {
        JSValueRef val = (JSValueRef)value.ptr;
        if (!val) return nullptr;

        JSValueRef exception = nullptr;

        // Check if it's an ArrayBuffer
        if (JSValueIsObject(context_, val)) {
            JSObjectRef obj = (JSObjectRef)val;

            // Try to get ArrayBuffer data directly
            void* data = JSObjectGetArrayBufferBytesPtr(context_, obj, &exception);
            if (!exception && data) {
                size_t len = JSObjectGetArrayBufferByteLength(context_, obj, &exception);
                if (size) *size = len;
                return data;
            }
            exception = nullptr;

            // Try to get TypedArray data
            data = JSObjectGetTypedArrayBytesPtr(context_, obj, &exception);
            if (!exception && data) {
                size_t len = JSObjectGetTypedArrayByteLength(context_, obj, &exception);
                if (size) *size = len;
                return data;
            }
        }

        if (size) *size = 0;
        return nullptr;
    }

    JSValueHandle createFloat32Array(const float* data, size_t count) override {
        size_t byteLength = count * sizeof(float);

        // Copy data to a new buffer (JSC takes ownership or copies)
        void* dataCopy = malloc(byteLength);
        memcpy(dataCopy, data, byteLength);

        JSValueRef exception = nullptr;

        // Create ArrayBuffer
        JSObjectRef arrayBuffer = JSObjectMakeArrayBufferWithBytesNoCopy(
            context_, dataCopy, byteLength,
            [](void* bytes, void* ctx) { free(bytes); },
            nullptr, &exception
        );
        if (exception) {
            free(dataCopy);
            return {nullptr, context_};
        }

        // Create Float32Array from ArrayBuffer
        JSObjectRef typedArray = JSObjectMakeTypedArrayWithArrayBuffer(
            context_, kJSTypedArrayTypeFloat32Array, arrayBuffer, &exception
        );

        return {(void*)typedArray, context_};
    }

    JSValueHandle createFloat32ArrayView(float* data, size_t count) override {
        size_t byteLength = count * sizeof(float);
        JSValueRef exception = nullptr;

        // Create ArrayBuffer backed by external memory (no copy, no dealloc)
        // Caller manages the memory lifetime
        JSObjectRef arrayBuffer = JSObjectMakeArrayBufferWithBytesNoCopy(
            context_, data, byteLength,
            nullptr,  // No deallocator - caller manages memory
            nullptr, &exception
        );
        if (exception) {
            return {nullptr, context_};
        }

        // Create Float32Array from ArrayBuffer
        JSObjectRef typedArray = JSObjectMakeTypedArrayWithArrayBuffer(
            context_, kJSTypedArrayTypeFloat32Array, arrayBuffer, &exception
        );

        return {(void*)typedArray, context_};
    }

    JSValueHandle createUint32Array(const uint32_t* data, size_t count) override {
        size_t byteLength = count * sizeof(uint32_t);

        void* dataCopy = malloc(byteLength);
        memcpy(dataCopy, data, byteLength);

        JSValueRef exception = nullptr;

        JSObjectRef arrayBuffer = JSObjectMakeArrayBufferWithBytesNoCopy(
            context_, dataCopy, byteLength,
            [](void* bytes, void* ctx) { free(bytes); },
            nullptr, &exception
        );
        if (exception) {
            free(dataCopy);
            return {nullptr, context_};
        }

        JSObjectRef typedArray = JSObjectMakeTypedArrayWithArrayBuffer(
            context_, kJSTypedArrayTypeUint32Array, arrayBuffer, &exception
        );

        return {(void*)typedArray, context_};
    }

    JSValueHandle createUint8Array(const uint8_t* data, size_t count) override {
        void* dataCopy = malloc(count);
        memcpy(dataCopy, data, count);

        JSValueRef exception = nullptr;

        JSObjectRef arrayBuffer = JSObjectMakeArrayBufferWithBytesNoCopy(
            context_, dataCopy, count,
            [](void* bytes, void* ctx) { free(bytes); },
            nullptr, &exception
        );
        if (exception) {
            free(dataCopy);
            return {nullptr, context_};
        }

        JSObjectRef typedArray = JSObjectMakeTypedArrayWithArrayBuffer(
            context_, kJSTypedArrayTypeUint8Array, arrayBuffer, &exception
        );

        return {(void*)typedArray, context_};
    }

    JSValueHandle newFunction(const char* name, NativeFunction fn) override {
        JSStringRef nameStr = JSStringCreateWithUTF8CString(name);

        // We need to store the callback somewhere JSC can access it
        // Use a static map with the function object pointer as key
        JSObjectRef funcObj = JSObjectMakeFunctionWithCallback(context_, nameStr, &nativeCallback);

        // Store the native function with the object pointer as key
        g_nativeFunctions[(void*)funcObj] = fn;

        JSStringRelease(nameStr);
        return {(void*)funcObj, context_};
    }

    JSValueHandle newMethod(const char* name, NativeMethod fn) override {
        JSStringRef nameStr = JSStringCreateWithUTF8CString(name);
        JSObjectRef funcObj = JSObjectMakeFunctionWithCallback(
            context_, nameStr, &nativeMethodCallback);
        g_nativeMethods[(void*)funcObj] = std::move(fn);
        JSStringRelease(nameStr);
        return {(void*)funcObj, context_};
    }

    // ========================================================================
    // Value Conversion
    // ========================================================================

    bool toBoolean(JSValueHandle value) override {
        return JSValueToBoolean(context_, (JSValueRef)value.ptr);
    }

    double toNumber(JSValueHandle value) override {
        JSValueRef exception = nullptr;
        return JSValueToNumber(context_, (JSValueRef)value.ptr, &exception);
    }

    std::string toString(JSValueHandle value) override {
        JSValueRef exception = nullptr;
        JSStringRef str = JSValueToStringCopy(context_, (JSValueRef)value.ptr, &exception);
        if (!str) return "";

        size_t maxSize = JSStringGetMaximumUTF8CStringSize(str);
        std::string result(maxSize, '\0');
        size_t actualSize = JSStringGetUTF8CString(str, &result[0], maxSize);
        result.resize(actualSize > 0 ? actualSize - 1 : 0);  // Remove null terminator from count

        JSStringRelease(str);
        return result;
    }

    bool isUndefined(JSValueHandle value) override {
        return JSValueIsUndefined(context_, (JSValueRef)value.ptr);
    }

    bool isNull(JSValueHandle value) override {
        return JSValueIsNull(context_, (JSValueRef)value.ptr);
    }

    bool isBoolean(JSValueHandle value) override {
        return JSValueIsBoolean(context_, (JSValueRef)value.ptr);
    }

    bool isNumber(JSValueHandle value) override {
        return JSValueIsNumber(context_, (JSValueRef)value.ptr);
    }

    bool isString(JSValueHandle value) override {
        return JSValueIsString(context_, (JSValueRef)value.ptr);
    }

    bool isObject(JSValueHandle value) override {
        return JSValueIsObject(context_, (JSValueRef)value.ptr);
    }

    bool isArray(JSValueHandle value) override {
        return JSValueIsArray(context_, (JSValueRef)value.ptr);
    }

    bool isFunction(JSValueHandle value) override {
        return JSValueIsObject(context_, (JSValueRef)value.ptr) &&
               JSObjectIsFunction(context_, (JSObjectRef)value.ptr);
    }

    // ========================================================================
    // Object Operations
    // ========================================================================

    bool setProperty(JSValueHandle obj, const char* name, JSValueHandle value) override {
        JSStringRef nameStr = JSStringCreateWithUTF8CString(name);
        JSValueRef exception = nullptr;
        JSObjectSetProperty(context_, (JSObjectRef)obj.ptr, nameStr, (JSValueRef)value.ptr, 0, &exception);
        JSStringRelease(nameStr);
        return exception == nullptr;
    }

    JSValueHandle getProperty(JSValueHandle obj, const char* name) override {
        JSStringRef nameStr = JSStringCreateWithUTF8CString(name);
        JSValueRef exception = nullptr;
        JSValueRef result = JSObjectGetProperty(context_, (JSObjectRef)obj.ptr, nameStr, &exception);
        JSStringRelease(nameStr);
        return {(void*)result, context_};
    }

    bool setPropertyIndex(JSValueHandle arr, uint32_t index, JSValueHandle value) override {
        JSValueRef exception = nullptr;
        JSObjectSetPropertyAtIndex(context_, (JSObjectRef)arr.ptr, index, (JSValueRef)value.ptr, &exception);
        return exception == nullptr;
    }

    JSValueHandle getPropertyIndex(JSValueHandle arr, uint32_t index) override {
        JSValueRef exception = nullptr;
        JSValueRef result = JSObjectGetPropertyAtIndex(context_, (JSObjectRef)arr.ptr, index, &exception);
        return {(void*)result, context_};
    }

    JSValueHandle call(JSValueHandle func, JSValueHandle thisArg, const std::vector<JSValueHandle>& args) override {
        std::vector<JSValueRef> jsArgs;
        jsArgs.reserve(args.size());
        for (const auto& arg : args) {
            jsArgs.push_back((JSValueRef)arg.ptr);
        }

        JSValueRef exception = nullptr;
        JSValueRef result = JSObjectCallAsFunction(
            context_,
            (JSObjectRef)func.ptr,
            thisArg.ptr ? (JSObjectRef)thisArg.ptr : nullptr,
            jsArgs.size(),
            jsArgs.empty() ? nullptr : jsArgs.data(),
            &exception
        );

        if (exception) {
            lastException_ = exception;
        }

        return {(void*)result, context_};
    }

    // ========================================================================
    // Memory Management
    // ========================================================================

    void protect(JSValueHandle value) override {
        JSValueProtect(context_, (JSValueRef)value.ptr);
    }

    void unprotect(JSValueHandle value) override {
        JSValueUnprotect(context_, (JSValueRef)value.ptr);
    }

    void releaseValue(JSValueHandle value) override {
        (void)value;
    }

    void setConsoleCallback(ConsoleCallback callback) override {
        consoleCallback_ = std::move(callback);
    }

    void gc() override {
        JSGarbageCollect(context_);
    }

    MemoryStats getMemoryStats() const override {
        MemoryStats stats;
        stats.nativeFunctions = g_nativeFunctions.size() + g_nativeMethods.size();
        return stats;
    }

    // ========================================================================
    // Error Handling
    // ========================================================================

    bool hasException() override {
        return lastException_ != nullptr;
    }

    std::string getException() override {
        if (!lastException_) return "";

        std::string result = toString({(void*)lastException_, context_});
        lastException_ = nullptr;
        return result;
    }

    void throwException(const char* message) override {
        JSStringRef msgStr = JSStringCreateWithUTF8CString(message);
        JSValueRef msgVal = JSValueMakeString(context_, msgStr);
        JSStringRelease(msgStr);

        // Create Error object
        JSValueRef args[] = {msgVal};
        JSStringRef errorName = JSStringCreateWithUTF8CString("Error");
        JSObjectRef errorConstructor = (JSObjectRef)JSObjectGetProperty(
            context_, JSContextGetGlobalObject(context_), errorName, nullptr);
        JSStringRelease(errorName);

        JSValueRef exception = nullptr;
        JSObjectRef error = JSObjectCallAsConstructor(context_, errorConstructor, 1, args, &exception);
        lastException_ = error ? error : msgVal;
    }

    // ========================================================================
    // Private Data
    // ========================================================================

    void setPrivateData(JSValueHandle obj, void* data) override {
        // JSC doesn't have a direct "set private data" - we'd need to use a weak map
        // or create a custom class. For now, use a property with a special name.
        // A better approach would be to use JSObjectSetPrivate with a custom class.
        privateDataMap_[(JSObjectRef)obj.ptr] = data;
    }

    void* getPrivateData(JSValueHandle obj) override {
        auto it = privateDataMap_.find((JSObjectRef)obj.ptr);
        return it != privateDataMap_.end() ? it->second : nullptr;
    }

    // ========================================================================
    // Raw Context Access
    // ========================================================================

    void* getRawContext() override {
        return context_;
    }

private:
    void setupGlobals() {
        // console.log / console.warn / console.error
        JSObjectRef console = JSObjectMake(context_, nullptr, nullptr);
        setGlobalProperty("console", {(void*)console, context_});

        auto consoleFn = [this](const char* prefix) {
            return newFunction(prefix, [this, prefix](void* ctx, const std::vector<JSValueHandle>& args) {
                JSGlobalContextRef context = (JSGlobalContextRef)ctx;
                std::ostringstream message;
                for (size_t i = 0; i < args.size(); i++) {
                    JSStringRef str = JSValueToStringCopy(context, (JSValueRef)args[i].ptr, nullptr);
                    if (str) {
                        size_t maxSize = JSStringGetMaximumUTF8CStringSize(str);
                        std::string result(maxSize, '\0');
                        JSStringGetUTF8CString(str, &result[0], maxSize);
                        message << result.c_str();
                        if (i < args.size() - 1) message << " ";
                        JSStringRelease(str);
                    }
                }
                std::cout << "[" << prefix << "] " << message.str() << std::endl;
                if (consoleCallback_) {
                    consoleCallback_(prefix, message.str());
                }
                return JSValueHandle{(void*)JSValueMakeUndefined(context), ctx};
            });
        };

        setProperty({(void*)console, context_}, "log", consoleFn("log"));
        setProperty({(void*)console, context_}, "warn", consoleFn("warn"));
        setProperty({(void*)console, context_}, "error", consoleFn("error"));
        setProperty({(void*)console, context_}, "info", consoleFn("info"));
        setProperty({(void*)console, context_}, "debug", consoleFn("debug"));

        // performance.now()
        JSObjectRef performance = JSObjectMake(context_, nullptr, nullptr);
        setGlobalProperty("performance", {(void*)performance, context_});

        startTime_ = std::chrono::high_resolution_clock::now();
        setProperty({(void*)performance, context_}, "now",
            newFunction("now", [this](void* ctx, const std::vector<JSValueHandle>& args) {
                auto now = std::chrono::high_resolution_clock::now();
                double ms = std::chrono::duration<double, std::milli>(now - startTime_).count();
                return JSValueHandle{(void*)JSValueMakeNumber((JSGlobalContextRef)ctx, ms), ctx};
            })
        );

        // setTimeout / setInterval (basic implementation)
        // Note: Full implementation requires integration with the event loop
        setGlobalProperty("setTimeout",
            newFunction("setTimeout", [](void* ctx, const std::vector<JSValueHandle>& args) {
                // TODO: Implement proper timer scheduling
                static int nextId = 1;
                return JSValueHandle{(void*)JSValueMakeNumber((JSGlobalContextRef)ctx, nextId++), ctx};
            })
        );

        setGlobalProperty("clearTimeout",
            newFunction("clearTimeout", [](void* ctx, const std::vector<JSValueHandle>& args) {
                // TODO: Implement timer cancellation
                return JSValueHandle{(void*)JSValueMakeUndefined((JSGlobalContextRef)ctx), ctx};
            })
        );
    }

    void reportException(JSValueRef exception) {
        std::string msg = toString({(void*)exception, context_});
        std::cerr << "[JSC] Error: " << msg << std::endl;
    }

    static JSValueRef nativeCallback(JSContextRef ctx, JSObjectRef function,
                                     JSObjectRef thisObject, size_t argumentCount,
                                     const JSValueRef arguments[], JSValueRef* exception) {
        // Find the native function for this JS function
        auto it = g_nativeFunctions.find((void*)function);
        if (it == g_nativeFunctions.end()) {
            std::cerr << "[JSC] Native function not found for callback" << std::endl;
            return JSValueMakeUndefined(ctx);
        }

        // Convert arguments
        std::vector<JSValueHandle> args;
        args.reserve(argumentCount);
        for (size_t i = 0; i < argumentCount; i++) {
            args.push_back({(void*)arguments[i], (void*)ctx});
        }

        // Call the native function
        JSValueHandle result = it->second((void*)ctx, args);
        return (JSValueRef)result.ptr;
    }

    static JSValueRef nativeMethodCallback(JSContextRef ctx, JSObjectRef function,
                                           JSObjectRef thisObject, size_t argumentCount,
                                           const JSValueRef arguments[], JSValueRef* exception) {
        auto it = g_nativeMethods.find((void*)function);
        if (it == g_nativeMethods.end()) return JSValueMakeUndefined(ctx);
        std::vector<JSValueHandle> args;
        args.reserve(argumentCount);
        for (size_t i = 0; i < argumentCount; i++) {
            args.push_back({(void*)arguments[i], (void*)ctx});
        }
        JSValueHandle receiver{(void*)thisObject, (void*)ctx};
        JSValueHandle result = it->second((void*)ctx, receiver, args);
        return result.ptr ? (JSValueRef)result.ptr : JSValueMakeUndefined(ctx);
    }

    JSContextGroupRef contextGroup_ = nullptr;
    JSGlobalContextRef context_ = nullptr;
    JSValueRef lastException_ = nullptr;
    ConsoleCallback consoleCallback_;
    std::unordered_map<JSObjectRef, void*> privateDataMap_;
    std::chrono::high_resolution_clock::time_point startTime_;
};

// Factory function
std::unique_ptr<Engine> createJSCEngine() {
    return std::make_unique<JSCEngine>();
}

}  // namespace js
}  // namespace mystral

#endif  // MYSTRAL_JS_JSC && __APPLE__
