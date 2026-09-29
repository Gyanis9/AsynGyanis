/**
 * @file Tracer.h
 * @brief 链路节的编排器：决定采不采、攒成批、交给出口，并把丢了都少报出来
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Tracing/Span.h"
#include "Net/Tracing/SpanExporter.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 链路节的编排器：一具进程内的「采 → 攒 → 送」机器
     * @details 三条职责各有边界：采样只在开新链路时由本端比例决定；收口的节进一条有上界的缓冲，
     *          由本类自己的出口线程按批量或时限送出去；丢掉每一条都计入计数——观测设施自己丢数据
     *          却不吭声，比没有观测设施更糟。
     * @note 事件循环线程只做「往缓冲里放一条」这一件事，文件写入与网络请求都在出口线程上。
     * @note 必须用 create() 建、用 std::shared_ptr 持有：每条节都回指本编排器，而协程帧里的节
     *       可能活得比调用栈久，引用计数是唯一能把这件事讲清楚的形状。
     * @note 缓冲满时丢新到的而不是丢最旧的：链路检索要的是「一次请求的全部节」，剪掉已经开始
     *       出口的那一批的开头，比让新请求没有链路更难查。
     * @see Span, SpanExporter, Traceparent
     */
    class ASYN_NET_API Tracer : public std::enable_shared_from_this<Tracer>
    {
    public:
        /// @brief 节的收口要往本类的缓冲里放，那条通道不对外公开
        friend class Span;

        /**
         * @brief 编排参数
         */
        struct Configuration
        {
            std::string               serviceName{};                 ///< 必填：本进程的服务名，出口侧的 service.name
            std::string               serviceVersion{};              ///< 可空：非空时随 resource 一起报出
            double                    sampleRatio{1.0};              ///< 新链路的采样比例，0.0~1.0；上游点名要采的不受它影响
            std::size_t               maximumPendingSpanCount{8192}; ///< 待出口缓冲的条数上界，越界只丢不再收
            std::size_t               exportBatchSpanCount{512};     ///< 攒够这么多就唤一次出口线程
            std::chrono::milliseconds exportInterval{2000};          ///< 没攒够也在这个点上出口一次
        };

        /**
         * @brief 建一具编排器并起出口线程
         * @param configuration 编排参数
         * @throws Base::LogicException 配置不成立：服务名为空、采样比例不在 0.0~1.0（NaN 也算）、
         *         缓冲上界为 0、批量为 0、出口时限不大于 0
         * @note 缓冲上界与批量都会被钳到实现认定的上界（见 .cpp 里的常量），钳制本身静默：
         *       配错的方向是「占更多内存」，钳制就是把它拉回有界，不需要为此让启动失败
         */
        [[nodiscard]] static std::shared_ptr<Tracer> create(Configuration configuration);

        Tracer(const Tracer &)            = delete;
        Tracer &operator=(const Tracer &) = delete;
        Tracer(Tracer &&)                 = delete;
        Tracer &operator=(Tracer &&)      = delete;

        /**
         * @brief 析构：请求停止、排空残留的节、关掉各出口
         * @details 残留的节要尽力送出去——服务正常退出时最后那一批里有正在处理的请求的链路，
         *          当场丢掉等于把最需要看的那段日志扔了。送不出去的部分已计入 droppedSpanCount()。
         */
        ~Tracer();

        /**
         * @brief 挂一个出口
         * @details 可以在任何时刻挂（包括已经跑起来之后）：先挂出口还是先起服务都不该改变可见行为，
         *          而构造期就要把出口配齐会让「按配置决定是否发 OTLP」这种写法变别扭。
         * @param exporter 出口；nullptr 按「没挂」处理，不改状态也不报错
         * @note 一具编排器可以挂多个出口（例如同时落文件与发 OTLP）。没有出口时缓冲照样收，
         *       收到上界就开始丢——那是一份看得见的浪费，droppedSpanCount() 会一路涨
         */
        void addExporter(std::shared_ptr<SpanExporter> exporter);

        /// @brief 已挂的出口数
        [[nodiscard]] std::size_t exporterCount() const noexcept;

        /**
         * @brief 开一节
         * @details 无 parent 时开一条新链路并按 sampleRatio 判采不采；有 parent 时沿用它的 trace-id、
         *          把 parent 的段标识当作上一节、并**照办 parent 的采样位**（W3C §3.2.1.3：判定是
         *          链路上游做的，下游只能遵从）。不采的一节返回非记录的替身——它仍然带着合法的
         *          链路标识，可以原样往下游传，于是「不采」不会让下游变成一条没有上文的孤立链路。
         * @param name 操作名
         * @param kind 本节在链路上的角色
         * @param parent 上游交来的链路上下文（extractTraceContext() 的结果，或上一节的 identifiers()）
         * @return Span 记录中的节，或非记录的替身
         */
        [[nodiscard]] Span startSpan(std::string_view name, SpanKind kind = SpanKind::Internal, const std::optional<TraceIdentifiers> &parent = std::nullopt);

        /**
         * @brief 等当前已受理的节全部交给出口
         * @details 水位取进场那一刻的受理数：出口线程取出之后仍可能没送完，等「缓冲空」会提前返回；
         *          而等「清零」在有持续生产者的进程里永远不成立——后来者的账不是本次要等的账。
         * @note 给收尾与用例用。运行期不要把它挂在请求路径上：那等于把「异步出口」退化成同步出口。
         * @note 停止已请求时立即返回：那时缓冲里的残留由析构负责送出去，这里再等就等不到了
         */
        void flush();

        /// @brief 缓冲里还压着多少条没交给出口
        [[nodiscard]] std::size_t pendingSpanCount() const noexcept;

        /// @brief 已交给出口的条数（按每条计；一批有多个出口时也只计一次）
        [[nodiscard]] std::uint64_t exportedSpanCount() const noexcept;

        /// @brief 被丢掉的条数：缓冲满、出口整批没收、停止窗口内收下的，都算在这里
        [[nodiscard]] std::uint64_t droppedSpanCount() const noexcept;

        /// @brief 出口整批没收的次数（按批计，与 droppedSpanCount() 的条数口径互补）
        [[nodiscard]] std::uint64_t exportFailureCount() const noexcept;

    private:
        /// @brief 构造只由 create() 走：保证实例一定由 shared_ptr 持有
        explicit Tracer(Configuration configuration);

        /// @brief 把一条收口的节放进缓冲；扩容失败与超出上界都咽下并计入丢弃数，返回是否收下了
        bool accept(SpanRecord &&record) noexcept;

        /// @brief 出口线程主体
        void workerLoop(const std::stop_token &stopToken);

        /**
         * @brief 从缓冲里取走最多一批节（在锁内）
         * @return std::vector<SpanRecord> 取走的部分；空表示没有可取的
         */
        std::vector<SpanRecord> takeBatchLocked(std::size_t maximumCount);

        /**
         * @brief 把一批交给所有出口（锁外执行）
         * @return std::size_t 这批被送出去的条数（某个出口整批没收时计为丢弃，不当成功）
         */
        std::size_t dispatchBatch(std::vector<SpanRecord> &&batch);

        /// @brief 判定「按本端比例，这条新链路是否要采」
        [[nodiscard]] bool shouldSampleNewTrace(const TraceIdentifiers &identifiers) const noexcept;

        Configuration                              m_configuration;         ///< 生效的编排参数（可能被钳制过上界）
        TraceResource                              m_resource{};            ///< 出口侧的出处标识，构造时从配置折出
        mutable std::mutex                         m_stateMutex;            ///< 护住缓冲、出口表与批量水位计数
        std::deque<SpanRecord>                     m_pending{};             ///< 待出口的节，按收口顺序
        std::vector<std::shared_ptr<SpanExporter>> m_exporters{};           ///< 出口表
        std::condition_variable                    m_workCondition;         ///< 叫出口线程：有批可走了
        std::condition_variable                    m_drainedCondition;      ///< 叫 flush() 的等待者：水位已越过
        std::uint64_t                              m_producedCount{0};      ///< 累计受理条数（锁内读）
        std::uint64_t                              m_settledCount{0};       ///< 累计已交出口条数（锁内读）
        std::size_t                                m_flushRequestCount{0};  ///< 正在等 flush() 的线程数：批量没攒够时也照样交付
        std::atomic<std::uint64_t>                 m_exportedSpanCount{0};  ///< 已交出去的条数
        std::atomic<std::uint64_t>                 m_droppedSpanCount{0};   ///< 被丢掉的条数（缓冲满、出口没收、停止窗口）
        std::atomic<std::uint64_t>                 m_exportFailureCount{0}; ///< 出口整批没收的次数（按批计，不按条）
        std::jthread                               m_workerThread{};        ///< 出口线程
        std::stop_token                            m_stopToken{};           ///< 与出口线程同一个停止来源
    };
} // namespace AsynGyanis::Net
