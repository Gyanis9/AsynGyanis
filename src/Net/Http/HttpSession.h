/**
 * @file HttpSession.h
 * @brief HTTP 会话：在单条 TCP 连接上做「解析—路由—应答」的保持活跃循环
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/LogicException.h"
#include "Core/Coroutine/Cancelable.h"
#include "Core/Coroutine/Task.h"
#include "Core/Socket/Connection.h"
#include "Net/Http/HttpParser.h"
#include "Net/Http/Router.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief HTTP 会话类：一条客户端 TCP 连接对应一个 HttpSession。
     *
     * @details start() 协程在连接上循环「读字节 → 定界 → 增量解析 → 路由分发 → 写响应」，
     *          直到对端关闭、报文出错、或按 Keep-Alive 判定应当收口。协议细节全部下沉到
     *          detail::httpKeepAliveLoop()，与 HttpsSession 共用同一份实现，两者只差传输层类型。
     *
     * @note 接收缓冲**跨次读取存续**：一个 TCP 包里粘着两条请求时，第一条应答完剩下的字节留在
     *       缓冲区里，下一轮先喂进解析器；不留存就会每次从缓冲区开头重读，把第二条请求静默丢掉。
     * @see detail::httpKeepAliveLoop(), shouldKeepAlive()
     */
    class HttpSession : public Core::Connection
    {
    public:
        /**
         * @brief 构造 HTTP 会话。
         * @param socket 已建立的异步 socket，所有权转移给基类 Core::Connection
         * @param router 全局路由器，用于分发请求；其生命周期必须不短于本会话
         *
         * @note 事件循环由 AsyncSocket 内部持有，会话不需要第二份引用，因此只收一个 socket
         *       （见 Core::Connection 的构造）。
         */
        HttpSession(Core::AsyncSocket socket, Router &router);

        /**
         * @brief 启动会话主协程：跑完整条保持活跃循环后关闭连接。
         *
         * @details 重写 Core::Connection::start()。基类默认实现只是一个立即完成的空协程
         *          （直接 co_return），供不需要协议逻辑的连接子类继承；本实现把它换成完整的
         *          HTTP 事务循环：转入 detail::httpKeepAliveLoop() 处理读、解析、路由与应答，
         *          循环退出后无条件调用 close() 归还描述符——会话不会因为退出路径不同而漏关 socket。
         *          与基类相同的另一点：异常不在此吞掉，原样抛给 TcpServer::handleConnection()。
         *
         * @return Core::Task<> 协程任务，连接结束（自然关闭或出错收口）时完成
         * @see Core::Connection::start(), detail::httpKeepAliveLoop()
         */
        Core::Task<> start() override;

        /**
         * @brief 按 RFC 9112 §9 判定这条事务之后是否保持连接（Keep-Alive）。
         *
         * @details 判定顺序固定，且**请求侧的显式 close 不可被响应头反转**：请求或响应带
         *          `Connection: close` → 一律断开；请求带 `Connection: keep-alive` → 保活（只对
         *          HTTP/1.0 有实际意义，1.1 默认本就保活）；都没有时 1.1 及以上默认保活，
         *          1.0 与 0.9 默认断开。响应的 `Connection: keep-alive` 不参与判定。
         *          同名头部的多个值按逗号拆分后逐 token 比对（`Connection: keep-alive, X` 这种写法合法）。
         *
         * @param request  已完成解析的请求
         * @param response 即将发送的响应
         * @return true 应保持连接，继续处理下一个请求
         * @return false 应答完成后关闭连接
         */
        [[nodiscard]] static bool shouldKeepAlive(const HttpRequest &request, const HttpResponse &response);

    private:
        Router &m_router;             ///< 路由器引用，用于分发请求
        HttpParser m_parser;          ///< HTTP 增量解析器，两条报文之间由会话显式 reset()
        std::vector<char> m_receiveBuffer; ///< 跨次读取存续的接收窗口，首次读取时按固定大小分配
    };

    // ============================================================================
    // 会话共享实现：报文定界 + Keep-Alive 事务循环（HttpSession 与 HttpsSession 共用）
    // ============================================================================

    namespace detail
    {
        /// 接收窗口大小，单位字节：只用来接住「刚到的字节」，正文与跨读的半行都由解析器自己存，
        /// 因此这一块固定大小就够——窗口永远是「开头一段未解析字节」，不需要按报文体量增长
        inline constexpr std::size_t kReceiveWindowLength = 8ull * 1024;

        /**
         * @brief 把解析失败类别翻译成要发的 4xx 响应（状态码与正文全 ASCII）
         * @details 边界判定（报文到哪里结束、哪里越界、哪里读不懂）全部由 HttpParser 负责并给出类别，
         *          会话只按类别选状态码，不重复判一次边界。
         * @param response 待填充的响应对象，进入本函数时应当是新构造的
         * @param errorKind 解析器给出的失败类别
         */
        void writeParseErrorResponse(HttpResponse &response, HttpParseErrorKind errorKind);

        /**
         * @brief 判断一组同名头部值里是否出现了某个 token（大小写不敏感，按逗号拆分）
         * @param headerValueList 同一头名的全部值，按线上到达顺序
         * @param expectedToken 要查找的 token，例如 "close"、"keep-alive"
         * @return true 至少有一条值里出现了该 token
         */
        bool headerValueListContainsToken(const std::vector<std::string> &headerValueList, std::string_view expectedToken);

        /**
         * @brief 停止回调实体：把连接的停止请求转发成本次请求的协作式取消
         *
         * @details stop_callback 按回调类型模板化并内联存储，这里用具名 struct 而不是
         *          lambda 或 std::function：既没有类型擦除，也不产生每请求一次的堆分配，
         *          同时让 ConnectionCancelForwarder 的成员类型可以在头文件里写出来。
         * @note 标准里的 stop_callback 只接受 (stop_token, callback) 两个构造参数，
         *       早期技术规范那套「函数指针 + void* 上下文」的三参形式并不存在。
         */
        struct RequestCancelForwarder
        {
            HttpRequest *request = nullptr; ///< 本次请求，其存活期由注册方的作用域保证

            /**
             * @brief 对当前请求发出协作式取消
             */
            void operator()() const
            {
                // 回调只可能在请求对象存活期间被触发：注册与注销都被
                // ConnectionCancelForwarder 的构造/析构夹住，这里仍判一次空以防误用
                if (request != nullptr)
                {
                    request->requestCancel();
                }
            }
        };

        /**
         * @brief 把「连接的停止请求」转发成「当前请求的协作式取消」
         *
         * @details 取消机制有两处：Core::Connection 的 Cancelable 与 HttpRequest 自带的 stop_source。
         *          真值来源取前者（连接级、由服务器与运维路径驱动），后者只作为业务侧的读取出口：
         *          超时中间件调 request.requestCancel()，而连接被关停时由本对象把信号补进同一个出口，
         *          业务只需认 request.cancelToken() 一处。
         * @note 按**连接**注册一次即可：回调指向解析器内部那个按连接复用的请求对象，
         *       于是不必跟着每条报文反复注册与注销。若按请求注册，注册动作本身（停止回调节点）
         *       就成了每请求一次的开销。
         */
        class ConnectionCancelForwarder
        {
        public:
            /**
             * @brief 注册停止回调
             * @param cancelable 所属连接的取消源
             * @param request 本次请求，收到停止请求时对它发出协作式取消
             */
            ConnectionCancelForwarder(Core::Cancelable &cancelable, HttpRequest &request);

        private:
            /// 停止回调：内联存储的具名可调用对象，析构即注销；C++20 的 stop_callback 只带回调类型一个模板参数
            std::stop_callback<RequestCancelForwarder> m_stopCallback;
        };

        /**
         * @brief 模板化的 HTTP 保持活跃事务循环。
         *
         * @details 承载全部 HTTP/1.1 协议逻辑：跨读取的缓冲管理、报文定界、增量解析、
         *          路由分发、Keep-Alive 判定与分片发送。传输层用模板参数区分，
         *          普通 TCP（Core::AsyncSocket）与 TLS（Core::TlsSocket）走同一份代码，
         *          避免两条实现路径各自漂移。
         *
         * @note 出错就收口，不原地复位后接着用：解析器一旦 Error 就进入粘滞错误态，
         *       而此刻字节流的边界已不可信，继续复用的风险高于断开重连。
         *       需要回 4xx 告诉客户端原因的，发完再断。
         * @note 读侧没有超时：半截头部/正文会让这条协程一直挂在 epoll 上。框架里定时器已经是
         *       循环级队列的轻量句柄（不占描述符、构造无系统调用），做连接级空闲超时的门槛
         *       只在「超时后如何收尾」这一策略上。
         *
         * @tparam Socket 传输层类型，需支持 asyncReceive/asyncSend
         * @param socket        传输层 socket 引用
         * @param cancelable    所属连接的取消源，用于把连接级停止转成本请求的协作式取消
         * @param router        路由器
         * @param parser        HTTP 增量解析器，由会话持有
         * @param receiveBuffer 跨次读取存续的接收缓冲，由会话持有
         * @param isAlive       连接存活谓词，每轮事务与每次挂起前检查
         */
        template<typename Socket>
        Core::Task<> httpKeepAliveLoop(Socket &socket,
                                       Core::Cancelable &cancelable,
                                       Router &router,
                                       HttpParser &parser,
                                       std::vector<char> &receiveBuffer,
                                       const std::function<bool()> &isAlive)
        {
            // 接收窗口里尚未交给解析器的字节数。窗口只用来「接住刚到的字节」：正文由解析器
            // 边收边存，跨读的半行也由解析器自己拼，因此这里永远是「窗口开头的一段」，
            // 解析器消费多少就把后面剩的挪到开头，不需要按报文体量扩容
            std::size_t windowLength = 0;

            bool keepAlive = true;

            // 响应对象按连接复用（每轮开头 reset）：容器容量跨请求保留，
            // 让「每条报文都重新长一遍头部容器」这笔开销消失
            HttpResponse response;

            // 连接被关停时把它转成本次请求的协作式取消：业务只认 request.cancelToken() 一处。
            // 只注册一次、覆盖整条连接：回调指向解析器内部那个按连接复用的请求对象，
            // 因此不必跟着每条报文走——那会让每个请求都付一次停止回调的注册开销。
            // 对象在循环外构造，作用域覆盖整个 keep-alive 循环，析构即注销
            ConnectionCancelForwarder cancelForwarder(cancelable, parser.request());

            // 发出响应：把「头部块 + 正文」作为两段提交，正文因此不必先拷进头部块。
            // 传输层支持聚合写（AsyncSocket）时是一次系统调用提交两段；TLS 记录层只接受
            // 单块明文，退回两次顺序发送——两者都在数据语义上等价，差别只在是否多一次拷贝。
            // 视图指向的数据活到本次 co_await 结束（响应对象活得更久，序列化结果活在这个
            // 完整表达式里），因此引用捕获是安全的
            const auto sendResponse = [&socket](const std::string_view head, const std::string_view body) -> Core::Task<bool>
            {
                if constexpr (requires { socket.asyncSendVectored(nullptr, 0); })
                {
                    if (body.empty())
                    {
                        const ssize_t writeLength = co_await socket.asyncSend(head.data(), head.size());
                        co_return writeLength > 0;
                    }
                    const Platform::Socket::WriteBuffer buffers[2] = {
                            {head.data(), head.size()},
                            {body.data(), body.size()},
                    };
                    const ssize_t writeLength = co_await socket.asyncSendVectored(buffers, 2);
                    co_return writeLength > 0;
                } else
                {
                    if (const ssize_t headLength = co_await socket.asyncSend(head.data(), head.size()); headLength <= 0)
                    {
                        co_return false;
                    }
                    if (body.empty())
                    {
                        co_return true;
                    }
                    const ssize_t bodyLength = co_await socket.asyncSend(body.data(), body.size());
                    co_return bodyLength > 0;
                }
            };

            // 读一次网络字节到窗口剩余空间。返回 0 表示对端正常关闭，负值表示连接不可用
            const auto readIntoWindow = [&isAlive, &receiveBuffer, &socket, &windowLength]() -> Core::Task<ssize_t>
            {
                // 挂起前先复查存活：对端断开或被服务器强制关闭时不该再多读一次
                if (!isAlive())
                {
                    co_return -1;
                }

                // 窗口挡满时才需要读：调用点保证「窗口里没有未解析的字节」到这里来，
                // 因此下面这两句是把窗口整体腾空，而不是在已有数据后面追加
                if (receiveBuffer.size() == 0)
                {
                    receiveBuffer.resize(kReceiveWindowLength);
                }
                windowLength = 0;

                try
                {
                    co_return co_await socket.asyncReceive(receiveBuffer.data(), receiveBuffer.size());
                } catch (const std::exception &)
                {
                    // 传输层读失败（对端 RST、描述符被 close() 关掉、TLS 记录错误）一律视为连接不可用：
                    // 字节流已经断了，这里没有任何可发给对端的东西，也无处可发
                    co_return -1;
                }
            };

            while (keepAlive && isAlive())
            {
                // ---------------- 第一步：把窗口里的字节交给解析器 ----------------
                // 窗口空才去读：解析器在 NeedMore 时会把喂进去的字节全部消费掉，因此「窗口里还有
                // 未解析字节」只可能出现在 Done 之后（剩下的属于下一条报文），此时应当先解析它
                if (windowLength == 0)
                {
                    const ssize_t receivedLength = co_await readIntoWindow();
                    if (receivedLength <= 0)
                    {
                        // 对端正常关闭（0）或连接不可用（负值）：半截报文不值得回包，直接结束会话
                        co_return;
                    }
                    windowLength = static_cast<std::size_t>(receivedLength);
                }

                const ParseStatus status = parser.parse(receiveBuffer.data(), windowLength);
                const std::size_t consumedLength = parser.consumedByteCount();

                // 契约：NeedMore 意味着本段输入已被全部消费。若一个字节都没消费却还要更多，
                // 窗口就无法推进，这一轮会变成死循环——宁可当场报错，也不要静默转圈
                if (status == ParseStatus::NeedMore && consumedLength == 0)
                {
                    throw Base::LogicException("HttpSession: 解析器未消费任何字节却要求更多输入，窗口无法推进");
                }

                // 已消费的前缀挪掉：Done 之后剩下的字节正是流水线里的下一条报文
                if (consumedLength != 0)
                {
                    const std::size_t unconsumedLength = windowLength - consumedLength;
                    if (unconsumedLength != 0)
                    {
                        std::memmove(receiveBuffer.data(), receiveBuffer.data() + consumedLength, unconsumedLength);
                    }
                    windowLength = unconsumedLength;
                }

                if (status == ParseStatus::NeedMore)
                {
                    continue;
                }

                if (status == ParseStatus::Error)
                {
                    // 出错即结束会话（不 reset 后接着复用同一条连接）。状态码按解析器给出的
                    // 失败类别映射：形态合法只是体量越界回 431/413，缺长度回 411，其余回 400
                    HttpResponse errorResponse;
                    writeParseErrorResponse(errorResponse, parser.errorKind());

                    co_await sendResponse(errorResponse.serializeHead(), errorResponse.body());

                    // 断开之前显式复位：把粘滞错误态与半成品请求一起清掉，
                    // 万一上层复用同一个解析器对象（例如把会话挪作他用），也不会读到脏请求
                    parser.reset();
                    co_return;
                }

                // ---------------- 第三步：路由与应答 ----------------
                HttpRequest &request = parser.request();

                // 响应对象按连接复用：容器容量跨请求保留，省掉每条报文重新分配一遍。
                // 复用必须配一次复位，否则上一条报文的头部会跟着下一条发出去
                response.reset();
                response.setHttpVersion(request.httpVersion()); // 状态行版本跟随请求，不硬编码 1.1

                std::exception_ptr handlerException = nullptr;
                try
                {
                    co_await router.route(request, response);
                } catch (...)
                {
                    // 先存起来，等取消转发器注销之后再改写响应：
                    // 顺序反过来会让析构阶段的动作压在已重置的响应上
                    handlerException = std::current_exception();
                }

                if (handlerException)
                {
                    // 业务异常必须整体重置响应再填 500：handler 可能已经写了一半头部与正文，
                    // 只改状态码会把半成品连同错的 content-length 一起发出去
                    const std::string requestVersion = request.httpVersion();
                    response.reset();
                    response.setStatus(500);
                    response.setBody("Internal Server Error");
                    response.setHeader("content-type", "text/plain");
                    if (!requestVersion.empty())
                    {
                        response.setHttpVersion(requestVersion);
                    }
                }

                // 取消转发器不在这里注销：它按连接注册一次（见循环前），
                // 回调指向解析器内部那个按连接复用的请求对象，跨请求依然指向正确目标

                keepAlive = HttpSession::shouldKeepAlive(request, response);

                const std::string &requestVersion = request.httpVersion();
                const bool isHttp10OrOlder = requestVersion.starts_with("HTTP/1.0") || requestVersion.starts_with("HTTP/0.9");

                // Connection 头按判定结果补齐：断连要显式告知，1.0 保活也要显式告知，
                // 否则对端会按自己的默认值理解这条连接，出现「一端留着、一端关掉」的错位
                if (!keepAlive)
                {
                    response.setHeader("connection", "close");
                } else if (isHttp10OrOlder && !response.getHeader("connection").has_value())
                {
                    response.setHeader("connection", "keep-alive");
                }

                // 头部序列化一次，正文留在响应对象里：两段一起提交，正文不必再拷一份。
                // serializedHead 是具名局部，正文视图指向的 response 也活到本次调用之后
                const std::string serializedHead = response.serializeHead();
                if (!co_await sendResponse(serializedHead, response.body()))
                {
                    co_return;
                }

                // ---------------- 第四步：为下一条报文复位 ----------------
                // 复位解析器（连带把请求对象重置成干净壳子）。窗口里若还有字节，那是流水线
                // 包进来的下一条报文，下一轮循环直接接着解析
                parser.reset();
            }
        }
    } // namespace detail
} // namespace AsynGyanis::Net
