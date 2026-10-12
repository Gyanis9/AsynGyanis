/**
 * @file HttpProxyPumpedTls.h
 * @brief TLS-in-TLS 泵：把内层 SSL 会话架在外层 TlsSocket 的明文流上（https 目标经 https 代理）
 * @author Gyanis
 * @date 2026-10-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 内层 SSL 用一对内存 BIO 收发密文，「泵」把两侧的字节搬运起来：SSL 产出的记录排进
 *          待发队列、由单个 drainer 经外层 TlsSocket::asyncSend 送出；外层收到的字节灌进读 BIO。
 *          两个方向各自独立、不引入跨挂起点的锁——单线程事件循环里方向内不含挂起点的 SSL
 *          操作段是原子的，「谁在发送」由单标志仲裁：没抢到的一方把字节留在队列里，由抢到的一方
 *          发到空为止（谁入队谁都行，顺序始终是入队顺序）。
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Platform/Platform.h"

#include <openssl/types.h>

#include <cstddef>
#include <memory>
#include <string>

namespace AsynGyanis::Core
{
    class TlsSocket;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    /**
     * @brief 内层 TLS 会话 + 外层 TLS 传输的合体：对上层提供与 TlsSocket 相同的收发口径
     *
     * @details 供出站客户端在「https 目标经 https 代理」时承载内层 TLS：外层先与代理握手并开
     *          CONNECT 隧道，内层 SSL 的 SNI、主机名校验与 ALPN 照常按**目标**配置，字节经泵
     *          在外层隧道里进出。收发口径与 `Core::TlsSocket` 逐条对齐：正常收口返回 0、
     *          其余失败一律抛异常、长度上限 INT_MAX、0 长度不做 I/O 直接回 0。
     * @warning 与 `Core::TlsSocket` 同一套并发约定：读、写各只允许一个并发调用者（h2 的读写
     *          两条协程正是这个形状）；对象只在所属事件循环上使用。
     */
    class HttpProxyPumpedTls
    {
    public:
        /// 内层 SSL 的所有权句柄：泵接管它，析构时双向关闭并释放（内存 BIO 随 SSL 一起归还）
        using InnerSslHandle = std::unique_ptr<SSL, void (*)(SSL *)>;

        /// 构造 InnerSslHandle 用的 deleter：尽力发一个 close_notify 再释放（纯本地收尾，失败无关紧要）
        static void freeInnerSsl(SSL *innerSsl) noexcept;

        /**
         * @brief 接管内层 SSL 与外层传输，并把内存 BIO 装上内层 SSL
         * @param innerSsl 已配好 SNI / 主机名校验 / ALPN 的内层 SSL（不得绑过文件描述符）
         * @param outer 与代理握手完成的外层 TLS 传输
         * @note BIO 创建失败（内存不足）时对象进入「未就绪」状态，由 isReady() 交回；
         *       不在构造里抛，调用点按返回值收口
         */
        HttpProxyPumpedTls(InnerSslHandle innerSsl, std::unique_ptr<Core::TlsSocket> outer);

        /// 析构：释放内层 SSL 并关闭外层传输
        ~HttpProxyPumpedTls();

        HttpProxyPumpedTls(const HttpProxyPumpedTls &)            = delete;
        HttpProxyPumpedTls &operator=(const HttpProxyPumpedTls &) = delete;
        HttpProxyPumpedTls(HttpProxyPumpedTls &&)                 = delete;
        HttpProxyPumpedTls &operator=(HttpProxyPumpedTls &&)      = delete;

        /// 内存 BIO 是否已就绪（false 表示创建失败，调用方应当收口并报「内存不足」一类原因）
        [[nodiscard]] bool isReady() const noexcept
        {
            return m_isReady;
        }

        /**
         * @brief 驱动内层 TLS 握手（字节经泵在外层隧道里进出）
         * @throws Core::CoreException 握手失败（证书、协议），或外层隧道中断
         */
        Core::Task<> handshake();

        /**
         * @brief 读取内层 TLS 的明文
         * @param buffer 接收缓冲区
         * @param length 缓冲区长度，不得超过 INT_MAX
         * @return Core::Task<ssize_t> 实际读到的字节数；0 表示对端发来 close_notify
         * @throws Base::InvalidArgumentException 长度超过 INT_MAX
         * @throws Core::CoreException 会话失效或外层隧道中断
         */
        Core::Task<ssize_t> asyncReceive(void *buffer, size_t length);

        /**
         * @brief 写出内层 TLS 的明文
         * @param buffer 发送缓冲区
         * @param length 缓冲区长度，不得超过 INT_MAX
         * @return Core::Task<ssize_t> 实际写出的字节数（可能小于 length，调用方按已写长度续发）
         * @throws Base::InvalidArgumentException 长度超过 INT_MAX
         * @throws Core::CoreException 会话失效或外层隧道中断
         */
        Core::Task<ssize_t> asyncSend(const void *buffer, size_t length);

        /// 关闭：释放内层 SSL 并关闭外层传输（幂等）
        void close() noexcept;

        /// 底层文件描述符（外层传输的）
        [[nodiscard]] int fileDescriptor() const noexcept;

        /// 内层 TLS 协商出的 ALPN 协议名（未协商时为空串）
        [[nodiscard]] std::string selectedAlpnProtocol() const;

    private:
        /// 取用之前的存活闸门：本端已 close() 时给出明确原因，而不是让 OpenSSL 报空错误
        void requireAlive(std::string_view operationName) const;

        /// 把写 BIO 里 SSL 已产出的记录搬进待发队列（同步段：读、写两个方向都会调用）
        void drainInnerOutput();

        /**
         * @brief 把待发队列经外层送出去
         * @return Core::Task<bool> true = 队已空（或正由别的协程在发）；false = 外层传输失效
         * @details 同一时刻只有一个 drainer：没抢到标志的调用方直接回 true（字节由抢到的一方发完，
         *          它循环到队列为空才放手）。跨挂起点只发生在这一处，且只有 drainer 碰外层 asyncSend
         */
        Core::Task<bool> flushPendingWire();

        /// 从外层收一段字节灌进读 BIO；外层收口或断链返回 false
        Core::Task<bool> pumpIncomingOnce();

        /// 把 SSL_get_error 的结果折成可读原因（不含 OpenSSL 错误队列原文）
        [[nodiscard]] static std::string describeSslFailure(int sslErrorCode);

        InnerSslHandle                   m_inner;             ///< 内层 SSL（内存 BIO 的所有者）
        std::unique_ptr<Core::TlsSocket> m_outer;             ///< 外层传输（与代理的 TLS 会话）
        std::string                      m_pendingWire;       ///< 等待送出的密文（入队顺序即线序）
        bool                             m_isFlushing{false}; ///< 单 drainer 标志
        bool                             m_isClosed{false};   ///< 本端已收口
        bool                             m_isReady{false};    ///< 内存 BIO 是否创建成功
    };

} // namespace AsynGyanis::Net
