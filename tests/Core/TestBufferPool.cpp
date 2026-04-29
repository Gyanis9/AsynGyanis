#include <catch2/catch_test_macros.hpp>
#include "Core/BufferPool.h"

using namespace Core;

TEST_CASE("BufferPool: construction and basic properties", "[BufferPool]") {
    BufferPool pool(1024, 8);
    REQUIRE(pool.bufferSize() == 1024);
    REQUIRE(pool.bufferCount() == 8);
}

TEST_CASE("BufferPool: acquire returns valid indices", "[BufferPool]") {
    BufferPool pool(256, 4);
    int idx0 = pool.acquire();
    int idx1 = pool.acquire();
    int idx2 = pool.acquire();
    int idx3 = pool.acquire();

    REQUIRE(idx0 >= 0);
    REQUIRE(idx1 >= 0);
    REQUIRE(idx2 >= 0);
    REQUIRE(idx3 >= 0);
    // All indices should be distinct
    REQUIRE(idx0 != idx1);
    REQUIRE(idx0 != idx2);
    REQUIRE(idx0 != idx3);
    REQUIRE(idx1 != idx2);
    REQUIRE(idx1 != idx3);
    REQUIRE(idx2 != idx3);
}

TEST_CASE("BufferPool: acquire returns -1 when full", "[BufferPool]") {
    BufferPool pool(64, 2);
    REQUIRE(pool.acquire() >= 0);
    REQUIRE(pool.acquire() >= 0);
    REQUIRE(pool.acquire() == -1);
}

TEST_CASE("BufferPool: release allows re-acquire", "[BufferPool]") {
    BufferPool pool(128, 2);
    int idx0 = pool.acquire();
    int idx1 = pool.acquire();
    REQUIRE(pool.acquire() == -1);

    pool.release(idx0);
    int idx2 = pool.acquire();
    REQUIRE(idx2 >= 0);
}

TEST_CASE("BufferPool: data returns valid pointer", "[BufferPool]") {
    BufferPool pool(1024, 4);
    int idx = pool.acquire();
    REQUIRE(idx >= 0);

    void *ptr = pool.data(idx);
    REQUIRE(ptr != nullptr);

    // Should be writable
    std::memset(ptr, 0xAB, 1024);
}

TEST_CASE("BufferPool: data is isolated between buffers", "[BufferPool]") {
    BufferPool pool(1024, 4);
    int idx0 = pool.acquire();
    int idx1 = pool.acquire();

    std::memset(pool.data(idx0), 0x11, 1024);
    std::memset(pool.data(idx1), 0x22, 1024);

    // Verify isolation
    auto *p0 = static_cast<unsigned char *>(pool.data(idx0));
    auto *p1 = static_cast<unsigned char *>(pool.data(idx1));
    REQUIRE(p0[0] == 0x11);
    REQUIRE(p1[0] == 0x22);
}

TEST_CASE("BufferPool: bufferSize zero throws or works", "[BufferPool]") {
    // Test edge case: buffer size 0
    BufferPool pool(0, 4);
    REQUIRE(pool.bufferSize() == 0);
    REQUIRE(pool.bufferCount() == 4);
    int idx = pool.acquire();
    REQUIRE(idx >= 0);
}
