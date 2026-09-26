// TestHttpServerLimits.cpp —— HTTP 连接级限额（HttpServerLimits + TcpServer 空闲清扫）的系统级覆盖：
//   一. 空闲超时：连上却不发任何字节的连接，在 idleTimeout + 清扫节拍内被服务端关闭；
//   二. 读超时：只发半条请求后静止，按 readTimeout 收口（idleTimeout 故意设得很长，证明起作用的是读超时）；
//   三. 单连接请求上限：达到上限的那条响应带 Connection: close，其后连接关闭、新请求不再被服务；
//   四. 写超时：**本机无法覆盖**——回环上把接收缓冲压到 8 KiB、响应体给到 8 MiB，整份响应仍在一次
//       WSASend 里发完（Windows 回环不制造部分写，此前实测过），构造不出确定性的「写阻塞」。
//       机制（发送前按 writeTimeout 刷新截止时间）仍在实现里，端到端验证只能在真实网络上做。
//   五. 拒绝面：清扫节拍设为 0 时，同一份空闲连接不再被超时收口。
//   六. 优雅关闭（TcpServer::drain）：在途请求被等完才收口（客户端拿到完整响应），
//      处理时间远超期限时则在期限附近返回并强关连接。
// 用例全部走真实回环套接字（清扫协程在 TcpServer 内部），不依赖任何外部服务。
// 回环夹具（RunningHttpServerFixture / LoopbackClient）在 HttpTestSupport.h 中，与观测性用例共用一份。

#include "Net/Http/HttpServer.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/InetAddress.h"
#include "HttpTestSupport.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/Router.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Platform/System/PlatformError.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace AsynGyanis::Net
{
    // 回环夹具（真实 HttpServer + 裸 socket 客户端）集中在本头文件里，观测性用例
    // 也复用它，避免出现第二套「起服务器、发请求、读响应」的实现
    using namespace HttpTestSupport;

    TEST(HttpServerLimits, IdleKeepAliveConnectionIsClosedAfterIdleTimeout)
    {
        // 空闲超时：连接建立后一个字节都不发，服务端必须按 idleTimeout 收口。
        // 读超时与写超时故意设得很长，关掉它们才能证明收口来自空闲容忍度
        HttpServerLimits limits;
        limits.idleTimeout  = std::chrono::milliseconds{400};
        limits.readTimeout  = std::chrono::seconds{10};
        limits.writeTimeout = std::chrono::seconds{10};

        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{50});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";
        EXPECT_FALSE(fixture.startThrew());

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        // 反向对照：空闲容忍度之内不该被提前收口（否则下面的断言可能只是「连上就被关」）
        std::string receivedText;
        EXPECT_FALSE(client.waitForClosure(receivedText, std::chrono::milliseconds{200})) << "连接在空闲容忍度之内就被关闭：说明截止时间被设成了立即到期";

        EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout)) << "空闲连接未被清扫协程收口：上界 kWaitTimeout（idleTimeout 400ms + 清扫节拍 50ms）";
        EXPECT_TRUE(receivedText.empty()) << "服务端在空闲连接上发了不该发的字节";

        // 会话随之退出：关闭动作确实回到了连接管理器，而不是只关了描述符
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";
    }

    TEST(HttpServerLimits, HalfSentRequestIsClosedAfterReadTimeout)
    {
        // 读超时：只发半条请求（缺收尾空行）后静止，必须按 readTimeout 收口。
        // idleTimeout 设成 10 秒：断言窗口内它不可能触发，收口只可能来自读超时
        HttpServerLimits limits;
        limits.idleTimeout  = std::chrono::seconds{10};
        limits.readTimeout  = std::chrono::milliseconds{300};
        limits.writeTimeout = std::chrono::seconds{10};

        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{30});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(halfRequestText(), kWaitTimeout)) << "半条请求未能写入";

        // 反向对照：读超时之内不该被提前收口
        std::string receivedText;
        EXPECT_FALSE(client.waitForClosure(receivedText, std::chrono::milliseconds{100})) << "半条请求在读超时之内就被关闭";

        EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout)) << "半条请求未被读超时收口：上界 kWaitTimeout（readTimeout 300ms + 清扫节拍 30ms）";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";
    }

    /**
     * @brief 处理器运行超过 readTimeout（但没超过 writeTimeout）时，连接不被清扫掐掉
     * @details 处理器相位既不读也不写，此前沿用的是最后一次读刷出的 readTimeout——慢处理器会被
     *          当成空闲连接收口，客户端拿不到任何响应（连 504 都没有）。现在该相位按 writeTimeout
     *          （响应产出预算）计时：要放宽处理器时限就调它
     */
    TEST(HttpServerLimits, SlowHandlerIsNotClosedWhileItRuns)
    {
        HttpServerLimits limits;
        limits.idleTimeout  = std::chrono::seconds{10};
        limits.readTimeout  = std::chrono::milliseconds{200}; // 处理器要跑得比它长
        limits.writeTimeout = std::chrono::seconds{5};        // 响应产出预算

        std::atomic<bool> handlerStarted{false};
        SlowRouteOptions  slowRoute;
        slowRoute.processingTime = std::chrono::milliseconds{700}; // 3.5 倍 readTimeout，远小于 writeTimeout
        slowRoute.handlerStarted = &handlerStarted;

        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{30}, slowRoute);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";
        EXPECT_FALSE(fixture.startThrew());

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /slow HTTP/1.1"), kWaitTimeout)) << "慢请求未能写入";
        ASSERT_TRUE(waitForCondition([&handlerStarted] { return handlerStarted.load(std::memory_order_acquire); }, kWaitTimeout)) << "慢路由未在时限内开始处理";

        std::string receivedText;
        EXPECT_TRUE(client.waitForText(receivedText, "served-slow", kWaitTimeout)) << "处理器运行期间连接被清扫掐掉：响应没到达（处理耗时 700ms，readTimeout 只有 200ms）";
    }

    /**
     * @brief 处理器运行超过 writeTimeout 时照样被收口——预算存在，不只是「忙就不掐」
     */
    TEST(HttpServerLimits, SlowHandlerIsClosedWhenItExceedsWriteTimeout)
    {
        HttpServerLimits limits;
        limits.idleTimeout  = std::chrono::seconds{10};
        limits.readTimeout  = std::chrono::seconds{10};       // 保证收口不来自读超时
        limits.writeTimeout = std::chrono::milliseconds{300}; // 处理器预算

        std::atomic<bool> handlerStarted{false};
        SlowRouteOptions  slowRoute;
        slowRoute.processingTime = std::chrono::milliseconds{700}; // 超预算两倍
        slowRoute.handlerStarted = &handlerStarted;

        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{30}, slowRoute);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";
        EXPECT_FALSE(fixture.startThrew());

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /slow HTTP/1.1"), kWaitTimeout)) << "慢请求未能写入";
        ASSERT_TRUE(waitForCondition([&handlerStarted] { return handlerStarted.load(std::memory_order_acquire); }, kWaitTimeout)) << "慢路由未在时限内开始处理";

        std::string receivedText;
        EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout)) << "处理器超出 writeTimeout 却没收口：上界 kWaitTimeout（writeTimeout 300ms + 清扫节拍 30ms）";
        EXPECT_EQ(receivedText.find("served-slow"), std::string::npos) << "超预算的处理器仍然把响应写了出来";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout));
    }

    TEST(HttpServerLimits, KeepAliveCapClosesConnectionAfterMaximumRequests)
    {
        // 单连接请求上限：上限为 2 时，第 2 条响应就是「达到上限」的那条——它必须带
        // Connection: close，随后连接关闭；第 3 条请求因此再也拿不到响应。
        // 超时三项都设得很长：本用例只验证计数上限，不该被任何超时收口干扰
        HttpServerLimits limits;
        limits.idleTimeout                  = std::chrono::seconds{10};
        limits.readTimeout                  = std::chrono::seconds{10};
        limits.writeTimeout                 = std::chrono::seconds{10};
        limits.maximumRequestsPerConnection = 2;

        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        // 第 1 条：正常服务且保持连接（HTTP/1.1 默认保活，不该出现 connection 头）
        ASSERT_TRUE(client.sendText(helloRequestText(), kWaitTimeout));
        std::string receivedText;
        // 等到正文出现才断言头部：只数状态行会读到半截头部，那时「没有 connection 头」并不成立
        ASSERT_TRUE(client.waitForText(receivedText, "served-hello", kWaitTimeout)) << "第 1 条请求未得到完整响应";
        EXPECT_EQ(receivedText.find("connection:"), std::string::npos) << "还没到上限就把连接收口了：响应里出现了 connection 头";

        // 第 2 条：这是达到上限的那一条，响应必须显式声明 close
        ASSERT_TRUE(client.sendText(makeRequestText("GET /hello HTTP/1.1"), kWaitTimeout));
        EXPECT_TRUE(client.waitForText(receivedText, "connection: close", kWaitTimeout)) << "达到请求上限的响应没有带 Connection: close";
        EXPECT_EQ(countStatusLines(receivedText), 2u) << "第 2 条请求没有得到响应";

        // 连接随后关闭：客户端读到 EOF 或 reset
        EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout)) << "达到上限后连接未关闭";

        // 第 3 条：连接已经在收口，写进去也不会再得到响应（写本身允许失败：对端已关闭）
        client.sendText(makeRequestText("GET /hello HTTP/1.1"), kWaitTimeout);
        EXPECT_FALSE(client.waitForStatusLines(receivedText, 3, std::chrono::milliseconds{300})) << "上限之后仍然服务了新请求";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";
    }

    TEST(HttpServerLimits, ZeroSweepIntervalDisablesIdleTimeout)
    {
        // 拒绝面：清扫节拍为 0 时不做连接级超时。同一份空闲连接在同样的观察到窗口内
        // 必须保持存活——它证明上面的收口确实来自清扫协程，而不是别处的关闭动作
        HttpServerLimits limits;
        limits.idleTimeout  = std::chrono::milliseconds{100};
        limits.readTimeout  = std::chrono::milliseconds{100};
        limits.writeTimeout = std::chrono::milliseconds{100};

        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{0});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        std::string receivedText;
        EXPECT_FALSE(client.waitForClosure(receivedText, std::chrono::milliseconds{1000})) << "清扫已按节拍 0 关闭，空闲连接却仍被收口";
        EXPECT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器在接受循环期间意外退出";
    }

    TEST(HttpServerLimits, DrainWaitsForInFlightRequestToFinish)
    {
        // 钉住：drain 会把在途请求等完——客户端拿到完整响应、连接随后关闭，且 drain 在期限之前返回
        // （证明它等的不是死期限）。处理耗时 150ms 远小于下面 4s 的期限，两者区分得开
        HttpServerLimits limits;
        limits.idleTimeout  = std::chrono::seconds{10};
        limits.readTimeout  = std::chrono::seconds{10};
        limits.writeTimeout = std::chrono::seconds{10};

        std::atomic<bool> handlerStarted{false};
        SlowRouteOptions  slowRoute;
        slowRoute.processingTime = std::chrono::milliseconds{150};
        slowRoute.handlerStarted = &handlerStarted;

        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{50}, slowRoute);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /slow HTTP/1.1"), kWaitTimeout)) << "慢请求未能写入";

        // 与 drain 对齐：必须等处理函数真的进入在途状态再发起优雅关闭，否则 drain 可能在请求被读入
        // 之前就把这条还空闲的连接收掉，用例就不再是「等在途请求」了
        ASSERT_TRUE(waitForCondition([&handlerStarted] { return handlerStarted.load(std::memory_order_acquire); }, kWaitTimeout)) << "慢路由未在时限内开始处理：上界 kWaitTimeout";

        constexpr std::chrono::milliseconds drainTimeout{4000};
        const auto                          drainStartTime = std::chrono::steady_clock::now();
        ASSERT_TRUE(fixture.drainServer(drainTimeout, kWaitTimeout)) << "drain 未在时限内完成：上界 kWaitTimeout";
        const std::chrono::milliseconds drainElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - drainStartTime);

        // 在途请求被等完：完整响应到达客户端，连接随后才关闭
        std::string receivedText;
        EXPECT_TRUE(client.waitForText(receivedText, "served-slow", kWaitTimeout)) << "在途请求的响应没有发完";
        EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout)) << "drain 完成后连接仍未关闭";

        // 耗时应覆盖处理时间（下界 100ms 留出对齐全过程的开销）且远小于期限：
        // 前者证明它确实等在了在途请求上，后者证明它没有按死期限空等到期
        EXPECT_GE(drainElapsed, std::chrono::milliseconds{100}) << "drain 没有等在在途请求上，耗时仅 " << drainElapsed.count() << "ms（处理耗时 150ms）";
        EXPECT_LT(drainElapsed, drainTimeout) << "drain 等满了期限：在途请求没被识别为在途工作，耗时 " << drainElapsed.count() << "ms";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";
    }

    TEST(HttpServerLimits, DrainForcesCloseWhenDeadlineExpires)
    {
        // 钉住：处理时间远超期限时，drain 在期限附近返回并把连接强关——兜底分支真的会收手，
        // 不会因为连接始终不清空而无限等下去
        HttpServerLimits limits;
        limits.idleTimeout  = std::chrono::seconds{10};
        limits.readTimeout  = std::chrono::seconds{10};
        limits.writeTimeout = std::chrono::seconds{10};

        std::atomic<bool> handlerStarted{false};
        SlowRouteOptions  slowRoute;
        slowRoute.processingTime = std::chrono::milliseconds{1500};
        slowRoute.handlerStarted = &handlerStarted;

        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{50}, slowRoute);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /slow HTTP/1.1"), kWaitTimeout)) << "慢请求未能写入";
        ASSERT_TRUE(waitForCondition([&handlerStarted] { return handlerStarted.load(std::memory_order_acquire); }, kWaitTimeout)) << "慢路由未在时限内开始处理：上界 kWaitTimeout";

        constexpr std::chrono::milliseconds drainTimeout{300};
        const auto                          drainStartTime = std::chrono::steady_clock::now();
        ASSERT_TRUE(fixture.drainServer(drainTimeout, kWaitTimeout)) << "drain 未在时限内完成：上界 kWaitTimeout";
        const std::chrono::milliseconds drainElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - drainStartTime);

        // 期限附近返回：下界是 drainTimeout（期限从协程自己开始跑起算，只会更晚），上界留一个节拍加唤醒误差
        EXPECT_GE(drainElapsed, drainTimeout) << "drain 在期限之前就返回了，耗时 " << drainElapsed.count() << "ms";
        EXPECT_LT(drainElapsed, drainTimeout + kDrainReturnSlack) << "drain 远超期限才返回，耗时 " << drainElapsed.count() << "ms（期限 " << drainTimeout.count() << "ms）";

        // 连接被强关，且未完成的响应不会被发出去
        std::string receivedText;
        EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout)) << "期限到点后连接没有被强关";
        EXPECT_EQ(receivedText.find("served-slow"), std::string::npos) << "处理未完成的响应不该出现在客户端";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";
    }
} // namespace AsynGyanis::Net
