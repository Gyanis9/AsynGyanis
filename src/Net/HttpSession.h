/**
 * @file HttpSession.h
 * @brief HTTP 会话类，处理单个 HTTP 请求/响应周期
 * @copyright Copyright (c) 2026
 */
#ifndef NET_HTTPSESSION_H
#define NET_HTTPSESSION_H

#include "Core/Connection.h"
#include "Core/Task.h"
#include "HttpParser.h"
#include "Router.h"

#include <vector>

namespace Net
{
    /**
     * @brief HTTP 会话类，继承自 Core::Connection，处理一个 HTTP 连接上的请求解析、路由分发和响应发送。
     *
     * 每个 HttpSession 对应一个客户端 TCP 连接。通过 start() 协程循环读取 HTTP 请求，
     * 使用 HttpParser 增量解析，解析完成后交给 Router 寻找匹配的路由处理函数，
     * 生成响应并返回给客户端。支持 Keep-Alive 长连接。
     */
    class HttpSession : public Core::Connection
    {
    public:
        /**
         * @brief 构造 HTTP 会话。
         * @param loop   所属的事件循环
         * @param socket 已建立的异步 socket
         * @param router 全局路由器，用于分发请求
         */
        HttpSession(Core::EventLoop &loop, Core::AsyncSocket socket, Router &router);

        /**
         * @brief 启动会话主协程。
         *
         * 循环执行：读取 socket 数据 -> 解析 HTTP 请求 -> 生成响应 -> 发送响应。
         * 根据 Keep-Alive 决定是否继续处理下一个请求。若解析出错或连接关闭，则退出循环并关闭连接。
         *
         * @return Core::Task<> 协程任务
         * @override 覆盖基类 Connection::start()
         */
        Core::Task<> start() override;

    public:
        /**
         * @brief 根据 HTTP 请求和响应判断是否应保持连接（Keep-Alive）。
         * @param req  请求对象
         * @param res  响应对象
         * @return true  应保持连接，会话继续处理下一个请求
         * @return false 应关闭连接
         */
        static bool shouldKeepAlive(const HttpRequest &req, const HttpResponse &res);

    private:

        Router &             m_router;                ///< 路由器引用，用于分发请求
        HttpParser           m_parser;                ///< HTTP 解析器，用于解析请求数据
        std::vector<char>    m_recvBuffer;            ///< 接收缓冲区，存储从 socket 读取的原始数据
        static constexpr int m_recvBufferSize = 8192; ///< 接收缓冲区大小（8KB）
    };
}

#endif
