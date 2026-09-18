/**
 * @file HttpTestSupport.h
 * @brief HTTP 回环测试夹具：起一台真实 HttpServer + 裸 socket 客户端，供 Net 层端到端用例共用
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Log/LogEvent.h"
#include "Base/Log/Logger.h"
#include "Base/Log/LoggerRegistry.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Net/Http/HttpServer.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/Router.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Platform/System/PlatformError.h"

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 回环测试夹具的命名空间
     *
     * @details 端口由内核分配（localhost(0)），因此用例之间不共用任何固定端口，并行运行时
     *          也不会互相撞车；每个夹具实例自建自清，退出时先关服务器再销毁循环线程。
     * @note 非模板的自由函数都带 inline：本头文件会被同一个测试可执行文件的多个翻译单元包含。
     */
    namespace HttpTestSupport
    {
        /// 一般等待上限：回环上的握手与一次清扫都在毫秒级完成；超时即判失败，不允许无界等待
        constexpr std::chrono::milliseconds kWaitTimeout{3000};

        /// 客户端单次读取的切片大小
        constexpr std::size_t kClientChunkLength = 16 * 1024;

        /// drain 期限用例允许的超期余量：一个轮询节拍（TcpServer 内部 50ms）加上线程唤醒与观测误差
        constexpr std::chrono::milliseconds kDrainReturnSlack{500};

        /**
         * @brief 在超时上限内逐毫秒轮询等待条件成立（定义见 CoreTestSupport.h）
         * @note 本模块的调用点都自带超时，口径见各自的 kWaitTimeout
         */
        using AsynGyanis::Core::TestSupport::waitForCondition;

        /**
         * @brief 在独立线程上驱动 EventLoop 的夹具（定义见 CoreTestSupport.h，借用模式）
         * @note 必须晚于服务器协程任务构造：析构顺序保证「先 join 循环线程，再销毁协程帧与服务器」
         */
        using AsynGyanis::Core::TestSupport::EventLoopThread;

        /**
         * @brief 把基类的监听描述符与活跃连接数透出成只读访问器的测试服务器
         * @details 端口由内核分配，只能从监听描述符反查；活跃连接数是「会话是否真的退出」的判据。
         */
        class TestHttpServer final : public HttpServer
        {
        public:
            using HttpServer::HttpServer;

            /// 监听描述符
            [[nodiscard]] int listenDescriptor() const
            {
                return m_acceptor.fileDescriptor();
            }

            /// 当前挂在连接管理器上的活跃连接数
            [[nodiscard]] std::size_t activeConnectionCount() const
            {
                return m_connectionManager.activeCount();
            }
        };

        /**
         * @brief 查询描述符上由内核实际分配的本地端口
         * @param descriptor 监听描述符
         * @return std::uint16_t 实际端口，失败返回 0
         */
        inline std::uint16_t queryBoundPort(const int descriptor)
        {
            sockaddr_in address{};
            socklen_t   addressLength = static_cast<socklen_t>(sizeof(address));
            if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &addressLength) != 0)
            {
                return 0;
            }
            return ntohs(address.sin_port);
        }

        /**
         * @brief 统计响应文本里的状态行条数
         * @details 只认 "HTTP/1." 前缀的出现次数：响应正文不以 CRLF 收尾，
         *          下一条状态行会紧跟在上一条正文之后，按行首判定会漏计。
         * @param responseText 已收到的全部字节
         * @return std::size_t 状态行条数
         */
        inline std::size_t countStatusLines(const std::string &responseText)
        {
            constexpr std::string_view statusLinePrefix = "HTTP/1.";
            std::size_t                statusLineCount  = 0;

            for (std::size_t foundPosition = responseText.find(statusLinePrefix); foundPosition != std::string::npos;
                 foundPosition = responseText.find(statusLinePrefix, foundPosition + statusLinePrefix.size()))
            {
                ++statusLineCount;
            }
            return statusLineCount;
        }

        /**
         * @brief 捕获根日志器输出的记录型 Sink（发送失败类用例据此断言日志条数）
         *
         * @details 会话跑在事件循环线程上，被测代码用 LOG_ERROR_FMT 这类宏走全局根日志器，因此本类
         *          构造时把自己的 Sink 挂到根日志器上、析构时摘除，消息按到达顺序累积。
         *          写入（事件循环线程）与读取（用例线程）跨线程，记录容器自带互斥量。
         * @note 摘除用 clearSinks()：TestNet 不加载日志配置，根日志器上除本类挂的 Sink 外没有别的；
         *       用例在本进程内独占运行（gtest 发现机制把每个用例当独立进程启动），无需跨进程协调
         */
        class LogCapture
        {
        public:
            /**
             * @brief 挂上记录型 Sink，此后根日志器的输出都进入本对象
             */
            LogCapture() :
                m_records(std::make_shared<Records>())
            {
                // Sink 交给根日志器接管所有权；记录容器按 shared_ptr 共享，因此日志器稍后仍在写也安全
                Base::LoggerRegistry::instance().getRootLogger().addSink(std::make_unique<RecordingSink>(m_records));
            }

            /**
             * @brief 摘除自己的 Sink，停止捕获
             */
            ~LogCapture()
            {
                Base::LoggerRegistry::instance().getRootLogger().clearSinks();
            }

            LogCapture(const LogCapture &) = delete;

            LogCapture &operator=(const LogCapture &) = delete;

            /**
             * @brief 统计已记录消息里包含指定子串的条数
             * @param fragment 待查找的子串，如「响应写出失败」
             * @return std::size_t 命中条数
             */
            [[nodiscard]] std::size_t countContaining(const std::string_view fragment) const
            {
                const std::lock_guard<std::mutex> recordLock(m_records->mutex);
                std::size_t                       matchCount = 0;

                for (const std::string &message: m_records->messages)
                {
                    if (message.find(fragment) != std::string::npos)
                    {
                        ++matchCount;
                    }
                }
                return matchCount;
            }

        private:
            /**
             * @brief 全部已记录消息的共享容器（本对象与 Sink 各持一份引用）
             */
            struct Records
            {
                std::mutex               mutex;    ///< 保护 messages 的互斥量
                std::vector<std::string> messages; ///< 已记录的日志消息正文，按到达顺序
            };

            /**
             * @brief 把事件消息追加进共享容器的记录型 Sink
             */
            class RecordingSink final : public Base::LogSink
            {
            public:
                /**
                 * @brief 构造记录型 Sink
                 * @param records 与用例共享的记录容器
                 */
                explicit RecordingSink(std::shared_ptr<Records> records) :
                    m_records(std::move(records))
                {
                }

                /**
                 * @brief 记录一条日志事件的消息正文
                 * @param event 日志事件
                 */
                void write(const Base::LogEvent &event) override
                {
                    const std::lock_guard<std::mutex> recordLock(m_records->mutex);
                    m_records->messages.push_back(event.message);
                }

                /**
                 * @brief 内容全在内存里，没有缓冲需要落盘，故为空实现
                 */
                void flush() override
                {
                }

            private:
                std::shared_ptr<Records> m_records; ///< 与用例共享的记录容器
            };

            std::shared_ptr<Records> m_records; ///< 记录容器（Sink 被日志器接管后仍由本对象持有）
        };

        /**
         * @brief 一次非阻塞读取的结果
         * @details 「对端已断」与「暂时没数据」必须分开：前者是本次用例的断言目标，
         *          后者只说明还要继续等。
         */
        enum class ReadOutcome
        {
            Data,       ///< 读到字节
            PeerClosed, ///< 对端正常关闭（读到 0）
            Broken,     ///< 连接被重置或出错（错误码既不是「暂无数据」也不是「被中断」）
            Idle        ///< 暂无数据，可稍后重试
        };

        /**
         * @brief 一条到 127.0.0.1 指定端口的非阻塞客户端连接
         * @details 客户端刻意不用框架的 AsyncSocket：用例只需要「发字节、看对端什么时候断」，
         *          不希望再引入第二套事件循环。析构即关闭，不给用例留残余连接。
         */
        class LoopbackClient
        {
        public:
            /**
             * @brief 连接指定端口
             * @param port 服务端监听端口
             * @param receiveBufferLength 本端接收缓冲上限（字节），0 表示用系统默认值
             */
            explicit LoopbackClient(const std::uint16_t port, const int receiveBufferLength = 0)
            {
                m_descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                if (!Platform::FileDescriptor::isValid(m_descriptor))
                {
                    m_descriptor = Platform::FileDescriptor::kInvalid;
                    return;
                }

                // 必须在 connect 之前收窄接收缓冲：之后设置不会改变已经协商好的接收窗口，
                // 而「对端读得慢」正是写超时用例要构造的前提
                if (receiveBufferLength > 0)
                {
                    [[maybe_unused]] const int setResult = ::setsockopt(
                            m_descriptor, SOL_SOCKET, SO_RCVBUF,
                            reinterpret_cast<const char *>(&receiveBufferLength), static_cast<socklen_t>(sizeof(receiveBufferLength)));
                }

                sockaddr_in address{};
                address.sin_family      = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port        = htons(port);
                if (::connect(m_descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
                {
                    Platform::FileDescriptor::close(m_descriptor);
                    m_descriptor = Platform::FileDescriptor::kInvalid;
                    return;
                }

                // 连上之后转非阻塞：用例靠「读到什么」推进，不能把测试线程阻塞在读上
                Platform::FileDescriptor::setNonBlocking(m_descriptor);
            }

            ~LoopbackClient()
            {
                closeNow();
            }

            LoopbackClient(const LoopbackClient &) = delete;

            LoopbackClient &operator=(const LoopbackClient &) = delete;

            [[nodiscard]] bool isValid() const noexcept
            {
                return Platform::FileDescriptor::isValid(m_descriptor);
            }

            /**
             * @brief 把整段字节写出去
             * @details 用带 MSG_NOSIGNAL 的 send 而不是 FileDescriptor::write：用例会故意往
             *          一条已经被服务端收口的连接上补写字节，裸 write 会触发 SIGPIPE 打死测试进程。
             * @param payload 待发字节
             * @param timeout 写入等待上限，超时返回 false 让用例干净失败，绝不挂死测试线程
             * @return true 全部字节已被内核接收
             */
            bool sendText(const std::string_view payload, const std::chrono::milliseconds timeout) const
            {
                const auto  deadline      = std::chrono::steady_clock::now() + timeout;
                std::size_t writtenLength = 0;

                while (writtenLength < payload.size())
                {
                    const int sendLength = ::send(m_descriptor, payload.data() + writtenLength,
                                                  static_cast<int>(payload.size() - writtenLength), MSG_NOSIGNAL);
                    if (sendLength > 0)
                    {
                        writtenLength += static_cast<std::size_t>(sendLength);
                        continue;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return true;
            }

            /**
             * @brief 读一次，读到的字节追加到 accumulated
             * @param accumulated 输入输出：累计读到的字节
             * @return ReadOutcome 本次读取的结论
             */
            ReadOutcome readOnce(std::string &accumulated) const
            {
                std::array<char, kClientChunkLength> chunkStorage{};
                const ssize_t readLength = Platform::FileDescriptor::read(m_descriptor, chunkStorage.data(), chunkStorage.size());
                if (readLength > 0)
                {
                    accumulated.append(chunkStorage.data(), static_cast<std::size_t>(readLength));
                    return ReadOutcome::Data;
                }
                if (readLength == 0)
                {
                    return ReadOutcome::PeerClosed;
                }

                // -1 有两种含义：暂时没数据（继续等）与连接已断（用例的断言目标），必须分开
                const int errorCode = Platform::PlatformError::lastSocketErrorCode();
                if (errorCode == Platform::PlatformError::kWouldBlock || errorCode == Platform::PlatformError::kInterrupted)
                {
                    return ReadOutcome::Idle;
                }
                return ReadOutcome::Broken;
            }

            /**
             * @brief 轮询读直到观察到对端关闭或连接出错
             * @param accumulated 输入输出：累计读到的字节
             * @param timeout 等待上限
             * @return true 在时限内观察到连接已断
             */
            bool waitForClosure(std::string &accumulated, const std::chrono::milliseconds timeout) const
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (true)
                {
                    const ReadOutcome outcome = readOnce(accumulated);
                    if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                    {
                        return true;
                    }
                    if (outcome == ReadOutcome::Data)
                    {
                        continue;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            /**
             * @brief 轮询读直到累计出现指定条数的响应状态行
             * @param accumulated 输入输出：累计读到的字节
             * @param expectedStatusLineCount 期望出现几条 "HTTP/1."
             * @param timeout 等待上限
             * @return true 在时限内凑齐
             */
            bool waitForStatusLines(std::string &accumulated, const std::size_t expectedStatusLineCount,
                                    const std::chrono::milliseconds timeout) const
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (countStatusLines(accumulated) < expectedStatusLineCount)
                {
                    const ReadOutcome outcome = readOnce(accumulated);
                    if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                    {
                        // 对端已经断了，再等也不会多出状态行
                        return countStatusLines(accumulated) >= expectedStatusLineCount;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return countStatusLines(accumulated) >= expectedStatusLineCount;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return true;
            }

            /**
             * @brief 轮询读直到累计出现指定文本
             * @details 断言头部内容必须用本方法而不是「数到状态行就查」：状态行与后面的头部行
             *          可能分属两个 TCP 段，按状态行提前返回会读到半截头部。
             * @param accumulated 输入输出：累计读到的字节
             * @param expectedText 期望出现的文本
             * @param timeout 等待上限
             * @return true 在时限内出现
             */
            bool waitForText(std::string &accumulated, const std::string_view expectedText,
                             const std::chrono::milliseconds timeout) const
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (accumulated.find(expectedText) == std::string::npos)
                {
                    const ReadOutcome outcome = readOnce(accumulated);
                    if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                    {
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return accumulated.find(expectedText) != std::string::npos;
            }

            /// 关闭本端
            void closeNow() noexcept
            {
                Platform::FileDescriptor::close(m_descriptor);
                m_descriptor = Platform::FileDescriptor::kInvalid;
            }

        private:
            Platform::Socket::Initialization m_socketInitialization; ///< 保证 Winsock 在本对象存活期间保持初始化
            int                              m_descriptor{Platform::FileDescriptor::kInvalid}; ///< 客户端描述符
        };

        /**
         * @brief 一条完整响应：头部块与正文分开
         */
        struct ParsedResponse
        {
            std::string headers; ///< 含状态行与各头部行（不含结尾空行）
            std::string body;    ///< 正文原始字节
        };

        /**
         * @brief 从头部块里取 content-length
         * @param headers 头部块
         * @return std::size_t 长度；缺失或非法时为 0
         */
        inline std::size_t parseContentLength(const std::string_view headers)
        {
            std::string lowered(headers);
            for (char &character: lowered)
            {
                if (character >= 'A' && character <= 'Z')
                {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }

            const std::size_t position = lowered.find("content-length: ");
            if (position == std::string::npos)
            {
                return 0;
            }

            std::size_t value = 0;
            for (std::size_t index = position + std::string_view("content-length: ").size(); index < lowered.size(); ++index)
            {
                const char character = lowered[index];
                if (character < '0' || character > '9')
                {
                    break;
                }
                value = value * 10 + static_cast<std::size_t>(character - '0');
            }
            return value;
        }

        /**
         * @brief 发一条请求并读回完整响应（按 content-length 读满正文）
         * @details 一次调用就是一条独立连接：建连、发请求、读满、析构关闭。正文按头部声明的
         *          content-length 收齐才返回，因此文件与压缩正文这类大正文同样适用；
         *          分块响应（chunked）不收 content-length，本方法会读不齐而返回空值。
         * @param port 监听端口
         * @param requestText 请求报文
         * @param timeout 建连、发送与读取共用的等待上限
         * @return std::optional<ParsedResponse> 解析结果；读不全时为空
         */
        inline std::optional<ParsedResponse> sendAndReadResponse(const std::uint16_t port, const std::string &requestText,
                                                                 const std::chrono::milliseconds timeout = kWaitTimeout)
        {
            LoopbackClient client(port);
            if (!client.isValid())
            {
                return std::nullopt;
            }
            if (!client.sendText(requestText, timeout))
            {
                return std::nullopt;
            }

            std::string accumulated;
            const auto  deadline = std::chrono::steady_clock::now() + timeout;

            // 第一步：等头部块收尾
            while (accumulated.find("\r\n\r\n") == std::string::npos && std::chrono::steady_clock::now() < deadline)
            {
                if (client.readOnce(accumulated) == ReadOutcome::PeerClosed)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }

            const std::size_t headerEnd = accumulated.find("\r\n\r\n");
            if (headerEnd == std::string::npos)
            {
                return std::nullopt;
            }

            // 第二步：从 content-length 算出正文长度，读满为止
            const std::size_t contentLength = parseContentLength(accumulated.substr(0, headerEnd));
            const std::size_t expectedTotal = headerEnd + 4 + contentLength;
            while (accumulated.size() < expectedTotal && std::chrono::steady_clock::now() < deadline)
            {
                if (client.readOnce(accumulated) == ReadOutcome::PeerClosed)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            if (accumulated.size() < expectedTotal)
            {
                return std::nullopt;
            }

            ParsedResponse parsed;
            parsed.headers = accumulated.substr(0, headerEnd);
            parsed.body    = accumulated.substr(headerEnd + 4, contentLength);
            return parsed;
        }

        /// 头部块里是否出现某个（大小写不敏感的）头部行
        inline bool hasHeaderLine(const std::string_view headers, const std::string_view expectedLine)
        {
            std::string loweredHeaders(headers);
            std::string loweredLine(expectedLine);
            for (std::string *text: {&loweredHeaders, &loweredLine})
            {
                for (char &character: *text)
                {
                    if (character >= 'A' && character <= 'Z')
                    {
                        character = static_cast<char>(character - 'A' + 'a');
                    }
                }
            }
            return loweredHeaders.find(loweredLine) != std::string::npos;
        }

        /**
         * @brief 慢路由选项：用例用它构造「处理中」的在途请求
         * @details 处理函数只在第一次进入时置位 started 标记，用例先等到该标记再发起 drain，
         *          才能保证 drain 那一刻这条连接确实处在「有在途工作」的状态。
         */
        struct SlowRouteOptions
        {
            std::chrono::milliseconds processingTime{0};      ///< 该路由的处理耗时；非正数表示不注册这条路由
            std::atomic<bool>      *handlerStarted{nullptr};  ///< 处理函数进入时置位的标记，可空
        };

        /**
         * @brief 附加路由注册动作
         * @details 由用例提供、在投递 start() 之前执行一次，参数依次为路由器与承载它的事件循环
         *          （处理函数需要内建定时器时用它）。用它注册的路由与内置路由同批落定，
         *          因此不会出现「运行期改路由表」那种生效时机不可预期的状态。
         */
        using RouteRegistrar = std::function<void(Router &, Core::EventLoop &)>;

        /// 服务器启动前的最后一道配置动作：拿到服务器本体，用于落定 setLimits() 之外的开关
        /// （例如 setHttp2CleartextEnabled()）——这些开关同样必须在 start() 之前生效
        using ServerConfigurator = std::function<void(TestHttpServer &)>;

        /**
         * @brief 服务器夹具骨架：事件循环、被测服务器、主协程任务与承载线程
         *
         * @tparam ServerType 被测服务器类型（HttpServer / HttpsServer 的测试派生类）
         *
         * @details 成员顺序即生命周期顺序：循环 → 结果槽 → 服务器 → 主协程任务 → 循环线程；
         *          析构先等循环线程退出再收尾服务器。子类在构造体里做启动前配置（附加路由、
         *          端点开关、TLS 装配等），最后调用 startServer() 投递主协程。
         */
        template<typename ServerType>
        class RunningServerFixture
        {
        public:
            ~RunningServerFixture()
            {
                // 顺序要紧：先让循环线程停手并退出，再收尾服务器。挂起的等待器（accept 的事件
                // 注册、会话读等待、清扫协程的定时器登记）都活在循环内部的无锁结构里，而收尾会
                // 销毁这些协程帧；循环仍在跑时从本线程销毁它们，等于跨线程改动那些结构
                m_loopThread.join();
                m_server.close();
            }

            RunningServerFixture(const RunningServerFixture &)            = delete;
            RunningServerFixture &operator=(const RunningServerFixture &) = delete;

            /// 被测服务器本体：观测性用例据此读取统计快照（读取侧全是原子量或加锁接口，跨线程安全）
            [[nodiscard]] ServerType &server() noexcept
            {
                return m_server;
            }

            /// 服务器是否已进入接受循环
            [[nodiscard]] bool awaitRunning(const std::chrono::milliseconds timeout) const
            {
                return waitForCondition(
                        [this]
                        {
                            return m_server.isRunning();
                        },
                        timeout);
            }

            /// 活跃连接是否已全部退场
            [[nodiscard]] bool awaitConnectionsDrained(const std::chrono::milliseconds timeout) const
            {
                return waitForCondition(
                        [this]
                        {
                            return m_server.activeConnectionCount() == 0;
                        },
                        timeout);
            }

            /// 是否已有连接被挂上连接管理器（先确认它被接受，再断言它退场才有意义）
            [[nodiscard]] bool awaitConnectionAccepted(const std::chrono::milliseconds timeout) const
            {
                return waitForCondition(
                        [this]
                        {
                            return m_server.activeConnectionCount() >= 1;
                        },
                        timeout);
            }

            /// 内核实际分配的监听端口
            [[nodiscard]] std::uint16_t listeningPort() const
            {
                return queryBoundPort(m_server.listenDescriptor());
            }

            /// start() 是否以异常收场（用于把这台用例的失败与「配置没生效」区分开）
            [[nodiscard]] bool startThrew() const
            {
                return m_startThrew.load(std::memory_order_acquire);
            }

            /**
             * @brief 在循环线程上执行一段动作，并等它做完
             *
             * @details 会话、套接字与连接管理器都归事件循环所有：从测试线程直接读它们的字段，就是在与
             *          循环抢同一批句柄（IoWatcher 是无锁结构，空闲清扫还会随时收口连接）。要碰这些
             *          对象就走这条路；只读原子量或加锁快照的观测接口（见 server()）才可以跨线程直接读。
             */
            void runOnLoopAndWait(const std::function<void()> &action)
            {
                std::atomic<bool> isFinished{false};
                m_loop.scheduler().postRemote(
                        [&action, &isFinished]
                        {
                            action();
                            isFinished.store(true, std::memory_order_release);
                        });
                EXPECT_TRUE(waitForCondition([&isFinished] { return isFinished.load(std::memory_order_acquire); }, kWaitTimeout))
                        << "投递到循环线程的动作没有在时限内完成";
            }

        protected:
            /**
             * @brief 停止承载线程并 join
             *
             * @details 派生夹具若有**协程帧成员**（如 drain 任务），必须在自己的析构体里先调用本方法：
             *          派生成员的析构早于基类析构体，不先 join 的话循环线程可能正在恢复那些帧
             *          （TSan 在 Http2CleartextSession 的优雅关停用例上报的正是这一处）。
             */
            void joinLoopThread()
            {
                m_loopThread.join();
            }

            /**
             * @brief 构造骨架：用工厂造服务器（TLS 服务器的构造参数不止地址），落定通行配置
             * @tparam ServerFactory 由事件循环造出服务器的可调用对象
             * @param limits 连接级限额
             * @param sweepInterval 空闲清扫节拍
             * @param parserLimits 解析器资源上限
             * @param serverFactory 服务器工厂，形如 (Core::EventLoop &) -> ServerType
             */
            template<typename ServerFactory>
            RunningServerFixture(const HttpServerLimits &limits, const std::chrono::milliseconds sweepInterval,
                                 const HttpParserLimits &parserLimits, ServerFactory serverFactory) :
                m_loop(), m_server(serverFactory(m_loop)), m_serverTask(driveStart(m_server, m_startThrew)), m_loopThread(m_loop)
            {
                // 限额、清扫节拍与路由都必须在投递 start() 之前落定：清扫协程按 start() 那一刻的
                // 节拍投递，之后再改不会有清扫发生；路由同理，运行期改表生效时机不可预期
                m_server.setLimits(limits);
                m_server.setParserLimits(parserLimits);
                m_server.setIdleCheckInterval(sweepInterval);
                m_server.router().get("/hello", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                {
                    response.setBody("served-hello");
                    co_return;
                });
            }

            /// 投递主协程：子类做完全部启动前配置后调用，且只调用一次
            void startServer()
            {
                m_loopThread.schedule(m_serverTask);
            }

            /// 把一个任务投到承载线程上执行（任务对象须由调用方持有到完成）
            void scheduleOnLoopThread(Core::Task<> &task)
            {
                m_loopThread.schedule(task);
            }

            /// 承载服务器的事件循环（注册需要定时器的路由时用）
            [[nodiscard]] Core::EventLoop &loop() noexcept
            {
                return m_loop;
            }

        private:
            /**
             * @brief 把 start() 包一层，记录它是否抛异常
             * @param server 被测服务器
             * @param startThrew 输出：是否抛异常
             * @return Core::Task<> 协程，start() 返回后完成
             */
            static Core::Task<> driveStart(ServerType &server, std::atomic<bool> &startThrew)
            {
                try
                {
                    co_await server.start();
                } catch (...)
                {
                    // 只标记不抛出：用例据此断言「服务器没起来」而不是让测试进程带崩
                    startThrew.store(true, std::memory_order_release);
                }
                co_return;
            }

            Core::EventLoop   m_loop;              ///< 事件循环本体
            std::atomic<bool> m_startThrew{false}; ///< start() 的退出方式，必须先于任务构造
            ServerType        m_server;            ///< 被测服务器
            Core::Task<>      m_serverTask;        ///< 由 driveStart 产生的主协程任务
            EventLoopThread   m_loopThread;        ///< 承载 run() 的线程，最后构造、最先析构
        };

        /**
         * @brief 跑起一台真实 HttpServer 的夹具（骨架见 RunningServerFixture）
         */
        class RunningHttpServerFixture final : public RunningServerFixture<TestHttpServer>
        {
        public:
            /**
             * @brief 构造并启动服务器
             * @param limits 连接级限额
             * @param sweepInterval 空闲清扫节拍
             * @param slowRoute 可选的慢路由（处理耗时与进入标记）
             * @param registerRoutes 可选的附加路由注册动作，在投递 start() 之前执行
             * @param parserLimits 可选的解析器资源上限，在投递 start() 之前落定，只影响此后新建的会话
             * @param configureServer 可选的启动前配置动作（例如打开 h2c），同样在投递 start() 之前执行
             */
            RunningHttpServerFixture(const HttpServerLimits &limits, const std::chrono::milliseconds sweepInterval,
                                     const SlowRouteOptions &slowRoute = {}, const RouteRegistrar &registerRoutes = {},
                                     const HttpParserLimits &parserLimits = HttpParserLimits{},
                                     const ServerConfigurator &configureServer = {}) :
                RunningServerFixture<TestHttpServer>(
                        limits, sweepInterval, parserLimits,
                        [](Core::EventLoop &serverLoop)
                        {
                            return TestHttpServer(serverLoop, Core::InetAddress::localhost(0));
                        })
            {
                // 慢路由用定时等待模拟「处理中」：定时等待挂在事件循环上，因此 drain 与本请求都能照常推进，
                // 处理耗时越长，越能分辨「等完在途请求」与「等满死期限」
                if (slowRoute.processingTime > std::chrono::milliseconds::zero())
                {
                    Core::EventLoop &serverLoop = loop();
                    server().router().get("/slow",
                                          [&serverLoop, processingTime = slowRoute.processingTime, handlerStarted = slowRoute.handlerStarted](
                                                  HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        // 先置位再等待：用例据此确认此刻连接已被标记为「有在途工作」
                        if (handlerStarted != nullptr)
                        {
                            handlerStarted->store(true, std::memory_order_release);
                        }
                        Core::Timer processingTimer(serverLoop);
                        co_await processingTimer.waitFor(processingTime);
                        response.setBody("served-slow");
                        co_return;
                    });
                }

                // 附加路由：与内置路由同批落定，仍然在 start() 之前
                if (registerRoutes)
                {
                    registerRoutes(server().router(), loop());
                }

                // 启动前配置：与限额、路由同一时机，保证开关在第一个连接被接受之前就位
                if (configureServer)
                {
                    configureServer(server());
                }

                startServer();
            }

            ~RunningHttpServerFixture()
            {
                // drain 任务（协程帧成员）的析构早于基类析构体：先让循环线程停手并 join，
                // 否则它可能正在恢复那些帧（TSan 报过这一处数据竞争）
                joinLoopThread();
            }

            /**
             * @brief 把 drain() 投到循环线程并等它跑完
             * @details drain 只能在所属循环线程上运行：本方法按调度器投递，并把任务对象留在成员里
             *          活到跑完（协程帧必须有人持有）。完成标记每次调用先清空，可重复调用。
             * @param drainTimeout 交给 drain 的最长等待时长
             * @param waitTimeout 本方法自身的等待上限
             * @return true drain 在时限内完成
             */
            [[nodiscard]] bool drainServer(const std::chrono::milliseconds drainTimeout, const std::chrono::milliseconds waitTimeout)
            {
                m_drainFinished.store(false, std::memory_order_release);
                m_drainTask = driveDrain(server(), m_drainFinished, drainTimeout);
                scheduleOnLoopThread(m_drainTask);
                return waitForCondition(
                        [this]
                        {
                            return m_drainFinished.load(std::memory_order_acquire);
                        },
                        waitTimeout);
            }

        private:
            /**
             * @brief 把 drain() 包一层，跑完即置位完成标记
             * @param server 被测服务器
             * @param drainFinished 输出：drain 是否已返回
             * @param drainTimeout 交给 drain 的最长等待时长
             * @return Core::Task<> 协程，drain 返回后完成
             */
            static Core::Task<> driveDrain(TestHttpServer &server, std::atomic<bool> &drainFinished, const std::chrono::milliseconds drainTimeout)
            {
                co_await server.drain(drainTimeout);
                drainFinished.store(true, std::memory_order_release);
                co_return;
            }

            std::atomic<bool> m_drainFinished{false}; ///< drain 是否已返回，必须先于 drain 任务构造
            Core::Task<>      m_drainTask{nullptr};   ///< 由 driveDrain 产生的 drain 协程任务
        };

        /**
         * @brief 组装一条请求报文
         * @param requestLine 请求行原文，不含行尾 CRLF
         * @return std::string 以空行收尾的完整报文
         */
        inline std::string makeRequestText(const std::string_view requestLine)
        {
            std::string request(requestLine);
            request.append("\r\nhost: test\r\n\r\n");
            return request;
        }

        /**
         * @brief 组装一条带附加头部的请求报文
         * @param requestLine 请求行原文，不含行尾 CRLF
         * @param headerLines 附加头部行原文，每项不含行尾 CRLF；写在固定 host 头之前
         * @return std::string 以空行收尾的完整报文
         */
        inline std::string makeRequestText(const std::string_view requestLine, const std::vector<std::string> &headerLines)
        {
            std::string request(requestLine);
            request.append("\r\n");
            for (const std::string &headerLine: headerLines)
            {
                request.append(headerLine);
                request.append("\r\n");
            }
            request.append("host: test\r\n\r\n");
            return request;
        }

        /// 一条完整的 GET 请求报文
        inline std::string helloRequestText()
        {
            return makeRequestText("GET /hello HTTP/1.1");
        }

        /// 半条 GET 请求：头部块没有收尾空行，解析器只会停在「还要更多字节」上
        inline std::string halfRequestText()
        {
            return std::string("GET /hello HTTP/1.1\r\nhost: test\r\n");
        }
    } // namespace HttpTestSupport
} // namespace AsynGyanis::Net
