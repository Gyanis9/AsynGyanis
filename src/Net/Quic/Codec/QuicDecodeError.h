/**
 * @file QuicDecodeError.h
 * @brief QUIC 报文解码失败的类别与中文原因，供上层按类别决定怎么回对端
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 报文来自不可信的对端，解不开是**可恢复错误**而不是调用方的用法错误，因此走
 *          `std::expected` 的失败分支返回，不抛异常。类别按「怎么回才对端有用」划分，
 *          与 `Http2FrameErrorKind` 同一套判据：文案会改，分类才是契约。
 */

#pragma once

#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief QUIC 报文层解码失败的类别
     *
     * @details 新增类别一律追加在末尾：取值可能被记进日志与指标，中间插入会让既有取值的含义整体位移。
     */
    enum class QuicDecodeErrorKind
    {
        Truncated, ///< 字段越过报文末尾（含 Length 域声明的长度大于实收字节数）：整包无法解释，按不可解报文丢弃
        Malformed, ///< 字段值违反 QUIC v1 的硬性规则（长头固定位为 0、连接标识长度超 20 等）：RFC 9000 §17.2/§17.3.1 要求丢弃，上层也可按章节回 PROTOCOL_VIOLATION
    };

    /**
     * @brief 一次解码失败的完整说明
     */
    struct QuicDecodeError
    {
        QuicDecodeErrorKind kind{QuicDecodeErrorKind::Malformed}; ///< 失败类别：上层据此选上线错误码，不去匹配文案
        std::string message;                                      ///< 中文原因，含实际取值与对应的 RFC 章节
    };
} // namespace AsynGyanis::Net
