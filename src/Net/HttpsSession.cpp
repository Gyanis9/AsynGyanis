/**
 * @file HttpsSession.cpp
 * @brief HTTPS 会话，先完成 TLS 握手再在加密通道上跑 HTTP 周期
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

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
        Core::Connection(Core::AsyncSocket(loop, -1)),
        m_tlsSocket(std::move(tlsSocket)), m_router(router),
        m_receiveBuffer(m_receiveBufferSize)
    {
    }

    Core::Task<> HttpsSession::start()
    {
        // 步骤 1：TLS 握手
        try
        {
            co_await m_tlsSocket.handshake();
        } catch (const std::exception &e)
        {
            LOG_ERROR_FMT("TLS handshake failed: {} (fileDescriptor={})", e.what(), m_tlsSocket.fileDescriptor());
            m_tlsSocket.close();
            co_return;
        }

        LOG_DEBUG_FMT("TLS handshake done (fileDescriptor={})", m_tlsSocket.fileDescriptor());

        // 步骤 2：基于 TLS 的 HTTP keep-alive 循环（共享模板）
        co_await detail::httpKeepAliveLoop(
                m_tlsSocket, m_router, m_parser, m_receiveBuffer,
                [this]()
                {
                    return isAlive();
                });

        LOG_DEBUG_FMT("HttpsSession closing fileDescriptor={}", m_tlsSocket.fileDescriptor());
        m_tlsSocket.close();
        co_return;
    }

}
