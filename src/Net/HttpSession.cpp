#include "HttpSession.h"

#include "Base/Exception.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <vector>

namespace Net
{
    HttpSession::HttpSession(Core::EventLoop &loop, Core::AsyncSocket socket, Router &router) :
        Core::Connection(loop, std::move(socket)), m_router(router),
        m_recvBuffer(m_recvBufferSize)
    {
    }

    Core::Task<> HttpSession::start()
    {
        bool keepAlive = true;

        auto sendAll = [this](const std::string_view data) -> Core::Task<>
        {
            size_t totalSent = 0;
            while (totalSent < data.size())
            {
                const ssize_t n = co_await socket().asyncSend(data.data() + totalSent,
                                                              data.size() - totalSent);
                if (n <= 0)
                    co_return;
                totalSent += static_cast<size_t>(n);
            }
        };

        while (keepAlive && isAlive())
        {
            std::string errorResponse;
            bool        hasError = false;

            try
            {
                ssize_t n = co_await socket().asyncRecv(m_recvBuffer.data(), m_recvBuffer.size());
                if (n <= 0)
                    break;

                auto status = m_parser.parse(m_recvBuffer.data(), static_cast<size_t>(n));

                while (status == ParseStatus::NeedMore)
                {
                    n = co_await socket().asyncRecv(m_recvBuffer.data(), m_recvBuffer.size());
                    if (n <= 0)
                        co_return;
                    status = m_parser.parse(m_recvBuffer.data(), static_cast<size_t>(n));
                }

                if (status == ParseStatus::Error)
                {
                    HttpResponse res;
                    res.setStatus(400);
                    res.setBody(std::format("Bad Request: {}", m_parser.errorMessage()));
                    res.setHeader("content-type", "text/plain");
                    res.setHeader("connection", "close");
                    errorResponse = res.toString();
                    hasError      = true;
                } else if (status == ParseStatus::Done)
                {
                    auto &       req = m_parser.request();
                    HttpResponse res;

                    try
                    {
                        co_await m_router.route(req, res);
                    } catch (const std::exception &e)
                    {
                        res = HttpResponse::serverError(e.what());
                    }

                    keepAlive = shouldKeepAlive(req, res);

                    const auto &version  = req.httpVersion();
                    const bool  isHttp10 = version.starts_with("HTTP/1.0") || version.starts_with("HTTP/0.9");

                    if (!keepAlive)
                        res.setHeader("connection", "close");
                    else if (isHttp10 && res.headers().find("connection") == res.headers().end())
                        res.setHeader("connection", "keep-alive");

                    std::string responseStr = res.toString();
                    // Fast path: single send for small responses (avoids coroutine frame)
                    if (responseStr.size() <= 4096)
                        co_await socket().asyncSend(responseStr.data(), responseStr.size());
                    else
                        co_await sendAll(responseStr);

                    if (keepAlive)
                        m_parser.reset();
                }
            } catch (const std::exception &e)
            {
                HttpResponse res = HttpResponse::serverError(e.what());
                res.setHeader("connection", "close");
                errorResponse = res.toString();
                hasError      = true;
            }

            if (hasError)
            {
                if (errorResponse.size() <= 4096)
                    co_await socket().asyncSend(errorResponse.data(), errorResponse.size());
                else
                    co_await sendAll(errorResponse);
                keepAlive = false;
            }
        }

        close();
        co_return;
    }

    bool HttpSession::shouldKeepAlive(const HttpRequest &req, const HttpResponse &res) const
    {
        bool keepAlive = true;
        if (const auto &version = req.httpVersion(); version.starts_with("HTTP/1.0") || version.starts_with("HTTP/0.9"))
            keepAlive = false;

        if (const auto connectionHeader = req.getHeader("connection"); connectionHeader.has_value())
        {
            std::string val = connectionHeader.value();
            std::ranges::transform(val, val.begin(),
                                   [](const unsigned char c)
                                   {
                                       return std::tolower(c);
                                   });
            if (val == "close")
                keepAlive = false;
            else if (val == "keep-alive")
                keepAlive = true;
        }

        if (const auto resConnection = res.headers().find("connection"); resConnection != res.headers().end())
        {
            std::string val = resConnection->second;
            std::ranges::transform(val, val.begin(),
                                   [](unsigned char c)
                                   {
                                       return std::tolower(c);
                                   });
            if (val == "close")
                keepAlive = false;
            else if (val == "keep-alive")
                keepAlive = true;
        }

        return keepAlive;
    }

}
