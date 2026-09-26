/**
 * @file HttpContentCoding.h
 * @brief 出站响应的 Content-Encoding 处理：代为声明 Accept-Encoding，并把压缩正文解回来
 * @author Gyanis
 * @date 2026-09-26
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 客户端不解压就等于把「对端压缩过的字节」原样交给业务，而业务多半按文本处理它。
 *          这一份把配套的两件事收在一处：请求侧代为声明本端能接受哪些编码，响应侧按同一判据
 *          解回来。两边必须同源，否则「声明了却不解」与「解了却没声明」这两种半套行为
 *          都能悄悄成立，而且都只在特定对端身上才露出来。
 *
 *          口径对齐 Go 标准库的出站客户端：**只有本端代为加了 Accept-Encoding 时才透明解压**，
 *          解完把 content-encoding 与 content-length 两条一起删掉——前者已被兑现，后者的长度
 *          描述的是压缩前的字节，留着就是让业务按错的长度去数一份新正文。
 */

#pragma once

#include "Net/Http/Client/HttpClient.h"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /// 代为声明的取值：只列本端真能解回来的编码（br/zstd 没有内置解码器，声明了就是给自己挖坑）
    inline constexpr std::string_view kOutboundAcceptEncodingValue{"gzip, deflate"};

    /// 解出来的正文长度上界：压缩比可达上千倍，不设界等于让对端用几百字节撑爆本端内存
    inline constexpr std::size_t kOutboundDecompressedBodyLimitBytes = 64U * 1024U * 1024U;

    /**
     * @brief 这次出站请求该不该由本端代为声明 Accept-Encoding
     * @details 判据是「调用方自己写过 accept-encoding 没有」（名字大小写不敏感）。写过就返回假，
     *          本端既不代加声明也不碰正文：那是调用方在自行处理编码，本端插手会把人家要拿去做
     *          别的用途的字节解掉，也可能解出完全不是他所请求的那份内容。
     *          h1 拼请求文、h2 组头部、响应收尾三处都问这一个判据。
     * @param headers 请求的附加头部
     * @return true 表示本端要代为声明，并据此处理响应正文
     */
    [[nodiscard]] bool shouldAdvertiseAcceptEncoding(const std::vector<HttpClientHeaderField> &headers) noexcept;

    /**
     * @brief 按响应的 Content-Encoding 就地解回正文
     * @details 空正文（HEAD 的应答、204/304，或对端什么都没发）不动也不报错——那时正文压根不存在。
     * @param response 已组装好的出站响应；成功解压时正文被换掉，两条相关头部被删
     * @param maxOutputByteCount 解出长度的上界
     * @return std::expected<bool, std::string> true=解过；false=原样交回（无编码或 identity）；
     *         失败=中文原因（对端发了本端没声明的编码、解坏了、超出上界）
     */
    [[nodiscard]] std::expected<bool, std::string> decodeResponseBodyInPlace(HttpClientResponse &response, std::size_t maxOutputByteCount = kOutboundDecompressedBodyLimitBytes);

    /**
     * @brief 一次出站请求的收尾：本端代加过声明时按响应的 Content-Encoding 解回正文
     * @details 两条承载（h1 与 h2）都汇在 performRequest 的出口，于是「代加才解」这条约束只写一遍。
     *          解失败算这次请求失败：把压缩字节原样交回去，业务看到的是「状态码 200、长度也对、
     *          内容是乱码」，比一个错误难查得多。
     * @param request 这次请求（问它的附加头部里有没有调用方自己写的 accept-encoding）
     * @param response 已拿到的响应，就地改正文与头部
     * @param failureReason 输出：失败原因（解坏、超上界、对端发了没请求的编码）
     * @return true 表示响应可以交给调用方（含「本来就没有编码要解」这一形）
     */
    [[nodiscard]] bool applyContentEncoding(const HttpClientRequest &request, HttpClientResponse &response, std::string &failureReason);

} // namespace AsynGyanis::Net
