#pragma once

#include <cstddef>
#include <cstdint>

namespace mystral::js {
class Engine;
}

namespace mystral::webgpu {

bool initBindings(
    js::Engine* engine,
    void* wgpuInstance,
    void* wgpuAdapter,
    void* wgpuDevice,
    void* wgpuQueue,
    void* wgpuSurface,
    uint32_t surfaceFormat,
    uint32_t width,
    uint32_t height,
    bool debug = false,
    uint32_t maxFrameLatency = 2,
    uint64_t maxTrackedGpuMemoryBytes = 0);

size_t processAsyncCompletions(size_t maxCount = static_cast<size_t>(-1));
void waitForDawnFrameSlot();
bool submittedDawnWorkLastFrame();
void pumpDawnProgress();
void* getCurrentSurfaceTexture();
void setOffscreenTexture(void* texture, void* textureView);
void beginDawnFrame();
void endDawnFrame();
void releaseReloadResources();
void resetSessionBindings();

} // namespace mystral::webgpu
