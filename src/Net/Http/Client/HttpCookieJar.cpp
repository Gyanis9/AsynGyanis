#include "Net/Http/Client/HttpCookieJar.h"

#include <algorithm>
#include <cctype>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 把主机名/域名按 ASCII 转小写并折掉前导点
         * @details 域名比较不区分大小写（RFC 6265 §5.1.3），但只在 ASCII 范围内折叠——
         *          走 locale 相关的 tolower 会让同一个罐子在不同语言设置的机器上匹配出不同结果
         */
        [[nodiscard]] std::string normalizeDomain(std::string_view domain)
        {
            while (!domain.empty() && domain.front() == '.')
            {
                domain.remove_prefix(1);
            }
            std::string normalized;
            normalized.reserve(domain.size());
            for (const char character: domain)
            {
                normalized.push_back((character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character);
            }
            return normalized;
        }

        /**
         * @brief 域名匹配（RFC 6265 §5.1.3）
         * @details 两个条件之一：逐字相同；或请求主机是给定域的真子域
         *          （`shop.example.com` 对 `example.com` 成立，对 `example.com.evil.net` 不成立——
         *          后者虽然后缀里含着那串字符，但结尾不是它）
         */
        [[nodiscard]] bool domainMatches(std::string_view requestHost, const std::string &cookieDomain) noexcept
        {
            if (requestHost == cookieDomain)
            {
                return true;
            }
            if (requestHost.size() <= cookieDomain.size() + 1)
            {
                return false;
            }
            const std::string_view suffix = requestHost.substr(requestHost.size() - cookieDomain.size());
            return suffix == cookieDomain && requestHost[requestHost.size() - cookieDomain.size() - 1] == '.';
        }

        /**
         * @brief 路径匹配（RFC 6265 §5.1.4）
         * @details 逐字相同；或 Cookie 路径是请求路径的前缀，且前缀之后紧跟 '/'（否则
         *          `/api` 会把 `/apifuzz` 也罩进去）；或 Cookie 路径本身以 '/' 结尾
         */
        [[nodiscard]] bool pathMatches(std::string_view cookiePath, std::string_view requestPath) noexcept
        {
            if (cookiePath.empty())
            {
                return false;
            }
            if (requestPath.size() < cookiePath.size())
            {
                return false;
            }
            if (requestPath.substr(0, cookiePath.size()) != cookiePath)
            {
                return false;
            }
            if (requestPath.size() == cookiePath.size() || cookiePath.back() == '/')
            {
                return true;
            }
            return requestPath[cookiePath.size()] == '/';
        }

        /// 摘掉路径上的查询串：默认路径按 RFC 只看 uri-path
        [[nodiscard]] std::string_view stripQuery(std::string_view requestPath) noexcept
        {
            const std::size_t queryPosition = requestPath.find('?');
            return queryPosition == std::string_view::npos ? requestPath : requestPath.substr(0, queryPosition);
        }

        /**
         * @brief 按 RFC 6265 §5.1.4 推缺省路径
         * @details 不是「截到最后一个斜杠」这么含糊：路径只有一个斜杠（`/a`）时缺省是 `/`，
         *          多个斜杠时去掉最右一个斜杠及其之后的部分（`/api/v1/users` → `/api/v1`）。
         *          推错一位就会让 Cookie 的作用域静默变宽或变窄
         */
        [[nodiscard]] std::string computeDefaultPath(std::string_view requestPath)
        {
            const std::string_view path = stripQuery(requestPath);
            if (path.empty() || path.front() != '/')
            {
                return "/";
            }
            if (std::ranges::count(path, '/') < 2)
            {
                return "/";
            }
            return std::string(path.substr(0, path.rfind('/')));
        }
    } // namespace

    bool HttpCookieJar::isIpAddressLiteral(const std::string_view host) noexcept
    {
        if (host.empty())
        {
            return false;
        }
        if (host.find(':') != std::string_view::npos)
        {
            return true; // IPv6（ParsedUrl 已把方括号摘掉）
        }
        if (host.find('.') == std::string_view::npos)
        {
            return false; // 没有点就不是 IPv4 字面量，也不是单标签主机名的对家
        }
        std::size_t octetLength = 0;
        unsigned    octetValue  = 0;
        const auto  resetOctet  = [&octetLength, &octetValue]() noexcept
        {
            if (octetLength == 0 || octetLength > 3 || octetValue > 255U)
            {
                return false;
            }
            octetLength = 0;
            octetValue  = 0;
            return true;
        };
        for (const char character: host)
        {
            if (character == '.')
            {
                if (!resetOctet())
                {
                    return false;
                }
                continue;
            }
            if (character < '0' || character > '9')
            {
                return false;
            }
            octetValue = octetValue * 10U + static_cast<unsigned>(character - '0');
            ++octetLength;
        }
        return resetOctet();
    }

    HttpCookieJar::HttpCookieJar(const Limits limits) noexcept : m_limits(limits)
    {
    }

    bool HttpCookieJar::isSameScope(const Entry &entry, const Entry &candidate) noexcept
    {
        return entry.name == candidate.name && entry.domain == candidate.domain && entry.path == candidate.path && entry.isHostOnly == candidate.isHostOnly;
    }

    bool HttpCookieJar::matches(const Entry &entry, const std::string_view requestHost, const bool isSecureConnection, const std::string_view requestPath) noexcept
    {
        if (entry.isHostOnly)
        {
            if (normalizeDomain(requestHost) != entry.domain)
            {
                return false;
            }
        } else if (!domainMatches(normalizeDomain(requestHost), entry.domain))
        {
            return false;
        }

        // Secure 的 Cookie 绝不走明文：这一条漏了，登录态就会在重定向链上被顺走
        if (entry.isSecure && !isSecureConnection)
        {
            return false;
        }
        return pathMatches(entry.path, stripQuery(requestPath));
    }

    void HttpCookieJar::enforceLimitsLocked()
    {
        // 先收单个域：某个站点狂发 Cookie 不该把别的站点的会话挤掉
        while (m_limits.maximumCookiesPerDomain > 0U && m_entries.size() > 1U)
        {
            const Entry *victim             = nullptr;
            std::size_t  largestDomainCount = 0;
            for (const Entry &probe: m_entries)
            {
                const std::size_t count = static_cast<std::size_t>(std::ranges::count_if(m_entries, [&probe](const Entry &entry) { return entry.domain == probe.domain; }));
                if (count <= m_limits.maximumCookiesPerDomain)
                {
                    continue;
                }
                if (count > largestDomainCount || (victim != nullptr && probe.sequence < victim->sequence))
                {
                    largestDomainCount = count;
                    victim             = &probe;
                }
            }
            if (victim == nullptr)
            {
                break;
            }
            // 挑该域里插入最早的那条淘汰
            const std::string victimDomain = victim->domain;
            const Entry      *oldest       = victim;
            for (const Entry &candidate: m_entries)
            {
                if (candidate.domain == victimDomain && candidate.sequence < oldest->sequence)
                {
                    oldest = &candidate;
                }
            }
            m_entries.erase(std::ranges::find_if(m_entries, [oldest](const Entry &entry) { return entry.sequence == oldest->sequence; }));
        }

        while (m_limits.maximumTotalCookies > 0U && m_entries.size() > m_limits.maximumTotalCookies)
        {
            const auto victim = std::ranges::min_element(m_entries, [](const Entry &left, const Entry &right) { return left.sequence < right.sequence; });
            if (victim == m_entries.end())
            {
                break;
            }
            m_entries.erase(victim);
        }
    }

    void HttpCookieJar::storeFromResponse(const std::string_view requestHost, const bool isSecureConnection, const std::string_view requestPath,
                                          const std::vector<std::string> &setCookieHeaderValues, const std::chrono::system_clock::time_point receivedAt)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);

        const std::string requestDomain = normalizeDomain(requestHost);
        if (requestDomain.empty())
        {
            return;
        }

        for (const std::string &headerValue: setCookieHeaderValues)
        {
            const std::optional<HttpCookie> parsedCookie = HttpCookie::parseSetCookie(headerValue);
            if (!parsedCookie.has_value())
            {
                continue;
            }

            Entry entry;
            entry.name     = parsedCookie->name();
            entry.value    = parsedCookie->value();
            entry.isSecure = parsedCookie->isSecure();
            entry.path     = parsedCookie->path().has_value() ? *parsedCookie->path() : computeDefaultPath(requestPath);

            if (parsedCookie->domain().has_value())
            {
                // RFC 6265 §5.2.3：IP 字面量不接受 Domain 属性，且属性必须罩得住请求主机
                if (isIpAddressLiteral(requestDomain))
                {
                    continue;
                }
                const std::string cookieDomain = normalizeDomain(*parsedCookie->domain());
                if (cookieDomain.empty() || !domainMatches(requestDomain, cookieDomain))
                {
                    continue;
                }
                entry.domain     = cookieDomain;
                entry.isHostOnly = false;
            } else
            {
                entry.domain     = requestDomain;
                entry.isHostOnly = true;
            }

            if (entry.isSecure && !isSecureConnection)
            {
                // 明文连接上收到的 Secure Cookie 直接丢：收下就等于留着一次误发的机会
                continue;
            }

            std::optional<std::chrono::system_clock::time_point> expiryAt;
            if (parsedCookie->maxAgeSeconds().has_value())
            {
                const std::int64_t seconds = *parsedCookie->maxAgeSeconds();
                if (seconds <= 0)
                {
                    // 删除语义：把同作用域那条摘掉就不再放回
                    std::erase_if(m_entries, [&entry](const Entry &stored) { return isSameScope(stored, entry); });
                    continue;
                }
                expiryAt = receivedAt + std::chrono::seconds(seconds);
            } else if (parsedCookie->expiresAt().has_value())
            {
                expiryAt = *parsedCookie->expiresAt();
            }
            entry.expiresAt = expiryAt;

            if (expiryAt.has_value() && *expiryAt <= receivedAt)
            {
                std::erase_if(m_entries, [&entry](const Entry &stored) { return isSameScope(stored, entry); });
                continue;
            }

            entry.sequence = m_sequence++;
            // 同名同域同路径即替换：累积两份会让下一次同时发两条，服务端拿到哪条全看顺序
            std::erase_if(m_entries, [&entry](const Entry &stored) { return isSameScope(stored, entry); });
            m_entries.push_back(std::move(entry));
            enforceLimitsLocked();
        }
    }

    std::optional<std::string> HttpCookieJar::buildRequestHeader(const std::string_view requestHost, const bool isSecureConnection, const std::string_view requestPath) const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const std::string                 header = [this, requestHost, isSecureConnection, requestPath]
        {
            const auto now = std::chrono::system_clock::now();

            // 顺手把已过期的摘掉：不然它们只是每请求一次都参与一次匹配判断的僵尸
            std::vector<const Entry *> candidates;
            candidates.reserve(m_entries.size());
            for (const Entry &entry: m_entries)
            {
                if (entry.expiresAt.has_value() && *entry.expiresAt <= now)
                {
                    continue;
                }
                if (matches(entry, requestHost, isSecureConnection, requestPath))
                {
                    candidates.push_back(&entry);
                }
            }

            // RFC 6265 §5.4：路径长的在前，长度相同则先存的在前
            std::ranges::sort(candidates,
                              [](const Entry *left, const Entry *right)
                              {
                                  if (left->path.size() != right->path.size())
                                  {
                                      return left->path.size() > right->path.size();
                                  }
                                  return left->sequence < right->sequence;
                              });

            std::string text;
            for (const Entry *entry: candidates)
            {
                if (!text.empty())
                {
                    text += "; ";
                }
                text += entry->name;
                text += "=";
                text += entry->value;
            }
            return text;
        }();

        if (header.empty())
        {
            return std::nullopt;
        }
        return header;
    }

    std::size_t HttpCookieJar::cookieCount() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

    void HttpCookieJar::clear() noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }
} // namespace AsynGyanis::Net
