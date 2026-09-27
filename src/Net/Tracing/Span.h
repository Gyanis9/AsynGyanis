/**
 * @file Span.h
 * @brief 链路里的一节：记录名字、起止、附加维度与结局，收口后交给 Tracer 出口
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http/TraceContext.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace AsynGyanis::Net
{
    class Tracer;

    /// 一节上允许挂的维度条数上限：越界只丢新来的并计数。设上限的理由是「一节的正文要整条交给出口」，
    /// 无界增长等于让调用方（处理器里一个循环）能把出口内存拉爆
    inline constexpr std::size_t kMaximumSpanAttributeCount = 32U;

    /**
     * @brief 一节在链路上的角色
     * @details 只列本引擎真会产生的三档。OTLP 另有 Producer/Consumer 两档，那是消息队列一侧的说法
     *          （「投递」与「取出」）；本引擎没有 broker 侧的链路，造出来就是一个没有产生方的枚举值。
     *          数值与 OTLP 的 SpanKind 对齐（1 起）不是为了少写映射，而是让出口侧可以直接透传。
     */
    enum class SpanKind : std::uint8_t
    {
        Internal = 1, ///< 进程内的一节：后台任务、批处理阶段
        Server   = 2, ///< 入站请求的服务端一节
        Client   = 3, ///< 出站调用的一节
    };

    /**
     * @brief 一节的结局
     * @details 三档都留着：检索侧「没显式判定过」与「判定为正常」是两个不同的查询条件，
     *          把 Unset 折成 Ok 会让前者永远查不出来。数值与 OTLP 的 Status.code 一致。
     */
    enum class SpanStatusCode : std::uint8_t
    {
        Unset = 0, ///< 没有人显式判定过结局
        Ok    = 1, ///< 显式判定为正常
        Error = 2, ///< 显式判定为失败，说明文字放在 statusMessage 里
    };

    /**
     * @brief 一节上的一个附加维度的取值
     * @details 只有文本与整数两类。OTLP 的 AnyValue 还允许布尔与浮点，这里不做：当前没有产生方，
     *          而每多一类都要在「写入侧、JSON 渲染侧、用例」三处各跟一遍。要加的时候按同一形状扩即可。
     */
    using SpanAttributeValue = std::variant<std::string, std::int64_t>;

    /**
     * @brief 一节上的一个附加维度（键值对）
     */
    struct SpanAttribute
    {
        std::string        key{};   ///< 维度名，沿用 OTel 的语义约定写法（http.request.method 之类）
        SpanAttributeValue value{}; ///< 取值：文本或整数
    };

    /**
     * @brief 一节在链路上的位置：链路标识 + 本节标识 + 上一节标识
     * @details 三段都是定长小写十六进制文本（含结尾 NUL），与 TraceIdentifiers 同一理由：
     *          链路上下文是每条被采的请求都要带的东西，用 std::string 装等于给热路径加三次分配。
     *          parentSpanId 用「首字节是 NUL」表达「本节就是链路的根」——定长数组没有空值，
     *          而全零的段标识在 W3C 里本就是非法值，拿它当哨兵不会与真实标识相撞。
     */
    struct SpanIdentity
    {
        std::array<char, kTraceIdHexDigitCount + 1U> traceId{};      ///< 32 位小写十六进制
        std::array<char, kSpanIdHexDigitCount + 1U>  spanId{};       ///< 16 位：本节自己的标识
        std::array<char, kSpanIdHexDigitCount + 1U>  parentSpanId{}; ///< 16 位：上一节；全 NUL 表示根

        /// @brief 链路标识文本（32 位十六进制），不含 NUL
        [[nodiscard]] std::string_view traceIdText() const noexcept;

        /// @brief 本节标识文本（16 位十六进制），不含 NUL
        [[nodiscard]] std::string_view spanIdText() const noexcept;

        /// @brief 上一节标识文本（16 位十六进制）；本节是根时给出空串
        [[nodiscard]] std::string_view parentSpanIdText() const noexcept;

        /// @brief 本节是否由别的一节引出（false = 链路的根）
        [[nodiscard]] bool hasParent() const noexcept;
    };

    /**
     * @brief 一节收口后的成品：只读值对象，出口侧拿到的就是它
     * @details 起止用「绝对时刻 + 时长」两条轴分开存：绝对时刻来自 system_clock，是检索与对齐用的
     *          （也是 OTLP 要的 startTimeUnixNano）；时长来自 steady_clock，是量出来的，不受
     *          NTP 回拨与夏令时影响。把两者合成一个结束时刻是白扔一条信息——墙钟被往回拨时
     *          「结束早于开始」的节在下游是查不出问题的，只有分开的时长能暴露它。
     */
    struct SpanRecord
    {
        SpanIdentity                          identity{};                    ///< 在链路上的位置
        std::string                           name{};                        ///< 操作名
        SpanKind                              kind{SpanKind::Internal};      ///< 角色
        std::chrono::system_clock::time_point startMoment{};                 ///< 开始时刻（墙钟）
        std::chrono::nanoseconds              duration{0};                   ///< 时长（单调钟量得）
        SpanStatusCode                        status{SpanStatusCode::Unset}; ///< 结局
        std::string                           statusMessage{};               ///< 结局说明，空串表示没有
        std::vector<SpanAttribute>            attributes{};                  ///< 附加维度，按写入顺序
        std::size_t                           droppedAttributeCount{0};      ///< 因超出上限而被丢掉的维度条数
    };

    /**
     * @brief 正在记录的一节
     *
     * @details 生命周期即这一节的时长：构造（由 Tracer::startSpan()）时打开始时刻，
     *          finish() 或析构时把成品交给所属 Tracer 的待出口缓冲。析构也会收口，
     *          为的是「处理器有五六条 return」这种形状下不必每条出口都手写一次 finish()——
     *          漏掉一条不会丢数据，只会让结束时刻变成作用域结束的时刻。
     * @note 未采样的请求上拿到的是一个**非记录的替身**（isRecording() 为假）：写入全是空操作，
     *        于是处理器侧不需要到处判空，链路上下文照样能从这里取出去往下传。
     *        刻意不返回 std::optional\<Span\>：那会把「判一下」的负担摊到每个使用点上，
     *        而替身在这里是完全合法的上下文来源。
     * @note 只能移动、不能拷贝：一节只该被收口一次。
     */
    class Span
    {
    public:
        /**
         * @brief 造一个非记录的替身：没有所属 Tracer，所有写入都是空操作
         * @details 也是「链路上下文缺席时的默认值」，让调用方可以按值持有而不必每处判空。
         */
        Span() noexcept;

        /**
         * @brief 析构：尚未收口就收口
         * @details finish() 本身不抛出（缓冲满或扩容失败都只计一次丢弃），因此这里不存在
         *          「异常逃出析构」这条路。
         */
        ~Span();

        Span(const Span &)            = delete;
        Span &operator=(const Span &) = delete;

        Span(Span &&other) noexcept;
        Span &operator=(Span &&other) noexcept;

        /// @brief 本节是否真的在记录（false = 非记录的替身，或已经收口过）
        [[nodiscard]] bool isRecording() const noexcept;

        /// @brief 操作名
        [[nodiscard]] std::string_view name() const noexcept;

        /**
         * @brief 改操作名
         * @details 给「开始一节时还不知道自己属于哪个操作」的调用方用：HTTP 服务端一节正是这种——
         *          中间件在路由匹配之前就要开始计时，匹配到哪个路由只有处理器结束后才知道。
         * @param name 新名字；非记录时空操作
         */
        void setName(std::string_view name);

        /**
         * @brief 挂一个文本维度：同名已存在就换值并保留原位置
         * @param key 维度名
         * @param value 取值，按文本存
         * @throws std::bad_alloc 维度表要扩容却拿不到内存时抛出（超出条数上限不抛，只丢并计数）
         */
        void setAttribute(std::string_view key, std::string_view value);

        /**
         * @brief 挂一个整数维度：同名已存在就换值并保留原位置
         * @param key 维度名
         * @param value 取值，按 64 位整数存（状态码、字节数这类要能直接比较的量）
         * @throws std::bad_alloc 维度表要扩容却拿不到内存时抛出（超出条数上限不抛，只丢并计数）
         */
        void setAttribute(std::string_view key, std::int64_t value);

        /**
         * @brief 判定结局
         * @details 后写覆盖先写，但一旦判为 Error 就不再被 Ok 抹平：一条节里先失败后「补个成功」
         *          多半是收尾路径上的误判，把错误结局藏起来的代价比反过来大。
         * @param status 结局码
         * @param message 说明文字（空串表示不带；只有 Error 时才需要在检索侧读它）
         */
        void setStatus(SpanStatusCode status, std::string_view message = {});

        /// @brief 本节在链路上的位置（含上一节标识）
        [[nodiscard]] const SpanIdentity &identity() const noexcept;

        /**
         * @brief 把本节换算成「交给下一跳的链路上下文」
         * @details 结果里的 parentId 是**本节**的标识——正是 traceparent 该带的值：下一跳把它当作
         *          自己的上一节。可直接交给 Traceparent::renderInto() 写进出站头部。
         * @return TraceIdentifiers 链路标识 + 本节标识 + 本节携带的采样位
         */
        [[nodiscard]] TraceIdentifiers identifiers() const noexcept;

        /**
         * @brief 收口：把成品交给所属 Tracer 的待出口缓冲
         * @details 幂等；非记录的替身什么也不做。
         * @note 不抛出：缓冲扩容失败时由 Tracer 计一次丢弃并当场咽下。为一条链路把异常送回业务路径
         *       （以及从这里走出去、在析构里终止进程）都不是划算的买卖
         */
        void finish() noexcept;

    private:
        friend class Tracer;

        /**
         * @brief 由 Tracer::startSpan() 调用的真构造：位置、沿用的标志位、名字、角色与所属编排器一并给定
         * @param tracer 所属编排器；空指针表示这是一节非记录的替身，收口时什么也不做
         * @param identity 本节在链路上的位置（含上一节标识）
         * @param flags 要往下传的 trace-flags 原值（含未定义的位）
         * @param version 沿用的 traceparent 版本号
         * @param name 操作名
         * @param kind 角色
         */
        Span(std::shared_ptr<Tracer> tracer, SpanIdentity identity, std::uint8_t flags, std::uint8_t version, std::string_view name, SpanKind kind);

        /// @brief 交出成品（不带走记录状态）：finish() 与析构共用同一条收口路径
        [[nodiscard]] SpanRecord takeRecord() noexcept;

        /**
         * @brief 两个公开的 setAttribute() 重载共用的写入路径：同名换值、超上限则丢并计数
         * @param key 维度名
         * @param value 已折成 variant 的取值
         */
        void writeAttribute(std::string_view key, SpanAttributeValue value);

        std::shared_ptr<Tracer>               m_tracer;                        ///< 所属编排器；空 = 非记录的替身
        SpanIdentity                          m_identity{};                    ///< 在链路上的位置
        std::uint8_t                          m_flags{0};                      ///< trace-flags 原值：未定义的位要照原样往下传
        std::uint8_t                          m_version{0};                    ///< 沿用上游的版本号；本端新开的链路是 0
        std::string                           m_name{};                        ///< 操作名
        SpanKind                              m_kind{SpanKind::Internal};      ///< 角色
        std::chrono::system_clock::time_point m_startMoment{};                 ///< 开始时刻（墙钟），出口正文用它
        std::chrono::steady_clock::time_point m_steadyStart{};                 ///< 开始时刻（单调钟），量时长用它
        SpanStatusCode                        m_status{SpanStatusCode::Unset}; ///< 结局
        std::string                           m_statusMessage{};               ///< 结局说明
        std::vector<SpanAttribute>            m_attributes{};                  ///< 附加维度，按写入顺序
        std::size_t                           m_droppedAttributeCount{0};      ///< 被上限挡下的维度条数
        bool                                  m_isFinished{false};             ///< 是否已收口：析构据此避免二次交出
    };
} // namespace AsynGyanis::Net
