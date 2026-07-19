#include "webgpu/pipeline_bindings.h"

#include "webgpu/binding_internal.h"
#include "webgpu/format_conversions.h"
#include "mystral/webgpu_compat.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace mystral::webgpu {

using bridge::stringToFormat;

#if defined(MYSTRAL_WEBGPU_DAWN)
static void onRenderPipelineCreated(WGPUCreatePipelineAsyncStatus status,
                                    WGPURenderPipeline pipeline,
                                    WGPUStringView message,
                                    void* userdata1,
                                    void*) {
    auto* pending = static_cast<bridge::AsyncBridge::PendingPromise*>(userdata1);
    const std::string error = message.data && message.length > 0
        ? std::string(message.data, message.length)
        : std::string();
    g_asyncBridge.enqueue([pending, status, pipeline, error]() {
        if (!pending->active || pending->session != g_asyncBridge.session() || pending->engine != g_engine) {
            if (pipeline) wgpuRenderPipelineRelease(pipeline);
            g_asyncBridge.settle(pending, false, {});
            return;
        }
        if (status == WGPUCreatePipelineAsyncStatus_Success && pipeline) {
            g_asyncBridge.settle(pending, true, wrapRenderPipeline(pipeline));
        } else {
            if (pipeline) wgpuRenderPipelineRelease(pipeline);
            g_asyncBridge.settle(
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
    auto* pending = static_cast<bridge::AsyncBridge::PendingPromise*>(userdata1);
    const std::string error = message.data && message.length > 0
        ? std::string(message.data, message.length)
        : std::string();
    g_asyncBridge.enqueue([pending, status, pipeline, error]() {
        if (!pending->active || pending->session != g_asyncBridge.session() || pending->engine != g_engine) {
            if (pipeline) wgpuComputePipelineRelease(pipeline);
            g_asyncBridge.settle(pending, false, {});
            return;
        }
        if (status == WGPUCreatePipelineAsyncStatus_Success && pipeline) {
            g_asyncBridge.settle(pending, true, wrapComputePipeline(pipeline));
        } else {
            if (pipeline) wgpuComputePipelineRelease(pipeline);
            g_asyncBridge.settle(
                pending,
                false,
                {},
                error.empty() ? "Failed to create compute pipeline asynchronously" : error);
        }
    });
}
#endif

void releaseRenderPipeline(uint64_t id) {
    auto it = g_renderPipelineRegistry.find(id);
    if (it == g_renderPipelineRegistry.end()) return;
    wgpuRenderPipelineRelease(it->second);
    g_renderPipelineRegistry.erase(it);
}

void releaseComputePipeline(uint64_t id) {
    auto it = g_computePipelineRegistry.find(id);
    if (it == g_computePipelineRegistry.end()) return;
    wgpuComputePipelineRelease(it->second);
    g_computePipelineRegistry.erase(it);
}

js::JSValueHandle wrapRenderPipeline(WGPURenderPipeline pipeline) {
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

js::JSValueHandle wrapComputePipeline(WGPUComputePipeline pipeline) {
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

void installPipelineBindings(js::JSValueHandle device) {    // device.createShaderModule(descriptor)
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

            // Get vertex stage. entryPoint is OPTIONAL per spec - when
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
                auto* pending = g_asyncBridge.createPromise(promise);
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
            return asyncCreation ? g_asyncBridge.resolvedPromise(jsPipeline) : jsPipeline;
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
            // entryPoint is OPTIONAL per spec - zero-init/null selects
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
                auto* pending = g_asyncBridge.createPromise(promise);
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
            return asyncCreation ? g_asyncBridge.resolvedPromise(jsPipeline) : jsPipeline;
        });
    };
    g_engine->setProperty(device, "createComputePipeline", createComputePipelineBinding(false));
    g_engine->setProperty(device, "createComputePipelineAsync", createComputePipelineBinding(true));
}

} // namespace mystral::webgpu
