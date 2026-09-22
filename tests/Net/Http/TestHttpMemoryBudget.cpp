// 在途正文全局预算的用例：额度语义、并发下不超发，以及超预算请求被 503 收口 预算类本身是纯原子的账本，前四个用例直接测它；最后一个用例跑真实服务器，验证 「正文边收边攒时就判定」这条时机：声明 100 字节正文、只发
// 64 字节，此刻若预算不够 会话就该收口，而不是等收齐（收齐时内存已经占住了）。
#include "Net/Http/HttpMemoryBudget.h"

#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpServerLimits.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        /// 预算用例用的限额：超时都设长，避免清扫协程在断言之前把连接收口掉
        HttpServerLimits makeBudgetTestLimits()
        {
            HttpServerLimits limits;
            limits.idleTimeout  = std::chrono::seconds{10};
            limits.readTimeout  = std::chrono::seconds{10};
            limits.writeTimeout = std::chrono::seconds{10};
            return limits;
        }

        /**
         * @brief 轮询读，直到累计文本里出现指定标记
         * @details 与观测性用例里的同名助手同构：读一次 → 看标记 → 到时限或对端收口就停，
         *          绝不阻塞测试线程等一个可能永远不来的字节
         * @param client 回环客户端
         * @param accumulated 输入输出：累计读到的字节
         * @param expectedText 作为「响应已到达」判据的标记文本
         * @param timeout 等待上限
         * @return true 时限内读到了标记
         */
        bool waitForTextOccurrence(const LoopbackClient &client, std::string &accumulated, const std::string_view expectedText,
                                   const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (accumulated.find(expectedText) == std::string::npos)
            {
                const ReadOutcome outcome = client.readOnce(accumulated);
                if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                {
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return accumulated.find(expectedText) != std::string::npos;
        }

        /**
         * @brief 等到额度归零
         * @details 额度在「应答写完」之后由守卫归还，而客户端读到响应与守卫析构之间没有严格顺序，
         *          因此这里轮询等待而不是断言一次：等待的是「最终会归还」，不是「此刻已归还」。
         * @param budget 预算对象
         * @param timeout 等待上限
         * @return true 时限内额度归零
         */
        bool waitUntilQuotaReturned(const std::shared_ptr<HttpMemoryBudget> &budget, const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (budget->reservedByteCount() != 0)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return true;
        }

        /**
         * @brief 轮询读，直到标记在累计文本里出现够 needed 次
         * @details keep-alive 上连着发两条请求时，「读到过一条响应」不算证据——必须数到第二条，
         *          否则第一条的响应就能让断言假绿
         * @param client 回环客户端
         * @param accumulated 输入输出：累计读到的字节
         * @param marker 数它的出现次数
         * @param needed 期望次数
         * @param timeout 等待上限
         * @return true 时限内数够了
         */
        bool waitForTextOccurrences(const LoopbackClient &client, std::string &accumulated, const std::string_view marker,
                                    const std::size_t needed, const std::chrono::milliseconds timeout)
        {
            const auto countMatches = [&accumulated, marker]
            {
                std::size_t matchCount = 0;
                for (std::size_t offset = accumulated.find(marker); offset != std::string::npos; offset = accumulated.find(marker, offset + marker.size()))
                {
                    ++matchCount;
                }
                return matchCount;
            };

            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (countMatches() < needed)
            {
                const ReadOutcome outcome = client.readOnce(accumulated);
                if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                {
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return countMatches() >= needed;
        }

        /// 预算跨连接用例的正文长度：单条不超上限，两条一起就超（100 的预算放不下 90 + 90）
        constexpr std::size_t kBudgetTestBodyBytes = 90;

        /**
         * @brief 造一条带正文的 POST 请求文本
         * @param path 请求目标
         * @return std::string 请求头 + 声明长度 + 正文
         */
        std::string makeBodyRequest(const std::string &path)
        {
            std::string request = "POST " + path + " HTTP/1.1\r\nhost: test\r\ncontent-length: " +
                                  std::to_string(kBudgetTestBodyBytes) + "\r\n\r\n";
            request.append(kBudgetTestBodyBytes, 'z');
            return request;
        }
    } // namespace

    /**
     * @brief 预留累加、归还后可复用、超限预留被拒
     */
    TEST(HttpMemoryBudgetTest, ReservesAccumulatesAndReleases)
    {
        HttpMemoryBudget budget(100);

        EXPECT_TRUE(budget.tryReserve(40));
        EXPECT_TRUE(budget.tryReserve(60));
        EXPECT_EQ(budget.reservedByteCount(), 100U);
        EXPECT_EQ(budget.maximumTotalBytes(), 100U);

        // 正好用满后再要一个字节即被拒
        EXPECT_FALSE(budget.tryReserve(1));

        budget.release(60);
        EXPECT_EQ(budget.reservedByteCount(), 40U);
        // 归还出来的额度必须能被再次借出，否则预算会随流量单调收紧
        EXPECT_TRUE(budget.tryReserve(60));
        EXPECT_EQ(budget.reservedByteCount(), 100U);
    }

    /**
     * @brief 失败的预留不能占用任何额度
     */
    TEST(HttpMemoryBudgetTest, FailedReservationConsumesNoQuota)
    {
        HttpMemoryBudget budget(10);
        EXPECT_TRUE(budget.tryReserve(8));

        EXPECT_FALSE(budget.tryReserve(5));
        // 被拒之后账目必须原封不动：否则一次失败会持续吃掉额度，最终连本该放行的也拒掉
        EXPECT_EQ(budget.reservedByteCount(), 8U);
        EXPECT_TRUE(budget.tryReserve(2));
    }

    /**
     * @brief 上限为 0 表示不限流，但仍记账（可观测）
     */
    TEST(HttpMemoryBudgetTest, ZeroLimitMeansUnlimitedButStillCounts)
    {
        HttpMemoryBudget budget(0);

        EXPECT_TRUE(budget.tryReserve(1ull << 40));
        EXPECT_EQ(budget.reservedByteCount(), 1ull << 40);
        budget.release(1ull << 40);
        EXPECT_EQ(budget.reservedByteCount(), 0U);
    }

    /**
     * @brief 多线程同时预留时总量不会超过上限
     */
    TEST(HttpMemoryBudgetTest, ConcurrentReservationsNeverExceedLimit)
    {
        constexpr std::size_t kLimit = 1000;
        HttpMemoryBudget      budget(kLimit);

        std::atomic<int>         successfulReservationCount{0};
        std::vector<std::thread> reservers;
        reservers.reserve(8);
        for (int index = 0; index < 8; ++index)
        {
            reservers.emplace_back(
                    [&budget, &successfulReservationCount]()
                    {
                        // 每次只借 1 字节、借到借不动为止：竞争下若存在「先判断后写入」的裂缝，
                        // 总量就会超过上限，这条断言正是用来钉住那类竞态的
                        while (budget.tryReserve(1))
                        {
                            successfulReservationCount.fetch_add(1, std::memory_order_relaxed);
                        }
                    });
        }
        for (std::thread &reserver: reservers)
        {
            reserver.join();
        }

        EXPECT_EQ(successfulReservationCount.load(), static_cast<int>(kLimit));
        EXPECT_EQ(budget.reservedByteCount(), kLimit);
    }

    /**
     * @brief 归还多于账目时按 0 收住，预算不会被一次错账翻转成「放行一切」
     * @details 旧写法是无符号 fetch_sub：多还一点就把账目绕成天文数字，而 tryReserve 判的是
     *          「上限减当前值」——跟着回绕之后剩余额度变成巨大，限额这道防线当场失效。
     *          这条用例钉的是回绕不再发生，且上限依旧被守住
     */
    TEST(HttpMemoryBudgetTest, OverReleaseIsClampedAndKeepsTheLimit)
    {
        HttpMemoryBudget budget(100);
        ASSERT_TRUE(budget.tryReserve(10));

        budget.release(20);
        EXPECT_EQ(budget.reservedByteCount(), 0U) << "多归还把账目绕成了大数，预算已不再拒绝超限";

        EXPECT_TRUE(budget.tryReserve(100));
        EXPECT_FALSE(budget.tryReserve(1)) << "账目回绕后上限形同虚设：用满了还放行";
    }

    /**
     * @brief Reservation 把自己的额度记在预算上，缩容不动账、超限不占账、二次归还是空操作
     * @details 这一层是「不漏还、不双还」的全部机制，此前只被会话用例间接经过，没有直测
     */
    TEST(HttpMemoryBudgetTest, ReservationTracksGrowShrinkAndReleaseAll)
    {
        HttpMemoryBudget budget(100);
        HttpMemoryBudget::Reservation reservation(&budget);
        ASSERT_TRUE(reservation.hasBudget());

        EXPECT_TRUE(reservation.growTo(40));
        EXPECT_EQ(budget.reservedByteCount(), 40U);

        // 缩到比已占的还小：空操作，账目必须一分不动（正文被截短不等于额度被归还）
        EXPECT_TRUE(reservation.growTo(10));
        EXPECT_EQ(budget.reservedByteCount(), 40U);

        // 增量超出剩余额度：整次调用不占任何额度，调用方据此收口
        EXPECT_FALSE(reservation.growTo(200));
        EXPECT_EQ(budget.reservedByteCount(), 40U);

        reservation.releaseAll();
        EXPECT_EQ(budget.reservedByteCount(), 0U);
        // 重复归还必须是空操作：守卫析构与手工归还叠加时不能把账目还成负数
        reservation.releaseAll();
        EXPECT_EQ(budget.reservedByteCount(), 0U);

        // 归零之后同一份占用还能继续按新正文长回去
        EXPECT_TRUE(reservation.growTo(100));
        EXPECT_EQ(budget.reservedByteCount(), 100U);
    }

    /**
     * @brief 移交占用时额度恰好跟着走一次
     * @details 待服务记录在容器间移动是常态：移走的一方必须交出归还责任（否则析构时二次归还），
     *          而接收方析构时要能把整份额度还回去。移动赋值还要先还掉自己原来的账
     */
    TEST(HttpMemoryBudgetTest, ReservationMoveTransfersQuotaExactlyOnce)
    {
        HttpMemoryBudget budget(100);

        HttpMemoryBudget::Reservation source(&budget);
        ASSERT_TRUE(source.growTo(30));
        HttpMemoryBudget::Reservation receiver(std::move(source));
        EXPECT_FALSE(source.hasBudget()) << "被移走的一方仍认为自己绑着预算，析构时会二次归还";
        EXPECT_EQ(budget.reservedByteCount(), 30U);

        // 赋值接收方自己先还掉在占的额度，再接管对方的
        HttpMemoryBudget::Reservation assigned(&budget);
        ASSERT_TRUE(assigned.growTo(20));
        assigned = std::move(receiver);
        EXPECT_EQ(budget.reservedByteCount(), 30U) << "移动赋值没先归还自己的账，或把对方的账丢了";

        // 接收方析构只该还一次：还完账目归零，而不是绕成负数或留下尾巴
        assigned.releaseAll();
        EXPECT_EQ(budget.reservedByteCount(), 0U);
    }

    /**
     * @brief 未绑定预算的占用一切为空操作，重新绑定会先结清旧账
     */
    TEST(HttpMemoryBudgetTest, UnboundReservationIsInertAndResetSettlesOldBudget)
    {
        HttpMemoryBudget firstBudget(100);
        HttpMemoryBudget secondBudget(100);

        HttpMemoryBudget::Reservation unbound;
        EXPECT_FALSE(unbound.hasBudget());
        EXPECT_TRUE(unbound.growTo(1000));
        unbound.releaseAll();

        HttpMemoryBudget::Reservation bound(&firstBudget);
        ASSERT_TRUE(bound.growTo(40));
        EXPECT_EQ(firstBudget.reservedByteCount(), 40U);

        // 换绑到另一份预算：旧账当场结清，新预算不会被「继承」上一段的占用
        bound.reset(&secondBudget);
        EXPECT_EQ(firstBudget.reservedByteCount(), 0U);
        EXPECT_EQ(secondBudget.reservedByteCount(), 0U);
        EXPECT_TRUE(bound.growTo(50));
        EXPECT_EQ(secondBudget.reservedByteCount(), 50U);

        // 绑到空指针表示此后不记账，但已占的额度仍要还掉
        bound.reset(nullptr);
        EXPECT_FALSE(bound.hasBudget());
        EXPECT_EQ(secondBudget.reservedByteCount(), 0U);
    }

    /**
     * @brief 正文超出全局预算的请求立即被 503 收口，且收口后额度归还
     */
    TEST(HttpMemoryBudgetTest, RejectsRequestBodyBeyondGlobalBudgetWith503)
    {
        // 预算 32 字节：请求声明 100 字节正文，只发出 64 字节，此刻就该判定超预算
        auto budget = std::make_shared<HttpMemoryBudget>(32);
        RunningHttpServerFixture fixture(makeBudgetTestLimits(), std::chrono::milliseconds{100}, {}, {}, HttpParserLimits{},
                                         [budget](TestHttpServer &server)
                                         {
                                             server.setMemoryBudget(budget);
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";

        LoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        std::string request = "POST /hello HTTP/1.1\r\nhost: test\r\ncontent-length: 100\r\n\r\n";
        request.append(64, 'x');
        std::string receivedText;
        ASSERT_TRUE(client.sendText(request, kWaitTimeout)) << "请求未能写入";
        ASSERT_TRUE(waitForTextOccurrence(client, receivedText, "503", kWaitTimeout))
                << "超预算的请求未得到 503，实际收到：" << receivedText;

        EXPECT_TRUE(waitUntilQuotaReturned(budget, kWaitTimeout)) << "连接收口后额度仍未归还，当前占用 " << budget->reservedByteCount();
    }

    /** 
     * @brief 预算充裕时请求照常完成，且应答写完即归还额度
     */
    TEST(HttpMemoryBudgetTest, ServesRequestWithinBudgetAndReturnsQuota)
    {
        auto budget = std::make_shared<HttpMemoryBudget>(1ull << 20);
        RunningHttpServerFixture fixture(makeBudgetTestLimits(), std::chrono::milliseconds{100}, {}, {}, HttpParserLimits{},
                                         [budget](TestHttpServer &server)
                                         {
                                             server.setMemoryBudget(budget);
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";

        LoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        // 带正文的请求走完正常路径（路径不存在，回 404，但正文照样被解析与记账）
        std::string request = "POST /missing HTTP/1.1\r\nhost: test\r\ncontent-length: 40\r\n\r\n";
        request.append(40, 'y');
        std::string receivedText;
        ASSERT_TRUE(client.sendText(request, kWaitTimeout)) << "请求未能写入";
        ASSERT_TRUE(waitForTextOccurrence(client, receivedText, "404", kWaitTimeout))
                << "正文在预算内的请求未被正常处理，实际收到：" << receivedText;

        EXPECT_TRUE(waitUntilQuotaReturned(budget, kWaitTimeout)) << "应答写完额度仍未归还，当前占用 " << budget->reservedByteCount();
    }

    /**
     * @brief 同一条 keep-alive 连接上的第二个正文请求，不该被上一个请求的额度挡住
     * @details 这笔预算管的是「同时在途的正文总量」：额度若到应答写完都不归还，一条长连接发第二个
     *          正文就会被 503——上限会被单个连接自己吃满，与「跨连接总量」这个初衷不符
     */
    TEST(HttpMemoryBudgetTest, ReturnsQuotaBetweenKeepAliveRequestsOnOneConnection)
    {
        // 预算 100：装得下一条 90 字节的正文，装不下两条同时在场
        auto budget = std::make_shared<HttpMemoryBudget>(100);

        HttpParserLimits parserLimits;
        parserLimits.maximumBodySize = 1024; // 让全局预算成为唯一的约束，而不是单请求正文上限

        RunningHttpServerFixture fixture(makeBudgetTestLimits(), std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [](Router &router, Core::EventLoop &loop)
                                         {
                                             registerBodyHoldingRoute(router, loop, nullptr, std::chrono::milliseconds{1});
                                         },
                                         parserLimits,
                                         [budget](TestHttpServer &server)
                                         {
                                             server.setMemoryBudget(budget);
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";

        LoopbackClient client(fixture.listeningPort());
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        std::string receivedText;
        ASSERT_TRUE(client.sendText(makeBodyRequest(std::string{kBodyHoldingRoutePath}), kWaitTimeout)) << "第一条请求未能写入";
        ASSERT_TRUE(waitForTextOccurrences(client, receivedText, kBodyHoldingResponseBody, 1, kWaitTimeout))
                << "第一条请求没拿到正常响应，实际收到：" << receivedText;
        ASSERT_TRUE(waitUntilQuotaReturned(budget, kWaitTimeout))
                << "第一条应答已写完，额度却仍占着 " << budget->reservedByteCount() << " 字节";

        // 同一条连接上再发一条同量正文：额度归还过就该照常通过，而不是被判超预算
        ASSERT_TRUE(client.sendText(makeBodyRequest(std::string{kBodyHoldingRoutePath}), kWaitTimeout)) << "第二条请求未能写入（连接被提前收口？）";
        ASSERT_TRUE(waitForTextOccurrences(client, receivedText, kBodyHoldingResponseBody, 2, kWaitTimeout))
                << "同一条连接上的第二条正文请求没拿到正常响应，实际收到：" << receivedText;
        EXPECT_EQ(receivedText.find("503"), std::string::npos)
                << "先后进行的两个请求被算成同时在场，第二条被误判超预算：" << receivedText;
    }

    /**
     * @brief 一条连接正占着额度时，另一条连接的正文超出的那部分要按 503 收口
     * @details 这是这笔预算存在的全部理由——多条连接各压一份正文时总占用要有上限。若判定按连接各算一份，
     *          第二条连接照样能塞满自己的正文，全局上限等于没有
     */
    TEST(HttpMemoryBudgetTest, RejectsSecondConnectionBodyWhileFirstIsStillHeld)
    {
        auto budget = std::make_shared<HttpMemoryBudget>(100);
        std::atomic<bool> isFirstHandlerRunning{false};

        HttpParserLimits parserLimits;
        parserLimits.maximumBodySize = 1024;

        RunningHttpServerFixture fixture(makeBudgetTestLimits(), std::chrono::milliseconds{100}, SlowRouteOptions{},
                                         [&isFirstHandlerRunning](Router &router, Core::EventLoop &loop)
                                         {
                                             // 400 毫秒的按住窗口：足够第二条连接发完正文并拿到判定
                                             registerBodyHoldingRoute(router, loop, &isFirstHandlerRunning, std::chrono::milliseconds{400});
                                         },
                                         parserLimits,
                                         [budget](TestHttpServer &server)
                                         {
                                             server.setMemoryBudget(budget);
                                         });
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环";

        LoopbackClient holdingClient(fixture.listeningPort());
        ASSERT_TRUE(holdingClient.isValid()) << "第一条回环连接失败";
        ASSERT_TRUE(holdingClient.sendText(makeBodyRequest(std::string{kBodyHoldingRoutePath}), kWaitTimeout)) << "第一条请求未能写入";

        ASSERT_TRUE(waitForCondition(
                [&isFirstHandlerRunning]
                {
                    return isFirstHandlerRunning.load(std::memory_order_acquire);
                },
                kWaitTimeout))
                << "第一条请求没进处理器，额度根本没被占住";
        EXPECT_GE(budget->reservedByteCount(), kBudgetTestBodyBytes)
                << "处理器还在跑时额度就该占着，实际占用 " << budget->reservedByteCount() << " 字节";

        LoopbackClient secondClient(fixture.listeningPort());
        ASSERT_TRUE(secondClient.isValid()) << "第二条回环连接失败";
        ASSERT_TRUE(secondClient.sendText(makeBodyRequest(std::string{kBodyHoldingRoutePath}), kWaitTimeout)) << "第二条请求未能写入";

        std::string secondText;
        ASSERT_TRUE(waitForTextOccurrence(secondClient, secondText, "503", kWaitTimeout))
                << "两条连接各压 90 字节、预算只有 100，第二条却没被按 503 收口，实际收到：" << secondText;

        // 第一条照常做完：它占的额度是先前记下的，不受第二条被拒的影响
        std::string holdingText;
        ASSERT_TRUE(waitForTextOccurrence(holdingClient, holdingText, kBodyHoldingResponseBody, kWaitTimeout))
                << "占着额度的那条请求没能做完，实际收到：" << holdingText;
        EXPECT_TRUE(waitUntilQuotaReturned(budget, kWaitTimeout)) << "全部请求收口后额度仍未归还，当前占用 " << budget->reservedByteCount();
    }
} // namespace AsynGyanis::Net
