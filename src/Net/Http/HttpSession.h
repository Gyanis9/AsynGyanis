/**
 * @file HttpSession.h
 * @brief HTTP 会话：在单条 TCP 连接上做「定界—解析—路由—应答」的保持活跃循环
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Cancelable.h"
#include "Core/Coroutine/Task.h"
#include "Core/Socket/Connection.h"
#include "Net/Http/HttpParser.h"
#include "Net/Http/Router.h"

#include <algorithm>
#include <cstddef>
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
     *       缓冲区里，下一轮先喂进解析器，不会像早先那样每次固定读到缓冲区开头、
     *       把第二条请求静默丢掉。
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
         * @note 早先的形参里还有一个 `Core::EventLoop &loop`，实现中从未使用：事件循环已由
         *       AsyncSocket 内部持有，会话不需要第二份引用，故该参数已删除（见 Core::Connection
         *       的构造：它只要一个 socket）。
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
         * @details 判定顺序固定，且**请求侧的显式 close 不可被响应头反转**：
         *          @li 请求带 `Connection: close` → 一律断开（客户端明确指令，HTTP/1.1 与 1.0 同治）；
         *          @li 响应带 `Connection: close` → 一律断开（服务器侧主动收口，例如中间件降级）；
         *          @li 请求带 `Connection: keep-alive` → 保活；这条只对 HTTP/1.0 有实际意义，
         *              因为 1.0 默认逐请求断连，而 1.1 默认本就保活；
         *          @li 以上都没有时看版本：HTTP/1.1 及以上默认保活，HTTP/1.0 与无头部块的 HTTP/0.9 默认断开。
         *          响应里的 `Connection: keep-alive` 不参与判定——早先实现把它放在最后一步读取，
         *          于是「请求 close + 响应 keep-alive」会被反转成保活，已修正。
         *          同名头部的多个值按逗号拆分后逐 token 比对（`Connection: keep-alive, X` 这类写法合法）。
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
        std::vector<char> m_receiveBuffer; ///< 跨次读取存续的接收缓冲，容量按需在会话内增长
    };

    // ============================================================================
    // 会话共享实现：报文定界 + Keep-Alive 事务循环（HttpSession 与 HttpsSession 共用）
    // ============================================================================

    namespace detail
    {
        /// 接收缓冲初值，单位字节：8 KiB，与解析器「单行/单 URI 上限 8 KiB」同档，常规请求一次读取即可定界
        inline constexpr std::size_t kInitialReceiveBufferLength = 8ull * 1024;

        /**
         * @brief 请求头部块（含结尾空行）允许的最大字节数，超限回 431
         * @details 与 HttpParser 的头部块上限刻意同值：两处一旦不一致，较松的那处会先把超限字节
         *          交给较紧的那处，表现为「本应 431 却成了 400」。解析器那份是私有常量，
         *          改那里时必须同步改这里。
         */
        inline constexpr std::size_t kMaximumHeaderBlockLength = 64ull * 1024;

        /**
         * @brief 按 Content-Length 放行正文的上限，单位字节
         * @details 与解析器侧的正文上限对齐（8 MiB）。定界器按声明值提前拦截，是为了根本不去收
         *          这些字节；解析器那份是最后防线，管的是谎报长度的对端。
         */
        inline constexpr std::size_t kMaximumDeclaredBodyLength = 8ull * 1024 * 1024;

        /**
         * @brief 接收缓冲的硬上限，单位字节
         * @details 头部块上限再加一个初值大小的读切片：正常路径上定界器会先一步给出 431，
         *          这里只是兜住「定界器判定之前不再需要更多字节」的极端情形，避免无界增长。
         */
        inline constexpr std::size_t kMaximumReceiveBufferLength = kMaximumHeaderBlockLength + kInitialReceiveBufferLength;

        /// 单次发送的分界阈值，单位字节：不超过它就直接交给一次 asyncSend，省掉一个内层协程帧
        inline constexpr std::size_t kSmallResponseSendThreshold = 4ull * 1024;

        /**
         * @brief 一次报文定界的结论
         */
        enum class FrameOutcome
        {
            NeedMore,                   ///< 头部块还没收齐，需要继续读
            Complete,                   ///< 已定界，messageLength 可用
            HeaderBlockTooLarge,        ///< 头部块超出 kMaximumHeaderBlockLength → 431
            ContentLengthTooLarge,      ///< Content-Length 声明值超出上限 → 413
            ContentLengthInvalid,       ///< Content-Length 值非法、多值不一致，或与 chunked 并存 → 400
            ChunkedBodyNotSupported     ///< Transfer-Encoding: chunked 的请求体 → 411 Length Required
        };

        /**
         * @brief 定界结果：结论 + 头部块长度 + 整条报文长度
         */
        struct FramingResult
        {
            FrameOutcome outcome{FrameOutcome::NeedMore}; ///< 定界结论
            std::size_t headerBlockLength{0};             ///< 头部块字节数，含结尾空行
            std::size_t messageLength{0};                 ///< 整条报文（请求行 + 头部块 + 正文）字节数
        };

        /**
         * @brief 在喂解析器之前算出「这一条报文到哪里结束」
         *
         * @details 为什么非有这一步不可：HttpParser 不回报已消耗的字节数，llhttp 一旦在同一次
         *          调用里撞到下一条报文的开头，会把那个字节一并吞掉（解析器为此在 onMessageBegin
         *          里设了流水线守卫），上层就再也不知道边界在哪、也就无法把剩余字节留给下一条请求。
         *          因此由会话先算出边界，每次只喂「不超过本条报文结尾」的字节。
         *
         *          三种正文形态：
         *          @li 无 Content-Length 且非分块 → 报文在头部块结尾处完成（GET/HEAD/DELETE 一类）；
         *          @li 有 Content-Length → 头部块 + 声明长度；同一条报文里出现不一致的多个
         *              Content-Length 判为非法（请求走私的经典入口），一致则按该值处理；
         *          @li Transfer-Encoding: chunked → 回 411 Length Required。本框架不做分块请求体的
         *              帧定界：要在不整包缓冲的前提下切出边界，就得把定界器写成可续走的增量状态机，
         *              而解析器不回报消耗字节数这一前提让它无法与安全边界同时成立；
         *          @li 同时带 Content-Length 与 chunked → 一律判非法（400）：两者对「报文到哪结束」
         *              会给出不同答案，代理链上下游就此错位，正是 CL.TE / TE.CL 走私的形态。
         *
         * @param data   本条报文起点，指向接收缓冲中的有效区；length 为 0 时允许传空指针语义的地址
         * @param length 本条报文起点之后已可读的字节数
         * @return FramingResult 定界结果；除 NeedMore/Complete 外的结论都应回 4xx 后立即收口
         */
        FramingResult frameRequestMessage(const char *data, std::size_t length);

        /**
         * @brief 把定界失败结论翻译成要发的 4xx 响应（正文与原因短语全 ASCII）
         * @param response 待填充的响应对象，进入本函数时应当是新构造的
         * @param outcome 定界结论；Complete/NeedMore 不由本函数处理
         */
        void writeFramingErrorResponse(HttpResponse &response, FrameOutcome outcome);

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
         * @brief 把「连接的停止请求」转发成「本次请求的协作式取消」
         *
         * @details 取消机制有两处：Core::Connection 的 Cancelable 与 HttpRequest 自带的 stop_source。
         *          真值来源取前者（连接级、由服务器与运维路径驱动），后者只作为业务侧的读取出口：
         *          超时中间件调 request.requestCancel()，而连接被关停时由本对象把信号补进同一个出口，
         *          业务只需认 request.cancelToken() 一处。
         * @note 生命周期必须严格覆盖「本次请求被路由到」这一段：析构即注销回调，
         *       绝不允许活到解析器 reset() 之后，否则回调会写到下一条请求的取消源上。
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
         * @note 读侧没有超时：半截头部/正文会让这条协程一直挂在 epoll 上。框架目前只有
         *       每请求的 Core::Timer，没有连接级的空闲定时器池，加之前需先决定回收策略。
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
            // 有效数据恒为 [readOffset, usedLength)：交给解析器的字节就地前移游标（不搬内存），
            // 需要腾出读空间或一条报文收尾时，才把剩余字节整体搬回缓冲区开头
            std::size_t usedLength = 0;
            std::size_t readOffset = 0;

            // 本条报文的定界状态；未定界时 headerBlockLength/messageLength 均无意义
            bool isMessageFramed = false;
            std::size_t headerBlockLength = 0;
            std::size_t messageLength = 0;
            std::size_t fedLength = 0;

            bool keepAlive = true;

            // 前移有效区到缓冲区开头。注意 fedLength 不随之清零：它统计的是「本条报文已交给
            // 解析器多少字节」，与内存位置无关，报文结束复位时由下面第二步之后的分支统一归零
            const auto compactReceiveBuffer = [&readOffset, &receiveBuffer, &usedLength]()
            {
                if (readOffset == 0)
                {
                    return;
                }
                std::copy(receiveBuffer.begin() + static_cast<std::ptrdiff_t>(readOffset),
                          receiveBuffer.begin() + static_cast<std::ptrdiff_t>(usedLength),
                          receiveBuffer.begin());
                usedLength -= readOffset;
                readOffset = 0;
            };

            // 分批发完一段字节：asyncSend 允许部分写入，返回 0 或负值即连接不可用
            const auto sendAll = [&socket](const std::string_view data) -> Core::Task<bool>
            {
                std::size_t sentLength = 0;
                while (sentLength < data.size())
                {
                    const ssize_t writeLength = co_await socket.asyncSend(
                            data.data() + sentLength, data.size() - sentLength);
                    if (writeLength <= 0)
                    {
                        co_return false;
                    }
                    sentLength += static_cast<std::size_t>(writeLength);
                }
                co_return true;
            };

            // 发出响应：小响应走单次发送，省掉一个内层协程帧
            const auto sendResponse = [&sendAll, &socket](const std::string_view serializedResponse) -> Core::Task<bool>
            {
                if (serializedResponse.size() <= kSmallResponseSendThreshold)
                {
                    const ssize_t writeLength = co_await socket.asyncSend(serializedResponse.data(), serializedResponse.size());
                    co_return writeLength > 0;
                }
                co_return co_await sendAll(serializedResponse);
            };

            // 读一次网络字节并追加到有效区末尾。返回 0 表示对端正常关闭，负值表示连接不可用
            const auto readIntoBuffer = [&compactReceiveBuffer, &isAlive, &receiveBuffer, &socket, &usedLength](const std::size_t minimumExtraLength) -> Core::Task<ssize_t>
            {
                // 挂起前先复查存活：对端断开或被服务器强制关闭时不该再多读一次
                if (!isAlive())
                {
                    co_return -1;
                }

                // 先丢弃已经交给解析器的前段：不这么做，长正文会把缓冲区一路顶到整条报文的长度
                compactReceiveBuffer();

                const std::size_t requiredCapacity = usedLength + minimumExtraLength;
                if (requiredCapacity > receiveBuffer.size())
                {
                    // 几何增长：每次多要 8 KiB 会把「凑齐一个长头部」读成 O(n²) 次系统调用
                    std::size_t desiredCapacity = receiveBuffer.size() == 0 ? kInitialReceiveBufferLength : receiveBuffer.size();
                    while (desiredCapacity < requiredCapacity)
                    {
                        desiredCapacity *= 2;
                    }
                    // 硬上限兜底：定界器会在更早的位置给出 431，走到这里说明对端在灌无结尾的头部
                    if (desiredCapacity > kMaximumReceiveBufferLength)
                    {
                        desiredCapacity = kMaximumReceiveBufferLength;
                    }
                    if (desiredCapacity <= usedLength)
                    {
                        co_return -1;
                    }
                    receiveBuffer.resize(desiredCapacity);
                }

                const std::size_t writableLength = receiveBuffer.size() - usedLength;
                if (writableLength == 0)
                {
                    co_return -1;
                }

                try
                {
                    co_return co_await socket.asyncReceive(receiveBuffer.data() + usedLength, writableLength);
                } catch (const std::exception &)
                {
                    // 传输层读失败（对端 RST、描述符被 close() 关掉、TLS 记录错误）一律视为连接不可用：
                    // 字节流已经断了，这里没有任何可发给对端的东西，也无处可发
                    co_return -1;
                }
            };

            while (keepAlive && isAlive())
            {
                // ---------------- 第一步：定界。头部块没收齐就不会往下走 ----------------
                while (!isMessageFramed)
                {
                    const FramingResult framing = frameRequestMessage(receiveBuffer.data() + readOffset, usedLength - readOffset);

                    if (framing.outcome == FrameOutcome::Complete)
                    {
                        isMessageFramed   = true;
                        headerBlockLength = framing.headerBlockLength;
                        messageLength     = framing.messageLength;
                        break;
                    }

                    // 超限与非法在此收口：回完 4xx 就断连，不给对端继续灌字节的机会。
                    // 此时还没有可信的请求对象，协议版本只能按本服务器支持的 1.1 写
                    if (framing.outcome != FrameOutcome::NeedMore)
                    {
                        HttpResponse errorResponse;
                        writeFramingErrorResponse(errorResponse, framing.outcome);
                        errorResponse.setHeader("connection", "close");

                        // 本分支直接结束会话，因此不再回填 keepAlive：发完这一句就收口
                        co_await sendResponse(errorResponse.toString());
                        co_return;
                    }

                    // 已缓冲的字节数够到头部块上限却还没出现结尾空行：现在就收口。
                    // 不抢先判一下的话，下一步的读会因为「缓冲区再也长不大」返回连接不可用，
                    // 431 就退成了静默断连，客户端根本不知道是自己头部太长
                    if (usedLength - readOffset >= kMaximumHeaderBlockLength)
                    {
                        HttpResponse headerTooLargeResponse;
                        writeFramingErrorResponse(headerTooLargeResponse, FrameOutcome::HeaderBlockTooLarge);
                        headerTooLargeResponse.setHeader("connection", "close");

                        co_await sendResponse(headerTooLargeResponse.toString());
                        co_return;
                    }

                    const ssize_t receivedLength = co_await readIntoBuffer(kInitialReceiveBufferLength);
                    if (receivedLength <= 0)
                    {
                        // 对端正常关闭（0）或连接不可用（负值）：半截头部不值得回包，直接结束会话
                        co_return;
                    }
                    usedLength += static_cast<std::size_t>(receivedLength);
                }

                // ---------------- 第二步：按「不超过报文结尾」的切片喂给解析器 ----------------
                const std::size_t availableLength = usedLength - readOffset;
                if (availableLength == 0)
                {
                    // 缓冲区已被解析器吃空而报文还没结束：正文仍在路上，先去读
                    const ssize_t receivedLength = co_await readIntoBuffer(kInitialReceiveBufferLength);
                    if (receivedLength <= 0)
                    {
                        co_return;
                    }
                    usedLength += static_cast<std::size_t>(receivedLength);
                    continue;
                }

                std::size_t sliceLength = messageLength - fedLength;
                if (sliceLength > availableLength)
                {
                    sliceLength = availableLength;
                }

                const ParseStatus status = parser.parse(receiveBuffer.data() + readOffset, sliceLength);

                // 交出去的字节一律按已消费处理：llhttp 返回 HPE_OK 的定义就是「输入已全部吃进状态机」，
                // 而 Done/Error 之后的剩余字节由各分支处置，既不重算也不漏算
                readOffset += sliceLength;
                fedLength  += sliceLength;

                if (status == ParseStatus::NeedMore)
                {
                    // 喂完了定界器认定的整条报文却还没完成，只能是两者对边界的判断不一致
                    // （典型场景：客户端谎报 Content-Length 报小了）。此时留在缓冲区里的字节
                    // 归属不明，绝不能再当下一条报文解析
                    if (fedLength >= messageLength)
                    {
                        HttpResponse errorResponse;
                        errorResponse.setStatus(400);
                        errorResponse.setBody("Bad Request: Inconsistent Message Length");
                        errorResponse.setHeader("content-type", "text/plain");
                        errorResponse.setHeader("connection", "close");

                        co_await sendResponse(errorResponse.toString());
                        co_return;
                    }
                    continue;
                }

                if (status == ParseStatus::Error)
                {
                    // 出错即结束会话（不 reset 后接着复用同一条连接）。超限与报文非法分开回：
                    // 前者说明形态合法但体量越界（431/413），后者是根本读不懂（400）
                    HttpResponse errorResponse;
                    const bool isErrorInsideHeader = fedLength <= headerBlockLength;
                    if (parser.isLimitExceeded())
                    {
                        errorResponse.setStatus(isErrorInsideHeader ? 431 : 413);
                        errorResponse.setBody(isErrorInsideHeader ? "Request Header Fields Too Large" : "Payload Too Large");
                    } else
                    {
                        errorResponse.setStatus(400);
                        errorResponse.setBody("Bad Request");
                    }
                    errorResponse.setHeader("content-type", "text/plain");
                    errorResponse.setHeader("connection", "close");

                    co_await sendResponse(errorResponse.toString());

                    // 断开之前显式复位：把粘滞错误态与半成品请求一起清掉，
                    // 万一上层复用同一个解析器对象（例如把会话挪作他用），也不会读到脏请求
                    parser.reset();
                    co_return;
                }

                // ---------------- 第三步：路由与应答 ----------------
                HttpRequest &request = parser.request();
                HttpResponse response;
                response.setHttpVersion(request.httpVersion()); // 状态行版本跟随请求，不硬编码 1.1

                // 连接被关停时把它转成本次请求的协作式取消：业务只认 request.cancelToken() 一处
                std::optional<ConnectionCancelForwarder> cancelForwarder;
                cancelForwarder.emplace(cancelable, request);

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

                // 取消转发器在此注销：request 马上就要被下一条报文 reset() 复用，
                // 回调绝不能活过这个界线
                cancelForwarder.reset();

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

                const std::string serializedResponse = response.toString();
                if (!co_await sendResponse(serializedResponse))
                {
                    co_return;
                }

                // 定界长度与实际喂入长度不一致 = 客户端谎报了长度：多出来的字节归属不明，
                // 应答已经发出，就此收口，不再冒险把它当成下一条报文的开头
                if (fedLength != messageLength)
                {
                    co_return;
                }

                // ---------------- 第四步：为下一条报文复位 ----------------
                // 复位顺序固定：先清解析器（连带把请求对象重置成干净壳子），再清定界游标，
                // 最后压缩缓冲——此刻 [readOffset, usedLength) 里剩的正是一条流水线包进来的下一条报文
                parser.reset();
                isMessageFramed   = false;
                headerBlockLength = 0;
                messageLength     = 0;
                fedLength         = 0;

                compactReceiveBuffer();
            }
        }
    } // namespace detail
} // namespace AsynGyanis::Net
