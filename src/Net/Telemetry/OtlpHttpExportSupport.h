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
#include "Core/Coroutine/Task.h"
#include "Net/Http/Client/HttpClient.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

    /**
     * @brief 校验附加头部：名字非空、不含撕裂请求行的字节、不占用出口自己写的名字
     * @param headers 配置里的附加头部
     * @param rejectionPrefix 该出口的异常前缀（如「OTLP 日志出口无法启动：」）
     * @throws Base::InvalidArgumentException 任一条不成立，文案点回出问题的头部名
     */
    inline void validateOtlpExtraHeaders(const std::vector<HttpClientHeaderField> &headers, const std::string_view rejectionPrefix)
    {
        const auto reject = [rejectionPrefix](const std::string &reason) { throw Base::InvalidArgumentException(std::string(rejectionPrefix) + reason); };
        for (const HttpClientHeaderField &header: headers)
        {
            if (header.first.empty())
            {
                reject("附加头部的名字为空：请求行会被撕裂。请给一个非空的头部名");
            }
            if (hasUnsafeHeaderByte(header.first) || hasUnsafeHeaderByte(header.second))
            {
                reject("附加头部「" + header.first + "」的名字或值里有 CR、LF 或 NUL：那等于在请求里塞进额外一行");
            }
            if (isReservedHeaderName(header.first))
            {
                reject("附加头部占用了「" + header.first + "」：这一项由本出口自己写（正文媒体类型固定是 application/json），再给一份就会出现两条同名头部");
            }
        }
    }

    /// 一次导出投递的结果：失败原因非空即未送达；成功时 response 非空且状态码为 2xx
    struct OtlpHttpExportAttempt
    {
        std::unique_ptr<HttpClientResponse> response;
        std::string                         failureReason;
    };

    /**
     * @brief 向采集端发一次 OTLP 请求，把「抛异常 / 没拿到响应 / 非 2xx」折成同一条失败原因
     * @details 出站异常不能带出导出协程：那会顺着 run() 打断整条循环线程。这里就地折成原因文本，
     *          三个出口的程序计数与退避策略仍各留各的。
     * @param client 出站客户端
     * @param endpoint 采集端地址，同时用于失败文案
     * @param request 装配好的请求；正文视图须活到本次 co_await 返回
     * @param requestTimeout 单次请求时限
     * @param failurePrefix 失败文案前缀（如「OTLP metrics 出口」）
     * @return Core::Task<OtlpHttpExportAttempt> 投递结果
     */
    inline Core::Task<OtlpHttpExportAttempt> sendOtlpHttpRequest(HttpClient &client, const std::string &endpoint, const HttpClientRequest &request,
                                                                 const std::chrono::milliseconds requestTimeout, const std::string_view failurePrefix)
    {
        OtlpHttpExportAttempt attempt;
        try
        {
            attempt.response = co_await client.send(endpoint, request, requestTimeout);
        } catch (const std::exception &exception)
        {
            attempt.failureReason = std::string(failurePrefix) + "：发 POST 时抛出：" + std::string{exception.what()};
            co_return attempt;
        }
        if (attempt.response == nullptr)
        {
            attempt.failureReason = std::string(failurePrefix) + "：向 " + endpoint + " 发 POST 没能拿到响应（连不上、超时或对端关闭）";
        } else if (attempt.response->statusCode < 200 || attempt.response->statusCode >= 300)
        {
            attempt.failureReason = std::string(failurePrefix) + "：采集端回了状态码 " + std::to_string(attempt.response->statusCode);
        }
        co_return attempt;
    }
} // namespace AsynGyanis::Net::Telemetry
