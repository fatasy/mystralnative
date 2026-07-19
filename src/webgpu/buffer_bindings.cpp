#include "webgpu/buffer_bindings.h"

#include "webgpu/binding_internal.h"
#include "mystral/webgpu_compat.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace mystral::webgpu {

struct BufferMapAsyncContext {
    bridge::AsyncBridge::PendingPromise* promise = nullptr;
    uint64_t bufferId = 0;
    WGPUMapMode mode = WGPUMapMode_None;
};

#if defined(MYSTRAL_WEBGPU_DAWN)
static void onBufferMapped(WGPUMapAsyncStatus status,
                           WGPUStringView message,
                           void* userdata1,
                           void*) {
    auto* context = static_cast<BufferMapAsyncContext*>(userdata1);
    const std::string error = message.data && message.length > 0
        ? std::string(message.data, message.length)
        : std::string();
    g_asyncBridge.enqueue([context, status, error]() {
        auto* pending = context->promise;
        if (!pending->active || pending->session != g_asyncBridge.session()) {
            g_asyncBridge.settle(pending, false, {});
            delete context;
            return;
        }

        auto buffer = g_bufferRegistry.find(context->bufferId);
        if (buffer == g_bufferRegistry.end()) {
            g_asyncBridge.settle(pending, false, {}, "GPUBuffer was destroyed while mapping");
        } else {
            buffer->second.mapPending = false;
            if (status == WGPUMapAsyncStatus_Success) {
                buffer->second.isMapped = true;
                buffer->second.mapMode = context->mode;
                g_asyncBridge.settle(pending, true, g_engine->newUndefined());
            } else {
                g_asyncBridge.settle(
                    pending,
                    false,
                    {},
                    error.empty() ? "GPUBuffer mapping failed" : error);
            }
        }
        delete context;
    });
}
#else
static void onBufferMapped(WGPUBufferMapAsyncStatus status, void* userdata) {
    auto* context = static_cast<BufferMapAsyncContext*>(userdata);
    g_asyncBridge.enqueue([context, status]() {
        auto* pending = context->promise;
        if (!pending->active || pending->session != g_asyncBridge.session() || pending->engine != g_engine) {
            g_asyncBridge.settle(pending, false, {});
            delete context;
            return;
        }

        auto buffer = g_bufferRegistry.find(context->bufferId);
        if (buffer == g_bufferRegistry.end()) {
            g_asyncBridge.settle(pending, false, {}, "GPUBuffer was destroyed while mapping");
        } else {
            buffer->second.mapPending = false;
            if (status == WGPUBufferMapAsyncStatus_Success_Compat) {
                buffer->second.isMapped = true;
                buffer->second.mapMode = context->mode;
                g_asyncBridge.settle(pending, true, g_engine->newUndefined());
            } else {
                g_asyncBridge.settle(pending, false, {}, "GPUBuffer mapping failed");
            }
        }
        delete context;
    });
}
#endif

void releaseBuffer(uint64_t id, bool destroy) {
    auto it = g_bufferRegistry.find(id);
    if (it == g_bufferRegistry.end()) return;
    if (destroy) wgpuBufferDestroy(it->second.buffer);
    wgpuBufferRelease(it->second.buffer);
    g_trackedBufferBytes = it->second.size > g_trackedBufferBytes
        ? 0
        : g_trackedBufferBytes - it->second.size;
    g_bufferRegistry.erase(it);
}

void installBufferBindings(js::JSValueHandle device) {    // device.createBuffer(descriptor)
    g_engine->setProperty(device, "createBuffer",
        g_engine->newFunction("createBuffer", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                g_engine->throwException("createBuffer requires a descriptor");
                return g_engine->newUndefined();
            }

            auto descriptor = args[0];
            double size = g_engine->toNumber(g_engine->getProperty(descriptor, "size"));
            double usage = g_engine->toNumber(g_engine->getProperty(descriptor, "usage"));
            if (!std::isfinite(size) || size <= 0 || std::floor(size) != size ||
                size > static_cast<double>((std::numeric_limits<uint64_t>::max)())) {
                g_engine->throwException("createBuffer size must be a positive integer");
                return g_engine->newUndefined();
            }
            const uint64_t bufferSize = static_cast<uint64_t>(size);
            if (!canAllocateTrackedGpuBytes(bufferSize)) {
                g_engine->throwException("GPU memory budget exceeded by createBuffer");
                return g_engine->newUndefined();
            }

            // Check for mappedAtCreation
            auto mappedAtCreationProp = g_engine->getProperty(descriptor, "mappedAtCreation");
            bool mappedAtCreation = !g_engine->isUndefined(mappedAtCreationProp) && g_engine->toBoolean(mappedAtCreationProp);

            WGPUBufferDescriptor bufferDesc = {};
            bufferDesc.size = bufferSize;
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
            g_bufferRegistry[bufferId] = {buffer, bufferSize, (WGPUBufferUsage)(uint32_t)usage, mappedAtCreation, nullptr, 0, initialMapMode, false};
            g_trackedBufferBytes += bufferSize;
            updatePeakTrackedGpuBytes();

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
                        return g_asyncBridge.rejectedPromise("GPUBuffer not found");
                    }

                    auto& bufferInfo = it->second;

                    // Already mapped (mappedAtCreation)?
                    if (bufferInfo.isMapped) {
                        return g_asyncBridge.rejectedPromise("GPUBuffer is already mapped");
                    }
                    if (bufferInfo.mapPending) {
                        return g_asyncBridge.rejectedPromise("GPUBuffer already has a pending mapAsync operation");
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
                        return g_asyncBridge.rejectedPromise("GPUBuffer map range is out of bounds");
                    }
                    uint64_t mapSize = args.size() > 2
                        ? (uint64_t)g_engine->toNumber(args[2])
                        : bufferInfo.size - offset;
                    if (mapSize > bufferInfo.size - offset) {
                        return g_asyncBridge.rejectedPromise("GPUBuffer map range is out of bounds");
                    }

                    // Debug: Log buffer info
                    bool hasMapRead = (bufferInfo.usage & WGPUBufferUsage_MapRead) != 0;
                    (void)hasMapRead;  // Used for debug logging when enabled

                    // mapAsync is an execution boundary: submitting a command
                    // buffer that references a buffer after mapping has started is
                    // invalid. Readbacks therefore become an intentional extra
                    // native submit instead of waiting for endDawnFrame.
                    g_frameQueue.flushBeforeImmediateOperation();

                    js::JSValueHandle promise;
                    auto* pending = g_asyncBridge.createPromise(promise);
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
                    g_frameQueue.flushBeforeImmediateOperation();
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
}

} // namespace mystral::webgpu
