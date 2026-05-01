#include "HttpParser.h"

#include <format>

namespace Net
{

    HttpParser::HttpParser()
    {
        llhttp_settings_init(&m_settings);

        m_settings.on_url              = onUrl;
        m_settings.on_header_field     = onHeaderField;
        m_settings.on_header_value     = onHeaderValue;
        m_settings.on_body             = onBody;
        m_settings.on_message_complete = onMessageComplete;

        llhttp_init(&m_parser, HTTP_REQUEST, &m_settings);
        m_parser.data = this;
    }

    HttpParser::~HttpParser() = default;

    ParseStatus HttpParser::parse(const char *data, const size_t len)
    {
        if (m_complete)
        {
            return ParseStatus::Done;
        }

        const auto result = llhttp_execute(&m_parser, data, len);

        if (m_complete)
            return ParseStatus::Done;

        // HPE_OK 但消息未完成 → 需要更多数据
        if (result == HPE_OK)
            return ParseStatus::NeedMore;

        if (result == HPE_PAUSED)
            return ParseStatus::Ok;

        if (m_parser.reason && m_parser.reason[0] != '\0')
        {
            m_errorMessage = m_parser.reason;
        } else
        {
            m_errorMessage = std::format("HTTP parse error: {}", llhttp_errno_name(result));
        }
        m_error = true;
        return ParseStatus::Error;
    }

    void HttpParser::reset()
    {
        m_currentRequest.reset();
        m_currentHeaderField.clear();
        m_currentUrl.clear();
        m_error = false;
        m_errorMessage.clear();
        m_complete = false;
        llhttp_init(&m_parser, HTTP_REQUEST, &m_settings);
        m_parser.data = this;
    }

    HttpRequest &HttpParser::request()
    {
        return m_currentRequest;
    }

    bool HttpParser::hasError() const
    {
        return m_error;
    }

    std::string HttpParser::errorMessage() const
    {
        return m_errorMessage;
    }

    int HttpParser::onUrl(llhttp_t *parser, const char *data, const size_t len)
    {
        const auto self = static_cast<HttpParser *>(parser->data);
        self->m_currentUrl.append(data, len);
        return 0;
    }

    int HttpParser::onHeaderField(llhttp_t *parser, const char *data, const size_t len)
    {
        const auto self = static_cast<HttpParser *>(parser->data);
        self->m_currentHeaderField.append(data, len);
        return 0;
    }

    int HttpParser::onHeaderValue(llhttp_t *parser, const char *data, const size_t len)
    {
        const auto  self = static_cast<HttpParser *>(parser->data);
        std::string value(data, len);
        self->m_currentRequest.addHeader(self->m_currentHeaderField, std::move(value));
        self->m_currentHeaderField.clear();
        return 0;
    }

    int HttpParser::onBody(llhttp_t *parser, const char *data, const size_t len)
    {
        const auto self = static_cast<HttpParser *>(parser->data);
        self->m_currentRequest.appendBody(data, len);
        return 0;
    }

    int HttpParser::onMessageComplete(llhttp_t *parser)
    {
        auto *self = static_cast<HttpParser *>(parser->data);

        HttpMethod method;
        switch (parser->method)
        {
            case HTTP_GET:
                method = HttpMethod::GET;
                break;
            case HTTP_POST:
                method = HttpMethod::POST;
                break;
            case HTTP_PUT:
                method = HttpMethod::PUT;
                break;
            case HTTP_DELETE:
                method = HttpMethod::DELETE;
                break;
            case HTTP_PATCH:
                method = HttpMethod::PATCH;
                break;
            case HTTP_HEAD:
                method = HttpMethod::HEAD;
                break;
            case HTTP_OPTIONS:
                method = HttpMethod::OPTIONS;
                break;
            default:
                method = HttpMethod::UNKNOWN;
                break;
        }

        self->m_currentRequest.setMethod(method);
        self->m_currentRequest.setUri(self->m_currentUrl);

        // 静态查找表避免每次请求的 std::format 分配
        static constexpr const char *kHttpVersions[2][4] = {
                {"HTTP/0.9", "HTTP/0.1", "HTTP/0.2", "HTTP/0.3"},
                {"HTTP/1.0", "HTTP/1.1", "HTTP/1.2", "HTTP/1.3"},
        };
        const auto major = static_cast<size_t>(parser->http_major);
        const auto minor = static_cast<size_t>(parser->http_minor);
        if (major < 2 && minor < 4)
            self->m_currentRequest.setHttpVersion(kHttpVersions[major][minor]);
        else
            self->m_currentRequest.setHttpVersion(
                    std::format("HTTP/{}.{}", parser->http_major, parser->http_minor));

        self->m_complete = true;
        return 0;
    }

}
