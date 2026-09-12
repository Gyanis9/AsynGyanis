// TestHttpServerLimits.cpp —— HTTP 连接级限额（HttpServerLimits + TcpServer 空闲清扫）的系统级覆盖：
//   一. 空闲超时：连上却不发任何字节的连接，在 idleTimeout + 清扫节拍内被服务端关闭；
//   二. 读超时：只发半条请求后静止，按 readTimeout 收口（idleTimeout 故意设得很长，证明起作用的是读超时）；
//   三. 单连接请求上限：达到上限的那条响应带 Connection: close，其后连接关闭、新请求不再被服务；
//   四. 写超时：**本机无法覆盖**——回环上把接收缓冲压到 8 KiB、响应体给到 8 MiB，整份响应仍在一次
//       WSASend 里发完（Windows 回环不制造部分写，此前实测过），构造不出确定性的「写阻塞」。
//       机制（发送前按 writeTimeout 刷新截止时间）仍在实现里，端到端验证只能在真实网络上做。
//   五. 拒绝面：清扫节拍设为 0 时，同一份空闲连接不再被超时收口。
// 用例全部走真实回环套接字（清扫协程在 TcpServer 内部），不依赖任何外部服务。

#include "Net/Http/HttpServer.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/Router.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Platform/System/PlatformError.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 一般等待上限：回环上的握手与一次清扫都在毫秒级完成；超时即判失败，不允许无界等待
        constexpr std::chrono::milliseconds kWaitTimeout{3000};

        /// 客户端单次读取的切片大小
        constexpr std::size_t kClientChunkLength = 16 * 1024;

        /**
         * @brief 在超时上限内逐毫秒轮询等待条件成立
         * @tparam Predicate 可调用对象，返回 bool
         * @param predicate 待轮询的条件
         * @param timeout 超时上限
         * @return true 条件在时限内成立
         */
        template<typename Predicate>
        bool waitForCondition(Predicate predicate, const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!predicate())
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }

        /**
         * @brief 在独立线程上驱动 EventLoop 的夹具
         * @details 必须晚于服务器协程任务构造：析构顺序保证「先 join 循环线程，再销毁协程帧与服务器」。
         */
        class EventLoopThread
        {
        public:
            explicit EventLoopThread(Core::EventLoop &loop) :
                m_loop(loop), m_worker([this]
                {
                    m_loop.run();
                })
            {
            }

            ~EventLoopThread()
            {
                m_loop.stop();
                if (m_worker.joinable())
                {
                    m_worker.join();
                }
            }

            EventLoopThread(const EventLoopThread &) = delete;
            EventLoopThread &operator=(const EventLoopThread &) = delete;

            /// 把协程投给事件循环线程执行
            void schedule(Core::Task<> &task)
            {
                m_loop.scheduler().scheduleRemote(task.handle());
            }

        private:
            Core::EventLoop &m_loop;  ///< 被执行的事件循环
            std::thread     m_worker; ///< 承载 run() 的线程
        };

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
        std::uint16_t queryBoundPort(const int descriptor)
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
        std::size_t countStatusLines(const std::string &responseText)
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
         * @brief 跑起一台真实 HttpServer 的夹具
         * @details 成员顺序即生命周期顺序：循环 → 结果槽 → 服务器 → 主协程任务 → 循环线程。
         *          析构体先让服务器收手（stop + 关闭全部连接），再按逆序 join 线程、销毁协程帧与服务器。
         */
        class RunningHttpServerFixture
        {
        public:
            /**
             * @brief 构造并启动服务器
             * @param limits 连接级限额
             * @param sweepInterval 空闲清扫节拍
             */
            RunningHttpServerFixture(const HttpServerLimits &limits, const std::chrono::milliseconds sweepInterval) :
                m_loop(),
                m_server(m_loop, Core::InetAddress::localhost(0)),
                m_serverTask(driveStart(m_server, m_startThrew)),
                m_loopThread(m_loop)
            {
                // 限额、清扫节拍与路由都必须在投递 start() 之前落定：清扫协程按 start() 那一刻的
                // 节拍投递，之后再改不会有清扫发生；路由同理，运行期改表生效时机不可预期
                m_server.setLimits(limits);
                m_server.setIdleCheckInterval(sweepInterval);
                m_server.router().get("/hello", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                {
                    response.setBody("served-hello");
                    co_return;
                });

                m_loopThread.schedule(m_serverTask);
            }

            ~RunningHttpServerFixture()
            {
                // 循环线程还活着时请服务器收手：stop() 关监听器、shutdown() 关掉全部活跃连接，
                // 会话与清扫协程因此能正常走完收尾，不会把协程帧留在 epoll 上
                m_server.close();
            }

            RunningHttpServerFixture(const RunningHttpServerFixture &) = delete;

            RunningHttpServerFixture &operator=(const RunningHttpServerFixture &) = delete;

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

        private:
            /**
             * @brief 把 start() 包一层，记录它是否抛异常
             * @param server 被测服务器
             * @param startThrew 输出：是否抛异常
             * @return Core::Task<> 协程，start() 返回后完成
             */
            static Core::Task<> driveStart(TestHttpServer &server, std::atomic<bool> &startThrew)
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

            Core::EventLoop    m_loop;       ///< 事件循环本体
            std::atomic<bool>  m_startThrew{false}; ///< start() 的退出方式，必须先于任务构造
            TestHttpServer     m_server;     ///< 被测服务器
            Core::Task<>       m_serverTask; ///< 由 driveStart 产生的主协程任务
            EventLoopThread    m_loopThread; ///< 承载 run() 的线程，最后构造、最先析构
        };

        /**
         * @brief 组装一条请求报文
         * @param requestLine 请求行原文，不含行尾 CRLF
         * @return std::string 以空行收尾的完整报文
         */
        std::string makeRequestText(const std::string_view requestLine)
        {
            std::string request(requestLine);
            request.append("\r\nhost: test\r\n\r\n");
            return request;
        }

        /// 一条完整的 GET 请求报文
        std::string helloRequestText()
        {
            return makeRequestText("GET /hello HTTP/1.1");
        }

        /// 半条 GET 请求：头部块没有收尾空行，解析器只会停在「还要更多字节」上
        std::string halfRequestText()
        {
            return std::string("GET /hello HTTP/1.1\r\nhost: test\r\n");
        }
    } // namespace

    TEST(HttpServerLimits, IdleKeepAliveConnectionIsClosedAfterIdleTimeout)
    {
        // 空闲超时：连接建立后一个字节都不发，服务端必须按 idleTimeout 收口。
        // 读超时与写超时故意设得很长，关掉它们才能证明收口来自空闲容忍度
        HttpServerLimits limits;
        limits.idleTimeout  = std::chrono::milliseconds{400};
        limits.readTimeout  = std::chrono::seconds{10};
        limits.writeTimeout = std::chrono::seconds{10};

        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{50});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";
        EXPECT_FALSE(fixture.startThrew());

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        // 反向对照：空闲容忍度之内不该被提前收口（否则下面的断言可能只是「连上就被关」）
        std::string receivedText;
        EXPECT_FALSE(client.waitForClosure(receivedText, std::chrono::milliseconds{200}))
                << "连接在空闲容忍度之内就被关闭：说明截止时间被设成了立即到期";

        EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout))
                << "空闲连接未被清扫协程收口：上界 kWaitTimeout（idleTimeout 400ms + 清扫节拍 50ms）";
        EXPECT_TRUE(receivedText.empty()) << "服务端在空闲连接上发了不该发的字节";

        // 会话随之退出：关闭动作确实回到了连接管理器，而不是只关了描述符
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";
    }

    TEST(HttpServerLimits, HalfSentRequestIsClosedAfterReadTimeout)
    {
        // 读超时：只发半条请求（缺收尾空行）后静止，必须按 readTimeout 收口。
        // idleTimeout 设成 10 秒：断言窗口内它不可能触发，收口只可能来自读超时
        HttpServerLimits limits;
        limits.idleTimeout  = std::chrono::seconds{10};
        limits.readTimeout  = std::chrono::milliseconds{300};
        limits.writeTimeout = std::chrono::seconds{10};

        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{30});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";
        ASSERT_TRUE(client.sendText(halfRequestText(), kWaitTimeout)) << "半条请求未能写入";

        // 反向对照：读超时之内不该被提前收口
        std::string receivedText;
        EXPECT_FALSE(client.waitForClosure(receivedText, std::chrono::milliseconds{100}))
                << "半条请求在读超时之内就被关闭";

        EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout))
                << "半条请求未被读超时收口：上界 kWaitTimeout（readTimeout 300ms + 清扫节拍 30ms）";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";
    }

    TEST(HttpServerLimits, KeepAliveCapClosesConnectionAfterMaximumRequests)
    {
        // 单连接请求上限：上限为 2 时，第 2 条响应就是「达到上限」的那条——它必须带
        // Connection: close，随后连接关闭；第 3 条请求因此再也拿不到响应。
        // 超时三项都设得很长：本用例只验证计数上限，不该被任何超时收口干扰
        HttpServerLimits limits;
        limits.idleTimeout                  = std::chrono::seconds{10};
        limits.readTimeout                  = std::chrono::seconds{10};
        limits.writeTimeout                 = std::chrono::seconds{10};
        limits.maximumRequestsPerConnection = 2;

        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        // 第 1 条：正常服务且保持连接（HTTP/1.1 默认保活，不该出现 connection 头）
        ASSERT_TRUE(client.sendText(helloRequestText(), kWaitTimeout));
        std::string receivedText;
        // 等到正文出现才断言头部：只数状态行会读到半截头部，那时「没有 connection 头」并不成立
        ASSERT_TRUE(client.waitForText(receivedText, "served-hello", kWaitTimeout)) << "第 1 条请求未得到完整响应";
        EXPECT_EQ(receivedText.find("connection:"), std::string::npos)
                << "还没到上限就把连接收口了：响应里出现了 connection 头";

        // 第 2 条：这是达到上限的那一条，响应必须显式声明 close
        ASSERT_TRUE(client.sendText(makeRequestText("GET /hello HTTP/1.1"), kWaitTimeout));
        EXPECT_TRUE(client.waitForText(receivedText, "connection: close", kWaitTimeout))
                << "达到请求上限的响应没有带 Connection: close";
        EXPECT_EQ(countStatusLines(receivedText), 2u) << "第 2 条请求没有得到响应";

        // 连接随后关闭：客户端读到 EOF 或 reset
        EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout)) << "达到上限后连接未关闭";

        // 第 3 条：连接已经在收口，写进去也不会再得到响应（写本身允许失败：对端已关闭）
        client.sendText(makeRequestText("GET /hello HTTP/1.1"), kWaitTimeout);
        EXPECT_FALSE(client.waitForStatusLines(receivedText, 3, std::chrono::milliseconds{300}))
                << "上限之后仍然服务了新请求";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";
    }

    TEST(HttpServerLimits, ZeroSweepIntervalDisablesIdleTimeout)
    {
        // 拒绝面：清扫节拍为 0 时不做连接级超时。同一份空闲连接在同样的观察到窗口内
        // 必须保持存活——它证明上面的收口确实来自清扫协程，而不是别处的关闭动作
        HttpServerLimits limits;
        limits.idleTimeout  = std::chrono::milliseconds{100};
        limits.readTimeout  = std::chrono::milliseconds{100};
        limits.writeTimeout = std::chrono::milliseconds{100};

        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{0});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        std::string receivedText;
        EXPECT_FALSE(client.waitForClosure(receivedText, std::chrono::milliseconds{1000}))
                << "清扫已按节拍 0 关闭，空闲连接却仍被收口";
        EXPECT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器在接受循环期间意外退出";
    }
} // namespace AsynGyanis::Net
