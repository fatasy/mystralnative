#pragma once

#include <memory>
#include <string>
#include <functional>
#include <vector>
#include <cstdint>

namespace mystral {

/**
 * Runtime configuration options
 */
struct RuntimeConfig {
    int width = 800;
    int height = 600;
    const char* title = "Mystral Game";
    bool fullscreen = false;
    bool vsync = true;
    bool resizable = true;
    bool noSdl = false;  // Run without SDL (headless GPU mode, no window)
    bool watch = false;  // Watch mode: reload script on file changes
    bool debug = false;  // Enable verbose debug logging
};

struct EvaluationResult {
    bool success = false;
    std::string valueJson;
    std::string error;
};

struct RuntimeProfileStatistics {
    double minMs = 0.0;
    double meanMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
};

struct RuntimeMemorySnapshot {
    uint64_t heapUsedBytes = 0;
    uint64_t heapTotalBytes = 0;
    uint64_t heapLimitBytes = 0;
    uint64_t nativeFunctions = 0;
    uint64_t frameHandles = 0;
};

struct RuntimeProfileReport {
    uint64_t sampledFrames = 0;
    double wallTimeMs = 0.0;
    RuntimeProfileStatistics frame;
    RuntimeProfileStatistics events;
    RuntimeProfileStatistics asyncWork;
    RuntimeProfileStatistics callbacks;
    RuntimeProfileStatistics animationFrame;
    RuntimeProfileStatistics cleanup;
    uint64_t framesOver8_33Ms = 0;
    uint64_t framesOver16_67Ms = 0;
    uint64_t framesOver33_33Ms = 0;
    RuntimeMemorySnapshot memoryStart;
    RuntimeMemorySnapshot memoryEnd;
    uint64_t workersCreated = 0;
    uint64_t workerMessagesProcessed = 0;
    uint64_t workerTimerCallbacks = 0;
    uint64_t workerMessagesRejected = 0;
    double workerBusyTimeMs = 0.0;
    uint64_t workerInputQueueEndBytes = 0;
    uint64_t workerOutputQueueEndBytes = 0;
    uint64_t workerInputQueuePeakBytes = 0;
    uint64_t workerOutputQueuePeakBytes = 0;
    uint64_t sharedMemoryStartBytes = 0;
    uint64_t sharedMemoryEndBytes = 0;
    uint64_t jobsSubmitted = 0;
    uint64_t jobsCompleted = 0;
    uint64_t jobsCancelled = 0;
    uint64_t jobsFailed = 0;
    uint64_t jobsRejected = 0;
    uint64_t jobQueueEnd = 0;
    uint64_t jobQueuePeak = 0;
    uint64_t jobInFlightEnd = 0;
    uint64_t jobInFlightPeak = 0;
    uint32_t jobWorkerCount = 0;
    RuntimeProfileStatistics jobQueueWait;
    RuntimeProfileStatistics jobExecution;
};

using ConsoleCallback = std::function<void(const std::string& type, const std::string& message)>;

/**
 * Mystral Native Runtime
 *
 * A lightweight runtime for JavaScript/TypeScript games using WebGPU.
 * Combines SDL3 for windowing/input, wgpu/Dawn for WebGPU, and V8 for JS.
 *
 * Example usage:
 *   auto runtime = mystral::Runtime::create({.width = 1280, .height = 720});
 *   runtime->loadScript("game.js");
 *   runtime->run();
 */
class Runtime {
public:
    /**
     * Create a new runtime instance
     * @param config Runtime configuration
     * @return Unique pointer to the runtime, or nullptr on failure
     */
    static std::unique_ptr<Runtime> create(const RuntimeConfig& config = {});

    virtual ~Runtime() = default;

    // ========================================================================
    // Script Loading
    // ========================================================================

    /**
     * Load and execute a JavaScript file
     * @param path Path to the JavaScript file
     * @return true on success
     */
    virtual bool loadScript(const std::string& path) = 0;

    /**
     * Evaluate JavaScript code directly
     * @param code JavaScript code to evaluate
     * @param filename Virtual filename for error messages
     * @return true on success
     */
    virtual bool evalScript(const std::string& code, const std::string& filename = "<eval>") = 0;

    /**
     * Evaluate an expression and return a JSON-safe description of its value.
     */
    virtual EvaluationResult evaluateExpression(const std::string& expression) = 0;

    /**
     * Dispatch a semantic agent command and return its JSON result.
     */
    virtual EvaluationResult dispatchAgentCommand(
        const std::string& method,
        const std::string& paramsJson) = 0;

    /**
     * Observe JavaScript console output.
     */
    virtual void setConsoleCallback(ConsoleCallback callback) = 0;

    /**
     * Reload the currently loaded script (for hot reload)
     * Clears timers and requestAnimationFrame callbacks, then re-evaluates the script.
     * @return true on success
     */
    virtual bool reloadScript() = 0;

    // ========================================================================
    // Main Loop
    // ========================================================================

    /**
     * Run the main loop (blocking)
     * Processes events and calls requestAnimationFrame callbacks until quit
     */
    virtual void run() = 0;

    /**
     * Process a single frame (non-blocking)
     * @return false if the runtime should quit
     */
    virtual bool pollEvents() = 0;

    /** Pause game callbacks while keeping the native/debug event loop alive. */
    virtual void setPaused(bool paused) = 0;

    virtual bool isPaused() const = 0;

    /** Execute a bounded number of game frames, then remain paused. */
    virtual void stepFrames(uint32_t count) = 0;

    virtual uint64_t getFrameCount() const = 0;

    /** Start collecting per-phase frame timings and a starting memory snapshot. */
    virtual void startProfiler(uint64_t expectedFrames = 0) = 0;

    /** Stop profiling and aggregate all collected samples. */
    virtual RuntimeProfileReport stopProfiler() = 0;

    /**
     * Request the runtime to quit
     */
    virtual void quit() = 0;

    /**
     * Get the exit code set by process.exit()
     * @return Exit code (0 by default, or value passed to process.exit())
     */
    virtual int getExitCode() const = 0;

    // ========================================================================
    // Window Management
    // ========================================================================

    /**
     * Resize the window
     */
    virtual void resize(int width, int height) = 0;

    /**
     * Set fullscreen mode
     */
    virtual void setFullscreen(bool fullscreen) = 0;

    /**
     * Get current window width
     */
    virtual int getWidth() const = 0;

    /**
     * Get current window height
     */
    virtual int getHeight() const = 0;

    // ========================================================================
    // Access to Internals (for advanced use)
    // ========================================================================

    /**
     * Get the underlying V8 isolate (`v8::Isolate*`).
     */
    virtual void* getJSContext() = 0;

    /**
     * Get the WebGPU device (WGPUDevice)
     */
    virtual void* getWGPUDevice() = 0;

    /**
     * Get the WebGPU queue (WGPUQueue)
     */
    virtual void* getWGPUQueue() = 0;

    /**
     * Get the WebGPU instance (WGPUInstance)
     */
    virtual void* getWGPUInstance() = 0;

    /**
     * Get the current render texture (WGPUTexture)
     * For async video capture - returns the texture being rendered to
     */
    virtual void* getCurrentTexture() = 0;

    /**
     * Get the SDL window (SDL_Window*)
     */
    virtual void* getSDLWindow() = 0;

    // ========================================================================
    // Screenshot
    // ========================================================================

    /**
     * Capture a screenshot of the current window
     * @param filename Path to save the screenshot (PNG format)
     * @return true on success
     */
    virtual bool saveScreenshot(const std::string& filename) = 0;

    /**
     * Capture the current frame as RGBA pixel data (for video recording)
     * @param outData Output vector to receive RGBA data (width * height * 4 bytes)
     * @param outWidth Output parameter for frame width
     * @param outHeight Output parameter for frame height
     * @return true on success
     */
    virtual bool captureFrame(std::vector<uint8_t>& outData, uint32_t& outWidth, uint32_t& outHeight) = 0;

protected:
    Runtime() = default;
};

// Version info - uses CMake-defined MYSTRAL_VERSION
#ifndef MYSTRAL_VERSION
#define MYSTRAL_VERSION "0.1.5"
#endif

inline const char* getVersion() {
    return MYSTRAL_VERSION;
}

// Build configuration - uses CMake-defined values
#ifndef MYSTRAL_JS_ENGINE
#define MYSTRAL_JS_ENGINE "v8"
#endif

#ifndef MYSTRAL_WEBGPU_BACKEND
#define MYSTRAL_WEBGPU_BACKEND "wgpu-native"
#endif

inline const char* getJSEngine() {
    return MYSTRAL_JS_ENGINE;
}

inline const char* getWebGPUBackend() {
    return MYSTRAL_WEBGPU_BACKEND;
}

}  // namespace mystral
