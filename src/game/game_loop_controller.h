#pragma once

#include "mystral/game/game_loop.h"
#include "mystral/js/engine.h"

namespace mystral::game {

// Owns the JavaScript-facing game-loop API and its protected tick callback.
// RuntimeImpl remains responsible only for deciding when to advance it.
class GameLoopController {
public:
    void install(js::Engine* engine);
    void reset();
    void advance();
    void rebaseClock();

    bool isRunning() const { return loop_.state().running; }
    GameLoopState state() const { return loop_.state(); }
    const GameLoopStats& stats() const { return loop_.stats(); }
    void resetHighWaterMarks() { loop_.resetHighWaterMarks(); }

private:
    static double monotonicNowMs();

    js::Engine* engine_ = nullptr;
    GameLoop loop_;
    js::JSValueHandle tickHandler_;
};

}  // namespace mystral::game
