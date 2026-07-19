#include "webgpu/binding_internal.h"

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
