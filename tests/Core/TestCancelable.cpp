#include <catch2/catch_test_macros.hpp>
#include "Core/Cancelable.h"

using namespace Core;

TEST_CASE("Cancelable: default state is not stopped", "[Cancelable]") {
    Cancelable c;
    REQUIRE_FALSE(c.isStopRequested());
}

TEST_CASE("Cancelable: requestStop sets stop requested", "[Cancelable]") {
    Cancelable c;
    REQUIRE(c.requestStop());
    REQUIRE(c.isStopRequested());
}

TEST_CASE("Cancelable: stopToken reflects stop state", "[Cancelable]") {
    Cancelable c;
    auto token = c.stopToken();
    REQUIRE_FALSE(token.stop_requested());

    c.requestStop();
    REQUIRE(token.stop_requested());
}

TEST_CASE("Cancelable: move construction preserves state", "[Cancelable]") {
    Cancelable c1;
    c1.requestStop();

    Cancelable c2(std::move(c1));
    REQUIRE(c2.isStopRequested());
}

TEST_CASE("Cancelable: move assignment preserves state", "[Cancelable]") {
    Cancelable c1;
    c1.requestStop();

    Cancelable c2;
    c2 = std::move(c1);
    REQUIRE(c2.isStopRequested());
}

TEST_CASE("Cancelable: stopSource returns valid reference", "[Cancelable]") {
    Cancelable c;
    auto &src = c.stopSource();
    REQUIRE_FALSE(src.stop_requested());

    src.request_stop();
    REQUIRE(c.isStopRequested());
}

TEST_CASE("Cancelable: multiple requestStop calls are idempotent", "[Cancelable]") {
    Cancelable c;
    // First call returns true (transitioned to stopped)
    REQUIRE(c.requestStop());
    // Second call returns false per std::stop_source spec (already stopped)
    REQUIRE_FALSE(c.requestStop());
    // But isStopRequested() stays true
    REQUIRE(c.isStopRequested());
}
