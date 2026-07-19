#pragma once

#include "mystral/async/job_system.h"
#include "mystral/js/engine.h"
#include "webgpu/async_bridge.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mystral::webgpu::bridge {

struct DecodedImageData {
    std::vector<uint8_t> encoded;
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    std::string error;
};

void decodeImageData(const async::JobContext& job, DecodedImageData& image);

class ImageDecoderBindings {
public:
    void install(js::Engine* engine, AsyncBridge* asyncBridge, bool verbose);
    void cancelPending();

private:
    js::Engine* engine_ = nullptr;
    AsyncBridge* asyncBridge_ = nullptr;
    bool verbose_ = false;
    uint64_t generation_ = (uint64_t{1} << 63) | 1;
};

} // namespace mystral::webgpu::bridge
