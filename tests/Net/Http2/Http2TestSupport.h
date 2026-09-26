/**
 * @file Http2TestSupport.h
 * @brief HTTP/2 测试共用的组帧小工具
 * @author Gyanis
 * @date 2026-09-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details Connection / Session / CleartextSession / Frame 四个测试文件都要手拼 HPACK
 *          字段与帧字节，「索引字段 / 字面量字段 / 整帧 / 找头值 / 空 SETTINGS ACK」
 *          五处曾各复制多份；统一放本头供这四个文件包含。
 */

#pragma once

#include "Net/Http2/Hpack.h"
#include "Net/Http2/Http2Frame.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net::TestSupport
{
    /**
     * @brief 编一个索引字段表示（RFC 7541 §6.1）
     * @param index 表下标（1..61 是静态表）
     * @return std::string 编码后字节
     */
    [[nodiscard]] inline std::string hpackIndexedField(const std::size_t index)
    {
        return encodeHpackInteger(index, 7, 0x80);
    }

    /**
     * @brief 编一个字面量字段表示（带索引名，RFC 7541 §6.2.1）
     * @param staticNameIndex 静态表名下标
     * @param value 头值
     * @return std::string 编码后字节
     */
    [[nodiscard]] inline std::string hpackLiteralField(const std::size_t staticNameIndex, const std::string_view value)
    {
        std::string bytes = encodeHpackInteger(staticNameIndex, 6, 0x40);
        appendHpackString(bytes, value);
        return bytes;
    }

    /**
     * @brief 用编码器拼出一帧（帧头长度按负载实际大小定）
     * @param type 帧类型
     * @param flags 标志位
     * @param streamId 流号
     * @param payload 负载字节
     * @return std::string 完整帧字节
     */
    [[nodiscard]] inline std::string makeFrame(const Http2FrameType type, const unsigned char flags, const std::uint32_t streamId, const std::string_view payload)
    {
        return encodeHttp2Frame(type, flags, streamId, payload);
    }

    /**
     * @brief 在解码后的头列表里找一条头值
     * @param headerFields 头列表
     * @param name 头名
     * @return std::string 头值；没有该头时为空串
     */
    [[nodiscard]] inline std::string findHeaderValue(const std::vector<HpackHeaderField> &headerFields, const std::string_view name)
    {
        for (const HpackHeaderField &field: headerFields)
        {
            if (field.name == name)
            {
                return field.value;
            }
        }
        return {};
    }

    /**
     * @brief 拼一个空的 SETTINGS ACK 帧（RFC 7540 §6.5 要求 ACK 负载为空）
     * @return std::string 完整帧字节
     */
    [[nodiscard]] inline std::string makeSettingsAckFrame()
    {
        return encodeHttp2SettingsFrame(Http2SettingsPayload{.isAcknowledgement = true});
    }
} // namespace AsynGyanis::Net::TestSupport
