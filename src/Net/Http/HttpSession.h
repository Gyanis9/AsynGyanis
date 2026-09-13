/**
 * @file HttpSession.h
 * @brief HTTP 会话：在单条 TCP 连接上做「解析—路由—应答」的保持活跃循环
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/LogicException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Cancelable.h"
#include "Core/Coroutine/Task.h"
#include "Core/Socket/Connection.h"
#include "Net/Http/HttpParser.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/Router.h"
#include "Net/WebSocket/WebSocketHandshake.h"
#include "Net/WebSocket/WebSocketPeer.h"

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
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
         * @param limits 连接级限额的共享只读配置；传空指针表示按 HttpServerLimits 的默认值执行
         * @param metrics 统计采集端；传空指针表示本会话不采集统计（请求计数、状态码分类与延迟直方图都不更新）
         * @param requestIdGenerator request-id 生成器；传空指针表示本会话不为请求落定 request-id
         *
         * @note 事件循环由 AsyncSocket 内部持有，会话不需要第二份引用，因此只收一个 socket
         *       （见 Core::Connection 的构造）。
         * @note 配置按 shared_ptr 只读共享而不是按值拷一份：一份配置被本服务器所有会话共用，
         *       换配置时整体换代（HttpServer::setLimits()），在途会话永远读到自己那份完整配置
         * @note 统计对象与生成器同样按 shared_ptr 共享：它们由服务器持有，会话只是借用来上报，
         *       因此会话比服务器活得久时也不会写到已释放对象上；计数器内部全是原子量，
         *       多线程上的会话并发上报同一个服务器是安全的
         */
        HttpSession(Core::AsyncSocket socket, Router &router, std::shared_ptr<const HttpServerLimits> limits = nullptr,
                    std::shared_ptr<HttpMetricsCollector> metrics = nullptr,
                    std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator = nullptr);

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

        /**
         * @brief 本连接被空闲清扫协程按超时关闭时上报到所属服务器的统计
         *
         * @details 重写 Core::Connection::onIdleTimeoutClosed()：把这次收口累加进 timeoutClosedCount。
         *          基类默认实现是空操作（不关心超时的连接无需上报），这里的差异只多一次原子自增，
         *          不再写日志——清扫协程已经记下了「哪条连接、超时误差多大」。
         * @note 未持有统计对象时（例如只关心协议的调用方直接构造会话）什么都不做
         */
        void onIdleTimeoutClosed() noexcept override;

    private:
        Router &m_router;             ///< 路由器引用，用于分发请求
        HttpParser m_parser;          ///< HTTP 增量解析器，两条报文之间由会话显式 reset()
        std::vector<char> m_receiveBuffer; ///< 跨次读取存续的接收窗口，首次读取时按固定大小分配
        std::shared_ptr<const HttpServerLimits> m_limits; ///< 连接级限额，与服务器共享、只读（构造时保证非空）
        std::shared_ptr<HttpMetricsCollector> m_metrics;  ///< 统计采集端，与服务器共享；空指针表示本会话不采集
        std::shared_ptr<HttpRequestIdGenerator> m_requestIdGenerator; ///< request-id 生成器，与服务器共享；空指针表示不落定 request-id
    };

    // ============================================================================
    // 会话共享实现：报文定界 + Keep-Alive 事务循环（HttpSession 与 HttpsSession 共用）
    // ============================================================================

    namespace detail
    {
        /// 接收窗口大小，单位字节：只用来接住「刚到的字节」，正文与跨读的半行都由解析器自己存，
        /// 因此这一块固定大小就够——窗口永远是「开头一段未解析字节」，不需要按报文体量增长
        inline constexpr std::size_t kReceiveWindowLength = 8ull * 1024;

        /// 分块传输的终止块：零长度块加尾部空行，即「本条消息到此结束」（RFC 9112 §7.1）。
        /// 它同时就是 keep-alive 的消息边界，因此流式响应写完不必断开连接
        inline constexpr std::string_view kChunkedTerminator = "0\r\n\r\n";

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
         * @brief 在途工作标记的 RAII 守卫：构造置位、析构清除
         *
         * @details 协议层用 Core::Connection 的 busy 标记告诉 TcpServer::drain()「本连接正在处理请求」。
         *          标记的存续期就是「处理中」这段作用域，因此用守卫而不是在每条出口上手工清除：
         *          提前 co_return 与异常展开都会走到析构，不会留下永远忙碌的连接。
         */
        class BusyScope
        {
        public:
            /**
             * @brief 置位所属连接的在途工作标记
             * @param connection 所属连接，其生命周期必须覆盖本守卫（两者同活在会话协程帧里）
             */
            explicit BusyScope(Core::Connection &connection) noexcept :
                m_connection(&connection)
            {
                m_connection->setBusy(true);
            }

            /**
             * @brief 析构时清除在途工作标记
             */
            ~BusyScope()
            {
                m_connection->setBusy(false);
            }

            BusyScope(const BusyScope &) = delete;

            BusyScope &operator=(const BusyScope &) = delete;

        private:
            Core::Connection *m_connection{nullptr}; ///< 被标记的连接（非拥有，随帧存活）
        };

        /**
         * @brief WebSocket 阶段：101 之后在这条连接上跑业务处理器，直到任一侧收口
         *
         * @details 本阶段是升级分支的全部实现：读字节 → 喂解码器（分片重组、掩码与 RSV 校验都在
         *          解码层）→ 由 WebSocketPeer 交给业务 → 按 RFC 6455 §5.5.1 收尾。业务协程不 co_await
         *          而是显式推进，因此这条连接上任何时刻都只有一个协程在跑：会话在业务挂回 receive()
         *          时拿回控制权，业务返回（正常或抛异常）时置位标记，下一轮循环据此进入收尾。
         *
         * @note 一条连接要么 HTTP 要么 WebSocket：本阶段返回后调用方直接结束会话，不再回到
         *       keep-alive 事务循环。
         * @note 收尾时机：业务在会话等读期间返回时，会话要等这条连接下一次可读、对端关闭或
         *       空闲清扫（idleTimeout）到期才会收口——会话协程此刻挂在 socket 的读等待上，
         *       而跨协程恢复一个挂在 IO 上的协程会破坏它内部的挂起链，因此不做这种唤醒。
         *
         * @tparam Socket 传输层类型，需支持 asyncReceive/asyncSend
         * @param socket 传输层 socket 引用，其生命周期覆盖整个阶段
         * @param connection 所属连接，用于刷新空闲截止时间与查询存活；其生命周期覆盖整个阶段
         * @param limits 连接级限额，取自 HttpServerLimits
         * @param handler 业务处理器；按值接收（惰性协程的入参必须由协程帧自己持有）
         * @param receiveBuffer 会话的接收窗口，本阶段按窗口长度整块读取
         * @param pendingLength 升级请求之后窗口里剩余的字节数：客户端可能在 101 之前就把第一帧
         *        发了过来，这些字节必须先喂给解码器
         */
        template<typename Socket>
        Core::Task<> webSocketSessionStage(Socket &socket,
                                           Core::Connection &connection,
                                           const HttpServerLimits &limits,
                                           WebSocketHandler handler,
                                           std::vector<char> &receiveBuffer,
                                           std::size_t pendingLength)
        {
            // 帧发送路径：把一整帧按写超时约束写出去。写之前刷新截止时间的依据与 HTTP 阶段发送响应
            // 一致（HttpServerLimits::writeTimeout 约束的是「等待可写的最长空闲」，慢消费者防线）；
            // 回调按引用捕获 socket 与连接，两者都活到整条连接结束
            const auto sendFrameBytes = [&socket, &connection, &limits](const std::string_view frameBytes) -> Core::Task<bool>
            {
                connection.refreshIdleDeadline(limits.writeTimeout);
                try
                {
                    // 一直写到整帧出门：asyncSend 允许部分写，而帧少一个字节对端就再也找不回边界
                    std::size_t writtenLength = 0;
                    while (writtenLength < frameBytes.size())
                    {
                        const ssize_t writeLength =
                                co_await socket.asyncSend(frameBytes.data() + writtenLength, frameBytes.size() - writtenLength);
                        if (writeLength <= 0)
                        {
                            co_return false;
                        }
                        writtenLength += static_cast<std::size_t>(writeLength);
                    }
                    co_return true;
                } catch (const std::exception &)
                {
                    // 传输层写失败（对端 RST、描述符被清扫协程关掉）一律视为连接不可用：
                    // 字节流已断，这里没有可发给对端的东西
                    co_return false;
                }
            };

            WebSocketPeer peer(sendFrameBytes);

            bool isBusinessFinished = false;

            // 业务协程的包装：返回（正常或抛异常）之后置位标记。闭包按引用捕获上面那个局部，
            // 而业务协程帧的存活期短于本阶段，因此引用始终有效
            const auto runBusiness = [&isBusinessFinished](WebSocketHandler businessHandler, WebSocketPeer &businessPeer) -> Core::Task<>
            {
                try
                {
                    co_await businessHandler(businessPeer);
                } catch (const std::exception &exception)
                {
                    // 101 已经上线，此刻没有任何可以回给对端的东西：原因只能进日志
                    LOG_ERROR_FMT("WebSocket 会话：业务处理器抛出异常，已按连接不可用收口，原因：{}", exception.what());
                } catch (...)
                {
                    LOG_ERROR_FMT("WebSocket 会话：业务处理器抛出非标准异常（无 what() 描述），已按连接不可用收口");
                }
                isBusinessFinished = true;
            };

            // 启动业务：跑到首次 receive() 或首个 send*() 挂起为止（返回则标记已结束）
            Core::Task<> businessTask = runBusiness(std::move(handler), peer);
            businessTask.handle().resume();

            // 客户端可能在 101 之后立刻发帧：窗口里剩下的字节先喂给解码器，别把它丢了
            WebSocketFeedStatus feedStatus = WebSocketFeedStatus::Accepted;
            if (pendingLength > 0)
            {
                feedStatus = peer.feedBytes(receiveBuffer.data(), pendingLength);
            }

            // 读循环：读 → 喂解码器 → 交给业务。等下一段字节前按空闲容忍度计时，依据是
            // HttpServerLimits::idleTimeout 的语义「两次能收到字节之间的容忍时长」——WebSocket
            // 阶段没有「半条报文」这一相位，帧与帧之间的间隔就是这条连接的空闲
            while (feedStatus == WebSocketFeedStatus::Accepted && !isBusinessFinished && peer.isOpen() && connection.isAlive())
            {
                connection.refreshIdleDeadline(limits.idleTimeout);

                ssize_t receivedLength = 0;
                try
                {
                    receivedLength = co_await socket.asyncReceive(receiveBuffer.data(), receiveBuffer.size());
                } catch (const std::exception &)
                {
                    // 传输层读失败（对端 RST、描述符被清扫协程关掉）：字节流已断，按收口处理
                    break;
                }
                if (receivedLength <= 0)
                {
                    // 0 是对端正常关闭，负值是连接不可用，两者都只剩收尾
                    break;
                }

                // 一段字节一次喂完：解码器内部按帧推进，产出即排队交给业务
                feedStatus = peer.feedBytes(receiveBuffer.data(), static_cast<std::size_t>(receivedLength));
            }

            // 收尾（RFC 6455 §5.5.1）：任一侧收口都要结束会话，本侧尚未发起关闭且没有在途写时补一条
            // Close。业务仍有帧在写时不插这一条——两条写路径的字节会在连接上互相穿插，此刻直接结束
            // 会话（§7.1.7 允许服务端不等关闭握手就断开 TCP）
            if (feedStatus == WebSocketFeedStatus::DecodeError)
            {
                // 帧格式违规回 1002、文本负载非法回 1007、体量越界回 1009：三类失败对端的处置不同，
                // 不能合成一个码。具体是哪一类由 WebSocketPeer 按失败来源给出，会话只负责发出与收口
                const std::uint16_t errorCode = peer.decodeErrorCloseCode();
                LOG_ERROR_FMT("WebSocket 会话：对端违反 RFC 6455，按状态码 {} 关闭连接，原因：{}", errorCode, peer.decodeErrorText());
                if (peer.isOpen() && !peer.isWriteInFlight())
                {
                    [[maybe_unused]] const bool isErrorCloseSent = co_await peer.close(errorCode);
                }
            } else if (peer.isOpen() && !peer.isWriteInFlight())
            {
                // 业务返回、对端关闭或读超时：本侧主动发起正常关闭
                [[maybe_unused]] const bool isNormalCloseSent = co_await peer.close(kWebSocketNormalClosureCode);
            }

            // 标记收口：此后 peer 不再交付消息、send*() 一律返回 false。挂起中的业务协程不唤醒，
            // 它的帧随本协程一起销毁（会话已经结束，让它继续跑没有意义）
            peer.markClosed();
            co_return;
        }

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
         * @note 超时判定不在本协程里做（清扫协程关掉连接后本帧可能立刻销毁，挂起的定时等待会
         *       指向已释放的帧）：这里只按相位把时限刷进 connection 的空闲截止时间，
         *       到点关连接由 TcpServer 的清扫协程负责。
         *
         * @tparam Socket 传输层类型，需支持 asyncReceive/asyncSend
         * @param socket        传输层 socket 引用
         * @param cancelable    所属连接的取消源，用于把连接级停止转成本请求的协作式取消
         * @param router        路由器
         * @param parser        HTTP 增量解析器，由会话持有
         * @param receiveBuffer 跨次读取存续的接收缓冲，由会话持有
         * @param isAlive       连接存活谓词，每轮事务与每次挂起前检查
         * @param connection    所属连接，用于按相位刷新空闲截止时间并维护在途工作标记；其生命周期必须覆盖整个循环
         * @param limits        连接级限额，取自 HttpServerLimits；0 字段表示关闭对应项保护
         * @param metrics       统计采集端，可为空；为空时请求计数、状态码分类与延迟直方图都不更新
         * @param requestIdGenerator request-id 生成器，可为空；为空时不为请求落定 request-id，
         *                          响应也不带 x-request-id（HTTPS 会话当前即走这条路）
         */
        template<typename Socket>
        Core::Task<> httpKeepAliveLoop(Socket &socket,
                                       Core::Cancelable &cancelable,
                                       Router &router,
                                       HttpParser &parser,
                                       std::vector<char> &receiveBuffer,
                                       const std::function<bool()> &isAlive,
                                       Core::Connection &connection,
                                       const HttpServerLimits &limits,
                                       HttpMetricsCollector *metrics = nullptr,
                                       const HttpRequestIdGenerator *requestIdGenerator = nullptr)
        {
            // 接收窗口里尚未交给解析器的字节数。窗口只用来「接住刚到的字节」：正文由解析器
            // 边收边存，跨读的半行也由解析器自己拼，因此这里永远是「窗口开头的一段」，
            // 解析器消费多少就把后面剩的挪到开头，不需要按报文体量扩容
            std::size_t windowLength = 0;

            bool keepAlive = true;

            // 本连接已服务的请求条数：达到 maximumRequestsPerConnection 后回完当前响应即收口，
            // 避免同一客户端长期占着一条连接不放
            std::size_t servedRequestCount = 0;

            // 解析器里是否已攒着半条请求：读超时只在「本请求已经开始」之后才顶替空闲容忍度，
            // 否则每轮等待都会把截止时间刷回更宽松的空闲值，读超时形同虚设
            bool isRequestInProgress = false;

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
            const auto sendResponse = [&socket, &connection, &limits](const std::string_view head, const std::string_view body) -> Core::Task<bool>
            {
                // 发送前把截止时间刷成写超时：对端只连不读（慢消费者）时写侧会一直挂起，
                // 超过容忍度就由清扫协程收口，而不是把连接永远挂在发送上。
                // 时限为 0 时这里等于清除截止时间，连接退回「不受写超时约束」
                connection.refreshIdleDeadline(limits.writeTimeout);

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

            // 流式响应的发送回调：把一段字节直接写到这条连接。复用上面那条分段/聚合写路径，
            // 因此 writeChunk 的每一段都是当场流出去的，不存在「先攒在内存里再整块发」的中间态；
            // 写之前的空闲截止时间刷新也在 sendResponse 里，流式期间的长写同样受写超时约束。
            // 回调引用本协程帧里的 socket 与连接，它们活到整条连接结束，故按连接装配一次即可
            const auto sendChunkSegment = [&sendResponse](const std::string_view segment) -> Core::Task<bool>
            {
                co_return co_await sendResponse(segment, std::string_view{});
            };

            // 装配给按连接复用的响应对象：它绑定的是这条连接而不是某一条报文，因此装一次就够，
            // reset() 也不会把它清掉（只清流式模式标记）。装配必然早于任何一次路由
            response.setChunkSender(sendChunkSegment);

            // 把异常的指针取成可读文本：流式响应中途失败时头部已经上线，改状态码已不可能，
            // 日志是唯一能交代原因的地方
            const auto describeException = [](const std::exception_ptr &exceptionPointer) -> std::string
            {
                if (!exceptionPointer)
                {
                    return {};
                }

                std::string description;
                try
                {
                    std::rethrow_exception(exceptionPointer);
                } catch (const std::exception &exception)
                {
                    description = exception.what();
                } catch (...)
                {
                    // 非标准异常没有 what()：给一句中文占位，好过把它当成「没有异常」
                    description = "非标准异常（无 what() 描述）";
                }
                return description;
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
                    // 相位决定用哪个时限：本请求已经开始（解析器里攒着半条报文）就按读超时约束，
                    // 只有「等一条新请求的第一个字节」才用 keep-alive 空闲容忍度
                    connection.refreshIdleDeadline(isRequestInProgress ? limits.readTimeout : limits.idleTimeout);

                    const ssize_t receivedLength = co_await readIntoWindow();
                    if (receivedLength <= 0)
                    {
                        // 对端正常关闭（0）或连接不可用（负值）：半截报文不值得回包，直接结束会话
                        co_return;
                    }
                    windowLength = static_cast<std::size_t>(receivedLength);
                    isRequestInProgress = true;

                    // 读到字节即重新计时：慢速攻击是把一条请求拆成很多次缓慢的写入，
                    // 因此这一项约束的是「相邻两次成功读取」的间隔，而不是整条请求的读总时长
                    connection.refreshIdleDeadline(limits.readTimeout);
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
                    // 失败类别映射：形态合法只是体量越界回 431/413，其余回 400。
                    // 这条路径有意不置 busy：报文没解析成功，没有在途业务要等，回完 4xx 即收口
                    HttpResponse errorResponse;
                    writeParseErrorResponse(errorResponse, parser.errorKind());

                    // 解析失败/协议错误收口单独计数，不并入已处理的请求条数：
                    // 两类流量在监控上要能分开看（前者是客户端或攻击，后者是正常业务）
                    if (metrics != nullptr)
                    {
                        metrics->countBadRequest();
                    }

                    co_await sendResponse(errorResponse.serializeHead(), errorResponse.body());

                    // 断开之前显式复位：把粘滞错误态与半成品请求一起清掉，
                    // 万一上层复用同一个解析器对象（例如把会话挪作他用），也不会读到脏请求
                    parser.reset();
                    co_return;
                }

                // 已收到完整请求：从这里到响应发完算「在途工作」，优雅关闭据此只等真正的在途请求，
                // 而不是把空闲 keep-alive 连接也一并等满期限。任何提前 co_return 与异常展开
                // 都会走到守卫析构，标记不会停留在「忙碌」上
                const BusyScope busyScope(connection);

                // 统计口径以「收到完整请求」为界：耗时从这里算到响应发完，条数在这里累加；
                // 时钟取一次、本轮复用，避免起止两点各取一次时钟引入偏差
                const std::chrono::steady_clock::time_point requestReceivedTime = std::chrono::steady_clock::now();
                if (metrics != nullptr)
                {
                    metrics->countParsedRequest();
                }

                // ---------------- 第三步：路由与应答 ----------------
                HttpRequest &request = parser.request();

                // request-id 在进入业务之前落定：中间件、业务与日志读到的都是同一个值。
                // 生成器缺席时保持请求对象的空 id，业务侧读 requestId() 得到空串即为「未采集」
                if (requestIdGenerator != nullptr)
                {
                    request.setRequestId(requestIdGenerator->resolve(request));
                }

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

                // 取消转发器不在这里注销：它按连接注册一次（见循环前），
                // 回调指向解析器内部那个按连接复用的请求对象，跨请求依然指向正确目标

                // ---------------- 升级分支：把这条连接交给 WebSocket 处理器 ----------------
                // 业务抛异常时这次升级没有完成（可能只登记了一半），按普通 500 收口：reset() 会把
                // 升级意图一并清掉，因此下面只在业务正常返回时才认这个标记
                if (handlerException == nullptr && response.isWebSocketUpgradeRequested())
                {
                    std::string upgradeFailureReason;
                    if (!isWebSocketUpgradeRequest(request, &upgradeFailureReason))
                    {
                        // 登记了升级但请求并不构成合法握手：回 400 让对端知道原因，随后按 close 收口。
                        // 中文原因同时进正文与日志——正文对端未必有人看，日志才是排查入口
                        LOG_ERROR_FMT("HttpSession: WebSocket 升级请求不合法，已回 400 并收口连接。request-id {}，路径 {}，原因：{}",
                                      request.requestId(), request.uri(), upgradeFailureReason);
                        if (metrics != nullptr)
                        {
                            metrics->countBadRequest();
                        }

                        response.reset();
                        response.setStatus(400);
                        response.setBody(upgradeFailureReason);
                        response.setHeader("content-type", "text/plain; charset=utf-8");
                        response.setHeader("connection", "close");
                        [[maybe_unused]] const bool isRejectionSent = co_await sendResponse(response.serializeHead(), response.body());
                        co_return;
                    }

                    // 校验已保证这条头部存在；真取不到只可能是请求对象在校验之后被改动，
                    // 此时宁可收口，也不发一条 Accept 值算错的 101
                    const std::optional<std::string> clientKey = request.getHeader("sec-websocket-key");
                    if (!clientKey.has_value())
                    {
                        co_return;
                    }

                    // 101 报文由握手模块逐字节生成，这里原样写出：不走 HttpResponse 的序列化，
                    // 否则会被补上 date / content-length，而切换协议的应答里没有这两条的位置
                    const std::string handshakeResponse = buildHandshakeResponse(*clientKey);
                    if (!co_await sendResponse(handshakeResponse, std::string_view{}))
                    {
                        // 101 没发出去：对端拿不到 Sec-WebSocket-Accept，这条连接不能再当 WebSocket 用
                        co_return;
                    }

                    LOG_INFO_FMT("HttpSession: 连接已升级到 WebSocket。request-id {}，路径 {}", request.requestId(), request.uri());

                    // 此后不再回到 HTTP keep-alive：一条连接要么 HTTP 要么 WebSocket。
                    // windowLength 是 101 之前就到达的剩余字节（升级请求之后的那一部分），
                    // 客户端可能已经在里面发了第一帧，必须一并交给解码器。
                    // 整段 WebSocket 通话都算在途工作（上面的 BusyScope 覆盖到这里）：优雅关闭会等它结束
                    co_return co_await detail::webSocketSessionStage(socket, connection, limits, response.webSocketHandler(),
                                                                     receiveBuffer, windowLength);
                }

                // 计数与上限：达到上限就让 keepAlive 变 false，从而走既有的
                // 「补 Connection: close 并收口」逻辑，而不是另开一条收尾路径
                ++servedRequestCount;
                const bool isRequestLimitReached = limits.maximumRequestsPerConnection > 0 &&
                                                   servedRequestCount >= limits.maximumRequestsPerConnection;

                // 流式响应且头部已随首段正文上线（见 HttpResponse::writeChunk）：对端手里已经有
                // 状态行与头部，此刻既改不了状态码、也补不了 Connection: close，收尾只剩补终止块
                const bool isChunkedHeadSent = response.isChunkedResponse() && response.hasSentChunkedHead();
                const std::string_view requestIdView = request.requestId();

                if (isChunkedHeadSent)
                {
                    // chunked 的 0\r\n\r\n 本身就是消息边界，连接因此按 RFC 9112 §7.1 照常可以复用：
                    // 是否保活仍按 shouldKeepAlive() 判定（请求或业务显式写了 close 就收口），
                    // 不因为是流式响应就强行声明 close —— 那会把「消息边界」与「连接存续」混为一谈。
                    // 业务抛异常是唯一例外：正文只发了一半，复用这条连接会让下一条报文接在半成品之后
                    keepAlive = !handlerException && HttpSession::shouldKeepAlive(request, response) && !isRequestLimitReached;

                    if (handlerException)
                    {
                        // 头部已上线，任何「改 500」都是不可能的：对端已经按原状态码读到了状态行与
                        // 前半段正文，这里只能补终止块让它的消息收完整，并把原因记进日志
                        LOG_ERROR_FMT("HttpSession: 流式响应的业务处理中途抛出异常，头部已上线无法改写状态码，"
                                      "已补终止块并收口连接。request-id {}，路径 {}，原因：{}",
                                      requestIdView, request.uri(), describeException(handlerException));
                    }

                    // 头部已在 writeChunk 里上线，这里只补终止块，不再重复发头部
                    if (!co_await sendChunkSegment(kChunkedTerminator))
                    {
                        // 终止块没发出去：本条消息对端收不全，不计状态码类与延迟
                        co_return;
                    }
                } else
                {
                    // 版本号取一次：500 改写与 Connection 头判定都要按请求版本回填，不硬编码 1.1
                    const std::string &requestVersion = request.httpVersion();

                    if (handlerException)
                    {
                        // 业务异常必须整体重置响应再填 500：handler 可能已经写了一半头部与正文，
                        // 只改状态码会把半成品连同错的 content-length 一起发出去。
                        // 流式模式若尚未写出任何一段，同样落到这里——头部还没上线，改写是完全有效的
                        response.reset();
                        response.setStatus(500);
                        response.setBody("Internal Server Error");
                        response.setHeader("content-type", "text/plain");
                        if (!requestVersion.empty())
                        {
                            response.setHttpVersion(requestVersion);
                        }
                    }

                    keepAlive = HttpSession::shouldKeepAlive(request, response) && !isRequestLimitReached;

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

                    // 响应自动带本次请求的 request-id，与 date 同属「自动补齐」语义：调用方显式设过
                    // 就不覆盖（业务可能想把上游网关的 id 透传下去）。放在 500 改写之后，
                    // 保证异常路径上的响应同样能被日志检索对上
                    if (!requestIdView.empty() && !response.getHeader(std::string(kRequestIdHeaderName)).has_value())
                    {
                        response.setHeader(std::string(kRequestIdHeaderName), std::string(requestIdView));
                    }

                    // 头部序列化一次，跟随段一起提交（普通响应跟正文，流式响应跟终止块）：
                    // serializedHead 是具名局部，跟随段视图指向的 response 也活到本次调用之后
                    const std::string serializedHead = response.serializeHead();
                    const std::string_view trailingSegment = response.isChunkedResponse() ? kChunkedTerminator
                                                                                         : response.body();
                    if (!co_await sendResponse(serializedHead, trailingSegment))
                    {
                        // 发送失败：响应没有真正发出，因此不计状态码类与延迟——那会让统计把
                        // 「对端没收到」的请求算成已应答
                        co_return;
                    }
                }

                // 响应已发出：状态码类与本次耗时（「收到完整请求」到「响应发完」）一起落账。
                // 流式响应中途出异常时正文只发了一半，落账等于把半成品记成已应答（状态码也不是真实结果），
                // 因此跳过——那条路径已经由上面的错误日志交代
                const bool isTruncatedChunkedResponse = isChunkedHeadSent && static_cast<bool>(handlerException);
                if (!isTruncatedChunkedResponse)
                {
                    const std::chrono::steady_clock::duration requestElapsed = std::chrono::steady_clock::now() - requestReceivedTime;
                    if (metrics != nullptr)
                    {
                        metrics->recordResponse(response.status(), requestElapsed);
                    }

                    // 一条请求一条日志：request-id 同时出现在响应头与这里，客户端报的响应与服务端的
                    // 处理记录因此能按同一个键对齐（排查线上问题时先要 id 再要日志）
                    if (!requestIdView.empty())
                    {
                        LOG_INFO_FMT("HttpSession: 请求已完成。request-id {}，路径 {}，状态码 {}，耗时 {}us",
                                     requestIdView, request.uri(), response.status(),
                                     std::chrono::duration_cast<std::chrono::microseconds>(requestElapsed).count());
                    }
                }

                // 应答已发出，回到「等一条新请求」的相位：不刷新的话，下一次读之前
                // 连接的截止时间还停在写超时上，对端的读空闲会比配置的空闲容忍度更早被收口
                connection.refreshIdleDeadline(limits.idleTimeout);

                // ---------------- 第四步：为下一条报文复位 ----------------
                // 复位解析器（连带把请求对象重置成干净壳子）。窗口里若还有字节，那是流水线
                // 包进来的下一条报文，下一轮循环直接接着解析
                parser.reset();

                // 本条报文已服务完毕：下一条从零开始，等它的第一个字节时重新按空闲容忍度计时
                isRequestInProgress = false;
            }
        }
    } // namespace detail
} // namespace AsynGyanis::Net
