#pragma once

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
    bool debug = false);

void processAsyncCompletions();
void* getCurrentSurfaceTexture();
void setOffscreenTexture(void* texture, void* textureView);
void beginDawnFrame();
void endDawnFrame();
void releaseReloadResources();
void resetSessionBindings();

} // namespace mystral::webgpu
