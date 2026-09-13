/**
 * @file HttpServerConfig.h
 * @brief 把 HTTP 服务器的可配置项接到配置文档的值上（限额、按 IP 限额、限流、指标开关）
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigValue.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpServerLimits.h"

#include <cstddef>

namespace AsynGyanis::Net
{
    /// 服务器配置在文档里的段名
    inline constexpr std::string_view kHttpServerConfigSection = "server";

    /**
     * @brief 一台 HTTP 服务器的可配置项集合。
     *
     * @details 每个字段的默认值与对应结构体的默认值一致，因此配置里**只写要改的那几项**即可，
     *          没写的键保持默认——这也让「升级后新增的键」在旧配置上自动取到合理取值。
     * @note 这里只放「启动时定一次」的项：限额与开关都在 start() 之前落定，运行期不再变。
     * @see readHttpServerConfiguration(), HttpServerLimits, HttpParserLimits
     */
    struct HttpServerConfiguration
    {
        HttpServerLimits limits{};              ///< 连接级限额：超时与单连接请求数上限
        HttpParserLimits parserLimits{};        ///< 单条报文的内存上限
        std::size_t maximumConnections{0};      ///< 全局并发连接上限，0 = 不限
        std::size_t maximumConnectionsPerIp{0}; ///< 单个来源的并发连接上限，0 = 不限
        double requestsPerSecond{0.0};          ///< 全局请求速率上限（令牌桶速率），0 = 不限流
        double rateLimitBurstCapacity{1.0};     ///< 令牌桶容量，即瞬时允许的突发量；速率不为 0 时必须 ≥ 1
        bool exposeMetrics{false};              ///< 是否注册 /metrics 与 /healthz
    };

    /**
     * @brief 从配置文档里读出 HTTP 服务器的可配置项。
     *
     * @details 读的是文档的 `server` 段，键名与 HttpServerConfiguration 的字段同名（超时一律带 `_ms`
     *          后缀，单位毫秒）。三层嵌套：`limits`（连接级）、`parser_limits`（单条报文）、
     *          `rate_limit`（令牌桶）。**没有任何配置时返回一份全默认值**，调用方因此不必区分
     *          「配置里没有 server 段」与「配置里有但没写这一项」。
     *
     * @param configurationRoot 配置文档的根值，即一个以 `server` 为键的对象。从
     *        Base::ConfigManager 取时用 `getSection("server")` 再补上段名这一层外壳
     *        （ConfigManager 的键是扁平的，get("server") 取不到中间层节点）
     * @return HttpServerConfiguration 读出来的配置；缺失的键取结构体默认值
     * @throws Base::ConfigValidationException 段或子段不是对象、出现未知键、类型不符或取值越界。
     *         未知键也算错是**刻意**的：把 `idle_timeout` 写成 `idle_timeout_ms` 时静默忽略
     *         等于配置没生效却看不出来，正是最难查的一类问题。
     * @note 只读不写，线程安全取决于调用方传入的值树是否可变（本函数不做任何修改）
     * @see HttpServerConfiguration, Base::ConfigManager
     */
    [[nodiscard]] HttpServerConfiguration readHttpServerConfiguration(const Base::ConfigValue &configurationRoot);

} // namespace AsynGyanis::Net
