/**
 * @file HttpProxyPolicy.h
 * @brief 出站 HTTP 代理策略：显式配置与环境（`http_proxy`/`https_proxy`/`no_proxy`）识别
 * @author Gyanis
 * @date 2026-10-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief 出站代理策略：显式值优先，空字段按 consultEnvironment 决定是否取进程环境
     *
     * @details 环境口径对齐 curl/Go：`http_proxy` 管 `http://` 目标、`https_proxy` 管 `https://` 目标、
     *          `no_proxy` 是豁免表；同名大写变量同样认（`http_proxy` 优先于 `HTTP_PROXY`）。
     *          实例在构造时调用 applyProxyEnvironment() 把环境读一次冻结——之后环境变化不再影响它。
     * @note 值为空且 `consultEnvironment` 为假 = 全部直连；这是显式关掉环境识别的写法。
     */
    struct HttpProxyPolicy
    {
        bool consultEnvironment{true}; ///< 空字段是否去读进程环境

        std::string httpProxy;  ///< `http://` 目标的代理地址；空 → 按 consultEnvironment 取环境
        std::string httpsProxy; ///< `https://` 目标的代理地址；空 → 按 consultEnvironment 取环境
        std::string noProxy;    ///< 豁免表（逗号分隔；`*` 全豁免、按域后缀含子域、可带 `:port`）；空 → 按 consultEnvironment 取环境
    };

    /**
     * @brief 一条解析完成的代理路由
     */
    struct HttpProxyRoute
    {
        std::string   host;                ///< 代理主机（IPv6 已去方括号）
        std::uint16_t port{0};             ///< 代理端口；0 表示未解析
        std::string   authorizationHeader; ///< 预拼的 `Basic …` 值；空 = 代理 URL 未带凭据

        /**
         * @brief 这条路由是否真的有效
         * @return true 端口非 0，可拨
         */
        [[nodiscard]] bool isActive() const noexcept
        {
            return port != 0;
        }
    };

    /**
     * @brief 把策略里空的字段用进程环境补齐
     * @param policy 显式策略（调用方给出）
     * @return HttpProxyPolicy 冻结后的副本；`consultEnvironment` 为假时原样返回
     * @note 构造期调一次即冻结；静态便捷入口每次调用现读一次（对齐 curl 每进程语义）
     */
    [[nodiscard]] ASYN_NET_API HttpProxyPolicy applyProxyEnvironment(HttpProxyPolicy policy);

    /**
     * @brief 每请求解析一次代理路由：豁免表命中 = 直连，否则按 scheme 取对应代理
     * @param policy 已冻结的策略（本函数不读环境）
     * @param scheme 目标 scheme（`http`/`https`；其余 scheme 一律直连）
     * @param host 目标主机（IPv6 不含方括号）
     * @param port 目标端口
     * @return nullopt = 直连；值 = 经该代理；unexpected = 策略里的代理地址不合法
     * @note 代理地址不合法时**拒绝请求而不是静默直连**：绕过运维配置的代理是安全面的事，
     *       不能退化成「看着能用」
     */
    [[nodiscard]] ASYN_NET_API std::expected<std::optional<HttpProxyRoute>, std::string> resolveOutboundProxy(const HttpProxyPolicy &policy, std::string_view scheme,
                                                                                                              std::string_view host, std::uint16_t port);

    /**
     * @brief 解析一条代理地址：`http://[user[:pass]@]host[:port]` 或裸 `host[:port]`
     * @param proxyUrl 代理地址文本（来自显式策略或环境变量）
     * @return HttpProxyRoute 主机、端口（缺省 80）与预拼的认证头；非法时给出点名原因
     * @details `https://` 代理（TLS 连到代理自身）当前不支持，遇到即拒并写明——静默降级成明文连代理
     *          比拒绝更糟。凭据段支持 `%XX` 转义；口令原文绝不回显进任何错误文案（见 redactProxyCredentials）。
     */
    [[nodiscard]] ASYN_NET_API std::expected<HttpProxyRoute, std::string> parseProxyUrl(std::string_view proxyUrl);

    /**
     * @brief 目标是否命中豁免表
     * @param host 目标主机（IPv6 不含方括号），比较不区分大小写
     * @param port 目标端口（条目带 `:port` 时参与匹配）
     * @param noProxyList 逗号分隔的豁免表；条目形如 `*`、`host`、`.host`、`host:port`、`[v6]:port`
     * @return true 命中（本次直连）
     * @note 后缀匹配按**点边界**：`example.com` 命中 `example.com` 与 `api.example.com`，不命中
     *       `notexample.com`；IP 字面量请写完整（后缀规则对 IP 同样生效，写全了才精确）
     */
    [[nodiscard]] ASYN_NET_API bool matchesNoProxy(std::string_view host, std::uint16_t port, std::string_view noProxyList) noexcept;

    /**
     * @brief 把代理地址文本里的凭据段打成 `***`（错误文案专用）
     * @param proxyUrl 原始代理地址文本
     * @return std::string 打码后的文本；无凭据段时原样返回
     */
    [[nodiscard]] ASYN_NET_API std::string redactProxyCredentials(std::string_view proxyUrl);
} // namespace AsynGyanis::Net
