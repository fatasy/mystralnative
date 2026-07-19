#pragma once

#include "mystral/js/engine.h"

#include <vector>

namespace mystral::js {

class AnimationFrameScheduler {
public:
    void install(Engine* engine);
    void reset();
    void execute();
    bool hasCallbacks() const { return !callbacks_.empty(); }

private:
    struct Callback {
        int id = 0;
        JSValueHandle function;
    };

    Engine* engine_ = nullptr;
    std::vector<Callback> callbacks_;
    int nextId_ = 1;
};

}  // namespace mystral::js
