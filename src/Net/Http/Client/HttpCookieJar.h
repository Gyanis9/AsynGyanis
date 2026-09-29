/**
 * @file HttpCookieJar.h
 * @brief 出站客户端的 Cookie 存储罐：按 RFC 6265 收、匹配、发
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Net/Http/HttpCookie.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 客户端的 Cookie 存储罐
     *
     * @details 按 RFC 6265 的规则收Cookie、判作用域、拼请求头：域属性只能收窄到请求主机、IP 字面量
     *          不接受 Domain 属性、Secure 属性只在加密连接上回送、路径按前缀匹配、Max-Age 为 0 或
     *          负数表示删除。同名同域同路径即替换，不累积。
     * @details 存储是**有界**的：每个域与总量各有上限，超出时按插入顺序淘汰最旧的一条。不设上限时，
     *          一个愿意一直回 Set-Cookie 的对端就能把客户端的内存吃光——那只是几个请求的延迟问题，
     *          不该变成内存问题。
     * @note 本类只在内存里存，进程退出即消失，也不区分「会话 Cookie」与「持久 Cookie」的落盘：
     *       要跨进程保留请自行把记录序列化出去（本版本不提供该入口）
     * @note 线程安全：全部公开方法都取内部锁，可以在多个事件循环线程之间共享一整个 jar。
     */
    class ASYN_NET_API HttpCookieJar
    {
    public:
        /**
         * @brief 存储上限
         */
        struct Limits
        {
            std::size_t maximumCookiesPerDomain{50}; ///< 单个域最多存几条，超出即按插入顺序淘汰最旧的
            std::size_t maximumTotalCookies{500};    ///< 整个罐子最多存几条，超出即按插入顺序淘汰最旧的
        };

        /**
         * @brief 按默认上限建一个空罐子
         * @details 与下面的带参构造分开写而不是给 `Limits` 缺省实参：GCC 不接受「嵌套类带非静态数据
         *          成员初始化器」当外层类成员的默认实参（本仓在 Http/3 那侧踩过同一条），
         *          而 `Limits{}` 又正是这种写法
         */
        HttpCookieJar() noexcept;

        /**
         * @brief 按给定上限建一个空罐子
         * @param limits 存储上限
         */
        explicit HttpCookieJar(Limits limits) noexcept;

        /**
         * @brief 收下响应里的全部 Set-Cookie
         * @param requestHost 本次请求的目标主机（不含端口，IPv6 不带方括号）
         * @param isSecureConnection 本次连接是否加密（决定 Secure 属性的 Cookie 能不能收、能不能发）
         * @param requestPath 本次请求的路径（用于推导缺省路径）
         * @param setCookieHeaderValues 响应里所有 set-cookie 头部的取值，按出现顺序
         * @param receivedAt 收到响应的时刻；缺省取当前时间。测试可以传一个固定时刻，
         *                   以免断言依赖墙钟
         */
        void storeFromResponse(std::string_view requestHost, bool isSecureConnection, std::string_view requestPath, const std::vector<std::string> &setCookieHeaderValues,
                               std::chrono::system_clock::time_point receivedAt = std::chrono::system_clock::now());

        /**
         * @brief 拼出发往该目标的那一条 Cookie 头部取值
         * @param requestHost 目标主机
         * @param isSecureConnection 本次连接是否加密
         * @param requestPath 本次请求的路径
         * @return std::optional<std::string> 形如 "a=1; b=2"；没有可发的 Cookie 时返回空
         * @note 排序按 RFC 6265 §5.4：路径长的在前，长度相同则先收到的在前
         */
        [[nodiscard]] std::optional<std::string> buildRequestHeader(std::string_view requestHost, bool isSecureConnection, std::string_view requestPath) const;

        /// 当前存着的 Cookie 条数（含尚未过期但还没被访问到的）
        [[nodiscard]] std::size_t cookieCount() const;

        /// 清空全部存储
        void clear() noexcept;

        /**
         * @brief 判断一个主机名是不是 IP 字面量
         * @details RFC 6265 §5.2.3 禁止对 IP 设 Domain 属性：`127.0.0.1` 与 `example.com` 没有
         *          「子域」关系，接受这种属性等于凭空造出一个跨主机生效的 Cookie。
         *          这里只按形状判（IPv4 的点分十进制、IPv6 的冒号），不做地址解析。
         * @param host 主机名
         * @return true 是 IP 字面量
         */
        [[nodiscard]] static bool isIpAddressLiteral(std::string_view host) noexcept;

    private:
        /// 罐子里的一条记录：作用域与存活期都已折好，发送侧只做匹配判断
        struct Entry
        {
            std::string                                          name;              ///< Cookie 名字
            std::string                                          value;             ///< Cookie 取值
            std::string                                          domain;            ///< 生效域（已折掉前导点）
            bool                                                 isHostOnly{false}; ///< 是否只对精确主机生效（无 Domain 属性即真）
            std::string                                          path{"/"};         ///< 生效路径
            bool                                                 isSecure{false};   ///< Secure 属性
            std::optional<std::chrono::system_clock::time_point> expiresAt;         ///< 过期时刻；空表示会话 Cookie
            std::uint64_t                                        sequence{0};       ///< 插入次序，用于淘汰与同长度排序
        };

        /// 判断一条记录能否发往给定目标（调用方须持锁）
        [[nodiscard]] static bool matches(const Entry &entry, std::string_view requestHost, bool isSecureConnection, std::string_view requestPath) noexcept;

        /// 逐字对齐的两个作用域是否相同，用于替换判定（调用方须持锁）
        [[nodiscard]] static bool isSameScope(const Entry &entry, const Entry &candidate) noexcept;

        /// 淘汰到上限之内（调用方须持锁）
        void enforceLimitsLocked();

        Limits             m_limits;      ///< 存储上限
        mutable std::mutex m_mutex;       ///< 保护 m_entries 与 m_sequence
        std::vector<Entry> m_entries;     ///< 全部记录
        std::uint64_t      m_sequence{0}; ///< 下一条记录的插入次序
    };
} // namespace AsynGyanis::Net
