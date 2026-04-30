#include "TlsSocket.h"
#include "EpollAwaiter.h"
#include "EventLoop.h"
#include "Base/Exception.h"

#include <openssl/err.h>

namespace Core
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

        auto &    epoll = m_loop->epoll();
        const int fd    = m_socket.fd();

        while (true)
        {
            const int ret = SSL_accept(m_ssl.get());
            if (ret == 1)
            {
                m_handshakeDone = true;
                co_return;
            }

            const int err = SSL_get_error(m_ssl.get(), ret);
            if (err == SSL_ERROR_WANT_READ)
            {
                co_await EpollAwaiter(epoll, fd, EPOLLIN);
                continue;
            }

            if (err == SSL_ERROR_WANT_WRITE)
            {
                co_await EpollAwaiter(epoll, fd, EPOLLOUT);
                continue;
            }

            char buf[256];
            ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
            throw Base::Exception(std::string("TLS handshake failed: ") + buf);
        }
    }

    Task<ssize_t> TlsSocket::asyncRecv(void *const buf, const size_t len) const
    {
        auto &    epoll = m_loop->epoll();
        const int fd    = m_socket.fd();

        if (len == 0)
        {
            co_return 0;
        }

        while (true)
        {
            const int ret = SSL_read(m_ssl.get(), buf, static_cast<int>(len));
            if (ret > 0)
            {
                co_return static_cast<ssize_t>(ret);
            }

            const int err = SSL_get_error(m_ssl.get(), ret);
            if (err == SSL_ERROR_WANT_READ)
            {
                co_await EpollAwaiter(epoll, fd, EPOLLIN);
                continue;
            }

            if (err == SSL_ERROR_WANT_WRITE)
            {
                co_await EpollAwaiter(epoll, fd, EPOLLOUT);
                continue;
            }

            if (err == SSL_ERROR_ZERO_RETURN)
            {
                co_return 0;
            }

            char errorBuf[256];
            ERR_error_string_n(ERR_get_error(), errorBuf, sizeof(errorBuf));
            throw Base::Exception(std::string("SSL_read failed: ") + errorBuf);
        }
    }

    Task<ssize_t> TlsSocket::asyncSend(const void *const buf, const size_t len) const
    {
        auto &    epoll = m_loop->epoll();
        const int fd    = m_socket.fd();

        if (len == 0)
        {
            co_return 0;
        }

        while (true)
        {
            const int ret = SSL_write(m_ssl.get(), buf, static_cast<int>(len));
            if (ret > 0)
            {
                co_return static_cast<ssize_t>(ret);
            }

            const int err = SSL_get_error(m_ssl.get(), ret);
            if (err == SSL_ERROR_WANT_WRITE)
            {
                co_await EpollAwaiter(epoll, fd, EPOLLOUT);
                continue;
            }

            if (err == SSL_ERROR_WANT_READ)
            {
                co_await EpollAwaiter(epoll, fd, EPOLLIN);
                continue;
            }

            char errorBuf[256];
            ERR_error_string_n(ERR_get_error(), errorBuf, sizeof(errorBuf));
            throw Base::Exception(std::string("SSL_write failed: ") + errorBuf);
        }
    }

    void TlsSocket::close()
    {
        m_ssl.reset();
        m_socket.close();
    }

    int TlsSocket::fd() const noexcept
    {
        return m_socket.fd();
    }

}
