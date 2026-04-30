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

        size_t start = 0;
        while (start < query.size())
        {
            const auto eqPos  = query.find('=', start);
            auto       ampPos = query.find('&', start);
            if (ampPos == std::string_view::npos)
                ampPos = query.size();

            if (eqPos != std::string_view::npos && eqPos < ampPos)
            {
                std::string key(query.substr(start, eqPos - start));
                std::string value(query.substr(eqPos + 1, ampPos - eqPos - 1));
                params[std::move(key)] = std::move(value);
            } else
            {
                std::string key(query.substr(start, ampPos - start));
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
