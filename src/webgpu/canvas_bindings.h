#pragma once

#include "mystral/js/engine.h"

namespace mystral::webgpu {

void installCanvasBindings(js::Engine* engine);
void installOffscreenCanvasBindings(js::Engine* engine);

} // namespace mystral::webgpu
