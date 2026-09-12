/**
 * @file HttpsSession.cpp
 * @brief HTTPS 会话实现：TLS 握手、强制关闭链路与共享事务循环的接入
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Http/HttpsSession.h"

#include "Base/Log/LogMacros.h"
#include "Net/Http/HttpSession.h"

#include <exception>
#include <stop_token>
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

        /**
         * @brief 停止回调实体：把 Core::Connection::close() 落到真实的 TLS 通道上
         * @details 基类的 close() 不是虚函数，派生类无法拦截，所以改用「取消源 + 停止回调」接线：
         *          Connection::close() 内部先 requestStop()，本次调用就会同步执行到这里，
         *          关掉描述符之后，阻塞在 epoll 上的 TLS 读会被唤醒并报错，事务循环随即退出。
         *          没有这一步，强制关闭就只是把一个占位套接字关掉，TLS 会话根本停不下来。
         * @note stop_callback 只接受 (stop_token, callback) 两参，因此上下文封装在实体里，
         *       而不是走早期技术规范那套「函数指针 + void*」。
         */
        struct TlsTransportCloser
        {
            HttpsSession *session = nullptr; ///< 注册回调时所对应的会话，其存活期由该会话的协程帧保证

            /**
             * @brief 关闭 TLS 传输通道
             */
            void operator()() const
            {
                // 回调随注册点的栈帧注销，这里判空只防误用
                if (session != nullptr)
                {
                    session->closeTlsTransport();
                }
            }
        };
    } // namespace

    HttpsSession::HttpsSession(Core::EventLoop &loop, Core::TlsSocket tlsSocket, Router &router) :
        // 基类只能拿到一条不持有描述符的占位套接字：真实描述符的所有权必须独一份，
        // 归 TlsSocket 管（它负责先 SSL_shutdown 再关描述符）。基类那份仅承担「存活位 + 取消源」
        Core::Connection(Core::AsyncSocket(loop, kInvalidSocketDescriptor)),
        m_tlsSocket(std::move(tlsSocket)),
        m_router(router),
        m_receiveBuffer(detail::kInitialReceiveBufferLength)
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

    Core::Task<> HttpsSession::start()
    {
        // 停止回调先于握手注册：握手期间被强制关闭也要能掐断通道，否则这条协程会一直挂在
        // epoll 上等一个再也不会来的握手事件。回调随本协程帧的结束而注销
        std::stop_callback<TlsTransportCloser> stopCallback(cancelable().stopToken(), TlsTransportCloser{this});

        try
        {
            co_await m_tlsSocket.handshake();
        } catch (const std::exception &handshakeException)
        {
            // 握手失败没有可信的明文可回，也没有可读的请求：关掉通道直接结束会话。
            // 走基类 close() 是为了让「存活位、停止请求、TLS 通道」三者一次收干净
            LOG_ERROR_FMT("HttpsSession: TLS 握手失败，已关闭连接（描述符={}）。原因：{}", m_tlsSocket.fileDescriptor(), handshakeException.what());
            close();
            co_return;
        }

        LOG_DEBUG_FMT("HttpsSession: TLS 握手完成（描述符={}），进入 HTTP 事务循环", m_tlsSocket.fileDescriptor());

        // 谓词提成命名局部：它要以 const std::function 引用的形式活过整个 co_await，
        // 放在本协程帧里比依赖临时量的析构时点更好读
        const std::function<bool()> alivePredicate = [this]()
        {
            return isAlive() && isTlsTransportOpen();
        };

        // 与 HttpSession 共用同一份事务循环，此处只换了传输层对象与存活谓词：
        // 谓词额外要看描述符，保证「对端断开 → 读出错 → 通道被关」之后循环一定退出，
        // 而不是只依赖基类那个没人置位的存活标志
        co_await detail::httpKeepAliveLoop(
                m_tlsSocket, cancelable(), m_router, m_parser, m_receiveBuffer, alivePredicate);

        LOG_DEBUG_FMT("HttpsSession: 事务循环结束，关闭 TLS 通道（描述符={}）", m_tlsSocket.fileDescriptor());

        // 收尾只调基类 close()：它会 requestStop() 并由上面的停止回调去关 TLS 通道，
        // 于是自然结束与被强制关闭走的是同一条清理路径
        close();
        co_return;
    }

} // namespace AsynGyanis::Net
