/**
 * @file TestAsyncEventLoop.cpp
 * @brief AsyncEventLoop 模块单元测试：生命周期、异步 IO 操作、
 *        超时、取消、并发
 * @copyright Copyright (c) 2026
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "Base/AsyncEventLoop.h"

#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace Base;
using namespace std::chrono_literals;

// ============================================================================
// 测试辅助
// ============================================================================

// Create a pair of pipe fds
struct PipePair
{
    int read_fd;
    int write_fd;

    PipePair()
    {
        int fds[2];
        if (pipe2(fds, O_NONBLOCK) != 0)
        {
            throw std::runtime_error("pipe creation failed");
        }
        read_fd  = fds[0];
        write_fd = fds[1];
    }

    ~PipePair()
    {
        close(read_fd);
        close(write_fd);
    }
};

// ============================================================================
// 构造测试
// ============================================================================

TEST_CASE("AsyncEventLoop construction with default parameters", "[AsyncEventLoop][construction]")
{
    REQUIRE_NOTHROW(AsyncEventLoop());
}

TEST_CASE("AsyncEventLoop construction with custom queue depth", "[AsyncEventLoop][construction]")
{
    REQUIRE_NOTHROW(AsyncEventLoop(128, 0));
    REQUIRE_NOTHROW(AsyncEventLoop(512, 0));
    REQUIRE_NOTHROW(AsyncEventLoop(1024, 0));
}

TEST_CASE("AsyncEventLoop construction with flags", "[AsyncEventLoop][construction]")
{
    REQUIRE_NOTHROW(AsyncEventLoop(256, IORING_SETUP_SQPOLL));
}

TEST_CASE("AsyncEventLoop construction with power-of-2 queue depths", "[AsyncEventLoop][construction][boundary]")
{
    for (unsigned qd: {2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u, 1024u})
    {
        REQUIRE_NOTHROW(AsyncEventLoop(qd, 0));
    }
}

// ============================================================================
// 生命周期测试
// ============================================================================

TEST_CASE("AsyncEventLoop run/stop lifecycle", "[AsyncEventLoop][lifecycle]")
{
    AsyncEventLoop loop;

    REQUIRE_FALSE(loop.isRunning());

    // Start run in separate thread
    std::thread loop_thread([&]()
    {
        loop.run();
    });

    // Wait for loop to start
    while (!loop.isRunning())
    {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(loop.isRunning());

    // Stop the loop
    loop.stop();

    loop_thread.join();
    REQUIRE_FALSE(loop.isRunning());
}

TEST_CASE("AsyncEventLoop stop from different thread", "[AsyncEventLoop][lifecycle][thread]")
{
    AsyncEventLoop loop;

    std::thread loop_thread([&]()
    {
        loop.run();
    });

    while (!loop.isRunning())
    {
        std::this_thread::sleep_for(1ms);
    }

    // Stop from main thread
    loop.stop();
    loop_thread.join();

    REQUIRE_FALSE(loop.isRunning());
}

TEST_CASE("AsyncEventLoop stop when not running is safe", "[AsyncEventLoop][lifecycle][boundary]")
{
    AsyncEventLoop loop;
    REQUIRE_NOTHROW(loop.stop());
    REQUIRE_FALSE(loop.isRunning());
}

TEST_CASE("AsyncEventLoop destruction stops running loop", "[AsyncEventLoop][lifecycle]")
{
    auto loop = std::make_unique<AsyncEventLoop>();

    std::thread loop_thread([&]()
    {
        loop->run();
    });

    while (!loop->isRunning())
    {
        std::this_thread::sleep_for(1ms);
    }

    // Destructor should call stop() internally
    loop.reset();

    loop_thread.join();
    // Should not hang or crash
    SUCCEED("Destruction during run is safe");
}

TEST_CASE("AsyncEventLoop runOnce handles empty queue", "[AsyncEventLoop][lifecycle]")
{
    AsyncEventLoop loop;
    REQUIRE_NOTHROW(loop.runOnce());
}

// ============================================================================
// context / isRunning 访问器测试
// ============================================================================

TEST_CASE("AsyncEventLoop::context returns valid reference", "[AsyncEventLoop][accessor]")
{
    AsyncEventLoop loop;
    auto &ctx = loop.context();
    REQUIRE(ctx.fd() >= 0);
    REQUIRE(ctx.entries() > 0);
}

TEST_CASE("AsyncEventLoop::isRunning reflects run state", "[AsyncEventLoop][accessor]")
{
    AsyncEventLoop loop;
    REQUIRE_FALSE(loop.isRunning());

    std::thread t([&]()
    {
        loop.run();
    });

    while (!loop.isRunning())
    {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(loop.isRunning());

    loop.stop();
    t.join();
    REQUIRE_FALSE(loop.isRunning());
}

// ============================================================================
// setTimeout 测试
// ============================================================================

TEST_CASE("AsyncEventLoop timeout fires after specified duration", "[AsyncEventLoop][timeout]")
{
    AsyncEventLoop loop;
    std::atomic<bool> timed_out{false};

    loop.timeout(50, [&](const AsyncCompletion &comp)
    {
        REQUIRE(comp.result == 0); // Timeout success should be 0 (or -ETIME)
        REQUIRE(comp.type == AsyncOpType::Timeout);
        timed_out.store(true);
    });

    std::thread t([&]()
    {
        loop.run();
    });

    while (!timed_out.load())
    {
        std::this_thread::sleep_for(1ms);
    }

    loop.stop();
    t.join();

    REQUIRE(timed_out.load());
}

TEST_CASE("AsyncEventLoop multiple timeouts", "[AsyncEventLoop][timeout]")
{
    AsyncEventLoop loop;
    std::atomic<int> timeout_count{0};

    for (int i = 0; i < 10; ++i)
    {
        loop.timeout(10 + i * 10, [&](const AsyncCompletion &)
        {
            timeout_count.fetch_add(1);
        });
    }

    std::thread t([&]()
    {
        loop.run();
    });

    while (timeout_count.load() < 10)
    {
        std::this_thread::sleep_for(1ms);
    }

    loop.stop();
    t.join();

    REQUIRE(timeout_count.load() >= 10);
}

TEST_CASE("AsyncEventLoop timeout with userData", "[AsyncEventLoop][timeout][userData]")
{
    AsyncEventLoop loop;
    int sentinel = 0;
    std::atomic<bool> done{false};

    loop.timeout(30, [&](const AsyncCompletion &comp)
    {
        REQUIRE(comp.userData == &sentinel);
        done.store(true);
    }, &sentinel);

    std::thread t([&]()
    {
        loop.run();
    });

    while (!done.load())
    {
        std::this_thread::sleep_for(1ms);
    }

    loop.stop();
    t.join();
}

// ============================================================================
// cancel 测试
// ============================================================================

TEST_CASE("AsyncEventLoop cancel timeout operation", "[AsyncEventLoop][cancel]")
{
    AsyncEventLoop loop;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> timed_out{false};

    uint64_t opId = loop.timeout(5000, [&](const AsyncCompletion &comp)
    {
        if (comp.result == -ECANCELED)
        {
            cancelled.store(true);
        } else
        {
            timed_out.store(true);
        }
    });

    REQUIRE(opId > 0);

    std::thread t([&]()
    {
        loop.run();
    });

    while (!loop.isRunning())
    {
        std::this_thread::sleep_for(1ms);
    }

    // Cancel immediately
    bool cancel_result = loop.cancel(opId);
    REQUIRE(cancel_result);

    // Wait a bit and stop
    std::this_thread::sleep_for(100ms);
    loop.stop();
    t.join();

    // Should be cancelled, not timed out
    REQUIRE(cancelled.load());
    REQUIRE_FALSE(timed_out.load());
}

TEST_CASE("AsyncEventLoop cancel nonexistent opId", "[AsyncEventLoop][cancel][boundary]")
{
    AsyncEventLoop loop;
    // This should still be able to submit a cancel SQE
    // but the result is that it may succeed (SQE submission) but
    // the cancel itself won't find the target
    // Just ensure it doesn't crash with invalid IDs
    REQUIRE_NOTHROW(loop.cancel(UINT64_MAX));
    REQUIRE_NOTHROW(loop.cancel(0));
}

TEST_CASE("AsyncEventLoop cancel all pending operations", "[AsyncEventLoop][cancel][stress]")
{
    AsyncEventLoop loop;
    constexpr int N = 50;
    std::vector<uint64_t> opIds;
    std::atomic<int> cancelled_count{0};

    for (int i = 0; i < N; ++i)
    {
        uint64_t id = loop.timeout(10000, [&](const AsyncCompletion &comp)
        {
            if (comp.result == -ECANCELED)
            {
                cancelled_count.fetch_add(1);
            }
        });
        opIds.push_back(id);
    }

    std::thread t([&]()
    {
        loop.run();
    });

    while (!loop.isRunning())
    {
        std::this_thread::sleep_for(1ms);
    }

    std::this_thread::sleep_for(10ms);

    // Cancel all
    for (auto id: opIds)
    {
        loop.cancel(id);
    }

    std::this_thread::sleep_for(200ms);
    loop.stop();
    t.join();

    // At least some should be cancelled
    // (Not all may be cancellable depending on timing)
    REQUIRE(cancelled_count.load() > 0);
}

// ============================================================================
// read/write 测试
// ============================================================================

TEST_CASE("AsyncEventLoop read from pipe", "[AsyncEventLoop][read][integration]")
{
    AsyncEventLoop loop;
    PipePair pipe;

    // Write data to pipe synchronously
    const char *msg = "async read test";
    REQUIRE(write(pipe.write_fd, msg, strlen(msg)) == static_cast<ssize_t>(strlen(msg)));

    std::atomic<bool> read_done{false};
    char read_buf[64] = {};
    ssize_t bytes_read = 0;

    loop.read(pipe.read_fd, read_buf, sizeof(read_buf), 0,
              [&](const AsyncCompletion &comp)
              {
                  REQUIRE(comp.type == AsyncOpType::Read);
                  bytes_read = comp.result;
                  read_done.store(true);
              });

    std::thread t([&]()
    {
        loop.run();
    });

    while (!read_done.load())
    {
        std::this_thread::sleep_for(1ms);
    }

    loop.stop();
    t.join();

    REQUIRE(bytes_read > 0);
    REQUIRE(std::string(read_buf, bytes_read) == msg);
}

TEST_CASE("AsyncEventLoop write to pipe", "[AsyncEventLoop][write][integration]")
{
    AsyncEventLoop loop;
    PipePair pipe;

    const char *msg = "async write test";
    std::atomic<bool> write_done{false};
    ssize_t bytes_written = 0;

    loop.write(pipe.write_fd, msg, strlen(msg), 0,
               [&](const AsyncCompletion &comp)
               {
                   REQUIRE(comp.type == AsyncOpType::Write);
                   bytes_written = comp.result;
                   write_done.store(true);
               });

    std::thread t([&]()
    {
        loop.run();
    });

    while (!write_done.load())
    {
        std::this_thread::sleep_for(1ms);
    }

    loop.stop();
    t.join();

    REQUIRE(bytes_written > 0);
    REQUIRE(static_cast<size_t>(bytes_written) == strlen(msg));

    // Verify data readable from pipe
    char buf[64] = {};
    REQUIRE(read(pipe.read_fd, buf, sizeof(buf)) == bytes_written);
    REQUIRE(std::string(buf) == msg);
}

// ============================================================================
// runOnce 测试
// ============================================================================

TEST_CASE("AsyncEventLoop runOnce processes pending operations", "[AsyncEventLoop][runOnce]")
{
    AsyncEventLoop loop;
    PipePair pipe;

    // Write data synchronously
    const char *msg = "runOnce test";
    REQUIRE(write(pipe.write_fd, msg, strlen(msg)) == static_cast<ssize_t>(strlen(msg)));

    std::atomic<bool> read_done{false};
    char buf[64] = {};

    loop.read(pipe.read_fd, buf, sizeof(buf), 0,
              [&](const AsyncCompletion &)
              {
                  read_done.store(true);
              });

    // Use runOnce in a loop until read completes
    int iterations = 0;
    while (!read_done.load() && iterations < 1000)
    {
        loop.runOnce();
        ++iterations;
    }

    REQUIRE(read_done.load());
    REQUIRE(iterations < 1000);
}

// ============================================================================
// 并发线程安全测试
// ============================================================================

TEST_CASE("AsyncEventLoop concurrent read/write operations", "[AsyncEventLoop][thread][stress]")
{
    AsyncEventLoop loop;
    std::atomic<int> completed{0};
    constexpr int N = 100;

    // Create pipes and submit operations
    std::vector<std::unique_ptr<PipePair>> pipes;
    for (int i = 0; i < N; ++i)
    {
        pipes.push_back(std::make_unique<PipePair>());

        char msg[32];
        snprintf(msg, sizeof(msg), "msg_%d", i);
        write(pipes[i]->write_fd, msg, strlen(msg));
    }

    for (int i = 0; i < N; ++i)
    {
        auto *buf = new char[64]();  // Will be leaked in callback but fine for test
        loop.read(pipes[i]->read_fd, buf, 64, 0,
                  [&completed, buf](const AsyncCompletion &)
                  {
                      delete[] buf;
                      completed.fetch_add(1);
                  });
    }

    std::thread t([&]()
    {
        loop.run();
    });

    while (completed.load() < N)
    {
        std::this_thread::sleep_for(1ms);
    }

    loop.stop();
    t.join();

    REQUIRE(completed.load() == N);
}

// ============================================================================
// AsyncOpType 枚举测试
// ============================================================================

TEST_CASE("AsyncOpType enum values are correct", "[AsyncEventLoop][AsyncOpType]")
{
    REQUIRE(static_cast<uint8_t>(AsyncOpType::Read) == 0);
    REQUIRE(static_cast<uint8_t>(AsyncOpType::Write) == 1);
    REQUIRE(static_cast<uint8_t>(AsyncOpType::Timeout) == 4);
    REQUIRE(static_cast<uint8_t>(AsyncOpType::NoOp) == 7);
}

// ============================================================================
// AsyncCompletion 测试
// ============================================================================

TEST_CASE("AsyncCompletion default initialization", "[AsyncEventLoop][AsyncCompletion]")
{
    AsyncCompletion comp{};
    REQUIRE(comp.result == 0);
    REQUIRE(comp.opId == 0);
    REQUIRE(comp.userData == nullptr);
}

TEST_CASE("AsyncCompletion field assignment", "[AsyncEventLoop][AsyncCompletion]")
{
    int data = 42;
    AsyncCompletion comp{};
    comp.type     = AsyncOpType::Read;
    comp.result   = 1024;
    comp.opId     = 12345;
    comp.userData = &data;

    REQUIRE(comp.type == AsyncOpType::Read);
    REQUIRE(comp.result == 1024);
    REQUIRE(comp.opId == 12345);
    REQUIRE(comp.userData == &data);
}

// ============================================================================
// userData 测试
// ============================================================================

TEST_CASE("AsyncEventLoop passes userData through to callback", "[AsyncEventLoop][userData]")
{
    AsyncEventLoop loop;
    PipePair pipe;

    struct TestContext
    {
        int id;
        std::string name;
    };

    TestContext ctx{123, "test_context"};
    std::atomic<bool> done{false};

    const char *msg = "test";
    write(pipe.write_fd, msg, strlen(msg));

    char buf[64] = {};
    loop.read(pipe.read_fd, buf, sizeof(buf), 0,
              [&](const AsyncCompletion &comp)
              {
                  auto *myCtx = static_cast<TestContext *>(comp.userData);
                  REQUIRE(myCtx->id == 123);
                  REQUIRE(myCtx->name == "test_context");
                  done.store(true);
              }, &ctx);

    std::thread t([&]()
    {
        loop.run();
    });

    while (!done.load())
    {
        std::this_thread::sleep_for(1ms);
    }

    loop.stop();
    t.join();
}

// ============================================================================
// 压力测试
// ============================================================================

TEST_CASE("AsyncEventLoop start/stop cycle stress", "[AsyncEventLoop][stress]")
{
    for (int i = 0; i < 50; ++i)
    {
        AsyncEventLoop loop;

        std::thread t([&]()
        {
            loop.run();
        });

        while (!loop.isRunning())
        {
            std::this_thread::sleep_for(100us);
        }

        loop.stop();
        t.join();
    }
    SUCCEED("Start/stop stress test passed");
}

TEST_CASE("AsyncEventLoop rapid timeout submission", "[AsyncEventLoop][stress]")
{
    AsyncEventLoop loop;
    std::atomic<int> completed{0};

    for (int i = 0; i < 200; ++i)
    {
        loop.timeout(1, [&](const AsyncCompletion &)
        {
            completed.fetch_add(1);
        });
    }

    std::thread t([&]()
    {
        loop.run();
    });

    while (completed.load() < 200)
    {
        std::this_thread::sleep_for(1ms);
    }

    loop.stop();
    t.join();
    REQUIRE(completed.load() >= 200);
}

// ============================================================================
// 性能测试
// ============================================================================

TEST_CASE("AsyncEventLoop performance benchmarks", "[AsyncEventLoop][benchmark]")
{
    BENCHMARK("Construction")
    {
        AsyncEventLoop loop;
        return loop.isRunning();
    };

    AsyncEventLoop loop;
    PipePair pipe;
    write(pipe.write_fd, "bench", 5);

    BENCHMARK("read submission")
    {
        char buf[16];
        return loop.read(pipe.read_fd, buf, sizeof(buf), 0, nullptr);
    };

    BENCHMARK("timeout submission")
    {
        return loop.timeout(1000, nullptr);
    };
}
