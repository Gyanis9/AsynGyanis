/**
 * @file TlsSocket.h
 * @brief TLS socket 包装器 — SSL_read/SSL_write 与非阻塞 epoll 集成
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_TLSSOCKET_H
#define CORE_TLSSOCKET_H

#include "AsyncSocket.h"
#include "Task.h"

#include <openssl/ssl.h>

#include <memory>

namespace Core
{
    class EventLoop;

    /**
     * @brief TLS socket 包装类，提供异步 SSL 握手、加密读写接口。
     *
     * 内部持有 SSL 对象和底层 AsyncSocket，通过 EpollAwaiter 处理非阻塞读/写事件。
     * 使用前必须调用 handshake() 完成 TLS 握手，之后方可使用 asyncRecv/asyncSend。
     */
    class TlsSocket
    {
    public:
        /**
         * @brief 构造 TlsSocket 对象。
         * @param ssl   已关联 socket fd 的 SSL 对象（由 TlsContext::createSSL 获得），所有权转移
         * @param loop  所属事件循环
         * @param socket 已建立的异步 socket（非阻塞，已连接）
         */
        TlsSocket(SSL *ssl, EventLoop &loop, AsyncSocket socket);

        /**
         * @brief 析构函数，释放 SSL 对象并关闭底层 socket。
         */
        ~TlsSocket();

        TlsSocket(const TlsSocket &)            = delete;
        TlsSocket &operator=(const TlsSocket &) = delete;

        /**
         * @brief 移动构造函数。
         * @param other 要移动的 TlsSocket 对象
         */
        TlsSocket(TlsSocket &&other) noexcept;

        /**
         * @brief 移动赋值运算符。
         * @param other 要移动的 TlsSocket 对象
         * @return 当前对象的引用
         */
        TlsSocket &operator=(TlsSocket &&other) noexcept;

        /**
         * @brief 执行 TLS 服务端握手（SSL_accept）。
         *
         * 处理非阻塞状态下的 SSL_ERROR_WANT_READ / WANT_WRITE，通过 EpollAwaiter 等待 socket
         * 可读/可写事件，直到握手完成或出错。该函数是一个协程任务，应使用 co_await 等待。
         *
         * @return Task<> 协程，握手完成后返回，若失败则抛出异常
         */
        Task<> handshake();

        /**
         * @brief TLS 加密接收数据。
         * @param buf 接收缓冲区
         * @param len 缓冲区长度
         * @return Task<ssize_t> 协程，恢复时返回实际读取的字节数（0 表示连接关闭，负数表示错误）
         */
        Task<ssize_t> asyncRecv(void *buf, size_t len) const;

        /**
         * @brief TLS 加密发送数据。
         * @param buf 发送缓冲区
         * @param len 缓冲区长度
         * @return Task<ssize_t> 协程，恢复时返回实际发送的字节数（负数表示错误）
         */
        Task<ssize_t> asyncSend(const void *buf, size_t len) const;

        /**
         * @brief 关闭连接，释放 SSL 对象并关闭底层 socket。
         */
        void close();

        /**
         * @brief 获取底层 socket 的文件描述符。
         * @return 文件描述符值
         */
        [[nodiscard]] int fd() const noexcept;

    private:
        struct SslDeleter
        {
            void operator()(SSL *ssl) const noexcept
            {
                if (ssl)
                {
                    SSL_shutdown(ssl);
                    SSL_free(ssl);
                }
            }
        };

        std::unique_ptr<SSL, SslDeleter> m_ssl;          ///< OpenSSL SSL 对象，RAII 管理
        EventLoop * m_loop{nullptr};        ///< 关联的事件循环，用于等待 socket 事件
        AsyncSocket m_socket;               ///< 底层异步 socket
        bool        m_handshakeDone{false}; ///< 握手是否已完成
    };

}

#endif
