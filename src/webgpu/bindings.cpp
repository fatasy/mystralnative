/**
 * WebGPU JavaScript Bindings
 *
 * This file exposes the WebGPU API to JavaScript via the JS engine abstraction.
 * Dawn provides the native webgpu.h implementation.
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

#include "webgpu/bindings.h"

#include "webgpu/binding_internal.h"
#include "webgpu/canvas_bindings.h"
#include "webgpu/constants.h"
#include "webgpu/diagnostics_bindings.h"
#include "webgpu/navigator_bindings.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

#include <webgpu/webgpu.h>
#include "mystral/webgpu_compat.h"

namespace mystral {
namespace webgpu {


size_t processAsyncCompletions(size_t maxCount) {
    return g_asyncBridge.process(maxCount);
}



static bool g_screenshotPending = false;
// Prevent capturing multiple screenshots per frame (Three.js does multiple queue.submit() per frame)
static std::vector<uint8_t> g_screenshotData;

void presentSurfaceIfReady() {
    if (!g_surface || !g_currentTexture || !g_commandEncoders.surfaceRenderPassEnded()) return;

    if (g_verboseLogging) std::cout << "[WebGPU] Presenting surface" << std::endl;
    wgpuSurfacePresent(g_surface);

    g_commandEncoders.markSurfacePresented();
    g_screenshot.beginPresentedFrame();

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

// Dawn resource cleanup is handled via Engine::registerRelease(), which sets up
// V8 weak callbacks. When the JS wrapper object is garbage collected (no more
// JS references), the callback fires and releases the Dawn resource.
// This is the same pattern Chrome uses for WebGPU resource lifecycle.


/**
 * Initialize WebGPU bindings in the JS engine
 */
bool initBindings(js::Engine* engine, void* wgpuInstance, void* wgpuAdapter, void* wgpuDevice, void* wgpuQueue, void* wgpuSurface, uint32_t surfaceFormat, uint32_t width, uint32_t height, bool debug, uint32_t maxFrameLatency, uint64_t maxTrackedGpuMemoryBytes) {
    if (!engine) {
        std::cerr << "[WebGPU] No JS engine provided for bindings" << std::endl;
        return false;
    }

    // Set verbose logging based on debug flag
    g_verboseLogging = debug;

    g_engine = engine;
    g_instance = (WGPUInstance)wgpuInstance;
    g_adapter = (WGPUAdapter)wgpuAdapter;
    g_device = (WGPUDevice)wgpuDevice;
    g_queue = (WGPUQueue)wgpuQueue;
    g_surface = (WGPUSurface)wgpuSurface;
    g_maxTrackedGpuMemoryBytes = maxTrackedGpuMemoryBytes;
    g_asyncBridge.configure(engine, g_instance, g_device);
    g_frameQueue.configure(g_instance, g_device, g_queue, maxFrameLatency);
    g_capabilities.configure(engine, g_adapter, g_device);

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
    g_canvasCompositor.configure(g_device, g_queue, g_surface, g_surfaceFormat);

    if (g_verboseLogging) {
        std::cout << "[WebGPU] Initializing JavaScript bindings..." << std::endl;
        std::cout << "[WebGPU] Surface format: " << surfaceFormat << std::endl;
    }

    installCanvasBindings(engine);

    installNavigatorBindings(engine);

    bridge::installConstants(engine);

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
    g_imageDecoder.install(engine, &g_asyncBridge, g_verboseLogging);

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

    installOffscreenCanvasBindings(engine);

    installDiagnosticsBindings(engine);

    if (g_verboseLogging) {
        std::cout << "[WebGPU] JavaScript bindings initialized" << std::endl;
        std::cout << "[WebGPU] createImageBitmap() available for image decoding" << std::endl;
    }

    return true;
}

// Getter for current texture (used by screenshot)
void* getCurrentRenderedTexture() {
    return g_currentTexture;
}

uint32_t getCurrentTextureWidth() {
    return g_canvasWidth;
}

uint32_t getCurrentTextureHeight() {
    return g_canvasHeight;
}

void* getCurrentSurfaceTexture() {
    // Return the texture that the current view was created from (for screenshots)
    // or the current texture if no view was created yet
    return g_currentViewSourceTexture ? g_currentViewSourceTexture : g_currentTexture;
}

// Screenshot buffer access
void* getScreenshotBuffer() {
    return g_screenshot.buffer();
}

size_t getScreenshotBufferSize() {
    return g_screenshot.bufferSize();
}

uint32_t getScreenshotBytesPerRow() {
    return g_screenshot.bytesPerRow();
}

bool isScreenshotReady() {
    return g_screenshot.ready();
}

void clearScreenshotReady() {
    g_screenshot.clearReady();
}

void setOffscreenTexture(void* texture, void* textureView) {
    g_offscreenTexture = (WGPUTexture)texture;
    g_offscreenTextureView = (WGPUTextureView)textureView;
    if (g_verboseLogging) std::cout << "[WebGPU] Offscreen texture set for headless rendering" << std::endl;
}

void beginDawnFrame() {
    g_frameQueue.beginFrame();
}

void releaseReloadResources() {
    g_frameQueue.endFrame();

    // A full bundle reload has no live application session. Release the
    // registry-owned resources now instead of waiting for library-level JS
    // caches to become collectible. Weak callbacks use the same registries,
    // so their later cleanup attempts remain idempotent.
    for (auto& entry : g_bufferRegistry) {
        wgpuBufferDestroy(entry.second.buffer);
        wgpuBufferRelease(entry.second.buffer);
    }
    g_bufferRegistry.clear();
    g_trackedBufferBytes = 0;

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
    g_estimatedTextureBytes = 0;

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
    g_canvasCompositor.setContext(nullptr);

    g_currentTexture = nullptr;
    g_currentViewSourceTexture = nullptr;
    g_currentTextureId = 0;
}

void resetSessionBindings() {
    // Stop JS settlement first. Native callbacks may still arrive, but their
    // contexts remain valid and will only perform native cleanup.
    g_asyncBridge.invalidateSession();

    g_imageDecoder.cancelPending();

    processAsyncCompletions();
    g_frameQueue.reset();

    g_transientMethods = {};
    g_asyncBridge.detach();
    g_engine = nullptr;
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
    if (auto texture = g_canvasCompositor.composite(g_canvasWidth, g_canvasHeight)) {
        g_currentTexture = texture;
    }

    // Preserve all Three.js pass boundaries inside the command-buffer array,
    // but cross the actual Dawn queue only once for the normal WebGPU frame.
    g_frameQueue.endFrame();
    presentSurfaceIfReady();

    // Tick the WebGPU device to process completed GPU work and free internal
    // resources (staging buffers, command encoder state, etc.). Without this,
    // internal objects accumulate unboundedly since completion callbacks never fire.
    if (g_device) {
        wgpuDeviceTick(g_device);
    }
}

}  // namespace webgpu
}  // namespace mystral
