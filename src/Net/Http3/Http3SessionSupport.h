/**
 * @file Http3SessionSupport.h
 * @brief Http3Session 各翻译单元共用的响应头常量与字节视图
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 响应头补齐规则与「字符串 → 字节视图」的转换在响应采集、信息性响应与流式
 *          写出之间共用；常量与转换只留这一份，避免各翻译单元对头名或零终止假设分叉。
 */

#pragma once

#include "Net/Http/HttpResponse.h"

#include <string_view>

namespace AsynGyanis::Net::Http3SessionDetail
{
    /// HTTP/3 响应里唯一必须由本端补上的头：状态伪头（RFC 9114 §4.3.2）
    inline constexpr const char *kStatusHeaderName = ":status";

    inline constexpr const char      *kContentTypeHeaderName   = "content-type";        ///< 正文媒体类型
    inline constexpr const char      *kContentLengthHeaderName = "content-length";      ///< 正文长度
    inline constexpr const char      *kDateHeaderName          = "date";                ///< 响应生成时刻
    inline constexpr const char      *kTrailerHeaderName       = "trailer";             ///< 尾部字段声明头（RFC 9110 §6.5.1）
    inline constexpr std::string_view kDefaultContentTypeValue = kPlainTextContentType; ///< 有正文却没设类型时的缺省值
} // namespace AsynGyanis::Net::Http3SessionDetail
