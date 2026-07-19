#pragma once

#include <string>

#include <webgpu/webgpu.h>

namespace mystral::webgpu::bridge {

const char* formatToString(WGPUTextureFormat format);
WGPUTextureFormat stringToFormat(const std::string& format);
WGPUTextureDimension stringToTextureDimension(const std::string& dimension);
WGPUTextureViewDimension stringToTextureViewDimension(const std::string& dimension);
WGPUAddressMode stringToAddressMode(const std::string& mode);
WGPUFilterMode stringToFilterMode(const std::string& mode);
WGPUMipmapFilterMode stringToMipmapFilterMode(const std::string& mode);
WGPUCompareFunction stringToCompareFunction(const std::string& function);

} // namespace mystral::webgpu::bridge
