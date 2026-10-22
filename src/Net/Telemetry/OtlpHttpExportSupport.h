/**
 * @file OtlpHttpExportSupport.h
 * @brief OTLP/HTTP 三个出口共用的头字段校验与采集端地址解析
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "Base/Exception/InvalidArgumentException.h"
#include "Net/Http/Client/HttpClient.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

namespace AsynGyanis::Net::Telemetry
{
    /// 三个出口自己会写的头部（大小写无关）：附加头占用它们就等于与出口抢同一行
    inline constexpr std::string_view kReservedOtlpHeaderNames[] = {"host", "content-length", "connection", "content-type"};

    /**
     * @brief 两个头部名忽略大小写是否相同
     * @param left 左名
     * @param right 右名
     * @return true 按 ASCII 折小写后逐字节相同
     */
    [[nodiscard]] inline bool isSameHeaderNameIgnoringCase(const std::string_view left, const std::string_view right) noexcept
    {
        if (left.size() != right.size())
        {
            return false;
        }
        const auto fold = [](const char character) { return character >= 'A' && character <= 'Z' ? static_cast<char>(character + 0x20) : character; };
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            if (fold(left[index]) != fold(right[index]))
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 头部名或值里有没有会撕裂请求行的字节
     * @param text 待检查文本
     * @return true 含 CR、LF 或 NUL
     */
    [[nodiscard]] inline bool hasUnsafeHeaderByte(const std::string_view text) noexcept
    {
        return std::ranges::any_of(text, [](const char character) { return character == '\r' || character == '\n' || character == '\0'; });
    }

    /**
     * @brief 头部名是否属于出口自己会写的那几个（大小写无关）
     * @param name 待检查的头部名
     * @return true 是保留名
     */
    [[nodiscard]] inline bool isReservedHeaderName(const std::string_view name) noexcept
    {
        return std::ranges::any_of(kReservedOtlpHeaderNames, [name](const std::string_view reserved) { return isSameHeaderNameIgnoringCase(reserved, name); });
    }

    /**
     * @brief 把采集端地址拆成 ParsedUrl，空与畸形都转成该出口的配置错误
     * @param endpoint 配置给出的地址
     * @param rejectionPrefix 该出口的异常前缀（如「OTLP 日志出口无法启动：」）
     * @param fieldName 配置字段的完整路径，用于把错误点回调用方填错的位置
     * @param examplePath 该信号的标准路径（/v1/logs、/v1/metrics、/v1/traces）
     * @return ParsedUrl 拆好的目标
     * @throws Base::InvalidArgumentException 地址为空或 parseUrl 判它不成形
     */
    [[nodiscard]] inline ParsedUrl makeOtlpTarget(const std::string &endpoint, const std::string_view rejectionPrefix, const std::string_view fieldName,
                                                  const std::string_view examplePath)
    {
        if (endpoint.empty())
        {
            throw Base::InvalidArgumentException(std::string(rejectionPrefix) + "采集端地址为空。请在 " + std::string(fieldName) + " 里给出形如 http(s)://collector:4318" +
                                                 std::string(examplePath) + " 的地址");
        }
        try
        {
            return parseUrl(endpoint);
        } catch (const Base::InvalidArgumentException &error)
        {
            throw Base::InvalidArgumentException(std::string(rejectionPrefix) + "采集端地址「" + endpoint + "」不是一枚可用的 http(s) URL：" + error.what() +
                                                 "。请写成 http(s)://host:port" + std::string(examplePath));
        }
    }
} // namespace AsynGyanis::Net::Telemetry
