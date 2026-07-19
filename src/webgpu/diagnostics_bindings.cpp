#include "webgpu/diagnostics_bindings.h"

#include "webgpu/binding_internal.h"
#include "webgpu/bridge_profiler.h"

namespace mystral::canvas {
size_t canvas2DContextCount();
}

namespace mystral::webgpu {

void installDiagnosticsBindings(js::Engine* engine) {    engine->setGlobalProperty("__mystralWebGpuStats",
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
            set("activeCommandEncoders", g_commandEncoders.liveCommandEncoderCount());
            set("activeRenderPasses", g_commandEncoders.liveRenderPassCount());
            set("activeComputePasses", g_commandEncoders.liveComputePassCount());
            set("activeBuffers", g_bufferRegistry.size());
            set("activeTextures", g_textureRegistry.size());
            set("activeRenderPipelines", g_renderPipelineRegistry.size());
            set("activeComputePipelines", g_computePipelineRegistry.size());
            set("activeOffscreenCanvases", g_offscreenCanvases.size());
            set("activeCanvas2DContexts", canvas::canvas2DContextCount());
            set("pendingAsyncOperations", g_asyncBridge.pendingCount());
            set("queuedAsyncCompletions", g_asyncBridge.queuedCount());

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