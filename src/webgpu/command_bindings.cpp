#include "webgpu/command_bindings.h"

#include "webgpu/binding_internal.h"
#include "mystral/webgpu_compat.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace mystral::webgpu {

js::JSValueHandle sharedMethod(js::JSValueHandle& slot,
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

void installCommandBindings(js::JSValueHandle device) {    // device.createCommandEncoder(descriptor?)
    g_engine->setProperty(device, "createCommandEncoder",
        g_engine->newFunction("createCommandEncoder", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            bridge::ScopedMeasurement measurement(bridge::Operation::CreateCommandEncoder);
            WGPUCommandEncoderDescriptor desc = {};
            WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(g_device, &desc);
            g_commandEncoders.trackCommandEncoder(encoder);

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
                    bridge::ScopedMeasurement measurement(bridge::Operation::BeginRenderPass);
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
                                g_commandEncoders.markSurfaceRenderPass(encoderToUse);
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
                    g_commandEncoders.trackRenderPass(encoderToUse, renderPass);

                    if (g_verboseLogging) std::cout << "[WebGPU] Render pass started (" << numAttachments << " attachments), clear: (" << firstR << "," << firstG << "," << firstB << "," << firstA << ")" << std::endl;

                    auto jsRenderPass = g_engine->newObject();
                    g_engine->setPrivateData(jsRenderPass, renderPass);
                    // end() is the single terminal owner. Avoid a
                    // GC-driven Dawn call while an encoder frame is
                    // still being assembled.

                    // A Three.js RenderPipeline can open an offscreen pass while
                    // the final surface pass is still alive. Every JS method must
                    // therefore target the encoder represented by this wrapper,
                    // not whichever pass happened to be opened most recently.
                    // renderPass.setPipeline(pipeline)
                    g_engine->setProperty(jsRenderPass, "setPipeline",
                        sharedMethod(g_transientMethods.renderSetPipeline, "setPipeline",
                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                            bridge::ScopedMeasurement measurement(bridge::Operation::RenderSetPipeline);
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
                            bridge::ScopedMeasurement measurement(bridge::Operation::RenderSetBindGroup);
                            if (args.size() < 2) {
                                g_engine->throwException("setBindGroup requires index and bindGroup");
                                return g_engine->newUndefined();
                            }

                            uint32_t groupIndex = (uint32_t)g_engine->toNumber(args[0]);
                            WGPURenderPassEncoder capturedRenderPassForCommands =
                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);
                            WGPUBindGroup bindGroup = (WGPUBindGroup)g_engine->getPrivateData(args[1]);

                            if (capturedRenderPassForCommands && bindGroup) {
                                std::vector<uint32_t> dynamicOffsets;
                                if (!parseDynamicOffsets(args, dynamicOffsets, "renderPass.setBindGroup")) {
                                    return g_engine->newUndefined();
                                }
                                wgpuRenderPassEncoderSetBindGroup(
                                    capturedRenderPassForCommands,
                                    groupIndex,
                                    bindGroup,
                                    dynamicOffsets.size(),
                                    dynamicOffsets.data());
                                if (g_verboseLogging) std::cout << "[WebGPU] Set bind group at index " << groupIndex << std::endl;
                            }

                            return g_engine->newUndefined();
                        })
                    );

                    // renderPass.draw(vertexCount, instanceCount?, firstVertex?, firstInstance?)
                    g_engine->setProperty(jsRenderPass, "draw",
                        sharedMethod(g_transientMethods.renderDraw, "draw",
                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                            bridge::ScopedMeasurement measurement(bridge::Operation::RenderDraw);
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
                            bridge::ScopedMeasurement measurement(bridge::Operation::RenderSetVertexBuffer);
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
                            bridge::ScopedMeasurement measurement(bridge::Operation::RenderSetIndexBuffer);
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
                            bridge::ScopedMeasurement measurement(bridge::Operation::RenderDrawIndexed);
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
                            bridge::ScopedMeasurement measurement(bridge::Operation::RenderDrawIndirect);
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
                            bridge::ScopedMeasurement measurement(bridge::Operation::RenderDrawIndexedIndirect);
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
                            bridge::ScopedMeasurement measurement(bridge::Operation::RenderExecuteBundles);
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
                            bridge::ScopedMeasurement measurement(bridge::Operation::RenderEnd);
                            WGPURenderPassEncoder pass =
                                (WGPURenderPassEncoder)g_engine->getPrivateData(receiver);
                            g_commandEncoders.closeRenderPass(pass);
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
                    bridge::ScopedMeasurement measurement(bridge::Operation::BeginComputePass);
                    WGPUCommandEncoder encoder =
                        (WGPUCommandEncoder)g_engine->getPrivateData(receiver);
                    if (!encoder) {
                        g_engine->throwException("No command encoder");
                        return g_engine->newUndefined();
                    }

                    WGPUComputePassDescriptor computePassDesc = {};
                    WGPUComputePassEncoder computePass =
                        wgpuCommandEncoderBeginComputePass(encoder, &computePassDesc);
                    g_commandEncoders.trackComputePass(encoder, computePass);

                    auto jsComputePass = g_engine->newObject();
                    g_engine->setPrivateData(jsComputePass, computePass);
                    // end() is the single terminal owner. Avoid a
                    // GC-driven Dawn call while compute work is
                    // still being encoded.

                    // computePass.setPipeline(pipeline)
                    g_engine->setProperty(jsComputePass, "setPipeline",
                        sharedMethod(g_transientMethods.computeSetPipeline, "setPipeline",
                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                            bridge::ScopedMeasurement measurement(bridge::Operation::ComputeSetPipeline);
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
                            bridge::ScopedMeasurement measurement(bridge::Operation::ComputeSetBindGroup);
                            if (args.size() < 2) return g_engine->newUndefined();
                            uint32_t index = (uint32_t)g_engine->toNumber(args[0]);
                            WGPUComputePassEncoder pass =
                                (WGPUComputePassEncoder)g_engine->getPrivateData(receiver);
                            WGPUBindGroup bindGroup = (WGPUBindGroup)g_engine->getPrivateData(args[1]);
                            if (pass && bindGroup) {
                                std::vector<uint32_t> dynamicOffsets;
                                if (!parseDynamicOffsets(args, dynamicOffsets, "computePass.setBindGroup")) {
                                    return g_engine->newUndefined();
                                }
                                wgpuComputePassEncoderSetBindGroup(
                                    pass,
                                    index,
                                    bindGroup,
                                    dynamicOffsets.size(),
                                    dynamicOffsets.data());
                            }
                            return g_engine->newUndefined();
                        })
                    );

                    // computePass.dispatchWorkgroups(countX, countY?, countZ?)
                    g_engine->setProperty(jsComputePass, "dispatchWorkgroups",
                        sharedMethod(g_transientMethods.computeDispatchWorkgroups, "dispatchWorkgroups",
                            [](void* ctx, js::JSValueHandle receiver, const std::vector<js::JSValueHandle>& args) {
                            bridge::ScopedMeasurement measurement(bridge::Operation::ComputeDispatchWorkgroups);
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
                            bridge::ScopedMeasurement measurement(bridge::Operation::ComputeEnd);
                            WGPUComputePassEncoder pass =
                                (WGPUComputePassEncoder)g_engine->getPrivateData(receiver);
                            g_commandEncoders.closeComputePass(pass);
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
                    bridge::ScopedMeasurement measurement(bridge::Operation::FinishCommandEncoder);
                    WGPUCommandEncoder encoderToFinish =
                        (WGPUCommandEncoder)g_engine->getPrivateData(receiver);
                    if (!encoderToFinish || !g_commandEncoders.isCommandEncoderLive(encoderToFinish)) {
                        g_engine->throwException("Command encoder is already finished");
                        return g_engine->newUndefined();
                    }

                    // Auto-end any active render/compute passes for THIS encoder
                    // Look up from per-encoder map, not global
                    WGPURenderPassEncoder renderPass = g_commandEncoders.renderPassFor(encoderToFinish);
                    if (renderPass) {
                        if (g_verboseLogging) std::cout << "[WebGPU] Auto-ending render pass (pass=" << (void*)renderPass << ", encoder=" << (void*)encoderToFinish << ")" << std::endl;
                        g_commandEncoders.closeRenderPass(renderPass);
                    }

                    WGPUComputePassEncoder computePass = g_commandEncoders.computePassFor(encoderToFinish);
                    if (computePass) {
                        if (g_verboseLogging) std::cout << "[WebGPU] Auto-ending compute pass (pass=" << (void*)computePass << ", encoder=" << (void*)encoderToFinish << ")" << std::endl;
                        g_commandEncoders.closeComputePass(computePass);
                    }

                    WGPUCommandBufferDescriptor cmdDesc = {};
                    WGPUCommandBuffer cmdBuffer = nullptr;

                    if (encoderToFinish) {
                        cmdBuffer = wgpuCommandEncoderFinish(encoderToFinish, &cmdDesc);
                        g_commandEncoders.finishCommandEncoder(encoderToFinish);
                        g_engine->setPrivateData(receiver, nullptr);

                        if (g_verboseLogging) std::cout << "[WebGPU] Command encoder finished, buffer: " << cmdBuffer << std::endl;
                    }

                    auto jsCommandBuffer = g_engine->newObject();
                    g_engine->setPrivateData(jsCommandBuffer, cmdBuffer);
                    g_commandEncoders.recordCommandBuffer(cmdBuffer);

                    return jsCommandBuffer;
                })
            );

            return jsEncoder;
        })
    );
}

} // namespace mystral::webgpu
