#include <catch2/catch_test_macros.hpp>
#include "Core/CoroutinePool.h"

using namespace Core;

TEST_CASE("CoroutinePool: instance returns singleton", "[CoroutinePool]") {
    CoroutinePool &p1 = CoroutinePool::instance();
    CoroutinePool &p2 = CoroutinePool::instance();
    REQUIRE(&p1 == &p2);
}

TEST_CASE("CoroutinePool: allocate within block size", "[CoroutinePool]") {
    auto &pool = CoroutinePool::instance();
    size_t initialCount = pool.allocatedCount();

    void *p = pool.allocate(128);
    REQUIRE(p != nullptr);

    // Fill with pattern to verify writable
    std::memset(p, 0xCD, 128);

    pool.deallocate(p, 128);
    // After dealloc, we should be able to allocate again
    void *p2 = pool.allocate(128);
    REQUIRE(p2 != nullptr);
    pool.deallocate(p2, 128);
}

TEST_CASE("CoroutinePool: allocate larger than block size falls back to new", "[CoroutinePool]") {
    auto &pool = CoroutinePool::instance();
    size_t largeSize = pool.blockSize() * 2;

    void *p = pool.allocate(largeSize);
    REQUIRE(p != nullptr);
    std::memset(p, 0xEF, largeSize);
    pool.deallocate(p, largeSize);
}

TEST_CASE("CoroutinePool: multiple allocations", "[CoroutinePool]") {
    auto &pool = CoroutinePool::instance();
    std::vector<void *> ptrs;
    for (int i = 0; i < 100; ++i) {
        void *p = pool.allocate(64);
        REQUIRE(p != nullptr);
        ptrs.push_back(p);
    }
    for (auto *p : ptrs) {
        pool.deallocate(p, 64);
    }
}

TEST_CASE("CoroutinePool: blockSize returns configured value", "[CoroutinePool]") {
    // blockSize depends on construction, but instance is default 256
    auto &pool = CoroutinePool::instance();
    REQUIRE(pool.blockSize() >= 64);
}
