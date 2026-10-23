// AsyncResolver 单元测试：空主机名与不存在的主机、回环解析、IP 文本与惰性两步调用

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "Base/Exception/Exception.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncResolver.h"
#include "AssertionSupport.h"
#include "CoreTestSupport.h"
namespace AsynGyanis::Core
{
    /// 把解析结果写入 result，然后停掉 loop（作为被调度到 loop 上的协程使用）
    static Core::Task<void> doResolve(Core::EventLoop &loop, std::vector<InetAddress> &result, std::string host, uint16_t port)
    {
        result = co_await AsyncResolver::resolve(loop, host, port);
        loop.stop();
    }

    /// 在专用 EventLoop 上执行 doResolve，阻塞等它完成
    static std::vector<InetAddress> resolveInLoop(std::string_view host, uint16_t port)
    {
        Core::EventLoop          loop;
        std::vector<InetAddress> result;
        auto                     work = doResolve(loop, result, std::string(host), port);
        if (!work.isReady())
            loop.scheduler().schedule(work.handle());
        loop.run();
        return result;
    }

    /// 带时限版本的同形驱动：结果写入 result 后停 loop
    static Core::Task<void> doResolveWithTimeout(Core::EventLoop &loop, std::vector<InetAddress> &result, std::string host, uint16_t port, std::chrono::milliseconds timeout)
    {
        result = co_await AsyncResolver::resolve(loop, host, port, timeout);
        loop.stop();
    }

    /// 在专用 EventLoop 上执行带时限解析，阻塞等它完成
    static std::vector<InetAddress> resolveInLoopWithTimeout(std::string_view host, uint16_t port, std::chrono::milliseconds timeout)
    {
        Core::EventLoop          loop;
        std::vector<InetAddress> result;
        auto                     work = doResolveWithTimeout(loop, result, std::string(host), port, timeout);
        if (!work.isReady())
            loop.scheduler().schedule(work.handle());
        loop.run();
        return result;
    }

    /// 解析一定失败的形状：DNS 标签上限 63 字节，200 个 'a' 在问网络之前就会被 getaddrinfo 拒掉。
    /// 不用 `.example` 那类保留域：本机或 CI 的解析器把它们答成什么都可能，断言会跟着机器变。
    static std::string overlongInvalidHostName()
    {
        return std::string(200U, 'a');
    }

    TEST(AsyncResolver, ReturnsEmptyForEmptyHost)
    {
        EXPECT_TRUE(resolveInLoop("", 80).empty());
    }

    /**
     * @brief 带时限的重载在字面量快路径上不误伤：1 毫秒预算也照样立刻拿到地址
     * @details 这条钉的是「定时器不能抢在已经就绪的结果前面」；真正的解析卡死无法在单测里
     *          稳定制造，超时路径靠 HttpClient 的请求时限做端到端兜底
     */
    TEST(AsyncResolver, TimedOverloadKeepsTheLiteralFastPath)
    {
        const std::vector<InetAddress> resolved = resolveInLoopWithTimeout("127.0.0.1", 8080, std::chrono::milliseconds{1});
        ASSERT_EQ(resolved.size(), 1U) << "字面量被 1 毫秒的时限误伤";
        EXPECT_EQ(resolved[0].ip(), "127.0.0.1");
    }

    TEST(AsyncResolver, ResolvesLocalhost)
    {
        auto r = resolveInLoop("localhost", 80);
        ASSERT_FALSE(r.empty());
        bool has = false;
        for (auto &a: r)
            if (a.ip() == "127.0.0.1" || a.ip() == "::1")
            {
                has = true;
                break;
            }
        EXPECT_TRUE(has);
    }

    TEST(AsyncResolver, ResolvesIpTextToSingleAddress)
    {
        auto r = resolveInLoop("127.0.0.1", 8080);
        ASSERT_FALSE(r.empty());
        EXPECT_EQ(r[0].ip(), "127.0.0.1");
        EXPECT_EQ(r[0].port(), 8080);
    }

    /**
     * @brief 钉住：IPv6 字面量不受 AI_ADDRCONFIG 那层筛选的摆布
     * @details hints 带 AI_ADDRCONFIG 时，glibc 按「本机有没有配到该族的非回环地址」筛掉结果——
     *          只有 ::1 可用的容器里 getaddrinfo("::1") 直接 EAI_ADDRFAMILY（实测），调用方写在脸上
     *          的地址反倒解析不出来，出站连 [::1]:端口 就永远失败。字面量不需要问任何人，就不该看
     *          这台机器的接口配置。配了非回环 IPv6 的机器上旧写法也能过，因此这条的红要在容器里读。
     */
    TEST(AsyncResolver, ResolvesIpv6LiteralWithoutAddressConfigFiltering)
    {
        const std::vector<InetAddress> resolved = resolveInLoop("::1", 8080);
        ASSERT_EQ(resolved.size(), 1U) << "IPv6 字面量没解析出来：它被当成名字查询交给了 AI_ADDRCONFIG";
        EXPECT_EQ(resolved[0].ip(), "::1");
        EXPECT_EQ(resolved[0].port(), 8080);
    }

    /**
     * @brief 钉住：名字解析按「主机+端口」缓存，且键对大小写不敏感
     * @details 判据取 stats() 的增量而不是耗时——命中与否是可数的，而计时在共享机器上量不出东西
     *          （同进程的其它用例还会一起动计数，所以只看差值）。四条查询三种结局：第一次必 miss，
     *          第二次同键必命中，换大小写仍算同一个键（域名大小写无关，不折叠就会各占一格），
     *          换端口必 miss（端口是键的一部分——把不同端口答成同一次命中是最坏的缓存）。
     */
    TEST(AsyncResolver, CachesNameLookupsPerHostAndPort)
    {
        const auto                     before    = AsyncResolver::stats();
        const std::vector<InetAddress> first     = resolveInLoop("localhost", 8080);
        const std::vector<InetAddress> second    = resolveInLoop("localhost", 8080);
        const std::vector<InetAddress> folded    = resolveInLoop("LOCALHOST", 8080);
        const std::vector<InetAddress> otherPort = resolveInLoop("localhost", 8081);
        const auto                     after     = AsyncResolver::stats();

        ASSERT_FALSE(first.empty()) << "localhost 都没解析出来，后面的判据都是空的";
        EXPECT_EQ(first.size(), second.size());
        EXPECT_EQ(first.size(), folded.size());
        EXPECT_EQ(first.size(), otherPort.size());
        EXPECT_EQ(after.lookupCount - before.lookupCount, 4U) << "四次名字查询没有都算进读数";
        EXPECT_EQ(after.cacheHitCount - before.cacheHitCount, 2U) << "第二次与换大小写那次该走缓存，实测命中增量=" << after.cacheHitCount - before.cacheHitCount;
    }

    TEST(AsyncResolver, ReturnsEmptyForMalformedHost)
    {
        EXPECT_TRUE(resolveInLoop(overlongInvalidHostName(), 80).empty());
    }

    /**
     * @brief 钉住：解析失败不写缓存，同一次抖动不会被放大成 60 秒连不上
     * @details 缓存只存非空结果（见 AsyncResolver 的类注释）。判据取 stats() 的增量：两次同一主机
     *          的失败查询都要真的走进解析流程，缓存命中数不能增加；把「空结果也缓存」改回去时，
     *          第二次会记成命中，这里当场红。
     */
    TEST(AsyncResolver, FailedLookupsAreNotCached)
    {
        const std::string host   = overlongInvalidHostName();
        const auto        before = AsyncResolver::stats();
        EXPECT_TRUE(resolveInLoop(host, 80).empty());
        EXPECT_TRUE(resolveInLoop(host, 80).empty());
        const auto after = AsyncResolver::stats();
        EXPECT_EQ(after.lookupCount - before.lookupCount, 2U) << "两次失败查询没有各算一次解析";
        EXPECT_EQ(after.cacheHitCount - before.cacheHitCount, 0U) << "失败结果被缓存：一次临时故障会粘住 60 秒";
    }

    /**
     * @brief 钉住：缓存条目按配置的有效期过期，过期后必须重新解析
     * @details 默认 60 秒的过期路径没法在单测里真等；把有效期压到 200 毫秒、用注入 lookup
     *          返回固定地址，再用有界轮询等到「下一次查询真的走进解析流程」。判据取 stats()
     *          的增量：第一次是解析，第二次是命中，过期后必须再解析一次。
     */
    TEST(AsyncResolver, CacheEntriesExpireAfterTheConfiguredTimeToLive)
    {
        struct ResolverSeamReset
        {
            ~ResolverSeamReset()
            {
                AsyncResolver::clearLookupFunction();
                AsyncResolver::setCacheTimeToLive(std::chrono::seconds{60});
            }
        } reset;

        constexpr std::string_view kHost = "cache-ttl-expiry.test";
        // stats().lookupCount 统计的是「查询次数」（命中与否都算），真实的后台查找次数由注入实现自己数
        const auto lookupCalls = std::make_shared<std::atomic<std::size_t>>(0);
        AsyncResolver::setLookupFunction(
                [lookupCalls](const std::string &, const std::uint16_t)
                {
                    lookupCalls->fetch_add(1, std::memory_order_relaxed);
                    return std::vector<InetAddress>{InetAddress("127.0.0.1", 1234)};
                });
        AsyncResolver::setCacheTimeToLive(std::chrono::milliseconds{200});
        EXPECT_EQ(AsyncResolver::cacheTimeToLive(), std::chrono::milliseconds{200});
        AsynGyanis::TestSupport::expectFailure<Base::InvalidArgumentException>([] { AsyncResolver::setCacheTimeToLive(std::chrono::milliseconds::zero()); }, "缓存有效期必须是正数",
                                                                               "零有效期");
        AsynGyanis::TestSupport::expectFailure<Base::InvalidArgumentException>([] { AsyncResolver::setCacheTimeToLive(std::chrono::milliseconds{-1}); }, "缓存有效期必须是正数",
                                                                               "负有效期");
        EXPECT_EQ(AsyncResolver::cacheTimeToLive(), std::chrono::milliseconds{200}) << "非法设置不该改变当前值";

        const auto before = AsyncResolver::stats();
        ASSERT_EQ(resolveInLoop(kHost, 80).size(), 1U) << "注入 lookup 没有交出地址";
        ASSERT_EQ(resolveInLoop(kHost, 80).size(), 1U);
        const auto afterHit = AsyncResolver::stats();
        EXPECT_EQ(afterHit.lookupCount - before.lookupCount, 2U) << "两次同键查询没有都算进查询读数";
        EXPECT_EQ(afterHit.cacheHitCount - before.cacheHitCount, 1U) << "第二次同键查询没有走缓存";
        ASSERT_EQ(lookupCalls->load(std::memory_order_relaxed), 1U) << "第一次查询没有真的调用注入 lookup";

        // 窗口型：有界轮询等 TTL 过期，判据是后台 lookup 被第二次真正调用
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline && lookupCalls->load(std::memory_order_relaxed) < 2U)
        {
            static_cast<void>(resolveInLoop(kHost, 80));
            if (lookupCalls->load(std::memory_order_relaxed) < 2U)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }
        EXPECT_EQ(lookupCalls->load(std::memory_order_relaxed), 2U) << "过了配置的有效期缓存仍然命中：过期路径没有生效";
    }

    /**
     * @brief 钉住：lookup 真的卡住时，带时限的解析按时返回空，后台线程结束后名额归还
     * @details 真实 getaddrinfo 在本机与 CI 上都会很快返回，制造不出「解析卡住」的形状；
     *          这条用例用一个可控 lookup 阻塞到测试放行，判据只看时间窗与活动计数。
     */
    TEST(AsyncResolver, TimesOutWhileLookupIsBlockedAndReleasesTheSlot)
    {
        auto                           release  = std::make_shared<std::promise<void>>();
        const std::shared_future<void> released = release->get_future().share();
        AsyncResolver::setLookupFunction(
                [released](const std::string &, const std::uint16_t)
                {
                    released.wait();
                    return std::vector<InetAddress>{};
                });

        struct LookupReset
        {
            ~LookupReset()
            {
                AsyncResolver::clearLookupFunction();
            }
        } reset;

        const auto started = std::chrono::steady_clock::now();
        const auto result  = resolveInLoopWithTimeout("blocked-lookup.test", 80, std::chrono::milliseconds{50});
        const auto elapsed = std::chrono::steady_clock::now() - started;

        EXPECT_TRUE(result.empty()) << "阻塞的 lookup 不该交出地址";
        EXPECT_GE(elapsed, std::chrono::milliseconds{40}) << "比时限还早返回，说明没有真的等";
        EXPECT_LT(elapsed, std::chrono::milliseconds{500}) << "解析卡住把调用方拖过了自己的时限";

        release->set_value();
        EXPECT_TRUE(TestSupport::waitForCondition([] { return AsyncResolver::activeLookupCount() == 0; }, std::chrono::seconds{2})) << "后台解析结束后活动名额没有归还";
    }

    /// 两步写法：先建 Task，再丢掉临时串，最后才 await
    static Core::Task<void> doResolveTwoStep(Core::EventLoop &loop, std::vector<InetAddress> &result)
    {
        // 主机名长度超过 SSO 阈值，确保它真的在堆上：只抄视图的话，这一行之后读的就是已释放内存
        Core::Task<std::vector<InetAddress>> pending = AsyncResolver::resolve(loop, overlongInvalidHostName(), 80);
        result                                       = co_await pending;
        loop.stop();
    }

    /**
     * @brief 两步写法（先建 Task、临时串销毁、再 await）下主机名不能被当成悬垂视图读
     * @details 惰性 Task 在调用时就把参数拷进帧，而帧体要等 co_await 才跑，中间隔着
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

        EXPECT_EQ(emptyResultCount, 0U) << "回环地址解析不该空手而归：空结果说明并发名额没归还，攒到上限后解析会永久被拒";
    }
} // namespace AsynGyanis::Core
