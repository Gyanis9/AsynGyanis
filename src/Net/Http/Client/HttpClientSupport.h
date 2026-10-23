/**
 * @file HttpClientSupport.h
 * @brief HttpClient 各翻译单元共用的剩余预算与出站建连入口
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 剩余预算的算法只有一份；两条建连入口在连接翻译单元里定义，请求编排只消费声明。
 *          该头只被实现文件包含，不构成对外契约。
 */

#pragma once

#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/Client/HttpOutboundConnectionPool.h"
#include "Net/Http/Client/HttpProxyPolicy.h"

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace AsynGyanis::Core
{
    class EventLoop;
    class TlsContext;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net::HttpClientDetail
{
    /**
     * @brief 本次请求还剩多少时间可以用于下一步
     * @param startedAt 请求开始时刻
     * @param requestTimeout 请求总时限
     * @return std::optional<std::chrono::milliseconds> 剩余预算；已经耗尽时为空
     */
    [[nodiscard]] inline std::optional<std::chrono::milliseconds> remainingBudget(const std::chrono::steady_clock::time_point startedAt,
                                                                                  const std::chrono::milliseconds             requestTimeout)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
        if (elapsed >= requestTimeout)
        {
            return std::nullopt;
        }
        return requestTimeout - elapsed;
    }

    /**
     * @brief 建一条明文出站连接（可经代理）
     * @param loop 所属事件循环
     * @param key 目标身份（主机、端口与 TLS 位）
     * @param startedAt 请求开始时刻
     * @param requestTimeout 请求总时限
     * @param failureReason 失败原因输出
     * @param proxyRoute 非空表示经代理拨号
     * @param clientTls 调用方提供的 TLS 客户端上下文，可为空
     * @return Core::Task<std::unique_ptr<HttpOutboundConnection>> 建成的连接；失败为空
     */
    Core::Task<std::unique_ptr<HttpOutboundConnection>> establishPlainConnection(Core::EventLoop &loop, const HttpOutboundEndpointKey &key,
                                                                                 std::chrono::steady_clock::time_point startedAt, std::chrono::milliseconds requestTimeout,
                                                                                 std::string &failureReason, const std::optional<HttpProxyRoute> &proxyRoute,
                                                                                 const Core::TlsContext *clientTls, std::stop_token cancelToken);

    /**
     * @brief 建一条 TLS 出站连接（可经代理隧道）
     * @param loop 所属事件循环
     * @param url 已拆开的目标 URL
     * @param startedAt 请求开始时刻
     * @param requestTimeout 请求总时限
     * @param failureReason 失败原因输出
     * @param clientTls 调用方提供的 TLS 客户端上下文，可为空
     * @param proxyRoute 非空表示经代理
     * @return Core::Task<std::unique_ptr<HttpOutboundConnection>> 建成的连接；失败为空
     */
    Core::Task<std::unique_ptr<HttpOutboundConnection>> establishSecureConnection(Core::EventLoop &loop, const ParsedUrl &url, std::chrono::steady_clock::time_point startedAt,
                                                                                  std::chrono::milliseconds requestTimeout, std::string &failureReason,
                                                                                  const Core::TlsContext *clientTls, const std::optional<HttpProxyRoute> &proxyRoute,
                                                                                  std::stop_token cancelToken);
} // namespace AsynGyanis::Net::HttpClientDetail
