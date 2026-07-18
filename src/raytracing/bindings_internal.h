#pragma once

#include "rt_common.h"
#include "mystral/js/engine.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mystral {
namespace rt {

extern std::unique_ptr<IRTBackend> g_rtBackend;
extern uint32_t g_nextGeometryId;
extern uint32_t g_nextBLASId;
extern uint32_t g_nextTLASId;
extern std::unordered_map<uint32_t, RTGeometryHandle> g_geometries;
extern std::unordered_map<uint32_t, RTBLASHandle> g_blases;
extern std::unordered_map<uint32_t, RTTLASHandle> g_tlases;

const float* extractFloat32Array(
    js::Engine* engine, js::JSValueHandle value, size_t* outCount);
const uint32_t* extractUint32Array(
    js::Engine* engine, js::JSValueHandle value, size_t* outCount);

js::JSValueHandle createGeometryJS(js::Engine* engine, uint32_t id);
js::JSValueHandle createBLASJS(js::Engine* engine, uint32_t id);
js::JSValueHandle createTLASJS(js::Engine* engine, uint32_t id);

uint32_t getGeometryId(js::Engine* engine, js::JSValueHandle obj);
uint32_t getBLASId(js::Engine* engine, js::JSValueHandle obj);
uint32_t getTLASId(js::Engine* engine, js::JSValueHandle obj);

js::JSValueHandle js_createGeometry(
    void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine);
js::JSValueHandle js_createBLAS(
    void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine);
js::JSValueHandle js_createTLAS(
    void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine);
js::JSValueHandle js_updateTLAS(
    void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine);
js::JSValueHandle js_traceRays(
    void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine);
js::JSValueHandle js_destroyBLAS(
    void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine);
js::JSValueHandle js_destroyTLAS(
    void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine);
js::JSValueHandle js_destroyGeometry(
    void* ctx, const std::vector<js::JSValueHandle>& args, js::Engine* engine);

}  // namespace rt
}  // namespace mystral
