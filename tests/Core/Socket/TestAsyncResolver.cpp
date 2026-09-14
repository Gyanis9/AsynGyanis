/** @file TestAsyncResolver.cpp 异步 DNS 解析器用例 */
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncResolver.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>
namespace AsynGyanis::Core
{
    /// 把解析结果写入 result，然后停掉 loop（作为被调度到 loop 上的协程使用）
    static Core::Task<void> doResolve(Core::EventLoop &loop, std::vector<InetAddress> &result,
                                      std::string host, uint16_t port)
    {
        result = co_await AsyncResolver::resolve(loop, host, port);
        loop.stop();
    }

    /// 在专用 EventLoop 上执行 doResolve，阻塞等它完成
    static std::vector<InetAddress> resolveInLoop(std::string_view host, uint16_t port)
    {
        Core::EventLoop loop;
        std::vector<InetAddress> result;
        auto work = doResolve(loop, result, std::string(host), port);
        if (!work.isReady())
            loop.scheduler().schedule(work.handle());
        loop.run();
        return result;
    }

    TEST(AsyncResolver, ReturnsEmptyForEmptyHost)
    {
        EXPECT_TRUE(resolveInLoop("", 80).empty());
    }

    TEST(AsyncResolver, ResolvesLocalhost)
    {
        auto r = resolveInLoop("localhost", 80);
        ASSERT_FALSE(r.empty());
        bool has = false;
        for (auto &a: r)
            if (a.ip() == "127.0.0.1" || a.ip() == "::1") { has = true; break; }
        EXPECT_TRUE(has);
    }

    TEST(AsyncResolver, ResolvesIpTextToSingleAddress)
    {
        auto r = resolveInLoop("127.0.0.1", 8080);
        ASSERT_FALSE(r.empty());
        EXPECT_EQ(r[0].ip(), "127.0.0.1");
        EXPECT_EQ(r[0].port(), 8080);
    }

    TEST(AsyncResolver, ReturnsEmptyForNonexistentHost)
    {
        EXPECT_TRUE(resolveInLoop("i-definitely-do-not-exist-99999999.example", 80).empty());
    }
} // namespace AsynGyanis::Core