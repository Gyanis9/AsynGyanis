// 多候选连接竞赛用例：族间交错排序、并发首发、尾部补发、黑洞不拖场、时限收场
//
// 一律用真 TCP（回环上的真监听端 + 内核直接拒绝的空端口），不用 createPair：竞赛要看的是
// 「SYN 出去之后谁先回话」，那是传输层的事，抽象出的套接字对给不出这个形状。
//
// 驱动一律走 EventLoopThread（生产的那条派发路径），不用本模块的手泵助手：关掉还挂着等待者的
// 套接字时，IoWatcher 会把等待者排进跨线程队列并顺带唤醒循环，那个唤醒哨兵事件只有 run() 认得——
// 手泵会把它当成注册对象派发，打到整数地址上（实测 0xc0000005）。

#include "Core/Socket/ConnectionRace.h"

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/InetAddress.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        using TestSupport::EventLoopThread;

        /// 黑洞地址：RFC 5737 的 TEST-NET-1，按规范不该在任何真实网络上被路由，因此 SYN 出去没人答
        constexpr std::string_view kBlackholeIp = "192.0.2.1";

        /// 黑洞候选的端口：地址本身就没人答，端口取哪个都不影响它该表现出的形状
        constexpr std::uint16_t kBlackholePort = 443;

        /// 认定「本机确实把这个地址当黑洞」的下限：比这更快收场就说明本机给了 ICMP 不可达，不是黑洞
        constexpr std::chrono::milliseconds kBlackholeMinimumPendingMilliseconds{150};

        /// 在胜者那条连接上写出去的标记：写成功才算「连上的确是一条能用的连接」
        constexpr std::string_view kProbeMarker = "race";

        /// 探测黑洞要给的时限（毫秒）：既要长到能看出「挂住」，也要短到不拖慢整组用例
        constexpr std::chrono::milliseconds kBlackholeProbeDeadline{400};

        /**
         * @brief 回环上的一个真监听端
         * @details 只借 loop 建描述符：本类不挂任何等待，因此 IoWatcher 不会被创建（注册推迟到首次
         *          等待），bind/listen/取端口与收口都不碰循环状态——在测试线程上建、在测试线程上关。
         */
        class TestListener
        {
        public:
            /**
             * @brief 绑一个回环临时端口并开始监听
             * @param loop 用来创建套接字的事件循环
             */
            explicit TestListener(EventLoop &loop) : m_socket(AsyncSocket::create(loop))
            {
                EXPECT_TRUE(m_socket.bind(InetAddress::localhost(0)));
                EXPECT_TRUE(m_socket.listen(8));
            }

            /// 本监听端占到的端口
            [[nodiscard]] std::uint16_t port() const
            {
                return m_socket.localAddress().port();
            }

            /// 可以用来当候选地址的本机地址
            [[nodiscard]] InetAddress address() const
            {
                return InetAddress::localhost(port());
            }

        private:
            AsyncSocket m_socket; ///< 监听套接字：析构即关掉端口
        };

        /**
         * @brief 一次占够若干「此刻没人听」的地址，再全部关掉
         * @details 逐个占再逐个放会让内核把刚关掉的端口发给下一个监听端，于是「应当被拒绝的候选」
         *          可能变成活的，用例的判据就反了。因此先把全部监听端留在同一张表里，端口取完再一起
         *          析构。留下的窗口内别的进程也可能占走某个端口，那时首条候选会连上而不是被拒——
         *          那样的失败会指向端口抢占，不是实现。
         * @param loop 用来创建监听套接字的事件循环
         * @param count 要占的端口数
         * @return std::vector<InetAddress> 这些端口对应的地址，此刻都已没人监听
         */
        std::vector<InetAddress> makeRefusingAddresses(EventLoop &loop, const std::size_t count)
        {
            std::vector<std::unique_ptr<TestListener>> holders;
            std::vector<InetAddress>                   addresses;
            holders.reserve(count);
            addresses.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                holders.push_back(std::make_unique<TestListener>(loop));
                addresses.push_back(holders.back()->address());
            }
            return addresses; // holders 在此析构，端口一并关掉
        }

        /**
         * @brief 一场竞赛在循环线程上量到的东西
         * @details 只带纯数据出去：胜者那条套接字属于驱动它的那个循环，不能搬到测试线程上销毁，
         *          因此在循环线程上写完标记就地收口。
         */
        struct RaceObservation
        {
            bool                         isConcluded{false}; ///< 竞赛协程自己收场了；false 表示它挂住或抛了
            std::optional<std::uint16_t> winnerPort{};       ///< 连上的候选端口，没人连上时为空
            std::size_t                  markerSentBytes{0}; ///< 在胜者那条连接上写出去的字节数
            std::chrono::milliseconds    elapsed{0};         ///< 从发起到收场的墙钟耗时
        };

        /**
         * @brief 在循环线程上跑一场竞赛并量下结果
         * @param loop 驱动协程的事件循环
         * @param candidates 候选地址（原序交给竞赛；排序本身另有用例钉）
         * @param deadline 整场时限
         * @return Task<RaceObservation> 观测量
         */
        Task<RaceObservation> observeRace(EventLoop &loop, std::vector<InetAddress> candidates, const std::chrono::milliseconds deadline)
        {
            RaceObservation observation;
            const auto      startedAt = std::chrono::steady_clock::now();

            std::optional<ConnectedCandidate> winner = co_await connectCandidates(loop, std::move(candidates), deadline);
            observation.isConcluded                  = true;
            observation.elapsed                      = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
            if (winner.has_value())
            {
                observation.winnerPort = winner->address.port();
                const std::string marker(kProbeMarker);
                observation.markerSentBytes = static_cast<std::size_t>(co_await winner->socket.asyncSend(marker.data(), marker.size()));
                winner->socket.close();
            }
            co_return observation;
        }

        /**
         * @brief 起一场竞赛并等到它收场
         * @param runner 后台事件循环
         * @param candidates 候选地址
         * @param deadline 整场时限
         * @return RaceObservation 观测量；未收场时 isConcluded 为 false
         */
        RaceObservation runRace(EventLoopThread &runner, const std::vector<InetAddress> &candidates, const std::chrono::milliseconds deadline)
        {
            TestSupport::CompletedTask<RaceObservation> completed = runner.runToCompletion(observeRace(runner.loop(), candidates, deadline));
            if (completed.error)
            {
                // 竞赛契约是「失败交出空值，不抛」：逃出来的异常先报出底层原因，再按未收场处理
                try
                {
                    std::rethrow_exception(completed.error);
                } catch (const std::exception &failure)
                {
                    ADD_FAILURE() << "竞赛协程抛出了异常（契约要求失败返回空值）：" << failure.what();
                }
                return RaceObservation{};
            }
            if (!completed.finished)
            {
                return RaceObservation{};
            }
            return std::move(*completed.value);
        }

        /**
         * @brief 探一次本机会不会把黑洞地址当真黑洞
         * @param runner 后台事件循环
         * @return true 单发一条黑洞候选会挂到时限（本组用例要的形态）；false 本机立刻回了不可达
         * @details 这个探测是**必须的**：若本机对 192.0.2.1 立刻给 ICMP 不可达，「在途候选不拖住整场」
         *          就没有鉴别力（顺序试法也能过）。与其让用例假绿，不如显式跳过并说明原因。
         */
        bool isBlackholePending(EventLoopThread &runner)
        {
            const RaceObservation probed = runRace(runner, {InetAddress(kBlackholeIp, kBlackholePort)}, kBlackholeProbeDeadline);
            return probed.isConcluded && !probed.winnerPort.has_value() && probed.elapsed >= kBlackholeMinimumPendingMilliseconds;
        }
    } // namespace

    /**
     * @brief 候选排序：两族交替、族内保序，一族排空后另一族按原序接上
     * @details 判据是「谁都不许占满前几席」：全 IPv6 在前、全 IPv4 在后都算没做到。首条所属的族
     *          就是首选族，这一条把 RFC 6724 的偏好保留下来（解析器给什么顺序，就以它为准）。
     * @note 比对用地址文本而不是 InetAddress：后者是 sockaddr_storage 大小的值类型，gtest 只能按
     *       字节块打印，红了看不出是哪一条排错了位
     */
    TEST(ConnectionRace, OrdersCandidatesByAlternatingFamilies)
    {
        const std::vector<InetAddress> v4First{InetAddress("192.0.2.1", 1), InetAddress("192.0.2.2", 2), InetAddress("2001:db8::1", 3), InetAddress("2001:db8::2", 4)};
        const std::vector<InetAddress> orderedV4First = orderForConnectionRace(v4First);
        ASSERT_EQ(orderedV4First.size(), v4First.size());
        EXPECT_EQ(orderedV4First[0].toString(), v4First[0].toString()) << "首选族的第一条要排在最前";
        EXPECT_EQ(orderedV4First[1].toString(), v4First[2].toString()) << "第二席要给另一族，否则一族能占满前几席";
        EXPECT_EQ(orderedV4First[2].toString(), v4First[1].toString());
        EXPECT_EQ(orderedV4First[3].toString(), v4First[3].toString());

        const std::vector<InetAddress> v6First{InetAddress("2001:db8::1", 1), InetAddress("192.0.2.1", 2), InetAddress("2001:db8::2", 3)};
        const std::vector<InetAddress> orderedV6First = orderForConnectionRace(v6First);
        ASSERT_EQ(orderedV6First.size(), v6First.size());
        EXPECT_EQ(orderedV6First[0].toString(), v6First[0].toString());
        EXPECT_EQ(orderedV6First[1].toString(), v6First[1].toString()) << "IPv6 首选时第二席仍要让给 IPv4";
        EXPECT_EQ(orderedV6First[2].toString(), v6First[2].toString()) << "IPv6 剩下的那条按原序接上";

        // 单族与空输入都不该被改动形状
        const std::vector<InetAddress> singleFamily{InetAddress("192.0.2.1", 1), InetAddress("192.0.2.2", 2)};
        ASSERT_EQ(orderForConnectionRace(singleFamily).size(), singleFamily.size());
        for (std::size_t index = 0; index < singleFamily.size(); ++index)
        {
            EXPECT_EQ(orderForConnectionRace(singleFamily)[index].toString(), singleFamily[index].toString());
        }
        EXPECT_TRUE(orderForConnectionRace({}).empty());
    }

    /**
     * @brief 第一条候选被内核直接拒绝时，第二条照样要连上
     * @details 这条只验「失败要往下试」，不涉及时序，因此在任何环境都成立。它同时钉住一件事：
     *          交出的胜者是**连上的那一条**，而不是候选表的第一条。
     */
    TEST(ConnectionRace, FailsOverToTheNextCandidateWhenTheHeadIsRefused)
    {
        EventLoopThread                runner;
        const TestListener             live(runner.loop());
        const std::vector<InetAddress> candidates{makeRefusingAddresses(runner.loop(), 1U)[0], live.address()};

        const RaceObservation outcome = runRace(runner, candidates, std::chrono::milliseconds{2000});
        ASSERT_TRUE(outcome.isConcluded) << "竞赛没收场：候选收口或等待方唤醒有一条没做到";
        ASSERT_TRUE(outcome.winnerPort.has_value()) << "首条秒失败、次条在听，却没连上任一条";
        EXPECT_EQ(*outcome.winnerPort, live.port());
        EXPECT_EQ(outcome.markerSentBytes, kProbeMarker.size()) << "连上了却写不出去，交出的不是可用连接";
    }

    /**
     * @brief 超过「同时在途上限」的候选不会漏：前面的收口之后要补发后面的
     * @details 上限的存在让一场竞赛最多同时发 kMaximumConcurrentCandidates 条 SYN，代价是排在
     *          上限之外的候选必须靠补发才轮得到。这里把上限那么多条全做成会秒失败的地址，最后一条
     *          才是活的：补发没做，这条用例就连不上。
     */
    TEST(ConnectionRace, LaunchesCandidatesBeyondTheConcurrentCapAfterTheHeadConcludes)
    {
        EventLoopThread    runner;
        const TestListener live(runner.loop());

        std::vector<InetAddress> candidates = makeRefusingAddresses(runner.loop(), kMaximumConcurrentCandidates);
        candidates.push_back(live.address());

        const RaceObservation outcome = runRace(runner, candidates, std::chrono::milliseconds{2000});
        ASSERT_TRUE(outcome.isConcluded) << "竞赛没收场：补发的候选大概没被启动，等待方醒不过来";
        ASSERT_TRUE(outcome.winnerPort.has_value()) << "超出在途上限的那条候选没被补发，于是整场连不上";
        EXPECT_EQ(*outcome.winnerPort, live.port());
    }

    /**
     * @brief 首条候选挂住时，整场不许等它：另一条连上的要照常交出
     * @details 这条是本设计的动机。顺序试法（本框架此前的做法）把整段预算给第一条候选，黑洞地址
     *          于是独自吃光时限，后面的候选根本轮不到——表现就是「双栈主机连不上、纯 IPv4 连得上」。
     *          判据取「连上了活的第二条」而不是耗时：耗时阈值只能证明快，连不上才是故障本身。
     * @note 需要本机真的把 192.0.2.1 当黑洞（探测不过则跳过，理由见 isBlackholePending）
     */
    TEST(ConnectionRace, DoesNotWaitForAPendingCandidateWhenAnotherOneConnects)
    {
        EventLoopThread runner;
        if (!isBlackholePending(runner))
        {
            GTEST_SKIP() << "本机对 192.0.2.1 立刻给出不可达而不是静默丢包，「在途候选不拖住整场」这条判据在这里没有鉴别力";
        }

        const TestListener             live(runner.loop());
        const std::vector<InetAddress> candidates{InetAddress(kBlackholeIp, kBlackholePort), live.address()};

        const RaceObservation outcome = runRace(runner, candidates, std::chrono::milliseconds{1500});
        ASSERT_TRUE(outcome.isConcluded) << "竞赛没收场";
        ASSERT_TRUE(outcome.winnerPort.has_value()) << "在途的黑洞候选拖住了整场，活的第二条没连上";
        EXPECT_EQ(*outcome.winnerPort, live.port());
        EXPECT_LT(outcome.elapsed, std::chrono::milliseconds{1000}) << "连上了却几乎花光时限：还是在等黑洞那条";
    }

    /**
     * @brief 没有一条候选连得上时，整场按时限收场并交出空结果
     * @details 收场纪律都在这里验：黑洞那条要被自己的看门狗关掉、协程要报收口，等待方才会醒。
     *          任一环漏掉，用例看到的就不是「空结果」而是没收场（isConcluded=false）。
     * @note 同样需要本机把 192.0.2.1 当黑洞
     */
    TEST(ConnectionRace, GivesUpAtTheDeadlineWhenNoCandidateIsReachable)
    {
        EventLoopThread runner;
        if (!isBlackholePending(runner))
        {
            GTEST_SKIP() << "本机对 192.0.2.1 立刻给出不可达而不是静默丢包，这里就没有「挂到时限才收场」的候选可验";
        }

        const std::vector<InetAddress> candidates{makeRefusingAddresses(runner.loop(), 1U)[0], InetAddress(kBlackholeIp, kBlackholePort)};
        const RaceObservation          outcome = runRace(runner, candidates, std::chrono::milliseconds{300});
        ASSERT_TRUE(outcome.isConcluded) << "整场没收场：到点的那条候选没被关掉或没报收口";
        EXPECT_FALSE(outcome.winnerPort.has_value()) << "一条都连不上，却交出了胜者";
        EXPECT_GE(outcome.elapsed, std::chrono::milliseconds{250}) << "比时限早这么多就收场，看门狗的时限没生效";
    }

    /**
     * @brief 空候选表与零预算都不该挂起，直接交出空结果
     * @details 这两种输入在生产里来自「解析出空列表」与「前面几步已把预算用光」。挂在这上面就是
     *          调用方永远等不到答案，因此单独钉一条：收场要走完，不能靠时限兜。
     */
    TEST(ConnectionRace, SettlesAtOnceWhenThereIsNothingToTry)
    {
        EventLoopThread    runner;
        const TestListener live(runner.loop());

        const RaceObservation withoutCandidates = runRace(runner, {}, std::chrono::milliseconds{2000});
        ASSERT_TRUE(withoutCandidates.isConcluded) << "空候选表把调用方挂住了";
        EXPECT_FALSE(withoutCandidates.winnerPort.has_value());

        const RaceObservation withoutBudget = runRace(runner, {live.address()}, std::chrono::milliseconds{0});
        ASSERT_TRUE(withoutBudget.isConcluded) << "预算为 0 时把调用方挂住了";
        EXPECT_FALSE(withoutBudget.winnerPort.has_value()) << "预算为 0 还连了出去，时限就成了摆设";
    }
} // namespace AsynGyanis::Core
