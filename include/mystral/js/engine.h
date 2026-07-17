/**
 * JavaScript Engine Abstraction
 *
 * This header defines a common interface for JavaScript engines.
 * Implementations exist for QuickJS and V8.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <functional>
#include <vector>
#include <cstdint>

namespace mystral {
namespace js {

/**
 * JavaScript value handle
 * Opaque handle to a JS value in the engine
 */
struct JSValueHandle {
    void* ptr = nullptr;
    void* ctx = nullptr;  // Context needed for some operations
};

/**
 * Native function signature
 * Called from JavaScript with arguments, returns a value
 */
using NativeFunction = std::function<JSValueHandle(void* ctx, const std::vector<JSValueHandle>& args)>;
using ConsoleCallback = std::function<void(const std::string& type, const std::string& message)>;

/** Native method signature. Unlike NativeFunction, this receives the actual
 * JavaScript `this` value so one callback can be shared by many wrappers. */
using NativeMethod = std::function<JSValueHandle(
    void* ctx,
    JSValueHandle receiver,
    const std::vector<JSValueHandle>& args)>;

struct MemoryStats {
    uint64_t heapUsedBytes = 0;
    uint64_t heapTotalBytes = 0;
    uint64_t heapLimitBytes = 0;
    uint64_t nativeFunctions = 0;
    uint64_t frameHandles = 0;
};

struct TransferredArrayBuffer {
    void* data = nullptr;
    size_t size = 0;
    std::shared_ptr<void> owner;
};

/**
 * Engine type enumeration
 */
enum class EngineType {
    QuickJS,
    V8,
    Unknown
};

/**
 * Abstract JavaScript engine interface
 */
class Engine {
public:
    virtual ~Engine() = default;

    /**
     * Get the engine type
     */
    virtual EngineType getType() const = 0;

    /**
     * Get the engine name as a string
     */
    virtual const char* getName() const = 0;

    // ========================================================================
    // Script Evaluation
    // ========================================================================

    /**
     * Evaluate JavaScript code
     * @param code The JavaScript source code
     * @param filename Filename for error messages
     * @return true on success, false on error
     */
    virtual bool eval(const char* code, const char* filename = "<eval>") = 0;

    /**
     * Evaluate JavaScript and return the result
     * @param code The JavaScript source code
     * @param filename Filename for error messages
     * @return The result value handle
     */
    virtual JSValueHandle evalWithResult(const char* code, const char* filename = "<eval>") = 0;

    /**
     * Evaluate JavaScript as a classic script (non-module).
     * Useful for CommonJS wrappers or JSON modules.
     */
    virtual bool evalScript(const char* code, const char* filename = "<eval>") = 0;

    /**
     * Evaluate a classic script and return the result.
     */
    virtual JSValueHandle evalScriptWithResult(const char* code, const char* filename = "<eval>") = 0;

    // ========================================================================
    // Global Object Access
    // ========================================================================

    /**
     * Get the global object
     */
    virtual JSValueHandle getGlobal() = 0;

    /**
     * Set a property on the global object
     */
    virtual bool setGlobalProperty(const char* name, JSValueHandle value) = 0;

    /**
     * Get a property from the global object
     */
    virtual JSValueHandle getGlobalProperty(const char* name) = 0;

    // ========================================================================
    // Value Creation
    // ========================================================================

    virtual JSValueHandle newUndefined() = 0;
    virtual JSValueHandle newNull() = 0;
    virtual JSValueHandle newBoolean(bool value) = 0;
    virtual JSValueHandle newNumber(double value) = 0;
    virtual JSValueHandle newString(const char* value) = 0;
    virtual JSValueHandle newObject() = 0;
    virtual JSValueHandle newArray(size_t length = 0) = 0;

    /**
     * Create an ArrayBuffer from raw bytes
     * @param data Pointer to the data (will be copied)
     * @param length Size in bytes
     * @return ArrayBuffer handle
     */
    virtual JSValueHandle newArrayBuffer(const uint8_t* data, size_t length) = 0;

    /**
     * Create an ArrayBuffer backed by external memory (no copy)
     * WARNING: The memory must remain valid for the lifetime of the ArrayBuffer
     * @param data Pointer to external memory
     * @param length Size in bytes
     * @return ArrayBuffer handle that directly references the external memory
     */
    virtual JSValueHandle newArrayBufferExternal(void* data, size_t length) = 0;

    /** Detach a plain ArrayBuffer and retain ownership of its bytes. */
    virtual bool transferArrayBuffer(JSValueHandle value, TransferredArrayBuffer& result) = 0;

    /** Create a plain ArrayBuffer over transferred bytes without another copy. */
    virtual JSValueHandle newTransferredArrayBuffer(const TransferredArrayBuffer& buffer) = 0;

    /**
     * Create a SharedArrayBuffer backed by externally owned memory.
     * The owner is retained by the engine backing store until the JavaScript
     * buffer is collected. Engines without external SharedArrayBuffer support
     * return an empty handle.
     */
    virtual JSValueHandle newSharedArrayBuffer(void* data,
                                               size_t length,
                                               std::shared_ptr<void> owner) {
        (void)data;
        (void)length;
        (void)owner;
        return {};
    }

    virtual bool supportsSharedArrayBuffer() const { return false; }

    /**
     * Get the raw data pointer from an ArrayBuffer or TypedArray
     * @param value The ArrayBuffer or TypedArray handle
     * @param size Output: size in bytes (optional, can be nullptr)
     * @return Pointer to the data, or nullptr if not an ArrayBuffer/TypedArray
     */
    virtual void* getArrayBufferData(JSValueHandle value, size_t* size) = 0;

    /**
     * Create a Float32Array from raw data
     * @param data Pointer to the float data (will be copied)
     * @param count Number of floats
     * @return Float32Array handle
     */
    virtual JSValueHandle createFloat32Array(const float* data, size_t count) = 0;

    /**
     * Create a Float32Array view into external memory (no copy)
     * @param data Pointer to the float data (NOT copied - caller must ensure lifetime)
     * @param count Number of floats
     * @return Float32Array handle backed by the external memory
     */
    virtual JSValueHandle createFloat32ArrayView(float* data, size_t count) = 0;

    /**
     * Create a Uint32Array from raw data
     * @param data Pointer to the uint32 data (will be copied)
     * @param count Number of uint32s
     * @return Uint32Array handle
     */
    virtual JSValueHandle createUint32Array(const uint32_t* data, size_t count) = 0;

    /**
     * Create a Uint8Array from raw data
     * @param data Pointer to the uint8 data (will be copied)
     * @param count Number of bytes
     * @return Uint8Array handle
     */
    virtual JSValueHandle createUint8Array(const uint8_t* data, size_t count) = 0;

    /**
     * Create a function from a native callback
     */
    virtual JSValueHandle newFunction(const char* name, NativeFunction fn) = 0;

    /** Create a receiver-aware function suitable for shared prototype/method use. */
    virtual JSValueHandle newMethod(const char* name, NativeMethod fn) = 0;

    // ========================================================================
    // Value Conversion
    // ========================================================================

    virtual bool toBoolean(JSValueHandle value) = 0;
    virtual double toNumber(JSValueHandle value) = 0;
    virtual std::string toString(JSValueHandle value) = 0;

    virtual bool isUndefined(JSValueHandle value) = 0;
    virtual bool isNull(JSValueHandle value) = 0;
    virtual bool isBoolean(JSValueHandle value) = 0;
    virtual bool isNumber(JSValueHandle value) = 0;
    virtual bool isString(JSValueHandle value) = 0;
    virtual bool isObject(JSValueHandle value) = 0;
    virtual bool isArray(JSValueHandle value) = 0;
    virtual bool isFunction(JSValueHandle value) = 0;

    // ========================================================================
    // Object Operations
    // ========================================================================

    virtual bool setProperty(JSValueHandle obj, const char* name, JSValueHandle value) = 0;
    virtual JSValueHandle getProperty(JSValueHandle obj, const char* name) = 0;
    virtual bool setPropertyIndex(JSValueHandle arr, uint32_t index, JSValueHandle value) = 0;
    virtual JSValueHandle getPropertyIndex(JSValueHandle arr, uint32_t index) = 0;

    /**
     * Call a function
     * @param func The function to call
     * @param thisArg The 'this' value (can be undefined)
     * @param args Arguments to pass
     * @return The return value
     */
    virtual JSValueHandle call(JSValueHandle func, JSValueHandle thisArg, const std::vector<JSValueHandle>& args) = 0;

    // ========================================================================
    // Memory Management
    // ========================================================================

    /**
     * Protect a value from garbage collection
     * Must call unprotect() when done
     */
    virtual void protect(JSValueHandle value) = 0;

    /**
     * Allow a value to be garbage collected
     */
    virtual void unprotect(JSValueHandle value) = 0;

    /**
     * Release a temporary value returned by an engine operation.
     */
    virtual void releaseValue(JSValueHandle value) = 0;

    /**
     * Run garbage collection (if supported)
     */
    virtual void gc() = 0;

    /** Lightweight runtime diagnostics used by native performance harnesses. */
    virtual MemoryStats getMemoryStats() const { return {}; }

    /**
     * Signal the start of a new animation frame.
     * Enables per-frame allocation tracking (e.g., NativeFunction objects).
     * Must be called before executeAnimationFrameCallbacks().
     */
    virtual void beginFrame() {}

    /**
     * Clear non-protected handles created during the current frame.
     * Called at the end of each animation frame to free intermediate
     * Persistent handles and per-frame native allocations.
     * Default implementation is a no-op for engines that don't need it.
     */
    virtual void clearFrameHandles() {}

    /**
     * Request that currently executing JavaScript stop as soon as possible.
     * This may be called from a thread other than the engine's owner thread.
     * Returns false when the engine has no safe interruption mechanism.
     */
    virtual bool requestTermination() { return false; }

    /**
     * Legacy no-ops retained for source compatibility. Native callbacks now
     * follow their JavaScript Function's GC lifetime instead of frame scope.
     */
    virtual void suspendFrameTracking() {}
    virtual void resumeFrameTracking() {}

    /**
     * Observe console output while preserving the normal stdout/stderr output.
     */
    virtual void setConsoleCallback(ConsoleCallback callback) = 0;

    /**
     * Register a release callback on a JS object wrapper.
     * When the JS object is garbage collected (no more JS references),
     * the callback fires to release the associated native resource.
     * Used for Dawn/WebGPU resource cleanup (texture views, bind groups, etc.).
     */
    virtual void registerRelease(JSValueHandle obj, std::function<void()> callback) {}

    // ========================================================================
    // Error Handling
    // ========================================================================

    /**
     * Check if the last operation threw an exception
     */
    virtual bool hasException() = 0;

    /**
     * Get and clear the current exception
     */
    virtual std::string getException() = 0;

    /**
     * Throw a JavaScript exception
     */
    virtual void throwException(const char* message) = 0;

    // ========================================================================
    // Private Data
    // ========================================================================

    /**
     * Set private C++ data on a JS object
     * Used to associate native objects with JS objects
     */
    virtual void setPrivateData(JSValueHandle obj, void* data) = 0;

    /**
     * Get private C++ data from a JS object
     */
    virtual void* getPrivateData(JSValueHandle obj) = 0;

    // ========================================================================
    // Raw Context Access
    // ========================================================================

    /**
     * Get the raw engine-specific context
     * - QuickJS: JSContext*
     * - V8: v8::Isolate*
     */
    virtual void* getRawContext() = 0;
};

/**
 * Create the default engine for the platform
 * - With MYSTRAL_USE_V8: V8
 * - Fallback: QuickJS
 */
std::unique_ptr<Engine> createEngine();

/**
 * Create a specific engine type
 * Returns nullptr if that engine is not compiled in
 */
std::unique_ptr<Engine> createEngine(EngineType type);

}  // namespace js
}  // namespace mystral
