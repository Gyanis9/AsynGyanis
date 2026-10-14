/**
 * @file QuicOutboundDatagram.h
 * @brief 核心产出、交给外壳发出去的一条 UDP 数据报：报文体与它的 IP ECN 标记
 * @author Gyanis
 * @date 2026-10-09
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace AsynGyanis::Net
{
    /// 路径标识的字节长度：`Core::InetAddress::canonicalBytes()` 的定长（QuicConnection.cpp 里有 static_assert 钉住两者一致）
    inline constexpr std::size_t kQuicPathKeyByteLength = 24;

    /**
     * @brief 一条对端路径的标识（RFC 9000 §9.3 的「路径」＝本端地址与对端地址的二元组，本层只看对端那一半）
     * @details 由 `Core::InetAddress::canonicalBytes()` 从来源地址折出：对端换地址之后这一格就变。
     *          状态机不认识地址本身，只按这个不透明的键记账与路由
     */
    using QuicPathKey = std::array<std::uint8_t, kQuicPathKeyByteLength>;

    /// @return true 这份路径标识全零，即还没有任何路径信息（首条认证过的报文到达之前）
    [[nodiscard]] inline bool isUnspecifiedQuicPathKey(const QuicPathKey &pathKey) noexcept
    {
        return pathKey == QuicPathKey{};
    }

    /**
     * @brief 一条待发数据报：报文体，加上这一条该带哪一格 IP ECN 标记
     * @details 两样必须一起交出去。§13.4.2.1 的 ECN 校验拿「本端当时标了哪一格」当依据去比对端报上来的
     *          计数，而真正把标记交给内核的是外壳（它才知道自己这台机读不读得到 ECN 字段）。
     *          分成「先取报文、再问这条标了什么」两次调用，会在中间状态变化时把线上与账上错开一格——
     *          那一格的错一路传染到验证结论，最坏时把一条好路判成不支持 ECN。
     * @note 本类型放在独立头里是因为两侧的公共面都要用它，而 `QuicConnection.h` 刻意只前向声明
     *       `QuicConnectionCore`（h3 与示例都不该为了一个结构体把整个状态机头拉进来）
     */
    struct ASYN_NET_API QuicOutboundDatagram
    {
        std::string  bytes{};         ///< 报文本体（帧序列已编好、已受保护）
        std::uint8_t ecnCodepoint{0}; ///< 要标进 IP 头 ECN 字段的取值，0 表示这条不标（见 `Platform::kEcnCodepoint*`）
        /**
         * @brief 这一条该发往哪条对端路径（§9.3）
         * @details 全零＝本连接的当前默认地址（外壳按自己记着的对端地址发）；非零＝外壳按收到的
         *          来源地址表换算目标地址。迁移期一个批次里可能混着两个目标：新路径的正常包与
         *          旧路径的复验探测（§9.3.3），所以这一格必须按条给，不能按批给
         */
        QuicPathKey pathKey{};
    };
} // namespace AsynGyanis::Net
