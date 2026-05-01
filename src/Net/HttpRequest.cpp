#include "HttpRequest.h"

#include <algorithm>
#include <cctype>

namespace Net
{

    HttpMethod HttpRequest::methodFromString(const std::string_view method)
    {
        if (method == "GET")
            return HttpMethod::GET;
        if (method == "POST")
            return HttpMethod::POST;
        if (method == "PUT")
            return HttpMethod::PUT;
        if (method == "DELETE")
            return HttpMethod::DELETE;
        if (method == "PATCH")
            return HttpMethod::PATCH;
        if (method == "HEAD")
            return HttpMethod::HEAD;
        if (method == "OPTIONS")
            return HttpMethod::OPTIONS;
        return HttpMethod::UNKNOWN;
    }

    void HttpRequest::setMethod(const HttpMethod method)
    {
        m_method = method;
    }

    HttpMethod HttpRequest::method() const
    {
        return m_method;
    }

    void HttpRequest::setUri(std::string uri)
    {
        m_uri = std::move(uri);
    }

    const std::string &HttpRequest::uri() const
    {
        return m_uri;
    }

    void HttpRequest::setHttpVersion(std::string version)
    {
        m_httpVersion = std::move(version);
    }

    const std::string &HttpRequest::httpVersion() const
    {
        return m_httpVersion;
    }

    void HttpRequest::addHeader(std::string key, std::string value)
    {
        std::ranges::transform(key, key.begin(),
                               [](const unsigned char c)
                               {
                                   return std::tolower(c);
                               });
        // Set-Cookie 头不能合并（RFC 6265），其他重复头用逗号合并（RFC 7230 §3.2.2）
        if (const auto it = m_headers.find(key); it != m_headers.end())
        {
            if (key == "set-cookie")
            {
                // 用唯一后缀保留多个 Set-Cookie：set-cookie, set-cookie_1, set-cookie_2 ...
                size_t idx = 1;
                std::string indexedKey;
                do
                {
                    indexedKey = key + "_" + std::to_string(idx++);
                } while (m_headers.find(indexedKey) != m_headers.end());
                m_headers[std::move(indexedKey)] = std::move(value);
                return;
            }
            it->second.append(", ");
            it->second.append(value);
            return;
        }
        m_headers[std::move(key)] = std::move(value);
    }

    std::optional<std::string> HttpRequest::getHeader(const std::string &key) const
    {
        std::string lowerKey = key;
        std::ranges::transform(lowerKey, lowerKey.begin(),
                               [](const unsigned char c)
                               {
                                   return std::tolower(c);
                               });
        if (const auto it = m_headers.find(lowerKey); it != m_headers.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    const std::unordered_map<std::string, std::string> &HttpRequest::headers() const
    {
        return m_headers;
    }

    void HttpRequest::setBody(std::string body)
    {
        m_body = std::move(body);
    }

    void HttpRequest::appendBody(const char *data, const size_t len)
    {
        m_body.append(data, len);
    }

    std::string_view HttpRequest::body() const
    {
        return m_body;
    }

    std::string HttpRequest::path() const
    {
        if (const auto pos = m_uri.find('?'); pos != std::string::npos)
        {
            return m_uri.substr(0, pos);
        }
        return m_uri;
    }

    std::unordered_map<std::string, std::string> HttpRequest::queryParams() const
    {
        std::unordered_map<std::string, std::string> params;

        const auto pos = m_uri.find('?');
        if (pos == std::string::npos)
        {
            return params;
        }

        const std::string_view query(&m_uri[pos + 1], m_uri.size() - pos - 1);

        // URL percent-decode helper: %XX → char, + → space
        static constexpr auto percentDecode = [](const std::string_view src) -> std::string
        {
            std::string result;
            result.reserve(src.size());
            for (size_t i = 0; i < src.size(); ++i)
            {
                if (src[i] == '%' && i + 2 < src.size())
                {
                    const auto hi = src[i + 1];
                    const auto lo = src[i + 2];
                    auto hexVal = [](const char c) -> int
                    {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        return -1;
                    };
                    const int h = hexVal(hi);
                    const int l = hexVal(lo);
                    if (h >= 0 && l >= 0)
                    {
                        result.push_back(static_cast<char>((h << 4) | l));
                        i += 2;
                        continue;
                    }
                }
                if (src[i] == '+')
                    result.push_back(' ');
                else
                    result.push_back(src[i]);
            }
            return result;
        };

        size_t start = 0;
        while (start < query.size())
        {
            const auto eqPos  = query.find('=', start);
            auto       ampPos = query.find('&', start);
            if (ampPos == std::string_view::npos)
                ampPos = query.size();

            if (eqPos != std::string_view::npos && eqPos < ampPos)
            {
                auto key   = percentDecode(query.substr(start, eqPos - start));
                auto value = percentDecode(query.substr(eqPos + 1, ampPos - eqPos - 1));
                params[std::move(key)] = std::move(value);
            } else
            {
                auto key = percentDecode(query.substr(start, ampPos - start));
                params[std::move(key)] = "";
            }

            start = ampPos + 1;
        }

        return params;
    }

    void HttpRequest::setParam(std::string key, std::string value)
    {
        m_params[std::move(key)] = std::move(value);
    }

    std::optional<std::string> HttpRequest::param(const std::string &key) const
    {
        if (const auto it = m_params.find(key); it != m_params.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    void HttpRequest::reset()
    {
        m_method = HttpMethod::UNKNOWN;
        m_uri.clear();
        m_httpVersion.clear();
        m_headers.clear();
        m_body.clear();
        m_params.clear();
        m_cancelSource = std::stop_source{};
    }

    std::stop_token HttpRequest::cancelToken() const noexcept
    {
        return m_cancelSource.get_token();
    }

    std::stop_source &HttpRequest::cancelSource() noexcept
    {
        return m_cancelSource;
    }

    bool HttpRequest::requestCancel() const
    {
        return m_cancelSource.request_stop();
    }

}
