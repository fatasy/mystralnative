#include "webgpu/queue_bindings.h"

#include "mystral/canvas/canvas2d.h"
#include "webgpu/binding_internal.h"
#include "webgpu/format_conversions.h"
#include "mystral/webgpu_compat.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

namespace mystral::webgpu {

#if defined(MYSTRAL_WEBGPU_DAWN)
static void onQueueWorkDone(WGPUQueueWorkDoneStatus status,
                            WGPUStringView message,
                            void* userdata1,
                            void*) {
    auto* pending = static_cast<bridge::AsyncBridge::PendingPromise*>(userdata1);
    const std::string error = message.data && message.length > 0
        ? std::string(message.data, message.length)
        : std::string();
    g_asyncBridge.enqueue([pending, status, error]() {
        if (!pending->active || pending->session != g_asyncBridge.session() || pending->engine != g_engine) {
            g_asyncBridge.settle(pending, false, {});
            return;
        }
        if (status == WGPUQueueWorkDoneStatus_Success) {
            g_asyncBridge.settle(pending, true, g_engine->newUndefined());
        } else {
            g_asyncBridge.settle(
                pending,
                false,
                {},
                error.empty() ? "GPU queue work failed" : error);
        }
    });
}
#endif

bool writeQueueBuffer(js::JSValueHandle bufferHandle,
                             js::JSValueHandle offsetHandle,
                             js::JSValueHandle data,
                             const js::JSValueHandle* dataOffsetHandle,
                             const js::JSValueHandle* sizeHandle,
                             bridge::Operation operation,
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
        g_frameQueue.writeBuffer(buffer, offset,
            (uint8_t*)basePtr + viewByteOffset + dataOffsetBytes, writeSize);
        bridge::recordBytes(operation, writeSize);
    }
    return true;
}

void installQueueBindings(js::JSValueHandle device) {    // device.queue
    auto queue = g_engine->newObject();
    g_engine->setPrivateData(queue, g_queue);

    // queue.submit(commandBuffers)
    g_engine->setProperty(queue, "submit",
        g_engine->newFunction("submit", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            bridge::ScopedMeasurement measurement(bridge::Operation::QueueSubmit);
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
                const bool submittedImmediately = !g_frameQueue.frameActive();
                g_frameQueue.submit(std::move(cmdBuffers));
                if (submittedImmediately) {
                    wgpuDeviceTick(g_device);
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
            if (g_commandEncoders.surfaceRenderPassEnded() && !g_screenshot.capturedThisFrame() && screenshotTexture && g_device && g_queue) {
                WGPUBuffer screenshotBuffer =
                    g_screenshot.ensureBuffer(g_device, g_canvasWidth, g_canvasHeight);

                // Create encoder to copy texture to buffer
                WGPUCommandEncoderDescriptor encDesc = {};
                WGPUCommandEncoder copyEncoder = wgpuDeviceCreateCommandEncoder(g_device, &encDesc);

                WGPUImageCopyTexture_Compat srcCopy = {};
                srcCopy.texture = screenshotTexture;
                srcCopy.mipLevel = 0;
                srcCopy.origin = {0, 0, 0};
                srcCopy.aspect = WGPUTextureAspect_All;

                WGPUImageCopyBuffer_Compat dstCopy = {};
                dstCopy.buffer = screenshotBuffer;
                dstCopy.layout.offset = 0;
                dstCopy.layout.bytesPerRow = g_screenshot.bytesPerRow();
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
                    g_frameQueue.submit(std::move(screenshotCommands));
                }

                // Outside the animation-frame scope, retain the previous eager
                // completion behavior. Normal frames flush this copy in endDawnFrame.
                if (!g_frameQueue.frameActive()) {
                    for (int syncIter = 0; syncIter < 100; syncIter++) {
                        wgpuDeviceTick(g_device);
                        if (g_instance) {
                            wgpuInstanceProcessEvents(g_instance);
                        }
                    }
                }

                g_screenshot.markReady();
                g_screenshot.markCapturedThisFrame();
            }

            if (!g_frameQueue.frameActive()) presentSurfaceIfReady();

            return g_engine->newUndefined();
        })
    );

    // queue.writeBuffer(buffer, offset, data, dataOffset?, size?)
    //
    // Spec conformance: when `data` is a TypedArray, the optional
    // dataOffset/size arguments are ELEMENT counts, not bytes
    // (https://www.w3.org/TR/webgpu/#dom-gpuqueue-writebuffer).
    // three.js relies on this for partial attribute uploads - the
    // previous byte interpretation shrank Float32Array writes by 4x.
    // The view's byteOffset must also be honored, so the copy is
    // resolved against the view's underlying ArrayBuffer.
    g_engine->setProperty(queue, "writeBuffer",
        g_engine->newFunction("writeBuffer", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            bridge::ScopedMeasurement measurement(bridge::Operation::QueueWriteBuffer);
            if (args.size() < 3) {
                g_engine->throwException("writeBuffer requires buffer, offset, and data");
                return g_engine->newUndefined();
            }

            const auto* dataOffset = args.size() > 3 ? &args[3] : nullptr;
            const auto* size = args.size() > 4 ? &args[4] : nullptr;
            writeQueueBuffer(args[0], args[1], args[2], dataOffset, size,
                bridge::Operation::QueueWriteBuffer, "writeBuffer");

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
            bridge::ScopedMeasurement measurement(bridge::Operation::QueueWriteBufferBatch);
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
                    bridge::Operation::QueueWriteBufferBatch, "writeBufferBatch");
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
            bridge::ScopedMeasurement measurement(bridge::Operation::QueueWriteTexture);
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
            g_frameQueue.flushBeforeImmediateOperation();
            wgpuQueueWriteTexture(g_queue, &destCopy, (uint8_t*)dataPtr + layoutOffset, dataSize - layoutOffset, &layout, &copySize);
            bridge::recordBytes(bridge::Operation::QueueWriteTexture, dataSize - layoutOffset);

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
            // Texture.from(imageBitmap) lands here - without the swap, red and blue
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

            g_frameQueue.flushBeforeImmediateOperation();
            wgpuQueueWriteTexture(g_queue, &destCopy, uploadDataPtr, dataSize, &layout, &copySize);

            if (g_verboseLogging) std::cout << "[WebGPU] copyExternalImageToTexture: " << width << "x" << height << (flipY ? " (flipY)" : "") << std::endl;

            return g_engine->newUndefined();
        })
    );

    // queue.onSubmittedWorkDone() - returns Promise that resolves when GPU work is done
    g_engine->setProperty(queue, "onSubmittedWorkDone",
        g_engine->newFunction("onSubmittedWorkDone", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            g_frameQueue.flushBeforeImmediateOperation();
            js::JSValueHandle promise;
            auto* pending = g_asyncBridge.createPromise(promise);
            if (!pending) return g_engine->newUndefined();

            WGPUQueueWorkDoneCallbackInfo callbackInfo = {};
            callbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
            callbackInfo.callback = onQueueWorkDone;
            callbackInfo.userdata1 = pending;
            wgpuQueueOnSubmittedWorkDone(g_queue, callbackInfo);
            return promise;
        })
    );

    g_engine->setProperty(device, "queue", queue);
}

} // namespace mystral::webgpu
