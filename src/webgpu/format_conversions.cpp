#include "webgpu/format_conversions.h"

#include <iostream>

namespace mystral::webgpu::bridge {

const char* formatToString(WGPUTextureFormat format) {
    switch (format) {
        case WGPUTextureFormat_BGRA8Unorm: return "bgra8unorm";
        case WGPUTextureFormat_BGRA8UnormSrgb: return "bgra8unorm-srgb";
        case WGPUTextureFormat_RGBA8Unorm: return "rgba8unorm";
        case WGPUTextureFormat_RGBA8UnormSrgb: return "rgba8unorm-srgb";
        case WGPUTextureFormat_R8Unorm: return "r8unorm";
        case WGPUTextureFormat_RG8Unorm: return "rg8unorm";
        case WGPUTextureFormat_R16Float: return "r16float";
        case WGPUTextureFormat_RG16Float: return "rg16float";
        case WGPUTextureFormat_R32Float: return "r32float";
        case WGPUTextureFormat_RG32Float: return "rg32float";
        case WGPUTextureFormat_RGBA16Float: return "rgba16float";
        case WGPUTextureFormat_RGBA32Float: return "rgba32float";
        case WGPUTextureFormat_Depth24Plus: return "depth24plus";
        case WGPUTextureFormat_Depth24PlusStencil8: return "depth24plus-stencil8";
        case WGPUTextureFormat_Depth32Float: return "depth32float";
        default: return "bgra8unorm";
    }
}

WGPUTextureFormat stringToFormat(const std::string& format) {
    if (format == "bgra8unorm") return WGPUTextureFormat_BGRA8Unorm;
    if (format == "bgra8unorm-srgb") return WGPUTextureFormat_BGRA8UnormSrgb;
    if (format == "rgba8unorm") return WGPUTextureFormat_RGBA8Unorm;
    if (format == "rgba8unorm-srgb") return WGPUTextureFormat_RGBA8UnormSrgb;
    if (format == "r8unorm") return WGPUTextureFormat_R8Unorm;
    if (format == "rg8unorm") return WGPUTextureFormat_RG8Unorm;
    if (format == "r16float") return WGPUTextureFormat_R16Float;
    if (format == "rg16float") return WGPUTextureFormat_RG16Float;
    if (format == "r32float") return WGPUTextureFormat_R32Float;
    if (format == "rg32float") return WGPUTextureFormat_RG32Float;
    if (format == "rgba16float") return WGPUTextureFormat_RGBA16Float;
    if (format == "rgba32float") return WGPUTextureFormat_RGBA32Float;
    if (format == "depth24plus") return WGPUTextureFormat_Depth24Plus;
    if (format == "depth24plus-stencil8") return WGPUTextureFormat_Depth24PlusStencil8;
    if (format == "depth32float") return WGPUTextureFormat_Depth32Float;
    if (!format.empty()) {
        std::cerr << "[WebGPU] Warning: Unrecognized format '" << format
                  << "', defaulting to BGRA8Unorm" << std::endl;
    }
    return WGPUTextureFormat_BGRA8Unorm;
}

WGPUTextureDimension stringToTextureDimension(const std::string& dimension) {
    if (dimension == "1d") return WGPUTextureDimension_1D;
    if (dimension == "2d") return WGPUTextureDimension_2D;
    if (dimension == "3d") return WGPUTextureDimension_3D;
    return WGPUTextureDimension_2D;
}

WGPUTextureViewDimension stringToTextureViewDimension(const std::string& dimension) {
    if (dimension == "1d") return WGPUTextureViewDimension_1D;
    if (dimension == "2d") return WGPUTextureViewDimension_2D;
    if (dimension == "2d-array") return WGPUTextureViewDimension_2DArray;
    if (dimension == "cube") return WGPUTextureViewDimension_Cube;
    if (dimension == "cube-array") return WGPUTextureViewDimension_CubeArray;
    if (dimension == "3d") return WGPUTextureViewDimension_3D;
    return WGPUTextureViewDimension_2D;
}

WGPUAddressMode stringToAddressMode(const std::string& mode) {
    if (mode == "clamp-to-edge") return WGPUAddressMode_ClampToEdge;
    if (mode == "repeat") return WGPUAddressMode_Repeat;
    if (mode == "mirror-repeat") return WGPUAddressMode_MirrorRepeat;
    return WGPUAddressMode_ClampToEdge;
}

WGPUFilterMode stringToFilterMode(const std::string& mode) {
    if (mode == "nearest") return WGPUFilterMode_Nearest;
    if (mode == "linear") return WGPUFilterMode_Linear;
    return WGPUFilterMode_Nearest;
}

WGPUMipmapFilterMode stringToMipmapFilterMode(const std::string& mode) {
    if (mode == "nearest") return WGPUMipmapFilterMode_Nearest;
    if (mode == "linear") return WGPUMipmapFilterMode_Linear;
    return WGPUMipmapFilterMode_Nearest;
}

WGPUCompareFunction stringToCompareFunction(const std::string& function) {
    if (function == "never") return WGPUCompareFunction_Never;
    if (function == "less") return WGPUCompareFunction_Less;
    if (function == "equal") return WGPUCompareFunction_Equal;
    if (function == "less-equal") return WGPUCompareFunction_LessEqual;
    if (function == "greater") return WGPUCompareFunction_Greater;
    if (function == "not-equal") return WGPUCompareFunction_NotEqual;
    if (function == "greater-equal") return WGPUCompareFunction_GreaterEqual;
    if (function == "always") return WGPUCompareFunction_Always;
    return WGPUCompareFunction_Undefined;
}

} // namespace mystral::webgpu::bridge
