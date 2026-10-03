/**
 * @file ProcessMetricsRegistry.h
 * @brief 进程级读数的登记处：不进 `HttpServerStats` 的那些计数在这里挂一条，抓 `/metrics` 时一并导出
 * @author Gyanis
 * @date 2026-09-30
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    /**
     * @brief 判断一段文本是否是合法的 Prometheus 指标名（或其前缀段）
     *
     * @details 合法形状是 `[a-zA-Z_:][a-zA-Z0-9_:]*`。登记侧（`ProcessMetricsRegistry::registerMetric`）
     *          与渲染侧（`/metrics` 的 `metric_name_prefix`）共用这一份判据：两处各写一遍就会出现
     *          「登记当场拒、抓取时静默产出一整片抓取端拒收的行」——后者更难查，因为端点照回 200。
     *
     * @param name 待判的名字，或名字的前缀段
     * @return true 非空且每一段都符合上述字符集（首字符不许是数字）
     * @return false 空串，或含上述集合之外的字符
     */
    [[nodiscard]] ASYN_CORE_API bool isLegalPrometheusMetricName(std::string_view name) noexcept;

    /**
     * @brief 一条读数的类型，决定导出时的 `# TYPE` 与采集侧的用法
     */
    enum class ProcessMetricKind
    {
        Counter, ///< 单调递增：只关心涨了多少，采集侧做 rate()
        Gauge    ///< 瞬时量：现在是多少（活跃连接数、到期时刻、池内在借数）
    };

    /**
     * @brief 同一个名字上挂着多条实例时，怎么并成一个数
     *
     * @details 两条 `UdpServer` 各登记一份收包数时，运维要的是进程总量，不是其中一台的份额；
     *          而两个证书管理器各报自己的到期时刻时，求和会得到一个谁也没配的数——那时该报
     *          **最早**的那张，因为它才是会先出事的那个。取最大没有对应的告警语义，故不提供。
     */
    enum class ProcessMetricMerge
    {
        Sum,       ///< 各实例相加（计数类）
        Min,       ///< 取最小值（「最早的那个期限」这类，0 也算一个真实的最小值）
        MinNonZero ///< 取最小值但**跳过 0**：有些读数里 0 的含义是「读不出/还没有」而不是「最早」，
                   ///< 让它参与求早会把另一个实例的真值压成假的告警（证书到期时刻即此形）
    };

    /// 取一次读数的回调。约定：只准读自己对象里的原子量；不准在里头碰注册表（注销要拿写锁，会自锁死），也不准阻塞
    using ProcessMetricProvider = std::function<std::uint64_t()>;

    /**
     * @brief `samples()` 交出的一份快照：渲染方要的东西全在里面，不再回调 provider
     */
    struct ASYN_CORE_API ProcessMetricSample
    {
        std::string       name;                           ///< 完整的指标名（不带前缀，见下面的命名约定）
        std::string       help;                           ///< 登记时给的那句说明，原样导出
        ProcessMetricKind kind{ProcessMetricKind::Gauge}; ///< 导出的 `# TYPE`
        std::uint64_t     value{0};                       ///< 按 merge 并好的读数
    };

    class ProcessMetricsRegistry;

    /**
     * @brief 一条登记的把手：析构即注销，不留悬空的 provider
     *
     * @details 持有方通常是那个被观测的对象本身（一个成员）。把手不注销就交给进程退出，
     *          那条读数会一直留在导出里——注销语义刻意交给 RAII，而不是「记得调 unregister」。
     */
    class ASYN_CORE_API ProcessMetricHandle
    {
    public:
        ProcessMetricHandle() noexcept = default;

        /// 注销这条登记（幂等：空把手什么都不做）
        ~ProcessMetricHandle();

        ProcessMetricHandle(const ProcessMetricHandle &)            = delete;
        ProcessMetricHandle &operator=(const ProcessMetricHandle &) = delete;
        ProcessMetricHandle(ProcessMetricHandle &&other) noexcept;
        ProcessMetricHandle &operator=(ProcessMetricHandle &&other) noexcept;

        /// 有没有握着一条登记
        [[nodiscard]] bool isValid() const noexcept
        {
            return m_registered;
        }

    private:
        friend class ProcessMetricsRegistry;

        ProcessMetricHandle(std::string name, std::uint64_t sequence) : m_name(std::move(name)), m_sequence(sequence), m_registered(true)
        {
        }

        std::string   m_name{};            ///< 登记时用的指标名
        std::uint64_t m_sequence{0};       ///< 同名多条实例时区分「是我这条」的序号
        bool          m_registered{false}; ///< 是否握着一条登记
    };

    /**
     * @brief 进程级读数的注册表。
     *
     * @details 为什么要有这么一层：`/metrics` 的渲染入口只吃 `Net::HttpServerStats` 的字段，
     *          于是**不进那份结构体的读数必然无出口**。把 UdpServer、连接池、ACME 的计数塞进
     *          `HttpServerStats` 是伪造依赖（它们不是 HTTP 服务器的状态，也不该被 HTTP 层知道），
     *          而且 `Database` 与 `Net` 是兄弟模块，池的读数根本到不了渲染点。这一层放在 `Core`
     *          （`Platform ← Base ← Core ← {Net, Database}` 的共同下层），Core 只记账、
     *          Net 那侧负责渲染成 Prometheus 文本。
     *
     * @note 命名约定：**登记的是完整名字**，不随 `/metrics` 的 `metric_name_prefix` 变。
     *       进程级读数抓哪台监听器都是同一份，给它套某个服务器的业务前缀反而会让人以为
     *       那台服务器独占这个数。建议形状 `asyn_<面>_<指标>_[total|seconds|bytes]`，
     *       计数类一律以 `_total` 结尾（Prometheus 的既有约定）。
     * @note 同名登记是支持的（多台同类对象各登记一份，按 `merge` 并成一个数）；但**同一个名字上的
     *       类型、说明与并法必须一致**——不一致就是两处代码抢一个名字，当场抛出来比导出两份
     *       互相矛盾的 `# TYPE` 好得多。
     */
    class ASYN_CORE_API ProcessMetricsRegistry
    {
    public:
        /**
         * @brief 登记一条进程级读数
         *
         * @param name 完整的指标名，必须匹配 `[a-zA-Z_:][a-zA-Z0-9_:]*`
         * @param help 一句中文说明，导出成 `# HELP`；同名重复登记时它必须与先登记的完全一致
         * @param kind 计数还是瞬时量，决定 `# TYPE`
         * @param merge 同名多条实例时怎么并
         * @param provider 取值回调，会被**每次抓取**调用一次
         * @return ProcessMetricHandle 注销把手；丢掉把手就等于把这条读数从导出里拿掉
         * @throws std::invalid_argument 名字不合法、provider 为空，或同名登记的类型/说明/并法不一致
         * @note 抓取时 provider 在注册表的读锁内被调用：它必须只做原子量读取，且不得回头调用注册表的
         *       任何接口（自锁死），也不得阻塞——那会把整个 `/metrics` 端点拖住
         */
        [[nodiscard]] static ProcessMetricHandle registerMetric(std::string name, std::string help, ProcessMetricKind kind, ProcessMetricMerge merge,
                                                                ProcessMetricProvider provider);

        /**
         * @brief 取当前所有登记的读数快照，按登记先后给出
         * @return std::vector<ProcessMetricSample> 每个名字一份（同名已按 merge 并好）
         * @note 抓取是**现取**而不是缓存：注册表里存的是回调，值在被抓的那一刻才读
         */
        [[nodiscard]] static std::vector<ProcessMetricSample> samples();

        /**
         * @brief 当前登记条数（按名字去重后）
         * @details 给用例与自检用：「注册表是空的」和「注册表没被接到出口上」是两种不同的失败，
         *          需要一个能数的量把它们分开
         */
        [[nodiscard]] static std::size_t nameCount();
    };
} // namespace AsynGyanis::Core
