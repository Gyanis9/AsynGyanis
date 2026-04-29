/**
 * @file HttpParser.h
 * @brief HTTP/1.1 parser wrapping llhttp C library
 * @copyright Copyright (c) 2026
 */
#ifndef NET_HTTPPARSER_H
#define NET_HTTPPARSER_H

#include "HttpRequest.h"

#include <llhttp.h>

#include <string>

namespace Net
{

    enum class ParseStatus
    {
        Ok,
        Error,
        NeedMore,
        Done
    };

    class HttpParser
    {
    public:
        HttpParser();
        ~HttpParser();

        HttpParser(const HttpParser &)            = delete;
        HttpParser &operator=(const HttpParser &) = delete;

        ParseStatus  parse(const char *data, size_t len);
        void         reset();
        HttpRequest &request();
        bool         hasError() const;
        std::string  errorMessage() const;

    private:
        static int onUrl(llhttp_t *parser, const char *data, size_t len);
        static int onHeaderField(llhttp_t *parser, const char *data, size_t len);
        static int onHeaderValue(llhttp_t *parser, const char *data, size_t len);
        static int onBody(llhttp_t *parser, const char *data, size_t len);
        static int onMessageComplete(llhttp_t *parser);

        llhttp_t          m_parser{};
        llhttp_settings_t m_settings{};
        HttpRequest       m_currentRequest;
        std::string       m_currentHeaderField;
        std::string       m_currentUrl;
        bool              m_error{false};
        std::string       m_errorMessage;
        bool              m_complete{false};
    };

}

#endif
