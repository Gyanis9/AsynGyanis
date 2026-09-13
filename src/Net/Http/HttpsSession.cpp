#include "Net/Http/HttpsSession.h"

#include "Base/Log/LogMacros.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/HttpSession.h"

#include <exception>
#include <stop_token>
#include <string>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 无效描述符的取值
         * @details 与 Core::AsyncSocket::close() 之后的 fileDescriptor() 保持一致：
         *          该实现关闭后就把内部描述符置成 -1，所以「-1」等价于「通道已关」。
         */
        constexpr int kInvalidSocketDescriptor = -1;
    } // namespace

    HttpsSession::HttpsSession(Core::EventLoop &loop, Core::TlsSocket tlsSocket, Router &router,
                               std::shared_ptr<const HttpServerLimits> limits,
                               std::shared_ptr<HttpMetricsCollector> metrics,
                               std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator) :
        // 基类只能拿到一条不持有描述符的占位套接字：真实描述符的所有权必须独一份，
        // 归 TlsSocket 管（它负责先 SSL_shutdown 再关描述符）。基类那份仅承担「存活位 + 取消源」
        Core::Connection(Core::AsyncSocket(loop, kInvalidSocketDescriptor)),
        m_tlsSocket(std::move(tlsSocket)),
        m_router(router),
        // 空配置按默认限额执行，与 HttpSession 保持同一套语义
        m_limits(limits != nullptr ? std::move(limits) : std::make_shared<const HttpServerLimits>()),
        // 统计对象与生成器允许为空：两种空值都表示「本会话不采集」，是明确的关闭语义，不补默认实例
        m_metrics(std::move(metrics)),
        m_requestIdGenerator(std::move(requestIdGenerator))
    {
    }

    void HttpsSession::closeTlsTransport()
    {
        // TlsSocket::close() 自身幂等：SSL 对象已释放时只是空操作，描述符也已置为无效值
        m_tlsSocket.close();
    }

    bool HttpsSession::isTlsTransportOpen() const noexcept
    {
        // 只看描述符而不看 SSL 状态：TlsSocket 没有暴露握手/关闭状态的查询接口，
        // 而它的 close() 一定会把底层描述符置成 -1，这个判据足够存活判定使用
        return m_tlsSocket.fileDescriptor() != kInvalidSocketDescriptor;
    }

    void HttpsSession::close()
    {
        // 先收 TLS 通道再走基类：SSL_shutdown 需要底层描述符仍然有效
        closeTlsTransport();
        Core::Connection::close();
    }

    bool HttpsSession::isAlive() const noexcept
    {
        return Core::Connection::isAlive() && isTlsTransportOpen();
    }

    std::string HttpsSession::remoteAddress() const
    {
        // 基类那份套接字是个不持有描述符的占位对象，向它要地址只会得到「不是套接字」的错误；
        // 真实描述符归 TlsSocket 所有，地址只能从它那里取
        return m_tlsSocket.remoteAddress().toString();
    }

    std::string HttpsSession::localAddress() const
    {
        return m_tlsSocket.localAddress().toString();
    }

    Core::Task<> HttpsSession::start()
    {
        /**
         * @brief 退出时无条件收口的守卫
         *
         * @details 收尾调用本类重写后的 close()：它先收 TLS 通道再复位基类存活位与取消源，
         *          于是「自然结束」与「被强制关闭」走同一条清理路径。写成 RAII 是为了覆盖
         *          事务循环写侧抛异常的路径：只在正常返回与握手失败两处显式调用，异常穿过时
         *          连接会停留在存活状态。
         */
        struct TransportCloser
        {
            HttpsSession *session = nullptr; ///< 需要在退出时收口的会话

            ~TransportCloser()
            {
                if (session != nullptr)
                {
                    session->close();
                }
            }
        } closer{this};

        try
        {
            co_await m_tlsSocket.handshake();
        } catch (const std::exception &handshakeException)
        {
            // 握手失败没有可信的明文可回，也没有可读的请求：记日志后直接结束会话，
            // 收口交给上面的 RAII 守卫（它会走基类 close()，让存活位、停止请求、
            // TLS 通道三者一次收干净）
            LOG_ERROR_FMT("HttpsSession: TLS 握手失败，已关闭连接（描述符={}）。原因：{}", m_tlsSocket.fileDescriptor(), handshakeException.what());
            co_return;
        }

        LOG_DEBUG_FMT("HttpsSession: TLS 握手完成（描述符={}），进入 HTTP 事务循环", m_tlsSocket.fileDescriptor());

        // 谓词提成命名局部：它要以 const std::function 引用的形式活过整个 co_await，
        // 放在本协程帧里比依赖临时量的析构时点更好读
        const std::function<bool()> alivePredicate = [this]()
        {
            return isAlive() && isTlsTransportOpen();
        };

        // 与 HttpSession 共用同一份事务循环，此处只换了传输层对象与存活谓词；限额、统计采集端与
        // request-id 生成器与 HTTP 侧同样一并转交，因此调度与计数口径两侧完全一致。
        // 谓词额外要看描述符，保证「对端断开 → 读出错 → 通道被关」之后循环一定退出，
        // 而不是只依赖基类那个没人置位的存活标志
        co_await detail::httpKeepAliveLoop(
                m_tlsSocket, cancelable(), m_router, m_parser, m_receiveBuffer, alivePredicate, *this, *m_limits,
                m_metrics.get(), m_requestIdGenerator.get());

        LOG_DEBUG_FMT("HttpsSession: 事务循环结束，关闭 TLS 通道（描述符={}）", m_tlsSocket.fileDescriptor());

        // 不再在此处 close()：收口统一交给函数开头的 RAII 守卫
        co_return;
    }

    void HttpsSession::onIdleTimeoutClosed() noexcept
    {
        // 只累加计数：清扫协程已经把「哪条连接、误差多大」写进了日志，这里再写一条只会让它翻倍。
        // 未持有统计对象时什么都不做——那表示调用方只关心协议本身
        if (m_metrics != nullptr)
        {
            m_metrics->countTimeoutClosedConnection();
        }
    }

} // namespace AsynGyanis::Net
