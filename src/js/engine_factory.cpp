/**
 * JavaScript Engine Factory
 *
 * Creates the V8 JavaScript engine.
 */

#include "mystral/js/engine.h"
#include <iostream>

namespace mystral {
namespace js {

#if defined(MYSTRAL_JS_V8)
std::unique_ptr<Engine> createV8Engine();
#endif

std::unique_ptr<Engine> createEngine() {
#if defined(MYSTRAL_JS_V8)
    std::cout << "[JS] Creating V8 engine" << std::endl;
    return createV8Engine();
#else
    std::cerr << "[JS] V8 is not available!" << std::endl;
    return nullptr;
#endif
}

std::unique_ptr<Engine> createEngine(EngineType type) {
    switch (type) {
        case EngineType::V8:
#if defined(MYSTRAL_JS_V8)
            std::cout << "[JS] Creating V8 engine" << std::endl;
            return createV8Engine();
#else
            std::cerr << "[JS] V8 is not available" << std::endl;
            return nullptr;
#endif

        default:
            std::cerr << "[JS] Unknown engine type" << std::endl;
            return nullptr;
    }
}

}  // namespace js
}  // namespace mystral
