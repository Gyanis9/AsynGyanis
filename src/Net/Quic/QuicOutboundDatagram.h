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

#include <cstdint>
#include <string>

namespace AsynGyanis::Net
{
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
    };
} // namespace AsynGyanis::Net
