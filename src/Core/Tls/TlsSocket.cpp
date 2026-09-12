#include "Core/Tls/TlsSocket.h"
#include "Core/EventLoop/EpollAwaiter.h"
#include "Core/EventLoop/EventLoop.h"
#include "Base/Exception/SystemException.h"

#include <openssl/err.h>

namespace AsynGyanis::Core
{
    TlsSocket::TlsSocket(SSL *ssl, EventLoop &loop, AsyncSocket socket) :
        m_ssl(ssl), m_loop(&loop), m_socket(std::move(socket))
    {
    }

    TlsSocket::~TlsSocket() = default;

    TlsSocket::TlsSocket(TlsSocket &&other) noexcept :
        m_ssl(std::move(other.m_ssl)),
        m_loop(other.m_loop),
        m_socket(std::move(other.m_socket)),
        m_handshakeDone(other.m_handshakeDone)
    {
    }

    TlsSocket &TlsSocket::operator=(TlsSocket &&other) noexcept
    {
        if (this != &other)
        {
            m_loop          = other.m_loop;
            m_ssl           = std::move(other.m_ssl);
            m_socket        = std::move(other.m_socket);
            m_handshakeDone = other.m_handshakeDone;
        }
        return *this;
    }

    Task<> TlsSocket::handshake()
    {
        if (m_handshakeDone)
        {
            co_return;
        }

        auto &    epoll          = m_loop->epoll();
        const int fileDescriptor = m_socket.fileDescriptor();

        while (true)
        {
            const int ret = SSL_accept(m_ssl.get());
            if (ret == 1)
            {
                m_handshakeDone = true;
                co_return;
            }

            const int error = SSL_get_error(m_ssl.get(), ret);
            if (error == SSL_ERROR_WANT_READ)
            {
                co_await EpollAwaiter(epoll, fileDescriptor, EPOLLIN);
                continue;
            }

            if (error == SSL_ERROR_WANT_WRITE)
            {
                co_await EpollAwaiter(epoll, fileDescriptor, EPOLLOUT);
                continue;
            }

            char buffer[256];
            ERR_error_string_n(ERR_get_error(), buffer, sizeof(buffer));
            throw Base::Exception(std::string("TLS handshake failed: ") + buffer);
        }
    }

    Task<ssize_t> TlsSocket::asyncReceive(void *const buffer, const size_t length) const
    {
        auto &    epoll          = m_loop->epoll();
        const int fileDescriptor = m_socket.fileDescriptor();

        if (length == 0)
        {
            co_return 0;
        }

        while (true)
        {
            const int ret = SSL_read(m_ssl.get(), buffer, static_cast<int>(length));
            if (ret > 0)
            {
                co_return static_cast<ssize_t>(ret);
            }

            const int error = SSL_get_error(m_ssl.get(), ret);
            if (error == SSL_ERROR_WANT_READ)
            {
                co_await EpollAwaiter(epoll, fileDescriptor, EPOLLIN);
                continue;
            }

            if (error == SSL_ERROR_WANT_WRITE)
            {
                co_await EpollAwaiter(epoll, fileDescriptor, EPOLLOUT);
                continue;
            }

            if (error == SSL_ERROR_ZERO_RETURN)
            {
                co_return 0;
            }

            char errorBuffer[256];
            ERR_error_string_n(ERR_get_error(), errorBuffer, sizeof(errorBuffer));
            throw Base::Exception(std::string("SSL_read failed: ") + errorBuffer);
        }
    }

    Task<ssize_t> TlsSocket::asyncSend(const void *const buffer, const size_t length) const
    {
        auto &    epoll          = m_loop->epoll();
        const int fileDescriptor = m_socket.fileDescriptor();

        if (length == 0)
        {
            co_return 0;
        }

        while (true)
        {
            const int ret = SSL_write(m_ssl.get(), buffer, static_cast<int>(length));
            if (ret > 0)
            {
                co_return static_cast<ssize_t>(ret);
            }

            const int error = SSL_get_error(m_ssl.get(), ret);
            if (error == SSL_ERROR_WANT_WRITE)
            {
                co_await EpollAwaiter(epoll, fileDescriptor, EPOLLOUT);
                continue;
            }

            if (error == SSL_ERROR_WANT_READ)
            {
                co_await EpollAwaiter(epoll, fileDescriptor, EPOLLIN);
                continue;
            }

            char errorBuffer[256];
            ERR_error_string_n(ERR_get_error(), errorBuffer, sizeof(errorBuffer));
            throw Base::Exception(std::string("SSL_write failed: ") + errorBuffer);
        }
    }

    void TlsSocket::close()
    {
        m_ssl.reset();
        m_socket.close();
    }

    int TlsSocket::fileDescriptor() const noexcept
    {
        return m_socket.fileDescriptor();
    }

}
