/**
 * JavaScript Engine Factory
 *
 * Creates the appropriate JS engine based on platform and build configuration.
 */

#include "mystral/js/engine.h"
#include <iostream>

namespace mystral {
namespace js {

// Forward declarations of engine factory functions
#if defined(MYSTRAL_JS_QUICKJS)
std::unique_ptr<Engine> createQuickJSEngine();
#endif

#if defined(MYSTRAL_JS_V8)
std::unique_ptr<Engine> createV8Engine();
#endif

std::unique_ptr<Engine> createEngine() {
#if defined(MYSTRAL_JS_V8)
    std::cout << "[JS] Creating V8 engine (platform default)" << std::endl;
    return createV8Engine();
#elif defined(MYSTRAL_JS_QUICKJS)
    std::cout << "[JS] Creating QuickJS engine (fallback)" << std::endl;
    return createQuickJSEngine();
#else
    std::cerr << "[JS] No JavaScript engine available!" << std::endl;
    return nullptr;
#endif
}

std::unique_ptr<Engine> createEngine(EngineType type) {
    switch (type) {
        case EngineType::QuickJS:
#if defined(MYSTRAL_JS_QUICKJS)
            std::cout << "[JS] Creating QuickJS engine" << std::endl;
            return createQuickJSEngine();
#else
            std::cerr << "[JS] QuickJS not compiled in" << std::endl;
            return nullptr;
#endif

        case EngineType::V8:
#if defined(MYSTRAL_JS_V8)
            std::cout << "[JS] Creating V8 engine" << std::endl;
            return createV8Engine();
#else
            std::cerr << "[JS] V8 not compiled in" << std::endl;
            return nullptr;
#endif

        default:
            std::cerr << "[JS] Unknown engine type" << std::endl;
            return nullptr;
    }
}

}  // namespace js
}  // namespace mystral
