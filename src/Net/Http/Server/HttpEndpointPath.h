/**
 * @file HttpEndpointPath.h
 * @brief 运维端点路径的形状校验：明文与 TLS 两侧共用同一份判据
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 不以 / 开头的路径永远匹配不到任何请求，静默注册等于给人一个「调了却没有端点」的
 *          假象。两台服务器、各六个端点必须按同一条口径拒绝，判据因此只留这一份。
 */

#pragma once

#include "Base/Exception/InvalidArgumentException.h"

#include <string>
#include <string_view>

namespace AsynGyanis::Net::HttpEndpoint
{
    /**
     * @brief 要求运维端点路径非空且以 / 开头
     * @param path 调用方给的路径
     * @param serverName 点名的服务器（HttpServer 或 HttpsServer），用于错误前缀
     * @param endpointName 端点种类（如「指标」「健康检查」），用于错误文案
     * @throws Base::InvalidArgumentException 路径为空或不以 / 开头
     */
    inline void requireAbsolutePath(const std::string_view path, const std::string_view serverName, const std::string_view endpointName)
    {
        if (path.empty() || path.front() != '/')
        {
            throw Base::InvalidArgumentException(std::string(serverName) + ": " + std::string(endpointName) + "端点路径必须以 / 开头，收到的是「" + std::string(path) + "」");
        }
    }
} // namespace AsynGyanis::Net::HttpEndpoint
