/**
 * @file HttpsSession.h
 * @brief HTTPS 会话 — 继承自 Connection，先执行 TLS 握手，再在加密通道上进行 HTTP 通信
 * @copyright Copyright (c) 2026
 */
#ifndef NET_HTTPSSESSION_H
#define NET_HTTPSSESSION_H

#include "Core/Connection.h"
#include "Core/TlsSocket.h"
#include "Core/Task.h"
#include "HttpParser.h"
#include "Router.h"

namespace Net
{
    /**
     * @brief HTTPS 会话类。
     *
     * 继承自 Core::Connection，为每个 HTTPS 连接提供安全通信能力。
     * 首先进行 TLS 握手（co_await m_tlsSocket.handshake()），
     * 之后在加密通道上循环读取解密后的 HTTP 请求，使用 HttpParser 解析，
     * 再通过 Router 分发并生成响应，最后通过 TlsSocket 加密发送响应。
     */
    class HttpsSession : public Core::Connection
    {
    public:
        /**
         * @brief 构造 HTTPS 会话。
         * @param loop      所属事件循环
         * @param tlsSocket 已创建但尚未握手的 TlsSocket 对象（内部持有原始 AsyncSocket 和 SSL 对象）
         * @param router    全局路由器，用于分发 HTTP 请求
         *
         * 注意：构造函数不执行握手，握手将在 start() 协程中进行。
         */
        HttpsSession(Core::EventLoop &loop, Core::TlsSocket tlsSocket, Router &router);

        /**
         * @brief 启动 HTTPS 会话主协程。
         *
         * 执行流程：
         * 1. 调用 m_tlsSocket.handshake() 完成 TLS 握手（若失败则关闭连接并返回）。
         * 2. 循环接收解密后的 HTTP 请求数据，解析为 HttpRequest。
         * 3. 使用 Router 匹配路由，生成 HttpResponse。
         * 4. 通过 m_tlsSocket.asyncSend() 发送加密响应。
         * 5. 根据 Keep-Alive 决定是否继续处理下一个请求。
         * 6. 若连接关闭或出错，退出循环并关闭连接。
         *
         * @return Core::Task<> 协程任务
         * @override 覆盖基类 Connection::start()
         */
        Core::Task<> start() override;

    private:
        Core::TlsSocket      m_tlsSocket;             ///< TLS Socket，封装 SSL 对象和原始 socket，提供加密读写
        Router &             m_router;                ///< 路由器引用，用于请求分发
        HttpParser           m_parser;                ///< HTTP 解析器，用于从解密数据中解析请求
        std::vector<char>    m_recvBuffer;            ///< 接收缓冲区（存储解密后的原始 HTTP 数据）
        static constexpr int m_recvBufferSize = 8192; ///< 接收缓冲区大小（8KB）
    };
}

#endif
