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
} // namespace AsynGyanis::Net
