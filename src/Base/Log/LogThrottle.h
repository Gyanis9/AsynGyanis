/**
 * @file LogThrottle.h
 * @brief 按时间窗压住「同一个调用点」重复日志的闸门：远端可驱动的告警不该把日志与事件循环一起淹掉
 * @author Gyanis
 * @date 2026-09-26
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

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
     * @note 一个调用点要按**运行期的 key** 分开压（每个来源 IP、每台设备各一个窗口）时，本类的
     *       形状给不了：那份状态得有地方放。那种用法看 LogThrottleRegistry。
     * @see ASYN_LOG_THROTTLED, LogThrottleRegistry
     */
    class ASYN_BASE_API LogThrottle
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

        std::chrono::milliseconds  m_interval;                  ///< 放行间隔；0 表示不压
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
#define ASYN_LOG_THROTTLED(interval)                                                                                                                                               \
    []() -> ::AsynGyanis::Base::LogThrottle &                                                                                                                                      \
    {                                                                                                                                                                              \
        static ::AsynGyanis::Base::LogThrottle throttle((interval));                                                                                                               \
        return throttle;                                                                                                                                                           \
    }()

    /**
     * @brief 一次「按 key 问闸门」的判定结果
     */
    struct LogThrottleDecision
    {
        bool          isPassed{false}; ///< true＝本条该写
        std::uint64_t droppedCount{0}; ///< 放行时＝这一段被压掉的条数；被压掉时恒为 0
    };

    /**
     * @brief 按**运行期 key** 分档的日志闸门表：一个调用点、每个 key 各一个窗口
     *
     * @details 为什么不是又一个调用点闸门：`ASYN_LOG_THROTTLED` 的状态长在调用点（函数局部 static），
     *          一个使用处一份。而有一类告警的区分单位是运行期才有的东西——哪个来源 IP、哪台设备、
     *          哪个频道——它们都从同一句 LOG_XXX 出来。共用一份的话第一个坏来源会把其余来源的告警
     *          一起压掉（正是 LogThrottle 那条 @warning 说的情形），调用方自己按 key 建表又要每人
     *          写一遍「表 + 锁 + 上界」，而这个上界漏写就是一个远端可驱动的内存增长点。
     *
     * @note 代价与 LogThrottle 相反，这里说清楚：本表每次判定要拿一次锁并哈希一次 key。
     *        LogThrottle 那份「只有原子量、没有查表」的形状是为调用点闸门保的，按 key 分档保不住——
     *        状态得有个地方按 key 存。相比一条被压掉的日志本来要付的格式化与落盘，这笔钱小得多；
     *        但别把它挂在每请求必过的路上。
     * @note 表有上界（kMaximumTrackedKeys），满时淘汰最久未触碰的那条。被淘汰的 key 下次再来时窗口
     *        是新的，也就是会再多放行一条：**内存封顶换极端基数下漏一条**，这是有意的取舍。
     *        因此 key 不要取每请求唯一量（请求 id、时间戳）——那会把表变成一台轮转器，每个 key 都压不住
     * @note 同一 key 的窗口以**第一次建目**时传入的 interval 为准，后续传入值只在该 key 被淘汰后重建时生效
     * @see ASYN_LOG_THROTTLED_KEYED
     */
    class ASYN_BASE_API LogThrottleRegistry
    {
    public:
        /// 同时跟踪的 key 数上限：到量按最久未触碰淘汰（理由见类头的内存封顶那条取舍）
        static constexpr std::size_t kMaximumTrackedKeys = 512;

        /**
         * @brief 全局唯一的那份表
         * @return 表引用
         * @note 刻意用函数局部 static 而不是挂在 LoggerRegistry 上：闸门表要在日志器还没建好之前
         *       就能用（启动失败路径也走这里），挂在注册表上会让两者的初始化顺序变成一个要操心的问题
         */
        [[nodiscard]] static LogThrottleRegistry &instance() noexcept;

        /**
         * @brief 问一句：这个 key 的这条该写吗
         * @param key      区分单位（来源 IP、设备号、频道名一类基数有限的文本）
         * @param interval 放行间隔；0 或负数表示这个 key 不压（每条都放行）
         * @return LogThrottleDecision 放行时带上这一段被压掉的条数，供调用方写进那一条里
         * @note 不抛：表建不起来（内存不够）时**放行**而不是压掉——压掉的那一条没有任何地方补记，
         *       运维会看到「这个告警从此再没响过」，那比洪水更难查
         */
        [[nodiscard]] LogThrottleDecision acquire(std::string_view key, std::chrono::milliseconds interval) noexcept;

        /**
         * @brief 当前表里有多少个 key
         * @return std::size_t 条数，恒不超过 kMaximumTrackedKeys
         * @note 给用例与运维读数用；本表只增到上界后开始轮转，这个数封顶就说明 key 基数超了
         */
        [[nodiscard]] std::size_t trackedKeyCount() const noexcept;

    private:
        /// 空表：上界的取舍交给淘汰，构造本身不预分配
        LogThrottleRegistry() = default;

        /**
         * @brief 一条 key 的闸门与它的键文本
         * @details 键文本在这里留一份：淘汰时从表尾那头要能认出该删索引里的哪一个
         */
        struct Entry
        {
            /**
             * @brief 建一条目
             * @param keyText 键文本（移动进来）
             * @param interval 该 key 的放行间隔
             */
            Entry(std::string keyText, std::chrono::milliseconds interval) : key(std::move(keyText)), throttle(interval)
            {
            }

            std::string key;      ///< 键文本
            LogThrottle throttle; ///< 这个 key 的闸门
        };

        /**
         * @brief 字符串视图的透明哈希：查表时不必先把视图做成临时 std::string
         * @details 与 Router/MySqlConnection 里那两份同形状——标准库的 std::hash<std::string> 没有
         *          is_transparent，异质查找就要自己补哈希与 std::equal_to<> 两样，缺一不成立
         */
        struct TransparentStringHash
        {
            using is_transparent = void; ///< 开启 unordered_map 的异构查找

            /**
             * @brief 计算视图的哈希
             * @param text 待哈希的视图
             * @return std::size_t 哈希值（与 std::hash<std::string_view> 一致）
             */
            [[nodiscard]] std::size_t operator()(const std::string_view text) const noexcept
            {
                return std::hash<std::string_view>{}(text);
            }
        };

        /// 触碰顺序表：表头＝最近触碰，表尾＝淘汰候选。用 list 是因为节点里的条目地址稳定，
        /// 索引里存的迭代器不会因别的插入被挪走（LogThrottle 不可移动，也就不能整块搬家）
        std::list<Entry> m_touchOrder;
        /// key → 表里的位置
        std::unordered_map<std::string, std::list<Entry>::iterator, TransparentStringHash, std::equal_to<>> m_index;
        /// 保护上面两份：判定要读改触碰顺序，不是只读原子量能覆盖的形状
        mutable std::mutex m_mutex;
    };

    /**
     * @brief 按运行期的 key 问一句「这条该写吗」（一个调用点、每个 key 各一份窗口）
     * @param key      区分单位，任何可隐式换成 std::string_view 的文本
     * @param interval 该 key 的放行间隔（首次建目时定下，见 LogThrottleRegistry 的 @note）
     * @return 该 key 这一条的判定结果
     * @details 与 ASYN_LOG_THROTTLED 同一形态（宏交回一个值，写不写、写什么仍由调用方定）：
     *          @code
     *          if (const auto decision = ASYN_LOG_THROTTLED_KEYED(sourceIp, std::chrono::seconds{10}); decision.isPassed)
     *          {
     *              LOG_WARN_FMT("……（该来源过去 10 秒内另有 {} 条同类被压掉）", decision.droppedCount);
     *          }
     *          @endcode
     * @note 上界与淘汰的取舍、以及「key 不要取每请求唯一量」都写在 LogThrottleRegistry 的类头里
     */
#define ASYN_LOG_THROTTLED_KEYED(key, interval) ::AsynGyanis::Base::LogThrottleRegistry::instance().acquire((key), (interval))

} // namespace AsynGyanis::Base
