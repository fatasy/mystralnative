/**
 * WebGPU JavaScript Bindings
 *
 * This file exposes the WebGPU API to JavaScript via the JS engine abstraction.
 * Both Dawn and wgpu-native implement the same webgpu.h C API, so the bindings
 * work with either backend.
 *
 * Key APIs exposed:
 * - canvas (global) - represents the window
 * - canvas.getContext('webgpu') -> GPUCanvasContext
 * - navigator.gpu
 * - navigator.gpu.requestAdapter() -> GPUAdapter
 * - GPUAdapter.requestDevice() -> GPUDevice
 * - GPUDevice.createBuffer()
 * - GPUDevice.createShaderModule()
 * - GPUDevice.createRenderPipeline()
 * - GPUDevice.createCommandEncoder()
 * - GPUQueue.submit()
 */

#include "mystral/js/engine.h"
#include "mystral/async/job_system.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <fstream>
#include <thread>
#include <chrono>
#include <utility>
#include <memory>

// stb_image for image loading (implementation in stb_impl.cpp)
#include "stb_image.h"

// libwebp for WebP image decoding (optional - for GLTF EXT_texture_webp extension)
#ifdef MYSTRAL_HAS_WEBP
#include <webp/decode.h>
#endif

// GLTF/GLB loader
#include "mystral/gltf/gltf_loader.h"

// Canvas 2D context (Skia-backed)
#include "mystral/canvas/canvas2d.h"

// Forward declaration for Canvas2D bindings
namespace mystral {
namespace canvas {
    js::JSValueHandle createCanvas2DContext(js::Engine* engine, int width, int height);
    void releaseReloadContexts(js::Engine* engine);
    size_t canvas2DContextCount();
}
}

// ============================================================================
// OffscreenCanvas - stores canvas element state for getContext support
// ============================================================================
struct OffscreenCanvas {
    int width = 300;
    int height = 150;
    mystral::js::JSValueHandle context2d;  // Cached 2D context (created on first getContext call)
    bool hasContext2d = false;
};

// Global storage for offscreen canvases (prevents them from being destroyed)
static std::unordered_map<int, std::unique_ptr<OffscreenCanvas>> g_offscreenCanvases;
static int g_nextOffscreenCanvasId = 0;

#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
#include <webgpu/webgpu.h>
#include "mystral/webgpu_compat.h"
#endif

// wgpu-native specific extension functions (not in standard webgpu.h)
#if defined(MYSTRAL_WEBGPU_WGPU)
extern "C" {
// Device poll - blocks until GPU work is done
// From wgpu/wgpu.h but declared here to avoid include path issues
typedef struct WGPUWrappedSubmissionIndex WGPUWrappedSubmissionIndex;
WGPUBool wgpuDevicePoll(WGPUDevice device, WGPUBool wait, WGPUWrappedSubmissionIndex const* wrappedSubmissionIndex);
}
#endif

namespace mystral {
namespace webgpu {

// Verbose logging flag - controlled by --debug CLI flag or MYSTRAL_DEBUG=1
static bool g_verboseLogging = false;  // Disabled by default, enable with --debug

// Diagnostic escape hatch: MYSTRAL_NO_GC_RELEASE=1 keeps GC-driven Dawn
// resource release OFF (leaks views/bind groups instead) — used to bisect
// native crashes where a resource is collected while still referenced by
// in-flight command buffers.
static bool gcReleaseEnabled() {
    static const bool enabled = std::getenv("MYSTRAL_NO_GC_RELEASE") == nullptr;
    return enabled;
}

// Store references to WebGPU objects
static WGPUAdapter g_adapter = nullptr;
static WGPUDevice g_device = nullptr;
static WGPUQueue g_queue = nullptr;
static WGPUSurface g_surface = nullptr;
static WGPUInstance g_instance = nullptr;
static js::Engine* g_engine = nullptr;

struct PendingPromise {
    js::Engine* engine = nullptr;
    js::JSValueHandle resolve;
    js::JSValueHandle reject;
    uint64_t session = 0;
    bool active = true;
};

static std::mutex g_asyncCompletionMutex;
static std::deque<std::function<void()>> g_asyncCompletions;
static std::unordered_set<PendingPromise*> g_pendingPromises;
static uint64_t g_asyncSession = 1;
static uint64_t g_imageDecodeGeneration = (uint64_t{1} << 63) | 1;

static PendingPromise* createPendingPromise(js::JSValueHandle& promise) {
    auto factory = g_engine->getGlobalProperty("__mystralCreateDeferred");
    auto deferred = g_engine->call(factory, g_engine->newUndefined(), {});
    if (!deferred.ptr) return nullptr;

    auto* pending = new PendingPromise{
        g_engine,
        g_engine->getProperty(deferred, "resolve"),
        g_engine->getProperty(deferred, "reject"),
        g_asyncSession,
        true,
    };
    promise = g_engine->getProperty(deferred, "promise");
    pending->engine->protect(pending->resolve);
    pending->engine->protect(pending->reject);
    g_pendingPromises.insert(pending);
    return pending;
}

static js::JSValueHandle makeError(js::Engine* engine, const std::string& message) {
    auto errorConstructor = engine->getGlobalProperty("Error");
    return engine->call(
        errorConstructor,
        engine->newUndefined(),
        {engine->newString(message.c_str())});
}

static void settlePendingPromise(PendingPromise* pending,
                                 bool success,
                                 js::JSValueHandle value,
                                 const std::string& error = {}) {
    if (!pending) return;
    if (pending->active && pending->session == g_asyncSession &&
        pending->engine == g_engine) {
        auto* engine = pending->engine;
        if (success) {
            engine->call(pending->resolve, engine->newUndefined(), {value});
        } else {
            engine->call(
                pending->reject,
                engine->newUndefined(),
                {makeError(engine, error.empty() ? "WebGPU async operation failed" : error)});
        }
        engine->unprotect(pending->resolve);
        engine->unprotect(pending->reject);
        pending->active = false;
    }
    g_pendingPromises.erase(pending);
    delete pending;
}

static js::JSValueHandle rejectedPromise(const std::string& message) {
    js::JSValueHandle promise;
    auto* pending = createPendingPromise(promise);
    if (!pending) {
        g_engine->throwException(message.c_str());
        return g_engine->newUndefined();
    }
    settlePendingPromise(pending, false, {}, message);
    return promise;
}

static js::JSValueHandle resolvedPromise(js::JSValueHandle value) {
    js::JSValueHandle promise;
    auto* pending = createPendingPromise(promise);
    if (!pending) return g_engine->newUndefined();
    settlePendingPromise(pending, true, value);
    return promise;
}

static void enqueueAsyncCompletion(std::function<void()> completion) {
    std::lock_guard<std::mutex> lock(g_asyncCompletionMutex);
    g_asyncCompletions.push_back(std::move(completion));
}

void processAsyncCompletions() {
    // endDawnFrame already advances the device during normal rendering. Poll
    // here only while a JS-visible operation is pending; this also keeps
    // promises moving when the runtime is paused and no frame is produced.
    if (!g_pendingPromises.empty()) {
#if defined(MYSTRAL_WEBGPU_DAWN)
        if (g_instance) wgpuInstanceProcessEvents(g_instance);
        if (g_device) wgpuDeviceTick(g_device);
#elif defined(MYSTRAL_WEBGPU_WGPU)
        if (g_device) wgpuDevicePoll(g_device, false, nullptr);
#endif
    }

    std::deque<std::function<void()>> completions;
    {
        std::lock_guard<std::mutex> lock(g_asyncCompletionMutex);
        completions.swap(g_asyncCompletions);
    }
    for (auto& completion : completions) completion();
}

static void abandonPendingPromises() {
    for (auto* pending : g_pendingPromises) {
        if (!pending->active) continue;
        pending->engine->unprotect(pending->resolve);
        pending->engine->unprotect(pending->reject);
        pending->active = false;
    }
}

struct DecodedImageData {
    std::vector<uint8_t> encoded;
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    std::string error;
};

static void decodeImageData(const async::JobContext& job, DecodedImageData& image) {
    if (job.isCancelled()) return;
    if (image.encoded.empty() ||
        image.encoded.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        image.error = "Image data is empty or too large to decode";
        return;
    }

    const auto* input = image.encoded.data();
    const size_t inputSize = image.encoded.size();
    const bool isWebP = inputSize >= 12 &&
        input[0] == 'R' && input[1] == 'I' && input[2] == 'F' && input[3] == 'F' &&
        input[8] == 'W' && input[9] == 'E' && input[10] == 'B' && input[11] == 'P';

    unsigned char* decoded = nullptr;
    if (isWebP) {
#ifdef MYSTRAL_HAS_WEBP
        decoded = WebPDecodeRGBA(input, inputSize, &image.width, &image.height);
        if (!decoded) image.error = "Failed to decode WebP image";
#else
        image.error = "WebP image detected but libwebp support is not compiled in";
#endif
    } else {
        int channels = 0;
        decoded = stbi_load_from_memory(
            input,
            static_cast<int>(inputSize),
            &image.width,
            &image.height,
            &channels,
            4);
        if (!decoded) {
            const char* reason = stbi_failure_reason();
            image.error = std::string("Failed to decode image") +
                (reason ? std::string(": ") + reason : std::string());
        }
    }

    if (!decoded) return;
    if (image.width <= 0 || image.height <= 0 ||
        static_cast<size_t>(image.width) >
            (std::numeric_limits<size_t>::max)() / 4 / static_cast<size_t>(image.height)) {
        image.error = "Decoded image dimensions are invalid";
    } else if (!job.isCancelled()) {
        const size_t byteCount =
            static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4;
        image.rgba.assign(decoded, decoded + byteCount);
    }

    if (isWebP) {
#ifdef MYSTRAL_HAS_WEBP
        WebPFree(decoded);
#endif
    } else {
        stbi_image_free(decoded);
    }
}

// ============================================================================
// Limits & features reflection helpers
// ============================================================================

// Fill a JS object with every field of a WGPULimits struct so JS sees what the
// native device/adapter actually supports. This matters for correctness, not
// just introspection: three.js reads limits.maxComputeWorkgroupsPerDimension
// to split large compute dispatches into 2D grids — with the old hardcoded
// object (which omitted the compute limits) a 4096² kernel dispatched
// 262144×1×1 workgroups, exceeding the 65535 cap and invalidating the whole
// command buffer.
static void setLimitsProperties(js::JSValueHandle obj, const WGPULimits& l) {
    auto set = [&](const char* name, double v) {
        g_engine->setProperty(obj, name, g_engine->newNumber(v));
    };
    set("maxTextureDimension1D", l.maxTextureDimension1D);
    set("maxTextureDimension2D", l.maxTextureDimension2D);
    set("maxTextureDimension3D", l.maxTextureDimension3D);
    set("maxTextureArrayLayers", l.maxTextureArrayLayers);
    set("maxBindGroups", l.maxBindGroups);
    set("maxBindGroupsPlusVertexBuffers", l.maxBindGroupsPlusVertexBuffers);
    set("maxBindingsPerBindGroup", l.maxBindingsPerBindGroup);
    set("maxDynamicUniformBuffersPerPipelineLayout", l.maxDynamicUniformBuffersPerPipelineLayout);
    set("maxDynamicStorageBuffersPerPipelineLayout", l.maxDynamicStorageBuffersPerPipelineLayout);
    set("maxSampledTexturesPerShaderStage", l.maxSampledTexturesPerShaderStage);
    set("maxSamplersPerShaderStage", l.maxSamplersPerShaderStage);
    set("maxStorageBuffersPerShaderStage", l.maxStorageBuffersPerShaderStage);
    set("maxStorageTexturesPerShaderStage", l.maxStorageTexturesPerShaderStage);
    set("maxUniformBuffersPerShaderStage", l.maxUniformBuffersPerShaderStage);
    set("maxUniformBufferBindingSize", (double)l.maxUniformBufferBindingSize);
    set("maxStorageBufferBindingSize", (double)l.maxStorageBufferBindingSize);
    set("minUniformBufferOffsetAlignment", l.minUniformBufferOffsetAlignment);
    set("minStorageBufferOffsetAlignment", l.minStorageBufferOffsetAlignment);
    set("maxVertexBuffers", l.maxVertexBuffers);
    set("maxBufferSize", (double)l.maxBufferSize);
    set("maxVertexAttributes", l.maxVertexAttributes);
    set("maxVertexBufferArrayStride", l.maxVertexBufferArrayStride);
    set("maxColorAttachments", l.maxColorAttachments);
    set("maxColorAttachmentBytesPerSample", l.maxColorAttachmentBytesPerSample);
    set("maxComputeWorkgroupStorageSize", l.maxComputeWorkgroupStorageSize);
    set("maxComputeInvocationsPerWorkgroup", l.maxComputeInvocationsPerWorkgroup);
    set("maxComputeWorkgroupSizeX", l.maxComputeWorkgroupSizeX);
    set("maxComputeWorkgroupSizeY", l.maxComputeWorkgroupSizeY);
    set("maxComputeWorkgroupSizeZ", l.maxComputeWorkgroupSizeZ);
    set("maxComputeWorkgroupsPerDimension", l.maxComputeWorkgroupsPerDimension);
}

// Query the real limits of the live device (Dawn and wgpu-native disagree on
// the out-struct shape). Returns false when no device is available.
static bool queryDeviceLimits(WGPULimits* out) {
    if (!g_device) return false;
#if defined(MYSTRAL_WEBGPU_DAWN)
    WGPULimits limits = {};
    wgpuDeviceGetLimits(g_device, &limits);
    *out = limits;
#else
    WGPUSupportedLimits supported = {};
    wgpuDeviceGetLimits(g_device, &supported);
    *out = supported.limits;
#endif
    return true;
}

static bool queryAdapterLimits(WGPULimits* out) {
    if (!g_adapter) return queryDeviceLimits(out);
#if defined(MYSTRAL_WEBGPU_DAWN)
    WGPULimits limits = {};
    wgpuAdapterGetLimits(g_adapter, &limits);
    *out = limits;
#else
    WGPUSupportedLimits supported = {};
    wgpuAdapterGetLimits(g_adapter, &supported);
    *out = supported.limits;
#endif
    return true;
}

// Standard WebGPU feature-name strings → WGPUFeatureName. Only names that are
// spelled identically in the JS spec and available in both backends' headers
// are listed unconditionally; newer enums are Dawn-only.
struct FeatureNameEntry {
    const char* name;
    WGPUFeatureName feature;
};
static const FeatureNameEntry kFeatureNames[] = {
    {"depth-clip-control", WGPUFeatureName_DepthClipControl},
    {"depth32float-stencil8", WGPUFeatureName_Depth32FloatStencil8},
    {"timestamp-query", WGPUFeatureName_TimestampQuery},
    {"texture-compression-bc", WGPUFeatureName_TextureCompressionBC},
    {"texture-compression-etc2", WGPUFeatureName_TextureCompressionETC2},
    {"texture-compression-astc", WGPUFeatureName_TextureCompressionASTC},
    {"indirect-first-instance", WGPUFeatureName_IndirectFirstInstance},
    {"shader-f16", WGPUFeatureName_ShaderF16},
    {"rg11b10ufloat-renderable", WGPUFeatureName_RG11B10UfloatRenderable},
    {"bgra8unorm-storage", WGPUFeatureName_BGRA8UnormStorage},
    {"float32-filterable", WGPUFeatureName_Float32Filterable},
#if defined(MYSTRAL_WEBGPU_DAWN)
    {"float32-blendable", WGPUFeatureName_Float32Blendable},
    {"clip-distances", WGPUFeatureName_ClipDistances},
    {"dual-source-blending", WGPUFeatureName_DualSourceBlending},
    {"subgroups", WGPUFeatureName_Subgroups},
    {"texture-formats-tier1", WGPUFeatureName_TextureFormatsTier1},
#endif
};

// Build a browser-shaped GPUSupportedFeatures: an array of the enabled
// feature-name strings (so `[...features]` works) with a `has()` method.
static js::JSValueHandle buildFeaturesObject(bool (*hasFeature)(WGPUFeatureName)) {
    auto features = g_engine->newArray(0);
    uint32_t count = 0;
    for (const auto& entry : kFeatureNames) {
        if (hasFeature(entry.feature)) {
            g_engine->setPropertyIndex(features, count++, g_engine->newString(entry.name));
        }
    }
    g_engine->setProperty(features, "size", g_engine->newNumber(count));
    g_engine->setProperty(features, "has",
        g_engine->newFunction("has", [hasFeature](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) return g_engine->newBoolean(false);
            std::string name = g_engine->toString(args[0]);
            for (const auto& entry : kFeatureNames) {
                if (name == entry.name) {
                    return g_engine->newBoolean(hasFeature(entry.feature));
                }
            }
            return g_engine->newBoolean(false);
        })
    );
    return features;
}

static bool deviceHasFeature(WGPUFeatureName feature) {
    return g_device && wgpuDeviceHasFeature(g_device, feature);
}

static bool adapterHasFeature(WGPUFeatureName feature) {
    if (!g_adapter) return deviceHasFeature(feature);
    return wgpuAdapterHasFeature(g_adapter, feature);
}

// Offscreen rendering support (for no-SDL mode)
static WGPUTexture g_offscreenTexture = nullptr;
static WGPUTextureView g_offscreenTextureView = nullptr;

// Canvas context state
static WGPUTextureFormat g_surfaceFormat = WGPUTextureFormat_BGRA8UnormSrgb;  // Default, updated from context
static uint32_t g_canvasWidth = 800;
static uint32_t g_canvasHeight = 600;
static bool g_contextConfigured = false;

// Current frame's texture (refreshed each frame)
static WGPUTexture g_currentTexture = nullptr;
static WGPUTextureView g_currentTextureView = nullptr;
// Track the texture that g_currentTextureView was created from
// (needed for screenshot since g_currentTexture may change between createView and screenshot)
static WGPUTexture g_currentViewSourceTexture = nullptr;

// Screenshot support - persistent buffer for capturing frames
static WGPUBuffer g_screenshotBuffer = nullptr;
static size_t g_screenshotBufferSize = 0;
static uint32_t g_screenshotBytesPerRow = 0;

// Global state for render pass (needed for callbacks in lambdas)
// These need to be declared here so they're visible to all lambdas
static WGPURenderPassEncoder g_jsRenderPass = nullptr;
static WGPUComputePassEncoder g_jsComputePass = nullptr;
static WGPUCommandEncoder g_jsCommandEncoder = nullptr;

// Per-encoder render pass tracking (fixes issue with multiple encoders)
// Maps command encoder pointer to its active render pass encoder
static std::unordered_map<WGPUCommandEncoder, WGPURenderPassEncoder> g_encoderRenderPassMap;
static std::unordered_map<WGPUCommandEncoder, WGPUComputePassEncoder> g_encoderComputePassMap;
static std::unordered_map<WGPURenderPassEncoder, WGPUCommandEncoder> g_renderPassEncoderMap;
static std::unordered_map<WGPUComputePassEncoder, WGPUCommandEncoder> g_computePassEncoderMap;
static std::unordered_set<WGPUCommandEncoder> g_liveCommandEncoders;
static std::unordered_set<WGPURenderPassEncoder> g_liveRenderPasses;
static std::unordered_set<WGPUComputePassEncoder> g_liveComputePasses;
// Track whether the current frame's surface render pass has been ended.
static bool g_surfaceRenderPassEnded = false;
static WGPUCommandEncoder g_surfaceRenderEncoder = nullptr;
static uint64_t g_commandEncodersCreated = 0;
static uint64_t g_renderPassesCreated = 0;
static uint64_t g_computePassesCreated = 0;
static uint64_t g_commandBuffersCreated = 0;

enum class ProfiledBridgeOp : size_t {
    CreateCommandEncoder,
    BeginRenderPass,
    BeginComputePass,
    FinishCommandEncoder,
    QueueSubmit,
    NativeQueueSubmit,
    QueueWriteBuffer,
    QueueWriteBufferBatch,
    QueueWriteTexture,
    RenderSetPipeline,
    RenderSetBindGroup,
    RenderSetVertexBuffer,
    RenderSetIndexBuffer,
    RenderDraw,
    RenderDrawIndexed,
    RenderDrawIndirect,
    RenderDrawIndexedIndirect,
    RenderExecuteBundles,
    RenderEnd,
    ComputeSetPipeline,
    ComputeSetBindGroup,
    ComputeDispatchWorkgroups,
    ComputeEnd,
    Count,
};

struct ProfiledBridgeMetric {
    uint64_t calls = 0;
    uint64_t totalNanoseconds = 0;
    uint64_t bytes = 0;
};

static constexpr std::array<const char*, static_cast<size_t>(ProfiledBridgeOp::Count)>
    kProfiledBridgeOpNames = {
        "createCommandEncoder",
        "beginRenderPass",
        "beginComputePass",
        "finishCommandEncoder",
        "queueSubmit",
        "nativeQueueSubmit",
        "queueWriteBuffer",
        "queueWriteBufferBatch",
        "queueWriteTexture",
        "renderSetPipeline",
        "renderSetBindGroup",
        "renderSetVertexBuffer",
        "renderSetIndexBuffer",
        "renderDraw",
        "renderDrawIndexed",
        "renderDrawIndirect",
        "renderDrawIndexedIndirect",
        "renderExecuteBundles",
        "renderEnd",
        "computeSetPipeline",
        "computeSetBindGroup",
        "computeDispatchWorkgroups",
        "computeEnd",
    };
static std::array<ProfiledBridgeMetric, static_cast<size_t>(ProfiledBridgeOp::Count)>
    g_profiledBridgeMetrics;

class ScopedBridgeMeasurement {
public:
    explicit ScopedBridgeMeasurement(ProfiledBridgeOp operation)
        : metric_(g_profiledBridgeMetrics[static_cast<size_t>(operation)]),
          startedAt_(std::chrono::steady_clock::now()) {}

    ~ScopedBridgeMeasurement() {
        const auto elapsed = std::chrono::steady_clock::now() - startedAt_;
        metric_.calls++;
        metric_.totalNanoseconds += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }

private:
    ProfiledBridgeMetric& metric_;
    std::chrono::steady_clock::time_point startedAt_;
};

static void recordBridgeBytes(ProfiledBridgeOp operation, uint64_t bytes) {
    g_profiledBridgeMetrics[static_cast<size_t>(operation)].bytes += bytes;
}

// Three.js finishes many independent command buffers per animation frame. The
// browser WebGPU API preserves queue.writeBuffer ordering between those submits,
// so deferring only the command buffers would make earlier passes observe later
// uniform data. Keep an ordered stream of upload batches and command buffers,
// convert buffer writes to staging-buffer copies, then submit the whole stream
// once at the end of the native frame.
struct DeferredBufferCopy {
    WGPUBuffer destination = nullptr;
    uint64_t destinationOffset = 0;
    uint64_t sourceOffset = 0;
    uint64_t size = 0;
};

struct DeferredQueueWork {
    enum class Kind {
        BufferCopies,
        CommandBuffers,
    };

    Kind kind = Kind::CommandBuffers;
    std::vector<DeferredBufferCopy> bufferCopies;
    std::vector<WGPUCommandBuffer> commandBuffers;
};

static bool g_dawnFrameActive = false;
static std::vector<DeferredQueueWork> g_deferredQueueWork;
static std::vector<uint8_t> g_deferredUploadBytes;
static std::unordered_set<WGPUBuffer> g_deferredUploadBuffers;
static uint64_t g_nativeQueueSubmits = 0;
static uint64_t g_nativeCommandBuffersSubmitted = 0;
static uint64_t g_nativeQueueSubmitNanoseconds = 0;
static uint64_t g_deferredUploadCommandBuffers = 0;
static uint64_t g_forcedFrameFlushes = 0;
static uint64_t g_maxCommandBuffersPerNativeSubmit = 0;

static uint64_t alignToCopyOffset(uint64_t value) {
    return (value + 3u) & ~uint64_t(3u);
}

static void recordNativeSubmit(const std::vector<WGPUCommandBuffer>& commandBuffers) {
    if (!g_queue || commandBuffers.empty()) return;
    const auto startedAt = std::chrono::steady_clock::now();
    wgpuQueueSubmit(g_queue, commandBuffers.size(), commandBuffers.data());
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;
    g_nativeQueueSubmits++;
    g_nativeCommandBuffersSubmitted += commandBuffers.size();
    g_maxCommandBuffersPerNativeSubmit = std::max<uint64_t>(
        g_maxCommandBuffersPerNativeSubmit, commandBuffers.size());
    g_nativeQueueSubmitNanoseconds += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    auto& metric = g_profiledBridgeMetrics[static_cast<size_t>(ProfiledBridgeOp::NativeQueueSubmit)];
    metric.calls++;
    metric.totalNanoseconds += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

static void releaseDeferredCommandBuffers() {
    for (auto& work : g_deferredQueueWork) {
        for (auto commandBuffer : work.commandBuffers) {
            if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
        }
    }
}

static void clearDeferredQueueWork() {
    for (auto buffer : g_deferredUploadBuffers) {
        wgpuBufferRelease(buffer);
    }
    g_deferredUploadBuffers.clear();
    g_deferredQueueWork.clear();
    g_deferredUploadBytes.clear();
}

static void fallbackFlushDeferredQueueWork() {
    // Allocation failure must not change queue ordering. Fall back to the old
    // write/submit sequence for this flush rather than submitting corrupt data.
    for (auto& work : g_deferredQueueWork) {
        if (work.kind == DeferredQueueWork::Kind::BufferCopies) {
            for (const auto& copy : work.bufferCopies) {
                wgpuQueueWriteBuffer(g_queue, copy.destination, copy.destinationOffset,
                    g_deferredUploadBytes.data() + copy.sourceOffset, copy.size);
            }
            continue;
        }

        recordNativeSubmit(work.commandBuffers);
        for (auto commandBuffer : work.commandBuffers) {
            if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
        }
    }
    clearDeferredQueueWork();
}

static void flushDeferredQueueWork() {
    if (g_deferredQueueWork.empty()) return;
    if (!g_queue || !g_device) {
        releaseDeferredCommandBuffers();
        clearDeferredQueueWork();
        return;
    }

    WGPUBuffer stagingBuffer = nullptr;
    if (!g_deferredUploadBytes.empty()) {
        WGPUBufferDescriptor stagingDescriptor = {};
        stagingDescriptor.size = alignToCopyOffset(g_deferredUploadBytes.size());
        stagingDescriptor.usage = WGPUBufferUsage_MapWrite | WGPUBufferUsage_CopySrc;
        stagingDescriptor.mappedAtCreation = true;
        stagingBuffer = wgpuDeviceCreateBuffer(g_device, &stagingDescriptor);

        void* mapped = stagingBuffer
            ? wgpuBufferGetMappedRange(stagingBuffer, 0, stagingDescriptor.size)
            : nullptr;
        if (!mapped) {
            if (stagingBuffer) wgpuBufferRelease(stagingBuffer);
            fallbackFlushDeferredQueueWork();
            return;
        }

        std::memcpy(mapped, g_deferredUploadBytes.data(), g_deferredUploadBytes.size());
        wgpuBufferUnmap(stagingBuffer);
    }

    std::vector<WGPUCommandBuffer> submissionBuffers;
    std::vector<WGPUCommandBuffer> uploadCommandBuffers;
    bool encodingFailed = false;

    for (const auto& work : g_deferredQueueWork) {
        if (work.kind == DeferredQueueWork::Kind::CommandBuffers) {
            submissionBuffers.insert(submissionBuffers.end(),
                work.commandBuffers.begin(), work.commandBuffers.end());
            continue;
        }

        WGPUCommandEncoderDescriptor encoderDescriptor = {};
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(g_device, &encoderDescriptor);
        if (!encoder) {
            encodingFailed = true;
            break;
        }

        for (const auto& copy : work.bufferCopies) {
            wgpuCommandEncoderCopyBufferToBuffer(encoder,
                stagingBuffer, copy.sourceOffset,
                copy.destination, copy.destinationOffset,
                copy.size);
        }

        WGPUCommandBufferDescriptor commandBufferDescriptor = {};
        WGPUCommandBuffer commandBuffer = wgpuCommandEncoderFinish(encoder, &commandBufferDescriptor);
        wgpuCommandEncoderRelease(encoder);
        if (!commandBuffer) {
            encodingFailed = true;
            break;
        }

        uploadCommandBuffers.push_back(commandBuffer);
        submissionBuffers.push_back(commandBuffer);
        g_deferredUploadCommandBuffers++;
    }

    if (encodingFailed) {
        for (auto commandBuffer : uploadCommandBuffers) {
            wgpuCommandBufferRelease(commandBuffer);
        }
        if (stagingBuffer) wgpuBufferRelease(stagingBuffer);
        fallbackFlushDeferredQueueWork();
        return;
    }

    recordNativeSubmit(submissionBuffers);
    releaseDeferredCommandBuffers();
    for (auto commandBuffer : uploadCommandBuffers) {
        wgpuCommandBufferRelease(commandBuffer);
    }
    if (stagingBuffer) wgpuBufferRelease(stagingBuffer);
    clearDeferredQueueWork();
}

static void deferCommandBuffers(std::vector<WGPUCommandBuffer>&& commandBuffers) {
    if (commandBuffers.empty()) return;
    if (!g_dawnFrameActive) {
        recordNativeSubmit(commandBuffers);
        for (auto commandBuffer : commandBuffers) {
            wgpuCommandBufferRelease(commandBuffer);
        }
        return;
    }

    if (!g_deferredQueueWork.empty() &&
        g_deferredQueueWork.back().kind == DeferredQueueWork::Kind::CommandBuffers) {
        auto& destination = g_deferredQueueWork.back().commandBuffers;
        destination.insert(destination.end(), commandBuffers.begin(), commandBuffers.end());
        return;
    }

    DeferredQueueWork work;
    work.kind = DeferredQueueWork::Kind::CommandBuffers;
    work.commandBuffers = std::move(commandBuffers);
    g_deferredQueueWork.push_back(std::move(work));
}

static void writeOrDeferQueueBuffer(WGPUBuffer buffer, uint64_t offset,
                                    const void* data, size_t size) {
    if (!buffer || !g_queue || !data || size == 0) return;

    // Valid WebGPU writeBuffer calls are 4-byte aligned. Preserve invalid-call
    // behavior through the native API instead of encoding an invalid copy.
    if (!g_dawnFrameActive || (offset & 3u) != 0 || (size & 3u) != 0) {
        if (g_dawnFrameActive && !g_deferredQueueWork.empty()) {
            g_forcedFrameFlushes++;
            flushDeferredQueueWork();
        }
        wgpuQueueWriteBuffer(g_queue, buffer, offset, data, size);
        return;
    }

    const uint64_t sourceOffset = alignToCopyOffset(g_deferredUploadBytes.size());
    g_deferredUploadBytes.resize(static_cast<size_t>(sourceOffset + size));
    std::memcpy(g_deferredUploadBytes.data() + sourceOffset, data, size);
    if (g_deferredUploadBuffers.insert(buffer).second) {
        wgpuBufferAddRef(buffer);
    }

    if (g_deferredQueueWork.empty() ||
        g_deferredQueueWork.back().kind != DeferredQueueWork::Kind::BufferCopies) {
        DeferredQueueWork work;
        work.kind = DeferredQueueWork::Kind::BufferCopies;
        g_deferredQueueWork.push_back(std::move(work));
    }

    g_deferredQueueWork.back().bufferCopies.push_back(
        {buffer, offset, sourceOffset, static_cast<uint64_t>(size)});
}

static bool writeQueueBuffer(js::JSValueHandle bufferHandle,
                             js::JSValueHandle offsetHandle,
                             js::JSValueHandle data,
                             const js::JSValueHandle* dataOffsetHandle,
                             const js::JSValueHandle* sizeHandle,
                             ProfiledBridgeOp operation,
                             const char* apiName) {
    WGPUBuffer buffer = (WGPUBuffer)g_engine->getPrivateData(bufferHandle);
    uint64_t offset = (uint64_t)g_engine->toNumber(offsetHandle);

    double bytesPerElement = 1;  // ArrayBuffer/DataView: offsets are bytes
    void* basePtr = nullptr;
    size_t viewByteOffset = 0;
    size_t viewByteLength = 0;

    auto bufferProp = g_engine->getProperty(data, "buffer");
    if (g_engine->isObject(bufferProp)) {
        // TypedArray or DataView: resolve against the underlying ArrayBuffer so
        // engines whose getArrayBufferData ignores view offsets still copy the
        // right window.
        auto bpeProp = g_engine->getProperty(data, "BYTES_PER_ELEMENT");
        if (g_engine->isNumber(bpeProp)) {
            bytesPerElement = g_engine->toNumber(bpeProp);
        }
        size_t baseSize = 0;
        basePtr = g_engine->getArrayBufferData(bufferProp, &baseSize);
        auto byteOffsetProp = g_engine->getProperty(data, "byteOffset");
        auto byteLengthProp = g_engine->getProperty(data, "byteLength");
        viewByteOffset = (size_t)g_engine->toNumber(byteOffsetProp);
        viewByteLength = (size_t)g_engine->toNumber(byteLengthProp);
        g_engine->releaseValue(bpeProp);
        g_engine->releaseValue(byteOffsetProp);
        g_engine->releaseValue(byteLengthProp);
    } else {
        size_t dataSize = 0;
        basePtr = g_engine->getArrayBufferData(data, &dataSize);
        viewByteLength = dataSize;
    }
    g_engine->releaseValue(bufferProp);

    if (!basePtr || viewByteLength == 0) {
        g_engine->throwException((std::string(apiName) + ": invalid data").c_str());
        return false;
    }

    size_t dataOffsetBytes = dataOffsetHandle && !g_engine->isUndefined(*dataOffsetHandle)
        ? (size_t)(g_engine->toNumber(*dataOffsetHandle) * bytesPerElement)
        : 0;
    if (dataOffsetBytes > viewByteLength) {
        g_engine->throwException((std::string(apiName) + ": range out of bounds").c_str());
        return false;
    }
    size_t writeSize = sizeHandle && !g_engine->isUndefined(*sizeHandle)
        ? (size_t)(g_engine->toNumber(*sizeHandle) * bytesPerElement)
        : (viewByteLength - dataOffsetBytes);

    if (dataOffsetBytes + writeSize > viewByteLength) {
        g_engine->throwException((std::string(apiName) + ": range out of bounds").c_str());
        return false;
    }

    if (buffer && g_queue) {
        writeOrDeferQueueBuffer(buffer, offset,
            (uint8_t*)basePtr + viewByteOffset + dataOffsetBytes, writeSize);
        recordBridgeBytes(operation, writeSize);
    }
    return true;
}

struct TransientMethodCache {
    js::JSValueHandle encoderBeginRenderPass;
    js::JSValueHandle encoderBeginComputePass;
    js::JSValueHandle encoderCopyBufferToBuffer;
    js::JSValueHandle encoderCopyBufferToTexture;
    js::JSValueHandle encoderCopyTextureToBuffer;
    js::JSValueHandle encoderCopyTextureToTexture;
    js::JSValueHandle encoderClearBuffer;
    js::JSValueHandle encoderFinish;
    js::JSValueHandle renderSetPipeline;
    js::JSValueHandle renderSetBindGroup;
    js::JSValueHandle renderDraw;
    js::JSValueHandle renderSetVertexBuffer;
    js::JSValueHandle renderSetIndexBuffer;
    js::JSValueHandle renderDrawIndexed;
    js::JSValueHandle renderDrawIndirect;
    js::JSValueHandle renderDrawIndexedIndirect;
    js::JSValueHandle renderSetViewport;
    js::JSValueHandle renderSetScissorRect;
    js::JSValueHandle renderSetBlendConstant;
    js::JSValueHandle renderSetStencilReference;
    js::JSValueHandle renderExecuteBundles;
    js::JSValueHandle renderEnd;
    js::JSValueHandle computeSetPipeline;
    js::JSValueHandle computeSetBindGroup;
    js::JSValueHandle computeDispatchWorkgroups;
    js::JSValueHandle computeEnd;
};
static TransientMethodCache g_transientMethods;

static js::JSValueHandle sharedMethod(js::JSValueHandle& slot,
                                      const char* name,
                                      js::NativeMethod method) {
    if (!slot.ptr) {
        slot = g_engine->newMethod(name, std::move(method));
        // The C++ handle is reused to attach the same JS Function to future
        // wrappers. The wrapper properties keep the Function itself reachable.
        g_engine->protect(slot);
    }
    return slot;
}

static void closeRenderPass(WGPURenderPassEncoder pass) {
    if (!pass || g_liveRenderPasses.erase(pass) == 0) return;
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    auto owner = g_renderPassEncoderMap.find(pass);
    if (owner != g_renderPassEncoderMap.end()) {
        g_encoderRenderPassMap.erase(owner->second);
        if (g_surfaceRenderEncoder == owner->second) g_surfaceRenderPassEnded = true;
        g_renderPassEncoderMap.erase(owner);
    }
    if (g_jsRenderPass == pass) g_jsRenderPass = nullptr;
}

static void closeComputePass(WGPUComputePassEncoder pass) {
    if (!pass || g_liveComputePasses.erase(pass) == 0) return;
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    auto owner = g_computePassEncoderMap.find(pass);
    if (owner != g_computePassEncoderMap.end()) {
        g_encoderComputePassMap.erase(owner->second);
        g_computePassEncoderMap.erase(owner);
    }
    if (g_jsComputePass == pass) g_jsComputePass = nullptr;
}

static bool g_screenshotPending = false;
static bool g_screenshotReady = false;
// Prevent capturing multiple screenshots per frame (Three.js does multiple queue.submit() per frame)
static bool g_screenshotCapturedThisFrame = false;
static std::vector<uint8_t> g_screenshotData;

// Main canvas 2D context (for Canvas 2D to WebGPU compositing)
static canvas::Canvas2DContext* g_mainCanvas2DContext = nullptr;

// Texture registry for tracking user-created textures
// Maps texture ID to {texture, format, dimensions, etc.}
struct TextureInfo {
    WGPUTexture texture;
    WGPUTextureFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t depthOrArrayLayers;
    uint32_t mipLevelCount;
    WGPUTextureDimension dimension;  // 1D, 2D, or 3D
    bool ownsReference;
    bool destroyOnReload;
};
static std::unordered_map<uint64_t, TextureInfo> g_textureRegistry;
static uint64_t g_nextTextureId = 1;
static uint64_t g_currentTextureId = 0;

// Buffer registry for tracking buffers (needed for mapping operations)
struct BufferInfo {
    WGPUBuffer buffer;
    uint64_t size;
    WGPUBufferUsage usage;
    bool isMapped;
    void* mappedData;
    uint64_t mappedSize;
    WGPUMapMode mapMode;  // Track whether mapped for read or write
    bool mapPending;
};
static std::unordered_map<uint64_t, BufferInfo> g_bufferRegistry;
static uint64_t g_nextBufferId = 1;

// Pipeline registries for getBindGroupLayout support
static std::unordered_map<uint64_t, WGPUComputePipeline> g_computePipelineRegistry;
static uint64_t g_nextComputePipelineId = 1;
static std::unordered_map<uint64_t, WGPURenderPipeline> g_renderPipelineRegistry;
static uint64_t g_nextRenderPipelineId = 1;

static void releaseBuffer(uint64_t id, bool destroy) {
    auto it = g_bufferRegistry.find(id);
    if (it == g_bufferRegistry.end()) return;
    if (destroy) wgpuBufferDestroy(it->second.buffer);
    wgpuBufferRelease(it->second.buffer);
    g_bufferRegistry.erase(it);
}

static void releaseTexture(uint64_t id, bool destroy) {
    auto it = g_textureRegistry.find(id);
    if (it == g_textureRegistry.end()) return;
    if (destroy && it->second.destroyOnReload) {
        wgpuTextureDestroy(it->second.texture);
    }
    if (it->second.ownsReference) {
        wgpuTextureRelease(it->second.texture);
    }
    g_textureRegistry.erase(it);
}

static void presentSurfaceIfReady() {
    if (!g_surface || !g_currentTexture || !g_surfaceRenderPassEnded) return;

    if (g_verboseLogging) std::cout << "[WebGPU] Presenting surface" << std::endl;
    wgpuSurfacePresent(g_surface);

    g_surfaceRenderEncoder = nullptr;
    g_surfaceRenderPassEnded = false;
    g_screenshotCapturedThisFrame = false;

    if (g_currentTextureView) {
        wgpuTextureViewRelease(g_currentTextureView);
        g_currentTextureView = nullptr;
    }

    if (g_currentTextureId != 0) {
        releaseTexture(g_currentTextureId, false);
        g_currentTextureId = 0;
    }
    g_currentTexture = nullptr;
    g_currentViewSourceTexture = nullptr;
}

static void releaseRenderPipeline(uint64_t id) {
    auto it = g_renderPipelineRegistry.find(id);
    if (it == g_renderPipelineRegistry.end()) return;
    wgpuRenderPipelineRelease(it->second);
    g_renderPipelineRegistry.erase(it);
}

static void releaseComputePipeline(uint64_t id) {
    auto it = g_computePipelineRegistry.find(id);
    if (it == g_computePipelineRegistry.end()) return;
    wgpuComputePipelineRelease(it->second);
    g_computePipelineRegistry.erase(it);
}

static js::JSValueHandle wrapRenderPipeline(WGPURenderPipeline pipeline) {
    const uint64_t pipelineId = g_nextRenderPipelineId++;
    g_renderPipelineRegistry[pipelineId] = pipeline;

    auto jsPipeline = g_engine->newObject();
    g_engine->setPrivateData(jsPipeline, pipeline);
    g_engine->setProperty(jsPipeline, "_pipelineId", g_engine->newNumber((double)pipelineId));
    g_engine->setProperty(jsPipeline, "_type", g_engine->newString("renderPipeline"));
    g_engine->setProperty(jsPipeline, "getBindGroupLayout",
        g_engine->newFunction("getBindGroupLayout", [pipelineId](void*, const std::vector<js::JSValueHandle>& args) {
            auto it = g_renderPipelineRegistry.find(pipelineId);
            if (it == g_renderPipelineRegistry.end() || !it->second) {
                std::cerr << "[WebGPU] getBindGroupLayout: Render pipeline not found" << std::endl;
                return g_engine->newUndefined();
            }

            const uint32_t groupIndex = args.empty() ? 0 : (uint32_t)g_engine->toNumber(args[0]);
            WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(it->second, groupIndex);
            if (!layout) return g_engine->newUndefined();

            auto jsLayout = g_engine->newObject();
            g_engine->setPrivateData(jsLayout, layout);
            g_engine->setProperty(jsLayout, "_type", g_engine->newString("bindGroupLayout"));
            if (gcReleaseEnabled()) {
                g_engine->registerRelease(jsLayout, [layout]() { wgpuBindGroupLayoutRelease(layout); });
            }
            return jsLayout;
        }));
    if (gcReleaseEnabled()) {
        g_engine->registerRelease(jsPipeline, [pipelineId]() { releaseRenderPipeline(pipelineId); });
    }
    if (g_verboseLogging) {
        std::cout << "[WebGPU] Render pipeline created (id=" << pipelineId << ")" << std::endl;
    }
    return jsPipeline;
}

static js::JSValueHandle wrapComputePipeline(WGPUComputePipeline pipeline) {
    const uint64_t pipelineId = g_nextComputePipelineId++;
    g_computePipelineRegistry[pipelineId] = pipeline;

    auto jsPipeline = g_engine->newObject();
    g_engine->setPrivateData(jsPipeline, pipeline);
    g_engine->setProperty(jsPipeline, "_pipelineId", g_engine->newNumber((double)pipelineId));
    g_engine->setProperty(jsPipeline, "_type", g_engine->newString("computePipeline"));
    g_engine->setProperty(jsPipeline, "getBindGroupLayout",
        g_engine->newFunction("getBindGroupLayout", [pipelineId](void*, const std::vector<js::JSValueHandle>& args) {
            auto it = g_computePipelineRegistry.find(pipelineId);
            if (it == g_computePipelineRegistry.end() || !it->second) {
                std::cerr << "[WebGPU] getBindGroupLayout: Compute pipeline not found" << std::endl;
                return g_engine->newUndefined();
            }

            const uint32_t groupIndex = args.empty() ? 0 : (uint32_t)g_engine->toNumber(args[0]);
            WGPUBindGroupLayout layout = wgpuComputePipelineGetBindGroupLayout(it->second, groupIndex);
            if (!layout) return g_engine->newUndefined();

            auto jsLayout = g_engine->newObject();
            g_engine->setPrivateData(jsLayout, layout);
            g_engine->setProperty(jsLayout, "_type", g_engine->newString("bindGroupLayout"));
            if (gcReleaseEnabled()) {
                g_engine->registerRelease(jsLayout, [layout]() { wgpuBindGroupLayoutRelease(layout); });
            }
            return jsLayout;
        }));
    if (gcReleaseEnabled()) {
        g_engine->registerRelease(jsPipeline, [pipelineId]() { releaseComputePipeline(pipelineId); });
    }
    if (g_verboseLogging) {
        std::cout << "[WebGPU] Compute pipeline created (id=" << pipelineId << ")" << std::endl;
    }
    return jsPipeline;
}

// Dawn resource cleanup is handled via Engine::registerRelease(), which sets up
// V8 weak callbacks. When the JS wrapper object is garbage collected (no more
// JS references), the callback fires and releases the Dawn resource.
// This is the same pattern Chrome uses for WebGPU resource lifecycle.

struct BufferMapAsyncContext {
    PendingPromise* promise = nullptr;
    uint64_t bufferId = 0;
    WGPUMapMode mode = WGPUMapMode_None;
};

// Buffer map completion is delivered back to JavaScript on the runtime thread.
#if defined(MYSTRAL_WEBGPU_DAWN)
static void onBufferMapped(WGPUMapAsyncStatus status,
                           WGPUStringView message,
                           void* userdata1,
                           void*) {
    auto* context = static_cast<BufferMapAsyncContext*>(userdata1);
    const std::string error = message.data && message.length > 0
        ? std::string(message.data, message.length)
        : std::string();
    enqueueAsyncCompletion([context, status, error]() {
        auto* pending = context->promise;
        if (!pending->active || pending->session != g_asyncSession) {
            settlePendingPromise(pending, false, {});
            delete context;
            return;
        }

        auto buffer = g_bufferRegistry.find(context->bufferId);
        if (buffer == g_bufferRegistry.end()) {
            settlePendingPromise(pending, false, {}, "GPUBuffer was destroyed while mapping");
        } else {
            buffer->second.mapPending = false;
            if (status == WGPUMapAsyncStatus_Success) {
                buffer->second.isMapped = true;
                buffer->second.mapMode = context->mode;
                settlePendingPromise(pending, true, g_engine->newUndefined());
            } else {
                settlePendingPromise(
                    pending,
                    false,
                    {},
                    error.empty() ? "GPUBuffer mapping failed" : error);
            }
        }
        delete context;
    });
}

static void onQueueWorkDone(WGPUQueueWorkDoneStatus status,
                            WGPUStringView message,
                            void* userdata1,
                            void*) {
    auto* pending = static_cast<PendingPromise*>(userdata1);
    const std::string error = message.data && message.length > 0
        ? std::string(message.data, message.length)
        : std::string();
    enqueueAsyncCompletion([pending, status, error]() {
        if (!pending->active || pending->session != g_asyncSession || pending->engine != g_engine) {
            settlePendingPromise(pending, false, {});
            return;
        }
        if (status == WGPUQueueWorkDoneStatus_Success) {
            settlePendingPromise(pending, true, g_engine->newUndefined());
        } else {
            settlePendingPromise(
                pending,
                false,
                {},
                error.empty() ? "GPU queue work failed" : error);
        }
    });
}

static void onRenderPipelineCreated(WGPUCreatePipelineAsyncStatus status,
                                    WGPURenderPipeline pipeline,
                                    WGPUStringView message,
                                    void* userdata1,
                                    void*) {
    auto* pending = static_cast<PendingPromise*>(userdata1);
    const std::string error = message.data && message.length > 0
        ? std::string(message.data, message.length)
        : std::string();
    enqueueAsyncCompletion([pending, status, pipeline, error]() {
        if (!pending->active || pending->session != g_asyncSession || pending->engine != g_engine) {
            if (pipeline) wgpuRenderPipelineRelease(pipeline);
            settlePendingPromise(pending, false, {});
            return;
        }
        if (status == WGPUCreatePipelineAsyncStatus_Success && pipeline) {
            settlePendingPromise(pending, true, wrapRenderPipeline(pipeline));
        } else {
            if (pipeline) wgpuRenderPipelineRelease(pipeline);
            settlePendingPromise(
                pending,
                false,
                {},
                error.empty() ? "Failed to create render pipeline asynchronously" : error);
        }
    });
}

static void onComputePipelineCreated(WGPUCreatePipelineAsyncStatus status,
                                     WGPUComputePipeline pipeline,
                                     WGPUStringView message,
                                     void* userdata1,
                                     void*) {
    auto* pending = static_cast<PendingPromise*>(userdata1);
    const std::string error = message.data && message.length > 0
        ? std::string(message.data, message.length)
        : std::string();
    enqueueAsyncCompletion([pending, status, pipeline, error]() {
        if (!pending->active || pending->session != g_asyncSession || pending->engine != g_engine) {
            if (pipeline) wgpuComputePipelineRelease(pipeline);
            settlePendingPromise(pending, false, {});
            return;
        }
        if (status == WGPUCreatePipelineAsyncStatus_Success && pipeline) {
            settlePendingPromise(pending, true, wrapComputePipeline(pipeline));
        } else {
            if (pipeline) wgpuComputePipelineRelease(pipeline);
            settlePendingPromise(
                pending,
                false,
                {},
                error.empty() ? "Failed to create compute pipeline asynchronously" : error);
        }
    });
}
#else
static void onBufferMapped(WGPUBufferMapAsyncStatus status, void* userdata) {
    auto* context = static_cast<BufferMapAsyncContext*>(userdata);
    enqueueAsyncCompletion([context, status]() {
        auto* pending = context->promise;
        if (!pending->active || pending->session != g_asyncSession || pending->engine != g_engine) {
            settlePendingPromise(pending, false, {});
            delete context;
            return;
        }

        auto buffer = g_bufferRegistry.find(context->bufferId);
        if (buffer == g_bufferRegistry.end()) {
            settlePendingPromise(pending, false, {}, "GPUBuffer was destroyed while mapping");
        } else {
            buffer->second.mapPending = false;
            if (status == WGPUBufferMapAsyncStatus_Success_Compat) {
                buffer->second.isMapped = true;
                buffer->second.mapMode = context->mode;
                settlePendingPromise(pending, true, g_engine->newUndefined());
            } else {
                settlePendingPromise(pending, false, {}, "GPUBuffer mapping failed");
            }
        }
        delete context;
    });
}
#endif

/**
 * Convert texture format enum to string
 */
static const char* formatToString(WGPUTextureFormat format) {
    switch (format) {
        case WGPUTextureFormat_BGRA8Unorm: return "bgra8unorm";
        case WGPUTextureFormat_BGRA8UnormSrgb: return "bgra8unorm-srgb";
        case WGPUTextureFormat_RGBA8Unorm: return "rgba8unorm";
        case WGPUTextureFormat_RGBA8UnormSrgb: return "rgba8unorm-srgb";
        case WGPUTextureFormat_R8Unorm: return "r8unorm";
        case WGPUTextureFormat_RG8Unorm: return "rg8unorm";
        case WGPUTextureFormat_R16Float: return "r16float";
        case WGPUTextureFormat_RG16Float: return "rg16float";
        case WGPUTextureFormat_R32Float: return "r32float";
        case WGPUTextureFormat_RG32Float: return "rg32float";
        case WGPUTextureFormat_RGBA16Float: return "rgba16float";
        case WGPUTextureFormat_RGBA32Float: return "rgba32float";
        case WGPUTextureFormat_Depth24Plus: return "depth24plus";
        case WGPUTextureFormat_Depth24PlusStencil8: return "depth24plus-stencil8";
        case WGPUTextureFormat_Depth32Float: return "depth32float";
        default: return "bgra8unorm";  // Default
    }
}

/**
 * Parse texture format string to enum
 */
static WGPUTextureFormat stringToFormat(const std::string& format) {
    if (format == "bgra8unorm") return WGPUTextureFormat_BGRA8Unorm;
    if (format == "bgra8unorm-srgb") return WGPUTextureFormat_BGRA8UnormSrgb;
    if (format == "rgba8unorm") return WGPUTextureFormat_RGBA8Unorm;
    if (format == "rgba8unorm-srgb") return WGPUTextureFormat_RGBA8UnormSrgb;
    if (format == "r8unorm") return WGPUTextureFormat_R8Unorm;
    if (format == "rg8unorm") return WGPUTextureFormat_RG8Unorm;
    if (format == "r16float") return WGPUTextureFormat_R16Float;
    if (format == "rg16float") return WGPUTextureFormat_RG16Float;
    if (format == "r32float") return WGPUTextureFormat_R32Float;
    if (format == "rg32float") return WGPUTextureFormat_RG32Float;
    if (format == "rgba16float") return WGPUTextureFormat_RGBA16Float;
    if (format == "rgba32float") return WGPUTextureFormat_RGBA32Float;
    if (format == "depth24plus") return WGPUTextureFormat_Depth24Plus;
    if (format == "depth24plus-stencil8") return WGPUTextureFormat_Depth24PlusStencil8;
    if (format == "depth32float") return WGPUTextureFormat_Depth32Float;
    // Log unrecognized formats for debugging
    if (!format.empty()) {
        std::cerr << "[WebGPU] Warning: Unrecognized format '" << format << "', defaulting to BGRA8Unorm" << std::endl;
    }
    return WGPUTextureFormat_BGRA8Unorm;  // Default to non-sRGB
}

/**
 * Parse texture dimension string to enum
 */
static WGPUTextureDimension stringToTextureDimension(const std::string& dim) {
    if (dim == "1d") return WGPUTextureDimension_1D;
    if (dim == "2d") return WGPUTextureDimension_2D;
    if (dim == "3d") return WGPUTextureDimension_3D;
    return WGPUTextureDimension_2D;  // Default
}

/**
 * Parse texture view dimension string to enum
 */
static WGPUTextureViewDimension stringToTextureViewDimension(const std::string& dim) {
    if (dim == "1d") return WGPUTextureViewDimension_1D;
    if (dim == "2d") return WGPUTextureViewDimension_2D;
    if (dim == "2d-array") return WGPUTextureViewDimension_2DArray;
    if (dim == "cube") return WGPUTextureViewDimension_Cube;
    if (dim == "cube-array") return WGPUTextureViewDimension_CubeArray;
    if (dim == "3d") return WGPUTextureViewDimension_3D;
    return WGPUTextureViewDimension_2D;  // Default
}

/**
 * Parse address mode string to enum
 */
static WGPUAddressMode stringToAddressMode(const std::string& mode) {
    if (mode == "clamp-to-edge") return WGPUAddressMode_ClampToEdge;
    if (mode == "repeat") return WGPUAddressMode_Repeat;
    if (mode == "mirror-repeat") return WGPUAddressMode_MirrorRepeat;
    return WGPUAddressMode_ClampToEdge;  // Default
}

/**
 * Parse filter mode string to enum
 */
static WGPUFilterMode stringToFilterMode(const std::string& mode) {
    if (mode == "nearest") return WGPUFilterMode_Nearest;
    if (mode == "linear") return WGPUFilterMode_Linear;
    return WGPUFilterMode_Nearest;  // Default
}

/**
 * Parse mipmap filter mode string to enum
 */
static WGPUMipmapFilterMode stringToMipmapFilterMode(const std::string& mode) {
    if (mode == "nearest") return WGPUMipmapFilterMode_Nearest;
    if (mode == "linear") return WGPUMipmapFilterMode_Linear;
    return WGPUMipmapFilterMode_Nearest;  // Default
}

/**
 * Parse compare function string to enum
 */
static WGPUCompareFunction stringToCompareFunction(const std::string& func) {
    if (func == "never") return WGPUCompareFunction_Never;
    if (func == "less") return WGPUCompareFunction_Less;
    if (func == "equal") return WGPUCompareFunction_Equal;
    if (func == "less-equal") return WGPUCompareFunction_LessEqual;
    if (func == "greater") return WGPUCompareFunction_Greater;
    if (func == "not-equal") return WGPUCompareFunction_NotEqual;
    if (func == "greater-equal") return WGPUCompareFunction_GreaterEqual;
    if (func == "always") return WGPUCompareFunction_Always;
    return WGPUCompareFunction_Undefined;  // Default (no comparison)
}

/**
 * Get the current swapchain texture (or offscreen texture in no-SDL mode)
 */
static WGPUTexture getCurrentSwapchainTexture() {
#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
    // In no-SDL mode, use the offscreen texture
    if (!g_surface) {
        if (g_offscreenTexture) {
            return g_offscreenTexture;
        }
        std::cerr << "[WebGPU] No surface and no offscreen texture available" << std::endl;
        return nullptr;
    }

    WGPUSurfaceTexture surfaceTexture;
    wgpuSurfaceGetCurrentTexture(g_surface, &surfaceTexture);

    if (!wgpuSurfaceTextureStatusIsSuccess(surfaceTexture.status)) {
        std::cerr << "[WebGPU] Failed to get current texture" << std::endl;
        return nullptr;
    }

    return surfaceTexture.texture;
#else
    return nullptr;
#endif
}

/**
 * Initialize WebGPU bindings in the JS engine
 */
bool initBindings(js::Engine* engine, void* wgpuInstance, void* wgpuAdapter, void* wgpuDevice, void* wgpuQueue, void* wgpuSurface, uint32_t surfaceFormat, uint32_t width, uint32_t height, bool debug) {
    if (!engine) {
        std::cerr << "[WebGPU] No JS engine provided for bindings" << std::endl;
        return false;
    }

#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
    // Set verbose logging based on debug flag
    g_verboseLogging = debug;

    g_engine = engine;
    g_instance = (WGPUInstance)wgpuInstance;
    g_adapter = (WGPUAdapter)wgpuAdapter;
    g_device = (WGPUDevice)wgpuDevice;
    g_queue = (WGPUQueue)wgpuQueue;
    g_surface = (WGPUSurface)wgpuSurface;

    if (!engine->evalScript(R"JS(
globalThis.__mystralCreateDeferred = function() {
    let resolve;
    let reject;
    const promise = new Promise(function(onResolve, onReject) {
        resolve = onResolve;
        reject = onReject;
    });
    return { promise, resolve, reject };
};
)JS", "webgpu-async.js")) {
        std::cerr << "[WebGPU] Failed to initialize async Promise support" << std::endl;
        return false;
    }

    // Set canvas dimensions from window size
    g_canvasWidth = width;
    g_canvasHeight = height;
    g_surfaceFormat = (WGPUTextureFormat)surfaceFormat;

    if (g_verboseLogging) {
        std::cout << "[WebGPU] Initializing JavaScript bindings..." << std::endl;
        std::cout << "[WebGPU] Surface format: " << surfaceFormat << std::endl;
    }

    // ========================================================================
    // Create a mock parent element for the canvas (needed by Debugger)
    // ========================================================================
    auto parentElement = engine->newObject();
    engine->setProperty(parentElement, "style", engine->newObject());
    engine->setProperty(parentElement, "appendChild",
        engine->newFunction("appendChild", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            // No-op in native runtime
            return args.empty() ? g_engine->newUndefined() : args[0];
        })
    );
    engine->setProperty(parentElement, "removeChild",
        engine->newFunction("removeChild", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return args.empty() ? g_engine->newUndefined() : args[0];
        })
    );

    // ========================================================================
    // Get existing canvas from runtime.cpp's document.getElementById
    // The canvas was created by setupDOMEvents() with addEventListener, style, etc.
    // We just need to add WebGPU-specific methods (getContext) to it.
    // ========================================================================
    auto existingDocument = engine->getGlobalProperty("document");
    auto getElementByIdFunc = engine->getProperty(existingDocument, "getElementById");

    // Call document.getElementById('canvas') to get the existing canvas
    std::vector<js::JSValueHandle> args;
    args.push_back(engine->newString("canvas"));
    auto canvasObject = engine->call(getElementByIdFunc, existingDocument, args);

    if (engine->isNull(canvasObject) || engine->isUndefined(canvasObject)) {
        std::cerr << "[WebGPU] Warning: No existing canvas found, creating new one" << std::endl;
        canvasObject = engine->newObject();
        engine->setProperty(canvasObject, "width", engine->newNumber(g_canvasWidth));
        engine->setProperty(canvasObject, "height", engine->newNumber(g_canvasHeight));
        engine->setProperty(canvasObject, "clientWidth", engine->newNumber(g_canvasWidth));
        engine->setProperty(canvasObject, "clientHeight", engine->newNumber(g_canvasHeight));
    }

    // Update canvas dimensions (in case they differ)
    engine->setProperty(canvasObject, "width", engine->newNumber(g_canvasWidth));
    engine->setProperty(canvasObject, "height", engine->newNumber(g_canvasHeight));
    engine->setProperty(canvasObject, "clientWidth", engine->newNumber(g_canvasWidth));
    engine->setProperty(canvasObject, "clientHeight", engine->newNumber(g_canvasHeight));

    // canvas.parentElement - mock parent element (for Debugger compatibility)
    engine->setProperty(canvasObject, "parentElement", parentElement);

    // canvas.getContext('webgpu') -> GPUCanvasContext
    // This is the WebGPU-specific method we add to the existing canvas
    engine->setProperty(canvasObject, "getContext",
        engine->newFunction("getContext", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                return g_engine->newNull();
            }

            std::string contextType = g_engine->toString(args[0]);

            // Handle Canvas 2D context
            if (contextType == "2d") {
                if (g_verboseLogging) std::cout << "[Canvas] Creating 2D context (" << g_canvasWidth << "x" << g_canvasHeight << ")" << std::endl;
                auto ctx2d = canvas::createCanvas2DContext(g_engine, g_canvasWidth, g_canvasHeight);

                // Set reference back to canvas
                auto canvas = g_engine->getGlobalProperty("canvas");
                g_engine->setProperty(ctx2d, "canvas", canvas);

                // Store the native context for Canvas 2D to WebGPU compositing
                g_mainCanvas2DContext = static_cast<canvas::Canvas2DContext*>(g_engine->getPrivateData(ctx2d));
                if (g_verboseLogging) std::cout << "[Canvas] Main canvas using 2D context - will composite to WebGPU" << std::endl;

                return ctx2d;
            }

            if (contextType != "webgpu") {
                std::cerr << "[Canvas] Unknown context type: " << contextType << std::endl;
                return g_engine->newNull();
            }

            // Create GPUCanvasContext
            auto canvasContext = g_engine->newObject();

            // Store reference to our surface
            g_engine->setPrivateData(canvasContext, g_surface);

            // context.canvas - reference back to canvas
            auto canvas = g_engine->getGlobalProperty("canvas");
            g_engine->setProperty(canvasContext, "canvas", canvas);

            // context.configure({ device, format, alphaMode })
            g_engine->setProperty(canvasContext, "configure",
                g_engine->newFunction("configure", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    if (args.empty()) {
                        g_engine->throwException("configure requires a descriptor");
                        return g_engine->newUndefined();
                    }

                    auto descriptor = args[0];

                    // Get format
                    std::string format = g_engine->toString(g_engine->getProperty(descriptor, "format"));
                    g_surfaceFormat = stringToFormat(format);
                    // Note: alphaMode and device are stored but surface is already configured

                    g_contextConfigured = true;
                    if (g_verboseLogging) std::cout << "[Canvas] Context configured with format: " << format << std::endl;

                    return g_engine->newUndefined();
                })
            );

            // context.unconfigure()
            g_engine->setProperty(canvasContext, "unconfigure",
                g_engine->newFunction("unconfigure", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    g_contextConfigured = false;
                    return g_engine->newUndefined();
                })
            );

            // context.getCurrentTexture() -> GPUTexture
            g_engine->setProperty(canvasContext, "getCurrentTexture",
                g_engine->newFunction("getCurrentTexture", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    // Get current swapchain texture
                    WGPUTexture texture = getCurrentSwapchainTexture();
                    if (!texture) {
                        g_engine->throwException("Failed to get current texture");
                        return g_engine->newUndefined();
                    }

                    g_currentTexture = texture;
                    static int frameCount = 0;
                    if (frameCount++ < 3) {
                        if (g_verboseLogging) std::cout << "[Canvas] Got texture: " << texture << std::endl;
                    }

                    // Register in texture registry so createView can find it
                    uint64_t textureId = g_nextTextureId++;
                    const bool ownsSurfaceReference = g_surface != nullptr;
                    g_textureRegistry[textureId] = {
                        texture, g_surfaceFormat, g_canvasWidth, g_canvasHeight, 1, 1,
                        WGPUTextureDimension_2D, ownsSurfaceReference, false
                    };
                    g_currentTextureId = textureId;

                    // Create JS wrapper for texture
                    auto jsTexture = g_engine->newObject();
                    g_engine->setPrivateData(jsTexture, texture);

                    // texture.width / height / depthOrArrayLayers
                    g_engine->setProperty(jsTexture, "width", g_engine->newNumber(g_canvasWidth));
                    g_engine->setProperty(jsTexture, "height", g_engine->newNumber(g_canvasHeight));
                    g_engine->setProperty(jsTexture, "depthOrArrayLayers", g_engine->newNumber(1));

                    // texture.format
                    g_engine->setProperty(jsTexture, "format", g_engine->newString(formatToString(g_surfaceFormat)));
                    g_engine->setProperty(jsTexture, "_textureId", g_engine->newNumber((double)textureId));

                    // texture.createView(descriptor?) -> GPUTextureView
                    // Capture textureId to look up the correct texture (not g_currentTexture which may change)
                    g_engine->setProperty(jsTexture, "createView",
                        g_engine->newFunction("createView", [textureId](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            // Look up texture from registry using captured textureId
                            auto it = g_textureRegistry.find(textureId);
                            if (it == g_textureRegistry.end()) {
                                std::cerr << "[WebGPU] Canvas createView: Texture " << textureId << " not found in registry" << std::endl;
                                g_engine->throwException("Texture not found in registry");
                                return g_engine->newUndefined();
                            }

                            WGPUTexture texture = it->second.texture;
                            if (!texture) {
                                g_engine->throwException("No current texture");
                                return g_engine->newUndefined();
                            }

                            // Create texture view
                            WGPUTextureViewDescriptor viewDesc = {};
                            viewDesc.format = it->second.format;
                            viewDesc.dimension = WGPUTextureViewDimension_2D;
                            viewDesc.baseMipLevel = 0;
                            viewDesc.mipLevelCount = 1;
                            viewDesc.baseArrayLayer = 0;
                            viewDesc.arrayLayerCount = 1;
                            viewDesc.aspect = WGPUTextureAspect_All;

                            WGPUTextureView view = wgpuTextureCreateView(texture, &viewDesc);

                            g_currentTextureView = view;
                            g_currentViewSourceTexture = texture;  // Track which texture the view was created from

                            // Create JS wrapper
                            auto jsView = g_engine->newObject();
                            g_engine->setPrivateData(jsView, view);
                            g_engine->setProperty(jsView, "_type", g_engine->newString("textureView"));

                            return jsView;
                        })
                    );

                    // texture.destroy()
                    g_engine->setProperty(jsTexture, "destroy",
                        g_engine->newFunction("destroy", [textureId](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            // Swapchain textures are managed by the surface, but remove from registry
                            g_textureRegistry.erase(textureId);
                            return g_engine->newUndefined();
                        })
                    );

                    return jsTexture;
                })
            );

            if (g_verboseLogging) std::cout << "[Canvas] WebGPU context created" << std::endl;
            return canvasContext;
        })
    );

    // Set global canvas - this is the SAME object as document.getElementById('canvas')
    // so it now has both WebGPU getContext AND event listener support
    engine->setGlobalProperty("canvas", canvasObject);

    // ========================================================================
    // Add missing methods to the existing document (from runtime.cpp)
    // We DON'T create a new document - just augment the existing one
    // ========================================================================

    // Add querySelector to existing document (if not present)
    engine->setProperty(existingDocument, "querySelector",
        engine->newFunction("querySelector", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            // Check if querying for canvas
            if (!args.empty()) {
                std::string selector = g_engine->toString(args[0]);
                if (selector == "canvas" || selector.find("canvas") != std::string::npos) {
                    return g_engine->getGlobalProperty("canvas");
                }
            }
            return g_engine->newNull();
        })
    );

    // Add createElement to existing document
    // NOTE: runtime.cpp sets up a createElement with canvas support (toDataURL) for @loaders.gl WebP detection
    // Preserve the runtime's text controls; this override adds Canvas 2D support.
    engine->setGlobalProperty("__mystralCreateElement",
        engine->getProperty(existingDocument, "createElement"));
    engine->setProperty(existingDocument, "createElement",
        engine->newFunction("createElement", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            // Get tag name if provided
            std::string tagName = "";
            if (!args.empty()) {
                tagName = g_engine->toString(args[0]);
            }

            if (tagName == "input" || tagName == "INPUT" ||
                tagName == "textarea" || tagName == "TEXTAREA") {
                auto fallback = g_engine->getGlobalProperty("__mystralCreateElement");
                auto document = g_engine->getGlobalProperty("document");
                if (g_engine->isFunction(fallback)) {
                    return g_engine->call(fallback, document, args);
                }
            }

            auto element = g_engine->newObject();

            // Add basic DOM element properties
            g_engine->setProperty(element, "style", g_engine->newObject());
            g_engine->setProperty(element, "className", g_engine->newString(""));
            g_engine->setProperty(element, "innerHTML", g_engine->newString(""));
            g_engine->setProperty(element, "textContent", g_engine->newString(""));
            g_engine->setProperty(element, "tagName", g_engine->newString(tagName.c_str()));
            g_engine->setProperty(element, "appendChild",
                g_engine->newFunction("appendChild", [](void* c, const std::vector<js::JSValueHandle>& a) {
                    return a.empty() ? g_engine->newUndefined() : a[0];
                })
            );
            g_engine->setProperty(element, "removeChild",
                g_engine->newFunction("removeChild", [](void* c, const std::vector<js::JSValueHandle>& a) {
                    return a.empty() ? g_engine->newUndefined() : a[0];
                })
            );
            g_engine->setProperty(element, "remove",
                g_engine->newFunction("remove", [](void* c, const std::vector<js::JSValueHandle>& a) {
                    // No-op in native runtime - element is not attached to DOM
                    return g_engine->newUndefined();
                })
            );
            g_engine->setProperty(element, "addEventListener",
                g_engine->newFunction("addEventListener", [](void* c, const std::vector<js::JSValueHandle>& a) {
                    // No-op in native runtime
                    return g_engine->newUndefined();
                })
            );
            g_engine->setProperty(element, "removeEventListener",
                g_engine->newFunction("removeEventListener", [](void* c, const std::vector<js::JSValueHandle>& a) {
                    return g_engine->newUndefined();
                })
            );

            // Special handling for canvas elements - add Canvas 2D support
            if (tagName == "canvas" || tagName == "CANVAS") {
                // Create OffscreenCanvas struct to store state
                int canvasId = g_nextOffscreenCanvasId++;
                auto offscreenCanvas = std::make_unique<OffscreenCanvas>();
                OffscreenCanvas* canvasPtr = offscreenCanvas.get();
                g_offscreenCanvases[canvasId] = std::move(offscreenCanvas);

                // Store the canvas ID as private data for getContext lookup
                g_engine->setPrivateData(element, reinterpret_cast<void*>(static_cast<intptr_t>(canvasId)));

                // Also store as property for debugging
                g_engine->setProperty(element, "_offscreenCanvasId", g_engine->newNumber(canvasId));

                // Default canvas dimensions (stored in struct)
                g_engine->setProperty(element, "width", g_engine->newNumber(canvasPtr->width));
                g_engine->setProperty(element, "height", g_engine->newNumber(canvasPtr->height));

                // Store reference to element globally so getContext can find it
                std::string globalName = "__offscreenCanvas_" + std::to_string(canvasId);
                g_engine->setGlobalProperty(globalName.c_str(), element);

                // Create getContext function
                // Capture canvasId to ensure each canvas element's getContext uses its own canvas
                // This fixes the bug where all canvases shared the same context
                auto getContextFn = g_engine->newFunction("getContext", [canvasId, canvasPtr](void* c, const std::vector<js::JSValueHandle>& contextArgs) {
                    if (contextArgs.empty()) {
                        return g_engine->newNull();
                    }

                    std::string contextType = g_engine->toString(contextArgs[0]);

                    // Use the captured canvasId to find the correct canvas
                    // This ensures each canvas element's getContext returns its own context
                    auto it = g_offscreenCanvases.find(canvasId);
                    if (it == g_offscreenCanvases.end()) {
                        std::cerr << "[Canvas] Canvas not found: " << canvasId << std::endl;
                        return g_engine->newNull();
                    }

                    OffscreenCanvas* canvas = it->second.get();

                    if (contextType == "2d") {
                        // Return cached context if already created
                        if (canvas->hasContext2d) {
                            return canvas->context2d;
                        }

                        // Get current dimensions from the canvas element (in case they were changed)
                        std::string globalName = "__offscreenCanvas_" + std::to_string(canvasId);
                        auto canvasElement = g_engine->getGlobalProperty(globalName.c_str());
                        if (!g_engine->isNull(canvasElement) && !g_engine->isUndefined(canvasElement)) {
                            auto widthProp = g_engine->getProperty(canvasElement, "width");
                            auto heightProp = g_engine->getProperty(canvasElement, "height");
                            if (!g_engine->isUndefined(widthProp)) {
                                canvas->width = static_cast<int>(g_engine->toNumber(widthProp));
                            }
                            if (!g_engine->isUndefined(heightProp)) {
                                canvas->height = static_cast<int>(g_engine->toNumber(heightProp));
                            }
                        }

                        // Create Canvas 2D context
                        if (g_verboseLogging) std::cout << "[Canvas] Creating offscreen 2D context (" << canvas->width << "x" << canvas->height << ")" << std::endl;
                        canvas->context2d = canvas::createCanvas2DContext(g_engine, canvas->width, canvas->height);
                        canvas->hasContext2d = true;
                        g_engine->protect(canvas->context2d);
                        return canvas->context2d;
                    }

                    if (contextType == "webgpu") {
                        // Create GPUCanvasContext for offscreen canvas
                        // This shares the main surface/device for simplicity
                        if (g_verboseLogging) std::cout << "[Canvas] Creating offscreen WebGPU context" << std::endl;

                        auto canvasContext = g_engine->newObject();

                        // Store reference to our surface
                        g_engine->setPrivateData(canvasContext, g_surface);

                        // context.canvas - reference back to canvas element
                        std::string globalName = "__offscreenCanvas_" + std::to_string(canvasId);
                        auto canvasElement = g_engine->getGlobalProperty(globalName.c_str());
                        g_engine->setProperty(canvasContext, "canvas", canvasElement);

                        // context.configure({ device, format, alphaMode })
                        g_engine->setProperty(canvasContext, "configure",
                            g_engine->newFunction("configure", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                if (args.empty()) {
                                    g_engine->throwException("configure requires a descriptor");
                                    return g_engine->newUndefined();
                                }
                                auto descriptor = args[0];
                                std::string format = g_engine->toString(g_engine->getProperty(descriptor, "format"));
                                g_surfaceFormat = stringToFormat(format);
                                g_contextConfigured = true;
                                if (g_verboseLogging) std::cout << "[Canvas] Offscreen context configured with format: " << format << std::endl;
                                return g_engine->newUndefined();
                            })
                        );

                        // context.unconfigure()
                        g_engine->setProperty(canvasContext, "unconfigure",
                            g_engine->newFunction("unconfigure", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                return g_engine->newUndefined();
                            })
                        );

                        // context.getCurrentTexture() -> GPUTexture
                        g_engine->setProperty(canvasContext, "getCurrentTexture",
                            g_engine->newFunction("getCurrentTexture", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                WGPUTexture texture = getCurrentSwapchainTexture();
                                if (!texture) {
                                    g_engine->throwException("Failed to get current texture");
                                    return g_engine->newUndefined();
                                }
                                g_currentTexture = texture;
                                if (g_verboseLogging) std::cout << "[Canvas] Offscreen got texture: " << (void*)texture << std::endl;

                                // Register in texture registry so createView can find it
                                uint64_t textureId = g_nextTextureId++;
                                const bool ownsSurfaceReference = g_surface != nullptr;
                                g_textureRegistry[textureId] = {
                                    texture, g_surfaceFormat, g_canvasWidth, g_canvasHeight, 1, 1,
                                    WGPUTextureDimension_2D, ownsSurfaceReference, false
                                };
                                g_currentTextureId = textureId;

                                // Create JS wrapper for texture
                                auto jsTexture = g_engine->newObject();
                                g_engine->setPrivateData(jsTexture, texture);

                                // texture.width / height / depthOrArrayLayers
                                g_engine->setProperty(jsTexture, "width", g_engine->newNumber(g_canvasWidth));
                                g_engine->setProperty(jsTexture, "height", g_engine->newNumber(g_canvasHeight));
                                g_engine->setProperty(jsTexture, "depthOrArrayLayers", g_engine->newNumber(1));

                                // texture.format
                                g_engine->setProperty(jsTexture, "format", g_engine->newString(formatToString(g_surfaceFormat)));
                                g_engine->setProperty(jsTexture, "_textureId", g_engine->newNumber((double)textureId));

                                // texture.createView(descriptor?) -> GPUTextureView
                                // Capture textureId to look up the correct texture (not g_currentTexture which may change)
                                g_engine->setProperty(jsTexture, "createView",
                                    g_engine->newFunction("createView", [textureId](void* c, const std::vector<js::JSValueHandle>& a) {
                                        // Look up texture from registry using captured textureId
                                        auto it = g_textureRegistry.find(textureId);
                                        if (it == g_textureRegistry.end()) {
                                            std::cerr << "[WebGPU] Offscreen createView: Texture " << textureId << " not found in registry" << std::endl;
                                            g_engine->throwException("Texture not found in registry");
                                            return g_engine->newUndefined();
                                        }

                                        WGPUTexture texture = it->second.texture;
                                        if (!texture) {
                                            g_engine->throwException("No current texture");
                                            return g_engine->newUndefined();
                                        }

                                        WGPUTextureViewDescriptor viewDesc = {};
                                        viewDesc.format = it->second.format;
                                        viewDesc.dimension = WGPUTextureViewDimension_2D;
                                        viewDesc.baseMipLevel = 0;
                                        viewDesc.mipLevelCount = 1;
                                        viewDesc.baseArrayLayer = 0;
                                        viewDesc.arrayLayerCount = 1;
                                        viewDesc.aspect = WGPUTextureAspect_All;

                                        WGPUTextureView view = wgpuTextureCreateView(texture, &viewDesc);
                                        g_currentTextureView = view;
                                        g_currentViewSourceTexture = texture;  // Track which texture the view was created from
                                        if (g_verboseLogging) std::cout << "[Canvas] Offscreen createView: texture=" << (void*)texture
                                                  << ", view=" << (void*)view << std::endl;

                                        auto jsView = g_engine->newObject();
                                        g_engine->setPrivateData(jsView, view);
                                        g_engine->setProperty(jsView, "_type", g_engine->newString("textureView"));

                                        return jsView;
                                    })
                                );

                                // texture.destroy()
                                g_engine->setProperty(jsTexture, "destroy",
                                    g_engine->newFunction("destroy", [textureId](void* c, const std::vector<js::JSValueHandle>& a) {
                                        // Swapchain textures are managed by the surface, but remove from registry
                                        g_textureRegistry.erase(textureId);
                                        return g_engine->newUndefined();
                                    })
                                );

                                return jsTexture;
                            })
                        );

                        return canvasContext;
                    }

                    // Ignore webgl requests silently (PixiJS feature detection)
                    if (contextType == "webgl" || contextType == "webgl2" || contextType == "experimental-webgl") {
                        return g_engine->newNull();
                    }

                    std::cerr << "[Canvas] Unsupported context type: " << contextType << std::endl;
                    return g_engine->newNull();
                });

                g_engine->setProperty(element, "getContext", getContextFn);
                if (g_verboseLogging) std::cout << "[Canvas] Created offscreen canvas " << canvasId << std::endl;

                // toDataURL for compatibility (returns empty data URI)
                g_engine->setProperty(element, "toDataURL",
                    g_engine->newFunction("toDataURL", [](void* c, const std::vector<js::JSValueHandle>& a) {
                        std::string mimeType = "image/png";
                        if (!a.empty()) {
                            mimeType = g_engine->toString(a[0]);
                        }
                        // Return a minimal data URI (for @loaders.gl WebP detection)
                        if (mimeType.find("webp") != std::string::npos) {
                            return g_engine->newString("data:image/webp;base64,");
                        }
                        return g_engine->newString("data:image/png;base64,");
                    })
                );

                // getBoundingClientRect - return canvas dimensions
                g_engine->setProperty(element, "getBoundingClientRect",
                    g_engine->newFunction("getBoundingClientRect", [](void* c, const std::vector<js::JSValueHandle>& a) {
                        // Get dimensions from the main canvas if available
                        auto rect = g_engine->newObject();
                        g_engine->setProperty(rect, "x", g_engine->newNumber(0));
                        g_engine->setProperty(rect, "y", g_engine->newNumber(0));
                        g_engine->setProperty(rect, "width", g_engine->newNumber(g_canvasWidth));
                        g_engine->setProperty(rect, "height", g_engine->newNumber(g_canvasHeight));
                        g_engine->setProperty(rect, "top", g_engine->newNumber(0));
                        g_engine->setProperty(rect, "left", g_engine->newNumber(0));
                        g_engine->setProperty(rect, "right", g_engine->newNumber(g_canvasWidth));
                        g_engine->setProperty(rect, "bottom", g_engine->newNumber(g_canvasHeight));
                        return rect;
                    })
                );
            }

            return element;
        })
    );

    // Add document.body if not present, or enhance existing body with required methods
    auto existingBody = engine->getProperty(existingDocument, "body");
    if (engine->isUndefined(existingBody) || engine->isNull(existingBody)) {
        existingBody = engine->newObject();
        engine->setProperty(existingDocument, "body", existingBody);
    }
    // Always add/update these methods on body
    engine->setProperty(existingBody, "style", engine->newObject());
    engine->setProperty(existingBody, "appendChild",
        engine->newFunction("appendChild", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return args.empty() ? g_engine->newUndefined() : args[0];
        })
    );
    engine->setProperty(existingBody, "removeChild",
        engine->newFunction("removeChild", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return args.empty() ? g_engine->newUndefined() : args[0];
        })
    );

    // ========================================================================
    // Navigator object
    // ========================================================================
    auto navigatorHandle = engine->getGlobalProperty("navigator");
    if (engine->isUndefined(navigatorHandle)) {
        navigatorHandle = engine->newObject();
        engine->setGlobalProperty("navigator", navigatorHandle);
    }

    // Add common navigator properties for browser compatibility
    // PixiJS and other libraries check these for feature detection
    engine->setProperty(navigatorHandle, "userAgent",
        engine->newString("Mozilla/5.0 (Macintosh; MystralNative/0.1) AppleWebKit/537.36 Chrome/120.0.0.0 Safari/537.36"));
    engine->setProperty(navigatorHandle, "platform", engine->newString("MystralNative"));
    engine->setProperty(navigatorHandle, "vendor", engine->newString("Mystral Engine"));
    engine->setProperty(navigatorHandle, "language", engine->newString("en-US"));
    engine->setProperty(navigatorHandle, "languages", engine->newArray(1));  // ["en-US"]
    engine->setProperty(navigatorHandle, "onLine", engine->newBoolean(true));
    engine->setProperty(navigatorHandle, "hardwareConcurrency", engine->newNumber(8));
    engine->setProperty(navigatorHandle, "maxTouchPoints", engine->newNumber(0));

    // Create navigator.gpu object
    auto gpuObject = engine->newObject();

    // ========================================================================
    // navigator.gpu.requestAdapter()
    // ========================================================================
    engine->setProperty(gpuObject, "requestAdapter",
        engine->newFunction("requestAdapter", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            // In native runtime, we already have an adapter, so just return a mock adapter object
            auto adapter = g_engine->newObject();

            // adapter.requestDevice()
            g_engine->setProperty(adapter, "requestDevice",
                g_engine->newFunction("requestDevice", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    // Return a device object wrapping our native device
                    auto device = g_engine->newObject();
                    g_engine->setPrivateData(device, g_device);

                    // device.queue
                    auto queue = g_engine->newObject();
                    g_engine->setPrivateData(queue, g_queue);

                    // queue.submit(commandBuffers)
                    g_engine->setProperty(queue, "submit",
                        g_engine->newFunction("submit", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::QueueSubmit);
                            if (args.empty()) {
                                return g_engine->newUndefined();
                            }

                            // Get command buffers array and submit them
                            auto cmdBuffersArray = args[0];

                            // Get array length
                            auto lengthProp = g_engine->getProperty(cmdBuffersArray, "length");
                            int length = (int)g_engine->toNumber(lengthProp);
                            g_engine->releaseValue(lengthProp);

                            // Collect command buffers
                            std::vector<WGPUCommandBuffer> cmdBuffers;
                            for (int i = 0; i < length; i++) {
                                auto cmdBufferHandle = g_engine->getPropertyIndex(cmdBuffersArray, i);
                                WGPUCommandBuffer cmdBuffer = (WGPUCommandBuffer)g_engine->getPrivateData(cmdBufferHandle);
                                if (cmdBuffer) {
                                    cmdBuffers.push_back(cmdBuffer);
                                }
                                g_engine->releaseValue(cmdBufferHandle);
                            }

                            // Preserve each logical submit boundary in the ordered work stream.
                            // beginDawnFrame/endDawnFrame collapse that stream into one native
                            // wgpuQueueSubmit while buffer uploads remain correctly interleaved.
                            static int submitCount = 0;
                            submitCount++;
                            if (!cmdBuffers.empty() && g_queue) {
                                const size_t commandBufferCount = cmdBuffers.size();
                                const bool submittedImmediately = !g_dawnFrameActive;
                                deferCommandBuffers(std::move(cmdBuffers));
                                if (submittedImmediately) {
#if defined(MYSTRAL_WEBGPU_DAWN)
                                    wgpuDeviceTick(g_device);
#elif defined(MYSTRAL_WEBGPU_WGPU)
                                    wgpuDevicePoll(g_device, false, nullptr);
#endif
                                }
                                if (g_verboseLogging) std::cout << "[WebGPU] Logical submit #" << submitCount << ": " << commandBufferCount << " command buffers, g_currentTexture=" << (void*)g_currentTexture << std::endl;
                            } else {
                                if (g_verboseLogging) std::cout << "[WebGPU] Logical submit #" << submitCount << ": EMPTY (length=" << length << "), g_currentTexture=" << (void*)g_currentTexture << std::endl;
                            }

                            // Copy texture to screenshot buffer ONLY when about to present
                            // This prevents capturing intermediate render passes (e.g., Three.js post-processing)
                            // Only capture when the surface render pass has ended, matching the present condition
                            // Also ensure we only capture once per frame (Three.js does multiple queue.submit() per frame)
                            WGPUTexture screenshotTexture = g_currentViewSourceTexture ? g_currentViewSourceTexture : g_currentTexture;
                            if (g_surfaceRenderPassEnded && !g_screenshotCapturedThisFrame && screenshotTexture && g_device && g_queue) {
                                // Calculate buffer requirements
                                uint32_t bytesPerPixel = 4;  // BGRA8
                                uint32_t unalignedBytesPerRow = g_canvasWidth * bytesPerPixel;
                                uint32_t bytesPerRow = (unalignedBytesPerRow + 255) & ~255;  // Align to 256
                                size_t requiredSize = bytesPerRow * g_canvasHeight;

                                // Create or resize screenshot buffer if needed
                                if (!g_screenshotBuffer || g_screenshotBufferSize < requiredSize) {
                                    if (g_screenshotBuffer) {
                                        wgpuBufferDestroy(g_screenshotBuffer);
                                        wgpuBufferRelease(g_screenshotBuffer);
                                    }

                                    WGPUBufferDescriptor bufferDesc = {};
                                    bufferDesc.size = requiredSize;
                                    bufferDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
                                    bufferDesc.mappedAtCreation = false;

                                    g_screenshotBuffer = wgpuDeviceCreateBuffer(g_device, &bufferDesc);
                                    g_screenshotBufferSize = requiredSize;
                                    g_screenshotBytesPerRow = bytesPerRow;
                                }

                                // Create encoder to copy texture to buffer
                                WGPUCommandEncoderDescriptor encDesc = {};
                                WGPUCommandEncoder copyEncoder = wgpuDeviceCreateCommandEncoder(g_device, &encDesc);

                                WGPUImageCopyTexture_Compat srcCopy = {};
                                srcCopy.texture = screenshotTexture;
                                srcCopy.mipLevel = 0;
                                srcCopy.origin = {0, 0, 0};
                                srcCopy.aspect = WGPUTextureAspect_All;

                                WGPUImageCopyBuffer_Compat dstCopy = {};
                                dstCopy.buffer = g_screenshotBuffer;
                                dstCopy.layout.offset = 0;
                                dstCopy.layout.bytesPerRow = bytesPerRow;
                                dstCopy.layout.rowsPerImage = g_canvasHeight;

                                WGPUExtent3D copySize = {g_canvasWidth, g_canvasHeight, 1};
                                wgpuCommandEncoderCopyTextureToBuffer(copyEncoder, &srcCopy, &dstCopy, &copySize);

                                if (g_verboseLogging) std::cout << "[Screenshot] Copying from texture " << (void*)screenshotTexture
                                          << " (format=" << g_surfaceFormat << ", size=" << g_canvasWidth << "x" << g_canvasHeight << ")" << std::endl;

                                WGPUCommandBufferDescriptor cmdDesc = {};
                                WGPUCommandBuffer copyCmd = wgpuCommandEncoderFinish(copyEncoder, &cmdDesc);
                                wgpuCommandEncoderRelease(copyEncoder);
                                if (copyCmd) {
                                    std::vector<WGPUCommandBuffer> screenshotCommands = {copyCmd};
                                    deferCommandBuffers(std::move(screenshotCommands));
                                }

                                // Outside the animation-frame scope, retain the previous eager
                                // completion behavior. Normal frames flush this copy in endDawnFrame.
                                if (!g_dawnFrameActive) {
                                    for (int syncIter = 0; syncIter < 100; syncIter++) {
#if defined(MYSTRAL_WEBGPU_DAWN)
                                        wgpuDeviceTick(g_device);
#elif defined(MYSTRAL_WEBGPU_WGPU)
                                        wgpuDevicePoll(g_device, false, nullptr);
#endif
                                        if (g_instance) {
                                            wgpuInstanceProcessEvents(g_instance);
                                        }
                                    }
                                }

                                g_screenshotReady = true;
                                g_screenshotCapturedThisFrame = true;
                            }

                            if (!g_dawnFrameActive) presentSurfaceIfReady();

                            return g_engine->newUndefined();
                        })
                    );

                    // queue.writeBuffer(buffer, offset, data, dataOffset?, size?)
                    //
                    // Spec conformance: when `data` is a TypedArray, the optional
                    // dataOffset/size arguments are ELEMENT counts, not bytes
                    // (https://www.w3.org/TR/webgpu/#dom-gpuqueue-writebuffer).
                    // three.js relies on this for partial attribute uploads — the
                    // previous byte interpretation shrank Float32Array writes 4×.
                    // The view's byteOffset must also be honored, so the copy is
                    // resolved against the view's underlying ArrayBuffer.
                    g_engine->setProperty(queue, "writeBuffer",
                        g_engine->newFunction("writeBuffer", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::QueueWriteBuffer);
                            if (args.size() < 3) {
                                g_engine->throwException("writeBuffer requires buffer, offset, and data");
                                return g_engine->newUndefined();
                            }

                            const auto* dataOffset = args.size() > 3 ? &args[3] : nullptr;
                            const auto* size = args.size() > 4 ? &args[4] : nullptr;
                            writeQueueBuffer(args[0], args[1], args[2], dataOffset, size,
                                ProfiledBridgeOp::QueueWriteBuffer, "writeBuffer");

                            return g_engine->newUndefined();
                        })
                    );

                    // Mystral extension: queue.writeBufferBatch(flatOperations)
                    // Each operation occupies five consecutive entries:
                    // [buffer, offset, data, dataOffset | undefined, size | undefined].
                    // The application can collect the writes issued between two
                    // submits and cross the JS/C++ bridge once without changing
                    // their queue order.
                    g_engine->setProperty(queue, "writeBufferBatch",
                        g_engine->newFunction("writeBufferBatch", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::QueueWriteBufferBatch);
                            if (args.empty()) {
                                g_engine->throwException("writeBufferBatch requires a flat operations array");
                                return g_engine->newUndefined();
                            }

                            auto operations = args[0];
                            auto lengthProp = g_engine->getProperty(operations, "length");
                            int length = (int)g_engine->toNumber(lengthProp);
                            g_engine->releaseValue(lengthProp);
                            if (length < 0 || length % 5 != 0) {
                                g_engine->throwException("writeBufferBatch operations length must be a multiple of 5");
                                return g_engine->newUndefined();
                            }

                            for (int index = 0; index < length; index += 5) {
                                auto buffer = g_engine->getPropertyIndex(operations, index);
                                auto offset = g_engine->getPropertyIndex(operations, index + 1);
                                auto data = g_engine->getPropertyIndex(operations, index + 2);
                                auto dataOffset = g_engine->getPropertyIndex(operations, index + 3);
                                auto size = g_engine->getPropertyIndex(operations, index + 4);
                                const bool written = writeQueueBuffer(buffer, offset, data, &dataOffset, &size,
                                    ProfiledBridgeOp::QueueWriteBufferBatch, "writeBufferBatch");
                                g_engine->releaseValue(buffer);
                                g_engine->releaseValue(offset);
                                g_engine->releaseValue(data);
                                g_engine->releaseValue(dataOffset);
                                g_engine->releaseValue(size);
                                if (!written) {
                                    break;
                                }
                            }

                            return g_engine->newUndefined();
                        })
                    );

                    // queue.writeTexture(destination, data, dataLayout, size)
                    g_engine->setProperty(queue, "writeTexture",
                        g_engine->newFunction("writeTexture", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::QueueWriteTexture);
                            if (args.size() < 4) {
                                g_engine->throwException("writeTexture requires destination, data, dataLayout, and size");
                                return g_engine->newUndefined();
                            }

                            // Parse destination {texture, mipLevel?, origin?, aspect?}
                            auto destination = args[0];
                            auto textureHandle = g_engine->getProperty(destination, "texture");
                            WGPUTexture texture = (WGPUTexture)g_engine->getPrivateData(textureHandle);

                            if (!texture) {
                                g_engine->throwException("writeTexture: invalid texture");
                                return g_engine->newUndefined();
                            }

                            auto mipLevelVal = g_engine->getProperty(destination, "mipLevel");
                            uint32_t mipLevel = g_engine->isUndefined(mipLevelVal) ? 0 : (uint32_t)g_engine->toNumber(mipLevelVal);

                            // Parse origin
                            auto originVal = g_engine->getProperty(destination, "origin");
                            uint32_t originX = 0, originY = 0, originZ = 0;
                            if (!g_engine->isUndefined(originVal)) {
                                auto lengthProp = g_engine->getProperty(originVal, "length");
                                if (!g_engine->isUndefined(lengthProp)) {
                                    // Array format
                                    int len = (int)g_engine->toNumber(lengthProp);
                                    if (len >= 1) originX = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(originVal, 0));
                                    if (len >= 2) originY = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(originVal, 1));
                                    if (len >= 3) originZ = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(originVal, 2));
                                } else {
                                    // Object format
                                    auto x = g_engine->getProperty(originVal, "x");
                                    auto y = g_engine->getProperty(originVal, "y");
                                    auto z = g_engine->getProperty(originVal, "z");
                                    if (!g_engine->isUndefined(x)) originX = (uint32_t)g_engine->toNumber(x);
                                    if (!g_engine->isUndefined(y)) originY = (uint32_t)g_engine->toNumber(y);
                                    if (!g_engine->isUndefined(z)) originZ = (uint32_t)g_engine->toNumber(z);
                                }
                            }

                            // Get ArrayBuffer data
                            size_t dataSize = 0;
                            void* dataPtr = g_engine->getArrayBufferData(args[1], &dataSize);

                            if (!dataPtr || dataSize == 0) {
                                g_engine->throwException("writeTexture: invalid data");
                                return g_engine->newUndefined();
                            }

                            // Parse size FIRST (need height for rowsPerImage default)
                            auto sizeVal = args[3];
                            uint32_t width = 1, height = 1, depthOrArrayLayers = 1;
                            auto lengthProp = g_engine->getProperty(sizeVal, "length");
                            if (!g_engine->isUndefined(lengthProp)) {
                                int len = (int)g_engine->toNumber(lengthProp);
                                if (len >= 1) width = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeVal, 0));
                                if (len >= 2) height = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeVal, 1));
                                if (len >= 3) depthOrArrayLayers = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeVal, 2));
                            } else {
                                auto w = g_engine->getProperty(sizeVal, "width");
                                auto h = g_engine->getProperty(sizeVal, "height");
                                auto d = g_engine->getProperty(sizeVal, "depthOrArrayLayers");
                                if (!g_engine->isUndefined(w)) width = (uint32_t)g_engine->toNumber(w);
                                if (!g_engine->isUndefined(h)) height = (uint32_t)g_engine->toNumber(h);
                                if (!g_engine->isUndefined(d)) depthOrArrayLayers = (uint32_t)g_engine->toNumber(d);
                            }

                            // Parse dataLayout {offset?, bytesPerRow, rowsPerImage?}
                            auto dataLayout = args[2];
                            auto layoutOffsetVal = g_engine->getProperty(dataLayout, "offset");
                            uint64_t layoutOffset = g_engine->isUndefined(layoutOffsetVal) ? 0 : (uint64_t)g_engine->toNumber(layoutOffsetVal);

                            uint32_t bytesPerRow = (uint32_t)g_engine->toNumber(g_engine->getProperty(dataLayout, "bytesPerRow"));

                            auto rowsPerImageVal = g_engine->getProperty(dataLayout, "rowsPerImage");
                            // rowsPerImage must be >= height for 2D textures (wgpu validation requirement)
                            uint32_t rowsPerImage = g_engine->isUndefined(rowsPerImageVal) ? height : (uint32_t)g_engine->toNumber(rowsPerImageVal);
                            if (rowsPerImage == 0) rowsPerImage = height;

                            // Create copy structures
                            WGPUImageCopyTexture_Compat destCopy = {};
                            destCopy.texture = texture;
                            destCopy.mipLevel = mipLevel;
                            destCopy.origin = {originX, originY, originZ};
                            destCopy.aspect = WGPUTextureAspect_All;

                            WGPUTextureDataLayout_Compat layout = {};
                            layout.offset = layoutOffset;
                            layout.bytesPerRow = bytesPerRow;
                            layout.rowsPerImage = rowsPerImage;

                            WGPUExtent3D copySize = {width, height, depthOrArrayLayers};

                            // Queue texture writes remain native queue operations. Flush earlier
                            // deferred work so their ordering relative to command buffers is exact.
                            if (g_dawnFrameActive && !g_deferredQueueWork.empty()) {
                                g_forcedFrameFlushes++;
                                flushDeferredQueueWork();
                            }
                            wgpuQueueWriteTexture(g_queue, &destCopy, (uint8_t*)dataPtr + layoutOffset, dataSize - layoutOffset, &layout, &copySize);
                            recordBridgeBytes(ProfiledBridgeOp::QueueWriteTexture, dataSize - layoutOffset);

                            if (g_verboseLogging) std::cout << "[WebGPU] writeTexture: " << width << "x" << height << " (" << dataSize << " bytes)" << std::endl;

                            return g_engine->newUndefined();
                        })
                    );

                    // queue.copyExternalImageToTexture(source, destination, copySize)
                    // Standard WebGPU way to upload ImageBitmap to texture
                    g_engine->setProperty(queue, "copyExternalImageToTexture",
                        g_engine->newFunction("copyExternalImageToTexture", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (args.size() < 3) {
                                g_engine->throwException("copyExternalImageToTexture requires source, destination, and copySize");
                                return g_engine->newUndefined();
                            }

                            // Parse source (ImageBitmap-like object or canvas element)
                            auto source = args[0];
                            auto sourceObj = g_engine->getProperty(source, "source");
                            if (g_engine->isUndefined(sourceObj)) {
                                sourceObj = source; // source might be passed directly
                            }

                            // Parse flipY option (default false per WebGPU spec)
                            bool flipY = false;
                            auto flipYProp = g_engine->getProperty(source, "flipY");
                            if (!g_engine->isUndefined(flipYProp)) {
                                flipY = g_engine->toBoolean(flipYProp);
                            }

                            // Parse destination.premultipliedAlpha (default false per WebGPU spec).
                            // PixiJS sets this true so its NORMAL blend (ONE, ONE_MINUS_SRC_ALPHA)
                            // produces correct results. Our PNG decoder returns straight alpha, so
                            // when the destination requests premultiplied we must multiply RGB by
                            // A/255 on the fly. Without this, pixels with 0<a<255 and color>0 render
                            // too bright / with color halos.
                            bool premultipliedAlpha = false;
                            {
                                auto premulProp = g_engine->getProperty(args[1], "premultipliedAlpha");
                                if (!g_engine->isUndefined(premulProp)) {
                                    premultipliedAlpha = g_engine->toBoolean(premulProp);
                                }
                            }

                            int imgWidth = 0;
                            int imgHeight = 0;
                            size_t dataSize = 0;
                            void* dataPtr = nullptr;

                            // Try to get data from ImageBitmap
                            auto imageData = g_engine->getProperty(sourceObj, "_data");
                            if (!g_engine->isUndefined(imageData)) {
                                // Standard ImageBitmap with _data
                                imgWidth = (int)g_engine->toNumber(g_engine->getProperty(sourceObj, "width"));
                                imgHeight = (int)g_engine->toNumber(g_engine->getProperty(sourceObj, "height"));
                                dataPtr = g_engine->getArrayBufferData(imageData, &dataSize);
                            } else {
                                // Check if it's a canvas element
                                auto tagName = g_engine->getProperty(sourceObj, "tagName");
                                std::string tagNameStr = g_engine->isUndefined(tagName) ? "" : g_engine->toString(tagName);

                                if (tagNameStr == "CANVAS" || tagNameStr == "canvas") {
                                    // Get the canvas ID from private data or property
                                    auto canvasIdProp = g_engine->getProperty(sourceObj, "_offscreenCanvasId");
                                    if (!g_engine->isUndefined(canvasIdProp)) {
                                        int canvasId = (int)g_engine->toNumber(canvasIdProp);
                                        auto it = g_offscreenCanvases.find(canvasId);
                                        if (it != g_offscreenCanvases.end() && it->second->hasContext2d) {
                                            // Get pixel data from the 2D context
                                            auto ctx2dHandle = it->second->context2d;
                                            auto nativeCtx = static_cast<canvas::Canvas2DContext*>(g_engine->getPrivateData(ctx2dHandle));
                                            if (nativeCtx) {
                                                imgWidth = it->second->width;
                                                imgHeight = it->second->height;
                                                dataPtr = const_cast<void*>(static_cast<const void*>(nativeCtx->getPixelData()));
                                                dataSize = nativeCtx->getPixelDataSize();
                                            }
                                        }
                                    }
                                }

                                // Check if it's already a 2D context (has getImageData method or _contextType)
                                auto contextType = g_engine->getProperty(sourceObj, "_contextType");
                                if (!g_engine->isUndefined(contextType)) {
                                    std::string ctxTypeStr = g_engine->toString(contextType);
                                    if (ctxTypeStr == "2d") {
                                        // It's a 2D context, get the canvas and then get pixel data
                                        auto canvas = g_engine->getProperty(sourceObj, "canvas");
                                        if (!g_engine->isUndefined(canvas)) {
                                            auto canvasIdProp = g_engine->getProperty(canvas, "_offscreenCanvasId");
                                            if (!g_engine->isUndefined(canvasIdProp)) {
                                                int canvasId = (int)g_engine->toNumber(canvasIdProp);
                                                auto it = g_offscreenCanvases.find(canvasId);
                                                if (it != g_offscreenCanvases.end() && it->second->hasContext2d) {
                                                    auto nativeCtx = static_cast<canvas::Canvas2DContext*>(g_engine->getPrivateData(sourceObj));
                                                    if (nativeCtx) {
                                                        imgWidth = it->second->width;
                                                        imgHeight = it->second->height;
                                                        dataPtr = const_cast<void*>(static_cast<const void*>(nativeCtx->getPixelData()));
                                                        dataSize = nativeCtx->getPixelDataSize();
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            if (!dataPtr || dataSize == 0) {
                                // Try to get width/height anyway for better error message
                                auto widthProp = g_engine->getProperty(sourceObj, "width");
                                auto heightProp = g_engine->getProperty(sourceObj, "height");
                                if (!g_engine->isUndefined(widthProp)) imgWidth = (int)g_engine->toNumber(widthProp);
                                if (!g_engine->isUndefined(heightProp)) imgHeight = (int)g_engine->toNumber(heightProp);

                                std::cerr << "[WebGPU] copyExternalImageToTexture: unsupported source type, width=" << imgWidth << ", height=" << imgHeight << std::endl;
                                // Return silently instead of throwing - PixiJS might be able to continue
                                return g_engine->newUndefined();
                            }

                            // Parse destination
                            auto destination = args[1];
                            auto textureObj = g_engine->getProperty(destination, "texture");
                            WGPUTexture texture = (WGPUTexture)g_engine->getPrivateData(textureObj);
                            if (!texture) {
                                g_engine->throwException("copyExternalImageToTexture: invalid texture");
                                return g_engine->newUndefined();
                            }

                            // Detect the destination texture format. In a real browser,
                            // copyExternalImageToTexture converts the source's RGBA pixels into the
                            // destination's format; we upload bytes verbatim via writeTexture, so for
                            // BGRA8 destinations we must swap the R/B channels ourselves. Our
                            // ImageBitmap data is always RGBA (stb_image / WebPDecodeRGBA), but PixiJS
                            // v8's TextureSource.defaultOptions.format is "bgra8unorm", so every
                            // Texture.from(imageBitmap) lands here — without the swap, red and blue
                            // come out transposed.
                            bool swapRB = false;
                            {
                                auto fmtProp = g_engine->getProperty(textureObj, "format");
                                if (!g_engine->isUndefined(fmtProp)) {
                                    std::string fmt = g_engine->toString(fmtProp);
                                    swapRB = (fmt == "bgra8unorm" || fmt == "bgra8unorm-srgb");
                                }
                            }

                            // Optional mipLevel and origin
                            uint32_t mipLevel = 0;
                            auto mipLevelVal = g_engine->getProperty(destination, "mipLevel");
                            if (!g_engine->isUndefined(mipLevelVal)) {
                                mipLevel = (uint32_t)g_engine->toNumber(mipLevelVal);
                            }

                            uint32_t originX = 0, originY = 0, originZ = 0;
                            auto originVal = g_engine->getProperty(destination, "origin");
                            if (!g_engine->isUndefined(originVal)) {
                                if (g_engine->isArray(originVal)) {
                                    originX = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(originVal, 0));
                                    originY = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(originVal, 1));
                                    originZ = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(originVal, 2));
                                }
                            }

                            // Parse copySize
                            auto sizeVal = args[2];
                            uint32_t width = imgWidth, height = imgHeight, depthOrArrayLayers = 1;
                            if (g_engine->isArray(sizeVal)) {
                                width = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeVal, 0));
                                height = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeVal, 1));
                                auto depthVal = g_engine->getPropertyIndex(sizeVal, 2);
                                if (!g_engine->isUndefined(depthVal)) {
                                    depthOrArrayLayers = (uint32_t)g_engine->toNumber(depthVal);
                                }
                            } else if (!g_engine->isUndefined(sizeVal)) {
                                auto widthVal = g_engine->getProperty(sizeVal, "width");
                                auto heightVal = g_engine->getProperty(sizeVal, "height");
                                if (!g_engine->isUndefined(widthVal)) width = (uint32_t)g_engine->toNumber(widthVal);
                                if (!g_engine->isUndefined(heightVal)) height = (uint32_t)g_engine->toNumber(heightVal);
                            }

                            // Handle flipY, premultipliedAlpha, and/or BGRA channel swap by writing
                            // into a staging copy. RGBA8 only (matches the hardcoded bytesPerRow below).
                            std::vector<uint8_t> stagingData;
                            void* uploadDataPtr = dataPtr;
                            if ((flipY || premultipliedAlpha || swapRB) && dataPtr && imgHeight > 0 && imgWidth > 0) {
                                size_t bytesPerRow = (size_t)imgWidth * 4;
                                stagingData.resize(dataSize);
                                const uint8_t* srcData = static_cast<const uint8_t*>(dataPtr);
                                for (int y = 0; y < imgHeight; y++) {
                                    const uint8_t* srcRow = srcData + (flipY ? (imgHeight - 1 - y) : y) * bytesPerRow;
                                    uint8_t* dstRow = stagingData.data() + (size_t)y * bytesPerRow;
                                    if (premultipliedAlpha || swapRB) {
                                        for (int x = 0; x < imgWidth; x++) {
                                            uint32_t r = srcRow[x * 4 + 0];
                                            uint32_t g = srcRow[x * 4 + 1];
                                            uint32_t b = srcRow[x * 4 + 2];
                                            uint32_t a = srcRow[x * 4 + 3];
                                            if (premultipliedAlpha) {
                                                // (v * a + 127) / 255 rounds correctly without a divide instruction
                                                r = (r * a + 127) / 255;
                                                g = (g * a + 127) / 255;
                                                b = (b * a + 127) / 255;
                                            }
                                            // BGRA8 destinations read byte 0 as B and byte 2 as R, so emit
                                            // the channels swapped; RGBA8 destinations get them in order.
                                            dstRow[x * 4 + 0] = (uint8_t)(swapRB ? b : r);
                                            dstRow[x * 4 + 1] = (uint8_t)g;
                                            dstRow[x * 4 + 2] = (uint8_t)(swapRB ? r : b);
                                            dstRow[x * 4 + 3] = (uint8_t)a;
                                        }
                                    } else {
                                        std::memcpy(dstRow, srcRow, bytesPerRow);
                                    }
                                }
                                uploadDataPtr = stagingData.data();
                                if (g_verboseLogging) {
                                    std::cout << "[WebGPU] copyExternalImageToTexture: "
                                              << (flipY ? "flipY " : "")
                                              << (premultipliedAlpha ? "premultiplyAlpha " : "")
                                              << (swapRB ? "swapRB" : "")
                                              << std::endl;
                                }
                            }

                            // Use writeTexture internally (same effect as copyExternalImageToTexture)
                            WGPUImageCopyTexture_Compat destCopy = {};
                            destCopy.texture = texture;
                            destCopy.mipLevel = mipLevel;
                            destCopy.origin = {originX, originY, originZ};
                            destCopy.aspect = WGPUTextureAspect_All;

                            WGPUTextureDataLayout_Compat layout = {};
                            layout.offset = 0;
                            layout.bytesPerRow = imgWidth * 4;  // RGBA
                            layout.rowsPerImage = imgHeight;

                            WGPUExtent3D copySize = {width, height, depthOrArrayLayers};

                            if (g_dawnFrameActive && !g_deferredQueueWork.empty()) {
                                g_forcedFrameFlushes++;
                                flushDeferredQueueWork();
                            }
                            wgpuQueueWriteTexture(g_queue, &destCopy, uploadDataPtr, dataSize, &layout, &copySize);

                            if (g_verboseLogging) std::cout << "[WebGPU] copyExternalImageToTexture: " << width << "x" << height << (flipY ? " (flipY)" : "") << std::endl;

                            return g_engine->newUndefined();
                        })
                    );

                    // queue.onSubmittedWorkDone() - returns Promise that resolves when GPU work is done
                    g_engine->setProperty(queue, "onSubmittedWorkDone",
                        g_engine->newFunction("onSubmittedWorkDone", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (g_dawnFrameActive && !g_deferredQueueWork.empty()) {
                                g_forcedFrameFlushes++;
                                flushDeferredQueueWork();
                            }
#if defined(MYSTRAL_WEBGPU_DAWN)
                            js::JSValueHandle promise;
                            auto* pending = createPendingPromise(promise);
                            if (!pending) return g_engine->newUndefined();

                            WGPUQueueWorkDoneCallbackInfo callbackInfo = {};
                            callbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
                            callbackInfo.callback = onQueueWorkDone;
                            callbackInfo.userdata1 = pending;
                            wgpuQueueOnSubmittedWorkDone(g_queue, callbackInfo);
                            return promise;
#else
                            // Compatibility fallback for the legacy wgpu-native API.
                            return g_engine->evalWithResult("Promise.resolve()", "<onSubmittedWorkDone>");
#endif
                        })
                    );

                    g_engine->setProperty(device, "queue", queue);

                    // The runtime owns one native device for the lifetime of
                    // the process. A JS renderer may dispose its wrapper on a
                    // full hot reload, but must not destroy that shared device.
                    g_engine->setProperty(device, "destroy",
                        g_engine->newFunction("destroy", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            return g_engine->newUndefined();
                        })
                    );

                    // device.limits - reflect the REAL limits of the live device
                    // (it is created with the adapter's full limits in context.cpp;
                    // a hardcoded subset here starved JS of the compute limits)
                    auto deviceLimits = g_engine->newObject();
                    WGPULimits realDeviceLimits = {};
                    if (queryDeviceLimits(&realDeviceLimits)) {
                        setLimitsProperties(deviceLimits, realDeviceLimits);
                    }
                    g_engine->setProperty(device, "limits", deviceLimits);

                    // device.features - reflect what the live device actually has
                    // (queried through wgpuDeviceHasFeature, not hardcoded)
                    g_engine->setProperty(device, "features", buildFeaturesObject(deviceHasFeature));

                    // device.createBuffer(descriptor)
                    g_engine->setProperty(device, "createBuffer",
                        g_engine->newFunction("createBuffer", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (args.empty()) {
                                g_engine->throwException("createBuffer requires a descriptor");
                                return g_engine->newUndefined();
                            }

                            auto descriptor = args[0];
                            double size = g_engine->toNumber(g_engine->getProperty(descriptor, "size"));
                            double usage = g_engine->toNumber(g_engine->getProperty(descriptor, "usage"));

                            // Check for mappedAtCreation
                            auto mappedAtCreationProp = g_engine->getProperty(descriptor, "mappedAtCreation");
                            bool mappedAtCreation = !g_engine->isUndefined(mappedAtCreationProp) && g_engine->toBoolean(mappedAtCreationProp);

                            WGPUBufferDescriptor bufferDesc = {};
                            bufferDesc.size = (uint64_t)size;
                            bufferDesc.usage = (WGPUBufferUsage)(uint32_t)usage;
                            bufferDesc.mappedAtCreation = mappedAtCreation;

                            WGPUBuffer buffer = wgpuDeviceCreateBuffer(g_device, &bufferDesc);
                            if (!buffer) {
                                g_engine->throwException("Failed to create buffer");
                                return g_engine->newUndefined();
                            }

                            // Register buffer for mapping operations
                            uint64_t bufferId = g_nextBufferId++;
                            // mappedAtCreation buffers are mapped for write
                            WGPUMapMode initialMapMode = mappedAtCreation ? WGPUMapMode_Write : WGPUMapMode_None;
                            g_bufferRegistry[bufferId] = {buffer, (uint64_t)size, (WGPUBufferUsage)(uint32_t)usage, mappedAtCreation, nullptr, 0, initialMapMode, false};

                            auto jsBuffer = g_engine->newObject();
                            g_engine->setPrivateData(jsBuffer, buffer);
                            g_engine->setProperty(jsBuffer, "size", g_engine->newNumber(size));
                            g_engine->setProperty(jsBuffer, "_bufferId", g_engine->newNumber((double)bufferId));
                            g_engine->setProperty(jsBuffer, "usage", g_engine->newNumber(usage));

                            // Set initial mapState
                            g_engine->setProperty(jsBuffer, "mapState", g_engine->newString(mappedAtCreation ? "mapped" : "unmapped"));

                            // buffer.mapAsync(mode, offset?, size?) -> Promise
                            // Returns a Promise that resolves when the buffer is mapped
                            g_engine->setProperty(jsBuffer, "mapAsync",
                                g_engine->newFunction("mapAsync", [bufferId](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                    auto it = g_bufferRegistry.find(bufferId);
                                    if (it == g_bufferRegistry.end()) {
                                        std::cerr << "[WebGPU] mapAsync: Buffer " << bufferId << " not found" << std::endl;
                                        return rejectedPromise("GPUBuffer not found");
                                    }

                                    auto& bufferInfo = it->second;

                                    // Already mapped (mappedAtCreation)?
                                    if (bufferInfo.isMapped) {
                                        return rejectedPromise("GPUBuffer is already mapped");
                                    }
                                    if (bufferInfo.mapPending) {
                                        return rejectedPromise("GPUBuffer already has a pending mapAsync operation");
                                    }

                                    // Get mode (default to READ)
                                    WGPUMapMode mode = WGPUMapMode_Read;
                                    if (!args.empty()) {
                                        uint32_t jsMode = (uint32_t)g_engine->toNumber(args[0]);
                                        // GPUMapMode.READ = 1, GPUMapMode.WRITE = 2
                                        if (jsMode == 2) mode = WGPUMapMode_Write;
                                    }

                                    uint64_t offset = args.size() > 1 ? (uint64_t)g_engine->toNumber(args[1]) : 0;
                                    if (offset > bufferInfo.size) {
                                        return rejectedPromise("GPUBuffer map range is out of bounds");
                                    }
                                    uint64_t mapSize = args.size() > 2
                                        ? (uint64_t)g_engine->toNumber(args[2])
                                        : bufferInfo.size - offset;
                                    if (mapSize > bufferInfo.size - offset) {
                                        return rejectedPromise("GPUBuffer map range is out of bounds");
                                    }

                                    // Debug: Log buffer info
                                    bool hasMapRead = (bufferInfo.usage & WGPUBufferUsage_MapRead) != 0;
                                    (void)hasMapRead;  // Used for debug logging when enabled

                                    // mapAsync is an execution boundary: submitting a command
                                    // buffer that references a buffer after mapping has started is
                                    // invalid. Readbacks therefore become an intentional extra
                                    // native submit instead of waiting for endDawnFrame.
                                    if (g_dawnFrameActive && !g_deferredQueueWork.empty()) {
                                        g_forcedFrameFlushes++;
                                        flushDeferredQueueWork();
                                    }

                                    js::JSValueHandle promise;
                                    auto* pending = createPendingPromise(promise);
                                    if (!pending) {
                                        return g_engine->newUndefined();
                                    }

                                    bufferInfo.mapPending = true;
                                    auto* context = new BufferMapAsyncContext{pending, bufferId, mode};
#if defined(MYSTRAL_WEBGPU_DAWN)
                                    WGPUBufferMapCallbackInfo callbackInfo = {};
                                    callbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
                                    callbackInfo.callback = onBufferMapped;
                                    callbackInfo.userdata1 = context;
                                    wgpuBufferMapAsync(bufferInfo.buffer, mode, offset, mapSize, callbackInfo);
#else
                                    wgpuBufferMapAsync(
                                        bufferInfo.buffer,
                                        mode,
                                        offset,
                                        mapSize,
                                        onBufferMapped,
                                        context);
#endif
                                    return promise;
                                })
                            );

                            // buffer.getMappedRange(offset?, size?) -> ArrayBuffer
                            // Capture bufferId in closure to identify the correct buffer
                            g_engine->setProperty(jsBuffer, "getMappedRange",
                                g_engine->newFunction("getMappedRange", [bufferId](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                    // Look up this specific buffer by its ID
                                    auto it = g_bufferRegistry.find(bufferId);
                                    if (it == g_bufferRegistry.end()) {
                                        std::cerr << "[WebGPU] getMappedRange: Buffer " << bufferId << " not found in registry" << std::endl;
                                        return g_engine->newUndefined();
                                    }

                                    auto& bufferInfo = it->second;

                                    if (!bufferInfo.isMapped && !bufferInfo.mappedData) {
                                        if (g_verboseLogging) std::cerr << "[WebGPU] getMappedRange: Buffer " << bufferId << " is not mapped" << std::endl;
                                        return g_engine->newUndefined();
                                    }

                                    uint64_t offset = args.empty() ? 0 : (uint64_t)g_engine->toNumber(args[0]);
                                    uint64_t rangeSize = args.size() > 1 ? (uint64_t)g_engine->toNumber(args[1]) : bufferInfo.size - offset;

                                    // Use wgpuBufferGetConstMappedRange for MAP_READ, wgpuBufferGetMappedRange for MAP_WRITE
                                    // Dawn requires the const version for read-only mapped buffers
                                    const void* mappedData = nullptr;
                                    if (bufferInfo.mapMode == WGPUMapMode_Read) {
                                        mappedData = wgpuBufferGetConstMappedRange(bufferInfo.buffer, offset, rangeSize);
                                    } else {
                                        mappedData = wgpuBufferGetMappedRange(bufferInfo.buffer, offset, rangeSize);
                                    }

                                    if (mappedData) {
                                        // Use newArrayBufferExternal to avoid copying
                                        // Cast away const for read-only buffers - the JS side shouldn't modify but we need void*
                                        return g_engine->newArrayBufferExternal(const_cast<void*>(mappedData), rangeSize);
                                    }

                                    if (g_verboseLogging) std::cerr << "[WebGPU] getMappedRange: GetMappedRange returned null for buffer " << bufferId << std::endl;
                                    return g_engine->newUndefined();
                                })
                            );

                            // buffer.unmap()
                            // Capture bufferId in closure to identify the correct buffer
                            g_engine->setProperty(jsBuffer, "unmap",
                                g_engine->newFunction("unmap", [bufferId](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                    // Look up this specific buffer by its ID
                                    auto it = g_bufferRegistry.find(bufferId);
                                    if (it == g_bufferRegistry.end()) {
                                        std::cerr << "[WebGPU] unmap: Buffer " << bufferId << " not found in registry" << std::endl;
                                        return g_engine->newUndefined();
                                    }

                                    auto& bufferInfo = it->second;
                                    if (bufferInfo.isMapped) {
                                        wgpuBufferUnmap(bufferInfo.buffer);
                                        bufferInfo.isMapped = false;
                                        bufferInfo.mappedData = nullptr;
                                        bufferInfo.mappedSize = 0;
                                    }
                                    return g_engine->newUndefined();
                                })
                            );

                            // buffer.destroy()
                            // Capture bufferId in closure to identify the correct buffer
                            g_engine->setProperty(jsBuffer, "destroy",
                                g_engine->newFunction("destroy", [bufferId](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                    if (g_dawnFrameActive && !g_deferredQueueWork.empty()) {
                                        g_forcedFrameFlushes++;
                                        flushDeferredQueueWork();
                                    }
                                    releaseBuffer(bufferId, true);
                                    return g_engine->newUndefined();
                                })
                            );

                            if (gcReleaseEnabled()) {
                                g_engine->registerRelease(jsBuffer, [bufferId]() {
                                    releaseBuffer(bufferId, false);
                                });
                            }

                            return jsBuffer;
                        })
                    );

                    // device.createShaderModule(descriptor)
                    g_engine->setProperty(device, "createShaderModule",
                        g_engine->newFunction("createShaderModule", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (args.empty()) {
                                g_engine->throwException("createShaderModule requires a descriptor");
                                return g_engine->newUndefined();
                            }

                            auto descriptor = args[0];
                            std::string code = g_engine->toString(g_engine->getProperty(descriptor, "code"));

                            // Debug: Print first 500 chars of shader code
                            if (g_verboseLogging && code.length() > 0) {
                                std::cout << "[Shader] Creating shader (" << code.length() << " chars):\n"
                                          << code.substr(0, std::min((size_t)500, code.length()))
                                          << (code.length() > 500 ? "\n..." : "") << std::endl;
                            }

                            WGPUShaderModuleWGSLDescriptor_Compat wgslDesc = {};
                            WGPUShaderModuleDescriptor shaderDesc = {};
                            setupShaderModuleWGSL(&shaderDesc, &wgslDesc, code.c_str());

                            WGPUShaderModule shaderModule = wgpuDeviceCreateShaderModule(g_device, &shaderDesc);

                            auto jsShader = g_engine->newObject();
                            g_engine->setPrivateData(jsShader, shaderModule);
                            if (gcReleaseEnabled()) {
                                g_engine->registerRelease(jsShader, [shaderModule]() {
                                    wgpuShaderModuleRelease(shaderModule);
                                });
                            }

                            return jsShader;
                        })
                    );

                    // device.createRenderPipeline(descriptor)
                    auto createRenderPipelineBinding = [](bool asyncCreation) {
                        return g_engine->newFunction("createRenderPipeline", [asyncCreation](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (args.empty()) {
                                g_engine->throwException("createRenderPipeline requires a descriptor");
                                return g_engine->newUndefined();
                            }

                            auto descriptor = args[0];

                            // Get vertex stage. entryPoint is OPTIONAL per spec — when
                            // omitted (three.js r183+ does this) the implementation must
                            // select the module's single entry point; a zero-init/null
                            // entryPoint means exactly that in both backends.
                            auto vertex = g_engine->getProperty(descriptor, "vertex");
                            auto vertexModule = g_engine->getProperty(vertex, "module");
                            auto vertexEntryProp = g_engine->getProperty(vertex, "entryPoint");
                            bool hasVertexEntry =
                                !g_engine->isUndefined(vertexEntryProp) && !g_engine->isNull(vertexEntryProp);
                            std::string vertexEntry = hasVertexEntry ? g_engine->toString(vertexEntryProp) : std::string();

                            // Get fragment stage (optional - depth-only pipelines don't have fragment)
                            auto fragment = g_engine->getProperty(descriptor, "fragment");
                            WGPUShaderModule fsModule = nullptr;
                            std::string fragmentEntry;
                            bool hasFragmentEntry = false;
                            bool hasFragment = !g_engine->isUndefined(fragment) && !g_engine->isNull(fragment);
                            if (hasFragment) {
                                auto fragmentModule = g_engine->getProperty(fragment, "module");
                                fsModule = (WGPUShaderModule)g_engine->getPrivateData(fragmentModule);
                                auto fragEntryProp = g_engine->getProperty(fragment, "entryPoint");
                                hasFragmentEntry =
                                    !g_engine->isUndefined(fragEntryProp) && !g_engine->isNull(fragEntryProp);
                                if (hasFragmentEntry) {
                                    fragmentEntry = g_engine->toString(fragEntryProp);
                                }
                            }

                            // Get native shader modules
                            WGPUShaderModule vsModule = (WGPUShaderModule)g_engine->getPrivateData(vertexModule);

                            // Create pipeline descriptor
                            WGPURenderPipelineDescriptor pipelineDesc = {};

                            // Check for layout property
                            auto layoutProp = g_engine->getProperty(descriptor, "layout");
                            if (!g_engine->isUndefined(layoutProp)) {
                                // Check if it's "auto" string or a PipelineLayout object
                                if (g_engine->isString(layoutProp)) {
                                    std::string layoutStr = g_engine->toString(layoutProp);
                                    if (layoutStr == "auto") {
                                        pipelineDesc.layout = nullptr;  // Auto layout
                                    }
                                } else {
                                    // It's a PipelineLayout object
                                    WGPUPipelineLayout layout = (WGPUPipelineLayout)g_engine->getPrivateData(layoutProp);
                                    pipelineDesc.layout = layout;
                                }
                            }

                            // Vertex state
                            pipelineDesc.vertex.module = vsModule;
                            if (hasVertexEntry) {
                                WGPU_SET_ENTRY_POINT(pipelineDesc.vertex, vertexEntry.c_str());
                            } else {
                                WGPU_SET_ENTRY_POINT_AUTO(pipelineDesc.vertex);
                            }

                            // Parse vertex buffers if present
                            std::vector<WGPUVertexBufferLayout> vertexBuffers;
                            std::vector<std::vector<WGPUVertexAttribute>> allAttributes; // Keep attributes alive

                            auto buffersArray = g_engine->getProperty(vertex, "buffers");
                            if (!g_engine->isUndefined(buffersArray)) {
                                auto buffersLen = g_engine->getProperty(buffersArray, "length");
                                int bufferCount = (int)g_engine->toNumber(buffersLen);

                                for (int i = 0; i < bufferCount; i++) {
                                    auto buffer = g_engine->getPropertyIndex(buffersArray, i);

                                    WGPUVertexBufferLayout layout = {};
                                    layout.arrayStride = (uint64_t)g_engine->toNumber(g_engine->getProperty(buffer, "arrayStride"));
                                    layout.stepMode = WGPUVertexStepMode_Vertex;

                                    // Parse step mode if present
                                    auto stepModeProp = g_engine->getProperty(buffer, "stepMode");
                                    if (!g_engine->isUndefined(stepModeProp)) {
                                        std::string stepModeStr = g_engine->toString(stepModeProp);
                                        if (stepModeStr == "instance") {
                                            layout.stepMode = WGPUVertexStepMode_Instance;
                                        }
                                    }

                                    // Parse attributes
                                    auto attrsArray = g_engine->getProperty(buffer, "attributes");
                                    if (!g_engine->isUndefined(attrsArray)) {
                                        auto attrsLen = g_engine->getProperty(attrsArray, "length");
                                        int attrCount = (int)g_engine->toNumber(attrsLen);

                                        std::vector<WGPUVertexAttribute> attributes;
                                        for (int j = 0; j < attrCount; j++) {
                                            auto attr = g_engine->getPropertyIndex(attrsArray, j);

                                            WGPUVertexAttribute va = {};
                                            va.shaderLocation = (uint32_t)g_engine->toNumber(g_engine->getProperty(attr, "shaderLocation"));
                                            va.offset = (uint64_t)g_engine->toNumber(g_engine->getProperty(attr, "offset"));

                                            std::string formatStr = g_engine->toString(g_engine->getProperty(attr, "format"));
                                            // Parse vertex format
                                            if (formatStr == "float32") va.format = WGPUVertexFormat_Float32;
                                            else if (formatStr == "float32x2") va.format = WGPUVertexFormat_Float32x2;
                                            else if (formatStr == "float32x3") va.format = WGPUVertexFormat_Float32x3;
                                            else if (formatStr == "float32x4") va.format = WGPUVertexFormat_Float32x4;
                                            else if (formatStr == "uint8x2") va.format = WGPUVertexFormat_Uint8x2;
                                            else if (formatStr == "uint8x4") va.format = WGPUVertexFormat_Uint8x4;
                                            else if (formatStr == "sint8x2") va.format = WGPUVertexFormat_Sint8x2;
                                            else if (formatStr == "sint8x4") va.format = WGPUVertexFormat_Sint8x4;
                                            else if (formatStr == "unorm8x2") va.format = WGPUVertexFormat_Unorm8x2;
                                            else if (formatStr == "unorm8x4") va.format = WGPUVertexFormat_Unorm8x4;
                                            else if (formatStr == "snorm8x2") va.format = WGPUVertexFormat_Snorm8x2;
                                            else if (formatStr == "snorm8x4") va.format = WGPUVertexFormat_Snorm8x4;
                                            else if (formatStr == "uint16x2") va.format = WGPUVertexFormat_Uint16x2;
                                            else if (formatStr == "uint16x4") va.format = WGPUVertexFormat_Uint16x4;
                                            else if (formatStr == "sint16x2") va.format = WGPUVertexFormat_Sint16x2;
                                            else if (formatStr == "sint16x4") va.format = WGPUVertexFormat_Sint16x4;
                                            else if (formatStr == "unorm16x2") va.format = WGPUVertexFormat_Unorm16x2;
                                            else if (formatStr == "unorm16x4") va.format = WGPUVertexFormat_Unorm16x4;
                                            else if (formatStr == "snorm16x2") va.format = WGPUVertexFormat_Snorm16x2;
                                            else if (formatStr == "snorm16x4") va.format = WGPUVertexFormat_Snorm16x4;
                                            else if (formatStr == "float16x2") va.format = WGPUVertexFormat_Float16x2;
                                            else if (formatStr == "float16x4") va.format = WGPUVertexFormat_Float16x4;
                                            else if (formatStr == "uint32") va.format = WGPUVertexFormat_Uint32;
                                            else if (formatStr == "uint32x2") va.format = WGPUVertexFormat_Uint32x2;
                                            else if (formatStr == "uint32x3") va.format = WGPUVertexFormat_Uint32x3;
                                            else if (formatStr == "uint32x4") va.format = WGPUVertexFormat_Uint32x4;
                                            else if (formatStr == "sint32") va.format = WGPUVertexFormat_Sint32;
                                            else if (formatStr == "sint32x2") va.format = WGPUVertexFormat_Sint32x2;
                                            else if (formatStr == "sint32x3") va.format = WGPUVertexFormat_Sint32x3;
                                            else if (formatStr == "sint32x4") va.format = WGPUVertexFormat_Sint32x4;
                                            else va.format = WGPUVertexFormat_Float32x3; // Default

                                            attributes.push_back(va);
                                        }

                                        allAttributes.push_back(attributes);
                                        layout.attributeCount = attributes.size();
                                        layout.attributes = allAttributes.back().data();
                                    }

                                    vertexBuffers.push_back(layout);
                                }

                                pipelineDesc.vertex.bufferCount = vertexBuffers.size();
                                pipelineDesc.vertex.buffers = vertexBuffers.data();
                            }

                            // Fragment state (only if fragment shader exists)
                            WGPUColorTargetState colorTarget = {};
                            WGPUFragmentState fragmentState = {};
                            std::vector<WGPUColorTargetState> colorTargets;
                            bool targetsExplicitlySpecified = false;
                            if (hasFragment && fsModule) {
                                // Parse targets from fragment descriptor
                                auto targetsProp = g_engine->getProperty(fragment, "targets");
                                if (!g_engine->isUndefined(targetsProp)) {
                                    targetsExplicitlySpecified = true;  // Even if empty array
                                    auto targetsLen = g_engine->getProperty(targetsProp, "length");
                                    int targetCount = (int)g_engine->toNumber(targetsLen);
                                    for (int i = 0; i < targetCount; i++) {
                                        auto target = g_engine->getPropertyIndex(targetsProp, i);
                                        WGPUColorTargetState targetState = {};

                                        auto formatProp = g_engine->getProperty(target, "format");
                                        if (!g_engine->isUndefined(formatProp)) {
                                            std::string formatStr = g_engine->toString(formatProp);
                                            targetState.format = stringToFormat(formatStr);
                                            if (targetCount >= 5) {
                                                if (g_verboseLogging) std::cout << "[WebGPU] Pipeline target " << i << ": format=" << formatStr << " (enum=" << targetState.format << ")" << std::endl;
                                            }
                                        } else {
                                            targetState.format = g_surfaceFormat;
                                        }
                                        targetState.writeMask = WGPUColorWriteMask_All;

                                        // Parse blend state if provided
                                        auto blendProp = g_engine->getProperty(target, "blend");
                                        if (!g_engine->isUndefined(blendProp)) {
                                            // Store blend state in a persistent container
                                            static std::vector<std::unique_ptr<WGPUBlendState>> blendStates;
                                            auto blendState = std::make_unique<WGPUBlendState>();

                                            // Helper lambda to parse blend factor
                                            auto parseBlendFactor = [](const std::string& str) -> WGPUBlendFactor {
                                                if (str == "zero") return WGPUBlendFactor_Zero;
                                                if (str == "one") return WGPUBlendFactor_One;
                                                if (str == "src") return WGPUBlendFactor_Src;
                                                if (str == "one-minus-src") return WGPUBlendFactor_OneMinusSrc;
                                                if (str == "src-alpha") return WGPUBlendFactor_SrcAlpha;
                                                if (str == "one-minus-src-alpha") return WGPUBlendFactor_OneMinusSrcAlpha;
                                                if (str == "dst") return WGPUBlendFactor_Dst;
                                                if (str == "one-minus-dst") return WGPUBlendFactor_OneMinusDst;
                                                if (str == "dst-alpha") return WGPUBlendFactor_DstAlpha;
                                                if (str == "one-minus-dst-alpha") return WGPUBlendFactor_OneMinusDstAlpha;
                                                if (str == "src-alpha-saturated") return WGPUBlendFactor_SrcAlphaSaturated;
                                                if (str == "constant") return WGPUBlendFactor_Constant;
                                                if (str == "one-minus-constant") return WGPUBlendFactor_OneMinusConstant;
                                                return WGPUBlendFactor_One;  // Default
                                            };

                                            // Helper lambda to parse blend operation
                                            auto parseBlendOp = [](const std::string& str) -> WGPUBlendOperation {
                                                if (str == "add") return WGPUBlendOperation_Add;
                                                if (str == "subtract") return WGPUBlendOperation_Subtract;
                                                if (str == "reverse-subtract") return WGPUBlendOperation_ReverseSubtract;
                                                if (str == "min") return WGPUBlendOperation_Min;
                                                if (str == "max") return WGPUBlendOperation_Max;
                                                return WGPUBlendOperation_Add;  // Default
                                            };

                                            // Parse color blend component
                                            auto colorProp = g_engine->getProperty(blendProp, "color");
                                            if (!g_engine->isUndefined(colorProp)) {
                                                auto srcFactor = g_engine->getProperty(colorProp, "srcFactor");
                                                auto dstFactor = g_engine->getProperty(colorProp, "dstFactor");
                                                auto operation = g_engine->getProperty(colorProp, "operation");
                                                if (!g_engine->isUndefined(srcFactor))
                                                    blendState->color.srcFactor = parseBlendFactor(g_engine->toString(srcFactor));
                                                else
                                                    blendState->color.srcFactor = WGPUBlendFactor_One;
                                                if (!g_engine->isUndefined(dstFactor))
                                                    blendState->color.dstFactor = parseBlendFactor(g_engine->toString(dstFactor));
                                                else
                                                    blendState->color.dstFactor = WGPUBlendFactor_Zero;
                                                if (!g_engine->isUndefined(operation))
                                                    blendState->color.operation = parseBlendOp(g_engine->toString(operation));
                                                else
                                                    blendState->color.operation = WGPUBlendOperation_Add;
                                            } else {
                                                // Default color blend (no blending)
                                                blendState->color.srcFactor = WGPUBlendFactor_One;
                                                blendState->color.dstFactor = WGPUBlendFactor_Zero;
                                                blendState->color.operation = WGPUBlendOperation_Add;
                                            }

                                            // Parse alpha blend component
                                            auto alphaProp = g_engine->getProperty(blendProp, "alpha");
                                            if (!g_engine->isUndefined(alphaProp)) {
                                                auto srcFactor = g_engine->getProperty(alphaProp, "srcFactor");
                                                auto dstFactor = g_engine->getProperty(alphaProp, "dstFactor");
                                                auto operation = g_engine->getProperty(alphaProp, "operation");
                                                if (!g_engine->isUndefined(srcFactor))
                                                    blendState->alpha.srcFactor = parseBlendFactor(g_engine->toString(srcFactor));
                                                else
                                                    blendState->alpha.srcFactor = WGPUBlendFactor_One;
                                                if (!g_engine->isUndefined(dstFactor))
                                                    blendState->alpha.dstFactor = parseBlendFactor(g_engine->toString(dstFactor));
                                                else
                                                    blendState->alpha.dstFactor = WGPUBlendFactor_Zero;
                                                if (!g_engine->isUndefined(operation))
                                                    blendState->alpha.operation = parseBlendOp(g_engine->toString(operation));
                                                else
                                                    blendState->alpha.operation = WGPUBlendOperation_Add;
                                            } else {
                                                // Default alpha blend (no blending)
                                                blendState->alpha.srcFactor = WGPUBlendFactor_One;
                                                blendState->alpha.dstFactor = WGPUBlendFactor_Zero;
                                                blendState->alpha.operation = WGPUBlendOperation_Add;
                                            }

                                            targetState.blend = blendState.get();
                                            blendStates.push_back(std::move(blendState));

                                            if (g_verboseLogging) std::cout << "[WebGPU] Pipeline target " << i << " has blend state" << std::endl;
                                        }

                                        colorTargets.push_back(targetState);
                                    }
                                }
                                // Only add default target if targets wasn't explicitly specified
                                // If targets: [] was specified, don't add any (depth-only pass)
                                if (colorTargets.empty() && !targetsExplicitlySpecified) {
                                    // Default single target only when targets is not specified at all
                                    colorTarget.format = g_surfaceFormat;
                                    colorTarget.writeMask = WGPUColorWriteMask_All;
                                    colorTargets.push_back(colorTarget);
                                }

                                fragmentState.module = fsModule;
                                if (hasFragmentEntry) {
                                    WGPU_SET_ENTRY_POINT(fragmentState, fragmentEntry.c_str());
                                } else {
                                    WGPU_SET_ENTRY_POINT_AUTO(fragmentState);
                                }
                                fragmentState.targetCount = colorTargets.size();
                                fragmentState.targets = colorTargets.data();
                                pipelineDesc.fragment = &fragmentState;
                                if (g_verboseLogging) std::cout << "[WebGPU] Render pipeline with " << colorTargets.size() << " color targets" << std::endl;
                            } else {
                                // Depth-only pipeline - no fragment state
                                pipelineDesc.fragment = nullptr;
                            }

                            // Primitive state
                            pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
                            pipelineDesc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
                            pipelineDesc.primitive.frontFace = WGPUFrontFace_CCW;
                            pipelineDesc.primitive.cullMode = WGPUCullMode_None;

                            // Parse primitive state if provided
                            auto primitiveProp = g_engine->getProperty(descriptor, "primitive");
                            if (!g_engine->isUndefined(primitiveProp)) {
                                auto topologyProp = g_engine->getProperty(primitiveProp, "topology");
                                if (!g_engine->isUndefined(topologyProp)) {
                                    std::string topologyStr = g_engine->toString(topologyProp);
                                    if (topologyStr == "point-list") pipelineDesc.primitive.topology = WGPUPrimitiveTopology_PointList;
                                    else if (topologyStr == "line-list") pipelineDesc.primitive.topology = WGPUPrimitiveTopology_LineList;
                                    else if (topologyStr == "line-strip") pipelineDesc.primitive.topology = WGPUPrimitiveTopology_LineStrip;
                                    else if (topologyStr == "triangle-list") pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
                                    else if (topologyStr == "triangle-strip") pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
                                }
                                auto cullModeProp = g_engine->getProperty(primitiveProp, "cullMode");
                                if (!g_engine->isUndefined(cullModeProp)) {
                                    std::string cullModeStr = g_engine->toString(cullModeProp);
                                    if (cullModeStr == "none") pipelineDesc.primitive.cullMode = WGPUCullMode_None;
                                    else if (cullModeStr == "front") pipelineDesc.primitive.cullMode = WGPUCullMode_Front;
                                    else if (cullModeStr == "back") pipelineDesc.primitive.cullMode = WGPUCullMode_Back;
                                }
                                auto frontFaceProp = g_engine->getProperty(primitiveProp, "frontFace");
                                if (!g_engine->isUndefined(frontFaceProp)) {
                                    std::string frontFaceStr = g_engine->toString(frontFaceProp);
                                    if (frontFaceStr == "ccw") pipelineDesc.primitive.frontFace = WGPUFrontFace_CCW;
                                    else if (frontFaceStr == "cw") pipelineDesc.primitive.frontFace = WGPUFrontFace_CW;
                                }
                            }

                            // Depth stencil state
                            WGPUDepthStencilState depthStencilState = {};
                            bool hasDepthStencil = false;

                            auto depthStencilProp = g_engine->getProperty(descriptor, "depthStencil");
                            if (!g_engine->isUndefined(depthStencilProp)) {
                                hasDepthStencil = true;

                                auto formatProp = g_engine->getProperty(depthStencilProp, "format");
                                if (!g_engine->isUndefined(formatProp)) {
                                    depthStencilState.format = stringToFormat(g_engine->toString(formatProp));
                                } else {
                                    depthStencilState.format = WGPUTextureFormat_Depth24Plus;
                                }

                                auto depthWriteEnabledProp = g_engine->getProperty(depthStencilProp, "depthWriteEnabled");
                                depthStencilState.depthWriteEnabled = g_engine->isUndefined(depthWriteEnabledProp)
                                    ? WGPU_OPTIONAL_BOOL_TRUE
                                    : (g_engine->toBoolean(depthWriteEnabledProp) ? WGPU_OPTIONAL_BOOL_TRUE : WGPU_OPTIONAL_BOOL_FALSE);

                                auto depthCompareProp = g_engine->getProperty(depthStencilProp, "depthCompare");
                                if (!g_engine->isUndefined(depthCompareProp)) {
                                    std::string compareStr = g_engine->toString(depthCompareProp);
                                    if (compareStr == "never") depthStencilState.depthCompare = WGPUCompareFunction_Never;
                                    else if (compareStr == "less") depthStencilState.depthCompare = WGPUCompareFunction_Less;
                                    else if (compareStr == "less-equal") depthStencilState.depthCompare = WGPUCompareFunction_LessEqual;
                                    else if (compareStr == "greater") depthStencilState.depthCompare = WGPUCompareFunction_Greater;
                                    else if (compareStr == "greater-equal") depthStencilState.depthCompare = WGPUCompareFunction_GreaterEqual;
                                    else if (compareStr == "equal") depthStencilState.depthCompare = WGPUCompareFunction_Equal;
                                    else if (compareStr == "not-equal") depthStencilState.depthCompare = WGPUCompareFunction_NotEqual;
                                    else if (compareStr == "always") depthStencilState.depthCompare = WGPUCompareFunction_Always;
                                } else {
                                    depthStencilState.depthCompare = WGPUCompareFunction_Less;
                                }

                                // Default stencil operations
                                depthStencilState.stencilFront.compare = WGPUCompareFunction_Always;
                                depthStencilState.stencilFront.failOp = WGPUStencilOperation_Keep;
                                depthStencilState.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
                                depthStencilState.stencilFront.passOp = WGPUStencilOperation_Keep;
                                depthStencilState.stencilBack = depthStencilState.stencilFront;
                                depthStencilState.stencilReadMask = 0xFFFFFFFF;
                                depthStencilState.stencilWriteMask = 0xFFFFFFFF;

                                pipelineDesc.depthStencil = &depthStencilState;
                            }

                            // Multisample state - parse from descriptor or use defaults
                            pipelineDesc.multisample.count = 1;
                            pipelineDesc.multisample.mask = 0xFFFFFFFF;
                            pipelineDesc.multisample.alphaToCoverageEnabled = false;

                            auto multisampleProp = g_engine->getProperty(descriptor, "multisample");
                            if (!g_engine->isUndefined(multisampleProp)) {
                                auto countProp = g_engine->getProperty(multisampleProp, "count");
                                if (!g_engine->isUndefined(countProp)) {
                                    pipelineDesc.multisample.count = (uint32_t)g_engine->toNumber(countProp);
                                }

                                auto maskProp = g_engine->getProperty(multisampleProp, "mask");
                                if (!g_engine->isUndefined(maskProp)) {
                                    pipelineDesc.multisample.mask = (uint32_t)g_engine->toNumber(maskProp);
                                }

                                auto alphaToCoverageProp = g_engine->getProperty(multisampleProp, "alphaToCoverageEnabled");
                                if (!g_engine->isUndefined(alphaToCoverageProp)) {
                                    pipelineDesc.multisample.alphaToCoverageEnabled = g_engine->toBoolean(alphaToCoverageProp);
                                }

                                if (g_verboseLogging) {
                                    std::cout << "[WebGPU] Render pipeline multisample: count=" << pipelineDesc.multisample.count
                                              << ", mask=" << pipelineDesc.multisample.mask << std::endl;
                                }
                            }

#if defined(MYSTRAL_WEBGPU_DAWN)
                            if (asyncCreation) {
                                js::JSValueHandle promise;
                                auto* pending = createPendingPromise(promise);
                                if (!pending) return g_engine->newUndefined();

                                WGPUCreateRenderPipelineAsyncCallbackInfo callbackInfo = {};
                                callbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
                                callbackInfo.callback = onRenderPipelineCreated;
                                callbackInfo.userdata1 = pending;
                                wgpuDeviceCreateRenderPipelineAsync(g_device, &pipelineDesc, callbackInfo);
                                return promise;
                            }
#else
                            (void)asyncCreation;
#endif

                            WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(g_device, &pipelineDesc);
                            if (!pipeline) {
                                g_engine->throwException("Failed to create render pipeline");
                                return g_engine->newUndefined();
                            }
                            auto jsPipeline = wrapRenderPipeline(pipeline);
                            return asyncCreation ? resolvedPromise(jsPipeline) : jsPipeline;
                        });
                    };
                    g_engine->setProperty(device, "createRenderPipeline", createRenderPipelineBinding(false));
                    g_engine->setProperty(device, "createRenderPipelineAsync", createRenderPipelineBinding(true));

                    // device.createComputePipeline(descriptor)
                    auto createComputePipelineBinding = [](bool asyncCreation) {
                        return g_engine->newFunction("createComputePipeline", [asyncCreation](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (args.empty()) {
                                g_engine->throwException("createComputePipeline requires a descriptor");
                                return g_engine->newUndefined();
                            }

                            auto descriptor = args[0];

                            // Get layout
                            auto layoutProp = g_engine->getProperty(descriptor, "layout");
                            WGPUPipelineLayout layout = nullptr;
                            bool isAutoLayout = false;
                            if (!g_engine->isUndefined(layoutProp) && !g_engine->isString(layoutProp)) {
                                layout = (WGPUPipelineLayout)g_engine->getPrivateData(layoutProp);
                            } else if (g_engine->isString(layoutProp)) {
                                std::string layoutStr = g_engine->toString(layoutProp);
                                if (layoutStr == "auto") {
                                    isAutoLayout = true;
                                    if (g_verboseLogging) std::cout << "[WebGPU] Using 'auto' layout for compute pipeline" << std::endl;
                                    std::cout.flush();
                                }
                            }

                            // Get compute stage
                            auto computeProp = g_engine->getProperty(descriptor, "compute");
                            auto moduleProp = g_engine->getProperty(computeProp, "module");
                            WGPUShaderModule module = (WGPUShaderModule)g_engine->getPrivateData(moduleProp);

                            // Entry point (default "main")
                            // entryPoint is OPTIONAL per spec — zero-init/null selects
                            // the module's single entry point
                            auto entryPointProp = g_engine->getProperty(computeProp, "entryPoint");
                            bool hasEntryPoint =
                                !g_engine->isUndefined(entryPointProp) && !g_engine->isNull(entryPointProp);
                            std::string entryPoint = hasEntryPoint ? g_engine->toString(entryPointProp) : std::string();

                            // Create pipeline
                            WGPUComputePipelineDescriptor pipelineDesc = {};
                            pipelineDesc.layout = layout;
                            pipelineDesc.compute.module = module;
                            if (hasEntryPoint) {
                                WGPU_SET_ENTRY_POINT(pipelineDesc.compute, entryPoint.c_str());
                            } else {
                                WGPU_SET_ENTRY_POINT_AUTO(pipelineDesc.compute);
                            }

#if defined(MYSTRAL_WEBGPU_DAWN)
                            if (asyncCreation) {
                                js::JSValueHandle promise;
                                auto* pending = createPendingPromise(promise);
                                if (!pending) return g_engine->newUndefined();

                                WGPUCreateComputePipelineAsyncCallbackInfo callbackInfo = {};
                                callbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
                                callbackInfo.callback = onComputePipelineCreated;
                                callbackInfo.userdata1 = pending;
                                wgpuDeviceCreateComputePipelineAsync(g_device, &pipelineDesc, callbackInfo);
                                return promise;
                            }
#else
                            (void)asyncCreation;
#endif

                            WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(g_device, &pipelineDesc);
                            if (!pipeline) {
                                g_engine->throwException("Failed to create compute pipeline");
                                return g_engine->newUndefined();
                            }
                            auto jsPipeline = wrapComputePipeline(pipeline);
                            return asyncCreation ? resolvedPromise(jsPipeline) : jsPipeline;
                        });
                    };
                    g_engine->setProperty(device, "createComputePipeline", createComputePipelineBinding(false));
                    g_engine->setProperty(device, "createComputePipelineAsync", createComputePipelineBinding(true));

                    // device.createCommandEncoder(descriptor?)
                    g_engine->setProperty(device, "createCommandEncoder",
                        g_engine->newFunction("createCommandEncoder", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::CreateCommandEncoder);
                            WGPUCommandEncoderDescriptor desc = {};
                            WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(g_device, &desc);
                            g_liveCommandEncoders.insert(encoder);
                            g_commandEncodersCreated++;

                            // Store in global for use by beginRenderPass
                            // Note: Multiple encoders are supported via per-encoder render pass tracking
                            g_jsCommandEncoder = encoder;

                            auto jsEncoder = g_engine->newObject();
                            g_engine->setPrivateData(jsEncoder, encoder);
                            // Command encoders have an explicit terminal owner:
                            // finish() releases the native handle. Registering a
                            // weak callback here lets V8 re-enter Dawn during GC,
                            // in the middle of long compute-heavy boot frames.

                            // encoder.beginRenderPass(descriptor)
                            g_engine->setProperty(jsEncoder, "beginRenderPass",
                                sharedMethod(g_transientMethods.encoderBeginRenderPass, "beginRenderPass",
                                    [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                    ScopedBridgeMeasurement measurement(ProfiledBridgeOp::BeginRenderPass);
                                    if (args.empty()) {
                                        g_engine->throwException("beginRenderPass requires a descriptor");
                                        return g_engine->newUndefined();
                                    }

                                    WGPUCommandEncoder encoderToUse =
                                        (WGPUCommandEncoder)g_engine->getPrivateData(receiver);
                                    if (!encoderToUse) {
                                        g_engine->throwException("Command encoder not available");
                                        return g_engine->newUndefined();
                                    }

                                    auto descriptor = args[0];
                                    auto colorAttachments = g_engine->getProperty(descriptor, "colorAttachments");

                                    // Parse all color attachments (deferred renderer uses multiple)
                                    auto attachmentsLengthProp = g_engine->getProperty(colorAttachments, "length");
                                    int numAttachments = g_engine->isUndefined(attachmentsLengthProp) ? 0 : (int)g_engine->toNumber(attachmentsLengthProp);
                                    std::vector<WGPURenderPassColorAttachment> colorAttachmentList;
                                    colorAttachmentList.reserve(numAttachments);

                                    double firstR = 0, firstG = 0, firstB = 0, firstA = 1;

                                    for (int i = 0; i < numAttachments; i++) {
                                        auto attachment = g_engine->getPropertyIndex(colorAttachments, i);
                                        auto viewHandle = g_engine->getProperty(attachment, "view");
                                        WGPUTextureView view = (WGPUTextureView)g_engine->getPrivateData(viewHandle);

                                        // Debug: Log first color attachment for comparison with g_currentTextureView
                                        if (i == 0) {
                                            if (g_verboseLogging) {
                                                std::cout << "[WebGPU] Render pass attachment[0]: view=" << (void*)view
                                                          << ", g_currentTextureView=" << (void*)g_currentTextureView
                                                          << ", matches=" << (view == g_currentTextureView ? "YES" : "NO") << std::endl;
                                            }

                                            // Track if this render pass uses the surface texture
                                            if (view == g_currentTextureView && g_currentTextureView != nullptr) {
                                                g_surfaceRenderEncoder = encoderToUse;
                                                g_surfaceRenderPassEnded = false;
                                            }
                                        }

                                        // Debug: Log GBuffer pass attachments
                                        if (numAttachments >= 5 && i == 0) {
                                            if (g_verboseLogging) std::cout << "[WebGPU] GBuffer pass - 5 attachments, view[0]=" << (void*)view << std::endl;
                                        }
                                        if (!view && numAttachments >= 5) {
                                            std::cerr << "[WebGPU] ERROR: GBuffer attachment " << i << " has null view!" << std::endl;
                                        }

                                        // Parse loadOp (default 'clear')
                                        WGPULoadOp loadOp = WGPULoadOp_Clear;
                                        auto loadOpProp = g_engine->getProperty(attachment, "loadOp");
                                        if (!g_engine->isUndefined(loadOpProp)) {
                                            std::string loadOpStr = g_engine->toString(loadOpProp);
                                            if (loadOpStr == "load") loadOp = WGPULoadOp_Load;
                                        }

                                        // Parse storeOp (default 'store')
                                        WGPUStoreOp storeOp = WGPUStoreOp_Store;
                                        auto storeOpProp = g_engine->getProperty(attachment, "storeOp");
                                        if (!g_engine->isUndefined(storeOpProp)) {
                                            std::string storeOpStr = g_engine->toString(storeOpProp);
                                            if (storeOpStr == "discard") storeOp = WGPUStoreOp_Discard;
                                        }

                                        // Parse clearValue only if loadOp is 'clear'
                                        double r = 0, g = 0, b = 0, a = 1;
                                        if (loadOp == WGPULoadOp_Clear) {
                                            auto clearValue = g_engine->getProperty(attachment, "clearValue");
                                            if (!g_engine->isUndefined(clearValue)) {
                                                // Check if it's an array [r, g, b, a] or object {r, g, b, a}
                                                if (g_engine->isArray(clearValue)) {
                                                    r = g_engine->toNumber(g_engine->getPropertyIndex(clearValue, 0));
                                                    g = g_engine->toNumber(g_engine->getPropertyIndex(clearValue, 1));
                                                    b = g_engine->toNumber(g_engine->getPropertyIndex(clearValue, 2));
                                                    a = g_engine->toNumber(g_engine->getPropertyIndex(clearValue, 3));
                                                } else {
                                                    r = g_engine->toNumber(g_engine->getProperty(clearValue, "r"));
                                                    g = g_engine->toNumber(g_engine->getProperty(clearValue, "g"));
                                                    b = g_engine->toNumber(g_engine->getProperty(clearValue, "b"));
                                                    a = g_engine->toNumber(g_engine->getProperty(clearValue, "a"));
                                                }
                                            }
                                        }

                                        if (i == 0) {
                                            firstR = r; firstG = g; firstB = b; firstA = a;
                                        }

                                        WGPURenderPassColorAttachment colorAttachment = {};
                                        colorAttachment.view = view;
                                        colorAttachment.loadOp = loadOp;
                                        colorAttachment.storeOp = storeOp;
                                        colorAttachment.clearValue = {r, g, b, a};
                                        colorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
                                        colorAttachmentList.push_back(colorAttachment);
                                    }

                                    WGPURenderPassDescriptor renderPassDesc = {};
                                    renderPassDesc.colorAttachmentCount = colorAttachmentList.size();
                                    renderPassDesc.colorAttachments = colorAttachmentList.data();

                                    // Parse depth stencil attachment if present
                                    WGPURenderPassDepthStencilAttachment depthStencilAttachment = {};
                                    auto depthStencilProp = g_engine->getProperty(descriptor, "depthStencilAttachment");
                                    if (!g_engine->isUndefined(depthStencilProp)) {
                                        auto depthViewHandle = g_engine->getProperty(depthStencilProp, "view");
                                        WGPUTextureView depthView = (WGPUTextureView)g_engine->getPrivateData(depthViewHandle);
                                        depthStencilAttachment.view = depthView;

                                        // Depth clear value (default 1.0)
                                        auto depthClearValueProp = g_engine->getProperty(depthStencilProp, "depthClearValue");
                                        depthStencilAttachment.depthClearValue = g_engine->isUndefined(depthClearValueProp)
                                            ? 1.0f : (float)g_engine->toNumber(depthClearValueProp);

                                        // Depth load/store ops (default clear/store)
                                        auto depthLoadOpProp = g_engine->getProperty(depthStencilProp, "depthLoadOp");
                                        if (!g_engine->isUndefined(depthLoadOpProp)) {
                                            std::string loadOpStr = g_engine->toString(depthLoadOpProp);
                                            if (loadOpStr == "load") depthStencilAttachment.depthLoadOp = WGPULoadOp_Load;
                                            else depthStencilAttachment.depthLoadOp = WGPULoadOp_Clear;
                                        } else {
                                            depthStencilAttachment.depthLoadOp = WGPULoadOp_Clear;
                                        }

                                        auto depthStoreOpProp = g_engine->getProperty(depthStencilProp, "depthStoreOp");
                                        if (!g_engine->isUndefined(depthStoreOpProp)) {
                                            std::string storeOpStr = g_engine->toString(depthStoreOpProp);
                                            if (storeOpStr == "discard") depthStencilAttachment.depthStoreOp = WGPUStoreOp_Discard;
                                            else depthStencilAttachment.depthStoreOp = WGPUStoreOp_Store;
                                        } else {
                                            depthStencilAttachment.depthStoreOp = WGPUStoreOp_Store;
                                        }

                                        // Stencil ops (default undefined/disabled)
                                        depthStencilAttachment.stencilClearValue = 0;
                                        depthStencilAttachment.stencilLoadOp = WGPULoadOp_Undefined;
                                        depthStencilAttachment.stencilStoreOp = WGPUStoreOp_Undefined;

                                        renderPassDesc.depthStencilAttachment = &depthStencilAttachment;
                                        if (g_verboseLogging) std::cout << "[WebGPU] Render pass with depth attachment, clear=" << depthStencilAttachment.depthClearValue << std::endl;
                                    }

                                    // Begin render pass on the captured encoder (not the global)
                                    WGPURenderPassEncoder renderPass = wgpuCommandEncoderBeginRenderPass(encoderToUse, &renderPassDesc);
                                    g_liveRenderPasses.insert(renderPass);
                                    g_renderPassesCreated++;

                                    // Store in per-encoder map (fixes issue with multiple encoders)
                                    g_encoderRenderPassMap[encoderToUse] = renderPass;
                                    g_renderPassEncoderMap[renderPass] = encoderToUse;

                                    // Also set global for backwards compatibility with render pass methods
                                    g_jsRenderPass = renderPass;

                                    if (g_verboseLogging) std::cout << "[WebGPU] Render pass started (" << numAttachments << " attachments), clear: (" << firstR << "," << firstG << "," << firstB << "," << firstA << ")" << std::endl;

                                    auto jsRenderPass = g_engine->newObject();
                                    g_engine->setPrivateData(jsRenderPass, renderPass);
                                    // end() is the single terminal owner. Avoid a
                                    // GC-driven Dawn call while an encoder frame is
                                    // still being assembled.

                                    // A Three.js RenderPipeline can open an offscreen pass while
                                    // the final surface pass is still alive. Every JS method must
                                    // therefore target the encoder represented by this wrapper,
                                    // not the most recently opened pass in g_jsRenderPass.
                                    // renderPass.setPipeline(pipeline)
                                    g_engine->setProperty(jsRenderPass, "setPipeline",
                                        sharedMethod(g_transientMethods.renderSetPipeline, "setPipeline",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::RenderSetPipeline);
                                            if (args.empty()) return g_engine->newUndefined();

                                            WGPURenderPassEncoder capturedRenderPassForCommands =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);
                                            WGPURenderPipeline pipeline = (WGPURenderPipeline)g_engine->getPrivateData(args[0]);
                                            if (capturedRenderPassForCommands && pipeline) {
                                                wgpuRenderPassEncoderSetPipeline(capturedRenderPassForCommands, pipeline);
                                                if (g_verboseLogging) std::cout << "[WebGPU] Pipeline set" << std::endl;
                                            }

                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // renderPass.setBindGroup(index, bindGroup, dynamicOffsets?)
                                    g_engine->setProperty(jsRenderPass, "setBindGroup",
                                        sharedMethod(g_transientMethods.renderSetBindGroup, "setBindGroup",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::RenderSetBindGroup);
                                            if (args.size() < 2) {
                                                g_engine->throwException("setBindGroup requires index and bindGroup");
                                                return g_engine->newUndefined();
                                            }

                                            uint32_t groupIndex = (uint32_t)g_engine->toNumber(args[0]);
                                            WGPURenderPassEncoder capturedRenderPassForCommands =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);
                                            WGPUBindGroup bindGroup = (WGPUBindGroup)g_engine->getPrivateData(args[1]);

                                            if (capturedRenderPassForCommands && bindGroup) {
                                                // TODO: Support dynamic offsets
                                                wgpuRenderPassEncoderSetBindGroup(capturedRenderPassForCommands, groupIndex, bindGroup, 0, nullptr);
                                                if (g_verboseLogging) std::cout << "[WebGPU] Set bind group at index " << groupIndex << std::endl;
                                            }

                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // renderPass.draw(vertexCount, instanceCount?, firstVertex?, firstInstance?)
                                    g_engine->setProperty(jsRenderPass, "draw",
                                        sharedMethod(g_transientMethods.renderDraw, "draw",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::RenderDraw);
                                            if (args.empty()) return g_engine->newUndefined();

                                            uint32_t vertexCount = (uint32_t)g_engine->toNumber(args[0]);
                                            uint32_t instanceCount = args.size() > 1 ? (uint32_t)g_engine->toNumber(args[1]) : 1;
                                            uint32_t firstVertex = args.size() > 2 ? (uint32_t)g_engine->toNumber(args[2]) : 0;
                                            uint32_t firstInstance = args.size() > 3 ? (uint32_t)g_engine->toNumber(args[3]) : 0;
                                            WGPURenderPassEncoder capturedRenderPassForCommands =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);

                                            if (capturedRenderPassForCommands) {
                                                wgpuRenderPassEncoderDraw(capturedRenderPassForCommands, vertexCount, instanceCount, firstVertex, firstInstance);
                                                if (g_verboseLogging) std::cout << "[WebGPU] Draw: " << vertexCount << " vertices" << std::endl;
                                            }

                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // renderPass.setVertexBuffer(slot, buffer, offset?, size?)
                                    g_engine->setProperty(jsRenderPass, "setVertexBuffer",
                                        sharedMethod(g_transientMethods.renderSetVertexBuffer, "setVertexBuffer",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::RenderSetVertexBuffer);
                                            if (args.size() < 2) return g_engine->newUndefined();

                                            uint32_t slot = (uint32_t)g_engine->toNumber(args[0]);
                                            WGPUBuffer buffer = (WGPUBuffer)g_engine->getPrivateData(args[1]);
                                            uint64_t offset = args.size() > 2 ? (uint64_t)g_engine->toNumber(args[2]) : 0;
                                            uint64_t size = args.size() > 3 ? (uint64_t)g_engine->toNumber(args[3]) : WGPU_WHOLE_SIZE;
                                            WGPURenderPassEncoder capturedRenderPassForCommands =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);

                                            if (capturedRenderPassForCommands && buffer) {
                                                wgpuRenderPassEncoderSetVertexBuffer(capturedRenderPassForCommands, slot, buffer, offset, size);
                                                if (g_verboseLogging) std::cout << "[WebGPU] Set vertex buffer at slot " << slot << std::endl;
                                            }

                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // renderPass.setIndexBuffer(buffer, format, offset?, size?)
                                    g_engine->setProperty(jsRenderPass, "setIndexBuffer",
                                        sharedMethod(g_transientMethods.renderSetIndexBuffer, "setIndexBuffer",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::RenderSetIndexBuffer);
                                            if (args.size() < 2) return g_engine->newUndefined();

                                            WGPUBuffer buffer = (WGPUBuffer)g_engine->getPrivateData(args[0]);
                                            std::string formatStr = g_engine->toString(args[1]);
                                            uint64_t offset = args.size() > 2 ? (uint64_t)g_engine->toNumber(args[2]) : 0;
                                            uint64_t size = args.size() > 3 ? (uint64_t)g_engine->toNumber(args[3]) : WGPU_WHOLE_SIZE;

                                            WGPUIndexFormat format = WGPUIndexFormat_Uint16;
                                            if (formatStr == "uint32") format = WGPUIndexFormat_Uint32;
                                            else if (formatStr == "uint16") format = WGPUIndexFormat_Uint16;
                                            WGPURenderPassEncoder capturedRenderPassForCommands =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);

                                            if (capturedRenderPassForCommands && buffer) {
                                                wgpuRenderPassEncoderSetIndexBuffer(capturedRenderPassForCommands, buffer, format, offset, size);
                                                if (g_verboseLogging) std::cout << "[WebGPU] Set index buffer, format: " << formatStr << std::endl;
                                            }

                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // renderPass.drawIndexed(indexCount, instanceCount?, firstIndex?, baseVertex?, firstInstance?)
                                    g_engine->setProperty(jsRenderPass, "drawIndexed",
                                        sharedMethod(g_transientMethods.renderDrawIndexed, "drawIndexed",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::RenderDrawIndexed);
                                            if (args.empty()) return g_engine->newUndefined();

                                            uint32_t indexCount = (uint32_t)g_engine->toNumber(args[0]);
                                            uint32_t instanceCount = args.size() > 1 ? (uint32_t)g_engine->toNumber(args[1]) : 1;
                                            uint32_t firstIndex = args.size() > 2 ? (uint32_t)g_engine->toNumber(args[2]) : 0;
                                            int32_t baseVertex = args.size() > 3 ? (int32_t)g_engine->toNumber(args[3]) : 0;
                                            uint32_t firstInstance = args.size() > 4 ? (uint32_t)g_engine->toNumber(args[4]) : 0;
                                            WGPURenderPassEncoder capturedRenderPassForCommands =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);

                                            if (capturedRenderPassForCommands) {
                                                wgpuRenderPassEncoderDrawIndexed(capturedRenderPassForCommands, indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
                                                if (g_verboseLogging) std::cout << "[WebGPU] DrawIndexed: " << indexCount << " indices, firstInstance=" << firstInstance << std::endl;
                                            }

                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // renderPass.drawIndirect(indirectBuffer, indirectOffset)
                                    g_engine->setProperty(jsRenderPass, "drawIndirect",
                                        sharedMethod(g_transientMethods.renderDrawIndirect, "drawIndirect",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::RenderDrawIndirect);
                                            if (args.size() < 2) return g_engine->newUndefined();

                                            WGPUBuffer indirectBuffer = (WGPUBuffer)g_engine->getPrivateData(args[0]);
                                            uint64_t indirectOffset = (uint64_t)g_engine->toNumber(args[1]);
                                            WGPURenderPassEncoder capturedRenderPassForCommands =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);

                                            if (capturedRenderPassForCommands && indirectBuffer) {
                                                wgpuRenderPassEncoderDrawIndirect(capturedRenderPassForCommands, indirectBuffer, indirectOffset);
                                                if (g_verboseLogging) std::cout << "[WebGPU] DrawIndirect at offset " << indirectOffset << std::endl;
                                            }

                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // renderPass.drawIndexedIndirect(indirectBuffer, indirectOffset)
                                    g_engine->setProperty(jsRenderPass, "drawIndexedIndirect",
                                        sharedMethod(g_transientMethods.renderDrawIndexedIndirect, "drawIndexedIndirect",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::RenderDrawIndexedIndirect);
                                            if (args.size() < 2) return g_engine->newUndefined();

                                            WGPUBuffer indirectBuffer = (WGPUBuffer)g_engine->getPrivateData(args[0]);
                                            uint64_t indirectOffset = (uint64_t)g_engine->toNumber(args[1]);
                                            WGPURenderPassEncoder capturedRenderPassForCommands =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);

                                            if (capturedRenderPassForCommands && indirectBuffer) {
                                                wgpuRenderPassEncoderDrawIndexedIndirect(capturedRenderPassForCommands, indirectBuffer, indirectOffset);
                                                if (g_verboseLogging) std::cout << "[WebGPU] DrawIndexedIndirect at offset " << indirectOffset << std::endl;
                                            }

                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // renderPass.setViewport(x, y, width, height, minDepth, maxDepth)
                                    g_engine->setProperty(jsRenderPass, "setViewport",
                                        sharedMethod(g_transientMethods.renderSetViewport, "setViewport",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            if (args.size() < 6) return g_engine->newUndefined();

                                            float x = (float)g_engine->toNumber(args[0]);
                                            float y = (float)g_engine->toNumber(args[1]);
                                            float width = (float)g_engine->toNumber(args[2]);
                                            float height = (float)g_engine->toNumber(args[3]);
                                            float minDepth = (float)g_engine->toNumber(args[4]);
                                            float maxDepth = (float)g_engine->toNumber(args[5]);
                                            WGPURenderPassEncoder capturedRenderPassForCommands =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);

                                            if (capturedRenderPassForCommands) {
                                                wgpuRenderPassEncoderSetViewport(capturedRenderPassForCommands, x, y, width, height, minDepth, maxDepth);
                                                if (g_verboseLogging) std::cout << "[WebGPU] SetViewport: " << x << "," << y << " " << width << "x" << height << std::endl;
                                            }

                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // renderPass.setScissorRect(x, y, width, height)
                                    g_engine->setProperty(jsRenderPass, "setScissorRect",
                                        sharedMethod(g_transientMethods.renderSetScissorRect, "setScissorRect",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            if (args.size() < 4) return g_engine->newUndefined();

                                            uint32_t x = (uint32_t)g_engine->toNumber(args[0]);
                                            uint32_t y = (uint32_t)g_engine->toNumber(args[1]);
                                            uint32_t width = (uint32_t)g_engine->toNumber(args[2]);
                                            uint32_t height = (uint32_t)g_engine->toNumber(args[3]);
                                            WGPURenderPassEncoder capturedRenderPassForCommands =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);

                                            if (capturedRenderPassForCommands) {
                                                wgpuRenderPassEncoderSetScissorRect(capturedRenderPassForCommands, x, y, width, height);
                                                if (g_verboseLogging) std::cout << "[WebGPU] SetScissorRect: " << x << "," << y << " " << width << "x" << height << std::endl;
                                            }

                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // renderPass.setBlendConstant(color)
                                    g_engine->setProperty(jsRenderPass, "setBlendConstant",
                                        sharedMethod(g_transientMethods.renderSetBlendConstant, "setBlendConstant",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            if (args.empty()) return g_engine->newUndefined();

                                            auto color = args[0];
                                            WGPUColor blendColor = {};
                                            if (g_engine->isArray(color)) {
                                                blendColor.r = g_engine->toNumber(g_engine->getPropertyIndex(color, 0));
                                                blendColor.g = g_engine->toNumber(g_engine->getPropertyIndex(color, 1));
                                                blendColor.b = g_engine->toNumber(g_engine->getPropertyIndex(color, 2));
                                                blendColor.a = g_engine->toNumber(g_engine->getPropertyIndex(color, 3));
                                            } else {
                                                blendColor.r = g_engine->toNumber(g_engine->getProperty(color, "r"));
                                                blendColor.g = g_engine->toNumber(g_engine->getProperty(color, "g"));
                                                blendColor.b = g_engine->toNumber(g_engine->getProperty(color, "b"));
                                                blendColor.a = g_engine->toNumber(g_engine->getProperty(color, "a"));
                                            }
                                            WGPURenderPassEncoder capturedRenderPassForCommands =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);

                                            if (capturedRenderPassForCommands) {
                                                wgpuRenderPassEncoderSetBlendConstant(capturedRenderPassForCommands, &blendColor);
                                            }

                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // renderPass.setStencilReference(reference)
                                    g_engine->setProperty(jsRenderPass, "setStencilReference",
                                        sharedMethod(g_transientMethods.renderSetStencilReference, "setStencilReference",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            if (args.empty()) return g_engine->newUndefined();

                                            uint32_t reference = (uint32_t)g_engine->toNumber(args[0]);
                                            WGPURenderPassEncoder capturedRenderPassForCommands =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);
                                            if (capturedRenderPassForCommands) {
                                                wgpuRenderPassEncoderSetStencilReference(capturedRenderPassForCommands, reference);
                                            }

                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // renderPass.executeBundles(bundles)
                                    // Used by Three.js for mipmap generation
                                    g_engine->setProperty(jsRenderPass, "executeBundles",
                                        sharedMethod(g_transientMethods.renderExecuteBundles, "executeBundles",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::RenderExecuteBundles);
                                            WGPURenderPassEncoder capturedRenderPassForBundles =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);
                                            if (args.empty() || !capturedRenderPassForBundles) return g_engine->newUndefined();

                                            auto bundlesArray = args[0];
                                            auto lengthProp = g_engine->getProperty(bundlesArray, "length");
                                            int bundleCount = g_engine->isUndefined(lengthProp) ? 0 : (int)g_engine->toNumber(lengthProp);

                                            std::vector<WGPURenderBundle> bundles;
                                            bundles.reserve(bundleCount);
                                            for (int i = 0; i < bundleCount; i++) {
                                                auto bundleHandle = g_engine->getPropertyIndex(bundlesArray, i);
                                                WGPURenderBundle bundle = (WGPURenderBundle)g_engine->getPrivateData(bundleHandle);
                                                if (bundle) bundles.push_back(bundle);
                                            }

                                            if (!bundles.empty()) {
                                                wgpuRenderPassEncoderExecuteBundles(capturedRenderPassForBundles, bundles.size(), bundles.data());
                                                if (g_verboseLogging) std::cout << "[WebGPU] Executed " << bundles.size() << " render bundles" << std::endl;
                                            }

                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // renderPass.end()
                                    g_engine->setProperty(jsRenderPass, "end",
                                        sharedMethod(g_transientMethods.renderEnd, "end",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::RenderEnd);
                                            WGPURenderPassEncoder pass =
                                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);
                                            closeRenderPass(pass);
                                            g_engine->setPrivateData(receiver, nullptr);
                                            if (g_verboseLogging) std::cout << "[WebGPU] Render pass ended" << std::endl;
                                            return g_engine->newUndefined();
                                        })
                                    );

                                    return jsRenderPass;
                                })
                            );

                            // encoder.beginComputePass(descriptor?)
                            g_engine->setProperty(jsEncoder, "beginComputePass",
                                sharedMethod(g_transientMethods.encoderBeginComputePass, "beginComputePass",
                                    [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                    ScopedBridgeMeasurement measurement(ProfiledBridgeOp::BeginComputePass);
                                    WGPUCommandEncoder encoder =
                                        (WGPUCommandEncoder)g_engine->getPrivateData(receiver);
                                    if (!encoder) {
                                        g_engine->throwException("No command encoder");
                                        return g_engine->newUndefined();
                                    }

                                    WGPUComputePassDescriptor computePassDesc = {};
                                    WGPUComputePassEncoder computePass =
                                        wgpuCommandEncoderBeginComputePass(encoder, &computePassDesc);
                                    g_jsComputePass = computePass;
                                    g_encoderComputePassMap[encoder] = computePass;
                                    g_computePassEncoderMap[computePass] = encoder;
                                    g_liveComputePasses.insert(computePass);
                                    g_computePassesCreated++;

                                    auto jsComputePass = g_engine->newObject();
                                    g_engine->setPrivateData(jsComputePass, computePass);
                                    // end() is the single terminal owner. Avoid a
                                    // GC-driven Dawn call while compute work is
                                    // still being encoded.

                                    // computePass.setPipeline(pipeline)
                                    g_engine->setProperty(jsComputePass, "setPipeline",
                                        sharedMethod(g_transientMethods.computeSetPipeline, "setPipeline",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::ComputeSetPipeline);
                                            if (args.empty()) return g_engine->newUndefined();
                                            WGPUComputePassEncoder pass =
                                                (WGPUComputePassEncoder)g_engine->getPrivateData(receiver);
                                            WGPUComputePipeline pipeline = (WGPUComputePipeline)g_engine->getPrivateData(args[0]);
                                            if (pass && pipeline) {
                                                wgpuComputePassEncoderSetPipeline(pass, pipeline);
                                            }
                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // computePass.setBindGroup(index, bindGroup, dynamicOffsets?)
                                    g_engine->setProperty(jsComputePass, "setBindGroup",
                                        sharedMethod(g_transientMethods.computeSetBindGroup, "setBindGroup",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::ComputeSetBindGroup);
                                            if (args.size() < 2) return g_engine->newUndefined();
                                            uint32_t index = (uint32_t)g_engine->toNumber(args[0]);
                                            WGPUComputePassEncoder pass =
                                                (WGPUComputePassEncoder)g_engine->getPrivateData(receiver);
                                            WGPUBindGroup bindGroup = (WGPUBindGroup)g_engine->getPrivateData(args[1]);
                                            if (pass && bindGroup) {
                                                wgpuComputePassEncoderSetBindGroup(pass, index, bindGroup, 0, nullptr);
                                            }
                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // computePass.dispatchWorkgroups(countX, countY?, countZ?)
                                    g_engine->setProperty(jsComputePass, "dispatchWorkgroups",
                                        sharedMethod(g_transientMethods.computeDispatchWorkgroups, "dispatchWorkgroups",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::ComputeDispatchWorkgroups);
                                            if (args.empty()) return g_engine->newUndefined();
                                            uint32_t countX = (uint32_t)g_engine->toNumber(args[0]);
                                            uint32_t countY = args.size() > 1 ? (uint32_t)g_engine->toNumber(args[1]) : 1;
                                            uint32_t countZ = args.size() > 2 ? (uint32_t)g_engine->toNumber(args[2]) : 1;
                                            WGPUComputePassEncoder pass =
                                                (WGPUComputePassEncoder)g_engine->getPrivateData(receiver);
                                            if (pass) {
                                                wgpuComputePassEncoderDispatchWorkgroups(pass, countX, countY, countZ);
                                            }
                                            return g_engine->newUndefined();
                                        })
                                    );

                                    // computePass.end()
                                    g_engine->setProperty(jsComputePass, "end",
                                        sharedMethod(g_transientMethods.computeEnd, "end",
                                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                            ScopedBridgeMeasurement measurement(ProfiledBridgeOp::ComputeEnd);
                                            WGPUComputePassEncoder pass =
                                                (WGPUComputePassEncoder)g_engine->getPrivateData(receiver);
                                            closeComputePass(pass);
                                            g_engine->setPrivateData(receiver, nullptr);
                                            return g_engine->newUndefined();
                                        })
                                    );

                                    if (g_verboseLogging) std::cout << "[WebGPU] Compute pass started" << std::endl;
                                    return jsComputePass;
                                })
                            );

                            // encoder.copyBufferToBuffer(source, sourceOffset, destination, destinationOffset, size)
                            g_engine->setProperty(jsEncoder, "copyBufferToBuffer",
                                sharedMethod(g_transientMethods.encoderCopyBufferToBuffer, "copyBufferToBuffer",
                                    [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                    WGPUCommandEncoder encoder =
                                        (WGPUCommandEncoder)g_engine->getPrivateData(receiver);
                                    if (args.size() < 5 || !encoder) return g_engine->newUndefined();

                                    WGPUBuffer source = (WGPUBuffer)g_engine->getPrivateData(args[0]);
                                    uint64_t sourceOffset = (uint64_t)g_engine->toNumber(args[1]);
                                    WGPUBuffer destination = (WGPUBuffer)g_engine->getPrivateData(args[2]);
                                    uint64_t destOffset = (uint64_t)g_engine->toNumber(args[3]);
                                    uint64_t size = (uint64_t)g_engine->toNumber(args[4]);

                                    if (source && destination) {
                                        wgpuCommandEncoderCopyBufferToBuffer(encoder, source, sourceOffset, destination, destOffset, size);
                                    }
                                    return g_engine->newUndefined();
                                })
                            );

                            // encoder.copyBufferToTexture(source, destination, copySize)
                            g_engine->setProperty(jsEncoder, "copyBufferToTexture",
                                sharedMethod(g_transientMethods.encoderCopyBufferToTexture, "copyBufferToTexture",
                                    [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                    WGPUCommandEncoder encoder =
                                        (WGPUCommandEncoder)g_engine->getPrivateData(receiver);
                                    if (args.size() < 3 || !encoder) return g_engine->newUndefined();

                                    auto sourceProp = args[0];
                                    auto destProp = args[1];
                                    auto sizeProp = args[2];

                                    // Source (buffer info)
                                    WGPUBuffer buffer = (WGPUBuffer)g_engine->getPrivateData(g_engine->getProperty(sourceProp, "buffer"));
                                    uint64_t offset = (uint64_t)g_engine->toNumber(g_engine->getProperty(sourceProp, "offset"));
                                    uint32_t bytesPerRow = (uint32_t)g_engine->toNumber(g_engine->getProperty(sourceProp, "bytesPerRow"));
                                    uint32_t rowsPerImage = (uint32_t)g_engine->toNumber(g_engine->getProperty(sourceProp, "rowsPerImage"));

                                    // Destination (texture info)
                                    WGPUTexture texture = (WGPUTexture)g_engine->getPrivateData(g_engine->getProperty(destProp, "texture"));
                                    uint32_t mipLevel = (uint32_t)g_engine->toNumber(g_engine->getProperty(destProp, "mipLevel"));
                                    auto originProp = g_engine->getProperty(destProp, "origin");
                                    uint32_t originX = 0, originY = 0, originZ = 0;
                                    if (!g_engine->isUndefined(originProp)) {
                                        originX = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(originProp, 0));
                                        originY = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(originProp, 1));
                                        originZ = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(originProp, 2));
                                    }

                                    // Copy size
                                    uint32_t width = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeProp, 0));
                                    uint32_t height = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeProp, 1));
                                    uint32_t depthOrLayers = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeProp, 2));
                                    if (depthOrLayers == 0) depthOrLayers = 1;

                                    if (buffer && texture) {
                                        WGPUImageCopyBuffer_Compat srcCopy = {};
                                        srcCopy.buffer = buffer;
                                        srcCopy.layout.offset = offset;
                                        srcCopy.layout.bytesPerRow = bytesPerRow;
                                        srcCopy.layout.rowsPerImage = rowsPerImage > 0 ? rowsPerImage : height;

                                        WGPUImageCopyTexture_Compat dstCopy = {};
                                        dstCopy.texture = texture;
                                        dstCopy.mipLevel = mipLevel;
                                        dstCopy.origin = {originX, originY, originZ};

                                        WGPUExtent3D copySize = {width, height, depthOrLayers};
                                        wgpuCommandEncoderCopyBufferToTexture(encoder, &srcCopy, &dstCopy, &copySize);
                                    }
                                    return g_engine->newUndefined();
                                })
                            );

                            // encoder.copyTextureToBuffer(source, destination, copySize)
                            g_engine->setProperty(jsEncoder, "copyTextureToBuffer",
                                sharedMethod(g_transientMethods.encoderCopyTextureToBuffer, "copyTextureToBuffer",
                                    [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                    WGPUCommandEncoder encoder =
                                        (WGPUCommandEncoder)g_engine->getPrivateData(receiver);
                                    if (args.size() < 3 || !encoder) return g_engine->newUndefined();

                                    auto sourceProp = args[0];
                                    auto destProp = args[1];
                                    auto sizeProp = args[2];

                                    // Source (texture info)
                                    WGPUTexture texture = (WGPUTexture)g_engine->getPrivateData(g_engine->getProperty(sourceProp, "texture"));
                                    uint32_t mipLevel = (uint32_t)g_engine->toNumber(g_engine->getProperty(sourceProp, "mipLevel"));
                                    auto originProp = g_engine->getProperty(sourceProp, "origin");
                                    uint32_t originX = 0, originY = 0, originZ = 0;
                                    if (!g_engine->isUndefined(originProp)) {
                                        originX = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(originProp, 0));
                                        originY = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(originProp, 1));
                                        originZ = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(originProp, 2));
                                    }

                                    // Destination (buffer info)
                                    WGPUBuffer buffer = (WGPUBuffer)g_engine->getPrivateData(g_engine->getProperty(destProp, "buffer"));
                                    uint64_t offset = (uint64_t)g_engine->toNumber(g_engine->getProperty(destProp, "offset"));
                                    uint32_t bytesPerRow = (uint32_t)g_engine->toNumber(g_engine->getProperty(destProp, "bytesPerRow"));
                                    uint32_t rowsPerImage = (uint32_t)g_engine->toNumber(g_engine->getProperty(destProp, "rowsPerImage"));

                                    // Copy size - can be array [w,h,d] or object {width, height, depthOrArrayLayers}
                                    uint32_t width = 0, height = 0, depthOrLayers = 1;
                                    auto widthProp = g_engine->getProperty(sizeProp, "width");
                                    if (!g_engine->isUndefined(widthProp)) {
                                        // Object format: { width, height, depthOrArrayLayers }
                                        width = (uint32_t)g_engine->toNumber(widthProp);
                                        height = (uint32_t)g_engine->toNumber(g_engine->getProperty(sizeProp, "height"));
                                        auto depthProp = g_engine->getProperty(sizeProp, "depthOrArrayLayers");
                                        depthOrLayers = g_engine->isUndefined(depthProp) ? 1 : (uint32_t)g_engine->toNumber(depthProp);
                                    } else {
                                        // Array format: [width, height, depth]
                                        width = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeProp, 0));
                                        height = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeProp, 1));
                                        depthOrLayers = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeProp, 2));
                                    }
                                    if (depthOrLayers == 0) depthOrLayers = 1;

                                    if (g_verboseLogging) {
                                        std::cout << "[WebGPU] copyTextureToBuffer: texture=" << texture
                                                  << ", buffer=" << buffer
                                                  << ", size=" << width << "x" << height << "x" << depthOrLayers
                                                  << ", bytesPerRow=" << bytesPerRow << std::endl;
                                    }

                                    if (buffer && texture) {
                                        WGPUImageCopyTexture_Compat srcCopy = {};
                                        srcCopy.texture = texture;
                                        srcCopy.mipLevel = mipLevel;
                                        srcCopy.origin = {originX, originY, originZ};

                                        WGPUImageCopyBuffer_Compat dstCopy = {};
                                        dstCopy.buffer = buffer;
                                        dstCopy.layout.offset = offset;
                                        dstCopy.layout.bytesPerRow = bytesPerRow;
                                        dstCopy.layout.rowsPerImage = rowsPerImage > 0 ? rowsPerImage : height;

                                        WGPUExtent3D copySize = {width, height, depthOrLayers};
                                        wgpuCommandEncoderCopyTextureToBuffer(encoder, &srcCopy, &dstCopy, &copySize);
                                    }
                                    return g_engine->newUndefined();
                                })
                            );

                            // encoder.copyTextureToTexture(source, destination, copySize)
                            g_engine->setProperty(jsEncoder, "copyTextureToTexture",
                                sharedMethod(g_transientMethods.encoderCopyTextureToTexture, "copyTextureToTexture",
                                    [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                    WGPUCommandEncoder encoder =
                                        (WGPUCommandEncoder)g_engine->getPrivateData(receiver);
                                    if (args.size() < 3 || !encoder) return g_engine->newUndefined();

                                    auto sourceProp = args[0];
                                    auto destProp = args[1];
                                    auto sizeProp = args[2];

                                    // Source texture
                                    WGPUTexture srcTexture = (WGPUTexture)g_engine->getPrivateData(g_engine->getProperty(sourceProp, "texture"));
                                    uint32_t srcMipLevel = (uint32_t)g_engine->toNumber(g_engine->getProperty(sourceProp, "mipLevel"));
                                    auto srcOriginProp = g_engine->getProperty(sourceProp, "origin");
                                    uint32_t srcOriginX = 0, srcOriginY = 0, srcOriginZ = 0;
                                    if (!g_engine->isUndefined(srcOriginProp)) {
                                        srcOriginX = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(srcOriginProp, 0));
                                        srcOriginY = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(srcOriginProp, 1));
                                        srcOriginZ = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(srcOriginProp, 2));
                                    }

                                    // Destination texture
                                    WGPUTexture dstTexture = (WGPUTexture)g_engine->getPrivateData(g_engine->getProperty(destProp, "texture"));
                                    uint32_t dstMipLevel = (uint32_t)g_engine->toNumber(g_engine->getProperty(destProp, "mipLevel"));
                                    auto dstOriginProp = g_engine->getProperty(destProp, "origin");
                                    uint32_t dstOriginX = 0, dstOriginY = 0, dstOriginZ = 0;
                                    if (!g_engine->isUndefined(dstOriginProp)) {
                                        dstOriginX = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(dstOriginProp, 0));
                                        dstOriginY = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(dstOriginProp, 1));
                                        dstOriginZ = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(dstOriginProp, 2));
                                    }

                                    // Copy size - handle both array and object forms
                                    uint32_t width = 1, height = 1, depthOrLayers = 1;
                                    if (g_engine->isArray(sizeProp)) {
                                        width = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeProp, 0));
                                        height = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeProp, 1));
                                        auto depthVal = g_engine->getPropertyIndex(sizeProp, 2);
                                        if (!g_engine->isUndefined(depthVal)) {
                                            depthOrLayers = (uint32_t)g_engine->toNumber(depthVal);
                                        }
                                    } else {
                                        width = (uint32_t)g_engine->toNumber(g_engine->getProperty(sizeProp, "width"));
                                        height = (uint32_t)g_engine->toNumber(g_engine->getProperty(sizeProp, "height"));
                                        auto depthVal = g_engine->getProperty(sizeProp, "depthOrArrayLayers");
                                        if (!g_engine->isUndefined(depthVal)) {
                                            depthOrLayers = (uint32_t)g_engine->toNumber(depthVal);
                                        }
                                    }
                                    if (depthOrLayers == 0) depthOrLayers = 1;

                                    if (srcTexture && dstTexture) {
                                        WGPUImageCopyTexture_Compat srcCopy = {};
                                        srcCopy.texture = srcTexture;
                                        srcCopy.mipLevel = srcMipLevel;
                                        srcCopy.origin = {srcOriginX, srcOriginY, srcOriginZ};

                                        WGPUImageCopyTexture_Compat dstCopy = {};
                                        dstCopy.texture = dstTexture;
                                        dstCopy.mipLevel = dstMipLevel;
                                        dstCopy.origin = {dstOriginX, dstOriginY, dstOriginZ};

                                        WGPUExtent3D copySize = {width, height, depthOrLayers};
                                        wgpuCommandEncoderCopyTextureToTexture(encoder, &srcCopy, &dstCopy, &copySize);
                                    }
                                    return g_engine->newUndefined();
                                })
                            );

                            // encoder.clearBuffer(buffer, offset?, size?)
                            g_engine->setProperty(jsEncoder, "clearBuffer",
                                sharedMethod(g_transientMethods.encoderClearBuffer, "clearBuffer",
                                    [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                    WGPUCommandEncoder encoder =
                                        (WGPUCommandEncoder)g_engine->getPrivateData(receiver);
                                    if (args.empty() || !encoder) return g_engine->newUndefined();

                                    WGPUBuffer buffer = (WGPUBuffer)g_engine->getPrivateData(args[0]);
                                    uint64_t offset = args.size() > 1 ? (uint64_t)g_engine->toNumber(args[1]) : 0;
                                    uint64_t size = args.size() > 2 ? (uint64_t)g_engine->toNumber(args[2]) : WGPU_WHOLE_SIZE;

                                    if (buffer) {
                                        wgpuCommandEncoderClearBuffer(encoder, buffer, offset, size);
                                    }
                                    return g_engine->newUndefined();
                                })
                            );

                            // encoder.finish(descriptor?)
                            g_engine->setProperty(jsEncoder, "finish",
                                sharedMethod(g_transientMethods.encoderFinish, "finish",
                                    [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                                    ScopedBridgeMeasurement measurement(ProfiledBridgeOp::FinishCommandEncoder);
                                    WGPUCommandEncoder encoderToFinish =
                                        (WGPUCommandEncoder)g_engine->getPrivateData(receiver);
                                    if (!encoderToFinish ||
                                        g_liveCommandEncoders.find(encoderToFinish) == g_liveCommandEncoders.end()) {
                                        g_engine->throwException("Command encoder is already finished");
                                        return g_engine->newUndefined();
                                    }

                                    // Auto-end any active render/compute passes for THIS encoder
                                    // Look up from per-encoder map, not global
                                    auto renderPassIt = g_encoderRenderPassMap.find(encoderToFinish);
                                    if (renderPassIt != g_encoderRenderPassMap.end() && renderPassIt->second) {
                                        WGPURenderPassEncoder renderPass = renderPassIt->second;
                                        if (g_verboseLogging) std::cout << "[WebGPU] Auto-ending render pass (pass=" << (void*)renderPass << ", encoder=" << (void*)encoderToFinish << ")" << std::endl;
                                        closeRenderPass(renderPass);
                                    }

                                    auto computePassIt = g_encoderComputePassMap.find(encoderToFinish);
                                    if (computePassIt != g_encoderComputePassMap.end() && computePassIt->second) {
                                        WGPUComputePassEncoder computePass = computePassIt->second;
                                        if (g_verboseLogging) std::cout << "[WebGPU] Auto-ending compute pass (pass=" << (void*)computePass << ", encoder=" << (void*)encoderToFinish << ")" << std::endl;
                                        closeComputePass(computePass);
                                    }

                                    WGPUCommandBufferDescriptor cmdDesc = {};
                                    WGPUCommandBuffer cmdBuffer = nullptr;

                                    if (encoderToFinish) {
                                        cmdBuffer = wgpuCommandEncoderFinish(encoderToFinish, &cmdDesc);
                                        g_liveCommandEncoders.erase(encoderToFinish);
                                        wgpuCommandEncoderRelease(encoderToFinish);
                                        g_engine->setPrivateData(receiver, nullptr);

                                        // Clear global if it matches
                                        if (g_jsCommandEncoder == encoderToFinish) {
                                            g_jsCommandEncoder = nullptr;
                                        }

                                        if (g_verboseLogging) std::cout << "[WebGPU] Command encoder finished, buffer: " << cmdBuffer << std::endl;
                                    }

                                    auto jsCommandBuffer = g_engine->newObject();
                                    g_engine->setPrivateData(jsCommandBuffer, cmdBuffer);
                                    if (cmdBuffer) g_commandBuffersCreated++;

                                    return jsCommandBuffer;
                                })
                            );

                            return jsEncoder;
                        })
                    );

                    // device.createTexture(descriptor)
                    g_engine->setProperty(device, "createTexture",
                        g_engine->newFunction("createTexture", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (args.empty()) {
                                g_engine->throwException("createTexture requires a descriptor");
                                return g_engine->newUndefined();
                            }

                            auto descriptor = args[0];

                            // Parse size - can be [width, height, depth] array or {width, height, depthOrArrayLayers} object
                            auto sizeVal = g_engine->getProperty(descriptor, "size");
                            uint32_t width = 1, height = 1, depthOrArrayLayers = 1;

                            // Check if size is an array
                            auto lengthProp = g_engine->getProperty(sizeVal, "length");
                            if (!g_engine->isUndefined(lengthProp)) {
                                // Array format: [width, height?, depth?]
                                int len = (int)g_engine->toNumber(lengthProp);
                                if (len >= 1) width = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeVal, 0));
                                if (len >= 2) height = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeVal, 1));
                                if (len >= 3) depthOrArrayLayers = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeVal, 2));
                            } else {
                                // Object format: {width, height, depthOrArrayLayers}
                                auto w = g_engine->getProperty(sizeVal, "width");
                                auto h = g_engine->getProperty(sizeVal, "height");
                                auto d = g_engine->getProperty(sizeVal, "depthOrArrayLayers");
                                if (!g_engine->isUndefined(w)) width = (uint32_t)g_engine->toNumber(w);
                                if (!g_engine->isUndefined(h)) height = (uint32_t)g_engine->toNumber(h);
                                if (!g_engine->isUndefined(d)) depthOrArrayLayers = (uint32_t)g_engine->toNumber(d);
                            }

                            // Parse format
                            std::string formatStr = g_engine->toString(g_engine->getProperty(descriptor, "format"));
                            WGPUTextureFormat format = stringToFormat(formatStr);

                            // Parse usage
                            double usageVal = g_engine->toNumber(g_engine->getProperty(descriptor, "usage"));
                            WGPUTextureUsage usage = (WGPUTextureUsage)(uint32_t)usageVal;

                            // Fix format/usage incompatibility:
                            // BGRA8UnormSrgb doesn't support StorageBinding, convert to BGRA8Unorm or RGBA8Unorm
                            if (format == WGPUTextureFormat_BGRA8UnormSrgb && (usage & WGPUTextureUsage_StorageBinding)) {
                                std::cout << "[WebGPU] Warning: BGRA8UnormSrgb doesn't support StorageBinding, using RGBA8Unorm instead" << std::endl;
                                format = WGPUTextureFormat_RGBA8Unorm;
                                formatStr = "rgba8unorm";
                            }
                            // Also handle BGRA8Unorm which may not support storage on all platforms
                            if (format == WGPUTextureFormat_BGRA8Unorm && (usage & WGPUTextureUsage_StorageBinding)) {
                                std::cout << "[WebGPU] Warning: BGRA8Unorm may not support StorageBinding, using RGBA8Unorm instead" << std::endl;
                                format = WGPUTextureFormat_RGBA8Unorm;
                                formatStr = "rgba8unorm";
                            }

                            // Parse optional properties
                            std::string dimensionStr = g_engine->toString(g_engine->getProperty(descriptor, "dimension"));
                            WGPUTextureDimension dimension = dimensionStr.empty() ? WGPUTextureDimension_2D : stringToTextureDimension(dimensionStr);

                            auto mipLevelCountVal = g_engine->getProperty(descriptor, "mipLevelCount");
                            uint32_t mipLevelCount = g_engine->isUndefined(mipLevelCountVal) ? 1 : (uint32_t)g_engine->toNumber(mipLevelCountVal);

                            auto sampleCountVal = g_engine->getProperty(descriptor, "sampleCount");
                            uint32_t sampleCount = g_engine->isUndefined(sampleCountVal) ? 1 : (uint32_t)g_engine->toNumber(sampleCountVal);

                            // Create texture descriptor
                            WGPUTextureDescriptor texDesc = {};
                            texDesc.size.width = width;
                            texDesc.size.height = height;
                            texDesc.size.depthOrArrayLayers = depthOrArrayLayers;
                            texDesc.format = format;
                            texDesc.usage = usage;
                            texDesc.dimension = dimension;
                            texDesc.mipLevelCount = mipLevelCount;
                            texDesc.sampleCount = sampleCount;

                            WGPUTexture texture = wgpuDeviceCreateTexture(g_device, &texDesc);

                            if (!texture) {
                                g_engine->throwException("Failed to create texture");
                                return g_engine->newUndefined();
                            }

                            // Create JS wrapper
                            auto jsTexture = g_engine->newObject();
                            g_engine->setPrivateData(jsTexture, texture);

                            // Store texture properties
                            g_engine->setProperty(jsTexture, "width", g_engine->newNumber(width));
                            g_engine->setProperty(jsTexture, "height", g_engine->newNumber(height));
                            g_engine->setProperty(jsTexture, "depthOrArrayLayers", g_engine->newNumber(depthOrArrayLayers));
                            g_engine->setProperty(jsTexture, "format", g_engine->newString(formatStr.c_str()));
                            g_engine->setProperty(jsTexture, "mipLevelCount", g_engine->newNumber(mipLevelCount));
                            g_engine->setProperty(jsTexture, "sampleCount", g_engine->newNumber(sampleCount));

                            // Register texture for lookup by createView
                            uint64_t textureId = g_nextTextureId++;
                            g_textureRegistry[textureId] = {
                                texture, format, width, height, depthOrArrayLayers, mipLevelCount,
                                dimension, true, true
                            };

                            // Store texture ID for lookup
                            g_engine->setProperty(jsTexture, "_textureId", g_engine->newNumber((double)textureId));

                            // texture.createView(descriptor?) - Store texture ID for lookup
                            // We store the textureId to look up the texture later since callbacks don't have 'this'
                            g_engine->setProperty(jsTexture, "_createViewTextureId", g_engine->newNumber((double)textureId));

                            g_engine->setProperty(jsTexture, "createView",
                                g_engine->newFunction("createView", [textureId](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                    // Look up texture from registry using captured textureId
                                    auto it = g_textureRegistry.find(textureId);
                                    if (it == g_textureRegistry.end()) {
                                        std::cerr << "[WebGPU] createView: Texture " << textureId << " not found in registry" << std::endl;
                                        return g_engine->newUndefined();
                                    }

                                    WGPUTexture texture = it->second.texture;
                                    if (!texture) {
                                        std::cerr << "[WebGPU] createView: Texture " << textureId << " is null" << std::endl;
                                        return g_engine->newUndefined();
                                    }

                                    WGPUTextureViewDescriptor viewDesc = {};
                                    // Default values - use all mips and layers from the texture
                                    viewDesc.format = it->second.format;
                                    viewDesc.mipLevelCount = it->second.mipLevelCount > 0 ? it->second.mipLevelCount : 1;
                                    viewDesc.baseMipLevel = 0;
                                    viewDesc.baseArrayLayer = 0;
                                    viewDesc.aspect = WGPUTextureAspect_All;

                                    // Default dimension and arrayLayerCount based on texture dimension
                                    if (it->second.dimension == WGPUTextureDimension_3D) {
                                        // 3D textures: view as 3D, arrayLayerCount must be 1
                                        viewDesc.dimension = WGPUTextureViewDimension_3D;
                                        viewDesc.arrayLayerCount = 1;
                                    } else if (it->second.dimension == WGPUTextureDimension_1D) {
                                        // 1D textures
                                        viewDesc.dimension = WGPUTextureViewDimension_1D;
                                        viewDesc.arrayLayerCount = 1;
                                    } else {
                                        // 2D textures: use layers for 2D-array, 1 for regular 2D
                                        viewDesc.arrayLayerCount = it->second.depthOrArrayLayers > 0 ? it->second.depthOrArrayLayers : 1;
                                        viewDesc.dimension = it->second.depthOrArrayLayers > 1 ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
                                    }

                                    // Parse view descriptor if provided
                                    if (!args.empty() && !g_engine->isUndefined(args[0])) {
                                        auto descriptor = args[0];

                                        // format (optional, defaults to texture format)
                                        auto formatProp = g_engine->getProperty(descriptor, "format");
                                        if (!g_engine->isUndefined(formatProp)) {
                                            viewDesc.format = stringToFormat(g_engine->toString(formatProp));
                                        } else {
                                            viewDesc.format = it->second.format;
                                        }

                                        // dimension (optional)
                                        auto dimensionProp = g_engine->getProperty(descriptor, "dimension");
                                        if (!g_engine->isUndefined(dimensionProp)) {
                                            std::string dimStr = g_engine->toString(dimensionProp);
                                            if (dimStr == "1d") viewDesc.dimension = WGPUTextureViewDimension_1D;
                                            else if (dimStr == "2d") viewDesc.dimension = WGPUTextureViewDimension_2D;
                                            else if (dimStr == "2d-array") viewDesc.dimension = WGPUTextureViewDimension_2DArray;
                                            else if (dimStr == "cube") viewDesc.dimension = WGPUTextureViewDimension_Cube;
                                            else if (dimStr == "cube-array") viewDesc.dimension = WGPUTextureViewDimension_CubeArray;
                                            else if (dimStr == "3d") viewDesc.dimension = WGPUTextureViewDimension_3D;
                                        }

                                        // aspect (optional)
                                        auto aspectProp = g_engine->getProperty(descriptor, "aspect");
                                        if (!g_engine->isUndefined(aspectProp)) {
                                            std::string aspectStr = g_engine->toString(aspectProp);
                                            if (aspectStr == "all") viewDesc.aspect = WGPUTextureAspect_All;
                                            else if (aspectStr == "stencil-only") viewDesc.aspect = WGPUTextureAspect_StencilOnly;
                                            else if (aspectStr == "depth-only") viewDesc.aspect = WGPUTextureAspect_DepthOnly;
                                        }

                                        // baseMipLevel (optional)
                                        auto baseMipProp = g_engine->getProperty(descriptor, "baseMipLevel");
                                        if (!g_engine->isUndefined(baseMipProp)) {
                                            viewDesc.baseMipLevel = (uint32_t)g_engine->toNumber(baseMipProp);
                                        }

                                        // mipLevelCount (optional)
                                        auto mipCountProp = g_engine->getProperty(descriptor, "mipLevelCount");
                                        if (!g_engine->isUndefined(mipCountProp)) {
                                            viewDesc.mipLevelCount = (uint32_t)g_engine->toNumber(mipCountProp);
                                        }

                                        // baseArrayLayer (optional)
                                        auto baseLayerProp = g_engine->getProperty(descriptor, "baseArrayLayer");
                                        if (!g_engine->isUndefined(baseLayerProp)) {
                                            viewDesc.baseArrayLayer = (uint32_t)g_engine->toNumber(baseLayerProp);
                                        }

                                        // arrayLayerCount (optional)
                                        auto layerCountProp = g_engine->getProperty(descriptor, "arrayLayerCount");
                                        if (!g_engine->isUndefined(layerCountProp)) {
                                            uint32_t requested = (uint32_t)g_engine->toNumber(layerCountProp);
                                            uint32_t maxLayers = it->second.depthOrArrayLayers > 0 ? it->second.depthOrArrayLayers : 1;
                                            // Clamp to actual texture layer count
                                            viewDesc.arrayLayerCount = std::min(requested, maxLayers - viewDesc.baseArrayLayer);
                                        }
                                    }
                                    // else: defaults are already set above

                                    // Final validation: Fix arrayLayerCount based on view dimension
                                    if (viewDesc.dimension == WGPUTextureViewDimension_3D ||
                                        viewDesc.dimension == WGPUTextureViewDimension_1D) {
                                        // 3D/1D textures have no array layers
                                        viewDesc.arrayLayerCount = 1;
                                    } else if (viewDesc.dimension == WGPUTextureViewDimension_Cube) {
                                        // Cube requires exactly 6 layers (the 6 faces)
                                        viewDesc.arrayLayerCount = 6;
                                    } else if (viewDesc.dimension == WGPUTextureViewDimension_CubeArray) {
                                        // CubeArray must have multiple of 6 layers
                                        uint32_t maxLayers = it->second.depthOrArrayLayers > 0 ? it->second.depthOrArrayLayers : 6;
                                        viewDesc.arrayLayerCount = std::min(viewDesc.arrayLayerCount, maxLayers);
                                        // Round down to nearest multiple of 6
                                        viewDesc.arrayLayerCount = (viewDesc.arrayLayerCount / 6) * 6;
                                        if (viewDesc.arrayLayerCount < 6) viewDesc.arrayLayerCount = 6;
                                    }

                                    WGPUTextureView view = wgpuTextureCreateView(texture, &viewDesc);
        
                                    if (!view) {
                                        std::cerr << "[WebGPU] createView: Failed to create texture view" << std::endl;
                                        return g_engine->newUndefined();
                                    }

                                    auto jsView = g_engine->newObject();
                                    g_engine->setPrivateData(jsView, view);
                                    g_engine->setProperty(jsView, "_type", g_engine->newString("textureView"));
                                    if (gcReleaseEnabled()) {
                                        g_engine->registerRelease(jsView, [view]() {
                                            wgpuTextureViewRelease(view);
                                        });
                                    }

                                    return jsView;
                                })
                            );

                            // texture.destroy()
                            g_engine->setProperty(jsTexture, "destroy",
                                g_engine->newFunction("destroy", [textureId](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                    if (g_dawnFrameActive && !g_deferredQueueWork.empty()) {
                                        g_forcedFrameFlushes++;
                                        flushDeferredQueueWork();
                                    }
                                    releaseTexture(textureId, true);
                                    return g_engine->newUndefined();
                                })
                            );
                            if (gcReleaseEnabled()) {
                                g_engine->registerRelease(jsTexture, [textureId]() {
                                    releaseTexture(textureId, false);
                                });
                            }

                            if (g_verboseLogging) std::cout << "[WebGPU] Created texture " << width << "x" << height << " format=" << formatStr << " (id=" << textureId << ")" << std::endl;
                            return jsTexture;
                        })
                    );

                    // device.createSampler(descriptor?)
                    g_engine->setProperty(device, "createSampler",
                        g_engine->newFunction("createSampler", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            WGPUSamplerDescriptor samplerDesc = {};

                            // Default values
                            samplerDesc.addressModeU = WGPUAddressMode_ClampToEdge;
                            samplerDesc.addressModeV = WGPUAddressMode_ClampToEdge;
                            samplerDesc.addressModeW = WGPUAddressMode_ClampToEdge;
                            samplerDesc.magFilter = WGPUFilterMode_Nearest;
                            samplerDesc.minFilter = WGPUFilterMode_Nearest;
                            samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
                            samplerDesc.lodMinClamp = 0.0f;
                            samplerDesc.lodMaxClamp = 32.0f;
                            samplerDesc.maxAnisotropy = 1;

                            if (!args.empty()) {
                                auto descriptor = args[0];

                                auto addressModeU = g_engine->getProperty(descriptor, "addressModeU");
                                if (!g_engine->isUndefined(addressModeU)) {
                                    samplerDesc.addressModeU = stringToAddressMode(g_engine->toString(addressModeU));
                                }

                                auto addressModeV = g_engine->getProperty(descriptor, "addressModeV");
                                if (!g_engine->isUndefined(addressModeV)) {
                                    samplerDesc.addressModeV = stringToAddressMode(g_engine->toString(addressModeV));
                                }

                                auto addressModeW = g_engine->getProperty(descriptor, "addressModeW");
                                if (!g_engine->isUndefined(addressModeW)) {
                                    samplerDesc.addressModeW = stringToAddressMode(g_engine->toString(addressModeW));
                                }

                                auto magFilter = g_engine->getProperty(descriptor, "magFilter");
                                if (!g_engine->isUndefined(magFilter)) {
                                    samplerDesc.magFilter = stringToFilterMode(g_engine->toString(magFilter));
                                }

                                auto minFilter = g_engine->getProperty(descriptor, "minFilter");
                                if (!g_engine->isUndefined(minFilter)) {
                                    samplerDesc.minFilter = stringToFilterMode(g_engine->toString(minFilter));
                                }

                                auto mipmapFilter = g_engine->getProperty(descriptor, "mipmapFilter");
                                if (!g_engine->isUndefined(mipmapFilter)) {
                                    samplerDesc.mipmapFilter = stringToMipmapFilterMode(g_engine->toString(mipmapFilter));
                                }

                                auto lodMinClamp = g_engine->getProperty(descriptor, "lodMinClamp");
                                if (!g_engine->isUndefined(lodMinClamp)) {
                                    samplerDesc.lodMinClamp = (float)g_engine->toNumber(lodMinClamp);
                                }

                                auto lodMaxClamp = g_engine->getProperty(descriptor, "lodMaxClamp");
                                if (!g_engine->isUndefined(lodMaxClamp)) {
                                    samplerDesc.lodMaxClamp = (float)g_engine->toNumber(lodMaxClamp);
                                }

                                auto compare = g_engine->getProperty(descriptor, "compare");
                                if (!g_engine->isUndefined(compare)) {
                                    samplerDesc.compare = stringToCompareFunction(g_engine->toString(compare));
                                }

                                auto maxAnisotropy = g_engine->getProperty(descriptor, "maxAnisotropy");
                                if (!g_engine->isUndefined(maxAnisotropy)) {
                                    samplerDesc.maxAnisotropy = (uint16_t)g_engine->toNumber(maxAnisotropy);
                                }
                            }

                            WGPUSampler sampler = wgpuDeviceCreateSampler(g_device, &samplerDesc);

                            auto jsSampler = g_engine->newObject();
                            g_engine->setPrivateData(jsSampler, sampler);
                            g_engine->setProperty(jsSampler, "_type", g_engine->newString("sampler"));
                            if (gcReleaseEnabled()) {
                                g_engine->registerRelease(jsSampler, [sampler]() {
                                    wgpuSamplerRelease(sampler);
                                });
                            }

                            if (g_verboseLogging) std::cout << "[WebGPU] Created sampler" << std::endl;
                            return jsSampler;
                        })
                    );

                    // device.createBindGroupLayout(descriptor)
                    g_engine->setProperty(device, "createBindGroupLayout",
                        g_engine->newFunction("createBindGroupLayout", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (args.empty()) {
                                g_engine->throwException("createBindGroupLayout requires a descriptor");
                                return g_engine->newUndefined();
                            }

                            auto descriptor = args[0];
                            auto entries = g_engine->getProperty(descriptor, "entries");
                            auto lengthProp = g_engine->getProperty(entries, "length");
                            int entryCount = g_engine->isUndefined(lengthProp) ? 0 : (int)g_engine->toNumber(lengthProp);

                            std::vector<WGPUBindGroupLayoutEntry> layoutEntries;
                            layoutEntries.reserve(entryCount);

                            for (int i = 0; i < entryCount; i++) {
                                auto entry = g_engine->getPropertyIndex(entries, i);

                                WGPUBindGroupLayoutEntry layoutEntry = {};
                                layoutEntry.binding = (uint32_t)g_engine->toNumber(g_engine->getProperty(entry, "binding"));
                                layoutEntry.visibility = (WGPUShaderStage)(uint32_t)g_engine->toNumber(g_engine->getProperty(entry, "visibility"));

                                // Check for buffer binding
                                auto buffer = g_engine->getProperty(entry, "buffer");
                                if (!g_engine->isUndefined(buffer)) {
                                    auto typeProp = g_engine->getProperty(buffer, "type");
                                    std::string typeStr = g_engine->isUndefined(typeProp) ? "" : g_engine->toString(typeProp);
                                    if (typeStr == "uniform" || typeStr == "") {
                                        // Default to uniform if no type specified (Three.js uses empty {})
                                        layoutEntry.buffer.type = WGPUBufferBindingType_Uniform;
                                    } else if (typeStr == "storage") {
                                        layoutEntry.buffer.type = WGPUBufferBindingType_Storage;
                                    } else if (typeStr == "read-only-storage") {
                                        layoutEntry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
                                    } else {
                                        // Default to uniform for unknown types
                                        layoutEntry.buffer.type = WGPUBufferBindingType_Uniform;
                                    }
                                }

                                // Check for sampler binding
                                auto sampler = g_engine->getProperty(entry, "sampler");
                                if (!g_engine->isUndefined(sampler)) {
                                    std::string typeStr = g_engine->toString(g_engine->getProperty(sampler, "type"));
                                    if (typeStr == "filtering") {
                                        layoutEntry.sampler.type = WGPUSamplerBindingType_Filtering;
                                    } else if (typeStr == "non-filtering") {
                                        layoutEntry.sampler.type = WGPUSamplerBindingType_NonFiltering;
                                    } else if (typeStr == "comparison") {
                                        layoutEntry.sampler.type = WGPUSamplerBindingType_Comparison;
                                    } else {
                                        // Default to filtering
                                        layoutEntry.sampler.type = WGPUSamplerBindingType_Filtering;
                                    }
                                }

                                // Check for texture binding
                                auto texture = g_engine->getProperty(entry, "texture");
                                if (!g_engine->isUndefined(texture)) {
                                    auto sampleTypeProp = g_engine->getProperty(texture, "sampleType");
                                    std::string sampleType = g_engine->isUndefined(sampleTypeProp) ? "" : g_engine->toString(sampleTypeProp);
                                    if (sampleType == "float" || sampleType == "") {
                                        // Default to float if no type specified (Three.js uses empty {})
                                        layoutEntry.texture.sampleType = WGPUTextureSampleType_Float;
                                    } else if (sampleType == "unfilterable-float") {
                                        layoutEntry.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
                                    } else if (sampleType == "depth") {
                                        layoutEntry.texture.sampleType = WGPUTextureSampleType_Depth;
                                    } else if (sampleType == "sint") {
                                        layoutEntry.texture.sampleType = WGPUTextureSampleType_Sint;
                                    } else if (sampleType == "uint") {
                                        layoutEntry.texture.sampleType = WGPUTextureSampleType_Uint;
                                    } else {
                                        // Default to float for unknown types
                                        layoutEntry.texture.sampleType = WGPUTextureSampleType_Float;
                                    }

                                    auto viewDim = g_engine->getProperty(texture, "viewDimension");
                                    if (!g_engine->isUndefined(viewDim)) {
                                        layoutEntry.texture.viewDimension = stringToTextureViewDimension(g_engine->toString(viewDim));
                                    } else {
                                        layoutEntry.texture.viewDimension = WGPUTextureViewDimension_2D;
                                    }

                                    auto multisampled = g_engine->getProperty(texture, "multisampled");
                                    layoutEntry.texture.multisampled = !g_engine->isUndefined(multisampled) && g_engine->toBoolean(multisampled);
                                }

                                // Check for storageTexture binding
                                auto storageTexture = g_engine->getProperty(entry, "storageTexture");
                                if (!g_engine->isUndefined(storageTexture)) {
                                    std::string access = g_engine->toString(g_engine->getProperty(storageTexture, "access"));
                                    if (access == "write-only") {
                                        layoutEntry.storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
                                    } else if (access == "read-only") {
                                        layoutEntry.storageTexture.access = WGPUStorageTextureAccess_ReadOnly;
                                    } else if (access == "read-write") {
                                        layoutEntry.storageTexture.access = WGPUStorageTextureAccess_ReadWrite;
                                    }

                                    auto format = g_engine->getProperty(storageTexture, "format");
                                    if (!g_engine->isUndefined(format)) {
                                        layoutEntry.storageTexture.format = stringToFormat(g_engine->toString(format));
                                    }

                                    auto viewDim = g_engine->getProperty(storageTexture, "viewDimension");
                                    if (!g_engine->isUndefined(viewDim)) {
                                        layoutEntry.storageTexture.viewDimension = stringToTextureViewDimension(g_engine->toString(viewDim));
                                    } else {
                                        layoutEntry.storageTexture.viewDimension = WGPUTextureViewDimension_2D;
                                    }
                                }

                                layoutEntries.push_back(layoutEntry);
                            }

                            WGPUBindGroupLayoutDescriptor layoutDesc = {};
                            layoutDesc.entryCount = layoutEntries.size();
                            layoutDesc.entries = layoutEntries.data();

                            WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(g_device, &layoutDesc);

                            auto jsLayout = g_engine->newObject();
                            g_engine->setPrivateData(jsLayout, layout);
                            if (gcReleaseEnabled()) {
                                g_engine->registerRelease(jsLayout, [layout]() {
                                    wgpuBindGroupLayoutRelease(layout);
                                });
                            }

                            if (g_verboseLogging) std::cout << "[WebGPU] Created bind group layout with " << entryCount << " entries" << std::endl;
                            return jsLayout;
                        })
                    );

                    // device.createBindGroup(descriptor)
                    g_engine->setProperty(device, "createBindGroup",
                        g_engine->newFunction("createBindGroup", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (args.empty()) {
                                g_engine->throwException("createBindGroup requires a descriptor");
                                return g_engine->newUndefined();
                            }

                            auto descriptor = args[0];
                            auto layoutHandle = g_engine->getProperty(descriptor, "layout");
                            WGPUBindGroupLayout layout = (WGPUBindGroupLayout)g_engine->getPrivateData(layoutHandle);

                            auto entries = g_engine->getProperty(descriptor, "entries");
                            auto lengthProp = g_engine->getProperty(entries, "length");
                            int entryCount = g_engine->isUndefined(lengthProp) ? 0 : (int)g_engine->toNumber(lengthProp);

                            std::vector<WGPUBindGroupEntry> bindGroupEntries;
                            bindGroupEntries.reserve(entryCount);
                            std::vector<WGPUTextureView> autoCreatedViews;

                            for (int i = 0; i < entryCount; i++) {
                                auto entry = g_engine->getPropertyIndex(entries, i);

                                WGPUBindGroupEntry bgEntry = {};
                                bgEntry.binding = (uint32_t)g_engine->toNumber(g_engine->getProperty(entry, "binding"));

                                auto resource = g_engine->getProperty(entry, "resource");

                                // Check if resource is a sampler (has no buffer property)
                                auto bufferProp = g_engine->getProperty(resource, "buffer");
                                if (!g_engine->isUndefined(bufferProp)) {
                                    // Buffer binding: {buffer, offset?, size?}
                                    bgEntry.buffer = (WGPUBuffer)g_engine->getPrivateData(bufferProp);

                                    auto offset = g_engine->getProperty(resource, "offset");
                                    bgEntry.offset = g_engine->isUndefined(offset) ? 0 : (uint64_t)g_engine->toNumber(offset);

                                    auto size = g_engine->getProperty(resource, "size");
                                    // Size 0 means whole buffer
                                    bgEntry.size = g_engine->isUndefined(size) ? WGPU_WHOLE_SIZE : (uint64_t)g_engine->toNumber(size);
                                } else {
                                    // Could be a sampler or texture view
                                    void* resourcePtr = g_engine->getPrivateData(resource);

                                    // Check for type hints set when creating the object
                                    auto typeHint = g_engine->getProperty(resource, "_type");
                                    if (!g_engine->isUndefined(typeHint)) {
                                        std::string typeStr = g_engine->toString(typeHint);
                                        if (typeStr == "sampler") {
                                            if (resourcePtr) {
                                                bgEntry.sampler = (WGPUSampler)resourcePtr;
                                            } else {
                                                std::cerr << "[WebGPU] Warning: Sampler at binding " << bgEntry.binding << " is null" << std::endl;
                                            }
                                        } else if (typeStr == "textureView") {
                                            if (resourcePtr) {
                                                bgEntry.textureView = (WGPUTextureView)resourcePtr;
                                            } else {
                                                std::cerr << "[WebGPU] Warning: TextureView at binding " << bgEntry.binding << " is null, creating placeholder" << std::endl;
                                                // Create a 1x1 placeholder texture view to avoid validation errors
                                                // This is a workaround for textures that failed to create
                                            }
                                        }
                                    } else if (resourcePtr) {
                                        // No type hint - try to detect based on properties
                                        // Check if it looks like a texture (has width/height/format properties)
                                        auto widthProp = g_engine->getProperty(resource, "width");
                                        auto formatProp = g_engine->getProperty(resource, "format");
                                        if (!g_engine->isUndefined(widthProp) && !g_engine->isUndefined(formatProp)) {
                                            // This is a texture, create a view automatically
                                            WGPUTexture tex = (WGPUTexture)resourcePtr;
                                            WGPUTextureViewDescriptor viewDesc = {};
                                            WGPUTextureView view = wgpuTextureCreateView(tex, &viewDesc);
                
                                            autoCreatedViews.push_back(view);
                                            bgEntry.textureView = view;
                                            if (g_verboseLogging) std::cout << "[WebGPU] Auto-created texture view for binding " << bgEntry.binding << std::endl;
                                        } else {
                                            // Assume sampler as fallback
                                            bgEntry.sampler = (WGPUSampler)resourcePtr;
                                        }
                                    } else {
                                        std::cerr << "[WebGPU] Warning: Resource at binding " << bgEntry.binding << " has null privateData" << std::endl;
                                    }
                                }

                                bindGroupEntries.push_back(bgEntry);
                            }

                            WGPUBindGroupDescriptor bgDesc = {};
                            bgDesc.layout = layout;
                            bgDesc.entryCount = bindGroupEntries.size();
                            bgDesc.entries = bindGroupEntries.data();

                            WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(g_device, &bgDesc);

                            // Release auto-created texture views — Dawn holds its own
                            // internal references through the bind group
                            for (auto v : autoCreatedViews) {
                                wgpuTextureViewRelease(v);
                            }

                            auto jsBindGroup = g_engine->newObject();
                            g_engine->setPrivateData(jsBindGroup, bindGroup);
                            if (gcReleaseEnabled()) {
                                g_engine->registerRelease(jsBindGroup, [bindGroup]() {
                                    wgpuBindGroupRelease(bindGroup);
                                });
                            }

                            if (g_verboseLogging) std::cout << "[WebGPU] Created bind group with " << entryCount << " entries" << std::endl;
                            return jsBindGroup;
                        })
                    );

                    // device.createPipelineLayout(descriptor)
                    g_engine->setProperty(device, "createPipelineLayout",
                        g_engine->newFunction("createPipelineLayout", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (args.empty()) {
                                g_engine->throwException("createPipelineLayout requires a descriptor");
                                return g_engine->newUndefined();
                            }

                            auto descriptor = args[0];
                            auto bindGroupLayouts = g_engine->getProperty(descriptor, "bindGroupLayouts");
                            auto lengthProp = g_engine->getProperty(bindGroupLayouts, "length");
                            int layoutCount = g_engine->isUndefined(lengthProp) ? 0 : (int)g_engine->toNumber(lengthProp);

                            std::vector<WGPUBindGroupLayout> layouts;
                            layouts.reserve(layoutCount);

                            for (int i = 0; i < layoutCount; i++) {
                                auto layoutHandle = g_engine->getPropertyIndex(bindGroupLayouts, i);
                                WGPUBindGroupLayout layout = (WGPUBindGroupLayout)g_engine->getPrivateData(layoutHandle);
                                layouts.push_back(layout);
                            }

                            WGPUPipelineLayoutDescriptor layoutDesc = {};
                            layoutDesc.bindGroupLayoutCount = layouts.size();
                            layoutDesc.bindGroupLayouts = layouts.data();

                            WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(g_device, &layoutDesc);

                            auto jsLayout = g_engine->newObject();
                            g_engine->setPrivateData(jsLayout, pipelineLayout);
                            if (gcReleaseEnabled()) {
                                g_engine->registerRelease(jsLayout, [pipelineLayout]() {
                                    wgpuPipelineLayoutRelease(pipelineLayout);
                                });
                            }

                            if (g_verboseLogging) std::cout << "[WebGPU] Created pipeline layout with " << layoutCount << " bind group layouts" << std::endl;
                            return jsLayout;
                        })
                    );

                    // device.createTextureView(texture, descriptor?) - Non-standard helper
                    // Workaround because texture.createView() can't easily access 'this'
                    g_engine->setProperty(device, "createTextureView",
                        g_engine->newFunction("createTextureView", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (args.empty()) {
                                g_engine->throwException("createTextureView requires a texture");
                                return g_engine->newUndefined();
                            }

                            auto textureHandle = args[0];
                            WGPUTexture texture = (WGPUTexture)g_engine->getPrivateData(textureHandle);

                            if (!texture) {
                                g_engine->throwException("createTextureView: invalid texture");
                                return g_engine->newUndefined();
                            }

                            // Get texture info
                            double formatEnum = g_engine->toNumber(g_engine->getProperty(textureHandle, "_formatEnum"));
                            WGPUTextureFormat format = formatEnum == 0 ? g_surfaceFormat : (WGPUTextureFormat)(int)formatEnum;

                            // Get format from _textureId if available
                            auto textureIdVal = g_engine->getProperty(textureHandle, "_textureId");
                            if (!g_engine->isUndefined(textureIdVal)) {
                                uint64_t textureId = (uint64_t)g_engine->toNumber(textureIdVal);
                                auto it = g_textureRegistry.find(textureId);
                                if (it != g_textureRegistry.end()) {
                                    format = it->second.format;
                                }
                            }

                            WGPUTextureViewDescriptor viewDesc = {};
                            viewDesc.format = format;
                            viewDesc.dimension = WGPUTextureViewDimension_2D;
                            viewDesc.baseMipLevel = 0;
                            viewDesc.mipLevelCount = 1;
                            viewDesc.baseArrayLayer = 0;
                            viewDesc.arrayLayerCount = 1;
                            viewDesc.aspect = WGPUTextureAspect_All;

                            // Parse descriptor if provided
                            if (args.size() > 1 && !g_engine->isUndefined(args[1])) {
                                auto descriptor = args[1];

                                auto formatProp = g_engine->getProperty(descriptor, "format");
                                if (!g_engine->isUndefined(formatProp)) {
                                    viewDesc.format = stringToFormat(g_engine->toString(formatProp));
                                }

                                auto dimensionProp = g_engine->getProperty(descriptor, "dimension");
                                if (!g_engine->isUndefined(dimensionProp)) {
                                    viewDesc.dimension = stringToTextureViewDimension(g_engine->toString(dimensionProp));
                                }

                                auto baseMipLevel = g_engine->getProperty(descriptor, "baseMipLevel");
                                if (!g_engine->isUndefined(baseMipLevel)) {
                                    viewDesc.baseMipLevel = (uint32_t)g_engine->toNumber(baseMipLevel);
                                }

                                auto mipLevelCount = g_engine->getProperty(descriptor, "mipLevelCount");
                                if (!g_engine->isUndefined(mipLevelCount)) {
                                    viewDesc.mipLevelCount = (uint32_t)g_engine->toNumber(mipLevelCount);
                                }

                                auto baseArrayLayer = g_engine->getProperty(descriptor, "baseArrayLayer");
                                if (!g_engine->isUndefined(baseArrayLayer)) {
                                    viewDesc.baseArrayLayer = (uint32_t)g_engine->toNumber(baseArrayLayer);
                                }

                                auto arrayLayerCount = g_engine->getProperty(descriptor, "arrayLayerCount");
                                if (!g_engine->isUndefined(arrayLayerCount)) {
                                    uint32_t requested = (uint32_t)g_engine->toNumber(arrayLayerCount);
                                    // Clamp to 1 for surface textures (which only have 1 layer)
                                    // or look up actual layer count from registry
                                    auto textureIdVal2 = g_engine->getProperty(textureHandle, "_textureId");
                                    uint32_t maxLayers = 1;
                                    if (!g_engine->isUndefined(textureIdVal2)) {
                                        uint64_t tid = (uint64_t)g_engine->toNumber(textureIdVal2);
                                        auto it = g_textureRegistry.find(tid);
                                        if (it != g_textureRegistry.end()) {
                                            maxLayers = it->second.depthOrArrayLayers > 0 ? it->second.depthOrArrayLayers : 1;
                                        }
                                    }
                                    viewDesc.arrayLayerCount = std::min(requested, maxLayers - viewDesc.baseArrayLayer);
                                }

                                auto aspect = g_engine->getProperty(descriptor, "aspect");
                                if (!g_engine->isUndefined(aspect)) {
                                    std::string aspectStr = g_engine->toString(aspect);
                                    if (aspectStr == "all") viewDesc.aspect = WGPUTextureAspect_All;
                                    else if (aspectStr == "stencil-only") viewDesc.aspect = WGPUTextureAspect_StencilOnly;
                                    else if (aspectStr == "depth-only") viewDesc.aspect = WGPUTextureAspect_DepthOnly;
                                }
                            }

                            // Final validation: Fix arrayLayerCount based on view dimension
                            if (viewDesc.dimension == WGPUTextureViewDimension_3D ||
                                viewDesc.dimension == WGPUTextureViewDimension_1D) {
                                viewDesc.arrayLayerCount = 1;
                            } else if (viewDesc.dimension == WGPUTextureViewDimension_Cube) {
                                viewDesc.arrayLayerCount = 6;
                            }

                            WGPUTextureView view = wgpuTextureCreateView(texture, &viewDesc);

                            auto jsView = g_engine->newObject();
                            g_engine->setPrivateData(jsView, view);
                            g_engine->setProperty(jsView, "_type", g_engine->newString("textureView"));
                            if (gcReleaseEnabled()) {
                                g_engine->registerRelease(jsView, [view]() {
                                    wgpuTextureViewRelease(view);
                                });
                            }

                            if (g_verboseLogging) std::cout << "[WebGPU] Created texture view" << std::endl;
                            return jsView;
                        })
                    );

                    // device.createRenderBundleEncoder(descriptor)
                    // Used by Three.js for mipmap generation
                    g_engine->setProperty(device, "createRenderBundleEncoder",
                        g_engine->newFunction("createRenderBundleEncoder", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (args.empty()) {
                                g_engine->throwException("createRenderBundleEncoder requires a descriptor");
                                return g_engine->newUndefined();
                            }

                            auto descriptor = args[0];

                            // Parse color formats
                            auto colorFormats = g_engine->getProperty(descriptor, "colorFormats");
                            auto colorFormatsLength = g_engine->getProperty(colorFormats, "length");
                            int colorFormatCount = g_engine->isUndefined(colorFormatsLength) ? 0 : (int)g_engine->toNumber(colorFormatsLength);

                            std::vector<WGPUTextureFormat> formats;
                            formats.reserve(colorFormatCount);
                            for (int i = 0; i < colorFormatCount; i++) {
                                auto formatProp = g_engine->getPropertyIndex(colorFormats, i);
                                if (!g_engine->isUndefined(formatProp) && !g_engine->isNull(formatProp)) {
                                    formats.push_back(stringToFormat(g_engine->toString(formatProp)));
                                }
                            }

                            // Parse depth stencil format
                            WGPUTextureFormat depthFormat = WGPUTextureFormat_Undefined;
                            auto depthFormatProp = g_engine->getProperty(descriptor, "depthStencilFormat");
                            if (!g_engine->isUndefined(depthFormatProp) && !g_engine->isNull(depthFormatProp)) {
                                depthFormat = stringToFormat(g_engine->toString(depthFormatProp));
                            }

                            // Parse sample count
                            uint32_t sampleCount = 1;
                            auto sampleCountProp = g_engine->getProperty(descriptor, "sampleCount");
                            if (!g_engine->isUndefined(sampleCountProp)) {
                                sampleCount = (uint32_t)g_engine->toNumber(sampleCountProp);
                            }

                            WGPURenderBundleEncoderDescriptor desc = {};
                            desc.colorFormatCount = formats.size();
                            desc.colorFormats = formats.data();
                            desc.depthStencilFormat = depthFormat;
                            desc.sampleCount = sampleCount;

                            WGPURenderBundleEncoder bundleEncoder = wgpuDeviceCreateRenderBundleEncoder(g_device, &desc);

                            if (!bundleEncoder) {
                                g_engine->throwException("Failed to create render bundle encoder");
                                return g_engine->newUndefined();
                            }

                            auto jsEncoder = g_engine->newObject();
                            g_engine->setPrivateData(jsEncoder, bundleEncoder);

                            // Capture for closures
                            WGPURenderBundleEncoder capturedEncoder = bundleEncoder;
                            auto bundleEncoderAlive = std::make_shared<bool>(true);
                            if (gcReleaseEnabled()) {
                                g_engine->registerRelease(jsEncoder, [capturedEncoder, bundleEncoderAlive]() {
                                    if (!*bundleEncoderAlive) return;
                                    *bundleEncoderAlive = false;
                                    wgpuRenderBundleEncoderRelease(capturedEncoder);
                                });
                            }

                            // renderBundleEncoder.setPipeline(pipeline)
                            g_engine->setProperty(jsEncoder, "setPipeline",
                                g_engine->newFunction("setPipeline", [capturedEncoder](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                    if (args.empty()) return g_engine->newUndefined();
                                    WGPURenderPipeline pipeline = (WGPURenderPipeline)g_engine->getPrivateData(args[0]);
                                    wgpuRenderBundleEncoderSetPipeline(capturedEncoder, pipeline);
                                    return g_engine->newUndefined();
                                })
                            );

                            // renderBundleEncoder.setVertexBuffer(slot, buffer, offset?, size?)
                            g_engine->setProperty(jsEncoder, "setVertexBuffer",
                                g_engine->newFunction("setVertexBuffer", [capturedEncoder](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                    if (args.size() < 2) return g_engine->newUndefined();
                                    uint32_t slot = (uint32_t)g_engine->toNumber(args[0]);
                                    WGPUBuffer buffer = (WGPUBuffer)g_engine->getPrivateData(args[1]);
                                    uint64_t offset = args.size() > 2 && !g_engine->isUndefined(args[2]) ? (uint64_t)g_engine->toNumber(args[2]) : 0;
                                    uint64_t size = args.size() > 3 && !g_engine->isUndefined(args[3]) ? (uint64_t)g_engine->toNumber(args[3]) : WGPU_WHOLE_SIZE;
                                    wgpuRenderBundleEncoderSetVertexBuffer(capturedEncoder, slot, buffer, offset, size);
                                    return g_engine->newUndefined();
                                })
                            );

                            // renderBundleEncoder.setIndexBuffer(buffer, format, offset?, size?)
                            g_engine->setProperty(jsEncoder, "setIndexBuffer",
                                g_engine->newFunction("setIndexBuffer", [capturedEncoder](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                    if (args.size() < 2) return g_engine->newUndefined();
                                    WGPUBuffer buffer = (WGPUBuffer)g_engine->getPrivateData(args[0]);
                                    std::string formatStr = g_engine->toString(args[1]);
                                    WGPUIndexFormat format = formatStr == "uint32" ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16;
                                    uint64_t offset = args.size() > 2 && !g_engine->isUndefined(args[2]) ? (uint64_t)g_engine->toNumber(args[2]) : 0;
                                    uint64_t size = args.size() > 3 && !g_engine->isUndefined(args[3]) ? (uint64_t)g_engine->toNumber(args[3]) : WGPU_WHOLE_SIZE;
                                    wgpuRenderBundleEncoderSetIndexBuffer(capturedEncoder, buffer, format, offset, size);
                                    return g_engine->newUndefined();
                                })
                            );

                            // renderBundleEncoder.setBindGroup(index, bindGroup, dynamicOffsets?)
                            g_engine->setProperty(jsEncoder, "setBindGroup",
                                g_engine->newFunction("setBindGroup", [capturedEncoder](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                    if (args.size() < 2) return g_engine->newUndefined();
                                    uint32_t index = (uint32_t)g_engine->toNumber(args[0]);
                                    WGPUBindGroup bindGroup = (WGPUBindGroup)g_engine->getPrivateData(args[1]);

                                    // Parse dynamic offsets if provided
                                    std::vector<uint32_t> dynamicOffsets;
                                    if (args.size() > 2 && !g_engine->isUndefined(args[2])) {
                                        auto offsetsArray = args[2];
                                        auto lengthProp = g_engine->getProperty(offsetsArray, "length");
                                        int offsetCount = g_engine->isUndefined(lengthProp) ? 0 : (int)g_engine->toNumber(lengthProp);
                                        for (int i = 0; i < offsetCount; i++) {
                                            dynamicOffsets.push_back((uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(offsetsArray, i)));
                                        }
                                    }

                                    wgpuRenderBundleEncoderSetBindGroup(capturedEncoder, index, bindGroup, dynamicOffsets.size(), dynamicOffsets.data());
                                    return g_engine->newUndefined();
                                })
                            );

                            // renderBundleEncoder.draw(vertexCount, instanceCount?, firstVertex?, firstInstance?)
                            g_engine->setProperty(jsEncoder, "draw",
                                g_engine->newFunction("draw", [capturedEncoder](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                    if (args.empty()) return g_engine->newUndefined();
                                    uint32_t vertexCount = (uint32_t)g_engine->toNumber(args[0]);
                                    uint32_t instanceCount = args.size() > 1 ? (uint32_t)g_engine->toNumber(args[1]) : 1;
                                    uint32_t firstVertex = args.size() > 2 ? (uint32_t)g_engine->toNumber(args[2]) : 0;
                                    uint32_t firstInstance = args.size() > 3 ? (uint32_t)g_engine->toNumber(args[3]) : 0;
                                    wgpuRenderBundleEncoderDraw(capturedEncoder, vertexCount, instanceCount, firstVertex, firstInstance);
                                    return g_engine->newUndefined();
                                })
                            );

                            // renderBundleEncoder.drawIndexed(indexCount, instanceCount?, firstIndex?, baseVertex?, firstInstance?)
                            g_engine->setProperty(jsEncoder, "drawIndexed",
                                g_engine->newFunction("drawIndexed", [capturedEncoder](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                    if (args.empty()) return g_engine->newUndefined();
                                    uint32_t indexCount = (uint32_t)g_engine->toNumber(args[0]);
                                    uint32_t instanceCount = args.size() > 1 ? (uint32_t)g_engine->toNumber(args[1]) : 1;
                                    uint32_t firstIndex = args.size() > 2 ? (uint32_t)g_engine->toNumber(args[2]) : 0;
                                    int32_t baseVertex = args.size() > 3 ? (int32_t)g_engine->toNumber(args[3]) : 0;
                                    uint32_t firstInstance = args.size() > 4 ? (uint32_t)g_engine->toNumber(args[4]) : 0;
                                    wgpuRenderBundleEncoderDrawIndexed(capturedEncoder, indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
                                    return g_engine->newUndefined();
                                })
                            );

                            // renderBundleEncoder.finish(descriptor?)
                            g_engine->setProperty(jsEncoder, "finish",
                                g_engine->newFunction("finish", [capturedEncoder, bundleEncoderAlive](void* ctx, const std::vector<js::JSValueHandle>& args) {
                                    if (!*bundleEncoderAlive) {
                                        g_engine->throwException("Render bundle encoder is already finished");
                                        return g_engine->newUndefined();
                                    }
                                    WGPURenderBundleDescriptor desc = {};
                                    WGPURenderBundle bundle = wgpuRenderBundleEncoderFinish(capturedEncoder, &desc);
                                    *bundleEncoderAlive = false;
                                    wgpuRenderBundleEncoderRelease(capturedEncoder);

                                    auto jsBundle = g_engine->newObject();
                                    g_engine->setPrivateData(jsBundle, bundle);
                                    g_engine->setProperty(jsBundle, "_type", g_engine->newString("renderBundle"));
                                    if (bundle && gcReleaseEnabled()) {
                                        g_engine->registerRelease(jsBundle, [bundle]() {
                                            wgpuRenderBundleRelease(bundle);
                                        });
                                    }

                                    if (g_verboseLogging) std::cout << "[WebGPU] Render bundle finished" << std::endl;
                                    return jsBundle;
                                })
                            );

                            if (g_verboseLogging) std::cout << "[WebGPU] Created render bundle encoder" << std::endl;
                            return jsEncoder;
                        })
                    );

                    // device.pushErrorScope(filter) - Push an error scope for validation/OOM/internal errors
                    // Used by Three.js for error handling during pipeline creation
                    g_engine->setProperty(device, "pushErrorScope",
                        g_engine->newFunction("pushErrorScope", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            // In native runtime, we can use Dawn's error scope API
                            // For now, just no-op since Dawn reports errors to the error callback
                            if (g_verboseLogging) {
                                std::string filter = args.empty() ? "validation" : g_engine->toString(args[0]);
                                std::cout << "[WebGPU] pushErrorScope: " << filter << std::endl;
                            }
                            return g_engine->newUndefined();
                        })
                    );

                    // device.popErrorScope() - Pop an error scope and return Promise<GPUError | null>
                    // Returns Promise<GPUError | null>
                    g_engine->setProperty(device, "popErrorScope",
                        g_engine->newFunction("popErrorScope", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            // Return a Promise that resolves to null (no error)
                            // In a full implementation, this would check for actual errors
                            if (g_verboseLogging) {
                                std::cout << "[WebGPU] popErrorScope" << std::endl;
                            }
                            // Use evalScriptWithResult (not evalWithResult) to get the actual Promise value
                            // evalWithResult uses module mode which returns module evaluation result, not expression value
                            return g_engine->evalScriptWithResult("Promise.resolve(null)", "popErrorScope");
                        })
                    );

                    // device.lost - Promise that resolves when the device is lost
                    // Required by Three.js WebGPU renderer during init
                    // We create a Promise that never resolves (device never lost in normal operation)
                    auto deviceLostPromise = g_engine->evalWithResult(
                        "new Promise(function(resolve) { globalThis.__mystral_device_lost_resolve = resolve; })",
                        "device.lost"
                    );
                    g_engine->setProperty(device, "lost", deviceLostPromise);

                    // Return the device directly
                    // await on a non-Promise just returns the value
                    return device;
                })
            );

            // adapter.features - reflect what the native adapter actually
            // exposes (queried through wgpuAdapterHasFeature, not hardcoded)
            g_engine->setProperty(adapter, "features", buildFeaturesObject(adapterHasFeature));

            // adapter.limits - the adapter's real limits (falls back to the
            // device when the adapter handle was not provided)
            auto limits = g_engine->newObject();
            WGPULimits realAdapterLimits = {};
            if (queryAdapterLimits(&realAdapterLimits)) {
                setLimitsProperties(limits, realAdapterLimits);
            }
            g_engine->setProperty(adapter, "limits", limits);

            // Return the adapter directly
            // await on a non-Promise just returns the value
            // Three.js: const adapter = await navigator.gpu.requestAdapter()
            // This works whether we return a Promise or the adapter directly
            return adapter;
        })
    );

    // navigator.gpu.getPreferredCanvasFormat()
    engine->setProperty(gpuObject, "getPreferredCanvasFormat",
        engine->newFunction("getPreferredCanvasFormat", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return g_engine->newString(formatToString(g_surfaceFormat));
        })
    );

    // Set navigator.gpu
    engine->setProperty(navigatorHandle, "gpu", gpuObject);

    // ========================================================================
    // GPU Usage Constants
    // ========================================================================
    auto gpuBufferUsage = engine->newObject();
    engine->setProperty(gpuBufferUsage, "MAP_READ", engine->newNumber(0x0001));
    engine->setProperty(gpuBufferUsage, "MAP_WRITE", engine->newNumber(0x0002));
    engine->setProperty(gpuBufferUsage, "COPY_SRC", engine->newNumber(0x0004));
    engine->setProperty(gpuBufferUsage, "COPY_DST", engine->newNumber(0x0008));
    engine->setProperty(gpuBufferUsage, "INDEX", engine->newNumber(0x0010));
    engine->setProperty(gpuBufferUsage, "VERTEX", engine->newNumber(0x0020));
    engine->setProperty(gpuBufferUsage, "UNIFORM", engine->newNumber(0x0040));
    engine->setProperty(gpuBufferUsage, "STORAGE", engine->newNumber(0x0080));
    engine->setProperty(gpuBufferUsage, "INDIRECT", engine->newNumber(0x0100));
    engine->setProperty(gpuBufferUsage, "QUERY_RESOLVE", engine->newNumber(0x0200));
    engine->setGlobalProperty("GPUBufferUsage", gpuBufferUsage);

    auto gpuTextureUsage = engine->newObject();
    engine->setProperty(gpuTextureUsage, "COPY_SRC", engine->newNumber(0x01));
    engine->setProperty(gpuTextureUsage, "COPY_DST", engine->newNumber(0x02));
    engine->setProperty(gpuTextureUsage, "TEXTURE_BINDING", engine->newNumber(0x04));
    engine->setProperty(gpuTextureUsage, "STORAGE_BINDING", engine->newNumber(0x08));
    engine->setProperty(gpuTextureUsage, "RENDER_ATTACHMENT", engine->newNumber(0x10));
    engine->setGlobalProperty("GPUTextureUsage", gpuTextureUsage);

    auto gpuShaderStage = engine->newObject();
    engine->setProperty(gpuShaderStage, "VERTEX", engine->newNumber(0x1));
    engine->setProperty(gpuShaderStage, "FRAGMENT", engine->newNumber(0x2));
    engine->setProperty(gpuShaderStage, "COMPUTE", engine->newNumber(0x4));
    engine->setGlobalProperty("GPUShaderStage", gpuShaderStage);

    auto gpuMapMode = engine->newObject();
    engine->setProperty(gpuMapMode, "READ", engine->newNumber(0x1));
    engine->setProperty(gpuMapMode, "WRITE", engine->newNumber(0x2));
    engine->setGlobalProperty("GPUMapMode", gpuMapMode);

    // =========================================================================
    // createImageBitmap() - Standard Web API for image decoding
    // =========================================================================
    // createImageBitmap(source) -> Promise<ImageBitmap>
    // source can be: Blob, ArrayBuffer, or object with arrayBuffer() method
    // Returns ImageBitmap with: width, height, close(), and internal pixel data
    //
    // Note: PNG/JPEG supported via stb_image. WebP supported via libwebp (when MYSTRAL_HAS_WEBP defined).

    // Native helper: copy the encoded bytes on the runtime thread, decode on
    // the shared job system, then create the JS result back on this thread.
    engine->setGlobalProperty("__decodeImageDataAsync",
        engine->newFunction("__decodeImageDataAsync", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                return rejectedPromise("Image decode requires an ArrayBuffer argument");
            }

            size_t inputSize = 0;
            void* inputData = g_engine->getArrayBufferData(args[0], &inputSize);
            if (!inputData || inputSize == 0) {
                return rejectedPromise("Image decode received an invalid ArrayBuffer");
            }

            auto image = std::make_shared<DecodedImageData>();
            const auto* bytes = static_cast<const uint8_t*>(inputData);
            image->encoded.assign(bytes, bytes + inputSize);

            js::JSValueHandle promise;
            auto* pending = createPendingPromise(promise);
            if (!pending) return g_engine->newUndefined();

            auto handle = async::getJobSystem().submit(
                async::JobPriority::Streaming,
                g_imageDecodeGeneration,
                [image](const async::JobContext& job) {
                    decodeImageData(job, *image);
                },
                [image, pending](async::JobStatus status) {
                    if (!pending->active || pending->session != g_asyncSession || pending->engine != g_engine) {
                        settlePendingPromise(pending, false, {});
                        return;
                    }
                    if (status == async::JobStatus::Cancelled) {
                        settlePendingPromise(pending, false, {}, "Image decode was cancelled");
                        return;
                    }
                    if (status == async::JobStatus::Failed && image->error.empty()) {
                        image->error = "Image decode job failed";
                    }
                    if (!image->error.empty() || image->rgba.empty()) {
                        settlePendingPromise(
                            pending,
                            false,
                            {},
                            image->error.empty() ? "Image decode produced no pixels" : image->error);
                        return;
                    }

                    auto result = g_engine->newObject();
                    auto pixels = g_engine->newArrayBuffer(image->rgba.data(), image->rgba.size());
                    g_engine->setProperty(result, "width", g_engine->newNumber(image->width));
                    g_engine->setProperty(result, "height", g_engine->newNumber(image->height));
                    g_engine->setProperty(result, "_data", pixels);
                    g_engine->setProperty(result, "_closed", g_engine->newBoolean(false));
                    if (g_verboseLogging) {
                        std::cout << "[createImageBitmap] Decoded "
                                  << image->width << "x" << image->height
                                  << " image asynchronously" << std::endl;
                    }
                    settlePendingPromise(pending, true, result);
                });

            if (!handle) {
                settlePendingPromise(
                    pending,
                    false,
                    {},
                    "Image decode queue is full or shutting down");
            }
            return promise;
        })
    );

    // JavaScript polyfill for createImageBitmap
    const char* imageBitmapPolyfill = R"(
// ImageBitmap class (web-compatible)
class ImageBitmap {
    constructor(width, height, data) {
        this.width = width;
        this.height = height;
        this._data = data;  // Internal RGBA pixel data
        this._closed = false;
    }

    close() {
        this._closed = true;
        this._data = null;
    }
}

// createImageBitmap - Standard Web API
// Supports: Blob, ArrayBuffer, Response, or object with arrayBuffer() method
async function createImageBitmap(source, options) {
    let arrayBuffer;

    if (source instanceof ArrayBuffer) {
        arrayBuffer = source;
    } else if (ArrayBuffer.isView(source)) {
        arrayBuffer = source.buffer.slice(source.byteOffset, source.byteOffset + source.byteLength);
    } else if (source && typeof source.arrayBuffer === 'function') {
        // Blob or Response
        arrayBuffer = await source.arrayBuffer();
    } else if (source && source._data) {
        // Already an ImageBitmap-like object
        return source;
    } else {
        throw new Error('createImageBitmap: unsupported source type');
    }

    // Decode using native function
    const decoded = await __decodeImageDataAsync(arrayBuffer);

    if (!decoded) {
        throw new Error('createImageBitmap: failed to decode image');
    }

    // Create ImageBitmap
    const bitmap = new ImageBitmap(decoded.width, decoded.height, decoded._data);
    return bitmap;
}

globalThis.createImageBitmap = createImageBitmap;
globalThis.ImageBitmap = ImageBitmap;

// CanvasRenderingContext2D - Placeholder class for instanceof checks
// The actual implementation is in Canvas2D bindings, this is just for type checking
class CanvasRenderingContext2D {
    constructor() {
        // This constructor is never called directly - contexts are created via getContext('2d')
    }
}
globalThis.CanvasRenderingContext2D = CanvasRenderingContext2D;

// HTMLCanvasElement - Placeholder class for instanceof checks
class HTMLCanvasElement {
    constructor() {}
}
globalThis.HTMLCanvasElement = HTMLCanvasElement;

// OffscreenCanvas - For type checking
class OffscreenCanvas {
    constructor(width, height) {
        this.width = width || 300;
        this.height = height || 150;
        this._contextType = null;
        this._context = null;
    }

    getContext(type, options) {
        if (type === '2d') {
            // For basic 2D context needs
            if (!this._context) {
                this._context = { canvas: this };
            }
            return this._context;
        }
        return null;
    }
}
globalThis.OffscreenCanvas = OffscreenCanvas;
)";
    engine->eval(imageBitmapPolyfill, "imageBitmap-polyfill.js");

    // =========================================================================
    // Mystral.loadGLTF() - GLTF/GLB file loader
    // =========================================================================
    // Returns parsed GLTF data as JavaScript object
    auto mystralNamespace = engine->newObject();

    engine->setProperty(mystralNamespace, "loadGLTF",
        engine->newFunction("loadGLTF", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                g_engine->throwException("loadGLTF requires a file path argument");
                return g_engine->newUndefined();
            }

            std::string path = g_engine->toString(args[0]);
            if (g_verboseLogging) std::cout << "[GLTF] Loading: " << path << std::endl;

            auto gltfData = mystral::gltf::loadGLTF(path);
            if (!gltfData) {
                g_engine->throwException(("Failed to load GLTF file: " + path).c_str());
                return g_engine->newUndefined();
            }

            // Convert to JavaScript object
            auto result = g_engine->newObject();

            // Meshes array
            auto jsMeshes = g_engine->newArray();
            for (size_t mi = 0; mi < gltfData->meshes.size(); mi++) {
                const auto& mesh = gltfData->meshes[mi];
                auto jsMesh = g_engine->newObject();
                g_engine->setProperty(jsMesh, "name", g_engine->newString(mesh.name.c_str()));

                // Primitives array
                auto jsPrimitives = g_engine->newArray();
                for (size_t pi = 0; pi < mesh.primitives.size(); pi++) {
                    const auto& prim = mesh.primitives[pi];
                    auto jsPrim = g_engine->newObject();

                    // Positions Float32Array
                    if (!prim.positions.data.empty()) {
                        auto posArr = g_engine->createFloat32Array(
                            prim.positions.data.data(),
                            prim.positions.data.size()
                        );
                        g_engine->setProperty(jsPrim, "positions", posArr);
                        g_engine->setProperty(jsPrim, "positionCount",
                            g_engine->newNumber((double)prim.positions.count));
                    }

                    // Normals Float32Array
                    if (!prim.normals.data.empty()) {
                        auto normArr = g_engine->createFloat32Array(
                            prim.normals.data.data(),
                            prim.normals.data.size()
                        );
                        g_engine->setProperty(jsPrim, "normals", normArr);
                    }

                    // Texcoords Float32Array
                    if (!prim.texcoords.data.empty()) {
                        auto uvArr = g_engine->createFloat32Array(
                            prim.texcoords.data.data(),
                            prim.texcoords.data.size()
                        );
                        g_engine->setProperty(jsPrim, "texcoords", uvArr);
                    }

                    // Tangents Float32Array
                    if (!prim.tangents.data.empty()) {
                        auto tanArr = g_engine->createFloat32Array(
                            prim.tangents.data.data(),
                            prim.tangents.data.size()
                        );
                        g_engine->setProperty(jsPrim, "tangents", tanArr);
                    }

                    // Indices Uint32Array
                    if (!prim.indices.empty()) {
                        auto idxArr = g_engine->createUint32Array(
                            prim.indices.data(),
                            prim.indices.size()
                        );
                        g_engine->setProperty(jsPrim, "indices", idxArr);
                        g_engine->setProperty(jsPrim, "indexCount",
                            g_engine->newNumber((double)prim.indices.size()));
                    }

                    g_engine->setProperty(jsPrim, "materialIndex",
                        g_engine->newNumber((double)prim.materialIndex));

                    g_engine->setPropertyIndex(jsPrimitives, pi, jsPrim);
                }
                g_engine->setProperty(jsMesh, "primitives", jsPrimitives);
                g_engine->setPropertyIndex(jsMeshes, mi, jsMesh);
            }
            g_engine->setProperty(result, "meshes", jsMeshes);

            // Materials array
            auto jsMaterials = g_engine->newArray();
            for (size_t mi = 0; mi < gltfData->materials.size(); mi++) {
                const auto& mat = gltfData->materials[mi];
                auto jsMat = g_engine->newObject();
                g_engine->setProperty(jsMat, "name", g_engine->newString(mat.name.c_str()));

                // PBR factors
                auto baseColor = g_engine->newArray();
                for (int i = 0; i < 4; i++) {
                    g_engine->setPropertyIndex(baseColor, i, g_engine->newNumber(mat.baseColorFactor[i]));
                }
                g_engine->setProperty(jsMat, "baseColorFactor", baseColor);
                g_engine->setProperty(jsMat, "metallicFactor", g_engine->newNumber(mat.metallicFactor));
                g_engine->setProperty(jsMat, "roughnessFactor", g_engine->newNumber(mat.roughnessFactor));

                // Emissive
                auto emissive = g_engine->newArray();
                for (int i = 0; i < 3; i++) {
                    g_engine->setPropertyIndex(emissive, i, g_engine->newNumber(mat.emissiveFactor[i]));
                }
                g_engine->setProperty(jsMat, "emissiveFactor", emissive);

                // Texture indices
                g_engine->setProperty(jsMat, "baseColorTextureIndex",
                    g_engine->newNumber(mat.baseColorTexture.imageIndex));
                g_engine->setProperty(jsMat, "metallicRoughnessTextureIndex",
                    g_engine->newNumber(mat.metallicRoughnessTexture.imageIndex));
                g_engine->setProperty(jsMat, "normalTextureIndex",
                    g_engine->newNumber(mat.normalTexture.imageIndex));
                g_engine->setProperty(jsMat, "occlusionTextureIndex",
                    g_engine->newNumber(mat.occlusionTexture.imageIndex));
                g_engine->setProperty(jsMat, "emissiveTextureIndex",
                    g_engine->newNumber(mat.emissiveTexture.imageIndex));

                g_engine->setProperty(jsMat, "normalScale", g_engine->newNumber(mat.normalScale));
                g_engine->setProperty(jsMat, "occlusionStrength", g_engine->newNumber(mat.occlusionStrength));
                g_engine->setProperty(jsMat, "alphaCutoff", g_engine->newNumber(mat.alphaCutoff));
                g_engine->setProperty(jsMat, "doubleSided", g_engine->newBoolean(mat.doubleSided));

                const char* alphaModeStr = "OPAQUE";
                if (mat.alphaMode == mystral::gltf::MaterialData::AlphaMode::Mask) alphaModeStr = "MASK";
                else if (mat.alphaMode == mystral::gltf::MaterialData::AlphaMode::Blend) alphaModeStr = "BLEND";
                g_engine->setProperty(jsMat, "alphaMode", g_engine->newString(alphaModeStr));

                g_engine->setPropertyIndex(jsMaterials, mi, jsMat);
            }
            g_engine->setProperty(result, "materials", jsMaterials);

            // Images array (with embedded data as ArrayBuffers)
            auto jsImages = g_engine->newArray();
            for (size_t ii = 0; ii < gltfData->images.size(); ii++) {
                const auto& img = gltfData->images[ii];
                auto jsImg = g_engine->newObject();
                g_engine->setProperty(jsImg, "name", g_engine->newString(img.name.c_str()));
                g_engine->setProperty(jsImg, "uri", g_engine->newString(img.uri.c_str()));
                g_engine->setProperty(jsImg, "mimeType", g_engine->newString(img.mimeType.c_str()));

                // Embedded image data as ArrayBuffer
                if (!img.data.empty()) {
                    auto dataArr = g_engine->createUint8Array(
                        img.data.data(),
                        img.data.size()
                    );
                    g_engine->setProperty(jsImg, "data", dataArr);
                }

                g_engine->setPropertyIndex(jsImages, ii, jsImg);
            }
            g_engine->setProperty(result, "images", jsImages);

            // Nodes array
            auto jsNodes = g_engine->newArray();
            for (size_t ni = 0; ni < gltfData->nodes.size(); ni++) {
                const auto& node = gltfData->nodes[ni];
                auto jsNode = g_engine->newObject();
                g_engine->setProperty(jsNode, "name", g_engine->newString(node.name.c_str()));
                g_engine->setProperty(jsNode, "meshIndex", g_engine->newNumber(node.meshIndex));

                // Transform - store as separate arrays
                auto translation = g_engine->newArray();
                auto rotation = g_engine->newArray();
                auto scale = g_engine->newArray();
                for (int i = 0; i < 3; i++) {
                    g_engine->setPropertyIndex(translation, i, g_engine->newNumber(node.translation[i]));
                    g_engine->setPropertyIndex(scale, i, g_engine->newNumber(node.scale[i]));
                }
                for (int i = 0; i < 4; i++) {
                    g_engine->setPropertyIndex(rotation, i, g_engine->newNumber(node.rotation[i]));
                }
                g_engine->setProperty(jsNode, "translation", translation);
                g_engine->setProperty(jsNode, "rotation", rotation);
                g_engine->setProperty(jsNode, "scale", scale);

                // Matrix (if present)
                if (node.hasMatrix) {
                    auto matrix = g_engine->newArray();
                    for (int i = 0; i < 16; i++) {
                        g_engine->setPropertyIndex(matrix, i, g_engine->newNumber(node.matrix[i]));
                    }
                    g_engine->setProperty(jsNode, "matrix", matrix);
                }

                // Children indices
                auto children = g_engine->newArray();
                for (size_t ci = 0; ci < node.children.size(); ci++) {
                    g_engine->setPropertyIndex(children, ci, g_engine->newNumber(node.children[ci]));
                }
                g_engine->setProperty(jsNode, "children", children);

                g_engine->setPropertyIndex(jsNodes, ni, jsNode);
            }
            g_engine->setProperty(result, "nodes", jsNodes);

            // Scenes array
            auto jsScenes = g_engine->newArray();
            for (size_t si = 0; si < gltfData->scenes.size(); si++) {
                const auto& scene = gltfData->scenes[si];
                auto jsScene = g_engine->newObject();
                g_engine->setProperty(jsScene, "name", g_engine->newString(scene.name.c_str()));

                auto sceneNodes = g_engine->newArray();
                for (size_t sni = 0; sni < scene.nodes.size(); sni++) {
                    g_engine->setPropertyIndex(sceneNodes, sni, g_engine->newNumber(scene.nodes[sni]));
                }
                g_engine->setProperty(jsScene, "nodes", sceneNodes);

                g_engine->setPropertyIndex(jsScenes, si, jsScene);
            }
            g_engine->setProperty(result, "scenes", jsScenes);
            g_engine->setProperty(result, "defaultScene", g_engine->newNumber(gltfData->defaultScene));

            if (g_verboseLogging) {
                std::cout << "[GLTF] Loaded " << gltfData->meshes.size() << " meshes, "
                          << gltfData->materials.size() << " materials, "
                          << gltfData->images.size() << " images" << std::endl;
            }

            return result;
        })
    );

    engine->setGlobalProperty("Mystral", mystralNamespace);

    // ========================================================================
    // Native helper for offscreen canvas getContext('2d')
    // Called by the JS closure created in createElement('canvas')
    // ========================================================================
    engine->setGlobalProperty("__nativeGetContext2D",
        engine->newFunction("__nativeGetContext2D", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.size() < 2) {
                std::cerr << "[Canvas] __nativeGetContext2D requires contextType and canvasId" << std::endl;
                return g_engine->newNull();
            }

            std::string contextType = g_engine->toString(args[0]);
            int canvasId = static_cast<int>(g_engine->toNumber(args[1]));

            if (contextType != "2d") {
                std::cerr << "[Canvas] Unsupported context type for offscreen canvas: " << contextType << std::endl;
                return g_engine->newNull();
            }

            auto it = g_offscreenCanvases.find(canvasId);
            if (it == g_offscreenCanvases.end()) {
                std::cerr << "[Canvas] Canvas not found: " << canvasId << std::endl;
                return g_engine->newNull();
            }

            OffscreenCanvas* canvas = it->second.get();

            // Return cached context if already created
            if (canvas->hasContext2d) {
                return canvas->context2d;
            }

            // Get current dimensions from the canvas element (in case they were changed)
            std::string globalName = "__offscreenCanvas_" + std::to_string(canvasId);
            auto canvasElement = g_engine->getGlobalProperty(globalName.c_str());
            if (!g_engine->isNull(canvasElement) && !g_engine->isUndefined(canvasElement)) {
                auto widthProp = g_engine->getProperty(canvasElement, "width");
                auto heightProp = g_engine->getProperty(canvasElement, "height");
                if (!g_engine->isUndefined(widthProp)) {
                    canvas->width = static_cast<int>(g_engine->toNumber(widthProp));
                }
                if (!g_engine->isUndefined(heightProp)) {
                    canvas->height = static_cast<int>(g_engine->toNumber(heightProp));
                }
            }

            // Create Canvas 2D context with current dimensions
            if (g_verboseLogging) std::cout << "[Canvas] Creating offscreen 2D context (" << canvas->width << "x" << canvas->height << ")" << std::endl;
            canvas->context2d = canvas::createCanvas2DContext(g_engine, canvas->width, canvas->height);
            canvas->hasContext2d = true;
            g_engine->protect(canvas->context2d);
            return canvas->context2d;
        })
    );

    // ========================================================================
    // Global createOffscreenCanvas2D(width, height) helper
    // Creates an offscreen canvas with a 2D context at the specified size
    // This is easier to use than document.createElement('canvas').getContext('2d')
    // since it handles dimensions correctly
    // ========================================================================
    engine->setGlobalProperty("createOffscreenCanvas2D",
        engine->newFunction("createOffscreenCanvas2D", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            int width = 800;
            int height = 600;

            if (args.size() >= 1) {
                width = static_cast<int>(g_engine->toNumber(args[0]));
            }
            if (args.size() >= 2) {
                height = static_cast<int>(g_engine->toNumber(args[1]));
            }

            if (g_verboseLogging) std::cout << "[Canvas] Creating offscreen 2D canvas (" << width << "x" << height << ")" << std::endl;

            // Create a wrapper object that mimics a canvas with a 2D context
            auto canvasWrapper = g_engine->newObject();
            g_engine->setProperty(canvasWrapper, "width", g_engine->newNumber(width));
            g_engine->setProperty(canvasWrapper, "height", g_engine->newNumber(height));

            // Create the 2D context
            auto ctx2d = canvas::createCanvas2DContext(g_engine, width, height);
            g_engine->setProperty(canvasWrapper, "_context", ctx2d);

            // getContext('2d') returns the pre-created context
            g_engine->setProperty(canvasWrapper, "getContext",
                g_engine->newFunction("getContext", [](void* c, const std::vector<js::JSValueHandle>& a) {
                    // Get the stored context from the global (we need a way to access it)
                    // For now, return null and let callers use the _context directly
                    return g_engine->newNull();
                })
            );

            return canvasWrapper;
        })
    );

    engine->setGlobalProperty("__mystralWebGpuStats",
        engine->newFunction("__mystralWebGpuStats", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            auto result = g_engine->newObject();
            auto set = [&](const char* name, uint64_t value) {
                g_engine->setProperty(result, name, g_engine->newNumber((double)value));
            };
            set("commandEncodersCreated", g_commandEncodersCreated);
            set("renderPassesCreated", g_renderPassesCreated);
            set("computePassesCreated", g_computePassesCreated);
            set("commandBuffersCreated", g_commandBuffersCreated);
            set("nativeQueueSubmits", g_nativeQueueSubmits);
            set("nativeCommandBuffersSubmitted", g_nativeCommandBuffersSubmitted);
            set("nativeQueueSubmitNanoseconds", g_nativeQueueSubmitNanoseconds);
            set("deferredUploadCommandBuffers", g_deferredUploadCommandBuffers);
            set("forcedFrameFlushes", g_forcedFrameFlushes);
            set("maxCommandBuffersPerNativeSubmit", g_maxCommandBuffersPerNativeSubmit);
            set("activeCommandEncoders", g_liveCommandEncoders.size());
            set("activeRenderPasses", g_liveRenderPasses.size());
            set("activeComputePasses", g_liveComputePasses.size());
            set("activeBuffers", g_bufferRegistry.size());
            set("activeTextures", g_textureRegistry.size());
            set("activeRenderPipelines", g_renderPipelineRegistry.size());
            set("activeComputePipelines", g_computePipelineRegistry.size());
            set("activeOffscreenCanvases", g_offscreenCanvases.size());
            set("activeCanvas2DContexts", canvas::canvas2DContextCount());
            set("pendingAsyncOperations", g_pendingPromises.size());
            {
                std::lock_guard<std::mutex> lock(g_asyncCompletionMutex);
                set("queuedAsyncCompletions", g_asyncCompletions.size());
            }

            auto bridge = g_engine->newObject();
            for (size_t index = 0; index < g_profiledBridgeMetrics.size(); index++) {
                const auto& metric = g_profiledBridgeMetrics[index];
                auto operation = g_engine->newObject();
                g_engine->setProperty(operation, "calls", g_engine->newNumber((double)metric.calls));
                g_engine->setProperty(operation, "totalNanoseconds",
                    g_engine->newNumber((double)metric.totalNanoseconds));
                g_engine->setProperty(operation, "bytes", g_engine->newNumber((double)metric.bytes));
                g_engine->setProperty(bridge, kProfiledBridgeOpNames[index], operation);
            }
            g_engine->setProperty(result, "profiledBridge", bridge);
            return result;
        })
    );

    if (g_verboseLogging) {
        std::cout << "[WebGPU] JavaScript bindings initialized" << std::endl;
        std::cout << "[WebGPU] createImageBitmap() available for image decoding" << std::endl;
    }

    return true;
#else
    std::cerr << "[WebGPU] No WebGPU backend available" << std::endl;
    return true;
#endif
}

// Getter for current texture (used by screenshot)
void* getCurrentRenderedTexture() {
#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
    return g_currentTexture;
#else
    return nullptr;
#endif
}

uint32_t getCurrentTextureWidth() {
    return g_canvasWidth;
}

uint32_t getCurrentTextureHeight() {
    return g_canvasHeight;
}

void* getCurrentSurfaceTexture() {
#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
    // Return the texture that the current view was created from (for screenshots)
    // or the current texture if no view was created yet
    return g_currentViewSourceTexture ? g_currentViewSourceTexture : g_currentTexture;
#else
    return nullptr;
#endif
}

// Screenshot buffer access
void* getScreenshotBuffer() {
#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
    return g_screenshotBuffer;
#else
    return nullptr;
#endif
}

size_t getScreenshotBufferSize() {
    return g_screenshotBufferSize;
}

uint32_t getScreenshotBytesPerRow() {
    return g_screenshotBytesPerRow;
}

bool isScreenshotReady() {
    return g_screenshotReady;
}

void clearScreenshotReady() {
    g_screenshotReady = false;
}

void setOffscreenTexture(void* texture, void* textureView) {
    g_offscreenTexture = (WGPUTexture)texture;
    g_offscreenTextureView = (WGPUTextureView)textureView;
    if (g_verboseLogging) std::cout << "[WebGPU] Offscreen texture set for headless rendering" << std::endl;
}

void beginDawnFrame() {
    // A stale stream means the previous frame exited unusually. Submit it
    // before opening the new frame so queue ordering is never crossed.
    if (!g_deferredQueueWork.empty()) {
        g_forcedFrameFlushes++;
        flushDeferredQueueWork();
    }
    g_dawnFrameActive = true;
}

void releaseReloadResources() {
    if (!g_deferredQueueWork.empty()) flushDeferredQueueWork();
    g_dawnFrameActive = false;

    // A full bundle reload has no live application session. Release the
    // registry-owned resources now instead of waiting for library-level JS
    // caches to become collectible. Weak callbacks use the same registries,
    // so their later cleanup attempts remain idempotent.
    for (auto& entry : g_bufferRegistry) {
        wgpuBufferDestroy(entry.second.buffer);
        wgpuBufferRelease(entry.second.buffer);
    }
    g_bufferRegistry.clear();

    for (auto& entry : g_textureRegistry) {
        auto& info = entry.second;
        if (info.destroyOnReload) {
            wgpuTextureDestroy(info.texture);
        }
        if (info.ownsReference) {
            wgpuTextureRelease(info.texture);
        }
    }
    g_textureRegistry.clear();

    for (auto& entry : g_renderPipelineRegistry) {
        wgpuRenderPipelineRelease(entry.second);
    }
    g_renderPipelineRegistry.clear();

    for (auto& entry : g_computePipelineRegistry) {
        wgpuComputePipelineRelease(entry.second);
    }
    g_computePipelineRegistry.clear();

    for (const auto& entry : g_offscreenCanvases) {
        const std::string globalName = "__offscreenCanvas_" + std::to_string(entry.first);
        g_engine->setGlobalProperty(globalName.c_str(), g_engine->newUndefined());
    }
    g_offscreenCanvases.clear();
    canvas::releaseReloadContexts(g_engine);
    g_mainCanvas2DContext = nullptr;

    g_currentTexture = nullptr;
    g_currentViewSourceTexture = nullptr;
    g_currentTextureId = 0;
}

void resetSessionBindings() {
    // Stop JS settlement first. Native callbacks may still arrive, but their
    // contexts remain valid and will only perform native cleanup.
    abandonPendingPromises();
    ++g_asyncSession;

    const uint64_t imageGeneration = g_imageDecodeGeneration++;
    if (async::getJobSystem().isRunning()) {
        async::getJobSystem().cancelGeneration(imageGeneration);
        async::getJobSystem().waitIdle();
        async::getJobSystem().processCompletions();
    }

    processAsyncCompletions();

    g_transientMethods = {};
    g_engine = nullptr;
}

// Canvas 2D compositing resources
static WGPUTexture g_canvas2DTexture = nullptr;
static WGPURenderPipeline g_canvas2DPipeline = nullptr;
static WGPUBindGroup g_canvas2DBindGroup = nullptr;
static WGPUSampler g_canvas2DSampler = nullptr;
static uint32_t g_canvas2DTextureWidth = 0;
static uint32_t g_canvas2DTextureHeight = 0;

void compositeCanvas2DToWebGPU() {
    if (!g_mainCanvas2DContext || !g_device || !g_queue || !g_surface) {
        return;
    }

    // Get Canvas 2D pixel data
    const uint8_t* pixelData = g_mainCanvas2DContext->getPixelData();
    size_t pixelDataSize = g_mainCanvas2DContext->getPixelDataSize();
    int width = g_mainCanvas2DContext->getWidth();
    int height = g_mainCanvas2DContext->getHeight();

    if (!pixelData || pixelDataSize == 0) {
        return;
    }

    // Create or resize texture if needed
    if (!g_canvas2DTexture || g_canvas2DTextureWidth != (uint32_t)width || g_canvas2DTextureHeight != (uint32_t)height) {
        if (g_canvas2DTexture) {
            wgpuTextureDestroy(g_canvas2DTexture);
            wgpuTextureRelease(g_canvas2DTexture);
        }
        if (g_canvas2DBindGroup) {
            wgpuBindGroupRelease(g_canvas2DBindGroup);
            g_canvas2DBindGroup = nullptr;
        }

        WGPUTextureDescriptor texDesc = {};
        texDesc.size = {(uint32_t)width, (uint32_t)height, 1};
        texDesc.mipLevelCount = 1;
        texDesc.sampleCount = 1;
        texDesc.dimension = WGPUTextureDimension_2D;
        texDesc.format = WGPUTextureFormat_RGBA8Unorm;
        texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;

        g_canvas2DTexture = wgpuDeviceCreateTexture(g_device, &texDesc);
        g_canvas2DTextureWidth = width;
        g_canvas2DTextureHeight = height;
    }

    // Upload pixel data to texture
    WGPUImageCopyTexture_Compat destTexture = {};
    destTexture.texture = g_canvas2DTexture;
    destTexture.mipLevel = 0;
    destTexture.origin = {0, 0, 0};
    destTexture.aspect = WGPUTextureAspect_All;

    WGPUTextureDataLayout_Compat dataLayout = {};
    dataLayout.offset = 0;
    dataLayout.bytesPerRow = width * 4;
    dataLayout.rowsPerImage = height;

    WGPUExtent3D writeSize = {(uint32_t)width, (uint32_t)height, 1};
    wgpuQueueWriteTexture(g_queue, &destTexture, pixelData, pixelDataSize, &dataLayout, &writeSize);

    // Create pipeline if needed
    if (!g_canvas2DPipeline) {
        // Simple fullscreen quad shader
        const char* shaderCode = R"(
            @group(0) @binding(0) var texSampler: sampler;
            @group(0) @binding(1) var tex: texture_2d<f32>;

            struct VertexOutput {
                @builtin(position) position: vec4f,
                @location(0) uv: vec2f,
            }

            @vertex
            fn vs_main(@builtin(vertex_index) vertexIndex: u32) -> VertexOutput {
                var positions = array<vec2f, 6>(
                    vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(-1.0, 1.0),
                    vec2f(-1.0, 1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0)
                );
                var uvs = array<vec2f, 6>(
                    vec2f(0.0, 1.0), vec2f(1.0, 1.0), vec2f(0.0, 0.0),
                    vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(1.0, 0.0)
                );
                var output: VertexOutput;
                output.position = vec4f(positions[vertexIndex], 0.0, 1.0);
                output.uv = uvs[vertexIndex];
                return output;
            }

            @fragment
            fn fs_main(input: VertexOutput) -> @location(0) vec4f {
                return textureSample(tex, texSampler, input.uv);
            }
        )";

        WGPUShaderModuleWGSLDescriptor_Compat wgslDesc = {};
        WGPUShaderModuleDescriptor shaderDesc = {};
        setupShaderModuleWGSL(&shaderDesc, &wgslDesc, shaderCode);

        WGPUShaderModule shaderModule = wgpuDeviceCreateShaderModule(g_device, &shaderDesc);

        // Create sampler
        WGPUSamplerDescriptor samplerDesc = {};
        samplerDesc.magFilter = WGPUFilterMode_Linear;
        samplerDesc.minFilter = WGPUFilterMode_Linear;
        samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
        samplerDesc.addressModeU = WGPUAddressMode_ClampToEdge;
        samplerDesc.addressModeV = WGPUAddressMode_ClampToEdge;
        samplerDesc.addressModeW = WGPUAddressMode_ClampToEdge;
        samplerDesc.maxAnisotropy = 1;
        samplerDesc.lodMinClamp = 0.0f;
        samplerDesc.lodMaxClamp = 1.0f;
        g_canvas2DSampler = wgpuDeviceCreateSampler(g_device, &samplerDesc);

        // Create bind group layout
        WGPUBindGroupLayoutEntry bgLayoutEntries[2] = {};
        bgLayoutEntries[0].binding = 0;
        bgLayoutEntries[0].visibility = WGPUShaderStage_Fragment;
        bgLayoutEntries[0].sampler.type = WGPUSamplerBindingType_Filtering;
        bgLayoutEntries[1].binding = 1;
        bgLayoutEntries[1].visibility = WGPUShaderStage_Fragment;
        bgLayoutEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
        bgLayoutEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

        WGPUBindGroupLayoutDescriptor bgLayoutDesc = {};
        bgLayoutDesc.entryCount = 2;
        bgLayoutDesc.entries = bgLayoutEntries;
        WGPUBindGroupLayout bgLayout = wgpuDeviceCreateBindGroupLayout(g_device, &bgLayoutDesc);

        // Create pipeline layout
        WGPUPipelineLayoutDescriptor pipelineLayoutDesc = {};
        pipelineLayoutDesc.bindGroupLayoutCount = 1;
        pipelineLayoutDesc.bindGroupLayouts = &bgLayout;
        WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(g_device, &pipelineLayoutDesc);

        // Create pipeline
        WGPURenderPipelineDescriptor pipelineDesc = {};
        pipelineDesc.layout = pipelineLayout;
        pipelineDesc.vertex.module = shaderModule;
        WGPU_SET_ENTRY_POINT(pipelineDesc.vertex, "vs_main");

        WGPUFragmentState fragmentState = {};
        fragmentState.module = shaderModule;
        WGPU_SET_ENTRY_POINT(fragmentState, "fs_main");
        fragmentState.targetCount = 1;

        WGPUColorTargetState colorTarget = {};
        colorTarget.format = g_surfaceFormat;
        colorTarget.writeMask = WGPUColorWriteMask_All;
        fragmentState.targets = &colorTarget;

        pipelineDesc.fragment = &fragmentState;
        pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        pipelineDesc.multisample.count = 1;
        pipelineDesc.multisample.mask = 0xFFFFFFFF;

        g_canvas2DPipeline = wgpuDeviceCreateRenderPipeline(g_device, &pipelineDesc);

        wgpuShaderModuleRelease(shaderModule);
        wgpuPipelineLayoutRelease(pipelineLayout);
        wgpuBindGroupLayoutRelease(bgLayout);

        if (!g_canvas2DPipeline) {
            std::cerr << "[Canvas2D] Failed to create compositing pipeline" << std::endl;
            return;
        }
    }

    // Create bind group (recreate if texture changed)
    if (!g_canvas2DBindGroup) {
        if (!g_canvas2DSampler || !g_canvas2DTexture) {
            return;
        }

        WGPUTextureViewDescriptor viewDesc = {};
        viewDesc.format = WGPUTextureFormat_RGBA8Unorm;
        viewDesc.dimension = WGPUTextureViewDimension_2D;
        viewDesc.baseMipLevel = 0;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = 0;
        viewDesc.arrayLayerCount = 1;
        WGPUTextureView texView = wgpuTextureCreateView(g_canvas2DTexture, &viewDesc);

        if (!texView) {
            return;
        }

        WGPUBindGroupEntry bgEntries[2] = {};
        bgEntries[0].binding = 0;
        bgEntries[0].sampler = g_canvas2DSampler;
        bgEntries[1].binding = 1;
        bgEntries[1].textureView = texView;

        WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(g_canvas2DPipeline, 0);
        if (!layout) {
            wgpuTextureViewRelease(texView);
            return;
        }

        WGPUBindGroupDescriptor bgDesc = {};
        bgDesc.layout = layout;
        bgDesc.entryCount = 2;
        bgDesc.entries = bgEntries;
        g_canvas2DBindGroup = wgpuDeviceCreateBindGroup(g_device, &bgDesc);

        wgpuBindGroupLayoutRelease(layout);
        wgpuTextureViewRelease(texView);

        if (!g_canvas2DBindGroup) {
            return;
        }
    }

    // Get surface texture
    WGPUSurfaceTexture surfaceTexture;
    wgpuSurfaceGetCurrentTexture(g_surface, &surfaceTexture);
    if (!wgpuSurfaceTextureStatusIsSuccess(surfaceTexture.status)) {
        return;
    }

    WGPUTextureViewDescriptor surfaceViewDesc = {};
    surfaceViewDesc.format = g_surfaceFormat;
    surfaceViewDesc.dimension = WGPUTextureViewDimension_2D;
    surfaceViewDesc.baseMipLevel = 0;
    surfaceViewDesc.mipLevelCount = 1;
    surfaceViewDesc.baseArrayLayer = 0;
    surfaceViewDesc.arrayLayerCount = 1;
    WGPUTextureView surfaceView = wgpuTextureCreateView(surfaceTexture.texture, &surfaceViewDesc);

    // Create command encoder and render pass
    WGPUCommandEncoderDescriptor encDesc = {};
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(g_device, &encDesc);

    WGPURenderPassColorAttachment colorAttachment = {};
    colorAttachment.view = surfaceView;
    colorAttachment.loadOp = WGPULoadOp_Clear;
    colorAttachment.storeOp = WGPUStoreOp_Store;
    colorAttachment.clearValue = {0.0, 0.0, 0.0, 1.0};
#if defined(MYSTRAL_WEBGPU_DAWN)
    colorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#endif

    WGPURenderPassDescriptor renderPassDesc = {};
    renderPassDesc.colorAttachmentCount = 1;
    renderPassDesc.colorAttachments = &colorAttachment;

    WGPURenderPassEncoder renderPass = wgpuCommandEncoderBeginRenderPass(encoder, &renderPassDesc);
    wgpuRenderPassEncoderSetPipeline(renderPass, g_canvas2DPipeline);
    wgpuRenderPassEncoderSetBindGroup(renderPass, 0, g_canvas2DBindGroup, 0, nullptr);
    wgpuRenderPassEncoderDraw(renderPass, 6, 1, 0, 0);
    wgpuRenderPassEncoderEnd(renderPass);
    wgpuRenderPassEncoderRelease(renderPass);

    // Copy rendered texture to screenshot buffer
    uint32_t bytesPerRow = ((g_canvasWidth * 4 + 255) / 256) * 256;  // Align to 256
    size_t requiredSize = bytesPerRow * g_canvasHeight;

    if (!g_screenshotBuffer || g_screenshotBufferSize < requiredSize) {
        if (g_screenshotBuffer) {
            wgpuBufferDestroy(g_screenshotBuffer);
            wgpuBufferRelease(g_screenshotBuffer);
        }

        WGPUBufferDescriptor bufferDesc = {};
        bufferDesc.size = requiredSize;
        bufferDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        bufferDesc.mappedAtCreation = false;

        g_screenshotBuffer = wgpuDeviceCreateBuffer(g_device, &bufferDesc);
        g_screenshotBufferSize = requiredSize;
        g_screenshotBytesPerRow = bytesPerRow;
    }

    // Copy surface texture to screenshot buffer
    WGPUImageCopyTexture_Compat srcCopy = {};
    srcCopy.texture = surfaceTexture.texture;
    srcCopy.mipLevel = 0;
    srcCopy.origin = {0, 0, 0};
    srcCopy.aspect = WGPUTextureAspect_All;

    WGPUImageCopyBuffer_Compat dstCopy = {};
    dstCopy.buffer = g_screenshotBuffer;
    dstCopy.layout.offset = 0;
    dstCopy.layout.bytesPerRow = bytesPerRow;
    dstCopy.layout.rowsPerImage = g_canvasHeight;

    WGPUExtent3D copySize = {g_canvasWidth, g_canvasHeight, 1};
    wgpuCommandEncoderCopyTextureToBuffer_Compat(encoder, &srcCopy, &dstCopy, &copySize);

    WGPUCommandBufferDescriptor cmdDesc = {};
    WGPUCommandBuffer cmdBuffer = wgpuCommandEncoderFinish(encoder, &cmdDesc);
    wgpuQueueSubmit(g_queue, 1, &cmdBuffer);

    wgpuCommandBufferRelease(cmdBuffer);
    wgpuCommandEncoderRelease(encoder);
    wgpuTextureViewRelease(surfaceView);

    // Present
    wgpuSurfacePresent(g_surface);

    // Track for screenshot
    g_currentTexture = surfaceTexture.texture;
    g_screenshotReady = true;
}

// ============================================================================
// Video capture callback support (used by GPUReadbackRecorder)
// ============================================================================

// Video capture callback - called when queue.submit happens with a surface texture
// This allows the video recorder to capture frames without modifying the render loop
static void (*g_videoCaptureCallback)(void* texture, uint32_t width, uint32_t height, void* userData) = nullptr;
static void* g_videoCaptureUserData = nullptr;

void setVideoCaptureCallback(void (*callback)(void* texture, uint32_t width, uint32_t height, void* userData), void* userData) {
    g_videoCaptureCallback = callback;
    g_videoCaptureUserData = userData;
}

void clearVideoCaptureCallback() {
    g_videoCaptureCallback = nullptr;
    g_videoCaptureUserData = nullptr;
}

// Internal function to invoke video capture callback (called from queue.submit)
void invokeVideoCaptureCallback(WGPUTexture texture, uint32_t width, uint32_t height) {
    if (g_videoCaptureCallback && texture) {
        g_videoCaptureCallback(static_cast<void*>(texture), width, height, g_videoCaptureUserData);
    }
}

void endDawnFrame() {
    // Composite Canvas 2D content to WebGPU if the main canvas uses 2D context
    compositeCanvas2DToWebGPU();

    // Preserve all Three.js pass boundaries inside the command-buffer array,
    // but cross the actual Dawn queue only once for the normal WebGPU frame.
    flushDeferredQueueWork();
    g_dawnFrameActive = false;
    presentSurfaceIfReady();

    // Tick the WebGPU device to process completed GPU work and free internal
    // resources (staging buffers, command encoder state, etc.). Without this,
    // internal objects accumulate unboundedly since completion callbacks never fire.
    if (g_device) {
#if defined(MYSTRAL_WEBGPU_DAWN)
        wgpuDeviceTick(g_device);
#elif defined(MYSTRAL_WEBGPU_WGPU)
        wgpuDevicePoll(g_device, false, nullptr);
#endif
    }
}

}  // namespace webgpu
}  // namespace mystral
