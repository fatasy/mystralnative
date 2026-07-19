#include "webgpu/constants.h"

#include "mystral/js/engine.h"

namespace mystral::webgpu::bridge {

void installConstants(js::Engine* engine) {
    auto gpuBufferUsage = engine->newObject();
    engine->setProperty(gpuBufferUsage, "MAP_READ", engine->newNumber(0x0001));
    engine->setProperty(gpuBufferUsage, "MAP_WRITE", engine->newNumber(0x0002));
    engine->setProperty(gpuBufferUsage, "COPY_SRC", engine->newNumber(0x0004));
    engine->setProperty(gpuBufferUsage, "COPY_DST", engine->newNumber(0x0008));
    engine->setProperty(gpuBufferUsage, "INDEX", engine->newNumber(0x0010));
    engine->setProperty(gpuBufferUsage, "VERTEX", engine->newNumber(0x0020));
    engine->setProperty(gpuBufferUsage, "UNIFORM", engine->newNumber(0x0040));
    engine->setProperty(gpuBufferUsage, "STORAGE", engine->newNumber(0x0080));
    engine->setProperty(gpuBufferUsage, "INDIRECT", engine->newNumber(0x0100));
    engine->setProperty(gpuBufferUsage, "QUERY_RESOLVE", engine->newNumber(0x0200));
    engine->setGlobalProperty("GPUBufferUsage", gpuBufferUsage);

    auto gpuTextureUsage = engine->newObject();
    engine->setProperty(gpuTextureUsage, "COPY_SRC", engine->newNumber(0x01));
    engine->setProperty(gpuTextureUsage, "COPY_DST", engine->newNumber(0x02));
    engine->setProperty(gpuTextureUsage, "TEXTURE_BINDING", engine->newNumber(0x04));
    engine->setProperty(gpuTextureUsage, "STORAGE_BINDING", engine->newNumber(0x08));
    engine->setProperty(gpuTextureUsage, "RENDER_ATTACHMENT", engine->newNumber(0x10));
    engine->setGlobalProperty("GPUTextureUsage", gpuTextureUsage);

    auto gpuShaderStage = engine->newObject();
    engine->setProperty(gpuShaderStage, "VERTEX", engine->newNumber(0x1));
    engine->setProperty(gpuShaderStage, "FRAGMENT", engine->newNumber(0x2));
    engine->setProperty(gpuShaderStage, "COMPUTE", engine->newNumber(0x4));
    engine->setGlobalProperty("GPUShaderStage", gpuShaderStage);

    auto gpuMapMode = engine->newObject();
    engine->setProperty(gpuMapMode, "READ", engine->newNumber(0x1));
    engine->setProperty(gpuMapMode, "WRITE", engine->newNumber(0x2));
    engine->setGlobalProperty("GPUMapMode", gpuMapMode);
}

} // namespace mystral::webgpu::bridge
