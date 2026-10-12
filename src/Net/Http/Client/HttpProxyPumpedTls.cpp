#include "Net/Http/Client/HttpProxyPumpedTls.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Exception/CoreException.h"
#include "Core/Tls/TlsSocket.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <limits>

namespace AsynGyanis::Net
{
    void HttpProxyPumpedTls::freeInnerSsl(SSL *innerSsl) noexcept
    {
        if (innerSsl != nullptr)
        {
            static_cast<void>(SSL_shutdown(innerSsl));
            SSL_free(innerSsl);
        }
    }

    HttpProxyPumpedTls::HttpProxyPumpedTls(InnerSslHandle innerSsl, std::unique_ptr<Core::TlsSocket> outer) : m_inner(std::move(innerSsl)), m_outer(std::move(outer))
    {
        BIO *readBio  = BIO_new(BIO_s_mem());
        BIO *writeBio = BIO_new(BIO_s_mem());
        if (readBio == nullptr || writeBio == nullptr)
        {
            BIO_free(readBio);
            BIO_free(writeBio);
            return;
        }
        // 所有权随这两个调用移交 SSL（SSL_free 时一并归还）；此后读写都走内存 BIO，
        // 不再绑文件描述符——密文的实际搬运由本类的泵完成
        SSL_set0_rbio(m_inner.get(), readBio);
        SSL_set0_wbio(m_inner.get(), writeBio);
        m_isReady = true;
    }

    HttpProxyPumpedTls::~HttpProxyPumpedTls()
    {
        close();
    }

    void HttpProxyPumpedTls::requireAlive(const std::string_view operationName) const
    {
        if (m_isClosed || m_inner == nullptr)
        {
            throw Core::CoreException("内层 TLS " + std::string(operationName) + "失败：会话已在本端收口");
        }
    }

    std::string HttpProxyPumpedTls::describeSslFailure(const int sslErrorCode)
    {
        if (sslErrorCode == SSL_ERROR_SYSCALL)
        {
            return "对端在记录中途断开（TCP 被截断）";
        }
        return "内层 TLS 层报出错误（SSL_get_error=" + std::to_string(sslErrorCode) + "）";
    }

    void HttpProxyPumpedTls::drainInnerOutput()
    {
        // SSL 的输出记录全从写 BIO 取走：这一步在无挂起点的同步段里完成，
        // 因此读写两个方向谁先进来都不会看到半批字节
        for (;;)
        {
            char      chunk[4096];
            const int pending = static_cast<int>(BIO_ctrl_pending(SSL_get_wbio(m_inner.get())));
            if (pending <= 0)
            {
                break;
            }
            const int chunkBytes = pending < static_cast<int>(sizeof(chunk)) ? pending : static_cast<int>(sizeof(chunk));
            const int readBytes  = BIO_read(SSL_get_wbio(m_inner.get()), chunk, chunkBytes);
            if (readBytes <= 0)
            {
                break;
            }
            m_pendingWire.append(chunk, static_cast<std::size_t>(readBytes));
        }
    }

    Core::Task<bool> HttpProxyPumpedTls::flushPendingWire()
    {
        // 没抢到标志的一方直接返回：字节留在队列里，由正在发的一方循环到空为止
        if (m_isFlushing || m_pendingWire.empty())
        {
            co_return true;
        }
        m_isFlushing = true;
        while (!m_pendingWire.empty())
        {
            std::string batch = std::move(m_pendingWire);
            m_pendingWire.clear();
            try
            {
                std::size_t sent = 0;
                while (sent < batch.size())
                {
                    const ssize_t writtenBytes = co_await m_outer->asyncSend(batch.data() + sent, batch.size() - sent);
                    if (writtenBytes <= 0)
                    {
                        m_isFlushing = false;
                        co_return false;
                    }
                    sent += static_cast<std::size_t>(writtenBytes);
                }
            } catch (const std::exception &)
            {
                m_isFlushing = false;
                co_return false;
            }
        }
        m_isFlushing = false;
        co_return true;
    }

    Core::Task<bool> HttpProxyPumpedTls::pumpIncomingOnce()
    {
        char buffer[8192];
        try
        {
            const ssize_t receivedBytes = co_await m_outer->asyncReceive(buffer, sizeof(buffer));
            if (receivedBytes <= 0)
            {
                co_return false;
            }
            co_return BIO_write(SSL_get_rbio(m_inner.get()), buffer, static_cast<int>(receivedBytes)) > 0;
        } catch (const std::exception &)
        {
            co_return false;
        }
    }

    Core::Task<> HttpProxyPumpedTls::handshake()
    {
        requireAlive("握手");
        for (;;)
        {
            ERR_clear_error();
            const int result = SSL_do_handshake(m_inner.get());
            drainInnerOutput();
            if (result == 1)
            {
                co_return;
            }
            const int error = SSL_get_error(m_inner.get(), result);
            if (error == SSL_ERROR_WANT_READ)
            {
                // 先把本端已产出的记录送出去（握手期两端互相等待是常态），再收一批
                if (!co_await flushPendingWire())
                {
                    throw Core::CoreException("内层 TLS 握手失败：外层隧道已中断");
                }
                if (!co_await pumpIncomingOnce())
                {
                    throw Core::CoreException("内层 TLS 握手失败：外层隧道对端已收口");
                }
                continue;
            }
            if (error == SSL_ERROR_WANT_WRITE)
            {
                if (!co_await flushPendingWire())
                {
                    throw Core::CoreException("内层 TLS 握手失败：外层隧道已中断");
                }
                continue;
            }
            throw Core::CoreException("内层 TLS 握手失败：" + describeSslFailure(error));
        }
    }

    Core::Task<ssize_t> HttpProxyPumpedTls::asyncReceive(void *const buffer, const size_t length)
    {
        requireAlive("读取");
        if (length > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            throw Base::InvalidArgumentException("内层 TLS 读取失败：单次长度超过 INT_MAX，请分批读取");
        }
        if (length == 0)
        {
            co_return 0;
        }
        for (;;)
        {
            const int result = SSL_read(m_inner.get(), buffer, static_cast<int>(length));
            drainInnerOutput();
            if (result > 0)
            {
                if (!co_await flushPendingWire())
                {
                    throw Core::CoreException("内层 TLS 读取失败：外层隧道已中断");
                }
                co_return result;
            }
            const int error = SSL_get_error(m_inner.get(), result);
            if (error == SSL_ERROR_ZERO_RETURN)
            {
                co_return 0;
            }
            if (error == SSL_ERROR_WANT_READ)
            {
                if (!co_await flushPendingWire())
                {
                    throw Core::CoreException("内层 TLS 读取失败：外层隧道已中断");
                }
                if (!co_await pumpIncomingOnce())
                {
                    // 外层收口而内层没给 close_notify：与 TlsSocket 对「TCP 被截断」的归类一致，按失败处置
                    throw Core::CoreException("内层 TLS 读取失败：外层隧道对端在记录中途收口");
                }
                continue;
            }
            if (error == SSL_ERROR_WANT_WRITE)
            {
                if (!co_await flushPendingWire())
                {
                    throw Core::CoreException("内层 TLS 读取失败：外层隧道已中断");
                }
                continue;
            }
            throw Core::CoreException("内层 TLS 读取失败：" + describeSslFailure(error));
        }
    }

    Core::Task<ssize_t> HttpProxyPumpedTls::asyncSend(const void *const buffer, const size_t length)
    {
        requireAlive("写入");
        if (length > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            throw Base::InvalidArgumentException("内层 TLS 写入失败：单次长度超过 INT_MAX，请分批写入");
        }
        if (length == 0)
        {
            co_return 0;
        }
        for (;;)
        {
            // WANT 之后必须用相同参数重试（OpenSSL 对 SSL_write 的硬要求），循环即满足
            const int result = SSL_write(m_inner.get(), buffer, static_cast<int>(length));
            drainInnerOutput();
            if (result > 0)
            {
                if (!co_await flushPendingWire())
                {
                    throw Core::CoreException("内层 TLS 写入失败：外层隧道已中断");
                }
                co_return result;
            }
            const int error = SSL_get_error(m_inner.get(), result);
            if (error == SSL_ERROR_WANT_WRITE)
            {
                if (!co_await flushPendingWire())
                {
                    throw Core::CoreException("内层 TLS 写入失败：外层隧道已中断");
                }
                continue;
            }
            if (error == SSL_ERROR_WANT_READ)
            {
                // TLS 1.3 的 KeyUpdate 一类会让写需要先读：把队列送出去再收一批
                if (!co_await flushPendingWire())
                {
                    throw Core::CoreException("内层 TLS 写入失败：外层隧道已中断");
                }
                if (!co_await pumpIncomingOnce())
                {
                    throw Core::CoreException("内层 TLS 写入失败：外层隧道对端已收口");
                }
                continue;
            }
            throw Core::CoreException("内层 TLS 写入失败：" + describeSslFailure(error));
        }
    }

    void HttpProxyPumpedTls::close() noexcept
    {
        if (m_isClosed)
        {
            return;
        }
        m_isClosed = true;
        // 先放手内层 SSL（deleter 尽力发一个 close_notify），再关外层传输；顺序反过来也能收口，
        // 但先内后外让「内层收尾失败不牵动外层」这一层语义更直白
        m_inner.reset();
        if (m_outer != nullptr)
        {
            m_outer->close();
        }
    }

    int HttpProxyPumpedTls::fileDescriptor() const noexcept
    {
        return m_outer != nullptr ? m_outer->fileDescriptor() : -1;
    }

    std::string HttpProxyPumpedTls::selectedAlpnProtocol() const
    {
        if (m_inner == nullptr)
        {
            return {};
        }
        const unsigned char *negotiated      = nullptr;
        unsigned int         negotiatedBytes = 0;
        SSL_get0_alpn_selected(m_inner.get(), &negotiated, &negotiatedBytes);
        return negotiated != nullptr ? std::string(reinterpret_cast<const char *>(negotiated), negotiatedBytes) : std::string{};
    }

} // namespace AsynGyanis::Net
