#include "HttpsSession.h"

#include "Base/Exception.h"
#include "Base/Logger.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <vector>

namespace Net
{
    HttpsSession::HttpsSession(Core::EventLoop &loop, Core::TlsSocket tlsSocket, Router &router) :
        // Pass a dummy socket (-1) to Connection; the real fd is managed by m_tlsSocket.
        Core::Connection(loop, Core::AsyncSocket(loop, -1)),
        m_tlsSocket(std::move(tlsSocket)), m_router(router),
        m_recvBuffer(m_recvBufferSize)
    {
    }

    Core::Task<> HttpsSession::start()
    {
        // Step 1: TLS handshake
        try
        {
            co_await m_tlsSocket.handshake();
        } catch (const std::exception &e)
        {
            LOG_ERROR_FMT("HttpsSession TLS handshake failed: {} (fd={})", e.what(), m_tlsSocket.fd());
            m_tlsSocket.close();
            co_return;
        }

        LOG_DEBUG_FMT("HttpsSession TLS handshake done (fd={})", m_tlsSocket.fd());

        // Step 2: HTTP keep-alive loop over TLS
        bool keepAlive = true;

        auto sendAll = [this](const std::string_view data) -> Core::Task<>
        {
            size_t totalSent = 0;
            while (totalSent < data.size())
            {
                const ssize_t n = co_await m_tlsSocket.asyncSend(data.data() + totalSent,
                                                                 data.size() - totalSent);
                if (n <= 0)
                {
                    LOG_WARN_FMT("HttpsSession sendAll: asyncSend returned {} (fd={})", n, m_tlsSocket.fd());
                    co_return;
                }
                totalSent += static_cast<size_t>(n);
            }
        };

        while (keepAlive && isAlive())
        {
            std::string errorResponse;
            bool        hasError = false;

            try
            {
                ssize_t n = co_await m_tlsSocket.asyncRecv(m_recvBuffer.data(), m_recvBuffer.size());
                if (n <= 0)
                {
                    LOG_DEBUG_FMT("HttpsSession recv returned {} (fd={}), closing", n, m_tlsSocket.fd());
                    break;
                }

                auto status = m_parser.parse(m_recvBuffer.data(), static_cast<size_t>(n));

                while (status == ParseStatus::NeedMore)
                {
                    n = co_await m_tlsSocket.asyncRecv(m_recvBuffer.data(), m_recvBuffer.size());
                    if (n <= 0)
                    {
                        LOG_DEBUG_FMT("HttpsSession recv(2) returned {} (fd={}), closing", n, m_tlsSocket.fd());
                        co_return;
                    }
                    status = m_parser.parse(m_recvBuffer.data(), static_cast<size_t>(n));
                }

                if (status == ParseStatus::Error)
                {
                    LOG_WARN_FMT("HttpsSession parse error: {} (fd={})", m_parser.errorMessage(), m_tlsSocket.fd());
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
                        LOG_ERROR_FMT("HttpsSession route error: {} (fd={})", e.what(), m_tlsSocket.fd());
                        res = HttpResponse::serverError(e.what());
                    }

                    const auto &version      = req.httpVersion();
                    const bool  isHttp10     = version.starts_with("HTTP/1.0") || version.starts_with("HTTP/0.9");
                    bool        reqKeepAlive = !isHttp10;

                    if (const auto connectionHeader = req.getHeader("connection"); connectionHeader.has_value())
                    {
                        std::string val = connectionHeader.value();
                        std::ranges::transform(val, val.begin(), [](const unsigned char c)
                        {
                            return std::tolower(c);
                        });
                        if (val == "close")
                            reqKeepAlive = false;
                        else if (val == "keep-alive")
                            reqKeepAlive = true;
                    }

                    if (!reqKeepAlive)
                        res.setHeader("connection", "close");
                    else if (isHttp10 && res.headers().find("connection") == res.headers().end())
                        res.setHeader("connection", "keep-alive");

                    keepAlive = reqKeepAlive;

                    std::string responseStr = res.toString();
                    if (responseStr.size() <= 4096)
                        co_await m_tlsSocket.asyncSend(responseStr.data(), responseStr.size());
                    else
                        co_await sendAll(responseStr);

                    if (keepAlive)
                        m_parser.reset();
                }
            } catch (const std::exception &e)
            {
                LOG_ERROR_FMT("HttpsSession exception: {} (fd={})", e.what(), m_tlsSocket.fd());
                HttpResponse res = HttpResponse::serverError(e.what());
                res.setHeader("connection", "close");
                errorResponse = res.toString();
                hasError      = true;
            }

            if (hasError)
            {
                if (errorResponse.size() <= 4096)
                    co_await m_tlsSocket.asyncSend(errorResponse.data(), errorResponse.size());
                else
                    co_await sendAll(errorResponse);
                keepAlive = false;
            }
        }

        LOG_DEBUG_FMT("HttpsSession closing fd={}", m_tlsSocket.fd());
        m_tlsSocket.close();
        co_return;
    }

}
