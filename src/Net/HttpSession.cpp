#include "HttpSession.h"

#include "Base/Exception.h"

#include <algorithm>
#include <cctype>

namespace Net
{
    HttpSession::HttpSession(Core::EventLoop &loop, Core::AsyncSocket socket, Router &router) :
        Core::Connection(std::move(socket)), m_router(router),
        m_receiveBuffer(m_receiveBufferSize)
    {
    }

    Core::Task<> HttpSession::start()
    {
        co_await detail::httpKeepAliveLoop(
                socket(), m_router, m_parser, m_receiveBuffer,
                [this]()
                {
                    return isAlive();
                });
        close();
        co_return;
    }

    bool HttpSession::shouldKeepAlive(const HttpRequest &request, const HttpResponse &response)
    {
        bool keepAlive = true;
        if (const auto &version = request.httpVersion(); version.starts_with("HTTP/1.0") || version.starts_with("HTTP/0.9"))
        {
            keepAlive = false;
        }

        if (const auto connectionHeader = request.getHeader("connection"); connectionHeader.has_value())
        {
            std::string value = connectionHeader.value();
            std::ranges::transform(value, value.begin(),
                                   [](const unsigned char c)
                                   {
                                       return std::tolower(c);
                                   });
            if (value == "close")
            {
                keepAlive = false;
            } else if (value == "keep-alive")
            {
                keepAlive = true;
            }
        }

        if (const auto responseConnection = response.headers().find("connection"); responseConnection != response.headers().end())
        {
            std::string value = responseConnection->second;
            std::ranges::transform(value, value.begin(),
                                   [](const unsigned char c)
                                   {
                                       return std::tolower(c);
                                   });
            if (value == "close")
            {
                keepAlive = false;
            } else if (value == "keep-alive")
            {
                keepAlive = true;
            }
        }

        return keepAlive;
    }

}
