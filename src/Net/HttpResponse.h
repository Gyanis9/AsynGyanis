/**
 * @file HttpResponse.h
 * @brief HTTP response builder and serializer
 * @copyright Copyright (c) 2026
 */
#ifndef NET_HTTPRESPONSE_H
#define NET_HTTPRESPONSE_H

#include <string>
#include <string_view>
#include <unordered_map>

namespace Net
{

    class HttpResponse
    {
    public:
        HttpResponse();

        void setStatus(int code);
        int  status() const;

        void                                                setHeader(const std::string &name, const std::string &value);
        const std::unordered_map<std::string, std::string> &headers() const;

        void             setBody(std::string_view body);
        std::string_view body() const;

        std::string toString() const;

        static HttpResponse ok(std::string body);
        static HttpResponse notFound();
        static HttpResponse serverError(std::string msg);

        void reset();

    private:
        static const char *statusMessage(int code);

        int                                          m_status;
        std::unordered_map<std::string, std::string> m_headers;
        std::string                                  m_body;
    };

}

#endif
