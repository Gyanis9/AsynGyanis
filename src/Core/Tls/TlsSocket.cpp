#include "Core/Tls/TlsSocket.h"
#include "Core/EventLoop/EpollAwaiter.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Exception/CoreException.h"

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
            // OpenSSL 的错误串本身是英文，但它是定位问题的唯一线索，因此保留并补上中文说明与常见原因
            throw CoreException(std::string("TLS 握手失败：") + buffer +
                                "（常见原因：对端证书不受信、协议版本不匹配、对端不是 TLS 服务，"
                                "或对端在握手期间关闭了连接）");
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
            throw CoreException(std::string("TLS 读取失败：") + errorBuffer +
                                "（连接多半已被对端关闭或 TLS 会话已失效，应关闭该连接而不是重试）");
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
            throw CoreException(std::string("TLS 写入失败：") + errorBuffer +
                                "（连接多半已被对端关闭或 TLS 会话已失效，应关闭该连接而不是重试）");
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
