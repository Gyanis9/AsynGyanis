/**
 * @file TraceContext.h
 * @brief W3C Trace Context：traceparent 的解析/校验/生成，与 tracestate 的条目管理
 * @author Gyanis
 * @date 2026-09-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Net/Http/HttpRequest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /// 链路上下文的入站头名（小写）：头部名按「大小写不敏感」读取，这里给的是存储形态
    inline constexpr std::string_view kTraceparentHeaderName = "traceparent";
    inline constexpr std::string_view kTracestateHeaderName  = "tracestate";

    /// trace-id 与 parent-id 的十六进制位数（W3C Trace Context §3.2.1.1 / §3.2.1.2）
    inline constexpr std::size_t kTraceIdHexDigitCount = 32;
    inline constexpr std::size_t kSpanIdHexDigitCount  = 16;

    /**
     * @brief 版本 0 的 traceparent 定长：`00-`(3) + 32 + `-`(1) + 16 + `-`(1) + 2
     * @details 公开这条常量不是为了少写一个字面量，而是给「按长度就能从日志里截出字段」的调用方
     *          一个与校验侧同源的数——两处各写 55 时，改一处就会让截断与判定分叉。
     */
    inline constexpr std::size_t kTraceparentCanonicalLength = 3U + kTraceIdHexDigitCount + 1U + kSpanIdHexDigitCount + 3U;

    /// tracestate 的条目上限：规范允许「收到太多条就删掉若干条」，因此这里是**截断**而不是拒绝
    inline constexpr std::size_t kMaximumTraceStateEntryCount = 32;

    /// tracestate 单条键或值的长度上限（§3.2.3 的 1*256 / 0*256）
    inline constexpr std::size_t kMaximumTraceStateTokenLength = 256;

    /// trace-flags 里唯一有语义的一位：其余位必须原样传递、收到时忽略（§3.2.1.3）
    inline constexpr std::uint8_t kSampledTraceFlag = 0x01U;

    /**
     * @brief 一条链路上的两个标识与采样位：本框架内的「当前上下文」值对象
     *
     * @details 三段都是定长小写十六进制文本（含结尾 NUL 的数组），因此它可以按值拷贝、放进
     *          请求对象而不向堆要一块内存——链路上下文是每条被跟踪的请求都要付的东西，
     *          用 std::string 装等于给热路径加两次分配。
     * @note 文本恒为小写：规范里 trace-id 与 parent-id 就是小写十六进制，大写一律视为非法输入。
     */
    struct TraceIdentifiers
    {
        std::array<char, kTraceIdHexDigitCount + 1U> traceId{};  ///< 32 位小写十六进制 + NUL
        std::array<char, kSpanIdHexDigitCount + 1U>  parentId{}; ///< 16 位小写十六进制 + NUL
        std::uint8_t                                 flags{0};   ///< trace-flags 的原值（含未定义的位）
        std::uint8_t                                 version{0}; ///< 收到的版本号；本实现只生成 0

        /// @brief 链路标识文本（32 位十六进制），不含 NUL
        [[nodiscard]] std::string_view traceIdText() const noexcept;

        /// @brief 来源段标识文本（16 位十六进制），不含 NUL
        [[nodiscard]] std::string_view parentIdText() const noexcept;

        /// @brief 采样位：非零表示上游已经决定「这条链路要采」
        [[nodiscard]] bool isSampled() const noexcept;

        /**
         * @brief 把采样位写成或抹掉，其余位原样保留
         * @param isSampled true 置上 kSampledTraceFlag，false 只清这一位
         */
        void setSampled(const bool isSampled) noexcept;
    };

    /**
     * @brief traceparent 字段的解析与生成（W3C Trace Context §3.2）
     *
     * @details 线上形态是 `version "-" trace-id "-" parent-id "-" trace-flags`，版本 0 时定长 55 字节。
     *          本类只负责「读」与「发新链路」两件事，刻意不做改写：
     *          · 收到的字段合法就**原样沿用**（含未知高版本的附加字段），一个字都不动——
     *            规范要求下游不得因为看不懂就拒绝或修剪上游的字段；
     *          · 缺席或非法才生成一条新的（版本固定 0），此时才走 renderInto。
     *          因此这里不需要「保留未知字段再回写」的缓冲，也就没有为它预留的容量。
     */
    class Traceparent
    {
    public:
        /**
         * @brief 解析并校验一个 traceparent 取值
         * @details 逐条按规范的 ABNF 判：分隔符位置、每段长度、字符集（只收小写十六进制）、
         *          版本 `ff` 非法、全零的 trace-id 与 parent-id 非法、版本 0 的取值必须恰好 55 字节、
         *          更高版本允许在 flags 之前多带 `-` 分隔的附加字段（忽略其含义但整条照旧采信）。
         * @param value 头部取值（首尾 OWS 由解析器裁掉，这里不再宽免空白）
         * @return std::optional<TraceIdentifiers> 合法时给出三段标识；任何一条不合都返回空
         */
        [[nodiscard]] static std::optional<TraceIdentifiers> parse(std::string_view value) noexcept;

        /**
         * @brief 生成一条全新链路的标识：随机的 32 位 trace-id 与 16 位 parent-id
         * @details 随机源是线程局部的 splitmix64 流，用 std::random_device 播一次种——
         *          链路标识只要「跨进程不撞」，不做密码学用途（与 request-id 的定长序号是两回事：
         *          后者要能从日志里按长度截出来，前者要全局唯一）。
         * @param isSampled 采样位初值
         * @return TraceIdentifiers 版本 0 的新标识
         */
        [[nodiscard]] static TraceIdentifiers generate(const bool isSampled = true) noexcept;

        /**
         * @brief 把标识渲染成线上形态（`00-…-…-01`）写进给定缓冲：容量随首次留下
         * @details 交给「每条请求都要渲染一次」的调用方复用同一块缓冲；要一份新串请直接用 value()。
         * @param target 写入目标，返回后其长度恰为该字段的线上长度
         * @param identifiers 待渲染的标识
         */
        static void renderInto(std::string &target, const TraceIdentifiers &identifiers);

        /**
         * @brief 把标识渲染成线上形态（交出一份新串）
         * @param identifiers 待渲染的标识
         * @return std::string 版本 0 时为 55 字节
         */
        [[nodiscard]] static std::string value(const TraceIdentifiers &identifiers);
    };

    /**
     * @brief tracestate 的一条键值（§3.2.3）
     */
    struct TraceStateEntry
    {
        std::string key{};   ///< 系统名（小写字符集，可带 `租户@厂商` 形态）
        std::string value{}; ///< 该系统自己的取值，可为空串
    };

    /**
     * @brief tracestate 的条目表：解析、按规范改写与渲染
     *
     * @details 与 traceparent 不同，本表**必须**能改写：任何往链路上加自己条目的系统都要把
     *          自己的键挪到最前、并丢掉后面同名的旧条目（§3.2.4.1 的三步：插入到头部、去重、截断到 32）。
     *          这里的三条纪律：
     *          · 解析要么全成要么全废——规范把「有重复键」整条判为无效，宁可丢掉全部条目
     *            也不要在两家系统之间留下「一半可信」的状态；
     *          · 键与值的字符集按 §3.2.3 收紧（`congo` 与 `tircongo` 直接判非法，那是规范里点名作废的键）；
     *          · 表本身按值语义，键与值各自持串：只有显式启用 tracestate 才会有分配，
     *            默认关闭（见 TraceContextOptions::vendorKey）。
     */
    class TraceState
    {
    public:
        /**
         * @brief 解析一个 tracestate 取值
         * @details 逗号分隔，逗号两侧空白（OWS）不算内容；键为空的条目、缺等号的条目、
         *          超长的键或值、字符集越界、以及重复的键都判**整条**无效（规范把重复键整条判废，
         *          半条可信比全都不可信更糟）。条目数超过 32 不判废：规范许可「收到过多条目时
         *          删掉若干条」，因此这里保留顺序、由 upsertFront 与 renderInto 从尾部截断。
         * @param value 头部取值
         * @return std::optional<TraceState> 有效时给出条目表；无效时为空
         */
        [[nodiscard]] static std::optional<TraceState> parse(std::string_view value);

        /**
         * @brief 判定一个 tracestate 键是否合法
         * @param key 待判定的键
         * @return true 合法
         * @return false 字符集越界、长度越界、首尾不合规定，或落在点名作废的两个键上
         */
        [[nodiscard]] static bool isValidKey(std::string_view key) noexcept;

        /**
         * @brief 判定一个 tracestate 值是否合法（不含首尾空白，长度 0~256，禁 `,` `=` `;` 与控制字符）
         * @param value 待判定的值
         * @return true 合法
         */
        [[nodiscard]] static bool isValidValue(std::string_view value) noexcept;

        /**
         * @brief 把一个条目按规范挪到最前：同名旧条目被丢弃，超出 32 条的部分从尾部截断
         * @details 键或值不合规范时不改表也不报错——调用方拿到 false，自己决定是「不加」还是「拒绝请求」。
         * @param key 系统名
         * @param value 该系统自己的取值
         * @return true 已写入（或因容量截断而少留了尾部条目，仍算成功）
         * @return false 键或值非法，表未改动
         */
        bool upsertFront(std::string_view key, std::string_view value);

        /// @brief 当前条目数。parse 可以给出多于 32 条的一份表（那是上游发来的原样），截断发生在写入与渲染时
        [[nodiscard]] std::size_t entryCount() const noexcept;

        /// @brief 条目表，按线上顺序（最前面是最后写入的系统）
        [[nodiscard]] const std::vector<TraceStateEntry> &entries() const noexcept;

        /**
         * @brief 渲染成线上形态（`k1=v1,k2=v2`）写进给定缓冲
         * @param target 写入目标；容量复用，长度按本次结果改
         * @param maximumEntryCount 只渲染最前这么多条（默认全渲染）；用于把长度压在上限内
         */
        void renderInto(std::string &target, std::size_t maximumEntryCount = kMaximumTraceStateEntryCount) const;

    private:
        std::vector<TraceStateEntry> m_entries; ///< 条目表，按线上顺序（最前面是最后写入的系统）
    };

    /**
     * @brief 从请求上读出当前的链路上下文
     *
     * @details 读的就是 traceparent 头部本身，不是另存一份：那条头部是上下文的权威形态，
     *          traceContextMiddleware 归一化之后「头部」与「上下文」不可能分叉。
     *          每次读都要过一遍 55 字节的定长校验（无分配、无拷贝），换掉的是「两处状态谁更新」
     *          这一类长期 bug——对只读上下文的处理器来说这笔买卖划算。
     * @param request 已收齐的请求对象
     * @return std::optional<TraceIdentifiers> 有效标识；头部缺席、形态不合、或同名出现**多条**时为空
     *         （多条是有歧义的输入，猜首条还是末条都是替上游做决定，一律按「不在任何链路里」处理）
     */
    [[nodiscard]] std::optional<TraceIdentifiers> extractTraceContext(const HttpRequest &request);
} // namespace AsynGyanis::Net
