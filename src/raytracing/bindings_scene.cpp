/**
 * Ray tracing TLAS and ray dispatch JavaScript operations.
 */

#include "bindings_internal.h"

#include <cstring>
#include <iostream>

namespace mystral {
namespace rt {

js::JSValueHandle js_createTLAS(void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine) {
    if (!g_rtBackend || !g_rtBackend->isSupported()) {
        std::cerr << "[MystralRT] createTLAS: Hardware ray tracing not available" << std::endl;
        return engine->newNull();
    }

    if (args.empty() || !engine->isArray(args[0])) {
        std::cerr << "[MystralRT] createTLAS: Expected array of instances" << std::endl;
        return engine->newNull();
    }

    auto instancesArr = args[0];

    // Get array length
    auto lengthVal = engine->getProperty(instancesArr, "length");
    size_t count = static_cast<size_t>(engine->toNumber(lengthVal));

    if (count == 0) {
        std::cerr << "[MystralRT] createTLAS: Empty instance array" << std::endl;
        return engine->newNull();
    }

    // Collect instance descriptions
    std::vector<RTTLASInstance> instances;
    instances.reserve(count);

    for (size_t i = 0; i < count; i++) {
        auto instObj = engine->getPropertyIndex(instancesArr, static_cast<uint32_t>(i));

        RTTLASInstance inst = {};

        // Get BLAS reference
        auto blasObj = engine->getProperty(instObj, "blas");
        uint32_t blasId = getBLASId(engine, blasObj);
        auto it = g_blases.find(blasId);
        if (it == g_blases.end()) {
            std::cerr << "[MystralRT] createTLAS: Invalid BLAS at instance " << i << std::endl;
            return engine->newNull();
        }
        inst.blas = it->second;

        // Get transform (4x4 matrix as Float32Array)
        auto transformVal = engine->getProperty(instObj, "transform");
        size_t transformCount = 0;
        const float* transformData = extractFloat32Array(engine, transformVal, &transformCount);
        if (transformData && transformCount >= 16) {
            std::memcpy(inst.transform, transformData, 16 * sizeof(float));
        } else {
            // Default to identity matrix
            inst.transform[0] = 1.0f; inst.transform[5] = 1.0f;
            inst.transform[10] = 1.0f; inst.transform[15] = 1.0f;
        }

        // Get optional instance ID
        auto instanceIdVal = engine->getProperty(instObj, "instanceId");
        inst.instanceId = engine->isUndefined(instanceIdVal) ? 0 : static_cast<uint32_t>(engine->toNumber(instanceIdVal));

        inst.mask = 0xFF;  // Default visibility
        inst.flags = 0;

        instances.push_back(inst);
    }

    // Create TLAS
    RTTLASHandle handle = g_rtBackend->createTLAS(instances.data(), instances.size());
    if (!handle._handle) {
        return engine->newNull();
    }

    // Store and return JS wrapper
    uint32_t id = g_nextTLASId++;
    handle._id = id;
    g_tlases[id] = handle;

    return createTLASJS(engine, id);
}

js::JSValueHandle js_updateTLAS(void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine) {
    if (!g_rtBackend || !g_rtBackend->isSupported()) {
        std::cerr << "[MystralRT] updateTLAS: Hardware ray tracing not available" << std::endl;
        return engine->newUndefined();
    }

    if (args.size() < 2) {
        std::cerr << "[MystralRT] updateTLAS: Expected (tlas, instances)" << std::endl;
        return engine->newUndefined();
    }

    // Get TLAS
    uint32_t tlasId = getTLASId(engine, args[0]);
    auto tlasIt = g_tlases.find(tlasId);
    if (tlasIt == g_tlases.end()) {
        std::cerr << "[MystralRT] updateTLAS: Invalid TLAS" << std::endl;
        return engine->newUndefined();
    }

    if (!engine->isArray(args[1])) {
        std::cerr << "[MystralRT] updateTLAS: Expected array of instances" << std::endl;
        return engine->newUndefined();
    }

    auto instancesArr = args[1];
    auto lengthVal = engine->getProperty(instancesArr, "length");
    size_t count = static_cast<size_t>(engine->toNumber(lengthVal));

    // Build instance array (same as createTLAS)
    std::vector<RTTLASInstance> instances;
    instances.reserve(count);

    for (size_t i = 0; i < count; i++) {
        auto instObj = engine->getPropertyIndex(instancesArr, static_cast<uint32_t>(i));

        RTTLASInstance inst = {};

        auto blasObj = engine->getProperty(instObj, "blas");
        uint32_t blasId = getBLASId(engine, blasObj);
        auto it = g_blases.find(blasId);
        if (it == g_blases.end()) {
            std::cerr << "[MystralRT] updateTLAS: Invalid BLAS at instance " << i << std::endl;
            return engine->newUndefined();
        }
        inst.blas = it->second;

        auto transformVal = engine->getProperty(instObj, "transform");
        size_t transformCount = 0;
        const float* transformData = extractFloat32Array(engine, transformVal, &transformCount);
        if (transformData && transformCount >= 16) {
            std::memcpy(inst.transform, transformData, 16 * sizeof(float));
        } else {
            inst.transform[0] = 1.0f; inst.transform[5] = 1.0f;
            inst.transform[10] = 1.0f; inst.transform[15] = 1.0f;
        }

        auto instanceIdVal = engine->getProperty(instObj, "instanceId");
        inst.instanceId = engine->isUndefined(instanceIdVal) ? 0 : static_cast<uint32_t>(engine->toNumber(instanceIdVal));

        inst.mask = 0xFF;
        inst.flags = 0;

        instances.push_back(inst);
    }

    g_rtBackend->updateTLAS(tlasIt->second, instances.data(), instances.size());
    return engine->newUndefined();
}

js::JSValueHandle js_traceRays(void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine) {
    if (!g_rtBackend || !g_rtBackend->isSupported()) {
        std::cerr << "[MystralRT] traceRays: Hardware ray tracing not available" << std::endl;
        return engine->newUndefined();
    }

    if (args.empty() || !engine->isObject(args[0])) {
        std::cerr << "[MystralRT] traceRays: Expected options object" << std::endl;
        return engine->newUndefined();
    }

    auto options = args[0];

    // Get TLAS
    auto tlasObj = engine->getProperty(options, "tlas");
    uint32_t tlasId = getTLASId(engine, tlasObj);
    auto tlasIt = g_tlases.find(tlasId);
    if (tlasIt == g_tlases.end()) {
        std::cerr << "[MystralRT] traceRays: Invalid TLAS" << std::endl;
        return engine->newUndefined();
    }

    // Get dimensions
    auto widthVal = engine->getProperty(options, "width");
    auto heightVal = engine->getProperty(options, "height");
    uint32_t width = static_cast<uint32_t>(engine->toNumber(widthVal));
    uint32_t height = static_cast<uint32_t>(engine->toNumber(heightVal));

    // Get output texture (WebGPU texture handle)
    auto outputTextureVal = engine->getProperty(options, "outputTexture");
    void* outputTexture = engine->getPrivateData(outputTextureVal);

    TraceRaysOptions traceOptions = {};
    traceOptions.tlas = tlasIt->second;
    traceOptions.width = width;
    traceOptions.height = height;
    traceOptions.outputTexture = outputTexture;

    // Optional uniforms
    auto uniformsVal = engine->getProperty(options, "uniforms");
    if (!engine->isUndefined(uniformsVal)) {
        size_t uniformsSize = 0;
        traceOptions.uniforms = engine->getArrayBufferData(uniformsVal, &uniformsSize);
        traceOptions.uniformsSize = uniformsSize;
    }

    g_rtBackend->traceRays(traceOptions);
    return engine->newUndefined();
}

js::JSValueHandle js_destroyTLAS(void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine) {
    if (args.empty()) return engine->newUndefined();

    uint32_t tlasId = getTLASId(engine, args[0]);
    auto it = g_tlases.find(tlasId);
    if (it != g_tlases.end()) {
        if (g_rtBackend) {
            g_rtBackend->destroyTLAS(it->second);
        }
        g_tlases.erase(it);
    }
    return engine->newUndefined();
}

}  // namespace rt
}  // namespace mystral
