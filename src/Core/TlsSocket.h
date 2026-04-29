/**
 * @file TlsSocket.h
 * @brief TLS socket wrapper — SSL_read/SSL_write with non-blocking epoll integration
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_TLSSOCKET_H
#define CORE_TLSSOCKET_H

#include "AsyncSocket.h"
#include "Task.h"

#include <openssl/ssl.h>

namespace Core
{
    class EventLoop;

    class TlsSocket
    {
    public:
        TlsSocket(SSL *ssl, EventLoop &loop, AsyncSocket socket);

        ~TlsSocket();

        TlsSocket(const TlsSocket &)            = delete;
        TlsSocket &operator=(const TlsSocket &) = delete;
        TlsSocket(TlsSocket &&other) noexcept;
        TlsSocket &operator=(TlsSocket &&other) noexcept;

        /**
         * @brief Perform TLS server-side handshake (SSL_accept).
         *        Handles non-blocking WANT_READ/WANT_WRITE via EpollAwaiter.
         */
        Task<> handshake();

        /**
         * @brief TLS-encrypted receive.
         */
        Task<ssize_t> asyncRecv(void *buf, size_t len) const;

        /**
         * @brief TLS-encrypted send.
         */
        Task<ssize_t> asyncSend(const void *buf, size_t len) const;

        void              close();
        [[nodiscard]] int fd() const noexcept;

    private:
        SSL *       m_ssl{nullptr};
        EventLoop * m_loop{nullptr};
        AsyncSocket m_socket;
        bool        m_handshakeDone{false};
    };

}

#endif
