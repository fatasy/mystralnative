#include "mystral/game/game_loop.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

bool near(double left, double right) {
    return std::abs(left - right) < 0.000001;
}

}  // namespace

int main() {
    using namespace mystral::game;

    GameLoop loop;
    std::string error;
    if (!loop.configure({10.0, 2, 1000.0}, &error)) return 1;

    std::vector<GameTick> ticks;
    loop.start(0.0);
    auto first = loop.advance(550.0, [&](const GameTick& tick) {
        ticks.push_back(tick);
        return true;
    });
    if (first.ticksExecuted != 2 || first.ticksDropped != 3 || ticks.size() != 2 ||
        ticks[0].tick != 0 || ticks[1].tick != 1 || !near(ticks[1].simulationTimeMs, 200.0) ||
        !near(loop.state().interpolationAlpha, 0.5)) {
        std::cerr << "Fixed-step catch-up validation failed" << std::endl;
        return 2;
    }

    loop.pause();
    if (!loop.step(3, &error)) return 3;
    auto stepped = loop.advance(10'000.0, [&](const GameTick& tick) {
        ticks.push_back(tick);
        return true;
    });
    if (stepped.ticksExecuted != 2 || loop.state().pendingStepTicks != 1 ||
        loop.state().tickCount != 4) {
        std::cerr << "Paused stepping validation failed" << std::endl;
        return 4;
    }
    stepped = loop.advance(20'000.0, [&](const GameTick&) { return true; });
    if (stepped.ticksExecuted != 1 || loop.state().pendingStepTicks != 0 ||
        loop.state().tickCount != 5) {
        return 5;
    }

    loop.resume(20'000.0);
    if (!loop.setTimeScale(2.0, &error)) return 6;
    auto scaled = loop.advance(20'050.0, [&](const GameTick&) { return true; });
    if (scaled.ticksExecuted != 1 || loop.state().tickCount != 6) {
        std::cerr << "Time-scale validation failed" << std::endl;
        return 7;
    }

    auto failed = loop.advance(20'150.0, [&](const GameTick&) { return false; });
    if (!failed.handlerFailed || !loop.state().paused ||
        loop.stats().handlerFailures != 1 || loop.state().tickCount != 6) {
        std::cerr << "Handler failure containment validation failed" << std::endl;
        return 8;
    }

    if (loop.configure({0.0, 2, 1000.0}, &error) ||
        loop.setTimeScale(0.0, &error)) {
        std::cerr << "Configuration validation failed" << std::endl;
        return 9;
    }

    loop.start(0.0);
    auto pausedFromHandler = loop.advance(250.0, [&](const GameTick&) {
        loop.pause();
        return true;
    });
    if (pausedFromHandler.ticksExecuted != 1 || !loop.state().paused) {
        std::cerr << "Immediate pause validation failed" << std::endl;
        return 10;
    }

    std::cout << "Game loop smoke test passed" << std::endl;
    return 0;
}
