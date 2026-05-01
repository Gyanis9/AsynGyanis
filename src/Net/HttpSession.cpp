#include "HttpSession.h"

#include "Base/Exception.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace Net
{
    HttpSession::HttpSession(Core::EventLoop &loop, Core::AsyncSocket socket, Router &router) :
        Core::Connection(loop, std::move(socket)), m_router(router),
        m_recvBuffer(m_recvBufferSize)
    {
    }

    Core::Task<> HttpSession::start()
    {
        co_await detail::httpKeepAliveLoop(
            socket(), m_router, m_parser, m_recvBuffer,
            [this]() { return isAlive(); });
        close();
        co_return;
    }

    bool HttpSession::shouldKeepAlive(const HttpRequest &req, const HttpResponse &res)
    {
        bool keepAlive = true;
        if (const auto &version = req.httpVersion(); version.starts_with("HTTP/1.0") || version.starts_with("HTTP/0.9"))
        {
            keepAlive = false;
        }

        if (const auto connectionHeader = req.getHeader("connection"); connectionHeader.has_value())
        {
            std::string val = connectionHeader.value();
            std::ranges::transform(val, val.begin(),
                                   [](const unsigned char c)
                                   {
                                       return std::tolower(c);
                                   });
            if (val == "close")
            {
                keepAlive = false;
            } else if (val == "keep-alive")
            {
                keepAlive = true;
            }
        }

        if (const auto resConnection = res.headers().find("connection"); resConnection != res.headers().end())
        {
            std::string val = resConnection->second;
            std::ranges::transform(val, val.begin(),
                                   [](const unsigned char c)
                                   {
                                       return std::tolower(c);
                                   });
            if (val == "close")
            {
                keepAlive = false;
            } else if (val == "keep-alive")
            {
                keepAlive = true;
            }
        }

        return keepAlive;
    }

}
