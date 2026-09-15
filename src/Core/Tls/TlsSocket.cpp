#include "Core/Tls/TlsSocket.h"
#include "Core/EventLoop/IoWatcher.h"
#include "Core/EventLoop/EventLoop.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Exception/CoreException.h"
#include "Core/Socket/InetAddress.h"

#include <limits>
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
                if (!co_await m_socket.waitReadable())
                {
                    throw CoreException("TLS 握手失败：等待可读期间套接字被关闭");
                }
                continue;
            }

            if (error == SSL_ERROR_WANT_WRITE)
            {
                if (!co_await m_socket.waitWritable())
                {
                    throw CoreException("TLS 握手失败：等待可写期间套接字被关闭");
                }
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
        if (length == 0)
        {
            co_return 0;
        }

        // 长度上限要先判：OpenSSL 的长度形参是 int，超限强转会得到可疑的负数，
        // 与同步套接字侧的显式拒绝保持同一口径
        if (length > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            throw Base::InvalidArgumentException("TLS 读取的长度超出单次调用上限（底层接口按 int 收长度）：请分批读取");
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
                if (!co_await m_socket.waitReadable())
                {
                    throw CoreException("TLS 读取失败：等待可读期间套接字被关闭");
                }
                continue;
            }

            if (error == SSL_ERROR_WANT_WRITE)
            {
                if (!co_await m_socket.waitWritable())
                {
                    throw CoreException("TLS 握手失败：等待可写期间套接字被关闭");
                }
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
        if (length == 0)
        {
            co_return 0;
        }

        // 长度上限同 asyncReceive()：底层按 int 收长度，超限强转会得到可疑的负数
        if (length > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            throw Base::InvalidArgumentException("TLS 写入的长度超出单次调用上限（底层接口按 int 收长度）：请分批写入");
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
                if (!co_await m_socket.waitWritable())
                {
                    throw CoreException("TLS 写入失败：等待可写期间套接字被关闭");
                }
                continue;
            }

            if (error == SSL_ERROR_WANT_READ)
            {
                if (!co_await m_socket.waitReadable())
                {
                    throw CoreException("TLS 读取失败：等待可读期间套接字被关闭");
                }
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

    std::string TlsSocket::selectedAlpnProtocol() const
    {
        // SSL 对象已经释放（close() 之后）：通道都没了，不存在协商结果
        if (!m_ssl)
        {
            return {};
        }

        const unsigned char *protocolName = nullptr;
        unsigned int protocolNameLength = 0;
        SSL_get0_alpn_selected(m_ssl.get(), &protocolName, &protocolNameLength);
        if (protocolName == nullptr || protocolNameLength == 0)
        {
            return {};
        }

        // 按「指针 + 长度」构造：协议名里可能出现的字节都由 RFC 7301 限定，但长度感知的写法与
        // 本仓库其它二进制安全接口保持一致，不依赖零终止
        return std::string(reinterpret_cast<const char *>(protocolName), protocolNameLength);
    }

    InetAddress TlsSocket::remoteAddress() const
    {
        // 地址只存在于底层套接字上：SSL 对象不保存地址，也不需要在关闭后提供它
        return m_socket.remoteAddress();
    }

    InetAddress TlsSocket::localAddress() const
    {
        return m_socket.localAddress();
    }

}
