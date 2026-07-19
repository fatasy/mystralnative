#include "webgpu/navigator_bindings.h"

#include "webgpu/binding_internal.h"
#include "webgpu/buffer_bindings.h"
#include "webgpu/command_bindings.h"
#include "webgpu/format_conversions.h"
#include "webgpu/pipeline_bindings.h"
#include "webgpu/queue_bindings.h"
#include "webgpu/resource_bindings.h"

#include <iostream>
#include <string>
#include <vector>

namespace mystral::webgpu {

using bridge::formatToString;

static const char* errorScopeName(WGPUErrorType type) {
    switch (type) {
        case WGPUErrorType_Validation: return "GPUValidationError";
        case WGPUErrorType_OutOfMemory: return "GPUOutOfMemoryError";
        case WGPUErrorType_Internal: return "GPUInternalError";
        default: return "GPUError";
    }
}

static void onErrorScopePopped(WGPUPopErrorScopeStatus status,
                               WGPUErrorType type,
                               WGPUStringView message,
                               void* userdata1,
                               void*) {
    auto* pending = static_cast<bridge::AsyncBridge::PendingPromise*>(userdata1);
    const std::string errorMessage = message.data && message.length > 0
        ? std::string(message.data, message.length)
        : std::string();
    g_asyncBridge.enqueue([pending, status, type, errorMessage]() {
        if (!pending->active || pending->session != g_asyncBridge.session() ||
            pending->engine != g_engine) {
            g_asyncBridge.settle(pending, false, {});
            return;
        }
        if (status != WGPUPopErrorScopeStatus_Success) {
            g_asyncBridge.settle(
                pending,
                false,
                {},
                errorMessage.empty() ? "Failed to pop WebGPU error scope" : errorMessage);
            return;
        }
        if (type == WGPUErrorType_NoError) {
            g_asyncBridge.settle(pending, true, g_engine->newNull());
            return;
        }

        auto error = g_engine->newObject();
        g_engine->setProperty(error, "name", g_engine->newString(errorScopeName(type)));
        g_engine->setProperty(error, "message", g_engine->newString(errorMessage.c_str()));
        g_asyncBridge.settle(pending, true, error);
    });
}

void installNavigatorBindings(js::Engine* engine) {    // ========================================================================
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

                    installQueueBindings(device);

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
                    g_capabilities.applyDeviceLimits(deviceLimits);
                    g_engine->setProperty(device, "limits", deviceLimits);

                    // device.features - reflect what the live device actually has
                    // (queried through wgpuDeviceHasFeature, not hardcoded)
                    g_engine->setProperty(device, "features", g_capabilities.deviceFeatures());

                    installBufferBindings(device);

                    installPipelineBindings(device);

                    installCommandBindings(device);

                    installResourceBindings(device);

                    // device.pushErrorScope(filter) - Push an error scope for validation/OOM/internal errors
                    // Used by Three.js for error handling during pipeline creation
                    g_engine->setProperty(device, "pushErrorScope",
                        g_engine->newFunction("pushErrorScope", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (args.empty()) {
                                g_engine->throwException("pushErrorScope requires a filter");
                                return g_engine->newUndefined();
                            }
                            const std::string filter = g_engine->toString(args[0]);
                            WGPUErrorFilter nativeFilter;
                            if (filter == "validation") nativeFilter = WGPUErrorFilter_Validation;
                            else if (filter == "out-of-memory") nativeFilter = WGPUErrorFilter_OutOfMemory;
                            else if (filter == "internal") nativeFilter = WGPUErrorFilter_Internal;
                            else {
                                g_engine->throwException("Invalid GPUErrorFilter");
                                return g_engine->newUndefined();
                            }
                            wgpuDevicePushErrorScope(g_device, nativeFilter);
                            if (g_verboseLogging) {
                                std::cout << "[WebGPU] pushErrorScope: " << filter << std::endl;
                            }
                            return g_engine->newUndefined();
                        })
                    );

                    // device.popErrorScope() - Pop an error scope and return Promise<GPUError | null>
                    // Returns Promise<GPUError | null>
                    g_engine->setProperty(device, "popErrorScope",
                        g_engine->newFunction("popErrorScope", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
                            if (g_verboseLogging) {
                                std::cout << "[WebGPU] popErrorScope" << std::endl;
                            }
                            js::JSValueHandle promise;
                            auto* pending = g_asyncBridge.createPromise(promise);
                            if (!pending) {
                                g_engine->throwException("Failed to create popErrorScope Promise");
                                return g_engine->newUndefined();
                            }

                            WGPUPopErrorScopeCallbackInfo callbackInfo = {};
                            callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
                            callbackInfo.callback = onErrorScopePopped;
                            callbackInfo.userdata1 = pending;
                            wgpuDevicePopErrorScope(g_device, callbackInfo);
                            return promise;
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
            g_engine->setProperty(adapter, "features", g_capabilities.adapterFeatures());

            // adapter.limits - the adapter's real limits (falls back to the
            // device when the adapter handle was not provided)
            auto limits = g_engine->newObject();
            g_capabilities.applyAdapterLimits(limits);
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
}

} // namespace mystral::webgpu
