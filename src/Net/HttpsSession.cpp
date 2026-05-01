#include "HttpsSession.h"

#include "Base/Exception.h"
#include "Base/Logger.h"
#include "HttpSession.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace Net
{
    HttpsSession::HttpsSession(Core::EventLoop &loop, Core::TlsSocket tlsSocket, Router &router) :
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
        }
        catch (const std::exception &e)
        {
            LOG_ERROR_FMT("TLS handshake failed: {} (fd={})", e.what(), m_tlsSocket.fd());
            m_tlsSocket.close();
            co_return;
        }

        LOG_DEBUG_FMT("TLS handshake done (fd={})", m_tlsSocket.fd());

        // Step 2: HTTP keep-alive loop over TLS (shared template)
        co_await detail::httpKeepAliveLoop(
            m_tlsSocket, m_router, m_parser, m_recvBuffer,
            [this]() { return isAlive(); });

        LOG_DEBUG_FMT("HttpsSession closing fd={}", m_tlsSocket.fd());
        m_tlsSocket.close();
        co_return;
    }

}
