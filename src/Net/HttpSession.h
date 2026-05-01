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

#include <functional>
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
         * @param loop      所属的事件循环
         * @param socket    已建立的异步 socket
         * @param router    全局路由器，用于分发请求
         * @param staticDir 静态文件根目录（可选），设置后自动启用静态文件服务
         */
        HttpSession(Core::EventLoop &loop, Core::AsyncSocket socket, Router &router,
                    std::optional<std::string> staticDir = std::nullopt);

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

        Router &                  m_router;                ///< 路由器引用，用于分发请求
        HttpParser                m_parser;                ///< HTTP 解析器，用于解析请求数据
        std::vector<char>         m_recvBuffer;            ///< 接收缓冲区，存储从 socket 读取的原始数据
        std::optional<std::string> m_staticDir;             ///< 静态文件根目录，若为 nullopt 表示未启用
        static constexpr int      m_recvBufferSize = 8192; ///< 接收缓冲区大小（8KB）
    };

    // ============================================================================
    // 共享 HTTP Keep-Alive 循环模板（HttpSession 与 HttpsSession 共用）
    // ============================================================================

    namespace detail
    {
        /**
         * @brief 模板化的 HTTP Keep-Alive 请求/响应循环。
         *
         * 封装了所有 HTTP/1.1 协议处理逻辑：增量解析、路由分发、错误处理、
         * Keep-Alive 判断、响应发送等。通过模板参数 Socket 支持普通 TCP
         *（AsyncSocket）和 TLS 加密（TlsSocket）两种传输层。
         *
         * @tparam Socket 传输层类型，需支持 asyncRecv/asyncSend/close 接口
         * @param sock        传输层 socket 引用
         * @param router      路由器，用于分发请求
         * @param parser      HTTP 增量解析器
         * @param recvBuffer  接收缓冲区
         * @param isAlive     连接存活检查回调
         */
        template <typename Socket>
        Core::Task<> httpKeepAliveLoop(
            Socket &sock,
            Router &router,
            HttpParser &parser,
            std::vector<char> &recvBuffer,
            const std::function<bool()> &isAlive)
        {
            // sendAll 辅助：分批发送大数据块，处理部分写入
            auto sendAll = [&sock](const std::string_view data) -> Core::Task<>
            {
                size_t totalSent = 0;
                while (totalSent < data.size())
                {
                    const ssize_t n = co_await sock.asyncSend(
                        data.data() + totalSent, data.size() - totalSent);
                    if (n <= 0)
                        co_return;
                    totalSent += static_cast<size_t>(n);
                }
            };

            bool keepAlive = true;

            while (keepAlive && isAlive())
            {
                std::string errorResponse;
                bool        hasError = false;

                try
                {
                    ssize_t n = co_await sock.asyncRecv(recvBuffer.data(), recvBuffer.size());
                    if (n <= 0)
                        break;

                    auto status = parser.parse(recvBuffer.data(), static_cast<size_t>(n));

                    // 增量解析：数据不足时持续读取
                    while (status == ParseStatus::NeedMore)
                    {
                        n = co_await sock.asyncRecv(recvBuffer.data(), recvBuffer.size());
                        if (n <= 0)
                            co_return;
                        status = parser.parse(recvBuffer.data(), static_cast<size_t>(n));
                    }

                    if (status == ParseStatus::Error)
                    {
                        HttpResponse res;
                        res.setStatus(400);
                        res.setBody(std::format("Bad Request: {}", parser.errorMessage()));
                        res.setHeader("content-type", "text/plain");
                        res.setHeader("connection", "close");
                        errorResponse = res.toString();
                        hasError      = true;
                    }
                    else if (status == ParseStatus::Done)
                    {
                        auto &       req = parser.request();
                        HttpResponse res;

                        try
                        {
                            co_await router.route(req, res);
                        }
                        catch (const std::exception &)
                        {
                            res = HttpResponse::serverError("Internal Server Error");
                        }

                        keepAlive = HttpSession::shouldKeepAlive(req, res);

                        const auto &version  = req.httpVersion();
                        const bool  isHttp10 = version.starts_with("HTTP/1.0")
                                            || version.starts_with("HTTP/0.9");

                        if (!keepAlive)
                            res.setHeader("connection", "close");
                        else if (isHttp10 && !res.headers().contains("connection"))
                            res.setHeader("connection", "keep-alive");

                        const std::string responseStr = res.toString();
                        // Fast path: 小响应单次发送，避免协程帧分配
                        if (responseStr.size() <= 4096)
                            co_await sock.asyncSend(responseStr.data(), responseStr.size());
                        else
                            co_await sendAll(responseStr);

                        if (keepAlive)
                            parser.reset();
                    }
                }
                catch (const std::exception &)
                {
                    HttpResponse res = HttpResponse::serverError("Internal Server Error");
                    res.setHeader("connection", "close");
                    errorResponse = res.toString();
                    hasError      = true;
                }

                if (hasError)
                {
                    if (errorResponse.size() <= 4096)
                        co_await sock.asyncSend(errorResponse.data(), errorResponse.size());
                    else
                        co_await sendAll(errorResponse);
                    keepAlive = false;
                }
            }
            co_return;
        }

    } // namespace detail
}

#endif
