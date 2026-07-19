#include "webgpu/diagnostics_bindings.h"

#include "webgpu/binding_internal.h"
#include "webgpu/bridge_profiler.h"

namespace mystral::canvas {
size_t canvas2DContextCount();
}

namespace mystral::webgpu {

void installDiagnosticsBindings(js::Engine* engine) {
    engine->setGlobalProperty("__mystralSetWebGpuProfiling",
        engine->newFunction("__mystralSetWebGpuProfiling", [](void*, const std::vector<js::JSValueHandle>& args) {
            const bool enabled = !args.empty() && g_engine->toBoolean(args[0]);
            if (args.size() > 1 && g_engine->toBoolean(args[1])) bridge::reset();
            bridge::setEnabled(enabled);
            return g_engine->newUndefined();
        })
    );

    engine->setGlobalProperty("__mystralWebGpuStats",
        engine->newFunction("__mystralWebGpuStats", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            auto result = g_engine->newObject();
            auto set = [&](const char* name, uint64_t value) {
                g_engine->setProperty(result, name, g_engine->newNumber((double)value));
            };
            const auto& encoderStats = g_commandEncoders.stats();
            set("commandEncodersCreated", encoderStats.commandEncodersCreated);
            set("renderPassesCreated", encoderStats.renderPassesCreated);
            set("computePassesCreated", encoderStats.computePassesCreated);
            set("commandBuffersCreated", encoderStats.commandBuffersCreated);
            const auto& queueStats = g_frameQueue.stats();
            set("nativeQueueSubmits", queueStats.nativeQueueSubmits);
            set("nativeCommandBuffersSubmitted", queueStats.nativeCommandBuffersSubmitted);
            set("nativeQueueSubmitNanoseconds", queueStats.nativeQueueSubmitNanoseconds);
            set("deferredUploadCommandBuffers", queueStats.deferredUploadCommandBuffers);
            set("forcedFrameFlushes", queueStats.forcedFrameFlushes);
            set("maxCommandBuffersPerNativeSubmit", queueStats.maxCommandBuffersPerNativeSubmit);
            set("uploadBufferAllocations", queueStats.uploadBufferAllocations);
            set("uploadBufferCapacityBytes", queueStats.uploadBufferCapacityBytes);
            set("peakUploadBytesPerFlush", queueStats.peakUploadBytesPerFlush);
            set("frameLatencyWaits", queueStats.frameLatencyWaits);
            set("frameLatencyWaitNanoseconds", queueStats.frameLatencyWaitNanoseconds);
            set("maxInFlightSubmissions", queueStats.maxInFlightSubmissions);
            set("dawnProgressPumps", queueStats.progressPumps);
            set("dawnProgressPumpNanoseconds", queueStats.progressPumpNanoseconds);
            const auto& screenshotStats = g_screenshot.stats();
            set("screenshotRequests", screenshotStats.requests);
            set("screenshotCapturedFrames", screenshotStats.capturedFrames);
            set("screenshotCapturedBytes", screenshotStats.capturedBytes);
            set("screenshotBufferBytes", g_screenshot.bufferSize());
            const auto& compositorStats = g_canvasCompositor.stats();
            set("canvas2DFramesComposited", compositorStats.framesComposited);
            set("canvas2DFramesWithoutUpload", compositorStats.framesWithoutUpload);
            set("canvas2DTextureUploads", compositorStats.textureUploads);
            set("canvas2DTextureUploadBytes", compositorStats.textureUploadBytes);
            set("activeCommandEncoders", g_commandEncoders.liveCommandEncoderCount());
            set("activeRenderPasses", g_commandEncoders.liveRenderPassCount());
            set("activeComputePasses", g_commandEncoders.liveComputePassCount());
            set("activeBuffers", g_bufferRegistry.size());
            set("activeTextures", g_textureRegistry.size());
            set("activeRenderPipelines", g_renderPipelineRegistry.size());
            set("activeComputePipelines", g_computePipelineRegistry.size());
            set("trackedBufferBytes", g_trackedBufferBytes);
            set("estimatedTextureBytes", g_estimatedTextureBytes);
            set("trackedGpuBytes", g_trackedBufferBytes + g_estimatedTextureBytes);
            set("peakTrackedGpuBytes", g_peakTrackedGpuBytes);
            set("maxTrackedGpuMemoryBytes", g_maxTrackedGpuMemoryBytes);
            set("activeOffscreenCanvases", g_offscreenCanvases.size());
            set("activeCanvas2DContexts", canvas::canvas2DContextCount());
            set("pendingAsyncOperations", g_asyncBridge.pendingCount());
            set("queuedAsyncCompletions", g_asyncBridge.queuedCount());
            set("bridgeProfilingEnabled", bridge::isEnabled() ? 1 : 0);

            auto bridge = g_engine->newObject();
            const auto& bridgeMetrics = bridge::metrics();
            const auto& bridgeNames = bridge::names();
            for (size_t index = 0; index < bridgeMetrics.size(); index++) {
                const auto& metric = bridgeMetrics[index];
                auto operation = g_engine->newObject();
                g_engine->setProperty(operation, "calls", g_engine->newNumber((double)metric.calls));
                g_engine->setProperty(operation, "totalNanoseconds",
                    g_engine->newNumber((double)metric.totalNanoseconds));
                g_engine->setProperty(operation, "bytes", g_engine->newNumber((double)metric.bytes));
                g_engine->setProperty(bridge, bridgeNames[index], operation);
            }
            g_engine->setProperty(result, "profiledBridge", bridge);
            return result;
        })
    );
}

} // namespace mystral::webgpu
