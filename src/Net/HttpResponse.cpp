#include "HttpResponse.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>

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

    void HttpResponse::setHttpVersion(std::string version)
    {
        m_httpVersion = std::move(version);
    }

    int HttpResponse::status() const
    {
        return m_status;
    }

    void HttpResponse::setHeader(const std::string &name, const std::string &value)
    {
        std::string lowerName = name;
        std::ranges::transform(lowerName, lowerName.begin(),
                               [](const unsigned char c)
                               {
                                   return std::tolower(c);
                               });
        // Set-Cookie 头不能合并（RFC 6265），重复设置时用唯一后缀保留
        if (const auto it = m_headers.find(lowerName); it != m_headers.end())
        {
            if (lowerName == "set-cookie")
            {
                size_t      idx = 1;
                std::string indexedKey;
                do
                {
                    indexedKey = lowerName + "_" + std::to_string(idx++);
                } while (m_headers.contains(indexedKey));
                m_headers[std::move(indexedKey)] = value;
                return;
            }
        }
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
            case 202:
                return "Accepted";
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
            case 307:
                return "Temporary Redirect";
            case 308:
                return "Permanent Redirect";
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
            case 406:
                return "Not Acceptable";
            case 408:
                return "Request Timeout";
            case 409:
                return "Conflict";
            case 410:
                return "Gone";
            case 413:
                return "Payload Too Large";
            case 414:
                return "URI Too Long";
            case 415:
                return "Unsupported Media Type";
            case 416:
                return "Range Not Satisfiable";
            case 422:
                return "Unprocessable Content";
            case 429:
                return "Too Many Requests";
            case 451:
                return "Unavailable For Legal Reasons";
            case 500:
                return "Internal Server Error";
            case 501:
                return "Not Implemented";
            case 502:
                return "Bad Gateway";
            case 503:
                return "Service Unavailable";
            case 504:
                return "Gateway Timeout";
            case 505:
                return "HTTP Version Not Supported";
            default:
                return "";
        }
    }

    std::string HttpResponse::toString() const
    {
        // Pre-compute size to avoid reallocations（含自动添加的 content-type / content-length）
        size_t estimated        = 48 + m_body.size(); // status line + CRLF CRLF
        bool   hasContentLength = false;
        bool   hasContentType   = false;
        for (const auto &[key, value]: m_headers)
        {
            estimated += key.size() + value.size() + 4; // "key: value\r\n"
            if (key == "content-length")
                hasContentLength = true;
            else if (key == "content-type")
                hasContentType = true;
        }
        if (!hasContentType && !m_body.empty())
            estimated += 28; // "content-type: text/plain\r\n"
        if (!hasContentLength)
            estimated += 16 + 20 + 2; // "content-length: " + max digits + "\r\n"

        std::string result;
        result.reserve(estimated);

        // Status line（使用请求对应的 HTTP 版本而非硬编码 HTTP/1.1）
        result.append(m_httpVersion);
        result.push_back(' ');
        // 状态码使用栈缓冲避免 std::to_string 堆分配
        char       statusBuf[8];
        const auto [ptr, _] = std::to_chars(statusBuf, statusBuf + sizeof(statusBuf), m_status);
        result.append(statusBuf, static_cast<size_t>(ptr - statusBuf));
        result.push_back(' ');
        result.append(statusMessage(m_status));
        result.append("\r\n");

        for (const auto &[key, value]: m_headers)
        {
            result.append(key);
            result.append(": ");
            result.append(value);
            result.append("\r\n");
        }

        if (!hasContentType && !m_body.empty())
            result.append("content-type: text/plain\r\n");

        if (!hasContentLength)
        {
            result.append("content-length: ");
            char       lenBuf[32];
            const auto [lp, ec2] = std::to_chars(lenBuf, lenBuf + sizeof(lenBuf),
                                                 static_cast<uint64_t>(m_body.size()));
            (void) ec2;
            result.append(lenBuf, static_cast<size_t>(lp - lenBuf));
            result.append("\r\n");
        }

        result.append("\r\n");
        result.append(m_body);

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
        m_status      = 200;
        m_httpVersion = "HTTP/1.1";
        m_headers.clear();
        m_body.clear();
    }

}
