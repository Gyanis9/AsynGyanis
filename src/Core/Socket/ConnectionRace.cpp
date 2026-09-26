// 多候选地址的并发连接竞赛：候选协程各挂各的套接字，第一个连上的定局并收掉其余

#include "Core/Socket/ConnectionRace.h"

#include "Base/Exception/Exception.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/DeadlineGuard.h"
#include "Core/EventLoop/EventLoop.h"

#include <chrono>
#include <cstddef>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        class CandidateRace;

        /**
         * @brief 一路候选的连接协程
         * @param race 所属竞赛的账本（非拥有；等待方协程帧持有它，且要活到本协程收口）
         * @param index 本协程负责的候选下标
         * @details 本协程没人 co_await，是手动启动的独立协程：它的收口只靠 Conclusion 这一条路，
         *          因此连异常出口也要经过它——漏计一次数的后果不是崩溃而是等待方永远醒不过来。
         */
        Task<void> runCandidateAttempt(CandidateRace &race, const std::size_t index);

        /**
         * @brief 一场竞赛的账本：候选、在途协程、套接字、胜者与等待方
         * @details 全部状态只在所属事件循环线程上读写（候选协程与等待方协程同一个循环），因此不需要
         *          原子量或互斥；跨协程的「谁赢」由 claimWin() 的单次判定决定，判定的读与写之间没有
         *          挂起点，所以不会被另一路插进来。
         */
        class CandidateRace
        {
        public:
            /**
             * @brief 建好账本
             * @param loop 所属事件循环
             * @param candidates 候选地址（搬进本账本）
             * @param deadline 整场时限
             */
            CandidateRace(EventLoop &loop, std::vector<InetAddress> candidates,
                          const std::chrono::milliseconds deadline)
                : m_loop(loop), m_candidates(std::move(candidates)), m_deadline(deadline),
                  m_startedAt(std::chrono::steady_clock::now()), m_sockets(m_candidates.size()),
                  m_attempts(m_candidates.size())
            {
            }

            /**
             * @brief 整场还剩多少预算
             * @return std::chrono::milliseconds 剩余毫秒；已到期给出 0（不为负）
             */
            [[nodiscard]] std::chrono::milliseconds remainingBudget() const
            {
                const auto elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                              m_startedAt);
                const auto remaining = m_deadline - elapsed;
                return remaining.count() > 0 ? remaining : std::chrono::milliseconds{0};
            }

            /**
             * @brief 本路候选能占多久的时限
             * @return std::chrono::milliseconds 至少 1 毫秒
             * @details 预算按「从本波起还有几波要分」切均。一两条地址（双栈主机最常见的形状）落在同
             *          一波里，rounds 就是 1，两条各拿完整预算——并发本来就在比谁先连上，不需要截。
             *          只有候选条数超过在途上限时才会有第二波，而第二波的候选**要等前面的收口才发起**：
             *          不切预算的话，几条一起挂住的头部会把整场吃光，第五条永远轮不到。实测正是这个
             *          形状：Windows 的完成端口后端上「连不上」不给可写事件，挂在 connect 上的协程要等
             *          自己的看门狗（一条拒绝的候选独吞 2000 ms 预算，补发的第五条到点即饿死）。
             *          切的代价是被切的头部可能本来连得上（连接耗时超过这一份预算），换来的是尾波
             *          一定有一次机会。
             */
            [[nodiscard]] std::chrono::milliseconds attemptSlice() const
            {
                const std::chrono::milliseconds remaining = remainingBudget();
                // 本条属于第几波（发起时 m_launchedCount 已含本条，故减一再除）
                const std::size_t wave = (m_launchedCount - 1U) / kMaximumConcurrentCandidates;
                const std::size_t totalWaves =
                        (m_candidates.size() + kMaximumConcurrentCandidates - 1U) / kMaximumConcurrentCandidates;
                const std::size_t rounds = totalWaves - wave;
                const std::chrono::milliseconds slice = remaining / static_cast<long long>(rounds);
                return slice.count() > 0 ? slice : std::chrono::milliseconds{1};
            }

            /**
             * @brief 补发候选，直到在途数触顶、候选发完、已定局或预算用尽
             * @details 首发由等待方协程调用，之后的补发由每一路候选收口时调用——因此「前一条秒失败」
             *          不会让下一条空等：这是不做固定错峰的理由（见 connectCandidates 的说明）。
             *          补发是就地 resume 新协程，它会跑到自己的首次挂起为止；一条都不挂起的极端情况
             *          （全部立刻被拒）会递归回本函数，深度上界是候选条数，而候选来自一次 DNS 应答，
             *          通常两三条、最多十几条。
             */
            void launchPending()
            {
                while (!m_winner.has_value() && !m_isBudgetExhausted && m_inFlightCount < kMaximumConcurrentCandidates
                       && m_launchedCount < m_candidates.size())
                {
                    if (remainingBudget().count() <= 0)
                    {
                        m_isBudgetExhausted = true;
                        return;
                    }
                    const std::size_t index = m_launchedCount++;
                    ++m_inFlightCount;
                    m_attempts[index].emplace(runCandidateAttempt(*this, index));
                    m_attempts[index]->handle().resume(); // 惰性协程：手动启动
                }
            }

            /**
             * @brief 收口一路候选：腾出在途名额、补发下一条，全场收完时唤醒等待方
             */
            void conclude()
            {
                --m_inFlightCount;
                launchPending();
                if (m_waiter && isSettled())
                {
                    // 排回循环而不是就地 resume：本函数可能在另一路候选的栈上被调用（补发时它一路
                    // 同步跑到底），就地恢复会让等待方协程在返回路径上销毁整场账本，而自己还引用着它
                    m_loop.scheduler().schedule(std::exchange(m_waiter, {}));
                }
            }

            /**
             * @brief 认领胜利者：第一个连上的拿走，之后的都算迟到
             * @param index 连上的候选下标
             * @return true 本条就是胜者（套接字已搬出账本，归调用方交给上层）；false 已有赢家
             */
            bool claimWin(const std::size_t index)
            {
                if (m_winner.has_value())
                {
                    return false;
                }
                m_winnerIndex = index;
                m_winner.emplace(ConnectedCandidate{std::move(*m_sockets[index]), m_candidates[index]});
                cancelOtherSockets();
                return true;
            }

            /**
             * @brief 取走胜者（整场收口后由等待方调用）
             * @return std::optional<ConnectedCandidate> 有人连上就交出，否则为空
             */
            std::optional<ConnectedCandidate> takeWinner()
            {
                return std::move(m_winner);
            }

            /**
             * @brief 全场是否已定局
             * @return true 所有已发起的候选都收口，且不再有可发起的候选（或已经拿到胜者）
             */
            [[nodiscard]] bool isSettled() const noexcept
            {
                return m_inFlightCount == 0U
                       && (m_winner.has_value() || m_isBudgetExhausted || m_launchedCount == m_candidates.size());
            }

            /// 登记等待方协程：定局时由最后一路收口的候选把它排回循环
            void setWaiter(const std::coroutine_handle<> waiter) noexcept
            {
                m_waiter = waiter;
            }

            /// 本候选的套接字槽位：由本路协程创建，也是 cancelOtherSockets() 用来掐断在途连接的把手
            std::optional<AsyncSocket> &socketSlot(const std::size_t index) noexcept
            {
                return m_sockets[index];
            }

            /// 候选地址表
            [[nodiscard]] const std::vector<InetAddress> &candidates() const noexcept
            {
                return m_candidates;
            }

            /// 所属事件循环
            [[nodiscard]] EventLoop &loop() noexcept
            {
                return m_loop;
            }

        private:
            /**
             * @brief 关掉除胜者以外所有已发起的套接字
             * @details 挂在 connect 上的等待器没有取消接口，close 是本框架里让它立刻收口的既定手段
             *          （与看门狗走同一条路径）。胜者那条已被搬空，关掉它只会是个空操作。
             */
            void cancelOtherSockets()
            {
                for (std::size_t index = 0; index < m_sockets.size(); ++index)
                {
                    if (index != m_winnerIndex && m_sockets[index].has_value())
                    {
                        m_sockets[index]->close();
                    }
                }
            }

            EventLoop &m_loop;                                ///< 所属事件循环
            std::vector<InetAddress> m_candidates;            ///< 候选地址（按调用方给的顺序）
            const std::chrono::milliseconds m_deadline;       ///< 整场时限
            const std::chrono::steady_clock::time_point m_startedAt; ///< 整场开始时刻，用于算剩余预算
            std::vector<std::optional<AsyncSocket>> m_sockets; ///< 每候选一条套接字（未发起时为空）
            std::vector<std::optional<Task<void>>> m_attempts; ///< 每候选一路协程帧，随账本一起销毁
            std::optional<ConnectedCandidate> m_winner;       ///< 胜者：连上的套接字与它连到的地址
            std::size_t m_winnerIndex{0};                     ///< 胜者的候选下标
            std::size_t m_inFlightCount{0};                   ///< 在途（已发起未收口）的候选数
            std::size_t m_launchedCount{0};                   ///< 已发起过的候选条数（发到哪里为止）
            bool m_isBudgetExhausted{false};                  ///< 预算用尽，剩下的候选不再发起
            std::coroutine_handle<> m_waiter{};               ///< 等待整场收口的协程
        };

        /**
         * @brief 等待方协程的等待器：定局前一直挂着
         */
        class RaceAwaiter
        {
        public:
            /**
             * @brief 绑定账本
             * @param race 本场竞赛（非拥有；它就在等待方的协程帧里）
             */
            explicit RaceAwaiter(CandidateRace &race) noexcept : m_race(race)
            {
            }

            /// 没有候选在途时不必挂起（空候选、预算为 0、或全部候选都同步收口了）
            [[nodiscard]] bool await_ready() const noexcept
            {
                return m_race.isSettled();
            }

            /// 登记等待方协程，由最后收口的那一路候选把它排回循环
            void await_suspend(const std::coroutine_handle<> waiter)
            {
                m_race.setWaiter(waiter);
            }

            /**
             * @brief 交出胜者；没人赢就是空
             * @details **只能取一次**：取走会把账本里的胜者搬空，第二次拿到的是描述符已归还的空壳——
             *          所以等待方协程要写成 `co_return co_await RaceAwaiter{race};`，先 co_await 丢弃
             *          再自己取一遍就会静默交出一条不能用的连接（实测 asyncSend 报 ENOTSOCK）。
             */
            std::optional<ConnectedCandidate> await_resume()
            {
                return m_race.takeWinner();
            }

        private:
            CandidateRace &m_race; ///< 所属竞赛账本
        };

        Task<void> runCandidateAttempt(CandidateRace &race, const std::size_t index)
        {
            /**
             * @brief 收口哨兵：析构即向账本报到
             * @details 用 RAII 而不是在每个出口各调一次：这一路协程有两条异常出口，漏掉一条就少一次
             *          计数，等待方会永远停在整场上——比连不上更难查的那种挂死。
             */
            struct Conclusion
            {
                CandidateRace &race; ///< 所属竞赛账本

                ~Conclusion()
                {
                    race.conclude();
                }
            } conclusion{race};

            const InetAddress &candidate = race.candidates()[index];
            std::optional<AsyncSocket> &slot = race.socketSlot(index);
            slot = AsyncSocket::create(race.loop(), candidate.family());
            AsyncSocket &socket = *slot;

            // 本条的时限由账本按「后面还排着几条」切给它的（见 CandidateRace::attemptSlice）
            const DeadlineGuard<AsyncSocket> guard(race.loop(), socket, race.attemptSlice(), "候选连接");

            bool isConnected = false;
            try
            {
                co_await socket.asyncConnect(candidate);
                isConnected = true;
            }
            catch (const Base::Exception &failure)
            {
                // 底层原文只进日志（what() 里带抛出点，不外传）：这一路的失败不该决定整场的说法
                LOG_WARN_FMT("ConnectionRace: 候选 {} 没连上。底层原因：{}", candidate.toString(), failure.what());
            }
            catch (...)
            {
                LOG_WARN_FMT("ConnectionRace: 候选 {} 没连上，且底层抛出的是框架之外的抛出物",
                             candidate.toString());
            }

            if (isConnected && !race.claimWin(index))
            {
                // 迟到了一条：整场已经有赢家，本条按「连上即关」收口，对端看到的是一次建立又立刻结束的连接
                socket.close();
            }
            co_return;
        }
    } // namespace

    std::vector<InetAddress> orderForConnectionRace(const std::vector<InetAddress> &resolved)
    {
        std::vector<InetAddress> preferred;  ///< 首选族（排序结果第一条所属的那一族）
        std::vector<InetAddress> other;      ///< 另一族
        preferred.reserve(resolved.size());
        other.reserve(resolved.size());

        const bool preferIpv6 = !resolved.empty() && resolved.front().family() == AF_INET6;
        for (const InetAddress &address: resolved)
        {
            if ((address.family() == AF_INET6) == preferIpv6)
            {
                preferred.push_back(address);
            }
            else
            {
                other.push_back(address);
            }
        }

        std::vector<InetAddress> ordered;
        ordered.reserve(resolved.size());
        for (std::size_t index = 0; index < preferred.size() || index < other.size(); ++index)
        {
            if (index < preferred.size())
            {
                ordered.push_back(preferred[index]);
            }
            if (index < other.size())
            {
                ordered.push_back(other[index]);
            }
        }
        return ordered;
    }

    Task<std::optional<ConnectedCandidate>> connectCandidates(EventLoop &loop,
                                                             std::vector<InetAddress> candidates,
                                                             const std::chrono::milliseconds deadline)
    {
        CandidateRace race(loop, std::move(candidates), deadline);
        race.launchPending();
        // 一次 co_await 就把胜者带走：await_resume() 是**搬空**账本里的胜者，多调一次第二次只会拿到
        // 一个描述符已归还的空壳
        co_return co_await RaceAwaiter{race};
    }
} // namespace AsynGyanis::Core
