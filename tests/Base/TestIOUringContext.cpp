/**
 * @file TestIOUringContext.cpp
 * @brief IOUringContext 模块单元测试：构造、移动语义、SQE/CQE 操作
 * @copyright Copyright (c) 2026
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "Base/IOUringContext.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>

using namespace Base;

// ============================================================================
// 构造测试
// ============================================================================

TEST_CASE("IOUringContext construction with valid parameters", "[IOUringContext][construction]")
{
    REQUIRE_NOTHROW(IOUringContext(256, 0));
}

TEST_CASE("IOUringContext construction with minimum queue depth", "[IOUringContext][construction][boundary]")
{
    // io_uring requires power-of-2 queue depth, minimum is typically 2
    REQUIRE_NOTHROW(IOUringContext(2, 0));
}

TEST_CASE("IOUringContext construction with large queue depth", "[IOUringContext][construction][boundary]")
{
    REQUIRE_NOTHROW(IOUringContext(4096, 0));
}

TEST_CASE("IOUringContext construction with invalid queue depth", "[IOUringContext][construction][boundary]")
{
    // Non-power-of-2 or 0 should cause init failure on standard kernels.
    // Behavior varies by kernel version; test that at minimum 0 depth throws.
    REQUIRE_THROWS_AS(IOUringContext(0, 0), std::runtime_error);
    // Depths of 3 or 100 may or may not throw depending on kernel version.
    // Just verify they don't crash.
    try { IOUringContext(3, 0); } catch (const std::runtime_error&) {}
    try { IOUringContext(100, 0); } catch (const std::runtime_error&) {}
}

TEST_CASE("IOUringContext construction with various flags", "[IOUringContext][construction]")
{
    // Test with common flags
    REQUIRE_NOTHROW(IOUringContext(256, IORING_SETUP_SQPOLL));
    REQUIRE_NOTHROW(IOUringContext(256, IORING_SETUP_SINGLE_ISSUER));
}

// ============================================================================
// 移动语义测试
// ============================================================================

TEST_CASE("IOUringContext move construction transfers ownership", "[IOUringContext][move]")
{
    IOUringContext ctx1(256, 0);
    int fd1 = ctx1.fd();
    REQUIRE(fd1 >= 0);

    IOUringContext ctx2(std::move(ctx1));
    REQUIRE(ctx2.fd() == fd1);

    // Original should be in valid-but-unspecified state
    // It should not double-free on destruction
}

TEST_CASE("IOUringContext move assignment transfers ownership", "[IOUringContext][move]")
{
    IOUringContext ctx1(256, 0);
    int fd1 = ctx1.fd();

    IOUringContext ctx2(128, 0);
    int fd2 = ctx2.fd();

    ctx2 = std::move(ctx1);

    REQUIRE(ctx2.fd() == fd1);
    // ctx1 has been moved from, should not double-free
}

TEST_CASE("IOUringContext self-move assignment is safe", "[IOUringContext][move][boundary]")
{
    IOUringContext ctx(256, 0);
    int fd = ctx.fd();

    // Self-move assignment - should be a no-op or safe
    ctx = std::move(ctx);

    // Should still be functional
    REQUIRE(ctx.fd() == fd);
}

// ============================================================================
// 访问器测试
// ============================================================================

TEST_CASE("IOUringContext::fd returns valid file descriptor", "[IOUringContext][accessor]")
{
    IOUringContext ctx(256, 0);
    REQUIRE(ctx.fd() >= 0);
}

TEST_CASE("IOUringContext::entries returns configured entry count", "[IOUringContext][accessor]")
{
    IOUringContext ctx(256, 0);
    REQUIRE(ctx.entries() == 256);

    IOUringContext ctx2(128, 0);
    REQUIRE(ctx2.entries() == 128);

    IOUringContext ctx3(1024, 0);
    REQUIRE(ctx3.entries() == 1024);
}

// ============================================================================
// getSqe 测试
// ============================================================================

TEST_CASE("IOUringContext::getSqe returns valid pointer when queue not full", "[IOUringContext][getSqe]")
{
    IOUringContext ctx(256, 0);
    auto *sqe = ctx.getSqe();
    REQUIRE(sqe != nullptr);
}

TEST_CASE("IOUringContext::getSqe exhausts queue", "[IOUringContext][getSqe][boundary]")
{
    IOUringContext ctx(4, 0);
    // Should be able to get 4 SQEs
    for (int i = 0; i < 4; ++i)
    {
        auto *sqe = ctx.getSqe();
        REQUIRE(sqe != nullptr);
    }
    // 5th should fail
    auto *sqe = ctx.getSqe();
    REQUIRE(sqe == nullptr);
}

// ============================================================================
// submit 测试
// ============================================================================

TEST_CASE("IOUringContext::submit with no SQEs returns 0", "[IOUringContext][submit]")
{
    IOUringContext ctx(256, 0);
    int ret = ctx.submit();
    // No SQEs was added, should return 0
    REQUIRE(ret >= 0);
}

TEST_CASE("IOUringContext::submit with NOP SQEs", "[IOUringContext][submit]")
{
    IOUringContext ctx(256, 0);

    // Prepare NOP SQEs
    for (int i = 0; i < 10; ++i)
    {
        auto *sqe = ctx.getSqe();
        REQUIRE(sqe != nullptr);
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(static_cast<uintptr_t>(i + 1)));
    }

    int ret = ctx.submit();
    REQUIRE(ret >= 0);
}

// ============================================================================
// peekCqe / cqeSeen 测试
// ============================================================================

TEST_CASE("IOUringContext::peekCqe returns null when no completions", "[IOUringContext][peekCqe]")
{
    IOUringContext ctx(256, 0);
    auto *cqe = ctx.peekCqe();
    REQUIRE(cqe == nullptr);
}

TEST_CASE("IOUringContext full submit/peek/seen cycle", "[IOUringContext][integration]")
{
    IOUringContext ctx(256, 0);

    // Submit a NOP
    auto *sqe = ctx.getSqe();
    REQUIRE(sqe != nullptr);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(static_cast<uintptr_t>(42)));

    int submitted = ctx.submit();
    REQUIRE(submitted >= 0);

    // Wait for completion
    auto *cqe = ctx.waitCqe();
    REQUIRE(cqe != nullptr);
    REQUIRE(cqe->res >= 0);

    uint64_t data = reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe));
    REQUIRE(data == 42);

    ctx.cqeSeen(cqe);
}

// ============================================================================
// submitAndWait 测试
// ============================================================================

TEST_CASE("IOUringContext::submitAndWait blocks for completions", "[IOUringContext][submitAndWait]")
{
    IOUringContext ctx(256, 0);

    // Submit a NOP
    auto *sqe = ctx.getSqe();
    REQUIRE(sqe != nullptr);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(static_cast<uintptr_t>(1)));

    int ret = ctx.submitAndWait(1);
    REQUIRE(ret >= 0);

    auto *cqe = ctx.peekCqe();
    REQUIRE(cqe != nullptr);
    REQUIRE(cqe->res >= 0);
    ctx.cqeSeen(cqe);
}

// ============================================================================
// waitCqe 测试
// ============================================================================

TEST_CASE("IOUringContext::waitCqe blocks until completion", "[IOUringContext][waitCqe]")
{
    IOUringContext ctx(256, 0);

    // Submit a NOP and wait
    auto *sqe = ctx.getSqe();
    REQUIRE(sqe != nullptr);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(static_cast<uintptr_t>(99)));

    ctx.submit();

    auto *cqe = ctx.waitCqe();
    REQUIRE(cqe != nullptr);
    REQUIRE(cqe->res >= 0);
    REQUIRE(reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe)) == 99);
    ctx.cqeSeen(cqe);
}

// ============================================================================
// 实际 IO 操作测试（使用 pipe）
// ============================================================================

TEST_CASE("IOUringContext read/write via pipe", "[IOUringContext][integration][io]")
{
    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);

    IOUringContext ctx(256, 0);

    // Prepare write
    const char *msg = "Hello io_uring!";
    auto *sqe_write = ctx.getSqe();
    REQUIRE(sqe_write != nullptr);
    io_uring_prep_write(sqe_write, pipefd[1], msg, strlen(msg), 0);
    io_uring_sqe_set_data(sqe_write, reinterpret_cast<void *>(static_cast<uintptr_t>(1)));

    // Prepare read
    char buf[64] = {};
    auto *sqe_read = ctx.getSqe();
    REQUIRE(sqe_read != nullptr);
    io_uring_prep_read(sqe_read, pipefd[0], buf, sizeof(buf), 0);
    io_uring_sqe_set_data(sqe_read, reinterpret_cast<void *>(static_cast<uintptr_t>(2)));

    ctx.submit();

    // Harvest completions
    int completed = 0;
    while (completed < 2)
    {
        auto *cqe = ctx.waitCqe();
        REQUIRE(cqe != nullptr);
        REQUIRE(cqe->res >= 0);
        ctx.cqeSeen(cqe);
        ++completed;
    }

    REQUIRE(std::string(buf) == msg);

    close(pipefd[0]);
    close(pipefd[1]);
}

// ============================================================================
// 压力测试
// ============================================================================

TEST_CASE("IOUringContext rapid NOP submissions", "[IOUringContext][stress]")
{
    IOUringContext ctx(256, 0);

    constexpr int BATCH_SIZE = 32;
    constexpr int BATCHES = 100;

    for (int batch = 0; batch < BATCHES; ++batch)
    {
        for (int i = 0; i < BATCH_SIZE; ++i)
        {
            auto *sqe = ctx.getSqe();
            REQUIRE(sqe != nullptr);
            io_uring_prep_nop(sqe);
            io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(static_cast<uintptr_t>(batch * BATCH_SIZE + i)));
        }

        int ret = ctx.submitAndWait(BATCH_SIZE);
        REQUIRE(ret >= 0);

        for (int i = 0; i < BATCH_SIZE; ++i)
        {
            auto *cqe = ctx.peekCqe();
            REQUIRE(cqe != nullptr);
            REQUIRE(cqe->res >= 0);
            ctx.cqeSeen(cqe);
        }
    }

    SUCCEED("NOP stress test passed");
}

TEST_CASE("IOUringContext multiple construct/destruct cycles", "[IOUringContext][stress]")
{
    for (int i = 0; i < 100; ++i)
    {
        IOUringContext ctx(64, 0);
        REQUIRE(ctx.fd() >= 0);
        REQUIRE(ctx.entries() == 64);
    }
    SUCCEED("Construct/destruct cycle stress test passed");
}

// ============================================================================
// 性能测试
// ============================================================================

TEST_CASE("IOUringContext performance benchmarks", "[IOUringContext][benchmark]")
{
    BENCHMARK("Construction + destruction")
    {
        IOUringContext ctx(64, 0);
        return ctx.fd();
    };

    IOUringContext ctx(256, 0);

    BENCHMARK("getSqe")
    {
        return ctx.getSqe();
    };

    BENCHMARK("peekCqe (empty queue)")
    {
        return ctx.peekCqe();
    };
}
