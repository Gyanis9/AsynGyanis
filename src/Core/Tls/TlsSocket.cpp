#include "Core/Tls/TlsSocket.h"
#include "Core/EventLoop/IoWatcher.h"
#include "Core/EventLoop/EventLoop.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Exception/CoreException.h"
#include "Core/Socket/InetAddress.h"
#include "Platform/System/PlatformError.h"

#include <limits>
#include <openssl/err.h>
#include <string>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 把一次失败的 SSL 调用翻成「原因 + 下一步」，握手/读取/写入三条入口共用
         * @param sslErrorCode SSL_get_error() 的结果，调用方已排除 WANT_READ/WANT_WRITE/ZERO_RETURN
         * @param callReturn 那次 SSL_connect/SSL_accept/SSL_read/SSL_write 的返回值
         * @return std::string 冒号之后的原因句，不含「TLS 读取失败：」这类前缀
         * @details 原因要从两条通道读：OpenSSL 的错误队列与平台的套接字错误码。只看队列时
         *          SSL_ERROR_SYSCALL 一类会打印出没有任何内容的 `error:00000000`，再配一句
         *          指向对端配置的猜测，排查的人会顺着去找证书而不是查断线。
         */
        std::string describeSslFailure(const int sslErrorCode, const int callReturn)
        {
            const unsigned long queueEntry = ERR_get_error();

            // 握手期被对端掐断时 OpenSSL 会留下这条 reason，它和「队列为空的 EOF」是同一件事
            const bool isAbruptTcpBreak = sslErrorCode == SSL_ERROR_SYSCALL ||
                                          (queueEntry != 0 &&
                                           ERR_GET_REASON(queueEntry) == SSL_R_UNEXPECTED_EOF_WHILE_READING);
            if (isAbruptTcpBreak)
            {
                // 返回 0 是读到文件尾（对端发了 FIN），返回 -1 是底层调用自己报错（重置一类）。
                // 两路的处置相同，但把看到的是哪一种写出来，读者不必再去猜
                if (callReturn == 0)
                {
                    return "对端没有发出 TLS 关闭通知（close_notify）就断开了 TCP 连接，本端读到的是文件尾："
                           "这条会话已经失效，关闭本端连接，需要时重新握手";
                }
                return "对端没有发出 TLS 关闭通知就断了连接，底层套接字报「" +
                       Platform::PlatformError::message(Platform::PlatformError::lastSocketErrorCode()) +
                       "」：TLS 层没有收到任何告警，关闭本端连接，需要时重新握手";
            }

            if (sslErrorCode == SSL_ERROR_ZERO_RETURN)
            {
                // 只在写侧落到这里：读侧把干净结束当作 0 返回，不会走到失败文案
                return "本端已经收到对端的 TLS 关闭通知（close_notify），这条会话进入关闭状态，不能再收发";
            }

            if (queueEntry != 0)
            {
                char queueText[256]{};
                ERR_error_string_n(queueEntry, queueText, sizeof(queueText));
                return std::string(queueText) +
                       "（TLS 协议层报错，常见原因：对端证书不受信、协议版本或加密套件不匹配、对端不是 TLS 服务）";
            }

            return "TLS 层报出未知错误（SSL_get_error=" + std::to_string(sslErrorCode) +
                   "，OpenSSL 错误队列为空）：按会话已失效处理，关闭本端连接";
        }
    } // namespace

    TlsSocket::TlsSocket(SSL *ssl, EventLoop &loop, AsyncSocket socket, const Role role) :
        m_ssl(ssl), m_loop(&loop), m_socket(std::move(socket)), m_role(role)
    {
    }

    TlsSocket::~TlsSocket()
    {
        // 必须显式走 close() 而不是交给成员逆序析构：m_ssl 声明在 m_socket 之前，逆序就是
        // 「先关描述符、后 SSL_shutdown」，那次 shutdown 的写入落在已关闭（且编号可被别的
        // 线程立刻复用）的描述符上。后果有两重：对端收不到 close_notify，它的 asyncReceive
        // 报「协议错误」而不是干净的结束符；更糟的是那几个字节的 TLS 告警记录会灌进复用
        // 同一编号的那个陌生连接
        close();
    }

    TlsSocket::TlsSocket(TlsSocket &&other) noexcept :
        m_ssl(std::move(other.m_ssl)),
        m_loop(other.m_loop),
        m_socket(std::move(other.m_socket)),
        m_role(other.m_role),
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
            m_role          = other.m_role;
            m_handshakeDone = other.m_handshakeDone;
        }
        return *this;
    }

    void TlsSocket::requireLiveContext(const std::string_view operationName) const
    {
        // OpenSSL 3 拿到空 SSL 指针不会崩，而是返回失败并留下一条 `error:00000000:lib(0)::reason(0)`
        // 之类的空错误；三条路径的兜底文案又把原因写成「对端关闭/会话失效」，与本端被关停这个真实
        // 起因无关，排查的人会顺着去找对端。抛运行期故障而非用法错误：TcpServer 的空闲清扫与优雅
        // 收口本就会在协程还挂着的时候关停本端，那是正常时序，不是调用方写错了
        if (m_ssl == nullptr)
        {
            throw CoreException(std::string(operationName) +
                                    "失败：本端 TLS 会话已释放（close() 之后不能再收发），"
                                    "请先让在途的收发协程结束、再关闭连接");
        }
    }

    Task<> TlsSocket::handshake()
    {
        if (m_handshakeDone)
        {
            co_return;
        }

        while (true)
        {
            // 两个等位都会让出调度，恢复时本端可能已被 close()：闸门在循环开头，不走「每条分支各判一次」
            requireLiveContext("TLS 握手");

            // 角色决定握手入口：服务端 SSL_accept、客户端 SSL_connect。用错的那个会让两端
            // 各停在初始状态等对方先说话——客户端用 SSL_accept 时握手永远完不成
            const int ret = m_role == Role::Client ? ::SSL_connect(m_ssl.get()) : ::SSL_accept(m_ssl.get());
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

            // 失败原因由共用的翻译给出来：这里不再无条件追加「证书不受信」那类猜测——
            // 对端在握手中途断线时那些猜测是错方向的
            throw CoreException("TLS 握手失败：" + describeSslFailure(error, ret));
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
            // 本端可能在任何一个让出点上被 close()（等可读、等可写、为写侧让出一次调度），
            // 恢复后先过闸门再交给 SSL_read
            requireLiveContext("TLS 读取");

            // 写侧还有没重试完的记录时不能插一次 SSL_read：那会由读侧替写侧把待发记录冲出去，
            // 写侧随后又按同一份数据重试，同一段明文在线上有两份（OpenSSL 明令禁止的交错）
            if (m_isWritePending)
            {
                if (!co_await yieldForPeerProgress())
                {
                    throw CoreException("TLS 读取失败：等写侧收掉待发记录时定时器不可用（描述符耗尽？）");
                }
                continue;
            }

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
                // 握手期的读会需要先写：写方向若已被写协程占着，就不能去抢等待槽（一个方向只允许
                // 一个等待者，抢槽会直接抛 LogicException，把一次读失败变成一条莫名异常）。
                // 改为让出一次调度，由写侧先把需要的字节发出去，再回来重试
                if (m_socket.isWaitingWritable())
                {
                    if (!co_await yieldForPeerProgress())
                    {
                        throw CoreException("TLS 读取失败：等写侧推进时定时器不可用（描述符耗尽？）");
                    }
                    // 让出期间本端可能被关停，由循环开头的闸门统一复查
                    continue;
                }
                if (!co_await m_socket.waitWritable())
                {
                    throw CoreException("TLS 读取失败：等待可写期间套接字被关闭");
                }
                continue;
            }

            if (error == SSL_ERROR_ZERO_RETURN)
            {
                co_return 0;
            }

            throw CoreException("TLS 读取失败：" + describeSslFailure(error, ret));
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

        // 出口统一落旗：牌子留在异常路径上会把读侧永久锁在让轮里，而那时连接已经不能用了。
        // 成功返回也走这里——写侧把待发记录收掉之后，读侧就不再受这条规矩约束
        struct WritePendingGuard
        {
            explicit WritePendingGuard(bool *const flag) noexcept : m_flag(flag) {}
            ~WritePendingGuard() noexcept { *m_flag = false; }

            WritePendingGuard(const WritePendingGuard &) = delete;
            WritePendingGuard &operator=(const WritePendingGuard &) = delete;

            bool *const m_flag; ///< 要落下的一面旗（指向所属 TlsSocket 的 m_isWritePending）
        } writePendingGuard{&m_isWritePending};

        while (true)
        {
            // 本端可能在任何一个让出点上被 close()（等可写、等可读、为反方向让出一次调度），
            // 恢复后先过闸门再交给 SSL_write
            requireLiveContext("TLS 写入");

            const int ret = SSL_write(m_ssl.get(), buffer, static_cast<int>(length));
            if (ret > 0)
            {
                // 记录已被 SSL 收下，读侧可以插进来（读侧的让轮就到这里为止）
                m_isWritePending = false;
                co_return static_cast<ssize_t>(ret);
            }

            const int error = SSL_get_error(m_ssl.get(), ret);
            if (error == SSL_ERROR_WANT_WRITE)
            {
                // 从这一刻起这条记录在 SSL 内部等着被重试：见 m_isWritePending 的说明
                m_isWritePending = true;
                // 与读侧对称：写方向若已被写协程占着（一条 TlsSocket 上读写各由一个协程驱动），
                // 不能去抢等待槽——抢槽会直接抛 LogicException，把一次可自愈的等待变成硬故障。
                // 让出一次调度，由占槽的一方先把字节发出去，再回来重试
                if (m_socket.isWaitingWritable())
                {
                    if (!co_await yieldForPeerProgress())
                    {
                        throw CoreException("TLS 写入失败：等写侧推进时定时器不可用（描述符耗尽？）");
                    }
                    // 让出期间本端可能被关停，由循环开头的闸门统一复查
                    continue;
                }
                if (!co_await m_socket.waitWritable())
                {
                    throw CoreException("TLS 写入失败：等待可写期间套接字被关闭");
                }
                continue;
            }

            if (error == SSL_ERROR_WANT_READ)
            {
                // 这条等待要求读侧真的能读：先把「写侧待发」的牌子落下，否则读侧会一直让轮下去
                m_isWritePending = false;
                // TLS 1.3 的 KeyUpdate（以及握手期）会让 SSL_write 需要先读：读方向若已有读协程
                // 在等，同样不能抢槽——让出一次调度，由读侧把那批握手字节吃进来再重试
                if (m_socket.isWaitingReadable())
                {
                    if (!co_await yieldForPeerProgress())
                    {
                        throw CoreException("TLS 写入失败：等读侧推进时定时器不可用（描述符耗尽？）");
                    }
                    continue;
                }
                if (!co_await m_socket.waitReadable())
                {
                    // 前缀按「哪条操作失败」算：这里等的是可读事件，但发起方是写入
                    throw CoreException("TLS 写入失败：等待可读期间套接字被关闭");
                }
                continue;
            }

            // 写侧的失败与读侧同一套口径：对端断了 TCP 与协议层报错要分开说，
            // 否则留下的还是一条没有内容的队列原文加一句猜测
            throw CoreException("TLS 写入失败：" + describeSslFailure(error, ret));
        }
    }

    Task<bool> TlsSocket::yieldForPeerProgress() const
    {
        // 让出一次调度：本方向需要反方向先推进，而反方向的等待槽已被占用
        if (m_loop == nullptr)
        {
            co_return false;
        }

        Timer timer(*m_loop);
        try
        {
            // 定时器申请不到描述符时会抛：按失败收场，由调用方给出可操作的错误文本
            static_cast<void>(co_await timer.waitFor(kPeerProgressYieldInterval));
        } catch (...)
        {
            co_return false;
        }
        co_return true;
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
