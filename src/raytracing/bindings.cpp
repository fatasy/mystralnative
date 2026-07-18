/**
 * MystralNative Ray Tracing JavaScript Bindings
 *
 * Implements the mystralRT global object for JavaScript access to
 * hardware ray tracing capabilities.
 *
 * API matches src/raytracing/types.ts MystralRT interface.
 */

#include "bindings.h"
#include "bindings_internal.h"

#include <iostream>

namespace mystral {
namespace rt {

// Global backend instance
std::unique_ptr<IRTBackend> g_rtBackend = nullptr;

// Handle tracking for JS object cleanup
uint32_t g_nextGeometryId = 1;
uint32_t g_nextBLASId = 1;
uint32_t g_nextTLASId = 1;

std::unordered_map<uint32_t, RTGeometryHandle> g_geometries;
std::unordered_map<uint32_t, RTBLASHandle> g_blases;
std::unordered_map<uint32_t, RTTLASHandle> g_tlases;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Extract Float32Array data from a JS value.
 * @param engine JS engine
 * @param value JS value (should be Float32Array)
 * @param outCount Output: number of floats
 * @return Pointer to float data, or nullptr on error
 */
const float* extractFloat32Array(js::Engine* engine, js::JSValueHandle value, size_t* outCount) {
    size_t byteSize = 0;
    void* data = engine->getArrayBufferData(value, &byteSize);
    if (!data || byteSize == 0) {
        return nullptr;
    }
    if (outCount) {
        *outCount = byteSize / sizeof(float);
    }
    return static_cast<const float*>(data);
}

/**
 * Extract Uint32Array data from a JS value.
 * @param engine JS engine
 * @param value JS value (should be Uint32Array)
 * @param outCount Output: number of uint32s
 * @return Pointer to uint32 data, or nullptr on error
 */
const uint32_t* extractUint32Array(js::Engine* engine, js::JSValueHandle value, size_t* outCount) {
    size_t byteSize = 0;
    void* data = engine->getArrayBufferData(value, &byteSize);
    if (!data || byteSize == 0) {
        return nullptr;
    }
    if (outCount) {
        *outCount = byteSize / sizeof(uint32_t);
    }
    return static_cast<const uint32_t*>(data);
}

/**
 * Create a JS geometry wrapper object.
 */
js::JSValueHandle createGeometryJS(js::Engine* engine, uint32_t id) {
    auto obj = engine->newObject();
    engine->setProperty(obj, "_type", engine->newString("geometry"));
    engine->setProperty(obj, "_id", engine->newNumber(static_cast<double>(id)));
    return obj;
}

/**
 * Create a JS BLAS wrapper object.
 */
js::JSValueHandle createBLASJS(js::Engine* engine, uint32_t id) {
    auto obj = engine->newObject();
    engine->setProperty(obj, "_type", engine->newString("blas"));
    engine->setProperty(obj, "_id", engine->newNumber(static_cast<double>(id)));
    return obj;
}

/**
 * Create a JS TLAS wrapper object.
 */
js::JSValueHandle createTLASJS(js::Engine* engine, uint32_t id) {
    auto obj = engine->newObject();
    engine->setProperty(obj, "_type", engine->newString("tlas"));
    engine->setProperty(obj, "_id", engine->newNumber(static_cast<double>(id)));
    return obj;
}

/**
 * Get geometry ID from JS object.
 */
uint32_t getGeometryId(js::Engine* engine, js::JSValueHandle obj) {
    auto idVal = engine->getProperty(obj, "_id");
    if (engine->isUndefined(idVal)) return 0;
    return static_cast<uint32_t>(engine->toNumber(idVal));
}

/**
 * Get BLAS ID from JS object.
 */
uint32_t getBLASId(js::Engine* engine, js::JSValueHandle obj) {
    auto idVal = engine->getProperty(obj, "_id");
    if (engine->isUndefined(idVal)) return 0;
    return static_cast<uint32_t>(engine->toNumber(idVal));
}

/**
 * Get TLAS ID from JS object.
 */
uint32_t getTLASId(js::Engine* engine, js::JSValueHandle obj) {
    auto idVal = engine->getProperty(obj, "_id");
    if (engine->isUndefined(idVal)) return 0;
    return static_cast<uint32_t>(engine->toNumber(idVal));
}

// ============================================================================
// JS Binding Functions
// ============================================================================

js::JSValueHandle js_isSupported(void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine) {
    if (!g_rtBackend) {
        return engine->newBoolean(false);
    }
    return engine->newBoolean(g_rtBackend->isSupported());
}

js::JSValueHandle js_getBackend(void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine) {
    if (!g_rtBackend) {
        return engine->newString("none");
    }
    return engine->newString(g_rtBackend->getBackend());
}

// ============================================================================
// Public API
// ============================================================================

bool initializeRTBindings(js::Engine* engine) {
    if (!engine) {
        std::cerr << "[MystralRT] initializeRTBindings: No JS engine provided" << std::endl;
        return false;
    }

    // Create RT backend
    g_rtBackend = createRTBackend();

    // Create mystralRT global object
    auto mystralRT = engine->newObject();

    // Register methods
    engine->setProperty(mystralRT, "isSupported",
        engine->newFunction("isSupported", [engine](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return js_isSupported(ctx, args, engine);
        })
    );

    engine->setProperty(mystralRT, "getBackend",
        engine->newFunction("getBackend", [engine](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return js_getBackend(ctx, args, engine);
        })
    );

    engine->setProperty(mystralRT, "createGeometry",
        engine->newFunction("createGeometry", [engine](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return js_createGeometry(ctx, args, engine);
        })
    );

    engine->setProperty(mystralRT, "createBLAS",
        engine->newFunction("createBLAS", [engine](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return js_createBLAS(ctx, args, engine);
        })
    );

    engine->setProperty(mystralRT, "createTLAS",
        engine->newFunction("createTLAS", [engine](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return js_createTLAS(ctx, args, engine);
        })
    );

    engine->setProperty(mystralRT, "updateTLAS",
        engine->newFunction("updateTLAS", [engine](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return js_updateTLAS(ctx, args, engine);
        })
    );

    engine->setProperty(mystralRT, "traceRays",
        engine->newFunction("traceRays", [engine](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return js_traceRays(ctx, args, engine);
        })
    );

    engine->setProperty(mystralRT, "destroyBLAS",
        engine->newFunction("destroyBLAS", [engine](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return js_destroyBLAS(ctx, args, engine);
        })
    );

    engine->setProperty(mystralRT, "destroyTLAS",
        engine->newFunction("destroyTLAS", [engine](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return js_destroyTLAS(ctx, args, engine);
        })
    );

    engine->setProperty(mystralRT, "destroyGeometry",
        engine->newFunction("destroyGeometry", [engine](void* ctx, const std::vector<js::JSValueHandle>& args) {
            return js_destroyGeometry(ctx, args, engine);
        })
    );

    // Register mystralRT as global
    engine->setGlobalProperty("mystralRT", mystralRT);

    std::cout << "[MystralRT] Bindings initialized (backend: " << g_rtBackend->getBackend() << ")" << std::endl;
    return true;
}

void cleanupRTBindings() {
    // Destroy all tracked resources
    for (auto& [id, handle] : g_tlases) {
        if (g_rtBackend) {
            g_rtBackend->destroyTLAS(handle);
        }
    }
    g_tlases.clear();

    for (auto& [id, handle] : g_blases) {
        if (g_rtBackend) {
            g_rtBackend->destroyBLAS(handle);
        }
    }
    g_blases.clear();

    for (auto& [id, handle] : g_geometries) {
        if (g_rtBackend) {
            g_rtBackend->destroyGeometry(handle);
        }
    }
    g_geometries.clear();

    // Destroy backend
    g_rtBackend.reset();

    // Reset ID counters
    g_nextGeometryId = 1;
    g_nextBLASId = 1;
    g_nextTLASId = 1;

    std::cout << "[MystralRT] Bindings cleaned up" << std::endl;
}

}  // namespace rt
}  // namespace mystral
