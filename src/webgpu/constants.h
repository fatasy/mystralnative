#pragma once

namespace mystral::js {
class Engine;
}

namespace mystral::webgpu::bridge {

void installConstants(js::Engine* engine);

} // namespace mystral::webgpu::bridge
