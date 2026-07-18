/**
 * WebGPU frame capture and screenshot support.
 */

#include "mystral/webgpu/context.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#if defined(MYSTRAL_WEBGPU_WGPU) || defined(MYSTRAL_WEBGPU_DAWN)
#include "webgpu/webgpu.h"
#include "mystral/webgpu_compat.h"
#endif

extern "C" int stbi_write_png(
    const char* filename, int w, int h, int comp, const void* data, int stride_in_bytes);

#if defined(MYSTRAL_WEBGPU_WGPU)
extern "C" {
typedef struct WGPUWrappedSubmissionIndex {
    WGPUQueue queue;
    uint64_t submissionIndex;
} WGPUWrappedSubmissionIndex;

WGPUBool wgpuDevicePoll(
    WGPUDevice device,
    WGPUBool wait,
    WGPUWrappedSubmissionIndex const* wrappedSubmissionIndex);
}
#endif

namespace mystral {
namespace webgpu {

// Screenshot callback data
// Note: Extra padding added due to observed stack corruption during initialization
struct BufferMapData {
    bool completed = false;
    uint8_t _pad1[7] = {};  // Padding to align status
    WGPUBufferMapAsyncStatus_Compat status = WGPUBufferMapAsyncStatus_Unknown_Compat;
    uint8_t _pad2[12] = {}; // Extra padding to absorb any overwrites
};

#if WGPU_BUFFER_MAP_USES_CALLBACK_INFO
// Dawn buffer map callback
static void onBufferMapped(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void* userdata2) {
    auto* data = static_cast<BufferMapData*>(userdata1);
    data->status = status;
    data->completed = true;
}
#else
// wgpu-native buffer map callback
static void onBufferMapped(WGPUBufferMapAsyncStatus status, void* userdata) {
    auto* data = static_cast<BufferMapData*>(userdata);
    data->status = status;
    data->completed = true;
}
#endif

// Forward declarations for bindings.cpp functions
void* getCurrentRenderedTexture();
uint32_t getCurrentTextureWidth();
uint32_t getCurrentTextureHeight();
void* getScreenshotBuffer();
size_t getScreenshotBufferSize();
uint32_t getScreenshotBytesPerRow();
bool isScreenshotReady();
void clearScreenshotReady();

bool Context::saveScreenshot(const char* filename) {
    if (!device_ || !queue_) {
        std::cerr << "[Screenshot] WebGPU not initialized" << std::endl;
        return false;
    }

    // Check if screenshot buffer is ready (populated during queue.submit)
    if (!isScreenshotReady()) {
        std::cerr << "[Screenshot] No rendered frame available yet" << std::endl;
        return false;
    }

    WGPUBuffer screenshotBuffer = (WGPUBuffer)getScreenshotBuffer();
    if (!screenshotBuffer) {
        std::cerr << "[Screenshot] Screenshot buffer not available" << std::endl;
        return false;
    }

    // Get dimensions for screenshot
    uint32_t width = getCurrentTextureWidth();
    uint32_t height = getCurrentTextureHeight();
    uint32_t bytesPerRow = getScreenshotBytesPerRow();
    size_t bufferSize = getScreenshotBufferSize();

    // Map the screenshot buffer (it was already populated during submit)
    BufferMapData mapData;

#if WGPU_BUFFER_MAP_USES_CALLBACK_INFO
    // Dawn uses CallbackInfo struct with required callback mode
    WGPUBufferMapCallbackInfo mapCallbackInfo = {};
    mapCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    mapCallbackInfo.callback = onBufferMapped;
    mapCallbackInfo.userdata1 = &mapData;
    mapCallbackInfo.userdata2 = nullptr;
    wgpuBufferMapAsync(screenshotBuffer, WGPUMapMode_Read, 0, bufferSize, mapCallbackInfo);
#else
    // wgpu-native uses separate callback and userdata
    wgpuBufferMapAsync(screenshotBuffer, WGPUMapMode_Read, 0, bufferSize, onBufferMapped, &mapData);
#endif

    // Use wgpuDevicePoll/Tick to wait for the buffer mapping to complete
#if defined(MYSTRAL_WEBGPU_WGPU)
    int maxIterations = 100;
    while (!mapData.completed && maxIterations-- > 0) {
        wgpuDevicePoll(device_, true, nullptr);
    }
#else
    // Dawn: Use device tick and instance process events
    int maxIterations = 5000;
    while (!mapData.completed && maxIterations-- > 0) {
        wgpuDeviceTick(device_);
        wgpuInstanceProcessEvents(instance_);
        if (!mapData.completed && maxIterations % 100 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
#endif

    if (!mapData.completed) {
        std::cerr << "[Screenshot] Buffer mapping timed out" << std::endl;
        return false;
    }

    if (mapData.status != WGPUBufferMapAsyncStatus_Success_Compat) {
        std::cerr << "[Screenshot] Buffer map failed with status: " << mapData.status << std::endl;
        return false;
    }

    // Read the data
    const void* mappedData = wgpuBufferGetConstMappedRange(screenshotBuffer, 0, bufferSize);
    if (!mappedData) {
        std::cerr << "[Screenshot] Failed to get mapped range" << std::endl;
        wgpuBufferUnmap(screenshotBuffer);
        return false;
    }

    // Debug: Print first few bytes of mapped data (BGRA format)
    const uint8_t* debugBytes = static_cast<const uint8_t*>(mappedData);
    std::cout << "[Screenshot] First 16 bytes (BGRA raw): ";
    for (int i = 0; i < 16; i++) {
        std::cout << (int)debugBytes[i] << " ";
    }
    std::cout << std::endl;

    // Also check bytes in the middle of the image
    size_t midOffset = bytesPerRow * (height / 2) + (width / 2) * 4;
    std::cout << "[Screenshot] Middle bytes (BGRA raw): ";
    for (int i = 0; i < 16 && (midOffset + i) < bufferSize; i++) {
        std::cout << (int)debugBytes[midOffset + i] << " ";
    }
    std::cout << std::endl;

    // Convert BGRA to RGBA and remove row padding
    std::vector<uint8_t> rgbaData(width * height * 4);
    const uint8_t* src = static_cast<const uint8_t*>(mappedData);
    uint8_t* dst = rgbaData.data();

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* srcRow = src + y * bytesPerRow;
        uint8_t* dstRow = dst + y * width * 4;
        for (uint32_t x = 0; x < width; x++) {
            // BGRA -> RGBA
            dstRow[x * 4 + 0] = srcRow[x * 4 + 2];  // R <- B
            dstRow[x * 4 + 1] = srcRow[x * 4 + 1];  // G <- G
            dstRow[x * 4 + 2] = srcRow[x * 4 + 0];  // B <- R
            dstRow[x * 4 + 3] = srcRow[x * 4 + 3];  // A <- A
        }
    }

    // Unmap the screenshot buffer (keep it for future screenshots)
    wgpuBufferUnmap(screenshotBuffer);

    // Save as PNG using stb_image_write
    if (!stbi_write_png(filename, width, height, 4, rgbaData.data(), width * 4)) {
        std::cerr << "[Screenshot] Failed to write PNG: " << filename << std::endl;
        return false;
    }

    std::cout << "[Screenshot] Saved: " << filename << " (" << width << "x" << height << ")" << std::endl;
    return true;
}

bool Context::captureFrame(std::vector<uint8_t>& outData, uint32_t& outWidth, uint32_t& outHeight) {
    if (!device_ || !queue_) {
        return false;
    }

    // Check if screenshot buffer is ready (populated during queue.submit)
    if (!isScreenshotReady()) {
        return false;
    }

    WGPUBuffer screenshotBuffer = (WGPUBuffer)getScreenshotBuffer();
    if (!screenshotBuffer) {
        return false;
    }

    // Get dimensions
    outWidth = getCurrentTextureWidth();
    outHeight = getCurrentTextureHeight();
    uint32_t bytesPerRow = getScreenshotBytesPerRow();
    size_t bufferSize = getScreenshotBufferSize();

    // Map the screenshot buffer
    BufferMapData mapData;

#if WGPU_BUFFER_MAP_USES_CALLBACK_INFO
    WGPUBufferMapCallbackInfo mapCallbackInfo = {};
    mapCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    mapCallbackInfo.callback = onBufferMapped;
    mapCallbackInfo.userdata1 = &mapData;
    mapCallbackInfo.userdata2 = nullptr;
    wgpuBufferMapAsync(screenshotBuffer, WGPUMapMode_Read, 0, bufferSize, mapCallbackInfo);
#else
    wgpuBufferMapAsync(screenshotBuffer, WGPUMapMode_Read, 0, bufferSize, onBufferMapped, &mapData);
#endif

#if defined(MYSTRAL_WEBGPU_WGPU)
    int maxIterations = 100;
    while (!mapData.completed && maxIterations-- > 0) {
        wgpuDevicePoll(device_, true, nullptr);
    }
#else
    int maxIterations = 5000;
    while (!mapData.completed && maxIterations-- > 0) {
        wgpuDeviceTick(device_);
        wgpuInstanceProcessEvents(instance_);
        if (!mapData.completed && maxIterations % 100 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
#endif

    if (!mapData.completed || mapData.status != WGPUBufferMapAsyncStatus_Success_Compat) {
        return false;
    }

    // Read the data
    const void* mappedData = wgpuBufferGetConstMappedRange(screenshotBuffer, 0, bufferSize);
    if (!mappedData) {
        wgpuBufferUnmap(screenshotBuffer);
        return false;
    }

    // Convert BGRA to RGBA and remove row padding
    outData.resize(outWidth * outHeight * 4);
    const uint8_t* src = static_cast<const uint8_t*>(mappedData);
    uint8_t* dst = outData.data();

    for (uint32_t y = 0; y < outHeight; y++) {
        const uint8_t* srcRow = src + y * bytesPerRow;
        uint8_t* dstRow = dst + y * outWidth * 4;
        for (uint32_t x = 0; x < outWidth; x++) {
            // BGRA -> RGBA
            dstRow[x * 4 + 0] = srcRow[x * 4 + 2];  // R <- B
            dstRow[x * 4 + 1] = srcRow[x * 4 + 1];  // G <- G
            dstRow[x * 4 + 2] = srcRow[x * 4 + 0];  // B <- R
            dstRow[x * 4 + 3] = srcRow[x * 4 + 3];  // A <- A
        }
    }

    wgpuBufferUnmap(screenshotBuffer);
    return true;
}

}  // namespace webgpu
}  // namespace mystral
