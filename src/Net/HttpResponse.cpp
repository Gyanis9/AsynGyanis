#include "HttpResponse.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace Net
{

    HttpResponse::HttpResponse() :
        m_status(200)
    {
    }

    void HttpResponse::setStatus(const int code)
    {
        m_status = code;
    }

    int HttpResponse::status() const
    {
        return m_status;
    }

    void HttpResponse::setHeader(const std::string &name, const std::string &value)
    {
        std::string lowerName = name;
        std::ranges::transform(lowerName, lowerName.begin(),
                               [](unsigned char c)
                               {
                                   return std::tolower(c);
                               });
        m_headers[std::move(lowerName)] = value;
    }

    const std::unordered_map<std::string, std::string> &HttpResponse::headers() const
    {
        return m_headers;
    }

    void HttpResponse::setBody(const std::string_view body)
    {
        m_body = std::string(body);
    }

    std::string_view HttpResponse::body() const
    {
        return m_body;
    }

    const char *HttpResponse::statusMessage(const int code)
    {
        switch (code)
        {
            case 200:
                return "OK";
            case 201:
                return "Created";
            case 204:
                return "No Content";
            case 206:
                return "Partial Content";
            case 301:
                return "Moved Permanently";
            case 302:
                return "Found";
            case 304:
                return "Not Modified";
            case 400:
                return "Bad Request";
            case 401:
                return "Unauthorized";
            case 403:
                return "Forbidden";
            case 404:
                return "Not Found";
            case 405:
                return "Method Not Allowed";
            case 408:
                return "Request Timeout";
            case 413:
                return "Payload Too Large";
            case 416:
                return "Range Not Satisfiable";
            case 429:
                return "Too Many Requests";
            case 500:
                return "Internal Server Error";
            case 502:
                return "Bad Gateway";
            case 503:
                return "Service Unavailable";
            default:
                return "Unknown";
        }
    }

    std::string HttpResponse::toString() const
    {
        std::string result;
        result.reserve(256 + m_body.size());

        result += std::format("HTTP/1.1 {} {}\r\n", m_status, statusMessage(m_status));

        bool hasContentLength = false;
        bool hasContentType   = false;

        for (const auto &[key, value]: m_headers)
        {
            result += std::format("{}: {}\r\n", key, value);
            if (key == "content-length")
                hasContentLength = true;
            if (key == "content-type")
                hasContentType = true;
        }

        if (!hasContentType && !m_body.empty())
            result += "Content-Type: text/plain\r\n";

        if (!hasContentLength)
            result += std::format("Content-Length: {}\r\n", m_body.size());

        result += "\r\n";
        result += m_body;

        return result;
    }

    HttpResponse HttpResponse::ok(std::string body)
    {
        HttpResponse res;
        res.setStatus(200);
        res.setBody(std::move(body));
        res.setHeader("content-type", "text/plain");
        return res;
    }

    HttpResponse HttpResponse::notFound()
    {
        HttpResponse res;
        res.setStatus(404);
        res.setBody("Not Found");
        res.setHeader("content-type", "text/plain");
        return res;
    }

    HttpResponse HttpResponse::serverError(std::string msg)
    {
        HttpResponse res;
        res.setStatus(500);
        res.setBody(std::move(msg));
        res.setHeader("content-type", "text/plain");
        return res;
    }

    void HttpResponse::reset()
    {
        m_status = 200;
        m_headers.clear();
        m_body.clear();
    }

}
