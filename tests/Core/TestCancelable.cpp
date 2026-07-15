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
    // 第一次调用返回 true（转换到已停止状态）
    REQUIRE(c.requestStop());
    // 第二次调用返回 false（根据 std::stop_source 规范，已经停止）
    REQUIRE_FALSE(c.requestStop());
    // 但 isStopRequested() 保持为 true
    REQUIRE(c.isStopRequested());
}
