// TestHttpsServer.cpp —— HTTPS 侧端到端覆盖（真实 TLS 回环 + 仓库自签证书）：
//   一. 请求服务与统计：一条 HTTPS 请求得到 200 与正确正文，totalRequestCount 与 2xx 计数各 +1；
//   二. request-id：响应带形态合法的 x-request-id，客户端自带的合法值被原样回显；
//   三. 空闲超时：短 idleTimeout + 短清扫节拍下，TLS 连接被清扫协程收口并计入 timeoutClosedCount；
//   四. 前缀隔离：HTTP 与 HTTPS 各自的 request-id 前缀不相等。
// 夹具在本文件内自建（TestHttpsServer + TlsLoopbackClient），回环端口由内核分配，用例之间不共用端口。

#include "Net/Http/HttpsServer.h"
#include "Net/Http/HttpServerStats.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/Router.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/Platform.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        /// 仓库内预生成的自签测试证书（CN=asyngyanis-test，有效期至 2036）
        const std::filesystem::path kTestCertificatePath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_cert.pem";

        /// 仓库内预生成的配套私钥
        const std::filesystem::path kTestKeyPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_key.pem";

        /// 生成的 request-id 形态：4 位十六进制前缀 + '-' + 16 位十六进制序号（见 HttpRequestId.h）
        constexpr std::size_t kGeneratedRequestIdLength = 4 + 1 + 16;

        /// 生成形态里分隔前缀与序号的位置
        constexpr std::size_t kGeneratedRequestIdSeparatorIndex = 4;

        /// request-id 前缀的长度（十六进制服务器序号）
        constexpr std::size_t kRequestIdPrefixLength = 4;

        /// request-id 响应头的行内前缀（响应头名前带 CRLF，避免误命中正文里的同名文本）
        constexpr std::string_view kRequestIdHeaderLinePrefix = "\r\nx-request-id: ";

        /// SSL_CTX 释放器：断言提前返回时客户端上下文也不会泄漏
        struct SslContextDeleter
        {
            void operator()(SSL_CTX *context) const noexcept
            {
                SSL_CTX_free(context);
            }
        };

        /// SSL 对象释放器
        struct SslDeleter
        {
            void operator()(SSL *ssl) const noexcept
            {
                SSL_free(ssl);
            }
        };

        /**
         * @brief 观测性/超时用例共用的限额：超时三项都设得很长
         * @details 只有空闲超时用例会覆写 idleTimeout；其余用例不该被清扫在中途收口连接。
         * @return HttpServerLimits 关掉超时保护的配置
         */
        HttpServerLimits makeLongTimeoutLimits()
        {
            HttpServerLimits limits;
            limits.idleTimeout  = std::chrono::seconds{10};
            limits.readTimeout  = std::chrono::seconds{10};
            limits.writeTimeout = std::chrono::seconds{10};
            return limits;
        }

        /**
         * @brief 判断一段文本是否是服务器生成的 request-id
         * @param requestId 待判定的标识
         * @return true 长度符合约定，且分隔符位置与其余字符都是小写十六进制
         */
        bool looksLikeGeneratedRequestId(const std::string_view requestId)
        {
            if (requestId.size() != kGeneratedRequestIdLength || requestId[kGeneratedRequestIdSeparatorIndex] != '-')
            {
                return false;
            }

            for (std::size_t index = 0; index < requestId.size(); ++index)
            {
                if (index == kGeneratedRequestIdSeparatorIndex)
                {
                    continue;
                }
                const char character = requestId[index];
                const bool isLowerHexDigit = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
                if (!isLowerHexDigit)
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 统计一段文本里子串出现的次数
         * @param text 待搜索文本
         * @param needle 目标子串
         * @return std::size_t 出现次数
         */
        std::size_t countTextOccurrences(const std::string &text, const std::string_view needle)
        {
            std::size_t occurrenceCount = 0;
            for (std::size_t foundPosition = text.find(needle); foundPosition != std::string::npos;
                 foundPosition = text.find(needle, foundPosition + needle.size()))
            {
                ++occurrenceCount;
            }
            return occurrenceCount;
        }

        /**
         * @brief 取响应文本里第 occurrence 个 x-request-id 头的值
         * @param responseText 已收到的全部字节
         * @param occurrence 第几个（从 0 开始）
         * @return std::string 头部值；不存在时返回空串
         */
        std::string requestIdHeaderAt(const std::string &responseText, const std::size_t occurrence)
        {
            std::size_t foundPosition = responseText.find(kRequestIdHeaderLinePrefix);
            for (std::size_t skippedCount = 0; foundPosition != std::string::npos; ++skippedCount)
            {
                if (skippedCount == occurrence)
                {
                    const std::size_t valueStart = foundPosition + kRequestIdHeaderLinePrefix.size();
                    const std::size_t lineEnd    = responseText.find("\r\n", valueStart);
                    if (lineEnd == std::string::npos)
                    {
                        return {};
                    }
                    return responseText.substr(valueStart, lineEnd - valueStart);
                }
                foundPosition = responseText.find(kRequestIdHeaderLinePrefix, foundPosition + kRequestIdHeaderLinePrefix.size());
            }
            return {};
        }

        /**
         * @brief 一条到 127.0.0.1 指定端口的 TLS 客户端
         *
         * @details 客户端刻意不用框架的 TlsSocket/AsyncSocket：用例只需要「握手、发字节、看对端什么时候断」，
         *          再引入一套事件循环无助于验证服务端行为。套接字保持非阻塞，握手与读写都用
         *          「SSL_xxx 返回 WANT_* 就轮询重试」的方式推进，因此任何一步都不会把测试线程挂死。
         * @note 客户端不校验服务端证书：仓库夹具是自签证书，用例要验证的是加密通道与协议行为本身。
         */
        class TlsLoopbackClient
        {
        public:
            /**
             * @brief 连接并完成 TLS 握手
             * @param port 服务端监听端口
             */
            explicit TlsLoopbackClient(const std::uint16_t port)
            {
                m_context.reset(SSL_CTX_new(TLS_client_method()));
                if (m_context == nullptr)
                {
                    return;
                }

                m_descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                if (!Platform::FileDescriptor::isValid(m_descriptor))
                {
                    m_descriptor = Platform::FileDescriptor::kInvalid;
                    return;
                }

                sockaddr_in address{};
                address.sin_family      = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port        = htons(port);
                if (::connect(m_descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
                {
                    closeNow();
                    return;
                }

                // 转非阻塞：握手与读写靠轮询推进，用例不会因为对端不响应而永久挂起
                Platform::FileDescriptor::setNonBlocking(m_descriptor);

                m_ssl.reset(SSL_new(m_context.get()));
                if (m_ssl == nullptr || SSL_set_fd(m_ssl.get(), m_descriptor) != 1)
                {
                    m_ssl.reset();
                    closeNow();
                    return;
                }

                m_handshakeDone = performHandshake(kWaitTimeout);
            }

            ~TlsLoopbackClient()
            {
                closeNow();
            }

            TlsLoopbackClient(const TlsLoopbackClient &) = delete;

            TlsLoopbackClient &operator=(const TlsLoopbackClient &) = delete;

            /// TLS 握手是否已完成
            [[nodiscard]] bool isHandshakeComplete() const noexcept
            {
                return m_handshakeDone;
            }

            /// 本端 TCP 端口：服务端在会话里看到的对端端口就是它
            [[nodiscard]] std::uint16_t localPort() const
            {
                return queryBoundPort(m_descriptor);
            }

            /**
             * @brief 把整段字节写出去
             * @param payload 待发字节
             * @param timeout 写入等待上限
             * @return true 全部字节已交给 OpenSSL
             */
            bool sendText(const std::string_view payload, const std::chrono::milliseconds timeout) const
            {
                if (!m_handshakeDone)
                {
                    return false;
                }

                const auto  deadline      = std::chrono::steady_clock::now() + timeout;
                std::size_t writtenLength = 0;

                while (writtenLength < payload.size())
                {
                    const int writeLength = SSL_write(m_ssl.get(), payload.data() + writtenLength,
                                                      static_cast<int>(payload.size() - writtenLength));
                    if (writeLength > 0)
                    {
                        writtenLength += static_cast<std::size_t>(writeLength);
                        continue;
                    }
                    const int errorCode = SSL_get_error(m_ssl.get(), writeLength);
                    if (errorCode != SSL_ERROR_WANT_READ && errorCode != SSL_ERROR_WANT_WRITE)
                    {
                        return false;
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
             * @return ReadOutcome 本次读取的结论；无数据可读时为 Idle，稍后重试即可
             */
            ReadOutcome readOnce(std::string &accumulated) const
            {
                if (!m_handshakeDone)
                {
                    return ReadOutcome::Broken;
                }

                std::array<char, kClientChunkLength> chunkStorage{};
                const int readLength = SSL_read(m_ssl.get(), chunkStorage.data(), static_cast<int>(chunkStorage.size()));
                if (readLength > 0)
                {
                    accumulated.append(chunkStorage.data(), static_cast<std::size_t>(readLength));
                    return ReadOutcome::Data;
                }

                const int errorCode = SSL_get_error(m_ssl.get(), readLength);
                if (errorCode == SSL_ERROR_ZERO_RETURN)
                {
                    return ReadOutcome::PeerClosed;
                }
                if (errorCode == SSL_ERROR_WANT_READ || errorCode == SSL_ERROR_WANT_WRITE)
                {
                    return ReadOutcome::Idle;
                }
                // 对端未发 close_notify 就断开时是 SYSCALL + 0：同样按「已关闭」而不是「出错」处理
                if (errorCode == SSL_ERROR_SYSCALL && readLength == 0)
                {
                    return ReadOutcome::PeerClosed;
                }
                return ReadOutcome::Broken;
            }

            /**
             * @brief 轮询读直到累计出现指定次数的标记文本
             * @param accumulated 输入输出：累计读到的字节
             * @param expectedText 标记文本
             * @param expectedCount 期望出现次数
             * @param timeout 等待上限
             * @return true 在时限内凑齐
             */
            bool waitForTextOccurrences(std::string &accumulated, const std::string_view expectedText,
                                        const std::size_t expectedCount, const std::chrono::milliseconds timeout) const
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (countTextOccurrences(accumulated, expectedText) < expectedCount)
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
                return countTextOccurrences(accumulated, expectedText) >= expectedCount;
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

            /// 关闭本端
            void closeNow() noexcept
            {
                // 先尽力发出 close_notify 再释放 SSL 对象：让服务端看到的是正常关闭
                if (m_ssl != nullptr)
                {
                    SSL_shutdown(m_ssl.get());
                    m_ssl.reset();
                }
                Platform::FileDescriptor::close(m_descriptor);
                m_descriptor = Platform::FileDescriptor::kInvalid;
            }

        private:
            /**
             * @brief 在时限内推进 SSL_connect 直到完成
             * @param timeout 握手等待上限
             * @return true 握手完成
             */
            bool performHandshake(const std::chrono::milliseconds timeout)
            {
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (true)
                {
                    const int result = SSL_connect(m_ssl.get());
                    if (result == 1)
                    {
                        return true;
                    }
                    const int errorCode = SSL_get_error(m_ssl.get(), result);
                    if (errorCode != SSL_ERROR_WANT_READ && errorCode != SSL_ERROR_WANT_WRITE)
                    {
                        return false;
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            std::unique_ptr<SSL_CTX, SslContextDeleter> m_context; ///< 客户端 TLS 上下文
            std::unique_ptr<SSL, SslDeleter>           m_ssl;     ///< 客户端 SSL 对象（关联 m_descriptor）
            int                                        m_descriptor{Platform::FileDescriptor::kInvalid}; ///< 底层 TCP 描述符
            bool                                       m_handshakeDone{false}; ///< TLS 握手是否已完成
            Platform::Socket::Initialization           m_socketInitialization; ///< 保证 Winsock 在本对象存活期间保持初始化
        };

        /**
         * @brief 把基类的监听描述符与活跃连接数透出成只读访问器的测试服务器
         * @details 端口由内核分配，只能从监听描述符反查；活跃连接数是「会话是否真的退出」的判据。
         */
        class TestHttpsServer final : public HttpsServer
        {
        public:
            using HttpsServer::HttpsServer;

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

            /// 当前挂在连接管理器上的活跃连接（取快照，供用例按基类接口观察会话）
            [[nodiscard]] std::vector<std::shared_ptr<Core::Connection> > activeConnections() const
            {
                return m_connectionManager.snapshot();
            }
        };

        /**
         * @brief 跑起一台真实 HttpsServer 的夹具
         * @details 成员顺序即生命周期顺序：循环 → 结果槽 → 服务器 → 主协程任务 → 循环线程。
         *          析构体先等循环线程退出，再收尾服务器，与 HTTP 侧夹具同一套顺序。
         */
        class RunningHttpsServerFixture
        {
        public:
            /**
             * @brief 构造并启动服务器
             * @param limits 连接级限额
             * @param sweepInterval 空闲清扫节拍
             * @param registerRoutes 可选的附加路由注册动作，在投递 start() 之前执行
             * @param parserLimits 可选的解析器资源上限，在投递 start() 之前落定，只影响此后新建的会话
             */
            RunningHttpsServerFixture(const HttpServerLimits &limits, const std::chrono::milliseconds sweepInterval,
                                      const RouteRegistrar &registerRoutes = {},
                                      const HttpParserLimits &parserLimits = HttpParserLimits{}) :
                m_loop(),
                m_server(m_loop, Core::InetAddress::localhost(0), kTestCertificatePath.string(), kTestKeyPath.string()),
                m_serverTask(driveStart(m_server, m_startThrew)),
                m_loopThread(m_loop)
            {
                // 限额、清扫节拍与路由都必须在投递 start() 之前落定，与 HTTP 夹具同一约束
                m_server.setLimits(limits);
                m_server.setParserLimits(parserLimits);
                m_server.setIdleCheckInterval(sweepInterval);
                m_server.router().get("/hello", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                {
                    response.setBody("served-hello");
                    co_return;
                });

                if (registerRoutes)
                {
                    registerRoutes(m_server.router(), m_loop);
                }

                m_loopThread.schedule(m_serverTask);
            }

            ~RunningHttpsServerFixture()
            {
                // 顺序要紧：先让循环线程停手并退出，再收尾服务器（收尾会销毁挂起的协程帧）
                m_loopThread.join();
                m_server.close();
            }

            RunningHttpsServerFixture(const RunningHttpsServerFixture &) = delete;

            RunningHttpsServerFixture &operator=(const RunningHttpsServerFixture &) = delete;

            /// 被测服务器本体：观测性用例据此读取统计快照
            [[nodiscard]] TestHttpsServer &server() noexcept
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
            static Core::Task<> driveStart(TestHttpsServer &server, std::atomic<bool> &startThrew)
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

            Core::EventLoop   m_loop;        ///< 事件循环本体
            std::atomic<bool> m_startThrew{false}; ///< start() 的退出方式，必须先于任务构造
            TestHttpsServer   m_server;      ///< 被测服务器
            Core::Task<>      m_serverTask;  ///< 由 driveStart 产生的主协程任务
            EventLoopThread   m_loopThread;  ///< 承载 run() 的线程，最后构造、最先析构
        };
    } // namespace

    /**
     * @brief 钉住：一条 HTTPS 请求得到 200 与正确正文，统计里的请求条数与状态码类计数各 +1
     */
    TEST(HttpsServer, ServesRequestAndCountsStats)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath))
                << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttpsServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环：上界 kWaitTimeout";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsLoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        std::string receivedText;
        ASSERT_TRUE(client.sendText(helloRequestText(), kWaitTimeout)) << "HTTPS 请求未能写入";
        ASSERT_TRUE(client.waitForTextOccurrences(receivedText, "served-hello", 1, kWaitTimeout))
                << "HTTPS 请求未得到完整响应";

        EXPECT_NE(receivedText.find("HTTP/1.1 200"), std::string::npos) << "响应状态行不是 200：「" << receivedText << "」";

        // 响应发出与计数落账之间隔着一次协程恢复，因此按条件轮询而不是立刻断言
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().stats().totalRequestCount >= 1;
                },
                kWaitTimeout)) << "统计未在时限内记下这条 HTTPS 请求";

        const HttpServerStats stats = fixture.server().stats();
        EXPECT_EQ(stats.totalRequestCount, 1u) << "累计请求条数不符（不含解析失败）";
        EXPECT_EQ(stats.status2xxCount, 1u) << "200 没有计入 2xx";
        EXPECT_EQ(stats.status1xxCount, 0u) << "1xx 计数应为 0";
        EXPECT_EQ(stats.status3xxCount, 0u) << "3xx 计数应为 0";
        EXPECT_EQ(stats.status4xxCount, 0u) << "4xx 计数应为 0";
        EXPECT_EQ(stats.status5xxCount, 0u) << "5xx 计数应为 0";
        EXPECT_EQ(stats.badRequestCount, 0u) << "正常报文被记成了协议错误";

        // 时序纪律：先等客户端读到关闭，再断言会话收口
        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "HTTPS 会话收口后未从连接管理器摘除";
        EXPECT_EQ(fixture.server().stats().activeConnectionCount, 0u) << "连接已关闭，活跃连接数没有回落";
    }

    /**
     * @brief 钉住：HTTPS 响应带形态合法的 x-request-id，且客户端自带的合法取值被原样回显
     * @details 两条请求走同一条 keep-alive TLS 连接：第一条不带 id 验证生成面，第二条带合法 id 验证采信面。
     */
    TEST(HttpsServer, GeneratesAndEchoesRequestId)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath))
                << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        RunningHttpsServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsLoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        // 第一条：不带 x-request-id，服务器应当生成一个形态合法的标识
        std::string receivedText;
        ASSERT_TRUE(client.sendText(helloRequestText(), kWaitTimeout)) << "第 1 条 HTTPS 请求未能写入";
        ASSERT_TRUE(client.waitForTextOccurrences(receivedText, "served-hello", 1, kWaitTimeout))
                << "第 1 条 HTTPS 请求未得到完整响应";

        const std::string generatedRequestId = requestIdHeaderAt(receivedText, 0);
        EXPECT_TRUE(looksLikeGeneratedRequestId(generatedRequestId))
                << "HTTPS 响应的 request-id 形态不符合约定（应为 4 位十六进制前缀 + '-' + 16 位十六进制序号）：「"
                << generatedRequestId << "」";

        // 第二条：带一个合法取值，服务器应当原样回显
        const std::string clientRequestId = "trace-id-from-client-42";
        ASSERT_TRUE(client.sendText(makeRequestText("GET /hello HTTP/1.1", {"x-request-id: " + clientRequestId}), kWaitTimeout))
                << "第 2 条 HTTPS 请求未能写入";
        ASSERT_TRUE(client.waitForTextOccurrences(receivedText, "served-hello", 2, kWaitTimeout))
                << "第 2 条 HTTPS 请求未得到完整响应";

        EXPECT_EQ(requestIdHeaderAt(receivedText, 1), clientRequestId) << "客户端自带的合法 request-id 没有被原样回显";
    }

    /**
     * @brief 钉住：TLS 连接建立后一个字节都不发，服务端按 idleTimeout 收口并计入 timeoutClosedCount
     *
     * @details 与 HTTP 侧的 IdleKeepAliveConnectionIsClosedAfterIdleTimeout 同一构造：短 idleTimeout
     *          配短清扫节拍，读/写超时故意设得很长，证明收口只可能来自空闲容忍度。容忍度之内先做一次
     *          反向对照，排除「连上就被关」这种假通过。
     */
    TEST(HttpsServer, ClosesIdleConnectionAndCountsTimeout)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath))
                << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        HttpServerLimits limits;
        limits.idleTimeout  = std::chrono::milliseconds{300};
        limits.readTimeout  = std::chrono::seconds{10};
        limits.writeTimeout = std::chrono::seconds{10};

        RunningHttpsServerFixture fixture(limits, std::chrono::milliseconds{30});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环：上界 kWaitTimeout";
        ASSERT_FALSE(fixture.startThrew()) << "HTTPS 服务器 start() 以异常收场";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        TlsLoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        // 会话报出的对端地址必须来自真实描述符：清扫协程关闭超时连接前要靠它写日志，
        // 取不到就会让整轮清扫中断（这正是本条用例钉住的失败路径）
        const std::vector<std::shared_ptr<Core::Connection> > activeConnections = fixture.server().activeConnections();
        ASSERT_EQ(activeConnections.size(), 1u) << "握手已完成，连接却不在连接管理器里";
        EXPECT_EQ(activeConnections.front()->remoteAddress(), "127.0.0.1:" + std::to_string(client.localPort()))
                << "HTTPS 会话没有报出真实对端地址：它去问了那条不持有描述符的占位套接字";

        // 反向对照：空闲容忍度之内不该被提前收口（否则下面的断言可能只是「连上就被关」）
        std::string receivedText;
        EXPECT_FALSE(client.waitForClosure(receivedText, std::chrono::milliseconds{100}))
                << "TLS 连接在空闲容忍度之内就被关闭：说明截止时间被设成了立即到期";

        EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout))
                << "空闲 TLS 连接未被清扫协程收口：上界 kWaitTimeout（idleTimeout 300ms + 清扫节拍 30ms）。"
                   "此刻仍挂在连接管理器上的连接数 "
                << fixture.server().activeConnectionCount();
        EXPECT_TRUE(receivedText.empty()) << "服务端在空闲连接上发了不该发的字节";

        // 上报与关闭必须同时发生：只关掉描述符而没有计入超时计数，说明清扫协程在收口链路上中途退出
        EXPECT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().stats().timeoutClosedCount >= 1;
                },
                kWaitTimeout)) << "被超时收口的连接没有计入 timeoutClosedCount";
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";
    }

    /**
     * @brief 钉住：HTTP 与 HTTPS 的 request-id 前缀不相等（同一进程内两边各用各的原子序号，避免 id 撞车）
     * @details 两侧各起一台真实服务器、各发一条请求，从响应头的 x-request-id 里取前缀比较。
     */
    TEST(HttpsServer, RequestIdPrefixDiffersFromHttpServer)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath))
                << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        // ---- HTTP 侧 ----
        RunningHttpServerFixture httpFixture(makeLongTimeoutLimits(), std::chrono::milliseconds{50});
        ASSERT_TRUE(httpFixture.awaitRunning(kWaitTimeout)) << "HTTP 服务器未在时限内进入接受循环";
        const std::uint16_t httpPort = httpFixture.listeningPort();
        ASSERT_NE(httpPort, 0);

        LoopbackClient httpClient(httpPort);
        ASSERT_TRUE(httpClient.isValid()) << "HTTP 回环连接失败";

        std::string httpReceivedText;
        ASSERT_TRUE(httpClient.sendText(helloRequestText(), kWaitTimeout)) << "HTTP 请求未能写入";
        ASSERT_TRUE(httpClient.waitForText(httpReceivedText, "served-hello", kWaitTimeout)) << "HTTP 请求未得到完整响应";

        const std::string httpRequestId = requestIdHeaderAt(httpReceivedText, 0);
        ASSERT_TRUE(looksLikeGeneratedRequestId(httpRequestId)) << "HTTP 侧 request-id 形态不符：「" << httpRequestId << "」";

        // ---- HTTPS 侧 ----
        RunningHttpsServerFixture httpsFixture(makeLongTimeoutLimits(), std::chrono::milliseconds{50});
        ASSERT_TRUE(httpsFixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环";
        const std::uint16_t httpsPort = httpsFixture.listeningPort();
        ASSERT_NE(httpsPort, 0);

        TlsLoopbackClient httpsClient(httpsPort);
        ASSERT_TRUE(httpsClient.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";

        std::string httpsReceivedText;
        ASSERT_TRUE(httpsClient.sendText(helloRequestText(), kWaitTimeout)) << "HTTPS 请求未能写入";
        ASSERT_TRUE(httpsClient.waitForTextOccurrences(httpsReceivedText, "served-hello", 1, kWaitTimeout))
                << "HTTPS 请求未得到完整响应";

        const std::string httpsRequestId = requestIdHeaderAt(httpsReceivedText, 0);
        ASSERT_TRUE(looksLikeGeneratedRequestId(httpsRequestId)) << "HTTPS 侧 request-id 形态不符：「" << httpsRequestId << "」";

        EXPECT_NE(httpRequestId.substr(0, kRequestIdPrefixLength), httpsRequestId.substr(0, kRequestIdPrefixLength))
                << "HTTP 与 HTTPS 用了同一个 request-id 前缀：「" << httpRequestId << "」与「" << httpsRequestId << "」";
    }

    /**
     * @brief 钉住：HTTPS 侧的解析上限与 HTTP 侧对称——头部值超限回 431 而不是 400，未超限的请求照常被服务
     */
    TEST(HttpsServer, Answers431WhenParserHeaderLimitIsSmall)
    {
        ASSERT_TRUE(std::filesystem::exists(kTestCertificatePath))
                << "缺少仓库自签证书夹具：" << kTestCertificatePath.string();

        HttpParserLimits parserLimits;
        parserLimits.maximumHeaderFieldValueLength = 32;

        // 连接级限额保持缺省（超时都很长），本用例只观察解析上限
        RunningHttpsServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100}, {}, parserLimits);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "HTTPS 服务器未在时限内进入接受循环：上界 kWaitTimeout";
        EXPECT_EQ(fixture.server().parserLimits().maximumHeaderFieldValueLength, 32u) << "落定的解析上限与传入值不符";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        // 越界的那一条：单个头部值 64 字节 > 32，必须回 431（形态合法、体量越界），不能退化成 400
        {
            TlsLoopbackClient client(listeningPort);
            ASSERT_TRUE(client.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";
            const std::string request = makeRequestText("GET /hello HTTP/1.1", {"x-blob: " + std::string(64, 'a')});
            ASSERT_TRUE(client.sendText(request, kWaitTimeout)) << "越界 HTTPS 请求未能写入";

            std::string receivedText;
            ASSERT_TRUE(client.waitForTextOccurrences(receivedText, "Request Header Fields Too Large", 1, kWaitTimeout))
                    << "越界头部未在时限内被判 431：上界 kWaitTimeout";
            EXPECT_NE(receivedText.find("HTTP/1.1 431"), std::string::npos) << receivedText;
            EXPECT_EQ(receivedText.find("HTTP/1.1 400"), std::string::npos) << "越界被当成了协议级非法：" << receivedText;
            EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout)) << "回完 431 没有收口";
        }

        // 同一台服务器上的下一条连接：未越界的请求照样拿到 200
        {
            TlsLoopbackClient normalClient(listeningPort);
            ASSERT_TRUE(normalClient.isHandshakeComplete()) << "TLS 回环握手未在时限内完成";
            ASSERT_TRUE(normalClient.sendText(helloRequestText(), kWaitTimeout)) << "正常 HTTPS 请求未能写入";

            std::string receivedText;
            ASSERT_TRUE(normalClient.waitForTextOccurrences(receivedText, "served-hello", 1, kWaitTimeout))
                    << "同一台服务器上未超限的请求未被正常服务：" << receivedText;
            EXPECT_NE(receivedText.find("HTTP/1.1 200"), std::string::npos) << receivedText;
        }
    }
} // namespace AsynGyanis::Net
