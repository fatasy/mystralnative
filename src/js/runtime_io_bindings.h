#pragma once

#include "mystral/js/engine.h"

#include <cstdint>
#include <queue>
#include <string>
#include <vector>

namespace mystral::js {

class RuntimeIOBindings {
public:
    void install(Engine* engine, const uint64_t* generation);
    void processFileCallbacks();
    void clearFileCallbacks();

private:
    struct PendingFileCallback {
        JSValueHandle callback;
        std::vector<uint8_t> data;
        std::string error;
    };

    Engine* engine_ = nullptr;
    const uint64_t* generation_ = nullptr;
    std::queue<PendingFileCallback> pendingFileCallbacks_;
};

} // namespace mystral::js
