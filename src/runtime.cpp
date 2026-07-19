#include "mystral/runtime.h"
#include "mystral/platform/window.h"
#include "mystral/platform/input.h"
#include "mystral/webgpu/context.h"
#include "mystral/js/engine.h"
#include "mystral/js/module_system.h"
#include "mystral/http/http_client.h"
#include "mystral/http/async_http_client.h"
#include "mystral/webtransport/webtransport.h"
#include "mystral/fs/async_file.h"
#include "mystral/fs/file_watcher.h"
#include "mystral/gltf/gltf_loader.h"
#include "mystral/audio/audio_bindings.h"
#include "mystral/vfs/embedded_bundle.h"
#include "mystral/async/event_loop.h"
#include "mystral/async/job_system.h"
#include "mystral/workers/shared_buffer.h"
#include "mystral/workers/worker_registry.h"
#include "storage/local_storage.h"
#include "game/game_loop_controller.h"
#include "js/animation_frame.h"
#include "js/runtime_sources.h"
#include "js/runtime_timers.h"
#include "js/runtime_io_bindings.h"
#include "dom/dom_event_system.h"
#include "debug/runtime_profiler.h"

// Ray tracing bindings (conditional)
#ifdef MYSTRAL_HAS_RAYTRACING
#include "raytracing/bindings.h"
#endif
#include <map>
#include <iostream>

// Android logging and native window
#if defined(__ANDROID__)
#include <android/log.h>
#include <android/native_window.h>
#define MYSTRAL_LOG_TAG "MystralRuntime"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, MYSTRAL_LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, MYSTRAL_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, MYSTRAL_LOG_TAG, __VA_ARGS__)
#else
#define LOGD(...)
#define LOGI(...)
#define LOGE(...)
#endif
#include <fstream>
#include <filesystem>
#include <sstream>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <functional>
#include <unordered_set>
#include <queue>
#include <mutex>
#include <csignal>
#include <algorithm>
#include <cmath>
#include <numeric>

// libuv for precise timers (conditional)
#if defined(MYSTRAL_HAS_LIBUV) && !defined(__ANDROID__) && !defined(IOS)
#include <uv.h>
#define MYSTRAL_USE_LIBUV_TIMERS 1
#endif

// Draco mesh decoder (conditional)
#ifdef MYSTRAL_HAS_DRACO
// Windows min/max macros from <windows.h> conflict with std::numeric_limits in Draco headers
#ifdef _WIN32
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max
#endif
#include <draco/compression/decode.h>
#include <draco/mesh/mesh.h>
#include <draco/core/decoder_buffer.h>
#ifdef _WIN32
#pragma pop_macro("min")
#pragma pop_macro("max")
#endif
#endif
#include <cstdlib>
#include <cstring>
#include "webgpu/bindings.h"

// Platform-specific includes for crash handler
#ifdef _WIN32
#include <io.h>
#define MYSTRAL_WRITE(fd, buf, len) _write(fd, buf, len)
#define MYSTRAL_STDERR_FD 2
#else
#include <unistd.h>
// Use a do-while to properly consume the return value and avoid warn_unused_result
#define MYSTRAL_WRITE(fd, buf, len) do { ssize_t _wr = write(fd, buf, len); (void)_wr; } while(0)
#define MYSTRAL_STDERR_FD STDERR_FILENO
#endif

#include <webgpu/webgpu.h>

// SDL3 for window property access (Android ANativeWindow, Windows HWND, etc.)
#include <SDL3/SDL.h>

namespace mystral {

// Flag to suppress crash dialogs (always on by default)
static bool g_suppressCrashDialog = true;

// Signal handler to suppress crash dialog
static void crashSignalHandler(int sig) {
    if (g_suppressCrashDialog) {
        // Print message to stderr but don't show crash dialog
        const char* sigName = "UNKNOWN";
        switch(sig) {
            case SIGABRT: sigName = "SIGABRT"; break;
            case SIGSEGV: sigName = "SIGSEGV"; break;
#ifndef _WIN32
            case SIGBUS: sigName = "SIGBUS"; break;
            case SIGTRAP: sigName = "SIGTRAP"; break;
#endif
            case SIGILL: sigName = "SIGILL"; break;
        }
        // Use write() since it's async-signal-safe
        MYSTRAL_WRITE(MYSTRAL_STDERR_FD, "[Mystral] Caught signal ", 24);
        MYSTRAL_WRITE(MYSTRAL_STDERR_FD, sigName, strlen(sigName));
        MYSTRAL_WRITE(MYSTRAL_STDERR_FD, ", exiting gracefully\n", 21);
        _exit(1);
    }
    // Re-raise for crash dialog if enabled (MYSTRAL_SHOW_CRASH_DIALOG=1)
    signal(sig, SIG_DFL);
    raise(sig);
}

// Install all crash signal handlers
static void installCrashHandlers() {
    // Check if user wants to see crash dialogs
    const char* showDialog = std::getenv("MYSTRAL_SHOW_CRASH_DIALOG");
    if (showDialog && (showDialog[0] == '1' || showDialog[0] == 't' || showDialog[0] == 'T')) {
        g_suppressCrashDialog = false;
        return;
    }

    signal(SIGABRT, crashSignalHandler);
    signal(SIGSEGV, crashSignalHandler);
#ifndef _WIN32
    signal(SIGBUS, crashSignalHandler);
    signal(SIGTRAP, crashSignalHandler);
#endif
    signal(SIGILL, crashSignalHandler);
}

static std::string quoteJavaScriptString(const std::string& value) {
    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (unsigned char c : value) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20) {
                    result += "\\u00";
                    result.push_back(hex[(c >> 4) & 0x0f]);
                    result.push_back(hex[c & 0x0f]);
                } else {
                    result.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    result.push_back('"');
    return result;
}

/**
 * Runtime implementation
 */
class RuntimeImpl : public Runtime {
public:
    RuntimeImpl(const RuntimeConfig& config)
        : config_(config)
        , running_(true)  // Start as running so pollEvents() works without run()
        , width_(config.width)
        , height_(config.height)
    {}

    ~RuntimeImpl() override {
        shutdown();
    }

    bool initialize() {
        std::cout << "[Mystral] Initializing runtime..." << std::endl;
        std::cout << "[Mystral] Window: " << width_ << "x" << height_ << std::endl;

        // NOTE: Crash handlers are installed AFTER full initialization
        // because Metal/WebGPU may use signals internally during setup

        // Initialize WebGPU context
        webgpu_ = std::make_unique<webgpu::Context>();

        // No-SDL mode: headless GPU without window system
        if (config_.noSdl) {
            std::cout << "[Mystral] Running in no-SDL mode (headless GPU)" << std::endl;

            if (!webgpu_->initializeHeadless()) {
                std::cerr << "[Mystral] Failed to initialize headless WebGPU" << std::endl;
                return false;
            }

            if (!webgpu_->createOffscreenTarget(width_, height_)) {
                std::cerr << "[Mystral] Failed to create offscreen render target" << std::endl;
                return false;
            }

            // Skip to JS engine initialization (no SDL needed)
            return initializeJSAndBindings();
        }

        // Initialize SDL3 window
        if (!platform::createWindow(config_.title, width_, height_, config_.fullscreen, config_.resizable)) {
            std::cerr << "[Mystral] Failed to create window" << std::endl;
            return false;
        }

        // Get actual window size (may differ from requested, especially on mobile with 0x0 meaning fullscreen)
        platform::getWindowSize(&width_, &height_);
        std::cout << "[Mystral] Actual window size: " << width_ << "x" << height_ << std::endl;

        // Initialize WebGPU instance
        if (!webgpu_->initialize()) {
            std::cerr << "[Mystral] Failed to initialize WebGPU" << std::endl;
            return false;
        }

        // Create WebGPU surface from platform-specific handle
#if defined(__APPLE__)
        // macOS/iOS: Use Metal layer
        void* metalView = platform::getMetalView();
        if (!metalView) {
            std::cerr << "[Mystral] Failed to get Metal view" << std::endl;
            return false;
        }

        void* metalLayer = platform::getMetalLayerFromView(metalView);
        if (!metalLayer) {
            std::cerr << "[Mystral] Failed to get Metal layer" << std::endl;
            return false;
        }

        if (!webgpu_->createSurface(metalLayer, webgpu::Context::PLATFORM_METAL)) {
            std::cerr << "[Mystral] Failed to create WebGPU surface" << std::endl;
            return false;
        }
#elif defined(__ANDROID__)
        // Android: Use ANativeWindow from SDL
        // The surface can be created/destroyed during Android lifecycle, so we need to
        // wait for a valid surface and validate it before use.
        // See: https://www.dre.vanderbilt.edu/~schmidt/android/android-4.0/out/target/common/docs/doc-comment-check/reference/android/view/SurfaceHolder.html

        SDL_Window* sdlWindow = platform::getSDLWindow();
        if (!sdlWindow) {
            std::cerr << "[Mystral] Failed to get SDL window" << std::endl;
            LOGE("Failed to get SDL window");
            return false;
        }
        LOGI("Got SDL window: %p", (void*)sdlWindow);

        // Wait for the window to be shown and have a valid surface
        // Process SDL events to let Android lifecycle settle
        void* nativeWindow = nullptr;
        bool windowShown = false;
        int waitAttempts = 0;
        const int maxWaitAttempts = 100;  // 10 seconds max

        LOGI("Waiting for valid ANativeWindow...");
        while (waitAttempts < maxWaitAttempts) {
            // Process SDL events - needed for Android lifecycle
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) {
                    LOGE("Quit event during surface wait");
                    return false;
                }
                if (event.type == SDL_EVENT_WINDOW_SHOWN ||
                    event.type == SDL_EVENT_WINDOW_RESTORED ||
                    event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                    LOGI("Window event: %d (shown/restored/focused)", event.type);
                    windowShown = true;
                }
            }

            // Try to get ANativeWindow
            nativeWindow = SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow),
                SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);

            if (nativeWindow) {
                // Validate the window using ANativeWindow_getWidth
                // Invalid windows return 0 or crash
                ANativeWindow* anw = (ANativeWindow*)nativeWindow;
                int32_t width = ANativeWindow_getWidth(anw);
                int32_t height = ANativeWindow_getHeight(anw);

                if (width > 0 && height > 0) {
                    LOGI("ANativeWindow validated: %p (%dx%d)", nativeWindow, width, height);
                    break;  // Window is valid
                } else {
                    LOGI("ANativeWindow invalid dimensions: %dx%d, continuing to wait", width, height);
                    nativeWindow = nullptr;  // Reset and try again
                }
            }

            SDL_Delay(100);  // Wait 100ms before retry
            waitAttempts++;
            if (waitAttempts % 10 == 0) {
                LOGI("Still waiting for ANativeWindow... attempt %d", waitAttempts);
            }
        }

        if (!nativeWindow) {
            std::cerr << "[Mystral] Failed to get valid ANativeWindow after " << maxWaitAttempts << " attempts" << std::endl;
            LOGE("Failed to get valid ANativeWindow after %d attempts", maxWaitAttempts);
            return false;
        }
        LOGI("Got valid ANativeWindow: %p after %d attempts", nativeWindow, waitAttempts);
        std::cout << "[Mystral] Got ANativeWindow: " << nativeWindow << std::endl;

        if (!webgpu_->createSurface(nativeWindow, webgpu::Context::PLATFORM_ANDROID)) {
            std::cerr << "[Mystral] Failed to create WebGPU surface" << std::endl;
            LOGE("Failed to create WebGPU surface");
            return false;
        }
        LOGI("WebGPU surface created successfully");
#elif defined(_WIN32)
        // Windows: Use HWND from SDL
        SDL_Window* sdlWindow = platform::getSDLWindow();
        if (!sdlWindow) {
            std::cerr << "[Mystral] Failed to get SDL window" << std::endl;
            return false;
        }

        void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (!hwnd) {
            std::cerr << "[Mystral] Failed to get HWND from SDL" << std::endl;
            return false;
        }
        std::cout << "[Mystral] Got HWND: " << hwnd << std::endl;

        if (!webgpu_->createSurface(hwnd, webgpu::Context::PLATFORM_WINDOWS)) {
            std::cerr << "[Mystral] Failed to create WebGPU surface" << std::endl;
            return false;
        }
#elif defined(__linux__)
        // Linux: Use X11 from SDL (Wayland support would need additional work)
        SDL_Window* sdlWindow = platform::getSDLWindow();
        if (!sdlWindow) {
            std::cerr << "[Mystral] Failed to get SDL window" << std::endl;
            return false;
        }

        // Try X11 first
        void* xdisplay = SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow),
            SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        if (xdisplay) {
            auto xwindow = static_cast<unsigned long>(SDL_GetNumberProperty(SDL_GetWindowProperties(sdlWindow),
                SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
            if (xwindow) {
                std::cout << "[Mystral] Using X11 display: " << xdisplay << " window: " << xwindow << std::endl;
                // Pass both display and window pointer for proper X11 surface creation
                // Dawn requires the X11 display pointer together with the window.
                if (!webgpu_->createSurfaceWithDisplay(xdisplay, reinterpret_cast<void*>(xwindow), webgpu::Context::PLATFORM_XLIB)) {
                    std::cerr << "[Mystral] Failed to create WebGPU surface" << std::endl;
                    return false;
                }
            } else {
                std::cerr << "[Mystral] Failed to get X11 window" << std::endl;
                return false;
            }
        } else {
            std::cerr << "[Mystral] X11 display not available. Wayland not yet supported." << std::endl;
            return false;
        }
#else
        std::cerr << "[Mystral] WebGPU surface creation not implemented for this platform" << std::endl;
        return false;
#endif

        // Configure surface with window dimensions
        LOGI("Configuring surface: %dx%d", width_, height_);
        if (!webgpu_->configureSurface(width_, height_)) {
            std::cerr << "[Mystral] Failed to configure WebGPU surface" << std::endl;
            LOGE("Failed to configure WebGPU surface");
            return false;
        }
        LOGI("Surface configured successfully");

        return initializeJSAndBindings();
    }

    // Helper method to initialize JS engine and bindings (shared by SDL and no-SDL paths)
    bool initializeJSAndBindings(bool initializeServices = true) {
        // Initialize JavaScript engine
        LOGI("Creating JavaScript engine...");
        jsEngine_ = js::createEngine();
        if (!jsEngine_) {
            std::cerr << "[Mystral] Failed to create JavaScript engine" << std::endl;
            LOGE("Failed to create JavaScript engine");
            return false;
        }
        LOGI("JS engine created: %s", jsEngine_->getName());
        std::cout << "[Mystral] Using JS engine: " << jsEngine_->getName() << std::endl;

        sharedBuffers_ = std::make_shared<workers::SharedBufferRegistry>();
        workerRuntimeState_ = std::make_shared<workers::WorkerRuntimeState>();
        workerRuntimeState_->maxDepth = config_.maxWorkerDepth;
        workerRuntimeState_->maxWorkers = config_.maxWorkerThreads;
        const uint32_t hardwareThreads = (std::max)(1u, std::thread::hardware_concurrency());
        const uint32_t cpuThreads = config_.maxCpuThreads > 0
            ? config_.maxCpuThreads
            : (std::max)(1u, hardwareThreads - 1);
        async::configureCpuBudget(cpuThreads);
        workerRuntimeState_->maxParallelism = cpuThreads;
        workerRegistry_ = std::make_unique<workers::WorkerRegistry>(
            jsEngine_->getType(),
            std::filesystem::current_path().string(),
            sharedBuffers_,
            workerRuntimeState_);

        // Set up requestAnimationFrame
        animationFrames_.install(jsEngine_.get());

        // Set up the fixed-timestep game scheduler. Rendering remains on RAF.
        gameLoop_.install(jsEngine_.get());

        // Set up setTimeout/setInterval
        timers_.install(jsEngine_.get());

        // Set up performance API
        setupPerformance();

        // Set up Node.js-compatible process object (process.exit, etc.)
        setupProcess();

        // Set up fetch API
        ioBindings_.install(jsEngine_.get(), &jsGeneration_);

        // Set up WebTransport API (QUIC/HTTP3 via quiche; stubbed if not built)
        webtransport::initBindings(jsEngine_.get());

        // Set up URL parsing, shared memory, and native Workers.
        setupURL();

        // Set up module system (ESM/CJS resolution)
        setupModules();

        // Set up DOM event system (document, window, addEventListener, etc.)
        domEvents_.install(jsEngine_.get(), width_, height_, config_.noSdl);

        // Set up the opt-in semantic inspection/action bridge for agents.
        setupAgentBridge();

        // Set up localStorage/sessionStorage (file-backed persistence)
        setupStorage();

        // Set up native GLTF loading API
        // This provides loadGLTF() for loading .glb/.gltf files from local paths
        setupGLTF();

        // Set up native Draco mesh decoder (if compiled with MYSTRAL_HAS_DRACO)
        setupDraco();

        // Set up Web Audio API bindings (skip in no-SDL mode - audio requires SDL)
        if (!config_.noSdl) {
            audio::initializeAudioBindings(jsEngine_.get());
        }

        // Set up WebGPU bindings in JS
        // For no-SDL mode, pass nullptr for surface (offscreen rendering uses texture directly)
        WGPUSurface surface = config_.noSdl ? nullptr : webgpu_->getSurface();
        if (!webgpu::initBindings(jsEngine_.get(), webgpu_->getInstance(), webgpu_->getAdapter(), webgpu_->getDevice(), webgpu_->getQueue(), surface, webgpu_->getPreferredFormat(), width_, height_, config_.debug)) {
            std::cerr << "[Mystral] Failed to initialize WebGPU bindings" << std::endl;
            return false;
        }

        // In no-SDL mode, set the offscreen texture for headless rendering
        if (config_.noSdl) {
            webgpu::setOffscreenTexture(
                webgpu_->getOffscreenTexture(),
                webgpu_->getOffscreenTextureView()
            );
        }

        // Set up ray tracing bindings (if compiled with MYSTRAL_HAS_RAYTRACING)
        setupRayTracing();

        // Install crash handlers AFTER full initialization
        // (Metal/WebGPU use signals during setup that we shouldn't intercept)
        installCrashHandlers();

        if (initializeServices) {
            // Reserve the main thread for rendering and run asset work on the
            // runtime-owned priority pool.
            async::getJobSystem().start();
            std::cout << "[JobSystem] Initialized with "
                      << async::getJobSystem().stats().workerCount
                      << " worker threads" << std::endl;

            // Initialize libuv event loop for async I/O (HTTP, file, timers)
            async::EventLoop::instance().init();

            // Initialize async HTTP client (uses libuv for non-blocking I/O)
            http::getAsyncHttpClient().init();

            // Initialize asset file reads on the native job system.
            fs::getAsyncFileReader().init();

            // Initialize file watcher (uses libuv fs_event for hot reload)
            fs::getFileWatcher().init();

            // Initialize WebTransport subsystem (sockets are created lazily)
            webtransport::init();
        }

        std::cout << "[Mystral] Runtime initialized" << std::endl;
        return true;
    }

    void shutdown() {
        std::cout << "[Mystral] Shutting down runtime..." << std::endl;
        running_ = false;

        if (workerRegistry_) {
            workerRegistry_->shutdown();
            workerRegistry_.reset();
        }

        // Clean up audio resources FIRST before touching JS objects
        // (Audio callback thread may be accessing JS handles)
        audio::cleanupAudioBindings();

        // Cancel and drain WebGPU/image async work while its JavaScript engine
        // and the native device are both still alive.
        webgpu::resetSessionBindings();

        fs::getAsyncFileReader().shutdown();
        async::getJobSystem().cancelGeneration(jsGeneration_);
        async::getJobSystem().shutdown();
        ioBindings_.clearFileCallbacks();

        // Clean up ray tracing resources
#ifdef MYSTRAL_HAS_RAYTRACING
        rt::cleanupRTBindings();
#endif

        // Shutdown async HTTP client (cancels pending requests)
        http::getAsyncHttpClient().shutdown();

        // Shutdown file watcher
        fs::getFileWatcher().shutdown();

        // Shut down WebTransport sessions (closes QUIC connections + uv handles)
        webtransport::shutdown();

        timers_.clear();

        // Shutdown libuv event loop (waits for pending handles to close)
        async::EventLoop::instance().shutdown();
        timers_.finishShutdown();

        animationFrames_.reset();
        gameLoop_.reset();
        domEvents_.reset();

        if (moduleSystem_) {
            moduleSystem_->clearCaches();
            js::setModuleSystem(nullptr);
            moduleSystem_.reset();
        }

        // Run garbage collection before destroying the engine
        // This helps clean up any lingering Promise objects, etc.
        if (jsEngine_) {
            jsEngine_->gc();
            jsEngine_->gc();  // Run twice for good measure
        }

        jsEngine_.reset();    // Release JS engine
        if (sharedBuffers_) {
            sharedBuffers_->clear();
            sharedBuffers_.reset();
        }
        webgpu_.reset();      // Release WebGPU resources
        if (!config_.noSdl) {
            platform::destroyWindow();
        }
    }

    // ========================================================================
    // Script Loading
    // ========================================================================

    bool loadScript(const std::string& path) override {
        std::cout << "[Mystral] Loading script: " << path << std::endl;

        if (!moduleSystem_) {
            std::cerr << "[Mystral] Module system not initialized" << std::endl;
            return false;
        }

        // Store script path for reloading
        scriptPath_ = path;
        updateDocumentScriptLocation(path);

        // Watch the containing directory so atomic bundle replacement does
        // not detach the watch from the old inode/file handle.
        if (config_.watch && fs::getFileWatcher().isReady()) {
            if (watchId_ >= 0) {
                fs::getFileWatcher().unwatch(watchId_);
            }
            std::error_code pathError;
            auto absoluteScript = std::filesystem::absolute(path, pathError).lexically_normal();
            if (pathError) absoluteScript = std::filesystem::path(path).lexically_normal();
            watchedScriptName_ = absoluteScript.filename().string();
            auto watchDirectory = absoluteScript.parent_path();
            if (watchDirectory.empty()) watchDirectory = ".";
            watchId_ = fs::getFileWatcher().watch(watchDirectory.string(), [this](const std::string& changedPath, fs::FileChangeType type) {
                if (std::filesystem::path(changedPath).filename().string() != watchedScriptName_) return;
                if (type == fs::FileChangeType::Modified || type == fs::FileChangeType::Renamed) {
                    std::cout << "[HotReload] File changed: " << changedPath << std::endl;
                    reloadRequested_ = true;
                    reloadDeadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                    reloadCandidateObserved_ = false;
                }
            });
            if (watchId_ >= 0) {
                std::cout << "[HotReload] Watching for changes: " << absoluteScript.string() << std::endl;
            }
        }

        return moduleSystem_->loadEntry(path);
    }

    bool evalScript(const std::string& code, const std::string& filename) override {
        std::cout << "[Mystral] Evaluating script: " << filename
                  << " (" << code.length() << " bytes)" << std::endl;

        if (!jsEngine_) {
            std::cerr << "[Mystral] No JavaScript engine available" << std::endl;
            return false;
        }

        return jsEngine_->evalScript(code.c_str(), filename.c_str());
    }

    EvaluationResult evaluateExpression(const std::string& expression) override {
        if (!jsEngine_) {
            return {false, "", "JavaScript engine is not available"};
        }

        if (jsEngine_->hasException()) {
            jsEngine_->getException();
        }

        const std::string source =
            "(() => { try {"
            "const value = (0, eval)(" + quoteJavaScriptString(expression) + ");"
            "const seen = new WeakSet();"
            "return JSON.stringify({ok:true,type:typeof value,value:value,description:String(value)},"
            "(_key,item) => {"
            "if (typeof item === 'bigint' || typeof item === 'symbol') return String(item);"
            "if (typeof item === 'function') return '[Function ' + (item.name || 'anonymous') + ']';"
            "if (typeof item === 'number' && !Number.isFinite(item)) return String(item);"
            "if (item && typeof item === 'object') { if (seen.has(item)) return '[Circular]'; seen.add(item); }"
            "return item;"
            "});"
            "} catch (error) {"
            "return JSON.stringify({ok:false,error:{message:String(error && error.message || error),"
            "stack:String(error && error.stack || '')}});"
            "} })()";

        js::JSValueHandle value = jsEngine_->evalScriptWithResult(source.c_str(), "<debug-evaluate>");
        if (!value.ptr || jsEngine_->hasException()) {
            std::string error = jsEngine_->hasException()
                ? jsEngine_->getException()
                : "Expression evaluation failed";
            return {false, "", error};
        }

        std::string valueJson = jsEngine_->toString(value);
        jsEngine_->releaseValue(value);
        if (valueJson.empty()) {
            return {false, "", "Expression result could not be serialized"};
        }
        return {true, std::move(valueJson), ""};
    }

    EvaluationResult dispatchAgentCommand(
        const std::string& method,
        const std::string& paramsJson) override {
        if (!jsEngine_) {
            return {false, "", "JavaScript engine is not available"};
        }

        if (jsEngine_->hasException()) {
            jsEngine_->getException();
        }

        const std::string source =
            "(() => { try {"
            "const params = JSON.parse(" + quoteJavaScriptString(paramsJson.empty() ? "{}" : paramsJson) + ");"
            "const value = globalThis.mystralAgent.__dispatch("
                + quoteJavaScriptString(method) + ", params);"
            "const seen = new WeakSet();"
            "const json = JSON.stringify(value, (_key,item) => {"
            "if (typeof item === 'bigint' || typeof item === 'symbol') return String(item);"
            "if (typeof item === 'function') return '[Function ' + (item.name || 'anonymous') + ']';"
            "if (typeof item === 'number' && !Number.isFinite(item)) return String(item);"
            "if (item && typeof item === 'object') { if (seen.has(item)) return '[Circular]'; seen.add(item); }"
            "return item;"
            "});"
            "return {ok:true,json};"
            "} catch (error) {"
            "return {ok:false,error:String(error && error.message || error)};"
            "} })()";

        js::JSValueHandle result = jsEngine_->evalScriptWithResult(source.c_str(), "<agent-dispatch>");
        if (!result.ptr || jsEngine_->hasException()) {
            std::string error = jsEngine_->hasException()
                ? jsEngine_->getException()
                : "Agent command failed";
            return {false, "", error};
        }

        js::JSValueHandle okValue = jsEngine_->getProperty(result, "ok");
        bool ok = jsEngine_->toBoolean(okValue);
        jsEngine_->releaseValue(okValue);

        js::JSValueHandle detailValue = jsEngine_->getProperty(result, ok ? "json" : "error");
        std::string detail = jsEngine_->toString(detailValue);
        jsEngine_->releaseValue(detailValue);
        jsEngine_->releaseValue(result);

        if (!ok) {
            return {false, "", detail.empty() ? "Agent command failed" : std::move(detail)};
        }
        if (detail.empty()) {
            return {false, "", "Agent command result could not be serialized"};
        }
        return {true, std::move(detail), ""};
    }

    void setConsoleCallback(ConsoleCallback callback) override {
        if (jsEngine_) {
            jsEngine_->setConsoleCallback(std::move(callback));
        }
    }

    bool reloadScript() override {
        if (scriptPath_.empty()) {
            std::cerr << "[HotReload] No script loaded to reload" << std::endl;
            return false;
        }

        std::cout << "[HotReload] Reloading script: " << scriptPath_ << std::endl;
        logHotReloadStats("before dispose");

        // Finish cancellation on the main thread before the old V8 isolate is
        // destroyed, so protected callbacks never outlive their generation.
        async::getJobSystem().cancelGeneration(jsGeneration_);
        async::getJobSystem().waitIdle();
        async::getJobSystem().processCompletions();
        ioBindings_.clearFileCallbacks();

        // Give the application one synchronous chance to release its scene,
        // renderer and JS-owned GPU wrappers before native callbacks vanish.
        auto disposeHook = jsEngine_->getGlobalProperty("__laasHotDispose");
        if (jsEngine_->isFunction(disposeHook)) {
            auto result = jsEngine_->call(disposeHook, jsEngine_->newUndefined(), {});
            if (!result.ptr) {
                std::cerr << "[HotReload] Application dispose hook failed" << std::endl;
            }
        }
        jsEngine_->setGlobalProperty("__laasHotDispose", jsEngine_->newUndefined());

        // Clear all pending timers
        timers_.clear();

        animationFrames_.reset();
        gameLoop_.reset();

        domEvents_.reset();

        // Clear module caches so script is re-read from disk
        if (moduleSystem_) {
            moduleSystem_->clearCaches();
        }

        webgpu::releaseReloadResources();

        // Native binding calls made by the dispose hook leave temporary V8
        // handles in the current frame set. Drop those roots before forcing
        // GC, otherwise the old Three texture/material graph survives until
        // after the new bundle has already allocated its replacement.
        jsEngine_->clearFrameHandles();
        jsEngine_->gc();
        jsEngine_->gc();
        logHotReloadStats("after GC");

        if (!recreateJavaScriptEngine()) {
            return false;
        }

        // Reload the script
        updateDocumentScriptLocation(scriptPath_);
        bool success = moduleSystem_->loadEntry(scriptPath_);

        if (success) {
            std::cout << "[HotReload] Script reloaded successfully" << std::endl;
        } else {
            std::cerr << "[HotReload] Failed to reload script" << std::endl;
        }

        return success;
    }

private:
    void updateDocumentScriptLocation(const std::string& path) {
        if (!jsEngine_ || path.empty()) return;

        std::string scriptUrl;
        if (path.find("://") != std::string::npos) {
            scriptUrl = path;
        } else {
            std::error_code pathError;
            auto absolutePath = std::filesystem::absolute(path, pathError).lexically_normal();
            if (pathError) return;
            const auto genericPath = absolutePath.generic_string();
#ifdef _WIN32
            scriptUrl = "file:///" + genericPath;
#else
            scriptUrl = "file://" + genericPath;
#endif
        }

        auto document = jsEngine_->getGlobalProperty("document");
        if (jsEngine_->isUndefined(document) || jsEngine_->isNull(document)) return;
        jsEngine_->setProperty(document, "baseURI", jsEngine_->newString(scriptUrl.c_str()));

        auto currentScript = jsEngine_->newObject();
        jsEngine_->setProperty(currentScript, "tagName", jsEngine_->newString("SCRIPT"));
        jsEngine_->setProperty(currentScript, "src", jsEngine_->newString(scriptUrl.c_str()));
        jsEngine_->setProperty(document, "currentScript", currentScript);
    }

    void logHotReloadStats(const char* phase) {
        const auto memory = jsEngine_->getMemoryStats();
        std::cout << "[HotReload] " << phase
                  << ": heap=" << (memory.heapUsedBytes / (1024 * 1024)) << " MB"
                  << ", nativeFunctions=" << memory.nativeFunctions;
        auto statsFunction = jsEngine_->getGlobalProperty("__mystralWebGpuStats");
        if (jsEngine_->isFunction(statsFunction)) {
            auto stats = jsEngine_->call(statsFunction, jsEngine_->newUndefined(), {});
            if (stats.ptr) {
                const auto number = [this, stats](const char* name) -> uint64_t {
                    return static_cast<uint64_t>(jsEngine_->toNumber(jsEngine_->getProperty(stats, name)));
                };
                std::cout << ", buffers=" << number("activeBuffers")
                          << ", textures=" << number("activeTextures")
                          << ", renderPipelines=" << number("activeRenderPipelines")
                          << ", computePipelines=" << number("activeComputePipelines")
                          << ", offscreenCanvases=" << number("activeOffscreenCanvases")
                          << ", canvas2DContexts=" << number("activeCanvas2DContexts")
                          << ", encoders=" << number("activeCommandEncoders")
                          << ", passes=" << (number("activeRenderPasses") + number("activeComputePasses"));
            }
        }
        std::cout << std::endl;
    }

    bool reloadFileIsStable(std::chrono::steady_clock::time_point now) {
        std::error_code error;
        const auto size = std::filesystem::file_size(scriptPath_, error);
        if (error || size == 0) {
            reloadCandidateObserved_ = false;
            return false;
        }
        const auto writeTime = std::filesystem::last_write_time(scriptPath_, error);
        if (error) {
            reloadCandidateObserved_ = false;
            return false;
        }
        if (!reloadCandidateObserved_ || size != reloadCandidateSize_ || writeTime != reloadCandidateWriteTime_) {
            reloadCandidateObserved_ = true;
            reloadCandidateSize_ = size;
            reloadCandidateWriteTime_ = writeTime;
            reloadCandidateSince_ = now;
            return false;
        }
        return now - reloadCandidateSince_ >= std::chrono::milliseconds(300);
    }

    bool recreateJavaScriptEngine() {
        // Bindings with native-side JS handles must detach before the engine
        // destroys its context. The Dawn context/device and SDL window remain.
        audio::cleanupAudioBindings();
#ifdef MYSTRAL_HAS_RAYTRACING
        rt::cleanupRTBindings();
#endif
        webtransport::resetBindings();

        if (workerRegistry_) {
            workerRegistry_->shutdown();
            workerRegistry_.reset();
        }

        if (moduleSystem_) {
            moduleSystem_->clearCaches();
            js::setModuleSystem(nullptr);
            moduleSystem_.reset();
        }

        ioBindings_.clearFileCallbacks();
#ifdef MYSTRAL_HAS_DRACO
        {
            std::lock_guard<std::mutex> lock(dracoMutex_);
            while (!pendingDracoCallbacks_.empty()) pendingDracoCallbacks_.pop();
        }
#endif

        animationFrames_.reset();
        gameLoop_.reset();
        domEvents_.reset();
        webgpu::resetSessionBindings();
        ++jsGeneration_;
        jsEngine_.reset();
        if (sharedBuffers_) {
            sharedBuffers_->clear();
            sharedBuffers_.reset();
        }

        if (!initializeJSAndBindings(false)) {
            std::cerr << "[HotReload] Failed to recreate JavaScript engine" << std::endl;
            return false;
        }
        logHotReloadStats("new engine");
        return true;
    }

public:

    // ========================================================================
    // Main Loop
    // ========================================================================

    void run() override {
        // Check if script already called process.exit() during loading
        if (!running_) {
            std::cout << "[Mystral] Skipping main loop (process.exit already called)" << std::endl;
            return;
        }

        std::cout << "[Mystral] Starting main loop..." << std::endl;

        // Mock event removed - was causing rotation without mouse button press
        // sendMockPointerEvent()

        // In no-SDL mode, track consecutive idle frames to detect when script is done
        int idleFrames = 0;
        const int maxIdleFrames = 3;  // Exit after 3 frames with no work

        while (running_) {
            // pollEvents() handles:
            // - SDL event polling
            // - Timer callbacks (setTimeout/setInterval)
            // - Microtask queue (promises)
            // - requestAnimationFrame callbacks (renders frame)
            if (!pollEvents()) {
                break;
            }

            // In no-SDL (headless) mode, exit when there's no more work to do
            if (config_.noSdl) {
                bool hasWork = animationFrames_.hasCallbacks() || timers_.hasActive() ||
                               gameLoop_.isRunning() ||
                               async::getJobSystem().stats().inFlight > 0 ||
                               (workerRegistry_ && workerRegistry_->size() > 0) ||
                               webtransport::hasActiveSessions();
                if (!hasWork) {
                    idleFrames++;
                    if (idleFrames >= maxIdleFrames) {
                        std::cout << "[Mystral] No-SDL mode: No more work, exiting cleanly" << std::endl;
                        running_ = false;
                        break;
                    }
                } else {
                    idleFrames = 0;
                }
            }
        }

        std::cout << "[Mystral] Main loop ended" << std::endl;
    }

    // Check if there are any active (non-cancelled) timers
    void renderFrame() {
        // Rendering is now driven by JavaScript through requestAnimationFrame
        // The JS code calls context.getCurrentTexture(), creates render passes,
        // and submits command buffers which also presents the surface.
        // So we don't need to do anything here - just let JS drive.
    }

    bool pollEvents() override {
        using ProfileClock = std::chrono::steady_clock;
        // Debug commands run during this function, so profiling may start or
        // stop halfway through a frame. Sample only a complete profiling frame,
        // and cap fixed-size profiles even if the client sends profile.stop late.
        const bool profilingFrame = profiler_.shouldSampleFrame();
        ProfileClock::time_point profileFrameStart;
        ProfileClock::time_point profilePhaseStart;
        debug::RuntimeProfiler::FrameSample profileSample;
        if (profilingFrame) {
            profileFrameStart = ProfileClock::now();
            profilePhaseStart = profileFrameStart;
        }

        // Poll SDL events through our platform layer (skip in no-SDL mode)
        if (!config_.noSdl) {
            if (!platform::pollEvents()) {
                running_ = false;
                return false;
            }
        }

        // Poll libuv event loop - process any ready I/O callbacks (non-blocking)
        // This handles async HTTP requests, file I/O, and libuv-based timers
        async::EventLoop::instance().runOnce();

        if (profilingFrame && profiler_.active()) {
            auto now = ProfileClock::now();
            profileSample.eventsMs = debug::RuntimeProfiler::millisecondsBetween(profilePhaseStart, now);
            profilePhaseStart = now;
        }

        if (paused_ && stepFramesRemaining_ == 0) {
            // Release native job results even while JavaScript callbacks are paused.
            async::getJobSystem().processCompletions();
            webgpu::processAsyncCompletions();
            gameLoop_.rebaseClock();
            return running_;
        }

        // Process completed async HTTP requests (invoke their JS callbacks)
        // This must be called after runOnce() to invoke callbacks safely on the main thread
        http::getAsyncHttpClient().processCompletedRequests();

        // Drive WebTransport QUIC sessions and dispatch their JS events (main thread)
        webtransport::processEvents();

        // Drain native job completions on the main thread. File completions
        // enqueue their JavaScript callbacks for the callback phase below.
        async::getJobSystem().processCompletions();

        // Process file watch events (for hot reload)
        fs::getFileWatcher().processPendingEvents();

        // Check if hot reload was requested
        const auto reloadNow = std::chrono::steady_clock::now();
        if (reloadRequested_ && reloadNow >= reloadDeadline_ && reloadFileIsStable(reloadNow)) {
            reloadRequested_ = false;
            reloadCandidateObserved_ = false;
            reloadScript();
        }

        if (profilingFrame && profiler_.active()) {
            auto now = ProfileClock::now();
            profileSample.asyncWorkMs = debug::RuntimeProfiler::millisecondsBetween(profilePhaseStart, now);
            profilePhaseStart = now;
        }

        // Execute timer callbacks (setTimeout, setInterval)
        timers_.executeCallbacks();

        // Process any queued file callbacks that were deferred from previous frames
        // We process them here (after other callbacks) to ensure we're not in a nested callback stack
        ioBindings_.processFileCallbacks();

        // Process completed async Draco decode results
        processPendingDracoCallbacks();

        processWorkerMessages();

        // Dawn callbacks may run on internal threads. Their JavaScript Promise
        // settlement is marshalled here so V8 is only touched by this thread.
        webgpu::processAsyncCompletions();

        // Process microtask queue for promises
        processMicrotasks();

        if (profilingFrame && profiler_.active()) {
            auto now = ProfileClock::now();
            profileSample.callbacksMs = debug::RuntimeProfiler::millisecondsBetween(profilePhaseStart, now);
            profilePhaseStart = now;
        }

        // Begin frame — enables per-frame temporary-handle tracking. Native
        // function callbacks have GC ownership and may safely outlive it.
        jsEngine_->beginFrame();
        webgpu::beginDawnFrame();

        // Advance the authoritative fixed-timestep simulation before rendering.
        gameLoop_.advance();

        if (profilingFrame && profiler_.active()) {
            auto now = ProfileClock::now();
            profileSample.simulationMs = debug::RuntimeProfiler::millisecondsBetween(profilePhaseStart, now);
            profilePhaseStart = now;
        }

        // Execute requestAnimationFrame callbacks (renders a frame)
        animationFrames_.execute();

        if (profilingFrame && profiler_.active()) {
            auto now = ProfileClock::now();
            profileSample.animationFrameMs = debug::RuntimeProfiler::millisecondsBetween(profilePhaseStart, now);
            profilePhaseStart = now;
        }

        // Free non-protected temporary handles and per-frame Dawn resources.
        jsEngine_->clearFrameHandles();
        webgpu::endDawnFrame();

        if (profilingFrame && profiler_.active()) {
            auto now = ProfileClock::now();
            profileSample.cleanupMs = debug::RuntimeProfiler::millisecondsBetween(profilePhaseStart, now);
            profileSample.frameMs = debug::RuntimeProfiler::millisecondsBetween(profileFrameStart, now);
            profiler_.addFrameSample(profileSample);
        }

        frameCount_++;
        if (paused_ && stepFramesRemaining_ > 0) {
            stepFramesRemaining_--;
        }

        // TODO: Translate to Web events via InputShim
        // TODO: Dispatch to JS

        return running_;
    }

    void setPaused(bool paused) override {
        paused_ = paused;
        if (!paused_) {
            stepFramesRemaining_ = 0;
        }
    }

    bool isPaused() const override {
        return paused_;
    }

    void stepFrames(uint32_t count) override {
        paused_ = true;
        stepFramesRemaining_ = count;
    }

    uint64_t getFrameCount() const override {
        return frameCount_;
    }

    void startProfiler(uint64_t expectedFrames) override {
        auto snapshot = captureProfilerSnapshot();
        gameLoop_.resetHighWaterMarks();
        snapshot.gameLoop = gameLoop_.stats();
        async::getJobSystem().resetHighWaterMarks();
        profiler_.start(expectedFrames, snapshot);
    }

    RuntimeProfileReport stopProfiler() override {
        return profiler_.stop(captureProfilerSnapshot());
    }

    void quit() override {
        std::cout << "[Mystral] Quit requested" << std::endl;
        running_ = false;
    }

    int getExitCode() const override {
        return exitCode_;
    }

    // ========================================================================
    // Window Management
    // ========================================================================

    void resize(int width, int height) override {
        std::cout << "[Mystral] Resize: " << width << "x" << height << std::endl;
        width_ = width;
        height_ = height;
        domEvents_.setSize(width, height);

        if (webgpu_) {
            webgpu_->resizeSurface(width, height);
        }

        platform::setWindowSize(width, height);
        // TODO: Dispatch resize event to JS
    }

    void setFullscreen(bool fullscreen) override {
        std::cout << "[Mystral] Fullscreen: " << (fullscreen ? "true" : "false") << std::endl;
        platform::setFullscreen(fullscreen);
    }

    int getWidth() const override { return width_; }
    int getHeight() const override { return height_; }

    // ========================================================================
    // Internals Access
    // ========================================================================

    void* getJSContext() override {
        return jsEngine_ ? jsEngine_->getRawContext() : nullptr;
    }

    void* getWGPUDevice() override {
        return webgpu_ ? webgpu_->getDevice() : nullptr;
    }

    void* getWGPUQueue() override {
        return webgpu_ ? webgpu_->getQueue() : nullptr;
    }

    void* getWGPUInstance() override {
        return webgpu_ ? webgpu_->getInstance() : nullptr;
    }

    void* getCurrentTexture() override {
        // Return the current surface texture for async capture
        // This is set during getCurrentTextureView() in bindings
        return webgpu::getCurrentSurfaceTexture();
    }

    void* getSDLWindow() override {
        if (config_.noSdl) {
            return nullptr;
        }
        return platform::getSDLWindow();
    }

    // ========================================================================
    // Screenshot
    // ========================================================================

    bool saveScreenshot(const std::string& filename) override {
        if (!webgpu_) {
            std::cerr << "[Mystral] Screenshot failed: WebGPU not initialized" << std::endl;
            return false;
        }
        return webgpu_->saveScreenshot(filename.c_str());
    }

    bool captureFrame(std::vector<uint8_t>& outData, uint32_t& outWidth, uint32_t& outHeight) override {
        if (!webgpu_) {
            return false;
        }
        return webgpu_->captureFrame(outData, outWidth, outHeight);
    }

private:
    RuntimeMemorySnapshot getMemorySnapshot() const {
        RuntimeMemorySnapshot snapshot;
        if (!jsEngine_) {
            return snapshot;
        }

        const auto memory = jsEngine_->getMemoryStats();
        snapshot.heapUsedBytes = memory.heapUsedBytes;
        snapshot.heapTotalBytes = memory.heapTotalBytes;
        snapshot.heapLimitBytes = memory.heapLimitBytes;
        snapshot.nativeFunctions = memory.nativeFunctions;
        snapshot.frameHandles = memory.frameHandles;
        return snapshot;
    }

    debug::RuntimeProfilerSnapshot captureProfilerSnapshot() const {
        debug::RuntimeProfilerSnapshot snapshot;
        snapshot.memory = getMemorySnapshot();
        snapshot.workers = workerRegistry_
            ? workerRegistry_->stats()
            : workers::WorkerRegistryStats{};
        snapshot.sharedMemoryBytes = sharedBuffers_ ? sharedBuffers_->allocatedBytes() : 0;
        snapshot.jobs = async::getJobSystem().stats();
        snapshot.gameLoop = gameLoop_.stats();
        return snapshot;
    }

    void setupAgentBridge() {
        if (!jsEngine_) return;

        const char* source = js::runtime_sources::agentBridge();

        if (!jsEngine_->evalScript(source, "<agent-bridge>")) {
            std::cerr << "[Mystral] Failed to initialize agent bridge" << std::endl;
        }
    }

    void setupPerformance() {
        if (!jsEngine_) return;

        // Create performance object with now() method
        auto performance = jsEngine_->newObject();

        jsEngine_->setProperty(performance, "now",
            jsEngine_->newFunction("now", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                // Return time in milliseconds since epoch (or some stable reference)
                auto now = std::chrono::high_resolution_clock::now();
                double timestamp = std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
                return jsEngine_->newNumber(timestamp);
            })
        );

        jsEngine_->setGlobalProperty("performance", performance);

        jsEngine_->setGlobalProperty("__mystralRuntimeStats",
            jsEngine_->newFunction("__mystralRuntimeStats", [this](void*, const std::vector<js::JSValueHandle>&) {
                const auto memory = jsEngine_->getMemoryStats();
                auto result = jsEngine_->newObject();
                jsEngine_->setProperty(result, "frame", jsEngine_->newNumber(static_cast<double>(frameCount_)));
                jsEngine_->setProperty(result, "paused", jsEngine_->newBoolean(paused_));
                const auto gameState = gameLoop_.state();
                const auto& gameStats = gameLoop_.stats();
                jsEngine_->setProperty(result, "gameLoopRunning", jsEngine_->newBoolean(gameState.running));
                jsEngine_->setProperty(result, "gameLoopPaused", jsEngine_->newBoolean(gameState.paused));
                jsEngine_->setProperty(result, "gameTickCount", jsEngine_->newNumber(static_cast<double>(gameState.tickCount)));
                jsEngine_->setProperty(result, "gameTicksDropped", jsEngine_->newNumber(static_cast<double>(gameStats.ticksDropped)));
                jsEngine_->setProperty(result, "heapUsedBytes", jsEngine_->newNumber(static_cast<double>(memory.heapUsedBytes)));
                jsEngine_->setProperty(result, "heapTotalBytes", jsEngine_->newNumber(static_cast<double>(memory.heapTotalBytes)));
                jsEngine_->setProperty(result, "heapLimitBytes", jsEngine_->newNumber(static_cast<double>(memory.heapLimitBytes)));
                jsEngine_->setProperty(result, "nativeFunctions", jsEngine_->newNumber(static_cast<double>(memory.nativeFunctions)));
                jsEngine_->setProperty(result, "frameHandles", jsEngine_->newNumber(static_cast<double>(memory.frameHandles)));
                const auto workerStats = workerRegistry_
                    ? workerRegistry_->stats()
                    : workers::WorkerRegistryStats{};
                jsEngine_->setProperty(result, "workersActive", jsEngine_->newNumber(static_cast<double>(workerStats.activeWorkers)));
                jsEngine_->setProperty(result, "workersCreated", jsEngine_->newNumber(static_cast<double>(workerStats.createdWorkers)));
                jsEngine_->setProperty(result, "nestedWorkersCreated", jsEngine_->newNumber(static_cast<double>(workerStats.nestedCreatedWorkers)));
                jsEngine_->setProperty(result, "workerMaxDepth", jsEngine_->newNumber(static_cast<double>(workerStats.maxDepth)));
                jsEngine_->setProperty(result, "sharedMemoryBytes", jsEngine_->newNumber(static_cast<double>(sharedBuffers_ ? sharedBuffers_->allocatedBytes() : 0)));
                const auto cpuBudget = async::cpuBudgetStats();
                jsEngine_->setProperty(result, "cpuBudgetThreads", jsEngine_->newNumber(static_cast<double>(cpuBudget.limit)));
                jsEngine_->setProperty(result, "cpuBudgetActive", jsEngine_->newNumber(static_cast<double>(cpuBudget.active)));
                jsEngine_->setProperty(result, "cpuBudgetPeakActive", jsEngine_->newNumber(static_cast<double>(cpuBudget.peakActive)));
                return result;
            })
        );
    }

    void setupProcess() {
        if (!jsEngine_) return;

        // Create Node.js-compatible process object
        auto process = jsEngine_->newObject();

        // process.exit(code) - cleanly exit the application
        jsEngine_->setProperty(process, "exit",
            jsEngine_->newFunction("exit", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                int exitCode = 0;
                if (!args.empty()) {
                    exitCode = static_cast<int>(jsEngine_->toNumber(args[0]));
                }
                std::cout << "[Mystral] process.exit(" << exitCode << ") called" << std::endl;
                exitCode_ = exitCode;
                running_ = false;
                return jsEngine_->newUndefined();
            })
        );

        // process.platform - useful for platform-specific code
#if defined(__APPLE__)
        jsEngine_->setProperty(process, "platform", jsEngine_->newString("darwin"));
#elif defined(_WIN32)
        jsEngine_->setProperty(process, "platform", jsEngine_->newString("win32"));
#elif defined(__linux__)
        jsEngine_->setProperty(process, "platform", jsEngine_->newString("linux"));
#elif defined(__ANDROID__)
        jsEngine_->setProperty(process, "platform", jsEngine_->newString("android"));
#else
        jsEngine_->setProperty(process, "platform", jsEngine_->newString("unknown"));
#endif

        // process.argv - command line arguments (placeholder for now)
        auto argv = jsEngine_->newArray();
        jsEngine_->setProperty(process, "argv", argv);

        // process.env - environment variables (empty object for now, could populate later)
        auto env = jsEngine_->newObject();
        jsEngine_->setProperty(process, "env", env);

        // process.memoryUsage() - native harness diagnostics. Field names are
        // explicit rather than pretending every engine exposes Node's full set.
        jsEngine_->setProperty(process, "memoryUsage",
            jsEngine_->newFunction("memoryUsage", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                const auto stats = jsEngine_->getMemoryStats();
                auto result = jsEngine_->newObject();
                jsEngine_->setProperty(result, "heapUsedBytes", jsEngine_->newNumber((double)stats.heapUsedBytes));
                jsEngine_->setProperty(result, "heapTotalBytes", jsEngine_->newNumber((double)stats.heapTotalBytes));
                jsEngine_->setProperty(result, "heapLimitBytes", jsEngine_->newNumber((double)stats.heapLimitBytes));
                jsEngine_->setProperty(result, "nativeFunctions", jsEngine_->newNumber((double)stats.nativeFunctions));
                jsEngine_->setProperty(result, "frameHandles", jsEngine_->newNumber((double)stats.frameHandles));
                return result;
            })
        );

        jsEngine_->setGlobalProperty("process", process);
    }

    void setupStorage() {
        if (!jsEngine_) return;

        // Initialize localStorage backed by a JSON file
        // Storage file is keyed by the current working directory name
        std::string storageDir = storage::LocalStorage::getStorageDirectory();
        std::string cwdStem = std::filesystem::current_path().filename().string();
        std::string filename = storage::LocalStorage::deriveStorageFilename(cwdStem);
        std::string storagePath = storageDir + "/" + filename;

        localStorage_.init(storagePath);
        std::cout << "[Mystral] localStorage initialized: " << storagePath << std::endl;

        // Register native C++ functions that the JS polyfill will call

        // __storageGetItem(key) -> string | null
        jsEngine_->setGlobalProperty("__storageGetItem",
            jsEngine_->newFunction("__storageGetItem", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) return jsEngine_->newNull();
                std::string key = jsEngine_->toString(args[0]);
                if (!localStorage_.has(key)) {
                    return jsEngine_->newNull();
                }
                return jsEngine_->newString(localStorage_.getItem(key).c_str());
            })
        );

        // __storageSetItem(key, value)
        jsEngine_->setGlobalProperty("__storageSetItem",
            jsEngine_->newFunction("__storageSetItem", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 2) return jsEngine_->newUndefined();
                std::string key = jsEngine_->toString(args[0]);
                std::string value = jsEngine_->toString(args[1]);
                localStorage_.setItem(key, value);
                return jsEngine_->newUndefined();
            })
        );

        // __storageRemoveItem(key)
        jsEngine_->setGlobalProperty("__storageRemoveItem",
            jsEngine_->newFunction("__storageRemoveItem", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) return jsEngine_->newUndefined();
                std::string key = jsEngine_->toString(args[0]);
                localStorage_.removeItem(key);
                return jsEngine_->newUndefined();
            })
        );

        // __storageClear()
        jsEngine_->setGlobalProperty("__storageClear",
            jsEngine_->newFunction("__storageClear", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                localStorage_.clear();
                return jsEngine_->newUndefined();
            })
        );

        // __storageKey(index) -> string | null
        jsEngine_->setGlobalProperty("__storageKey",
            jsEngine_->newFunction("__storageKey", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) return jsEngine_->newNull();
                int index = static_cast<int>(jsEngine_->toNumber(args[0]));
                if (index < 0 || index >= localStorage_.length()) {
                    return jsEngine_->newNull();
                }
                return jsEngine_->newString(localStorage_.key(index).c_str());
            })
        );

        // __storageLength() -> number
        jsEngine_->setGlobalProperty("__storageLength",
            jsEngine_->newFunction("__storageLength", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                return jsEngine_->newNumber(static_cast<double>(localStorage_.length()));
            })
        );

        // JavaScript polyfill that creates localStorage and sessionStorage globals
        const char* storagePolyfill = js::runtime_sources::storagePolyfill();

        jsEngine_->eval(storagePolyfill, "storage-polyfill.js");
    }


    void setupURL() {
        if (!jsEngine_) return;

        workers::installSharedBufferBindings(jsEngine_.get(), sharedBuffers_);
        if (!jsEngine_->evalScript(workers::sharedApiSource(), "mystral-shared.js")) {
            std::cerr << "[Mystral] Failed to initialize shared memory API" << std::endl;
        }

        if (!workers::installWorkerRegistryBindings(jsEngine_.get(), workerRegistry_.get())) {
            std::cerr << "[Mystral] Failed to initialize native Worker bindings" << std::endl;
        }

        // URL and URLSearchParams polyfills plus the native Worker facade.
        const char* urlPolyfill = js::runtime_sources::urlPolyfill();

        jsEngine_->eval(urlPolyfill, "url-worker-polyfill.js");
        std::cout << "[Mystral] URL and native Worker APIs initialized" << std::endl;
    }

    void setupModules() {
        if (!jsEngine_) return;

        std::string rootDir = std::filesystem::current_path().string();
        moduleSystem_ = std::make_unique<js::ModuleSystem>(jsEngine_.get(), rootDir);
        js::setModuleSystem(moduleSystem_.get());

        jsEngine_->setGlobalProperty("__mystralRequire",
            jsEngine_->newFunction("__mystralRequire", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                (void)ctx;
                if (args.empty()) {
                    return jsEngine_->newUndefined();
                }
                std::string spec = jsEngine_->toString(args[0]);
                std::string referrer;
                if (args.size() > 1 && !jsEngine_->isUndefined(args[1])) {
                    referrer = jsEngine_->toString(args[1]);
                }
                return moduleSystem_->require(spec, referrer);
            })
        );
    }

    void setupGLTF() {
        if (!jsEngine_) return;

        // Native GLTF loading function
        jsEngine_->setGlobalProperty("__loadGLTF",
            jsEngine_->newFunction("__loadGLTF", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.empty()) {
                    return jsEngine_->newNull();
                }

                std::unique_ptr<gltf::GLTFData> gltfData;

                // Check if first arg is a string (file path) or ArrayBuffer
                LOGI("__loadGLTF called with %zu args", args.size());
                std::cout << "[GLTF] __loadGLTF called with " << args.size() << " args" << std::endl;
                if (jsEngine_->isString(args[0])) {
                    std::string path = jsEngine_->toString(args[0]);
                    LOGI("Loading from file: %s", path.c_str());
                    std::cout << "[GLTF] Loading from file: " << path << std::endl;
                    gltfData = gltf::loadGLTF(path);
                } else {
                    // ArrayBuffer
                    LOGI("Getting ArrayBuffer data...");
                    std::cout << "[GLTF] Getting ArrayBuffer data..." << std::endl;
                    size_t size = 0;
                    void* data = jsEngine_->getArrayBufferData(args[0], &size);
                    LOGI("getArrayBufferData returned: data=%s, size=%zu", (data ? "valid" : "null"), size);
                    std::cout << "[GLTF] getArrayBufferData returned: data=" << (data ? "valid" : "null") << ", size=" << size << std::endl;
                    if (data && size > 0) {
                        std::string basePath = "";
                        if (args.size() > 1 && jsEngine_->isString(args[1])) {
                            basePath = jsEngine_->toString(args[1]);
                        }
                        LOGI("Loading from memory: %zu bytes, basePath=%s", size, basePath.c_str());
                        std::cout << "[GLTF] Loading from memory: " << size << " bytes, basePath=" << basePath << std::endl;
                        try {
                            gltfData = gltf::loadGLTFFromMemory(static_cast<const uint8_t*>(data), size, basePath);
                            LOGI("loadGLTFFromMemory returned: %s", (gltfData ? "valid" : "null"));
                            std::cout << "[GLTF] loadGLTFFromMemory returned: " << (gltfData ? "valid" : "null") << std::endl;
                        } catch (const std::exception& e) {
                            LOGE("loadGLTFFromMemory exception: %s", e.what());
                            std::cerr << "[GLTF] Exception: " << e.what() << std::endl;
                        } catch (...) {
                            LOGE("loadGLTFFromMemory unknown exception");
                            std::cerr << "[GLTF] Unknown exception" << std::endl;
                        }
                    } else {
                        LOGE("Invalid ArrayBuffer data");
                        std::cout << "[GLTF] ERROR: Invalid ArrayBuffer data" << std::endl;
                    }
                }

                if (!gltfData) {
                    return jsEngine_->newNull();
                }

                // Convert GLTF data to JavaScript object
                auto result = jsEngine_->newObject();

                // --- Meshes ---
                auto meshesArray = jsEngine_->newArray(gltfData->meshes.size());
                for (size_t mi = 0; mi < gltfData->meshes.size(); mi++) {
                    const auto& mesh = gltfData->meshes[mi];
                    auto meshObj = jsEngine_->newObject();
                    jsEngine_->setProperty(meshObj, "name", jsEngine_->newString(mesh.name.c_str()));

                    // Primitives
                    auto primsArray = jsEngine_->newArray(mesh.primitives.size());
                    for (size_t pi = 0; pi < mesh.primitives.size(); pi++) {
                        const auto& prim = mesh.primitives[pi];
                        auto primObj = jsEngine_->newObject();

                        // Positions
                        if (!prim.positions.data.empty()) {
                            auto posBuffer = jsEngine_->newArrayBuffer(
                                reinterpret_cast<const uint8_t*>(prim.positions.data.data()),
                                prim.positions.data.size() * sizeof(float));
                            jsEngine_->setProperty(primObj, "positions", posBuffer);
                            jsEngine_->setProperty(primObj, "vertexCount", jsEngine_->newNumber(prim.positions.count));
                        }

                        // Normals
                        if (!prim.normals.data.empty()) {
                            auto normBuffer = jsEngine_->newArrayBuffer(
                                reinterpret_cast<const uint8_t*>(prim.normals.data.data()),
                                prim.normals.data.size() * sizeof(float));
                            jsEngine_->setProperty(primObj, "normals", normBuffer);
                        }

                        // Texcoords
                        if (!prim.texcoords.data.empty()) {
                            auto uvBuffer = jsEngine_->newArrayBuffer(
                                reinterpret_cast<const uint8_t*>(prim.texcoords.data.data()),
                                prim.texcoords.data.size() * sizeof(float));
                            jsEngine_->setProperty(primObj, "texcoords", uvBuffer);
                        }

                        // Tangents
                        if (!prim.tangents.data.empty()) {
                            auto tanBuffer = jsEngine_->newArrayBuffer(
                                reinterpret_cast<const uint8_t*>(prim.tangents.data.data()),
                                prim.tangents.data.size() * sizeof(float));
                            jsEngine_->setProperty(primObj, "tangents", tanBuffer);
                        }

                        // Indices
                        if (!prim.indices.empty()) {
                            auto idxBuffer = jsEngine_->newArrayBuffer(
                                reinterpret_cast<const uint8_t*>(prim.indices.data()),
                                prim.indices.size() * sizeof(uint32_t));
                            jsEngine_->setProperty(primObj, "indices", idxBuffer);
                            jsEngine_->setProperty(primObj, "indexCount", jsEngine_->newNumber(prim.indices.size()));
                        }

                        // Material index
                        jsEngine_->setProperty(primObj, "materialIndex", jsEngine_->newNumber(prim.materialIndex));

                        jsEngine_->setPropertyIndex(primsArray, pi, primObj);
                    }
                    jsEngine_->setProperty(meshObj, "primitives", primsArray);
                    jsEngine_->setPropertyIndex(meshesArray, mi, meshObj);
                }
                jsEngine_->setProperty(result, "meshes", meshesArray);

                // --- Materials ---
                auto materialsArray = jsEngine_->newArray(gltfData->materials.size());
                for (size_t mi = 0; mi < gltfData->materials.size(); mi++) {
                    const auto& mat = gltfData->materials[mi];
                    auto matObj = jsEngine_->newObject();
                    jsEngine_->setProperty(matObj, "name", jsEngine_->newString(mat.name.c_str()));

                    // Base color factor
                    auto baseColor = jsEngine_->newArray(4);
                    for (int i = 0; i < 4; i++) {
                        jsEngine_->setPropertyIndex(baseColor, i, jsEngine_->newNumber(mat.baseColorFactor[i]));
                    }
                    jsEngine_->setProperty(matObj, "baseColorFactor", baseColor);

                    jsEngine_->setProperty(matObj, "metallicFactor", jsEngine_->newNumber(mat.metallicFactor));
                    jsEngine_->setProperty(matObj, "roughnessFactor", jsEngine_->newNumber(mat.roughnessFactor));
                    jsEngine_->setProperty(matObj, "baseColorTextureIndex", jsEngine_->newNumber(mat.baseColorTexture.imageIndex));
                    jsEngine_->setProperty(matObj, "normalTextureIndex", jsEngine_->newNumber(mat.normalTexture.imageIndex));
                    jsEngine_->setProperty(matObj, "metallicRoughnessTextureIndex", jsEngine_->newNumber(mat.metallicRoughnessTexture.imageIndex));

                    // Emissive
                    auto emissive = jsEngine_->newArray(3);
                    for (int i = 0; i < 3; i++) {
                        jsEngine_->setPropertyIndex(emissive, i, jsEngine_->newNumber(mat.emissiveFactor[i]));
                    }
                    jsEngine_->setProperty(matObj, "emissiveFactor", emissive);
                    jsEngine_->setProperty(matObj, "emissiveTextureIndex", jsEngine_->newNumber(mat.emissiveTexture.imageIndex));

                    // Alpha
                    const char* alphaMode = "OPAQUE";
                    if (mat.alphaMode == gltf::MaterialData::AlphaMode::Mask) alphaMode = "MASK";
                    else if (mat.alphaMode == gltf::MaterialData::AlphaMode::Blend) alphaMode = "BLEND";
                    jsEngine_->setProperty(matObj, "alphaMode", jsEngine_->newString(alphaMode));
                    jsEngine_->setProperty(matObj, "alphaCutoff", jsEngine_->newNumber(mat.alphaCutoff));
                    jsEngine_->setProperty(matObj, "doubleSided", jsEngine_->newBoolean(mat.doubleSided));

                    jsEngine_->setPropertyIndex(materialsArray, mi, matObj);
                }
                jsEngine_->setProperty(result, "materials", materialsArray);

                // --- Images ---
                auto imagesArray = jsEngine_->newArray(gltfData->images.size());
                for (size_t ii = 0; ii < gltfData->images.size(); ii++) {
                    const auto& img = gltfData->images[ii];
                    auto imgObj = jsEngine_->newObject();
                    jsEngine_->setProperty(imgObj, "name", jsEngine_->newString(img.name.c_str()));
                    jsEngine_->setProperty(imgObj, "uri", jsEngine_->newString(img.uri.c_str()));
                    jsEngine_->setProperty(imgObj, "mimeType", jsEngine_->newString(img.mimeType.c_str()));

                    // Embedded image data
                    if (!img.data.empty()) {
                        auto imgData = jsEngine_->newArrayBuffer(img.data.data(), img.data.size());
                        jsEngine_->setProperty(imgObj, "data", imgData);
                    }

                    jsEngine_->setPropertyIndex(imagesArray, ii, imgObj);
                }
                jsEngine_->setProperty(result, "images", imagesArray);

                // --- Nodes ---
                auto nodesArray = jsEngine_->newArray(gltfData->nodes.size());
                for (size_t ni = 0; ni < gltfData->nodes.size(); ni++) {
                    const auto& node = gltfData->nodes[ni];
                    auto nodeObj = jsEngine_->newObject();
                    jsEngine_->setProperty(nodeObj, "name", jsEngine_->newString(node.name.c_str()));
                    jsEngine_->setProperty(nodeObj, "meshIndex", jsEngine_->newNumber(node.meshIndex));

                    // Transform
                    if (node.hasMatrix) {
                        auto matrix = jsEngine_->newArray(16);
                        for (int i = 0; i < 16; i++) {
                            jsEngine_->setPropertyIndex(matrix, i, jsEngine_->newNumber(node.matrix[i]));
                        }
                        jsEngine_->setProperty(nodeObj, "matrix", matrix);
                    } else {
                        auto translation = jsEngine_->newArray(3);
                        auto rotation = jsEngine_->newArray(4);
                        auto scale = jsEngine_->newArray(3);
                        for (int i = 0; i < 3; i++) {
                            jsEngine_->setPropertyIndex(translation, i, jsEngine_->newNumber(node.translation[i]));
                            jsEngine_->setPropertyIndex(scale, i, jsEngine_->newNumber(node.scale[i]));
                        }
                        for (int i = 0; i < 4; i++) {
                            jsEngine_->setPropertyIndex(rotation, i, jsEngine_->newNumber(node.rotation[i]));
                        }
                        jsEngine_->setProperty(nodeObj, "translation", translation);
                        jsEngine_->setProperty(nodeObj, "rotation", rotation);
                        jsEngine_->setProperty(nodeObj, "scale", scale);
                    }

                    // Children
                    auto children = jsEngine_->newArray(node.children.size());
                    for (size_t ci = 0; ci < node.children.size(); ci++) {
                        jsEngine_->setPropertyIndex(children, ci, jsEngine_->newNumber(node.children[ci]));
                    }
                    jsEngine_->setProperty(nodeObj, "children", children);

                    jsEngine_->setPropertyIndex(nodesArray, ni, nodeObj);
                }
                jsEngine_->setProperty(result, "nodes", nodesArray);

                // --- Scenes ---
                auto scenesArray = jsEngine_->newArray(gltfData->scenes.size());
                for (size_t si = 0; si < gltfData->scenes.size(); si++) {
                    const auto& scene = gltfData->scenes[si];
                    auto sceneObj = jsEngine_->newObject();
                    jsEngine_->setProperty(sceneObj, "name", jsEngine_->newString(scene.name.c_str()));

                    auto sceneNodes = jsEngine_->newArray(scene.nodes.size());
                    for (size_t ni = 0; ni < scene.nodes.size(); ni++) {
                        jsEngine_->setPropertyIndex(sceneNodes, ni, jsEngine_->newNumber(scene.nodes[ni]));
                    }
                    jsEngine_->setProperty(sceneObj, "nodes", sceneNodes);

                    jsEngine_->setPropertyIndex(scenesArray, si, sceneObj);
                }
                jsEngine_->setProperty(result, "scenes", scenesArray);
                jsEngine_->setProperty(result, "defaultScene", jsEngine_->newNumber(gltfData->defaultScene));

                return result;
            })
        );

        // JavaScript wrapper for loadGLTF
        const char* gltfPolyfill = R"(
// GLTF Loader wrapper - always fetches file first for cross-platform compatibility
async function loadGLTF(urlOrPath) {
    console.log('loadGLTF: ' + urlOrPath);

    // Fetch the file (works for http://, https://, file://, and relative paths)
    // On Android, relative paths are read from assets via SDL
    const response = await fetch(urlOrPath);
    if (!response.ok) {
        throw new Error('Failed to fetch GLTF: ' + response.status + ' for ' + urlOrPath);
    }
    const buffer = await response.arrayBuffer();
    console.log('loadGLTF: fetched ' + buffer.byteLength + ' bytes');

    // Extract base path for external resources
    const lastSlash = urlOrPath.lastIndexOf('/');
    const basePath = lastSlash >= 0 ? urlOrPath.substring(0, lastSlash + 1) : '';

    return __loadGLTF(buffer, basePath);
}

globalThis.loadGLTF = loadGLTF;
)";

        jsEngine_->eval(gltfPolyfill, "gltf-polyfill.js");
        std::cout << "[Mystral] GLTF loader initialized" << std::endl;
    }

    void setupDraco() {
#ifdef MYSTRAL_HAS_DRACO
        if (!jsEngine_) return;

        // Callback-based native Draco decoder: __mystralNativeDecodeDraco(buffer, attrs, callback)
        // Runs decoding on a libuv thread pool thread, calls callback(result, error) on main thread.
        jsEngine_->setGlobalProperty("__mystralNativeDecodeDraco",
            jsEngine_->newFunction("__mystralNativeDecodeDraco", [this](void* ctx, const std::vector<js::JSValueHandle>& args) {
                if (args.size() < 3) {
                    std::cerr << "[Draco] __mystralNativeDecodeDraco requires 3 arguments (buffer, attributeMap, callback)" << std::endl;
                    return jsEngine_->newUndefined();
                }

                // Get compressed data from ArrayBuffer — must copy since JS buffer may be GC'd
                size_t compressedSize = 0;
                void* compressedData = jsEngine_->getArrayBufferData(args[0], &compressedSize);
                if (!compressedData || compressedSize == 0) {
                    std::cerr << "[Draco] Invalid compressed data buffer" << std::endl;
                    return jsEngine_->newUndefined();
                }

                // Get attribute IDs from the map object
                auto attrMap = args[1];
                auto posIdVal = jsEngine_->getProperty(attrMap, "POSITION");
                auto normIdVal = jsEngine_->getProperty(attrMap, "NORMAL");
                auto uvIdVal = jsEngine_->getProperty(attrMap, "TEXCOORD_0");

                int posAttrId = jsEngine_->isUndefined(posIdVal) ? -1 : static_cast<int>(jsEngine_->toNumber(posIdVal));
                int normAttrId = jsEngine_->isUndefined(normIdVal) ? -1 : static_cast<int>(jsEngine_->toNumber(normIdVal));
                int uvAttrId = jsEngine_->isUndefined(uvIdVal) ? -1 : static_cast<int>(jsEngine_->toNumber(uvIdVal));

                // Protect the callback from GC
                auto callback = args[2];
                jsEngine_->protect(callback);

                // Create decode context with a copy of the compressed data
                auto* decCtx = new DracoDecodeContext();
                decCtx->work.data = decCtx;
                decCtx->compressedData.assign(
                    static_cast<const uint8_t*>(compressedData),
                    static_cast<const uint8_t*>(compressedData) + compressedSize);
                decCtx->posAttrId = posAttrId;
                decCtx->normAttrId = normAttrId;
                decCtx->uvAttrId = uvAttrId;
                decCtx->callback = callback;
                decCtx->runtime = this;
                decCtx->generation = jsGeneration_;

                // Queue decoding on libuv thread pool
                uv_queue_work(
                    async::EventLoop::instance().handle(),
                    &decCtx->work,
                    // Worker function — runs on thread pool
                    [](uv_work_t* req) {
                        auto* dc = static_cast<DracoDecodeContext*>(req->data);

                        draco::DecoderBuffer decoderBuffer;
                        decoderBuffer.Init(reinterpret_cast<const char*>(dc->compressedData.data()),
                                           dc->compressedData.size());

                        draco::Decoder decoder;
                        auto typeResult = draco::Decoder::GetEncodedGeometryType(&decoderBuffer);
                        if (!typeResult.ok()) {
                            dc->error = "Failed to get geometry type: " + typeResult.status().error_msg_string();
                            return;
                        }

                        if (typeResult.value() != draco::TRIANGULAR_MESH) {
                            dc->error = "Unsupported geometry type (expected triangular mesh)";
                            return;
                        }

                        auto meshResult = decoder.DecodeMeshFromBuffer(&decoderBuffer);
                        if (!meshResult.ok()) {
                            dc->error = "Decode failed: " + meshResult.status().error_msg_string();
                            return;
                        }

                        auto mesh = std::move(meshResult).value();
                        uint32_t numPoints = mesh->num_points();
                        uint32_t numFaces = mesh->num_faces();

                        // Extract positions (vec3)
                        if (dc->posAttrId >= 0) {
                            const draco::PointAttribute* attr = mesh->GetAttributeByUniqueId(dc->posAttrId);
                            if (attr) {
                                dc->positions.resize(numPoints * 3);
                                for (draco::PointIndex pi(0); pi < numPoints; ++pi) {
                                    attr->GetMappedValue(pi, &dc->positions[pi.value() * 3]);
                                }
                            }
                        }

                        // Extract normals (vec3)
                        if (dc->normAttrId >= 0) {
                            const draco::PointAttribute* attr = mesh->GetAttributeByUniqueId(dc->normAttrId);
                            if (attr) {
                                dc->normals.resize(numPoints * 3);
                                for (draco::PointIndex pi(0); pi < numPoints; ++pi) {
                                    attr->GetMappedValue(pi, &dc->normals[pi.value() * 3]);
                                }
                            }
                        }

                        // Extract UVs (vec2)
                        if (dc->uvAttrId >= 0) {
                            const draco::PointAttribute* attr = mesh->GetAttributeByUniqueId(dc->uvAttrId);
                            if (attr) {
                                dc->uvs.resize(numPoints * 2);
                                for (draco::PointIndex pi(0); pi < numPoints; ++pi) {
                                    attr->GetMappedValue(pi, &dc->uvs[pi.value() * 2]);
                                }
                            }
                        }

                        // Extract indices
                        dc->indices.resize(numFaces * 3);
                        for (draco::FaceIndex fi(0); fi < numFaces; ++fi) {
                            const auto& face = mesh->face(fi);
                            dc->indices[fi.value() * 3 + 0] = face[0].value();
                            dc->indices[fi.value() * 3 + 1] = face[1].value();
                            dc->indices[fi.value() * 3 + 2] = face[2].value();
                        }

                        dc->numPoints = numPoints;
                        dc->numFaces = numFaces;
                    },
                    // After-work callback — runs on main thread (libuv loop iteration)
                    [](uv_work_t* req, int status) {
                        auto* dc = static_cast<DracoDecodeContext*>(req->data);
                        if (dc->generation != dc->runtime->jsGeneration_) {
                            delete dc;
                            return;
                        }
                        // Queue the result for processing on the JS main thread
                        std::lock_guard<std::mutex> lock(dc->runtime->dracoMutex_);
                        dc->runtime->pendingDracoCallbacks_.push(std::unique_ptr<DracoDecodeContext>(dc));
                    }
                );

                return jsEngine_->newUndefined();
            })
        );

        // Promise-based wrapper: __mystralNativeDecodeDracoAsync(buffer, attrs) → Promise<result>
        const char* dracoPolyfill = R"(
globalThis.__mystralNativeDecodeDracoAsync = function(buffer, attrs) {
    return new Promise(function(resolve, reject) {
        __mystralNativeDecodeDraco(buffer, attrs, function(result, error) {
            if (error) {
                reject(new Error(error));
            } else {
                resolve(result);
            }
        });
    });
};
)";
        jsEngine_->eval(dracoPolyfill, "draco-polyfill.js");

        std::cout << "[Mystral] Native Draco decoder initialized (async, libuv thread pool)" << std::endl;
#endif
    }

    void setupRayTracing() {
#ifdef MYSTRAL_HAS_RAYTRACING
        if (!jsEngine_) return;

        if (!rt::initializeRTBindings(jsEngine_.get())) {
            std::cerr << "[Mystral] Failed to initialize ray tracing bindings" << std::endl;
        }
#endif
    }

    void processPendingDracoCallbacks() {
#ifdef MYSTRAL_HAS_DRACO
        std::queue<std::unique_ptr<DracoDecodeContext>> toProcess;
        {
            std::lock_guard<std::mutex> lock(dracoMutex_);
            std::swap(toProcess, pendingDracoCallbacks_);
        }

        while (!toProcess.empty()) {
            auto dc = std::move(toProcess.front());
            toProcess.pop();
            if (dc->generation != jsGeneration_) continue;

            if (!dc->error.empty()) {
                // Error — call callback(null, errorString)
                auto nullVal = jsEngine_->newNull();
                auto errorVal = jsEngine_->newString(dc->error.c_str());
                std::vector<js::JSValueHandle> callbackArgs = { nullVal, errorVal };
                jsEngine_->call(dc->callback, jsEngine_->newUndefined(), callbackArgs);
                std::cerr << "[Draco] " << dc->error << std::endl;
            } else {
                // Success — build JS result object with ArrayBuffers
                auto result = jsEngine_->newObject();

                if (!dc->positions.empty()) {
                    jsEngine_->setProperty(result, "positions",
                        jsEngine_->newArrayBuffer(
                            reinterpret_cast<const uint8_t*>(dc->positions.data()),
                            dc->positions.size() * sizeof(float)));
                }
                if (!dc->normals.empty()) {
                    jsEngine_->setProperty(result, "normals",
                        jsEngine_->newArrayBuffer(
                            reinterpret_cast<const uint8_t*>(dc->normals.data()),
                            dc->normals.size() * sizeof(float)));
                }
                if (!dc->uvs.empty()) {
                    jsEngine_->setProperty(result, "uvs",
                        jsEngine_->newArrayBuffer(
                            reinterpret_cast<const uint8_t*>(dc->uvs.data()),
                            dc->uvs.size() * sizeof(float)));
                }
                if (!dc->indices.empty()) {
                    jsEngine_->setProperty(result, "indices",
                        jsEngine_->newArrayBuffer(
                            reinterpret_cast<const uint8_t*>(dc->indices.data()),
                            dc->indices.size() * sizeof(uint32_t)));
                }

                std::cout << "[Draco] Decoded mesh: " << dc->numPoints << " points, " << dc->numFaces << " faces" << std::endl;

                auto nullVal = jsEngine_->newNull();
                std::vector<js::JSValueHandle> callbackArgs = { result, nullVal };
                jsEngine_->call(dc->callback, jsEngine_->newUndefined(), callbackArgs);
            }

            jsEngine_->unprotect(dc->callback);
        }
#endif
    }


    void processWorkerMessages() {
        if (!workerRegistry_ || !jsEngine_) return;
        std::string error;
        if (!workers::dispatchWorkerRegistryMessages(
                jsEngine_.get(), workerRegistry_.get(), &error)) {
            std::cerr << "[Worker] Main-thread message handler failed: " << error << std::endl;
        }
    }

    void processMicrotasks() {
        // V8 performs microtask checkpoints after engine calls.
    }

    RuntimeConfig config_;
    bool running_;
    int exitCode_ = 0;  // Exit code set by process.exit()
    bool paused_ = false;
    uint32_t stepFramesRemaining_ = 0;
    uint64_t frameCount_ = 0;
    int width_;
    int height_;

    std::unique_ptr<webgpu::Context> webgpu_;
    std::unique_ptr<js::Engine> jsEngine_;
    std::unique_ptr<js::ModuleSystem> moduleSystem_;
    std::shared_ptr<workers::SharedBufferRegistry> sharedBuffers_;
    std::shared_ptr<workers::WorkerRuntimeState> workerRuntimeState_;
    std::unique_ptr<workers::WorkerRegistry> workerRegistry_;
    storage::LocalStorage localStorage_;

    js::AnimationFrameScheduler animationFrames_;
    game::GameLoopController gameLoop_;
    dom::DomEventSystem domEvents_;
    js::RuntimeTimers timers_;
    js::RuntimeIOBindings ioBindings_;
    debug::RuntimeProfiler profiler_;

    // setTimeout/setInterval state
    uint64_t jsGeneration_ = 1;

#ifdef MYSTRAL_HAS_DRACO
    // Context for async Draco decode work (libuv thread pool)
    struct DracoDecodeContext {
        uv_work_t work;
        // Input (copied from JS, safe to read on worker thread)
        std::vector<uint8_t> compressedData;
        int posAttrId = -1;
        int normAttrId = -1;
        int uvAttrId = -1;
        // Output (written by worker thread, read on main thread)
        std::vector<float> positions;
        std::vector<float> normals;
        std::vector<float> uvs;
        std::vector<uint32_t> indices;
        uint32_t numPoints = 0;
        uint32_t numFaces = 0;
        std::string error;
        // JS callback + back-reference
        js::JSValueHandle callback;
        RuntimeImpl* runtime = nullptr;
        uint64_t generation = 0;
    };
    std::queue<std::unique_ptr<DracoDecodeContext>> pendingDracoCallbacks_;
    std::mutex dracoMutex_;
#endif

    // Hot reload state
    std::string scriptPath_;  // Path to the currently loaded script
    std::string watchedScriptName_;
    int watchId_ = -1;        // File watcher ID (-1 if not watching)
    bool reloadRequested_ = false;  // Set when a file change is detected
    std::chrono::steady_clock::time_point reloadDeadline_{};
    bool reloadCandidateObserved_ = false;
    uintmax_t reloadCandidateSize_ = 0;
    std::filesystem::file_time_type reloadCandidateWriteTime_{};
    std::chrono::steady_clock::time_point reloadCandidateSince_{};

};

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<Runtime> Runtime::create(const RuntimeConfig& config) {
    auto runtime = std::make_unique<RuntimeImpl>(config);
    if (!runtime->initialize()) {
        return nullptr;
    }
    return runtime;
}

}  // namespace mystral
