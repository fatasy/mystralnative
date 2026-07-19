#include "webgpu/binding_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace mystral::webgpu {

static auto& context = bridge::bindingContext();

bool& g_verboseLogging = context.verboseLogging;
WGPUAdapter& g_adapter = context.adapter;
WGPUDevice& g_device = context.device;
WGPUQueue& g_queue = context.queue;
WGPUSurface& g_surface = context.surface;
WGPUInstance& g_instance = context.instance;
js::Engine*& g_engine = context.engine;
bridge::AsyncBridge& g_asyncBridge = context.asyncBridge;
bridge::Capabilities& g_capabilities = context.capabilities;
bridge::ImageDecoderBindings& g_imageDecoder = context.imageDecoder;
WGPUTexture& g_offscreenTexture = context.offscreenTexture;
WGPUTextureView& g_offscreenTextureView = context.offscreenTextureView;
WGPUTextureFormat& g_surfaceFormat = context.surfaceFormat;
uint32_t& g_canvasWidth = context.canvasWidth;
uint32_t& g_canvasHeight = context.canvasHeight;
bool& g_contextConfigured = context.contextConfigured;
WGPUTexture& g_currentTexture = context.currentTexture;
WGPUTextureView& g_currentTextureView = context.currentTextureView;
WGPUTexture& g_currentViewSourceTexture = context.currentViewSourceTexture;
bridge::ScreenshotState& g_screenshot = context.screenshot;
bridge::CanvasCompositor& g_canvasCompositor = context.canvasCompositor;
bridge::CommandEncoderState& g_commandEncoders = context.commandEncoders;
bridge::FrameQueue& g_frameQueue = context.frameQueue;
bridge::TransientMethodCache& g_transientMethods = context.transientMethods;
std::unordered_map<int, std::unique_ptr<bridge::OffscreenCanvasState>>& g_offscreenCanvases = context.offscreenCanvases;
int& g_nextOffscreenCanvasId = context.nextOffscreenCanvasId;
std::unordered_map<uint64_t, bridge::TextureInfo>& g_textureRegistry = context.textureRegistry;
uint64_t& g_nextTextureId = context.nextTextureId;
uint64_t& g_currentTextureId = context.currentTextureId;
std::unordered_map<uint64_t, bridge::BufferInfo>& g_bufferRegistry = context.bufferRegistry;
uint64_t& g_nextBufferId = context.nextBufferId;
std::unordered_map<uint64_t, WGPUComputePipeline>& g_computePipelineRegistry = context.computePipelineRegistry;
uint64_t& g_nextComputePipelineId = context.nextComputePipelineId;
std::unordered_map<uint64_t, WGPURenderPipeline>& g_renderPipelineRegistry = context.renderPipelineRegistry;
uint64_t& g_nextRenderPipelineId = context.nextRenderPipelineId;
uint64_t& g_trackedBufferBytes = context.trackedBufferBytes;
uint64_t& g_estimatedTextureBytes = context.estimatedTextureBytes;
uint64_t& g_peakTrackedGpuBytes = context.peakTrackedGpuBytes;
uint64_t& g_maxTrackedGpuMemoryBytes = context.maxTrackedGpuMemoryBytes;

namespace {

uint64_t saturatingAdd(uint64_t left, uint64_t right) {
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    return left > maximum - right ? maximum : left + right;
}

uint64_t saturatingMultiply(uint64_t left, uint64_t right) {
    if (left == 0 || right == 0) return 0;
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    return left > maximum / right ? maximum : left * right;
}

}  // namespace

uint64_t estimateTextureBytes(WGPUTextureFormat format, uint32_t width, uint32_t height,
                              uint32_t depthOrLayers, uint32_t mipLevels, uint32_t samples) {
    uint64_t bytesPerPixel = 4;
    switch (format) {
        case WGPUTextureFormat_R8Unorm:
        case WGPUTextureFormat_R8Snorm:
        case WGPUTextureFormat_R8Uint:
        case WGPUTextureFormat_R8Sint:
            bytesPerPixel = 1; break;
        case WGPUTextureFormat_R16Uint:
        case WGPUTextureFormat_R16Sint:
        case WGPUTextureFormat_R16Float:
        case WGPUTextureFormat_RG8Unorm:
        case WGPUTextureFormat_RG8Snorm:
        case WGPUTextureFormat_RG8Uint:
        case WGPUTextureFormat_RG8Sint:
            bytesPerPixel = 2; break;
        case WGPUTextureFormat_RG32Uint:
        case WGPUTextureFormat_RG32Sint:
        case WGPUTextureFormat_RG32Float:
        case WGPUTextureFormat_RGBA16Uint:
        case WGPUTextureFormat_RGBA16Sint:
        case WGPUTextureFormat_RGBA16Float:
            bytesPerPixel = 8; break;
        case WGPUTextureFormat_RGBA32Uint:
        case WGPUTextureFormat_RGBA32Sint:
        case WGPUTextureFormat_RGBA32Float:
            bytesPerPixel = 16; break;
        default:
            break;
    }
    uint64_t total = 0;
    uint64_t mipWidth = std::max<uint32_t>(1, width);
    uint64_t mipHeight = std::max<uint32_t>(1, height);
    const uint64_t layers = std::max<uint32_t>(1, depthOrLayers);
    for (uint32_t level = 0; level < std::max<uint32_t>(1, mipLevels); ++level) {
        uint64_t levelBytes = saturatingMultiply(mipWidth, mipHeight);
        levelBytes = saturatingMultiply(levelBytes, layers);
        levelBytes = saturatingMultiply(levelBytes, bytesPerPixel);
        levelBytes = saturatingMultiply(levelBytes, std::max<uint32_t>(1, samples));
        total = saturatingAdd(total, levelBytes);
        mipWidth = std::max<uint64_t>(1, mipWidth / 2);
        mipHeight = std::max<uint64_t>(1, mipHeight / 2);
    }
    return total;
}

bool canAllocateTrackedGpuBytes(uint64_t additionalBytes) {
    if (g_maxTrackedGpuMemoryBytes == 0) return true;
    const uint64_t current = saturatingAdd(g_trackedBufferBytes, g_estimatedTextureBytes);
    return additionalBytes <= g_maxTrackedGpuMemoryBytes &&
        current <= g_maxTrackedGpuMemoryBytes - additionalBytes;
}

void updatePeakTrackedGpuBytes() {
    g_peakTrackedGpuBytes = std::max(
        g_peakTrackedGpuBytes, saturatingAdd(g_trackedBufferBytes, g_estimatedTextureBytes));
}

bool gcReleaseEnabled() {
    static const bool enabled = std::getenv("MYSTRAL_NO_GC_RELEASE") == nullptr;
    return enabled;
}

bool parseDynamicOffsets(
    const std::vector<js::JSValueHandle>& args,
    std::vector<uint32_t>& offsets,
    const char* apiName) {
    offsets.clear();
    if (args.size() <= 2 || g_engine->isUndefined(args[2])) return true;

    auto fail = [apiName](const std::string& reason) {
        g_engine->throwException((std::string(apiName) + ": " + reason).c_str());
        return false;
    };
    auto toIndex = [&](js::JSValueHandle value, const char* name, size_t& result) {
        const double number = g_engine->toNumber(value);
        if (!std::isfinite(number) || number < 0 || std::floor(number) != number ||
            number > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
            return fail(std::string(name) + " must be a non-negative integer");
        }
        result = static_cast<size_t>(number);
        return true;
    };
    auto appendOffset = [&](js::JSValueHandle value) {
        const double number = g_engine->toNumber(value);
        if (!std::isfinite(number) || number < 0 || std::floor(number) != number ||
            number > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
            return fail("dynamic offsets must be unsigned 32-bit integers");
        }
        offsets.push_back(static_cast<uint32_t>(number));
        return true;
    };

    const auto data = args[2];
    if (g_engine->isNull(data) || !g_engine->isObject(data)) {
        return fail("dynamicOffsets must be an Array or Uint32Array");
    }
    if (g_engine->isArray(data)) {
        const auto lengthValue = g_engine->getProperty(data, "length");
        size_t length = 0;
        if (!toIndex(lengthValue, "dynamicOffsets.length", length)) return false;
        offsets.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            if (!appendOffset(g_engine->getPropertyIndex(data, static_cast<uint32_t>(i)))) {
                return false;
            }
        }
        return true;
    }

    const auto constructor = g_engine->getProperty(data, "constructor");
    const auto constructorName = g_engine->getProperty(constructor, "name");
    if (g_engine->isUndefined(constructorName) ||
        g_engine->toString(constructorName) != "Uint32Array") {
        return fail("dynamicOffsets must be an Array or Uint32Array");
    }

    size_t byteLength = 0;
    auto* values = static_cast<const uint32_t*>(g_engine->getArrayBufferData(data, &byteLength));
    if (byteLength % sizeof(uint32_t) != 0 || (!values && byteLength != 0)) {
        return fail("could not read dynamicOffsetsData");
    }

    const size_t elementCount = byteLength / sizeof(uint32_t);
    size_t start = 0;
    if (args.size() > 3 && !g_engine->isUndefined(args[3]) &&
        !toIndex(args[3], "dynamicOffsetsDataStart", start)) {
        return false;
    }
    if (start > elementCount) {
        return fail("dynamicOffsetsDataStart exceeds the Uint32Array length");
    }

    size_t length = elementCount - start;
    if (args.size() > 4 && !g_engine->isUndefined(args[4]) &&
        !toIndex(args[4], "dynamicOffsetsDataLength", length)) {
        return false;
    }
    if (length > elementCount - start) {
        return fail("dynamicOffsetsDataStart + dynamicOffsetsDataLength exceeds the Uint32Array length");
    }

    if (length > 0) offsets.assign(values + start, values + start + length);
    return true;
}

} // namespace mystral::webgpu
