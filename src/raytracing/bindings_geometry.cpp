/**
 * Ray tracing geometry and BLAS JavaScript operations.
 */

#include "bindings_internal.h"

#include <iostream>

namespace mystral {
namespace rt {

js::JSValueHandle js_createGeometry(void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine) {
    if (!g_rtBackend || !g_rtBackend->isSupported()) {
        std::cerr << "[MystralRT] createGeometry: Hardware ray tracing not available" << std::endl;
        return engine->newNull();
    }

    if (args.empty() || !engine->isObject(args[0])) {
        std::cerr << "[MystralRT] createGeometry: Expected options object" << std::endl;
        return engine->newNull();
    }

    auto options = args[0];

    // Extract vertices (required)
    auto verticesVal = engine->getProperty(options, "vertices");
    size_t vertexCount = 0;
    const float* vertices = extractFloat32Array(engine, verticesVal, &vertexCount);
    if (!vertices || vertexCount == 0) {
        std::cerr << "[MystralRT] createGeometry: Invalid or missing vertices" << std::endl;
        return engine->newNull();
    }

    // Extract optional parameters
    auto indicesVal = engine->getProperty(options, "indices");
    size_t indexCount = 0;
    const uint32_t* indices = nullptr;
    if (!engine->isUndefined(indicesVal)) {
        indices = extractUint32Array(engine, indicesVal, &indexCount);
    }

    auto strideVal = engine->getProperty(options, "vertexStride");
    size_t vertexStride = engine->isUndefined(strideVal) ? 12 : static_cast<size_t>(engine->toNumber(strideVal));

    auto offsetVal = engine->getProperty(options, "vertexOffset");
    size_t vertexOffset = engine->isUndefined(offsetVal) ? 0 : static_cast<size_t>(engine->toNumber(offsetVal));

    // Build geometry description
    RTGeometryDesc desc = {};
    desc.vertices = vertices;
    desc.vertexCount = vertexCount / 3;  // vec3 positions
    desc.vertexStride = vertexStride;
    desc.vertexOffset = vertexOffset;
    desc.indices = indices;
    desc.indexCount = indexCount;

    // Create geometry
    RTGeometryHandle handle = g_rtBackend->createGeometry(desc);
    if (!handle._handle) {
        return engine->newNull();
    }

    // Store and return JS wrapper
    uint32_t id = g_nextGeometryId++;
    handle._id = id;
    g_geometries[id] = handle;

    return createGeometryJS(engine, id);
}

js::JSValueHandle js_createBLAS(void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine) {
    if (!g_rtBackend || !g_rtBackend->isSupported()) {
        std::cerr << "[MystralRT] createBLAS: Hardware ray tracing not available" << std::endl;
        return engine->newNull();
    }

    if (args.empty() || !engine->isArray(args[0])) {
        std::cerr << "[MystralRT] createBLAS: Expected array of geometries" << std::endl;
        return engine->newNull();
    }

    auto geometriesArr = args[0];

    // Get array length
    auto lengthVal = engine->getProperty(geometriesArr, "length");
    size_t count = static_cast<size_t>(engine->toNumber(lengthVal));

    if (count == 0) {
        std::cerr << "[MystralRT] createBLAS: Empty geometry array" << std::endl;
        return engine->newNull();
    }

    // Collect geometry handles
    std::vector<RTGeometryHandle> handles;
    handles.reserve(count);

    for (size_t i = 0; i < count; i++) {
        auto geomObj = engine->getPropertyIndex(geometriesArr, static_cast<uint32_t>(i));
        uint32_t geomId = getGeometryId(engine, geomObj);

        auto it = g_geometries.find(geomId);
        if (it == g_geometries.end()) {
            std::cerr << "[MystralRT] createBLAS: Invalid geometry at index " << i << std::endl;
            return engine->newNull();
        }
        handles.push_back(it->second);
    }

    // Create BLAS
    RTBLASHandle handle = g_rtBackend->createBLAS(handles.data(), handles.size());
    if (!handle._handle) {
        return engine->newNull();
    }

    // Store and return JS wrapper
    uint32_t id = g_nextBLASId++;
    handle._id = id;
    g_blases[id] = handle;

    return createBLASJS(engine, id);
}

js::JSValueHandle js_destroyBLAS(void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine) {
    if (args.empty()) return engine->newUndefined();

    uint32_t blasId = getBLASId(engine, args[0]);
    auto it = g_blases.find(blasId);
    if (it != g_blases.end()) {
        if (g_rtBackend) {
            g_rtBackend->destroyBLAS(it->second);
        }
        g_blases.erase(it);
    }
    return engine->newUndefined();
}

js::JSValueHandle js_destroyGeometry(void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine) {
    if (args.empty()) return engine->newUndefined();

    uint32_t geomId = getGeometryId(engine, args[0]);
    auto it = g_geometries.find(geomId);
    if (it != g_geometries.end()) {
        if (g_rtBackend) {
            g_rtBackend->destroyGeometry(it->second);
        }
        g_geometries.erase(it);
    }
    return engine->newUndefined();
}

}  // namespace rt
}  // namespace mystral
