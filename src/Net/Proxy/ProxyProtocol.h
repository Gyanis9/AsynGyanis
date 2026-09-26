/**
 * @file ProxyProtocol.h
 * @brief PROXY 协议（v1 文本行与 v2 二进制块）的读侧解析：把负载均衡器交来的原始四层身份拆成地址对
 * @author Gyanis
 * @date 2026-09-26
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include "Core/Socket/InetAddress.h"

#include <cstddef>
#include <optional>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief v1（文本行）头的总长度上限
     * @details HAProxy 规定一条 v1 头不超过 107 字节 + CRLF，取 108 作硬上界：读侧没有这个上界，
     *          一个只发 `PROXY ` 然后一字节一挤的对端就能把接收缓冲撑住
     */
    inline constexpr std::size_t kMaximumProxyHeaderV1Bytes = 108U;

    /**
     * @brief v2（二进制块）头的总长度上限（含 16 字节定长段与 TLV）
     * @details v2 的长度字段是 16 位，理论上允许 64 KiB−16；实际地址块最多 36 字节（IPv6 两端 +
     *          两个端口），其余都是 TLV。1 KiB 足够装下今天所有已定义的 TLV，又让恶意对端没法用
     *          一个谎报的长度字段把服务器拖进大缓冲
     */
    inline constexpr std::size_t kMaximumProxyHeaderV2Bytes = 1024U;

    /**
     * @brief 一条 PROXY 头交出的四层身份
     */
    struct ProxyEndpoint
    {
        Core::InetAddress source{};       ///< 真实客户端地址（协议里写的「源」）
        Core::InetAddress destination{};  ///< 被代理的本端地址（协议里写的「目的」）
        bool hasAddresses{false};         ///< 这条头是否带地址：v1 的 `UNKNOWN` 与 v2 的 LOCAL 都不带
    };

    /**
     * @brief 已读到的前缀能给读侧的答复：还需不需要继续读、这条连接是不是已经判死
     */
    struct ProxyHeaderFraming
    {
        bool isStillPlausible{true};              ///< 前缀仍可能是一条头；为 false 时读侧应立刻判死，别再等
        std::optional<std::size_t> totalLength{}; ///< 已能定出整条头的字节数；还没定出时为空
    };

    /**
     * @brief 根据已读到的前缀判断这条头总共多少字节、还读不读得下去
     * @param buffered 已读到的字节（可以是半条头）
     * @return ProxyHeaderFraming 见结构体两个字段的说明
     * @details v1 靠 `PROXY ` 前缀认出、整条以第一个 CRLF 收尾（长度受 kMaximumProxyHeaderV1Bytes
     *          约束）；v2 靠 12 字节签名认出、整条长度 = 16 + 长度字段（受
     *          kMaximumProxyHeaderV2Bytes 约束）。签名对不齐的两种情形：前缀太短还判不出（继续读）、
     *          已经能断定不是头（例如以 `GET /` 开头——这是直接把服务器暴露在了公网上的形状）。
     */
    [[nodiscard]] ProxyHeaderFraming frameProxyHeader(std::string_view buffered) noexcept;

    /**
     * @brief 解析一条**完整**的 PROXY 头
     * @param header 头的全部字节（可以比头本身长：只解析最前面那一条，多余部分归调用方的下一段读）
     * @param consumedBytes 输出参数：本次吃掉的字节数（v1 含结尾 CRLF，v2 含 TLV）；**解析失败时置 0**，
     *        调用方因此不必分辨「没动」与「没吃」——失败就是什么都没吃
     * @return std::optional<ProxyEndpoint> 解析成功交出身份；格式不合规范交出空
     * @note 不合规就返回空而不是抛：读侧的处置是「关掉这条连接并记一条日志」，一条报文格式问题不该
     *       把异常穿过接受循环
     * @note v1 的 `UNKNOWN` 与 v2 的 `LOCAL` 都会交出 `hasAddresses=false`：这两类头只说明
     *       「前面确实有个代理」，没有可当作客户端的身份。把它们当地址用等于用一个不存在的来源记账
     */
    [[nodiscard]] std::optional<ProxyEndpoint> parseProxyHeader(std::string_view header,
                                                                std::size_t &consumedBytes) noexcept;
} // namespace AsynGyanis::Net
