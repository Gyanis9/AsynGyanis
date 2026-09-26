/**
 * @file TlsSocket.h
 * @brief TLS socket 包装器 — SSL_read/SSL_write 与非阻塞 epoll 集成
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "Core/EventLoop/Timer.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Coroutine/Task.h"

#include <openssl/ssl.h>

#include <memory>
#include <string>
#include <string_view>

namespace AsynGyanis::Core
{
    class EventLoop;

    /**
     * @brief TLS socket 包装类，提供异步 SSL 握手、加密读写接口
     * @note 必须先用 handshake() 完成握手，之后才能用 asyncReceive()/asyncSend()
     */
    class TlsSocket
    {
    public:
        /**
         * @brief 本端角色：决定握手走哪个 OpenSSL 入口
         * @details 用错入口两端会各停在初始状态等对方先说话，握手永远完不成——服务端只有
         *          `SSL_accept`，客户端只有 `SSL_connect`，二者不可互换
         */
        enum class Role
        {
            Server, ///< 服务端：握手走 SSL_accept（默认，与既有调用点一致）
            Client  ///< 客户端：握手走 SSL_connect
        };

        /**
         * @brief 构造 TlsSocket 对象。
         * @param ssl   已关联 socket 文件描述符的 SSL 对象（服务端由 TlsContext::createSSL 获得，
         *              客户端自行 SSL_new），所有权转移
         * @param loop  所属事件循环
         * @param socket 已建立的异步 socket（非阻塞，已连接）
         * @param role  本端角色，默认服务端
         */
        TlsSocket(SSL *ssl, EventLoop &loop, AsyncSocket socket, Role role = Role::Server);

        /**
         * @brief 析构函数，释放 SSL 对象并关闭底层 socket。
         */
        ~TlsSocket();

        TlsSocket(const TlsSocket &) = delete;

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
         * @brief 执行 TLS 握手（服务端走 SSL_accept，客户端走 SSL_connect）
         * @return Task<> 协程，握手完成后返回，失败则抛出异常
         * @throws CoreException 握手失败（对端证书不受信、协议版本不匹配、对端不是 TLS 服务，
         *         或对端在握手期间关闭连接）。调用方应关闭该连接而不是重试
         * @throws CoreException 本端会话已释放：close() 之后调用，或协程挂起期间被 close()
         */
        Task<> handshake();

        /**
         * @brief TLS 加密接收数据。
         * @param buffer 接收缓冲区
         * @param length 缓冲区长度，不得超过 INT_MAX（底层 SSL_read 按 int 收长度，超限会被静默窄化，
         *        因此当场拒绝，请分批读取）
         * @return Task<ssize_t> 协程，恢复时返回实际读取的字节数；0 表示连接已正常收口
         *         （对端发了 close_notify 或 TLS 层的 EOF）。**本方法不返回负数**，失败一律以异常结束
         * @warning 返回 0 有两种来源：连接正常收口，以及**本次 length 就是 0**（此时不做任何 I/O 直接
         *          返回 0）。不要把「缓冲区剩余空间」这类可能算出 0 的值当长度传进来——0 会被读成一次
         *          干净的收口。口径与 `AsyncSocket::asyncReceive` 一致（两者在会话层可互相替换）
         * @throws Base::InvalidArgumentException 长度超过 INT_MAX：取值非法属用法错误，走
         *         std::invalid_argument 分支（不在 CoreException 这条运行期故障链上）
         * @throws CoreException TLS 会话已失效（对端异常关闭等）。对端**正常**关闭会返回 0 而不是
         *         抛异常，因此调用方看到的异常一律意味着会话不可再用
         * @throws CoreException 本端会话已释放：close() 之后调用，或协程挂起期间被 close()
         */
        Task<ssize_t> asyncReceive(void *buffer, size_t length) const;

        /**
         * @brief TLS 加密发送数据。
         * @param buffer 发送缓冲区
         * @param length 缓冲区长度，不得超过 INT_MAX（底层 SSL_write 按 int 收长度，超限会被静默窄化，
         *        因此当场拒绝，请分批写入）
         * @return Task<ssize_t> 协程，恢复时返回实际写入的字节数（可能小于 length，调用方按已写长度续发）
         * @note **本方法不返回负数**，这一点与明文侧的 `AsyncSocket::asyncSend` 不同：那条以 -1 表示
         *       「对端已关闭」，而这里对端关闭走的是异常。只判返回值 `<= 0` 的写法在 TLS 上不会命中，
         *       失败要靠 catch 观察（会话层的两种传输因此在错误出口上并不等价）
         * @throws Base::InvalidArgumentException 长度超过 INT_MAX：取值非法属用法错误，走
         *         std::invalid_argument 分支（不在 CoreException 这条运行期故障链上）
         * @throws CoreException TLS 会话已失效（对端异常关闭、连接被重置等）
         * @throws CoreException 本端会话已释放：close() 之后调用，或协程挂起期间被 close()
         */
        Task<ssize_t> asyncSend(const void *buffer, size_t length) const;

        /**
         * @brief 关闭连接，释放 SSL 对象并关闭底层 socket。
         */
        void close();

        /**
         * @brief 获取底层 socket 的文件描述符。
         * @return 文件描述符值
         */
        [[nodiscard]] int fileDescriptor() const noexcept;

        /**
         * @brief 取 ALPN 协商结果
         * @details 走 SSL_get0_alpn_selected：客户端没提 ALPN、或列表里没有双方都支持的协议名时，
         *          结果为空串（此时双方按不使用 ALPN 处理，不代表握手失败）。
         * @return std::string 协商出的协议名，如 "h2"、"http/1.1"；未协商时为空串
         * @note **必须在握手完成之后调用**：ALPN 结果产生于握手过程，握手之前读到的一律是空串，
         *       调用方若在握手前分流会永远看到「未协商」
         */
        [[nodiscard]] std::string selectedAlpnProtocol() const;

        /**
         * @brief 获取对端的 IP 地址与端口。
         * @return InetAddress 对端地址
         * @throws Base::SystemException 底层套接字无法提供地址（通道已关闭等）
         */
        [[nodiscard]] InetAddress remoteAddress() const;

        /**
         * @brief 获取本端的 IP 地址与端口。
         * @return InetAddress 本端地址
         * @throws Base::SystemException 底层套接字无法提供地址（通道已关闭等）
         */
        [[nodiscard]] InetAddress localAddress() const;

    private:
        /**
         * @brief 取用 SSL 对象之前的存活闸门：会话已释放时给出本端原因，而不是让 OpenSSL 去报一个空错误
         * @details OpenSSL 3 对空 SSL 指针是返回失败而不是崩溃，但它从错误队列里取到的往往是
         *          `error:00000000:lib(0)::%reason(0)` 这种没有任何信息的原文，且各条路径的兜底文案
         *          一律把原因指向对端——真正的起因是本端被关停。闸门放在循环开头，
         *          因此每个让出点回到循环时都会被复查，新增让出分支不会漏保护。
         * @param operationName 报错文案里的操作名前缀（不含「失败」二字），如「TLS 读取」
         * @throws CoreException 本端 SSL 会话已释放
         */
        void requireLiveContext(std::string_view operationName) const;

        struct SslDeleter
        {
            void operator()(SSL *ssl) const noexcept
            {
                if (ssl)
                {
                    // TLS 双向关闭：第一次调用发送 close_notify
                    const int ret = SSL_shutdown(ssl);
                    if (ret == 0)
                    {
                        // 需要第二次调用接收对端的 close_notify
                        SSL_shutdown(ssl);
                    }
                    SSL_free(ssl);
                }
            }
        };

        std::unique_ptr<SSL, SslDeleter> m_ssl;                  ///< OpenSSL SSL 对象，RAII 管理
        /// 是否有一次 SSL_write 停在「WANT_WRITE，记录还在 SSL 内部待发」的中间状态上。
        /// OpenSSL 规定这种重试之前不许插入 SSL_read——插进去的话 SSL_read 会替写侧把待发记录
        /// 冲出去，写侧随后又按同一份数据重试，同一段明文在线上有两份，对端按记录解析当场错位。
        /// 读写各由一条协程驱动时这条禁令只能在本层守：见 asyncSend()/asyncReceive() 里的置位与让轮
        mutable bool m_isWritePending{false};
        /// 「反方向已被占用」时让出一次调度的时长：远小于任何握手/读超时口径，
        /// 只用来把控制权交回事件循环，让对方那个方向的协程先跑一步。
        /// 5ms 而非 1ms：对方方向靠自己的 I/O 事件推进，与本间隔无关，间隔只决定「本方向多久后
        /// 重试一次」——1ms 会在停顿期间造成近千赫兹的定时器唤醒，而 5ms 的重试延迟在握手场景里
        /// 完全够用
        static constexpr std::chrono::milliseconds kPeerProgressYieldInterval{5};

        /**
         * @brief 让出一次调度（定时器驱动），等反方向先推进
         * @details 用于 SSL_write 需要先读、SSL_read 需要先写这类情形：对方方向的等待槽不能抢
         *          （一个方向只允许一个等待者），自己方向又不能等（可写立刻返回会变成空转）。
         *          定时器是唯一既不抢槽也不空转的让出方式
         * @return true 让出成功；false 定时器不可用（调用方按失败收场）
         */
        [[nodiscard]] Task<bool> yieldForPeerProgress() const;

        EventLoop *                      m_loop{nullptr};        ///< 关联的事件循环，用于等待 socket 事件
        AsyncSocket                      m_socket;               ///< 底层异步 socket
        Role                             m_role{Role::Server};   ///< 本端角色：决定握手入口
        bool                             m_handshakeDone{false}; ///< 握手是否已完成
    };

} // namespace AsynGyanis::Core
