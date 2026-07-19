#pragma once

#include "mystral/js/engine.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_set>
#include <vector>

#if defined(MYSTRAL_HAS_LIBUV) && !defined(__ANDROID__) && !defined(IOS)
#include <uv.h>
#define MYSTRAL_TIMERS_USE_LIBUV 1
#endif

namespace mystral::js {

class RuntimeTimers {
public:
    void install(Engine* engine);
    void clear();
    void finishShutdown();
    bool hasActive() const;
    size_t executeCallbacks(size_t maxCount = static_cast<size_t>(-1));

private:
    void setupTimers();

#ifdef MYSTRAL_TIMERS_USE_LIBUV
    struct UvTimerContext {
        uv_timer_t handle;
        int id = 0;
        JSValueHandle callback;
        int intervalMs = 0;
        bool cancelled = false;
        bool callbackProtected = false;
        RuntimeTimers* runtime = nullptr;
    };

    struct PendingTimerCallback {
        int id = 0;
        JSValueHandle callback;
        int intervalMs = 0;
    };

    static void onUvTimerCallback(uv_timer_t* handle);
    static void onTimerClose(uv_handle_t* handle);
    int createUvTimer(JSValueHandle callback, int delayMs, int intervalMs);
    void cancelUvTimer(int id);
    void setupLibuvTimers();

    std::map<int, std::unique_ptr<UvTimerContext>> uvTimers_;
    std::queue<PendingTimerCallback> pendingTimerCallbacks_;
    std::mutex timerMutex_;
#else
    struct TimerCallback {
        int id = 0;
        JSValueHandle callback;
        std::chrono::high_resolution_clock::time_point targetTime;
        int intervalMs = 0;
        bool cancelled = false;
    };

    void setupChronoTimers();
    std::vector<TimerCallback> timerCallbacks_;
#endif

    Engine* engine_ = nullptr;
    std::unordered_set<int> cancelledTimerIds_;
    int nextTimerId_ = 1;
};

}  // namespace mystral::js
