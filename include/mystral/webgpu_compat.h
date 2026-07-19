/**
 * Dawn WebGPU helpers.
 *
 * Usage: Include this header after webgpu/webgpu.h.
 */

#ifndef MYSTRAL_WEBGPU_COMPAT_H
#define MYSTRAL_WEBGPU_COMPAT_H

#include <cstring>
#include <string>

#if !defined(MYSTRAL_WEBGPU_DAWN)
#error "MystralNative requires the Dawn WebGPU backend"
#endif

typedef WGPUSurfaceSourceMetalLayer WGPUSurfaceDescriptorFromMetalLayer_Compat;
#define WGPUSType_SurfaceDescriptorFromMetalLayer_Compat WGPUSType_SurfaceSourceMetalLayer

typedef WGPUSurfaceSourceWindowsHWND WGPUSurfaceDescriptorFromWindowsHWND_Compat;
#define WGPUSType_SurfaceDescriptorFromWindowsHWND_Compat WGPUSType_SurfaceSourceWindowsHWND

typedef WGPUSurfaceSourceXlibWindow WGPUSurfaceDescriptorFromXlibWindow_Compat;
#define WGPUSType_SurfaceDescriptorFromXlibWindow_Compat WGPUSType_SurfaceSourceXlibWindow

typedef WGPUSurfaceSourceAndroidNativeWindow WGPUSurfaceDescriptorFromAndroidNativeWindow_Compat;
#define WGPUSType_SurfaceDescriptorFromAndroidNativeWindow_Compat WGPUSType_SurfaceSourceAndroidNativeWindow

#define WGPU_NEEDS_PROC_INIT 1

typedef WGPUTexelCopyTextureInfo WGPUImageCopyTexture_Compat;
typedef WGPUTexelCopyBufferInfo WGPUImageCopyBuffer_Compat;
typedef WGPUTexelCopyBufferLayout WGPUTextureDataLayout_Compat;

typedef WGPUMapAsyncStatus WGPUBufferMapAsyncStatus_Compat;
#define WGPUBufferMapAsyncStatus_Success_Compat WGPUMapAsyncStatus_Success
#define WGPUBufferMapAsyncStatus_Unknown_Compat WGPUMapAsyncStatus_Error

#define WGPU_SURFACE_TEXTURE_STATUS_TYPE WGPUSurfaceGetCurrentTextureStatus
#define WGPUSurfaceGetCurrentTextureStatus_Success_Compat WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal
#define WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal_Compat WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal
#define WGPUSurfaceGetCurrentTextureStatus_Error_Compat WGPUSurfaceGetCurrentTextureStatus_Error

inline WGPUStringView WGPU_STRING_VIEW(const char* str) {
    WGPUStringView sv;
    sv.data = str;
    sv.length = str ? strlen(str) : 0;
    return sv;
}

#define WGPU_STRING_VIEW_NULL WGPUStringView{nullptr, 0}

inline const char* WGPU_STRING_VIEW_TO_CSTR(
    WGPUStringView sv, char* buffer, size_t bufferSize) {
    if (sv.data == nullptr || sv.length == 0) {
        return "unknown";
    }
    size_t copyLen = sv.length < bufferSize - 1 ? sv.length : bufferSize - 1;
    memcpy(buffer, sv.data, copyLen);
    buffer[copyLen] = '\0';
    return buffer;
}

#define WGPU_PRINT_STRING_VIEW(sv) \
    ((sv).data ? std::string((sv).data, (sv).length) : std::string("unknown"))

typedef WGPUShaderSourceWGSL WGPUShaderModuleWGSLDescriptor_Compat;
#define WGPUSType_ShaderModuleWGSLDescriptor_Compat WGPUSType_ShaderSourceWGSL

inline void setupShaderModuleWGSL(
    WGPUShaderModuleDescriptor* desc,
    WGPUShaderSourceWGSL* wgslDesc,
    const char* code) {
    wgslDesc->chain.next = nullptr;
    wgslDesc->chain.sType = WGPUSType_ShaderSourceWGSL;
    wgslDesc->code = WGPU_STRING_VIEW(code);
    desc->nextInChain = &wgslDesc->chain;
    desc->label = WGPU_STRING_VIEW_NULL;
}

#define WGPU_BUFFER_MAP_USES_CALLBACK_INFO 1
#define WGPU_USES_CALLBACK_INFO_PATTERN 1
#define wgpuCommandEncoderCopyTextureToBuffer_Compat wgpuCommandEncoderCopyTextureToBuffer

#define WGPU_SET_ENTRY_POINT(state, entry) \
    (state).entryPoint = WGPU_STRING_VIEW(entry)

#define WGPU_SET_ENTRY_POINT_AUTO(state) \
    (state).entryPoint = WGPUStringView{nullptr, WGPU_STRLEN}

#define WGPU_SET_LABEL(desc, str) do { \
    static const char* _label_str = str; \
    (desc).label = WGPUStringView{_label_str, strlen(_label_str)}; \
} while (0)

#define WGPU_OPTIONAL_BOOL_TRUE WGPUOptionalBool_True
#define WGPU_OPTIONAL_BOOL_FALSE WGPUOptionalBool_False
#define WGPU_OPTIONAL_BOOL_UNDEFINED WGPUOptionalBool_Undefined

inline bool wgpuSurfaceTextureStatusIsSuccess(int status) {
    return status == WGPUSurfaceGetCurrentTextureStatus_Success_Compat;
}

#endif  // MYSTRAL_WEBGPU_COMPAT_H
