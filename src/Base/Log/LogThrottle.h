/**
 * @file LogThrottle.h
 * @brief 按时间窗压住「同一个调用点」重复日志的闸门：远端可驱动的告警不该把日志与事件循环一起淹掉
 * @author Gyanis
 * @date 2026-09-26
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace AsynGyanis::Base
{
    /**
     * @brief 一个调用点的日志闸门：每 interval 至多放行一条，其余只累计条数
     *
     * @details 为什么需要它：有几条告警是**对端一句话就能触发一次**的（例如要求 PROXY 协议头的端口上，
     *          每个不打头的连接都会留一条 WARN）。这类路径上「一条连接一条日志」有两个后果——日志被
     *          同一句话刷满，真正的信息淹在里面看不见；以及同步落盘的每请求日志把事件循环拖住
     *          （本仓库真出过一次：热路径上的日志把停摆表现成 backlog 灌满后回 ECONNREFUSED）。
     *          压住重复条而不是关掉这条告警，是因为「有人在打这个端口」本身是要让运维看见的事。
     *
     * @note 判定只用原子量与一次 CAS：没有锁，也没有「按调用点建表」的哈希查找——闸门是给热路径用的，
     *       它自己不能成为新的开销来源。
     * @note 被压掉的条数不丢：放行那一刻由 droppedCount() 读出「自上次放行以来压掉了多少条」，
     *       调用方把它写进那条放行的日志里。于是量级信息始终可见，丢的只是重复条目里的逐条细节
     *       （地址、字节数）——那些值在同一条判定下每次都可能不同，但决定要不要看的人要的是频率。
     * @warning 一个实例代表**一个调用点**。跨调用点共用一份会让 A 点的洪水把 B 点也压掉。
     * @see ASYN_LOG_THROTTLED
     */
    class LogThrottle
    {
    public:
        /**
         * @brief 建一个闸门
         * @param interval 放行间隔；0 表示不压（每条都放行，等价于把这条路径退回未采样的形态）
         */
        explicit LogThrottle(std::chrono::milliseconds interval) noexcept;

        /**
         * @brief 问一句：这一条该写吗
         * @return true 到点了，本条放行（同时把上一条放行至今压掉的条数留给 droppedCount() 读）
         * @return false 还在窗口内，本条被压掉
         * @note 多个线程同时到点时只有一个放行：窗口起点用 CAS 抢，抢输的那一路走「压掉」分支。
         *       该用例（tests/Base/Log/TestLogThrottle.cpp 的
         *       HandsTheWindowToExactlyOneThreadWhenTheyRace）钉的是「窗口会推进」与「至少放行一条」
         *       两端；CAS 本身测不出来（撞不上那么窄的间隙），缘由写在那条用例的注释里
         */
        [[nodiscard]] bool acquire() noexcept;

        /**
         * @brief 上一次放行到现在被压掉了多少条
         * @return std::uint64_t 条数；本闸门一次都没放行过时为 0
         * @note 读它要在 acquire() 返回 true 之后，读到的才是「刚结束那一段」的条数
         */
        [[nodiscard]] std::uint64_t droppedCount() const noexcept;

    private:
        /**
         * @brief 把当前时刻折成「自 steady_clock 纪元的毫秒数」
         * @return std::uint64_t 此刻的毫秒刻度
         * @details 原子量只放得下整数，判定时只看差值，因此不进 chrono 类型
         */
        [[nodiscard]] static std::uint64_t nowMilliseconds() noexcept;

        std::chrono::milliseconds m_interval;                   ///< 放行间隔；0 表示不压
        std::atomic<std::uint64_t> m_nextPassAtMilliseconds{0}; ///< 下一个可放行的时刻（毫秒）
        std::atomic<std::uint64_t> m_droppedSincePass{0};       ///< 自上次放行以来被压掉的条数
        std::atomic<std::uint64_t> m_droppedReported{0};        ///< 上次放行时交出的条数，供调用方读
    };

    /**
     * @brief 在调用点就地建一个闸门（每个使用处各有一份状态）
     * @param interval 放行间隔，任何可隐式换成 std::chrono::milliseconds 的时长量都行
     * @return 该调用点那份闸门的引用
     * @details 形态是「宏返回引用」而不是宏把整条日志包掉：调用方仍然自己决定写哪一级、格式化什么，
     *          于是要么不新增任何日志宏、要么把每个级别都配一份带间隔参数的变体（六个级别乘两种
     *          形态，是十四份几乎一样的宏）。用法：
     *          @code
     *          if (auto &throttle = ASYN_LOG_THROTTLED(std::chrono::seconds{10}); throttle.acquire())
     *          {
     *              LOG_WARN_FMT("……（过去 10 秒内另有 {} 条同类被压掉）", throttle.droppedCount());
     *          }
     *          @endcode
     * @note 状态是 lambda 里的函数局部 static：C++11 起局部静态的初始化线程安全，而每个使用处的
     *       lambda 类型各不相同，因此各用各的窗口，互不干扰
     */
#define ASYN_LOG_THROTTLED(interval)                                                            \
    []() -> ::AsynGyanis::Base::LogThrottle &                                                   \
    {                                                                                           \
        static ::AsynGyanis::Base::LogThrottle throttle((interval));                            \
        return throttle;                                                                        \
    }()

} // namespace AsynGyanis::Base
