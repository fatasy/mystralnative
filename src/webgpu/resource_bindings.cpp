#include "webgpu/resource_bindings.h"

#include "webgpu/binding_internal.h"
#include "webgpu/format_conversions.h"
#include "mystral/webgpu_compat.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace mystral::webgpu {

using bridge::stringToAddressMode;
using bridge::stringToCompareFunction;
using bridge::stringToFilterMode;
using bridge::stringToFormat;
using bridge::stringToMipmapFilterMode;
using bridge::stringToTextureDimension;
using bridge::stringToTextureViewDimension;

void releaseTexture(uint64_t id, bool destroy) {
    auto it = g_textureRegistry.find(id);
    if (it == g_textureRegistry.end()) return;
    if (destroy && it->second.destroyOnReload) {
        wgpuTextureDestroy(it->second.texture);
    }
    if (it->second.ownsReference) {
        wgpuTextureRelease(it->second.texture);
    }
    g_textureRegistry.erase(it);
}

void installResourceBindings(js::JSValueHandle device) {    // device.createTexture(descriptor)
    g_engine->setProperty(device, "createTexture",
        g_engine->newFunction("createTexture", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                g_engine->throwException("createTexture requires a descriptor");
                return g_engine->newUndefined();
            }

            auto descriptor = args[0];

            // Parse size - can be [width, height, depth] array or {width, height, depthOrArrayLayers} object
            auto sizeVal = g_engine->getProperty(descriptor, "size");
            uint32_t width = 1, height = 1, depthOrArrayLayers = 1;

            // Check if size is an array
            auto lengthProp = g_engine->getProperty(sizeVal, "length");
            if (!g_engine->isUndefined(lengthProp)) {
                // Array format: [width, height?, depth?]
                int len = (int)g_engine->toNumber(lengthProp);
                if (len >= 1) width = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeVal, 0));
                if (len >= 2) height = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeVal, 1));
                if (len >= 3) depthOrArrayLayers = (uint32_t)g_engine->toNumber(g_engine->getPropertyIndex(sizeVal, 2));
            } else {
                // Object format: {width, height, depthOrArrayLayers}
                auto w = g_engine->getProperty(sizeVal, "width");
                auto h = g_engine->getProperty(sizeVal, "height");
                auto d = g_engine->getProperty(sizeVal, "depthOrArrayLayers");
                if (!g_engine->isUndefined(w)) width = (uint32_t)g_engine->toNumber(w);
                if (!g_engine->isUndefined(h)) height = (uint32_t)g_engine->toNumber(h);
                if (!g_engine->isUndefined(d)) depthOrArrayLayers = (uint32_t)g_engine->toNumber(d);
            }

            // Parse format
            std::string formatStr = g_engine->toString(g_engine->getProperty(descriptor, "format"));
            WGPUTextureFormat format = stringToFormat(formatStr);

            // Parse usage
            double usageVal = g_engine->toNumber(g_engine->getProperty(descriptor, "usage"));
            WGPUTextureUsage usage = (WGPUTextureUsage)(uint32_t)usageVal;

            // Fix format/usage incompatibility:
            // BGRA8UnormSrgb doesn't support StorageBinding, convert to BGRA8Unorm or RGBA8Unorm
            if (format == WGPUTextureFormat_BGRA8UnormSrgb && (usage & WGPUTextureUsage_StorageBinding)) {
                std::cout << "[WebGPU] Warning: BGRA8UnormSrgb doesn't support StorageBinding, using RGBA8Unorm instead" << std::endl;
                format = WGPUTextureFormat_RGBA8Unorm;
                formatStr = "rgba8unorm";
            }
            // Also handle BGRA8Unorm which may not support storage on all platforms
            if (format == WGPUTextureFormat_BGRA8Unorm && (usage & WGPUTextureUsage_StorageBinding)) {
                std::cout << "[WebGPU] Warning: BGRA8Unorm may not support StorageBinding, using RGBA8Unorm instead" << std::endl;
                format = WGPUTextureFormat_RGBA8Unorm;
                formatStr = "rgba8unorm";
            }

            // Parse optional properties
            std::string dimensionStr = g_engine->toString(g_engine->getProperty(descriptor, "dimension"));
            WGPUTextureDimension dimension = dimensionStr.empty() ? WGPUTextureDimension_2D : stringToTextureDimension(dimensionStr);

            auto mipLevelCountVal = g_engine->getProperty(descriptor, "mipLevelCount");
            uint32_t mipLevelCount = g_engine->isUndefined(mipLevelCountVal) ? 1 : (uint32_t)g_engine->toNumber(mipLevelCountVal);

            auto sampleCountVal = g_engine->getProperty(descriptor, "sampleCount");
            uint32_t sampleCount = g_engine->isUndefined(sampleCountVal) ? 1 : (uint32_t)g_engine->toNumber(sampleCountVal);

            // Create texture descriptor
            WGPUTextureDescriptor texDesc = {};
            texDesc.size.width = width;
            texDesc.size.height = height;
            texDesc.size.depthOrArrayLayers = depthOrArrayLayers;
            texDesc.format = format;
            texDesc.usage = usage;
            texDesc.dimension = dimension;
            texDesc.mipLevelCount = mipLevelCount;
            texDesc.sampleCount = sampleCount;

            WGPUTexture texture = wgpuDeviceCreateTexture(g_device, &texDesc);

            if (!texture) {
                g_engine->throwException("Failed to create texture");
                return g_engine->newUndefined();
            }

            // Create JS wrapper
            auto jsTexture = g_engine->newObject();
            g_engine->setPrivateData(jsTexture, texture);

            // Store texture properties
            g_engine->setProperty(jsTexture, "width", g_engine->newNumber(width));
            g_engine->setProperty(jsTexture, "height", g_engine->newNumber(height));
            g_engine->setProperty(jsTexture, "depthOrArrayLayers", g_engine->newNumber(depthOrArrayLayers));
            g_engine->setProperty(jsTexture, "format", g_engine->newString(formatStr.c_str()));
            g_engine->setProperty(jsTexture, "mipLevelCount", g_engine->newNumber(mipLevelCount));
            g_engine->setProperty(jsTexture, "sampleCount", g_engine->newNumber(sampleCount));

            // Register texture for lookup by createView
            uint64_t textureId = g_nextTextureId++;
            g_textureRegistry[textureId] = {
                texture, format, width, height, depthOrArrayLayers, mipLevelCount,
                dimension, true, true
            };

            // Store texture ID for lookup
            g_engine->setProperty(jsTexture, "_textureId", g_engine->newNumber((double)textureId));

            // texture.createView(descriptor?) - Store texture ID for lookup
            // We store the textureId to look up the texture later since callbacks don't have 'this'
            g_engine->setProperty(jsTexture, "_createViewTextureId", g_engine->newNumber((double)textureId));

            g_engine->setProperty(jsTexture, "createView",
                g_engine->newFunction("createView", [textureId](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    // Look up texture from registry using captured textureId
                    auto it = g_textureRegistry.find(textureId);
                    if (it == g_textureRegistry.end()) {
                        std::cerr << "[WebGPU] createView: Texture " << textureId << " not found in registry" << std::endl;
                        return g_engine->newUndefined();
                    }

                    WGPUTexture texture = it->second.texture;
                    if (!texture) {
                        std::cerr << "[WebGPU] createView: Texture " << textureId << " is null" << std::endl;
                        return g_engine->newUndefined();
                    }

                    WGPUTextureViewDescriptor viewDesc = {};
                    // Default values - use all mips and layers from the texture
                    viewDesc.format = it->second.format;
                    viewDesc.mipLevelCount = it->second.mipLevelCount > 0 ? it->second.mipLevelCount : 1;
                    viewDesc.baseMipLevel = 0;
                    viewDesc.baseArrayLayer = 0;
                    viewDesc.aspect = WGPUTextureAspect_All;

                    // Default dimension and arrayLayerCount based on texture dimension
                    if (it->second.dimension == WGPUTextureDimension_3D) {
                        // 3D textures: view as 3D, arrayLayerCount must be 1
                        viewDesc.dimension = WGPUTextureViewDimension_3D;
                        viewDesc.arrayLayerCount = 1;
                    } else if (it->second.dimension == WGPUTextureDimension_1D) {
                        // 1D textures
                        viewDesc.dimension = WGPUTextureViewDimension_1D;
                        viewDesc.arrayLayerCount = 1;
                    } else {
                        // 2D textures: use layers for 2D-array, 1 for regular 2D
                        viewDesc.arrayLayerCount = it->second.depthOrArrayLayers > 0 ? it->second.depthOrArrayLayers : 1;
                        viewDesc.dimension = it->second.depthOrArrayLayers > 1 ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
                    }

                    // Parse view descriptor if provided
                    if (!args.empty() && !g_engine->isUndefined(args[0])) {
                        auto descriptor = args[0];

                        // format (optional, defaults to texture format)
                        auto formatProp = g_engine->getProperty(descriptor, "format");
                        if (!g_engine->isUndefined(formatProp)) {
                            viewDesc.format = stringToFormat(g_engine->toString(formatProp));
                        } else {
                            viewDesc.format = it->second.format;
                        }

                        // dimension (optional)
                        auto dimensionProp = g_engine->getProperty(descriptor, "dimension");
                        if (!g_engine->isUndefined(dimensionProp)) {
                            std::string dimStr = g_engine->toString(dimensionProp);
                            if (dimStr == "1d") viewDesc.dimension = WGPUTextureViewDimension_1D;
                            else if (dimStr == "2d") viewDesc.dimension = WGPUTextureViewDimension_2D;
                            else if (dimStr == "2d-array") viewDesc.dimension = WGPUTextureViewDimension_2DArray;
                            else if (dimStr == "cube") viewDesc.dimension = WGPUTextureViewDimension_Cube;
                            else if (dimStr == "cube-array") viewDesc.dimension = WGPUTextureViewDimension_CubeArray;
                            else if (dimStr == "3d") viewDesc.dimension = WGPUTextureViewDimension_3D;
                        }

                        // aspect (optional)
                        auto aspectProp = g_engine->getProperty(descriptor, "aspect");
                        if (!g_engine->isUndefined(aspectProp)) {
                            std::string aspectStr = g_engine->toString(aspectProp);
                            if (aspectStr == "all") viewDesc.aspect = WGPUTextureAspect_All;
                            else if (aspectStr == "stencil-only") viewDesc.aspect = WGPUTextureAspect_StencilOnly;
                            else if (aspectStr == "depth-only") viewDesc.aspect = WGPUTextureAspect_DepthOnly;
                        }

                        // baseMipLevel (optional)
                        auto baseMipProp = g_engine->getProperty(descriptor, "baseMipLevel");
                        if (!g_engine->isUndefined(baseMipProp)) {
                            viewDesc.baseMipLevel = (uint32_t)g_engine->toNumber(baseMipProp);
                        }

                        // mipLevelCount (optional)
                        auto mipCountProp = g_engine->getProperty(descriptor, "mipLevelCount");
                        if (!g_engine->isUndefined(mipCountProp)) {
                            viewDesc.mipLevelCount = (uint32_t)g_engine->toNumber(mipCountProp);
                        }

                        // baseArrayLayer (optional)
                        auto baseLayerProp = g_engine->getProperty(descriptor, "baseArrayLayer");
                        if (!g_engine->isUndefined(baseLayerProp)) {
                            viewDesc.baseArrayLayer = (uint32_t)g_engine->toNumber(baseLayerProp);
                        }

                        // arrayLayerCount (optional)
                        auto layerCountProp = g_engine->getProperty(descriptor, "arrayLayerCount");
                        if (!g_engine->isUndefined(layerCountProp)) {
                            uint32_t requested = (uint32_t)g_engine->toNumber(layerCountProp);
                            uint32_t maxLayers = it->second.depthOrArrayLayers > 0 ? it->second.depthOrArrayLayers : 1;
                            // Clamp to actual texture layer count
                            viewDesc.arrayLayerCount = std::min(requested, maxLayers - viewDesc.baseArrayLayer);
                        }
                    }
                    // else: defaults are already set above

                    // Final validation: Fix arrayLayerCount based on view dimension
                    if (viewDesc.dimension == WGPUTextureViewDimension_3D ||
                        viewDesc.dimension == WGPUTextureViewDimension_1D) {
                        // 3D/1D textures have no array layers
                        viewDesc.arrayLayerCount = 1;
                    } else if (viewDesc.dimension == WGPUTextureViewDimension_Cube) {
                        // Cube requires exactly 6 layers (the 6 faces)
                        viewDesc.arrayLayerCount = 6;
                    } else if (viewDesc.dimension == WGPUTextureViewDimension_CubeArray) {
                        // CubeArray must have multiple of 6 layers
                        uint32_t maxLayers = it->second.depthOrArrayLayers > 0 ? it->second.depthOrArrayLayers : 6;
                        viewDesc.arrayLayerCount = std::min(viewDesc.arrayLayerCount, maxLayers);
                        // Round down to nearest multiple of 6
                        viewDesc.arrayLayerCount = (viewDesc.arrayLayerCount / 6) * 6;
                        if (viewDesc.arrayLayerCount < 6) viewDesc.arrayLayerCount = 6;
                    }

                    WGPUTextureView view = wgpuTextureCreateView(texture, &viewDesc);
        
                    if (!view) {
                        std::cerr << "[WebGPU] createView: Failed to create texture view" << std::endl;
                        return g_engine->newUndefined();
                    }

                    auto jsView = g_engine->newObject();
                    g_engine->setPrivateData(jsView, view);
                    g_engine->setProperty(jsView, "_type", g_engine->newString("textureView"));
                    if (gcReleaseEnabled()) {
                        g_engine->registerRelease(jsView, [view]() {
                            wgpuTextureViewRelease(view);
                        });
                    }

                    return jsView;
                })
            );

            // texture.destroy()
            g_engine->setProperty(jsTexture, "destroy",
                g_engine->newFunction("destroy", [textureId](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    g_frameQueue.flushBeforeImmediateOperation();
                    releaseTexture(textureId, true);
                    return g_engine->newUndefined();
                })
            );
            if (gcReleaseEnabled()) {
                g_engine->registerRelease(jsTexture, [textureId]() {
                    releaseTexture(textureId, false);
                });
            }

            if (g_verboseLogging) std::cout << "[WebGPU] Created texture " << width << "x" << height << " format=" << formatStr << " (id=" << textureId << ")" << std::endl;
            return jsTexture;
        })
    );

    // device.createSampler(descriptor?)
    g_engine->setProperty(device, "createSampler",
        g_engine->newFunction("createSampler", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            WGPUSamplerDescriptor samplerDesc = {};

            // Default values
            samplerDesc.addressModeU = WGPUAddressMode_ClampToEdge;
            samplerDesc.addressModeV = WGPUAddressMode_ClampToEdge;
            samplerDesc.addressModeW = WGPUAddressMode_ClampToEdge;
            samplerDesc.magFilter = WGPUFilterMode_Nearest;
            samplerDesc.minFilter = WGPUFilterMode_Nearest;
            samplerDesc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
            samplerDesc.lodMinClamp = 0.0f;
            samplerDesc.lodMaxClamp = 32.0f;
            samplerDesc.maxAnisotropy = 1;

            if (!args.empty()) {
                auto descriptor = args[0];

                auto addressModeU = g_engine->getProperty(descriptor, "addressModeU");
                if (!g_engine->isUndefined(addressModeU)) {
                    samplerDesc.addressModeU = stringToAddressMode(g_engine->toString(addressModeU));
                }

                auto addressModeV = g_engine->getProperty(descriptor, "addressModeV");
                if (!g_engine->isUndefined(addressModeV)) {
                    samplerDesc.addressModeV = stringToAddressMode(g_engine->toString(addressModeV));
                }

                auto addressModeW = g_engine->getProperty(descriptor, "addressModeW");
                if (!g_engine->isUndefined(addressModeW)) {
                    samplerDesc.addressModeW = stringToAddressMode(g_engine->toString(addressModeW));
                }

                auto magFilter = g_engine->getProperty(descriptor, "magFilter");
                if (!g_engine->isUndefined(magFilter)) {
                    samplerDesc.magFilter = stringToFilterMode(g_engine->toString(magFilter));
                }

                auto minFilter = g_engine->getProperty(descriptor, "minFilter");
                if (!g_engine->isUndefined(minFilter)) {
                    samplerDesc.minFilter = stringToFilterMode(g_engine->toString(minFilter));
                }

                auto mipmapFilter = g_engine->getProperty(descriptor, "mipmapFilter");
                if (!g_engine->isUndefined(mipmapFilter)) {
                    samplerDesc.mipmapFilter = stringToMipmapFilterMode(g_engine->toString(mipmapFilter));
                }

                auto lodMinClamp = g_engine->getProperty(descriptor, "lodMinClamp");
                if (!g_engine->isUndefined(lodMinClamp)) {
                    samplerDesc.lodMinClamp = (float)g_engine->toNumber(lodMinClamp);
                }

                auto lodMaxClamp = g_engine->getProperty(descriptor, "lodMaxClamp");
                if (!g_engine->isUndefined(lodMaxClamp)) {
                    samplerDesc.lodMaxClamp = (float)g_engine->toNumber(lodMaxClamp);
                }

                auto compare = g_engine->getProperty(descriptor, "compare");
                if (!g_engine->isUndefined(compare)) {
                    samplerDesc.compare = stringToCompareFunction(g_engine->toString(compare));
                }

                auto maxAnisotropy = g_engine->getProperty(descriptor, "maxAnisotropy");
                if (!g_engine->isUndefined(maxAnisotropy)) {
                    samplerDesc.maxAnisotropy = (uint16_t)g_engine->toNumber(maxAnisotropy);
                }
            }

            WGPUSampler sampler = wgpuDeviceCreateSampler(g_device, &samplerDesc);

            auto jsSampler = g_engine->newObject();
            g_engine->setPrivateData(jsSampler, sampler);
            g_engine->setProperty(jsSampler, "_type", g_engine->newString("sampler"));
            if (gcReleaseEnabled()) {
                g_engine->registerRelease(jsSampler, [sampler]() {
                    wgpuSamplerRelease(sampler);
                });
            }

            if (g_verboseLogging) std::cout << "[WebGPU] Created sampler" << std::endl;
            return jsSampler;
        })
    );

    // device.createBindGroupLayout(descriptor)
    g_engine->setProperty(device, "createBindGroupLayout",
        g_engine->newFunction("createBindGroupLayout", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                g_engine->throwException("createBindGroupLayout requires a descriptor");
                return g_engine->newUndefined();
            }

            auto descriptor = args[0];
            auto entries = g_engine->getProperty(descriptor, "entries");
            auto lengthProp = g_engine->getProperty(entries, "length");
            int entryCount = g_engine->isUndefined(lengthProp) ? 0 : (int)g_engine->toNumber(lengthProp);

            std::vector<WGPUBindGroupLayoutEntry> layoutEntries;
            layoutEntries.reserve(entryCount);

            for (int i = 0; i < entryCount; i++) {
                auto entry = g_engine->getPropertyIndex(entries, i);

                WGPUBindGroupLayoutEntry layoutEntry = {};
                layoutEntry.binding = (uint32_t)g_engine->toNumber(g_engine->getProperty(entry, "binding"));
                layoutEntry.visibility = (WGPUShaderStage)(uint32_t)g_engine->toNumber(g_engine->getProperty(entry, "visibility"));

                // Check for buffer binding
                auto buffer = g_engine->getProperty(entry, "buffer");
                if (!g_engine->isUndefined(buffer)) {
                    auto typeProp = g_engine->getProperty(buffer, "type");
                    std::string typeStr = g_engine->isUndefined(typeProp) ? "" : g_engine->toString(typeProp);
                    if (typeStr == "uniform" || typeStr == "") {
                        // Default to uniform if no type specified (Three.js uses empty {})
                        layoutEntry.buffer.type = WGPUBufferBindingType_Uniform;
                    } else if (typeStr == "storage") {
                        layoutEntry.buffer.type = WGPUBufferBindingType_Storage;
                    } else if (typeStr == "read-only-storage") {
                        layoutEntry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
                    } else {
                        // Default to uniform for unknown types
                        layoutEntry.buffer.type = WGPUBufferBindingType_Uniform;
                    }
                }

                // Check for sampler binding
                auto sampler = g_engine->getProperty(entry, "sampler");
                if (!g_engine->isUndefined(sampler)) {
                    std::string typeStr = g_engine->toString(g_engine->getProperty(sampler, "type"));
                    if (typeStr == "filtering") {
                        layoutEntry.sampler.type = WGPUSamplerBindingType_Filtering;
                    } else if (typeStr == "non-filtering") {
                        layoutEntry.sampler.type = WGPUSamplerBindingType_NonFiltering;
                    } else if (typeStr == "comparison") {
                        layoutEntry.sampler.type = WGPUSamplerBindingType_Comparison;
                    } else {
                        // Default to filtering
                        layoutEntry.sampler.type = WGPUSamplerBindingType_Filtering;
                    }
                }

                // Check for texture binding
                auto texture = g_engine->getProperty(entry, "texture");
                if (!g_engine->isUndefined(texture)) {
                    auto sampleTypeProp = g_engine->getProperty(texture, "sampleType");
                    std::string sampleType = g_engine->isUndefined(sampleTypeProp) ? "" : g_engine->toString(sampleTypeProp);
                    if (sampleType == "float" || sampleType == "") {
                        // Default to float if no type specified (Three.js uses empty {})
                        layoutEntry.texture.sampleType = WGPUTextureSampleType_Float;
                    } else if (sampleType == "unfilterable-float") {
                        layoutEntry.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
                    } else if (sampleType == "depth") {
                        layoutEntry.texture.sampleType = WGPUTextureSampleType_Depth;
                    } else if (sampleType == "sint") {
                        layoutEntry.texture.sampleType = WGPUTextureSampleType_Sint;
                    } else if (sampleType == "uint") {
                        layoutEntry.texture.sampleType = WGPUTextureSampleType_Uint;
                    } else {
                        // Default to float for unknown types
                        layoutEntry.texture.sampleType = WGPUTextureSampleType_Float;
                    }

                    auto viewDim = g_engine->getProperty(texture, "viewDimension");
                    if (!g_engine->isUndefined(viewDim)) {
                        layoutEntry.texture.viewDimension = stringToTextureViewDimension(g_engine->toString(viewDim));
                    } else {
                        layoutEntry.texture.viewDimension = WGPUTextureViewDimension_2D;
                    }

                    auto multisampled = g_engine->getProperty(texture, "multisampled");
                    layoutEntry.texture.multisampled = !g_engine->isUndefined(multisampled) && g_engine->toBoolean(multisampled);
                }

                // Check for storageTexture binding
                auto storageTexture = g_engine->getProperty(entry, "storageTexture");
                if (!g_engine->isUndefined(storageTexture)) {
                    std::string access = g_engine->toString(g_engine->getProperty(storageTexture, "access"));
                    if (access == "write-only") {
                        layoutEntry.storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
                    } else if (access == "read-only") {
                        layoutEntry.storageTexture.access = WGPUStorageTextureAccess_ReadOnly;
                    } else if (access == "read-write") {
                        layoutEntry.storageTexture.access = WGPUStorageTextureAccess_ReadWrite;
                    }

                    auto format = g_engine->getProperty(storageTexture, "format");
                    if (!g_engine->isUndefined(format)) {
                        layoutEntry.storageTexture.format = stringToFormat(g_engine->toString(format));
                    }

                    auto viewDim = g_engine->getProperty(storageTexture, "viewDimension");
                    if (!g_engine->isUndefined(viewDim)) {
                        layoutEntry.storageTexture.viewDimension = stringToTextureViewDimension(g_engine->toString(viewDim));
                    } else {
                        layoutEntry.storageTexture.viewDimension = WGPUTextureViewDimension_2D;
                    }
                }

                layoutEntries.push_back(layoutEntry);
            }

            WGPUBindGroupLayoutDescriptor layoutDesc = {};
            layoutDesc.entryCount = layoutEntries.size();
            layoutDesc.entries = layoutEntries.data();

            WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(g_device, &layoutDesc);

            auto jsLayout = g_engine->newObject();
            g_engine->setPrivateData(jsLayout, layout);
            if (gcReleaseEnabled()) {
                g_engine->registerRelease(jsLayout, [layout]() {
                    wgpuBindGroupLayoutRelease(layout);
                });
            }

            if (g_verboseLogging) std::cout << "[WebGPU] Created bind group layout with " << entryCount << " entries" << std::endl;
            return jsLayout;
        })
    );

    // device.createBindGroup(descriptor)
    g_engine->setProperty(device, "createBindGroup",
        g_engine->newFunction("createBindGroup", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                g_engine->throwException("createBindGroup requires a descriptor");
                return g_engine->newUndefined();
            }

            auto descriptor = args[0];
            auto layoutHandle = g_engine->getProperty(descriptor, "layout");
            WGPUBindGroupLayout layout = (WGPUBindGroupLayout)g_engine->getPrivateData(layoutHandle);

            auto entries = g_engine->getProperty(descriptor, "entries");
            auto lengthProp = g_engine->getProperty(entries, "length");
            int entryCount = g_engine->isUndefined(lengthProp) ? 0 : (int)g_engine->toNumber(lengthProp);

            std::vector<WGPUBindGroupEntry> bindGroupEntries;
            bindGroupEntries.reserve(entryCount);
            std::vector<WGPUTextureView> autoCreatedViews;

            for (int i = 0; i < entryCount; i++) {
                auto entry = g_engine->getPropertyIndex(entries, i);

                WGPUBindGroupEntry bgEntry = {};
                bgEntry.binding = (uint32_t)g_engine->toNumber(g_engine->getProperty(entry, "binding"));

                auto resource = g_engine->getProperty(entry, "resource");

                // Check if resource is a sampler (has no buffer property)
                auto bufferProp = g_engine->getProperty(resource, "buffer");
                if (!g_engine->isUndefined(bufferProp)) {
                    // Buffer binding: {buffer, offset?, size?}
                    bgEntry.buffer = (WGPUBuffer)g_engine->getPrivateData(bufferProp);

                    auto offset = g_engine->getProperty(resource, "offset");
                    bgEntry.offset = g_engine->isUndefined(offset) ? 0 : (uint64_t)g_engine->toNumber(offset);

                    auto size = g_engine->getProperty(resource, "size");
                    // Size 0 means whole buffer
                    bgEntry.size = g_engine->isUndefined(size) ? WGPU_WHOLE_SIZE : (uint64_t)g_engine->toNumber(size);
                } else {
                    // Could be a sampler or texture view
                    void* resourcePtr = g_engine->getPrivateData(resource);

                    // Check for type hints set when creating the object
                    auto typeHint = g_engine->getProperty(resource, "_type");
                    if (!g_engine->isUndefined(typeHint)) {
                        std::string typeStr = g_engine->toString(typeHint);
                        if (typeStr == "sampler") {
                            if (resourcePtr) {
                                bgEntry.sampler = (WGPUSampler)resourcePtr;
                            } else {
                                std::cerr << "[WebGPU] Warning: Sampler at binding " << bgEntry.binding << " is null" << std::endl;
                            }
                        } else if (typeStr == "textureView") {
                            if (resourcePtr) {
                                bgEntry.textureView = (WGPUTextureView)resourcePtr;
                            } else {
                                std::cerr << "[WebGPU] Warning: TextureView at binding " << bgEntry.binding << " is null, creating placeholder" << std::endl;
                                // Create a 1x1 placeholder texture view to avoid validation errors
                                // This is a workaround for textures that failed to create
                            }
                        }
                    } else if (resourcePtr) {
                        // No type hint - try to detect based on properties
                        // Check if it looks like a texture (has width/height/format properties)
                        auto widthProp = g_engine->getProperty(resource, "width");
                        auto formatProp = g_engine->getProperty(resource, "format");
                        if (!g_engine->isUndefined(widthProp) && !g_engine->isUndefined(formatProp)) {
                            // This is a texture, create a view automatically
                            WGPUTexture tex = (WGPUTexture)resourcePtr;
                            WGPUTextureViewDescriptor viewDesc = {};
                            WGPUTextureView view = wgpuTextureCreateView(tex, &viewDesc);

                            autoCreatedViews.push_back(view);
                            bgEntry.textureView = view;
                            if (g_verboseLogging) std::cout << "[WebGPU] Auto-created texture view for binding " << bgEntry.binding << std::endl;
                        } else {
                            // Assume sampler as fallback
                            bgEntry.sampler = (WGPUSampler)resourcePtr;
                        }
                    } else {
                        std::cerr << "[WebGPU] Warning: Resource at binding " << bgEntry.binding << " has null privateData" << std::endl;
                    }
                }

                bindGroupEntries.push_back(bgEntry);
            }

            WGPUBindGroupDescriptor bgDesc = {};
            bgDesc.layout = layout;
            bgDesc.entryCount = bindGroupEntries.size();
            bgDesc.entries = bindGroupEntries.data();

            WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(g_device, &bgDesc);

            // Release auto-created texture views - Dawn holds its own
            // internal references through the bind group
            for (auto v : autoCreatedViews) {
                wgpuTextureViewRelease(v);
            }

            auto jsBindGroup = g_engine->newObject();
            g_engine->setPrivateData(jsBindGroup, bindGroup);
            if (gcReleaseEnabled()) {
                g_engine->registerRelease(jsBindGroup, [bindGroup]() {
                    wgpuBindGroupRelease(bindGroup);
                });
            }

            if (g_verboseLogging) std::cout << "[WebGPU] Created bind group with " << entryCount << " entries" << std::endl;
            return jsBindGroup;
        })
    );

    // device.createPipelineLayout(descriptor)
    g_engine->setProperty(device, "createPipelineLayout",
        g_engine->newFunction("createPipelineLayout", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                g_engine->throwException("createPipelineLayout requires a descriptor");
                return g_engine->newUndefined();
            }

            auto descriptor = args[0];
            auto bindGroupLayouts = g_engine->getProperty(descriptor, "bindGroupLayouts");
            auto lengthProp = g_engine->getProperty(bindGroupLayouts, "length");
            int layoutCount = g_engine->isUndefined(lengthProp) ? 0 : (int)g_engine->toNumber(lengthProp);

            std::vector<WGPUBindGroupLayout> layouts;
            layouts.reserve(layoutCount);

            for (int i = 0; i < layoutCount; i++) {
                auto layoutHandle = g_engine->getPropertyIndex(bindGroupLayouts, i);
                WGPUBindGroupLayout layout = (WGPUBindGroupLayout)g_engine->getPrivateData(layoutHandle);
                layouts.push_back(layout);
            }

            WGPUPipelineLayoutDescriptor layoutDesc = {};
            layoutDesc.bindGroupLayoutCount = layouts.size();
            layoutDesc.bindGroupLayouts = layouts.data();

            WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(g_device, &layoutDesc);

            auto jsLayout = g_engine->newObject();
            g_engine->setPrivateData(jsLayout, pipelineLayout);
            if (gcReleaseEnabled()) {
                g_engine->registerRelease(jsLayout, [pipelineLayout]() {
                    wgpuPipelineLayoutRelease(pipelineLayout);
                });
            }

            if (g_verboseLogging) std::cout << "[WebGPU] Created pipeline layout with " << layoutCount << " bind group layouts" << std::endl;
            return jsLayout;
        })
    );

    // device.createTextureView(texture, descriptor?) - Non-standard helper
    // Workaround because texture.createView() can't easily access 'this'
    g_engine->setProperty(device, "createTextureView",
        g_engine->newFunction("createTextureView", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                g_engine->throwException("createTextureView requires a texture");
                return g_engine->newUndefined();
            }

            auto textureHandle = args[0];
            WGPUTexture texture = (WGPUTexture)g_engine->getPrivateData(textureHandle);

            if (!texture) {
                g_engine->throwException("createTextureView: invalid texture");
                return g_engine->newUndefined();
            }

            // Get texture info
            double formatEnum = g_engine->toNumber(g_engine->getProperty(textureHandle, "_formatEnum"));
            WGPUTextureFormat format = formatEnum == 0 ? g_surfaceFormat : (WGPUTextureFormat)(int)formatEnum;

            // Get format from _textureId if available
            auto textureIdVal = g_engine->getProperty(textureHandle, "_textureId");
            if (!g_engine->isUndefined(textureIdVal)) {
                uint64_t textureId = (uint64_t)g_engine->toNumber(textureIdVal);
                auto it = g_textureRegistry.find(textureId);
                if (it != g_textureRegistry.end()) {
                    format = it->second.format;
                }
            }

            WGPUTextureViewDescriptor viewDesc = {};
            viewDesc.format = format;
            viewDesc.dimension = WGPUTextureViewDimension_2D;
            viewDesc.baseMipLevel = 0;
            viewDesc.mipLevelCount = 1;
            viewDesc.baseArrayLayer = 0;
            viewDesc.arrayLayerCount = 1;
            viewDesc.aspect = WGPUTextureAspect_All;

            // Parse descriptor if provided
            if (args.size() > 1 && !g_engine->isUndefined(args[1])) {
                auto descriptor = args[1];

                auto formatProp = g_engine->getProperty(descriptor, "format");
                if (!g_engine->isUndefined(formatProp)) {
                    viewDesc.format = stringToFormat(g_engine->toString(formatProp));
                }

                auto dimensionProp = g_engine->getProperty(descriptor, "dimension");
                if (!g_engine->isUndefined(dimensionProp)) {
                    viewDesc.dimension = stringToTextureViewDimension(g_engine->toString(dimensionProp));
                }

                auto baseMipLevel = g_engine->getProperty(descriptor, "baseMipLevel");
                if (!g_engine->isUndefined(baseMipLevel)) {
                    viewDesc.baseMipLevel = (uint32_t)g_engine->toNumber(baseMipLevel);
                }

                auto mipLevelCount = g_engine->getProperty(descriptor, "mipLevelCount");
                if (!g_engine->isUndefined(mipLevelCount)) {
                    viewDesc.mipLevelCount = (uint32_t)g_engine->toNumber(mipLevelCount);
                }

                auto baseArrayLayer = g_engine->getProperty(descriptor, "baseArrayLayer");
                if (!g_engine->isUndefined(baseArrayLayer)) {
                    viewDesc.baseArrayLayer = (uint32_t)g_engine->toNumber(baseArrayLayer);
                }

                auto arrayLayerCount = g_engine->getProperty(descriptor, "arrayLayerCount");
                if (!g_engine->isUndefined(arrayLayerCount)) {
                    uint32_t requested = (uint32_t)g_engine->toNumber(arrayLayerCount);
                    // Clamp to 1 for surface textures (which only have 1 layer)
                    // or look up actual layer count from registry
                    auto textureIdVal2 = g_engine->getProperty(textureHandle, "_textureId");
                    uint32_t maxLayers = 1;
                    if (!g_engine->isUndefined(textureIdVal2)) {
                        uint64_t tid = (uint64_t)g_engine->toNumber(textureIdVal2);
                        auto it = g_textureRegistry.find(tid);
                        if (it != g_textureRegistry.end()) {
                            maxLayers = it->second.depthOrArrayLayers > 0 ? it->second.depthOrArrayLayers : 1;
                        }
                    }
                    viewDesc.arrayLayerCount = std::min(requested, maxLayers - viewDesc.baseArrayLayer);
                }

                auto aspect = g_engine->getProperty(descriptor, "aspect");
                if (!g_engine->isUndefined(aspect)) {
                    std::string aspectStr = g_engine->toString(aspect);
                    if (aspectStr == "all") viewDesc.aspect = WGPUTextureAspect_All;
                    else if (aspectStr == "stencil-only") viewDesc.aspect = WGPUTextureAspect_StencilOnly;
                    else if (aspectStr == "depth-only") viewDesc.aspect = WGPUTextureAspect_DepthOnly;
                }
            }

            // Final validation: Fix arrayLayerCount based on view dimension
            if (viewDesc.dimension == WGPUTextureViewDimension_3D ||
                viewDesc.dimension == WGPUTextureViewDimension_1D) {
                viewDesc.arrayLayerCount = 1;
            } else if (viewDesc.dimension == WGPUTextureViewDimension_Cube) {
                viewDesc.arrayLayerCount = 6;
            }

            WGPUTextureView view = wgpuTextureCreateView(texture, &viewDesc);

            auto jsView = g_engine->newObject();
            g_engine->setPrivateData(jsView, view);
            g_engine->setProperty(jsView, "_type", g_engine->newString("textureView"));
            if (gcReleaseEnabled()) {
                g_engine->registerRelease(jsView, [view]() {
                    wgpuTextureViewRelease(view);
                });
            }

            if (g_verboseLogging) std::cout << "[WebGPU] Created texture view" << std::endl;
            return jsView;
        })
    );

    // device.createRenderBundleEncoder(descriptor)
    // Used by Three.js for mipmap generation
    g_engine->setProperty(device, "createRenderBundleEncoder",
        g_engine->newFunction("createRenderBundleEncoder", [](void* ctx, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) {
                g_engine->throwException("createRenderBundleEncoder requires a descriptor");
                return g_engine->newUndefined();
            }

            auto descriptor = args[0];

            // Parse color formats
            auto colorFormats = g_engine->getProperty(descriptor, "colorFormats");
            auto colorFormatsLength = g_engine->getProperty(colorFormats, "length");
            int colorFormatCount = g_engine->isUndefined(colorFormatsLength) ? 0 : (int)g_engine->toNumber(colorFormatsLength);

            std::vector<WGPUTextureFormat> formats;
            formats.reserve(colorFormatCount);
            for (int i = 0; i < colorFormatCount; i++) {
                auto formatProp = g_engine->getPropertyIndex(colorFormats, i);
                if (!g_engine->isUndefined(formatProp) && !g_engine->isNull(formatProp)) {
                    formats.push_back(stringToFormat(g_engine->toString(formatProp)));
                }
            }

            // Parse depth stencil format
            WGPUTextureFormat depthFormat = WGPUTextureFormat_Undefined;
            auto depthFormatProp = g_engine->getProperty(descriptor, "depthStencilFormat");
            if (!g_engine->isUndefined(depthFormatProp) && !g_engine->isNull(depthFormatProp)) {
                depthFormat = stringToFormat(g_engine->toString(depthFormatProp));
            }

            // Parse sample count
            uint32_t sampleCount = 1;
            auto sampleCountProp = g_engine->getProperty(descriptor, "sampleCount");
            if (!g_engine->isUndefined(sampleCountProp)) {
                sampleCount = (uint32_t)g_engine->toNumber(sampleCountProp);
            }

            WGPURenderBundleEncoderDescriptor desc = {};
            desc.colorFormatCount = formats.size();
            desc.colorFormats = formats.data();
            desc.depthStencilFormat = depthFormat;
            desc.sampleCount = sampleCount;

            WGPURenderBundleEncoder bundleEncoder = wgpuDeviceCreateRenderBundleEncoder(g_device, &desc);

            if (!bundleEncoder) {
                g_engine->throwException("Failed to create render bundle encoder");
                return g_engine->newUndefined();
            }

            auto jsEncoder = g_engine->newObject();
            g_engine->setPrivateData(jsEncoder, bundleEncoder);

            // Capture for closures
            WGPURenderBundleEncoder capturedEncoder = bundleEncoder;
            auto bundleEncoderAlive = std::make_shared<bool>(true);
            if (gcReleaseEnabled()) {
                g_engine->registerRelease(jsEncoder, [capturedEncoder, bundleEncoderAlive]() {
                    if (!*bundleEncoderAlive) return;
                    *bundleEncoderAlive = false;
                    wgpuRenderBundleEncoderRelease(capturedEncoder);
                });
            }

            // renderBundleEncoder.setPipeline(pipeline)
            g_engine->setProperty(jsEncoder, "setPipeline",
                g_engine->newFunction("setPipeline", [capturedEncoder](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    if (args.empty()) return g_engine->newUndefined();
                    WGPURenderPipeline pipeline = (WGPURenderPipeline)g_engine->getPrivateData(args[0]);
                    wgpuRenderBundleEncoderSetPipeline(capturedEncoder, pipeline);
                    return g_engine->newUndefined();
                })
            );

            // renderBundleEncoder.setVertexBuffer(slot, buffer, offset?, size?)
            g_engine->setProperty(jsEncoder, "setVertexBuffer",
                g_engine->newFunction("setVertexBuffer", [capturedEncoder](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    if (args.size() < 2) return g_engine->newUndefined();
                    uint32_t slot = (uint32_t)g_engine->toNumber(args[0]);
                    WGPUBuffer buffer = (WGPUBuffer)g_engine->getPrivateData(args[1]);
                    uint64_t offset = args.size() > 2 && !g_engine->isUndefined(args[2]) ? (uint64_t)g_engine->toNumber(args[2]) : 0;
                    uint64_t size = args.size() > 3 && !g_engine->isUndefined(args[3]) ? (uint64_t)g_engine->toNumber(args[3]) : WGPU_WHOLE_SIZE;
                    wgpuRenderBundleEncoderSetVertexBuffer(capturedEncoder, slot, buffer, offset, size);
                    return g_engine->newUndefined();
                })
            );

            // renderBundleEncoder.setIndexBuffer(buffer, format, offset?, size?)
            g_engine->setProperty(jsEncoder, "setIndexBuffer",
                g_engine->newFunction("setIndexBuffer", [capturedEncoder](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    if (args.size() < 2) return g_engine->newUndefined();
                    WGPUBuffer buffer = (WGPUBuffer)g_engine->getPrivateData(args[0]);
                    std::string formatStr = g_engine->toString(args[1]);
                    WGPUIndexFormat format = formatStr == "uint32" ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16;
                    uint64_t offset = args.size() > 2 && !g_engine->isUndefined(args[2]) ? (uint64_t)g_engine->toNumber(args[2]) : 0;
                    uint64_t size = args.size() > 3 && !g_engine->isUndefined(args[3]) ? (uint64_t)g_engine->toNumber(args[3]) : WGPU_WHOLE_SIZE;
                    wgpuRenderBundleEncoderSetIndexBuffer(capturedEncoder, buffer, format, offset, size);
                    return g_engine->newUndefined();
                })
            );

            // renderBundleEncoder.setBindGroup(index, bindGroup, dynamicOffsets?)
            g_engine->setProperty(jsEncoder, "setBindGroup",
                g_engine->newFunction("setBindGroup", [capturedEncoder](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    if (args.size() < 2) return g_engine->newUndefined();
                    uint32_t index = (uint32_t)g_engine->toNumber(args[0]);
                    WGPUBindGroup bindGroup = (WGPUBindGroup)g_engine->getPrivateData(args[1]);

                    std::vector<uint32_t> dynamicOffsets;
                    if (!parseDynamicOffsets(args, dynamicOffsets, "renderBundleEncoder.setBindGroup")) {
                        return g_engine->newUndefined();
                    }

                    wgpuRenderBundleEncoderSetBindGroup(capturedEncoder, index, bindGroup, dynamicOffsets.size(), dynamicOffsets.data());
                    return g_engine->newUndefined();
                })
            );

            // renderBundleEncoder.draw(vertexCount, instanceCount?, firstVertex?, firstInstance?)
            g_engine->setProperty(jsEncoder, "draw",
                g_engine->newFunction("draw", [capturedEncoder](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    if (args.empty()) return g_engine->newUndefined();
                    uint32_t vertexCount = (uint32_t)g_engine->toNumber(args[0]);
                    uint32_t instanceCount = args.size() > 1 ? (uint32_t)g_engine->toNumber(args[1]) : 1;
                    uint32_t firstVertex = args.size() > 2 ? (uint32_t)g_engine->toNumber(args[2]) : 0;
                    uint32_t firstInstance = args.size() > 3 ? (uint32_t)g_engine->toNumber(args[3]) : 0;
                    wgpuRenderBundleEncoderDraw(capturedEncoder, vertexCount, instanceCount, firstVertex, firstInstance);
                    return g_engine->newUndefined();
                })
            );

            // renderBundleEncoder.drawIndexed(indexCount, instanceCount?, firstIndex?, baseVertex?, firstInstance?)
            g_engine->setProperty(jsEncoder, "drawIndexed",
                g_engine->newFunction("drawIndexed", [capturedEncoder](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    if (args.empty()) return g_engine->newUndefined();
                    uint32_t indexCount = (uint32_t)g_engine->toNumber(args[0]);
                    uint32_t instanceCount = args.size() > 1 ? (uint32_t)g_engine->toNumber(args[1]) : 1;
                    uint32_t firstIndex = args.size() > 2 ? (uint32_t)g_engine->toNumber(args[2]) : 0;
                    int32_t baseVertex = args.size() > 3 ? (int32_t)g_engine->toNumber(args[3]) : 0;
                    uint32_t firstInstance = args.size() > 4 ? (uint32_t)g_engine->toNumber(args[4]) : 0;
                    wgpuRenderBundleEncoderDrawIndexed(capturedEncoder, indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
                    return g_engine->newUndefined();
                })
            );

            // renderBundleEncoder.finish(descriptor?)
            g_engine->setProperty(jsEncoder, "finish",
                g_engine->newFunction("finish", [capturedEncoder, bundleEncoderAlive](void* ctx, const std::vector<js::JSValueHandle>& args) {
                    if (!*bundleEncoderAlive) {
                        g_engine->throwException("Render bundle encoder is already finished");
                        return g_engine->newUndefined();
                    }
                    WGPURenderBundleDescriptor desc = {};
                    WGPURenderBundle bundle = wgpuRenderBundleEncoderFinish(capturedEncoder, &desc);
                    *bundleEncoderAlive = false;
                    wgpuRenderBundleEncoderRelease(capturedEncoder);

                    auto jsBundle = g_engine->newObject();
                    g_engine->setPrivateData(jsBundle, bundle);
                    g_engine->setProperty(jsBundle, "_type", g_engine->newString("renderBundle"));
                    if (bundle && gcReleaseEnabled()) {
                        g_engine->registerRelease(jsBundle, [bundle]() {
                            wgpuRenderBundleRelease(bundle);
                        });
                    }

                    if (g_verboseLogging) std::cout << "[WebGPU] Render bundle finished" << std::endl;
                    return jsBundle;
                })
            );

            if (g_verboseLogging) std::cout << "[WebGPU] Created render bundle encoder" << std::endl;
            return jsEncoder;
        })
    );
}

} // namespace mystral::webgpu
