/**
 * @file Http3Error.h
 * @brief HTTP/3 与 QPACK 的线上错误码，供帧层、QPACK 层与会话层共用同一张取值表
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 取值逐条取自 RFC 9114 §8.1 与 RFC 9204 §4.5/§5，两类码共用同一个整数空间：它们最终都
 *          写进 QUIC 的连接错误码或流错误码。新增只能追加在末尾，且不得改动既有取值——取值是线上契约。
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief HTTP/3 层的线上错误码
     *
     * @details 名字沿用 RFC 里的标识符去掉前缀后的写法，便于与规范逐条对照；H3_NO_ERROR 之外的一律
     *          要按 §8.1 的要求中止连接或流，不允许多记一次就当没发生。
     */
    enum class Http3ErrorCode : std::uint64_t
    {
        NoError              = 0x0100, ///< H3_NO_ERROR：正常收尾，连接与流可以就此关闭而不算故障
        GeneralProtocolError = 0x0101, ///< H3_GENERAL_PROTOCOL_ERROR：对端违反协议但无更具体的码可用
        InternalError        = 0x0102, ///< H3_INTERNAL_ERROR：本端内部故障，与对端的行为无关
        StreamCreationError  = 0x0103, ///< H3_STREAM_CREATION_ERROR：流的类型或数量不允许（RFC 9114 §6.2.2/§7.2.5）
        ClosedCriticalStream = 0x0104, ///< H3_CLOSED_CRITICAL_STREAM：控制流或 QPACK 流被关闭
        FrameUnexpected      = 0x0105, ///< H3_FRAME_UNEXPECTED：帧出现在不允许它的位置
        FrameError           = 0x0106, ///< H3_FRAME_ERROR：帧自身不符合 §7.2 的布局
        ExcessiveLoad        = 0x0107, ///< H3_EXCESSIVE_LOAD：为了制造负载而发冗余报文
        IdError              = 0x0108, ///< H3_ID_ERROR：流标识或推送标识用错（重复、回退、超范围）
        SettingsError        = 0x0109, ///< H3_SETTINGS_ERROR：SETTINGS 帧里的参数非法
        MissingSettings      = 0x010a, ///< H3_MISSING_SETTINGS：控制流上没收到 SETTINGS 帧
        RequestRejected      = 0x010b, ///< H3_REQUEST_REJECTED：请求已被拒绝，副作用不再保证
        RequestCancelled     = 0x010c, ///< H3_REQUEST_CANCELLED：请求或其响应被取消
        RequestIncomplete    = 0x010d, ///< H3_REQUEST_INCOMPLETE：客户端流提前收尾，消息没送全
        MessageError         = 0x010e, ///< H3_MESSAGE_ERROR：HTTP 消息本身畸形，无法处理
        ConnectError         = 0x010f, ///< H3_CONNECT_ERROR：扩展 CONNECT 建立的隧道那侧失败（RFC 9220 §3.4）
        VersionFallback      = 0x0110, ///< H3_VERSION_FALLBACK：无法支持该请求，请回落到 HTTP/1.1
        DecompressionFailed  = 0x0200, ///< QPACK_DECOMPRESSION_FAILED：头块解不开（RFC 9204 §4.5）
        EncoderStreamError   = 0x0201, ///< QPACK_ENCODER_STREAM_ERROR：编码器流上的指令非法（RFC 9204 §4.4）
        DecoderStreamError   = 0x0202, ///< QPACK_DECODER_STREAM_ERROR：解码器流上的指令非法（RFC 9204 §4.6）
    };

    /**
     * @brief 取错误码对应的线上标识名，用于日志与报错文案
     * @param errorCode 错误码；取值不在 §8.1 与 RFC 9204 的表内时返回「未知错误码」的描述
     * @return 大写下划线的规范名（如 `H3_FRAME_UNEXPECTED`），不含前缀之外的额外文本
     * @note 未知取值不抛异常也不返回空串：对端可以发任何 62 位内的整数，日志里要照实记下它
     */
    [[nodiscard]] std::string_view http3ErrorCodeName(Http3ErrorCode errorCode) noexcept;
} // namespace AsynGyanis::Net
