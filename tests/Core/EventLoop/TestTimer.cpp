#include <catch2/catch_test_macros.hpp>
#include "Core/Timer.h"
#include "Core/EventLoop.h"

using namespace Core;
using namespace std::chrono_literals;

TEST_CASE("Timer: construction succeeds", "[Timer]") {
    EventLoop loop;
    REQUIRE_NOTHROW([&]() { Timer t(loop); }());
}

TEST_CASE("Timer: waitFor returns EpollAwaiter", "[Timer]") {
    EventLoop loop;
    Timer timer(loop);
    auto awaiter = timer.waitFor(100ms);
    (void)awaiter;
}
