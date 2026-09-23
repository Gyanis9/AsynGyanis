// AsyncResolver 单元测试：空主机名与不存在的主机、回环解析、IP 文本与惰性两步调用

#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncResolver.h"
#include <gtest/gtest.h>
#include <cstddef>
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

    /// 两步写法：先建 Task，再丢掉临时串，最后才 await
    static Core::Task<void> doResolveTwoStep(Core::EventLoop &loop, std::vector<InetAddress> &result)
    {
        // 主机名长度超过 SSO 阈值，确保它真的在堆上：只抄视图的话，这一行之后读的就是已释放内存
        Core::Task<std::vector<InetAddress>> pending =
                AsyncResolver::resolve(loop, std::string("no-such-host-with-a-long-name-99999999.example"), 80);
        result = co_await pending;
        loop.stop();
    }

    /**
     * @brief 两步写法（先建 Task、临时串销毁、再 await）下主机名不能被当成悬垂视图读
     * @details 惰性 Task 在**调用**时就把参数拷进帧，而帧体要等 co_await 才跑，中间隔着
     *          「发起到等待」这一步。参数若按视图收下，临时串早已销毁——Debug 构建带 ASan，
     *          这里会当场报释放后使用
     */
    TEST(AsyncResolver, KeepsHostAliveAcrossLazyTwoStepCall)
    {
        Core::EventLoop          loop;
        std::vector<InetAddress> result;
        auto                     work = doResolveTwoStep(loop, result);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        EXPECT_TRUE(result.empty()) << "这个主机名不该解析出任何地址";
    }

    /// 在同一个循环上连着跑 attempts 次回环解析，数下其中有几次空手而归（作为被调度到 loop 上的协程使用）
    static Core::Task<void> doResolveRepeatedly(Core::EventLoop &loop, std::size_t &emptyResultCount, const int attempts)
    {
        for (int attempt = 0; attempt < attempts; ++attempt)
        {
            const std::vector<InetAddress> addresses = co_await AsyncResolver::resolve(loop, "127.0.0.1", 80);
            if (addresses.empty())
            {
                ++emptyResultCount;
            }
        }
        loop.stop();
    }

    /**
     * @brief 每次解析都要把并发名额还得回去：漏还一次，攒到上限之后的解析就永远被拒
     * @details 解析并发上限是进程级的（同时在跑的解析到顶后按「失败返回空列表」收场）。这里连着跑
     *          300 次 —— 超过那个上限 —— 回环解析：只要有任何一次没归还名额，后面的解析就会整片命中
     *          上限而返回空列表。判据取「每一次都解析得出地址」，不测耗时也不赌调度时机
     */
    TEST(AsyncResolver, RepeatedResolutionsNeverGetStarvedByLeakedSlots)
    {
        constexpr int kResolutionAttempts = 300;

        Core::EventLoop loop;
        std::size_t     emptyResultCount{0};
        auto            work = doResolveRepeatedly(loop, emptyResultCount, kResolutionAttempts);
        if (!work.isReady())
        {
            loop.scheduler().schedule(work.handle());
        }
        loop.run();

        EXPECT_EQ(emptyResultCount, 0U)
            << "回环地址解析不该空手而归：空结果说明并发名额没归还，攒到上限后解析会永久被拒";
    }
} // namespace AsynGyanis::Core