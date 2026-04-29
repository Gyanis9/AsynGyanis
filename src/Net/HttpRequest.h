/**
 * @file HttpRequest.h
 * @brief Parsed HTTP request data object
 * @copyright Copyright (c) 2026
 */
#ifndef NET_HTTPREQUEST_H
#define NET_HTTPREQUEST_H

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Net
{

    enum class HttpMethod
    {
        GET,
        POST,
        PUT,
        DELETE,
        PATCH,
        HEAD,
        OPTIONS,
        UNKNOWN
    };

    class HttpRequest
    {
    public:
        HttpRequest() = default;

        static HttpMethod methodFromString(std::string_view method);

        void       setMethod(HttpMethod method);
        HttpMethod method() const;

        void               setUri(std::string uri);
        const std::string &uri() const;

        void               setHttpVersion(std::string version);
        const std::string &httpVersion() const;

        void                                                addHeader(std::string key, std::string value);
        std::optional<std::string>                          getHeader(const std::string &key) const;
        const std::unordered_map<std::string, std::string> &headers() const;

        void             setBody(std::string body);
        std::string_view body() const;

        std::string                                  path() const;
        std::unordered_map<std::string, std::string> queryParams() const;

        void                       setParam(std::string key, std::string value);
        std::optional<std::string> param(const std::string &key) const;

        void reset();

    private:
        HttpMethod                                   m_method{HttpMethod::UNKNOWN};
        std::string                                  m_uri;
        std::string                                  m_httpVersion;
        std::unordered_map<std::string, std::string> m_headers;
        std::string                                  m_body;
        std::unordered_map<std::string, std::string> m_params;
    };

}

#endif
