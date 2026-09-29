/**
 * @file HttpCookie.h
 * @brief HTTP Cookie 的结构化表示：Set-Cookie 的渲染与解析、Cookie 请求头的解析
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief SameSite 属性取值
     * @details 不设为默认：Set-Cookie 不带 SameSite 时浏览器按它自己的缺省处理（各家不同），
     *          框架替调用方填一个值等于改变既有站点的行为，那是要显式写出来的决定。
     */
    enum class CookieSameSitePolicy
    {
        Strict, ///< 跨站请求一律不带
        Lax,    ///< 顶级导航的 GET 才带（现代浏览器的缺省）
        None    ///< 跨站也带，但浏览器要求同时带 Secure
    };

    /**
     * @brief 一条 Cookie：名字、取值与它的属性
     *
     * @details 名字与取值是 RFC 6265 §4.1.1 的 token，属性段（Path/Domain/Expires/Max-Age/Secure/
     *          HttpOnly/SameSite）只有被显式设过才会出现在渲染结果里——不做「没设就补个 Path=/」
     *          这类好心，那会静默改变作用域。
     * @note 本类只负责一条 Cookie 的表示与文本化，跨站匹配与存储归 `HttpCookieJar`；
     *       它自己不判定「能不能发给这个主机」
     */
    class ASYN_NET_API HttpCookie
    {
    public:
        /**
         * @brief 构造一条没有属性的 Cookie
         * @param cookieName 名字，必须是 RFC 6265 的 token（不含控制符、分隔符与空格）
         * @param cookieValue 取值，允许空串；不含分号、换行、逗号与控制符
         * @throws Base::InvalidArgumentException 名字或取值含非法字符：写进 Set-Cookie 会破坏头部结构
         */
        HttpCookie(std::string_view cookieName, std::string_view cookieValue);

        /// 默认构造给「先建容器再填」的写法用：名字与取值都是空串
        HttpCookie() = default;

        /**
         * @brief 判断一段文本能否作为 Cookie 名字
         * @param text 待判文本，空串判 false（RFC 6265 要求名字至少一个字符）
         * @return true 全为 token 字符且非空
         */
        [[nodiscard]] static bool isValidName(std::string_view text) noexcept;

        /**
         * @brief 判断一段文本能否作为 Cookie 取值
         * @param text 待判文本，空串合法（RFC 6265 允许空取值）
         * @return true 全为合法取值字符
         */
        [[nodiscard]] static bool isValidValue(std::string_view text) noexcept;

        /// Cookie 名字
        [[nodiscard]] const std::string &name() const noexcept
        {
            return m_name;
        }

        /// Cookie 取值
        [[nodiscard]] const std::string &value() const noexcept
        {
            return m_value;
        }

        /**
         * @brief 换取值
         * @param cookieValue 新取值，空串合法
         * @throws Base::InvalidArgumentException 含非法字符
         */
        void setValue(std::string_view cookieValue);

        /// Path 属性；未设时为空 optional
        [[nodiscard]] const std::optional<std::string> &path() const noexcept
        {
            return m_path;
        }

        /**
         * @brief 设 Path 属性
         * @param path 路径，须以 '/' 开头（RFC 6265 §5.2.4 的缺省规则要求前缀匹配，非 '/' 开头的值
         *             会被浏览器整段丢掉，因此这里直接拒而不是默默补斜杠）
         * @throws Base::InvalidArgumentException 不以 '/' 开头
         */
        void setPath(std::string_view path);

        /// Domain 属性；未设时为空 optional（此时 Cookie 只对精确主机生效）
        [[nodiscard]] const std::optional<std::string> &domain() const noexcept
        {
            return m_domain;
        }

        /**
         * @brief 设 Domain 属性
         * @param domain 主机名；允许以 '.' 开头（按 RFC 6265 与无前导点等价，这里保留原样交出去）
         * @throws Base::InvalidArgumentException 空串或含非法字符（空格、控制符、'/'、';'、'"'、'='）
         */
        void setDomain(std::string_view domain);

        /// Expires 属性；未设时为空 optional
        [[nodiscard]] const std::optional<std::chrono::system_clock::time_point> &expiresAt() const noexcept
        {
            return m_expiresAt;
        }

        /**
         * @brief 设 Expires 属性（渲染成 IMF-fixdate）
         * @param expiresAt 过期时刻
         * @note 同时设了 Max-Age 时，浏览器以 Max-Age 为准（RFC 6265 §5.3）；本类不拦这种组合，
         *       两个属性都会照原样发出去
         */
        void setExpiresAt(std::chrono::system_clock::time_point expiresAt) noexcept
        {
            m_expiresAt = expiresAt;
        }

        /// Max-Age 属性（秒）；未设时为空 optional
        [[nodiscard]] const std::optional<std::int64_t> &maxAgeSeconds() const noexcept
        {
            return m_maxAgeSeconds;
        }

        /**
         * @brief 设 Max-Age 属性
         * @param seconds 秒数；0 或负数按 RFC 6265 表示「立即过期，用于删除 Cookie」，如实发出
         */
        void setMaxAgeSeconds(std::int64_t seconds) noexcept
        {
            m_maxAgeSeconds = seconds;
        }

        /// Secure 属性是否已设
        [[nodiscard]] bool isSecure() const noexcept
        {
            return m_isSecure;
        }

        /// 设 Secure 属性（只在加密连接上回送）
        void setSecure(bool isSecure = true) noexcept
        {
            m_isSecure = isSecure;
        }

        /// HttpOnly 属性是否已设（脚本读不到）
        [[nodiscard]] bool isHttpOnly() const noexcept
        {
            return m_isHttpOnly;
        }

        /// 设 HttpOnly 属性
        void setHttpOnly(bool isHttpOnly = true) noexcept
        {
            m_isHttpOnly = isHttpOnly;
        }

        /// SameSite 属性；未设时为空 optional
        [[nodiscard]] const std::optional<CookieSameSitePolicy> &sameSite() const noexcept
        {
            return m_sameSite;
        }

        /// 设 SameSite 属性
        void setSameSite(CookieSameSitePolicy policy) noexcept
        {
            m_sameSite = policy;
        }

        /**
         * @brief 渲染成一条 Set-Cookie 头部取值
         * @details 只输出被显式设过的属性，顺序固定为 name=value 后跟 Path、Domain、Max-Age、
         *          Expires、Secure、HttpOnly、SameSite，便于逐字断言与日志比对。
         * @return std::string 完整的头部取值（不含 "Set-Cookie:" 头部名本身）
         */
        [[nodiscard]] std::string renderAsSetCookie() const;

        /**
         * @brief 解析一条 Set-Cookie 头部取值
         * @details 服务端给的 Cookie 属性名大小写不敏感（'PATH'、'samesite' 都要认）；
         *          无法识别的属性段整段跳过（RFC 6265 §5.2 要求收端忽略未知属性），不判整条失败。
         *          取值按 RFC 6265 §5.1.2 认两种形状：裸的 cookie-octet 串，或首尾各一枚引号的
         *          quoted-string（内层 '\' 是转义，解开后按字面收）；引号本身不进存下来的值。
         * @param headerValue 头部取值
         * @return std::optional<HttpCookie> 解析结果；名字非法、首段缺 '='，或取值不是合法 cookie-octet
         *         （含 quoted-string 没闭合、解开后仍带空格）时返回空
         */
        [[nodiscard]] static std::optional<HttpCookie> parseSetCookie(std::string_view headerValue);

        /**
         * @brief 解析一条（或多条同名头合并成的）Cookie 请求头
         * @details 请求侧的 Cookie 头只带名字与取值，没有属性。'=' 之后到下一个 ';' 之前全算取值，
         *          取值里再出现的 '=' 原样保留；两侧空白被去掉，引号包起来的取值按 §5.1.2 解开。
         *          中间带空格的裸取值不算合法 cookie-octet，那**一条**会被跳过（其余各条照常交出来，
         *          判据见 isValidValue）。
         * @param headerValue 一个 Cookie 头的完整取值
         * @return std::vector<HttpCookie> 按出现顺序给出的 Cookie 列表，属性一律为未设
         * @note 跳过而不是收下再报错：Cookie 头是浏览器与各级代理拼出来的，混一条怪的就让整条头读不出
         *       等于把会话弄丢；要查「为什么少了一条」时看的是这里的判据，不是异常
         */
        [[nodiscard]] static std::vector<HttpCookie> parseCookieHeader(std::string_view headerValue);

    private:
        std::string m_name;  ///< Cookie 名字
        std::string m_value; ///< Cookie 取值

        std::optional<std::string>                           m_path;          ///< Path 属性
        std::optional<std::string>                           m_domain;        ///< Domain 属性
        std::optional<std::int64_t>                          m_maxAgeSeconds; ///< Max-Age 属性
        std::optional<std::chrono::system_clock::time_point> m_expiresAt;     ///< Expires 属性

        bool m_isSecure{false};   ///< Secure 属性（未设即 false）
        bool m_isHttpOnly{false}; ///< HttpOnly 属性（未设即 false）

        std::optional<CookieSameSitePolicy> m_sameSite; ///< SameSite 属性
    };
} // namespace AsynGyanis::Net
