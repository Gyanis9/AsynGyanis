/**
 * @file HttpClientUrl.h
 * @brief 出站客户端内部共用的 Host/:authority 文本拼装
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 实现侧的包含口：只有 HttpClient 的翻译单元会包含它，不构成对外契约。
 *          IPv6 字面量的方括号与默认端口的省略规则只留这一份，请求头与 h2/h3 伪头因此
 *          不会各拼一个样。
 */

#pragma once

#include "Net/Http/Client/HttpClient.h"

#include <string>

namespace AsynGyanis::Net::HttpClientUrl
{
    /**
     * @brief 拼出 Host / :authority 用的主机文本
     * @details IP 字面量在拆 URL 时按 RFC 3986 §3.2.2 去掉了方括号，这里要加回去：地址里自带
     *          冒号，不加回去「Host: ::1」会把头部与端口分隔符混成一团。端口只在不等于该协议的
     *          默认端口时写出（RFC 9110 §7.2 对 Host、RFC 9113 §8.3.1 对 :authority 同一条法）。
     * @param url 已拆好的目标
     * @return std::string 可直接写进 Host 头与 :authority 的主机文本
     */
    [[nodiscard]] std::string authorityText(const ParsedUrl &url);

    /**
     * @brief 把一条 Host 头写进请求文本
     * @param request 正在拼装的请求文本，就地追加
     * @param url 已拆好的目标
     */
    void appendHostHeader(std::string &request, const ParsedUrl &url);
} // namespace AsynGyanis::Net::HttpClientUrl
