#include "webgpu/canvas_compositor.h"

#include "mystral/webgpu_compat.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace mystral::webgpu::bridge {

void CanvasCompositor::configure(
    WGPUDevice device,
    WGPUQueue queue,
    WGPUSurface surface,
    WGPUTextureFormat surfaceFormat) {
    device_ = device;
    queue_ = queue;
    surface_ = surface;
    surfaceFormat_ = surfaceFormat;
}

void CanvasCompositor::setSurfaceFormat(WGPUTextureFormat surfaceFormat) {
    if (surfaceFormat_ == surfaceFormat) return;
    surfaceFormat_ = surfaceFormat;
    if (pipeline_) {
        wgpuRenderPipelineRelease(pipeline_);
        pipeline_ = nullptr;
    }
    if (bindGroup_) {
        wgpuBindGroupRelease(bindGroup_);
        bindGroup_ = nullptr;
    }
}

bool CanvasCompositor::composite(
    uint32_t canvasWidth, uint32_t canvasHeight, FrameQueue& frameQueue) {
    if (!context_ || !device_ || !queue_ || !surface_) {
        return false;
    }

    const bool pixelsChanged = context_->isDirty();
    if (!pixelsChanged) stats_.framesWithoutUpload++;

    // Get Canvas 2D pixel data
    const uint8_t* pixelData = context_->getPixelData();
    size_t pixelDataSize = context_->getPixelDataSize();
    int width = context_->getWidth();
    int height = context_->getHeight();

    if (!pixelData || pixelDataSize == 0) {
        return false;
    }

    // Create or resize texture if needed
    if (!texture_ || textureWidth_ != (uint32_t)width || textureHeight_ != (uint32_t)height) {
        if (texture_) {
            wgpuTextureDestroy(texture_);
            wgpuTextureRelease(texture_);
        }
        if (bindGroup_) {
            wgpuBindGroupRelease(bindGroup_);
            bindGroup_ = nullptr;
        }

        WGPUTextureDescriptor texDesc = {};
        texDesc.size = {(uint32_t)width, (uint32_t)height, 1};
        texDesc.mipLevelCount = 1;
        texDesc.sampleCount = 1;
        texDesc.dimension = WGPUTextureDimension_2D;
        texDesc.format = WGPUTextureFormat_RGBA8Unorm;
        texDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;

        texture_ = wgpuDeviceCreateTexture(device_, &texDesc);
        textureWidth_ = width;
        textureHeight_ = height;
        if (!texture_) return false;
    }

    if (pixelsChanged) {
        const auto dirty = context_->getDirtyRect();
        const uint32_t uploadWidth = static_cast<uint32_t>(dirty.width);
        const uint32_t uploadHeight = static_cast<uint32_t>(dirty.height);
        const uint32_t sourceBytesPerRow = static_cast<uint32_t>(width) * 4;
        const uint32_t uploadBytesPerRow = ((uploadWidth * 4 + 255) / 256) * 256;
        uploadScratch_.resize(static_cast<size_t>(uploadBytesPerRow) * uploadHeight);
        for (uint32_t row = 0; row < uploadHeight; ++row) {
            const auto* source = pixelData +
                static_cast<size_t>(dirty.y + row) * sourceBytesPerRow +
                static_cast<size_t>(dirty.x) * 4;
            std::memcpy(
                uploadScratch_.data() + static_cast<size_t>(row) * uploadBytesPerRow,
                source,
                static_cast<size_t>(uploadWidth) * 4);
        }

        WGPUImageCopyTexture_Compat destTexture = {};
        destTexture.texture = texture_;
        destTexture.mipLevel = 0;
        destTexture.origin = {
            static_cast<uint32_t>(dirty.x),
            static_cast<uint32_t>(dirty.y),
            0,
        };
        destTexture.aspect = WGPUTextureAspect_All;

        WGPUTextureDataLayout_Compat dataLayout = {};
        dataLayout.offset = 0;
        dataLayout.bytesPerRow = uploadBytesPerRow;
        dataLayout.rowsPerImage = uploadHeight;

        WGPUExtent3D writeSize = {uploadWidth, uploadHeight, 1};
        wgpuQueueWriteTexture(
            queue_, &destTexture, uploadScratch_.data(), uploadScratch_.size(),
            &dataLayout, &writeSize);
        context_->clearDirty();
        stats_.textureUploads++;
        stats_.textureUploadBytes += uploadScratch_.size();
    }

    // Create pipeline if needed
    if (!pipeline_) {
        // Simple fullscreen quad shader
        const char* shaderCode = R"(
            @group(0) @binding(0) var texSampler: sampler;
            @group(0) @binding(1) var tex: texture_2d<f32>;

            struct VertexOutput {
                @builtin(position) position: vec4f,
                @location(0) uv: vec2f,
            }

            @vertex
            fn vs_main(@builtin(vertex_index) vertexIndex: u32) -> VertexOutput {
                var positions = array<vec2f, 6>(
                    vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(-1.0, 1.0),
                    vec2f(-1.0, 1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0)
                );
                var uvs = array<vec2f, 6>(
                    vec2f(0.0, 1.0), vec2f(1.0, 1.0), vec2f(0.0, 0.0),
                    vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(1.0, 0.0)
                );
                var output: VertexOutput;
                output.position = vec4f(positions[vertexIndex], 0.0, 1.0);
                output.uv = uvs[vertexIndex];
                return output;
            }

            @fragment
            fn fs_main(input: VertexOutput) -> @location(0) vec4f {
                return textureSample(tex, texSampler, input.uv);
            }
        )";

        WGPUShaderModuleWGSLDescriptor_Compat wgslDesc = {};
        WGPUShaderModuleDescriptor shaderDesc = {};
        setupShaderModuleWGSL(&shaderDesc, &wgslDesc, shaderCode);

        WGPUShaderModule shaderModule = wgpuDeviceCreateShaderModule(device_, &shaderDesc);

        // Create sampler
        WGPUSamplerDescriptor samplerDesc = {};
        samplerDesc.magFilter = WGPUFilterMode_Linear;
        samplerDesc.minFilter = WGPUFilterMode_Linear;
        samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Linear;
        samplerDesc.addressModeU = WGPUAddressMode_ClampToEdge;
        samplerDesc.addressModeV = WGPUAddressMode_ClampToEdge;
        samplerDesc.addressModeW = WGPUAddressMode_ClampToEdge;
        samplerDesc.maxAnisotropy = 1;
        samplerDesc.lodMinClamp = 0.0f;
        samplerDesc.lodMaxClamp = 1.0f;
        if (!sampler_) sampler_ = wgpuDeviceCreateSampler(device_, &samplerDesc);

        // Create bind group layout
        WGPUBindGroupLayoutEntry bgLayoutEntries[2] = {};
        bgLayoutEntries[0].binding = 0;
        bgLayoutEntries[0].visibility = WGPUShaderStage_Fragment;
        bgLayoutEntries[0].sampler.type = WGPUSamplerBindingType_Filtering;
        bgLayoutEntries[1].binding = 1;
        bgLayoutEntries[1].visibility = WGPUShaderStage_Fragment;
        bgLayoutEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
        bgLayoutEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

        WGPUBindGroupLayoutDescriptor bgLayoutDesc = {};
        bgLayoutDesc.entryCount = 2;
        bgLayoutDesc.entries = bgLayoutEntries;
        WGPUBindGroupLayout bgLayout = wgpuDeviceCreateBindGroupLayout(device_, &bgLayoutDesc);

        // Create pipeline layout
        WGPUPipelineLayoutDescriptor pipelineLayoutDesc = {};
        pipelineLayoutDesc.bindGroupLayoutCount = 1;
        pipelineLayoutDesc.bindGroupLayouts = &bgLayout;
        WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(device_, &pipelineLayoutDesc);

        // Create pipeline
        WGPURenderPipelineDescriptor pipelineDesc = {};
        pipelineDesc.layout = pipelineLayout;
        pipelineDesc.vertex.module = shaderModule;
        WGPU_SET_ENTRY_POINT(pipelineDesc.vertex, "vs_main");

        WGPUFragmentState fragmentState = {};
        fragmentState.module = shaderModule;
        WGPU_SET_ENTRY_POINT(fragmentState, "fs_main");
        fragmentState.targetCount = 1;

        WGPUColorTargetState colorTarget = {};
        colorTarget.format = surfaceFormat_;
        colorTarget.writeMask = WGPUColorWriteMask_All;
        fragmentState.targets = &colorTarget;

        pipelineDesc.fragment = &fragmentState;
        pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        pipelineDesc.multisample.count = 1;
        pipelineDesc.multisample.mask = 0xFFFFFFFF;

        pipeline_ = wgpuDeviceCreateRenderPipeline(device_, &pipelineDesc);

        wgpuShaderModuleRelease(shaderModule);
        wgpuPipelineLayoutRelease(pipelineLayout);
        wgpuBindGroupLayoutRelease(bgLayout);

        if (!pipeline_) {
            std::cerr << "[Canvas2D] Failed to create compositing pipeline" << std::endl;
            return false;
        }
    }

    // Create bind group (recreate if texture changed)
    if (!bindGroup_) {
        if (!sampler_ || !texture_) {
            return false;
        }

        WGPUTextureViewDescriptor viewDesc = {};
        viewDesc.format = WGPUTextureFormat_RGBA8Unorm;
        viewDesc.dimension = WGPUTextureViewDimension_2D;
        viewDesc.baseMipLevel = 0;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = 0;
        viewDesc.arrayLayerCount = 1;
        WGPUTextureView texView = wgpuTextureCreateView(texture_, &viewDesc);

        if (!texView) {
            return false;
        }

        WGPUBindGroupEntry bgEntries[2] = {};
        bgEntries[0].binding = 0;
        bgEntries[0].sampler = sampler_;
        bgEntries[1].binding = 1;
        bgEntries[1].textureView = texView;

        WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(pipeline_, 0);
        if (!layout) {
            wgpuTextureViewRelease(texView);
            return false;
        }

        WGPUBindGroupDescriptor bgDesc = {};
        bgDesc.layout = layout;
        bgDesc.entryCount = 2;
        bgDesc.entries = bgEntries;
        bindGroup_ = wgpuDeviceCreateBindGroup(device_, &bgDesc);

        wgpuBindGroupLayoutRelease(layout);
        wgpuTextureViewRelease(texView);

        if (!bindGroup_) {
            return false;
        }
    }

    // Get surface texture
    WGPUSurfaceTexture surfaceTexture;
    wgpuSurfaceGetCurrentTexture(surface_, &surfaceTexture);
    if (!wgpuSurfaceTextureStatusIsSuccess(surfaceTexture.status)) {
        return false;
    }

    WGPUTextureViewDescriptor surfaceViewDesc = {};
    surfaceViewDesc.format = surfaceFormat_;
    surfaceViewDesc.dimension = WGPUTextureViewDimension_2D;
    surfaceViewDesc.baseMipLevel = 0;
    surfaceViewDesc.mipLevelCount = 1;
    surfaceViewDesc.baseArrayLayer = 0;
    surfaceViewDesc.arrayLayerCount = 1;
    WGPUTextureView surfaceView = wgpuTextureCreateView(surfaceTexture.texture, &surfaceViewDesc);
    if (!surfaceView) {
        wgpuTextureRelease(surfaceTexture.texture);
        return false;
    }

    // Create command encoder and render pass
    WGPUCommandEncoderDescriptor encDesc = {};
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, &encDesc);
    if (!encoder) {
        wgpuTextureViewRelease(surfaceView);
        wgpuTextureRelease(surfaceTexture.texture);
        return false;
    }

    WGPURenderPassColorAttachment colorAttachment = {};
    colorAttachment.view = surfaceView;
    colorAttachment.loadOp = WGPULoadOp_Clear;
    colorAttachment.storeOp = WGPUStoreOp_Store;
    colorAttachment.clearValue = {0.0, 0.0, 0.0, 1.0};
#if defined(MYSTRAL_WEBGPU_DAWN)
    colorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#endif

    WGPURenderPassDescriptor renderPassDesc = {};
    renderPassDesc.colorAttachmentCount = 1;
    renderPassDesc.colorAttachments = &colorAttachment;

    WGPURenderPassEncoder renderPass = wgpuCommandEncoderBeginRenderPass(encoder, &renderPassDesc);
    if (!renderPass) {
        wgpuCommandEncoderRelease(encoder);
        wgpuTextureViewRelease(surfaceView);
        wgpuTextureRelease(surfaceTexture.texture);
        return false;
    }
    wgpuRenderPassEncoderSetPipeline(renderPass, pipeline_);
    wgpuRenderPassEncoderSetBindGroup(renderPass, 0, bindGroup_, 0, nullptr);
    wgpuRenderPassEncoderDraw(renderPass, 6, 1, 0, 0);
    wgpuRenderPassEncoderEnd(renderPass);
    wgpuRenderPassEncoderRelease(renderPass);

    bool capturedScreenshot = false;
    if (screenshot_.shouldCapture()) {
        WGPUBuffer screenshotBuffer =
            screenshot_.ensureBuffer(device_, canvasWidth, canvasHeight);
        if (screenshotBuffer) {
            WGPUImageCopyTexture_Compat srcCopy = {};
            srcCopy.texture = surfaceTexture.texture;
            srcCopy.mipLevel = 0;
            srcCopy.origin = {0, 0, 0};
            srcCopy.aspect = WGPUTextureAspect_All;

            WGPUImageCopyBuffer_Compat dstCopy = {};
            dstCopy.buffer = screenshotBuffer;
            dstCopy.layout.offset = 0;
            dstCopy.layout.bytesPerRow = screenshot_.bytesPerRow();
            dstCopy.layout.rowsPerImage = canvasHeight;

            WGPUExtent3D copySize = {canvasWidth, canvasHeight, 1};
            wgpuCommandEncoderCopyTextureToBuffer_Compat(
                encoder, &srcCopy, &dstCopy, &copySize);
            capturedScreenshot = true;
        }
    }

    WGPUCommandBufferDescriptor cmdDesc = {};
    WGPUCommandBuffer cmdBuffer = wgpuCommandEncoderFinish(encoder, &cmdDesc);
    wgpuCommandEncoderRelease(encoder);
    if (!cmdBuffer) {
        wgpuTextureViewRelease(surfaceView);
        wgpuTextureRelease(surfaceTexture.texture);
        return false;
    }

    std::vector<WGPUCommandBuffer> commandBuffers = {cmdBuffer};
    frameQueue.submit(std::move(commandBuffers));
    surfaceTexture_ = surfaceTexture.texture;
    surfaceView_ = surfaceView;
    if (capturedScreenshot) screenshot_.markCaptured(canvasWidth, canvasHeight);
    stats_.framesComposited++;
    return true;
}

void CanvasCompositor::present() {
    if (!surfaceTexture_) return;
    wgpuSurfacePresent(surface_);
    screenshot_.beginPresentedFrame();
    if (surfaceView_) {
        wgpuTextureViewRelease(surfaceView_);
        surfaceView_ = nullptr;
    }
    wgpuTextureRelease(surfaceTexture_);
    surfaceTexture_ = nullptr;
}

} // namespace mystral::webgpu::bridge
