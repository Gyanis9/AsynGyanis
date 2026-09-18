/**
 * @file NetTestSupport.h
 * @brief Net 模块单元测试共用的文本与字节小工具
 * @author Gyanis
 * @date 2026-09-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 报文类用例遍布 Http / Http2 / WebSocket 三个子目录，同一批「查子串 / 数出现次数 /
 *          拼字节串 / 判请求标识」的小工具曾各自复制多份；统一放本头供各测试文件包含。
 *          与 CoreTestSupport.h 的协程夹具、HttpTestSupport.h 的服务器夹具各司其职。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net::TestSupport
{
    /// 自动生成的请求标识长度：4 位前缀 + '-' + 16 位十六进制
    inline constexpr std::size_t kGeneratedRequestIdLength = 4 + 1 + 16;

    /// 分隔符 '-' 在请求标识里的下标
    inline constexpr std::size_t kGeneratedRequestIdSeparatorIndex = 4;

    /**
     * @brief 判断文本里是否出现指定子串
     * @param haystack 待搜索文本
     * @param needle 目标子串
     * @return true 命中
     */
    [[nodiscard]] inline bool containsText(const std::string_view haystack, const std::string_view needle)
    {
        return haystack.find(needle) != std::string_view::npos;
    }

    /**
     * @brief 统计文本里指定子串出现的次数
     * @param text 待搜索文本
     * @param needle 目标子串
     * @return std::size_t 出现次数
     */
    [[nodiscard]] inline std::size_t countTextOccurrences(const std::string_view text, const std::string_view needle)
    {
        std::size_t occurrenceCount = 0;
        for (std::size_t foundPosition = text.find(needle); foundPosition != std::string_view::npos;
             foundPosition = text.find(needle, foundPosition + needle.size()))
        {
            ++occurrenceCount;
        }
        return occurrenceCount;
    }

    /**
     * @brief 由字节序列拼出二进制文本
     * @details 不能直接用字符串字面量：帧头与负载里常含 0x00，按 const char* 构造会被零终止截断，
     *          那样断言就测不到完整字节了。
     * @param byteValues 字节值序列
     * @return std::string 逐字节写入的结果
     */
    [[nodiscard]] inline std::string makeBytes(const std::initializer_list<unsigned char> byteValues)
    {
        std::string bytes;
        bytes.reserve(byteValues.size());
        for (const unsigned char byteValue: byteValues)
        {
            bytes.push_back(static_cast<char>(byteValue));
        }
        return bytes;
    }

    /**
     * @brief 把 32 位无符号数按大端写成 4 字节
     * @param value 待写出的数值
     * @return std::string 4 字节
     */
    [[nodiscard]] inline std::string makeBigEndian32(const std::uint32_t value)
    {
        std::string bytes;
        for (int shiftBitCount = 24; shiftBitCount >= 0; shiftBitCount -= 8)
        {
            bytes.push_back(static_cast<char>((value >> shiftBitCount) & 0xFFU));
        }
        return bytes;
    }

    /**
     * @brief 判断请求标识是否符合自动生成格式
     * @param requestId 待判定的标识
     * @return true 长度符合约定，且分隔符位置与其余字符都是小写十六进制
     */
    [[nodiscard]] inline bool looksLikeGeneratedRequestId(const std::string_view requestId)
    {
        if (requestId.size() != kGeneratedRequestIdLength || requestId[kGeneratedRequestIdSeparatorIndex] != '-')
        {
            return false;
        }

        for (std::size_t index = 0; index < requestId.size(); ++index)
        {
            if (index == kGeneratedRequestIdSeparatorIndex)
            {
                continue;
            }
            const char character = requestId[index];
            const bool isLowerHexDigit = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            if (!isLowerHexDigit)
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 用给定的头部行拼出一条完整 GET 报文
     * @param headerLines 头部行原文，不含行尾 CRLF
     * @return std::string 可直接喂给 parse() 的报文
     */
    [[nodiscard]] inline std::string makeRequestTextWithHeaders(const std::vector<std::string> &headerLines)
    {
        std::string message = "GET /indexed HTTP/1.1\r\n";
        for (const std::string &headerLine: headerLines)
        {
            message.append(headerLine);
            message.append("\r\n");
        }
        message.append("\r\n");
        return message;
    }
} // namespace AsynGyanis::Net::TestSupport
