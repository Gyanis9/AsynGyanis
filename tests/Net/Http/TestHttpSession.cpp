/**
 * @file TestHttpSession.cpp
 * @brief HttpSession 单元测试：保持活跃判定、跨次读取的缓冲与流水线残留、定界超限应答与取消转发
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Http/HttpSession.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/SystemException.h"
#include "Core/Coroutine/Cancelable.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/Connection.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"
#include "Platform/IO/FileDescriptor.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 一般等待上限：描述符对上的本机读写与一次路由都在毫秒级完成；超时即判失败，不允许无界等待
        constexpr std::chrono::milliseconds kWaitTimeout{2000};

        /// 大报文（数 KB 以上）写入的等待上限，留足内核缓冲被会话排空的余量
        constexpr std::chrono::milliseconds kLargePayloadWaitTimeout{6000};

        /// 「断言某事不发生」时用的观察窗口
        constexpr std::chrono::milliseconds kNegativeCheckTimeout{200};

        /// 从对端逐次读取时用的切片缓冲大小
        constexpr std::size_t kPeerChunkLength = 4096;

        /// 会话协程的退出方式
        enum class FailureKind
        {
            None,             ///< 正常收口
            SystemException,  ///< 抛出 Base::SystemException
            BaseException,    ///< 抛出其它 Base::Exception
            UnknownException  ///< 抛出框架外的异常
        };

        /**
         * @brief 会话协程的结果槽
         * @details finished 以 release 语义发布，failure 只在 finished 为 true 后可读。
         */
        struct SessionOutcome
        {
            std::atomic<bool> finished{false};      ///< start() 协程是否已结束
            FailureKind failure{FailureKind::None}; ///< 结束方式
        };

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
         * @brief 全双工描述符对夹具
         * @details 一端交给 HttpSession（所有权随 AsyncSocket 转移，本对象不再关闭它），
         *          另一端由测试自己读写原始字节：不占端口、跨平台，而且能把一个「TCP 包」
         *          精确切成两次 write 来验证 NeedMore 与流水线残留字节。
         */
        class LoopbackDescriptorPair
        {
        public:
            LoopbackDescriptorPair()
            {
                m_creationSucceeded = Platform::FileDescriptor::createPair(m_sessionSide, m_peerSide);
                if (!m_creationSucceeded)
                {
                    m_sessionSide = Platform::FileDescriptor::kInvalid;
                    m_peerSide    = Platform::FileDescriptor::kInvalid;
                }
            }

            ~LoopbackDescriptorPair()
            {
                Platform::FileDescriptor::close(m_sessionSide);
                Platform::FileDescriptor::close(m_peerSide);
            }

            LoopbackDescriptorPair(const LoopbackDescriptorPair &) = delete;
            LoopbackDescriptorPair &operator=(const LoopbackDescriptorPair &) = delete;

            [[nodiscard]] bool isValid() const noexcept
            {
                return m_creationSucceeded;
            }

            /// 测试自己那一端：写请求字节、读响应字节
            [[nodiscard]] int peerSide() const noexcept
            {
                return m_peerSide;
            }

            /**
             * @brief 取走交给会话的一端，并放弃本夹具对它的关闭责任
             * @return int 描述符编号
             */
            int takeSessionSide() noexcept
            {
                return std::exchange(m_sessionSide, Platform::FileDescriptor::kInvalid);
            }

            /// 关闭测试自己那一端，用来模拟「客户端先断开」
            void closePeerSide() noexcept
            {
                Platform::FileDescriptor::close(m_peerSide);
                m_peerSide = Platform::FileDescriptor::kInvalid;
            }

        private:
            bool m_creationSucceeded{false};                                     ///< 配对是否成功
            int  m_sessionSide{Platform::FileDescriptor::kInvalid};              ///< 交给会话的一端
            int  m_peerSide{Platform::FileDescriptor::kInvalid};                 ///< 测试自己持有的一端
        };

        /**
         * @brief 把整段字节写入描述符
         * @details 描述符对是非阻塞的，一次 write 可能短写或返回 EAGAIN，故按截止时间重试；
         *          超时返回 false 让用例干净失败，绝不把测试线程挂死。
         * @return true 全部字节已被内核接收
         */
        bool writeFully(const int descriptor, const std::string_view payload, const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            std::size_t writtenLength = 0;

            while (writtenLength < payload.size())
            {
                const ssize_t writeLength = Platform::FileDescriptor::write(
                        descriptor, payload.data() + writtenLength, payload.size() - writtenLength);
                if (writeLength > 0)
                {
                    writtenLength += static_cast<std::size_t>(writeLength);
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
         * @brief 轮询读取描述符，直到谓词满足或超时
         * @param descriptor 测试自己持有的那一端
         * @param received 输出：累计读到的字节
         * @param predicate 判定是否可以停止读取
         * @param timeout 等待上限
         * @return true 谓词在时限内成立
         */
        bool readUntilPredicate(const int descriptor, std::string &received, const std::function<bool(const std::string &)> &predicate,
                                const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            std::array<char, kPeerChunkLength> chunkStorage{};

            while (!predicate(received))
            {
                const ssize_t readLength = Platform::FileDescriptor::read(descriptor, chunkStorage.data(), chunkStorage.size());
                if (readLength > 0)
                {
                    received.append(chunkStorage.data(), static_cast<std::size_t>(readLength));
                    continue;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    break;
                }
                // 0 表示对端已关闭、-1 表示暂无数据或出错：让出时间片后继续轮询，直到超时
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return predicate(received);
        }

        /**
         * @brief 等待对端关闭连接
         * @details 非阻塞读返回 0 才算「对端已 FIN」，返回 -1 只是暂时没数据，两者必须分开。
         * @param descriptor 测试自己持有的那一端
         * @param timeout 等待上限
         * @return true 在时限内观察到对端关闭
         */
        bool waitForPeerClosed(const int descriptor, const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            std::array<char, kPeerChunkLength> drainStorage{};

            while (std::chrono::steady_clock::now() < deadline)
            {
                const ssize_t readLength = Platform::FileDescriptor::read(descriptor, drainStorage.data(), drainStorage.size());
                if (readLength == 0)
                {
                    return true;
                }
                if (readLength < 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            return false;
        }

        /**
         * @brief 统计响应文本里的状态行数
         * @details 只认行首的 "HTTP/1."，避免正文里出现同样字面量时被误计。
         * @param responseText 已收到的全部字节
         * @return std::size_t 状态行条数
         */
        std::size_t countStatusLines(const std::string &responseText)
        {
            // 不能按「行首」判定：响应正文不以 CRLF 收尾，流水线里第二条状态行会紧跟在上一条的
            // 正文之后（形如 served-firstHTTP/1.1 200 OK），既不在行首也不在 0 位置。
            // 这里直接统计状态行前缀的出现次数，前提是测试用的正文里不含该字面量
            constexpr std::string_view statusLinePrefix = "HTTP/1.";
            std::size_t statusLineCount = 0;

            for (std::size_t foundPosition = responseText.find(statusLinePrefix); foundPosition != std::string::npos;
                 foundPosition = responseText.find(statusLinePrefix, foundPosition + statusLinePrefix.size()))
            {
                ++statusLineCount;
            }
            return statusLineCount;
        }

        /**
         * @brief 判断响应文本是否含指定状态行（按行首匹配）
         * @param responseText 已收到的全部字节
         * @param statusLine 例如 "HTTP/1.1 431"
         * @return true 命中
         */
        bool containsStatusLine(const std::string &responseText, const std::string_view statusLine)
        {
            if (responseText.starts_with(statusLine))
            {
                return true;
            }
            const std::string linePrefixed = std::string("\r\n").append(statusLine);
            return responseText.find(linePrefixed) != std::string::npos;
        }

        /**
         * @brief 组装一条请求报文
         * @param requestLine 请求行原文，不含行尾 CRLF
         * @param headerLines 头部行原文，每项不含行尾 CRLF
         * @return std::string 以空行收尾的完整报文
         */
        std::string makeRequestText(const std::string_view requestLine, const std::vector<std::string> &headerLines)
        {
            std::string request(requestLine);
            request.append("\r\n");
            for (const std::string &headerLine: headerLines)
            {
                request.append(headerLine);
                request.append("\r\n");
            }
            request.append("\r\n");
            return request;
        }

        /**
         * @brief 在独立线程上驱动 EventLoop 的夹具
         * @details 必须晚于会话协程任务构造：析构顺序保证「先 join 循环线程，再销毁协程帧与会话」，
         *          否则循环线程可能恢复一个已被销毁的句柄。
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
         * @brief 跑一条 HTTP 会话直到收口，并记录退出方式
         * @param session 被测会话
         * @param outcome 结果槽
         * @return Core::Task<> 协程，start() 返回后置位 finished
         */
        Core::Task<> runSession(HttpSession &session, SessionOutcome &outcome)
        {
            try
            {
                co_await session.start();
            } catch (const Base::SystemException &)
            {
                outcome.failure = FailureKind::SystemException;
            } catch (const Base::Exception &)
            {
                outcome.failure = FailureKind::BaseException;
            } catch (...)
            {
                outcome.failure = FailureKind::UnknownException;
            }
            outcome.finished.store(true, std::memory_order_release);
            co_return;
        }

        /**
         * @brief 描述符对 + 路由器 + 会话 + 事件循环的用例夹具
         *
         * @details 成员声明顺序即生命周期顺序：循环 → 描述符对 → 路由器 → 会话 → 结果槽 →
         *       会话协程任务 → 循环线程。循环线程最后构造、最先析构，保证「先 join 线程、
         *       后销毁协程帧」；没调用过 start() 时它根本不存在。
         */
        class HttpSessionFixture
        {
        public:
            HttpSessionFixture() :
                m_loop(),
                m_descriptors(),
                m_session(Core::AsyncSocket(m_loop, m_descriptors.takeSessionSide()), m_router),
                m_startTask(runSession(m_session, m_outcome))
            {
            }

            HttpSessionFixture(const HttpSessionFixture &) = delete;
            HttpSessionFixture &operator=(const HttpSessionFixture &) = delete;

            /// 描述符对是否创建成功（失败时用例立即失败，而不是拿无效描述符去跑）
            [[nodiscard]] bool isValid() const noexcept
            {
                return m_descriptors.isValid();
            }

            /// 路由器：必须在 start() 之前注册完毕
            [[nodiscard]] Router &router() noexcept
            {
                return m_router;
            }

            /// 测试自己那一端描述符
            [[nodiscard]] int peerDescriptor() const noexcept
            {
                return m_descriptors.peerSide();
            }

            /// 被测会话本体，供需要直接取 Cancelable 的用例使用
            [[nodiscard]] HttpSession &session() noexcept
            {
                return m_session;
            }

            /// 所属事件循环，供 handler 内建定时器
            [[nodiscard]] Core::EventLoop &loop() noexcept
            {
                return m_loop;
            }

            /**
             * @brief 把请求字节写进会话的接收方向
             * @details 在 start() 之前调用时字节全躺在内核缓冲里，会话第一次读取必然同步拿到，
             *          完全不依赖 epoll 唤醒；载荷大于内核缓冲时必须先 start() 再写。
             */
            bool writeRequest(const std::string_view payload)
            {
                return writeFully(m_descriptors.peerSide(), payload, kLargePayloadWaitTimeout);
            }

            /// 启动事件循环线程并投递会话协程
            void start()
            {
                m_loopThread.emplace(m_loop);
                m_loopThread->schedule(m_startTask);
            }

            /// 会话协程是否已结束
            bool awaitFinished(const std::chrono::milliseconds timeout)
            {
                return waitForCondition(
                        [this]
                        {
                            return m_outcome.finished.load(std::memory_order_acquire);
                        },
                        timeout);
            }

            /// 会话协程当前是否已结束
            [[nodiscard]] bool isFinished() const
            {
                return m_outcome.finished.load(std::memory_order_acquire);
            }

            /// 会话协程的退出方式
            [[nodiscard]] FailureKind failure() const
            {
                return m_outcome.failure;
            }

            /// 关闭测试端并等待会话收口：保证用例退出时没有还挂在 epoll 上的协程
            bool closePeerAndAwaitFinished()
            {
                m_descriptors.closePeerSide();
                return awaitFinished(kWaitTimeout);
            }

        private:
            Core::EventLoop m_loop;                      ///< 事件循环本体
            LoopbackDescriptorPair m_descriptors;        ///< 全双工描述符对
            Router m_router;                             ///< 路由器，会话持有其引用
            HttpSession m_session;                       ///< 被测会话
            SessionOutcome m_outcome;                    ///< 会话协程结果槽
            Core::Task<> m_startTask;                    ///< 会话主协程任务
            std::optional<EventLoopThread> m_loopThread; ///< 循环线程，最后构造、最先析构
        };

        /**
         * @brief 注册一条「把路径回写进正文」的 GET 路由，便于分辨到底哪条请求被处理了
         * @param router 目标路由器
         * @param path 路径，同时也是期望的响应正文标记
         */
        void addPathEchoingRoute(Router &router, const std::string &path)
        {
            // 按值捕获 path：handler 会存活到用例结束，绑引用就是悬垂
            router.get(path, [path](HttpRequest &, HttpResponse &response) -> Core::Task<>
            {
                response.setBody("served-" + path.substr(1));
                co_return;
            });
        }

        /**
         * @brief 等待指定条数的响应状态行
         * @param fixture 会话夹具
         * @param responseText 输入输出：已累计读到的字节
         * @param expectedStatusLines 期望出现的状态行条数
         * @param timeout 等待上限
         * @return true 在时限内出现
         */
        bool awaitResponseLines(HttpSessionFixture &fixture, std::string &responseText, const std::size_t expectedStatusLines,
                                const std::chrono::milliseconds timeout)
        {
            return readUntilPredicate(fixture.peerDescriptor(), responseText,
                                      [expectedStatusLines](const std::string &accumulated)
                                      {
                                          return countStatusLines(accumulated) >= expectedStatusLines;
                                      },
                                      timeout);
        }
    } // namespace

    TEST(HttpSession, ConstructorTakesSocketAndRouterOnly)
    {
        // 构造函数已去掉从未使用的 Core::EventLoop& 形参：事件循环由 AsyncSocket 内部持有
        static_assert(std::is_constructible_v<HttpSession, Core::AsyncSocket, Router &>,
                      "HttpSession 应能以 (AsyncSocket, Router&) 构造");
        static_assert(!std::is_constructible_v<HttpSession, Core::EventLoop &, Core::AsyncSocket, Router &>,
                      "多余的 EventLoop& 形参应已删除");
        static_assert(std::is_base_of_v<Core::Connection, HttpSession>, "HttpSession 必须是一种 Connection");
        static_assert(std::is_polymorphic_v<HttpSession>, "start() 是虚函数，会话必须可多态销毁");
        SUCCEED() << "以上均为编译期断言";
    }

    TEST(HttpSession, RequestCloseHeaderForcesDisconnect)
    {
        HttpRequest request;
        request.setHttpVersion("HTTP/1.1");
        request.addHeader("connection", "close");
        const HttpResponse response;

        EXPECT_FALSE(HttpSession::shouldKeepAlive(request, response));
    }

    TEST(HttpSession, ResponseCloseHeaderForcesDisconnect)
    {
        // 服务器侧主动收口（例如中间件降级）同样必须断开
        HttpRequest request;
        request.setHttpVersion("HTTP/1.1");
        HttpResponse response;
        response.setHeader("connection", "close");

        EXPECT_FALSE(HttpSession::shouldKeepAlive(request, response));
    }

    TEST(HttpSession, RequestCloseIsNotReversedByResponseKeepAlive)
    {
        // 客户端指令优先：判定顺序若把「响应 keep-alive」放在最后，
        // 「请求 close」就会被反转成保活，而客户端的显式指令不可被反转
        HttpRequest request;
        request.setHttpVersion("HTTP/1.1");
        request.addHeader("connection", "close");

        HttpResponse response;
        response.setHeader("connection", "keep-alive");

        EXPECT_FALSE(HttpSession::shouldKeepAlive(request, response));
    }

    TEST(HttpSession, RequestKeepAliveOverridesHttp10Default)
    {
        // HTTP/1.0 默认逐请求断连，客户端显式要求保活时才保活（也是它唯一有实际意义的场合）
        HttpRequest request;
        request.setHttpVersion("HTTP/1.0");
        request.addHeader("connection", "keep-alive");
        const HttpResponse response;

        EXPECT_TRUE(HttpSession::shouldKeepAlive(request, response));
    }

    TEST(HttpSession, ProtocolVersionDecidesWhenNoConnectionHeaderPresent)
    {
        const HttpResponse response;

        HttpRequest httpOneZero;
        httpOneZero.setHttpVersion("HTTP/1.0");
        EXPECT_FALSE(HttpSession::shouldKeepAlive(httpOneZero, response));

        HttpRequest httpZeroNine;
        httpZeroNine.setHttpVersion("HTTP/0.9");
        EXPECT_FALSE(HttpSession::shouldKeepAlive(httpZeroNine, response));

        HttpRequest httpOneOne;
        httpOneOne.setHttpVersion("HTTP/1.1");
        EXPECT_TRUE(HttpSession::shouldKeepAlive(httpOneOne, response));

        // 版本为空（未经解析器填充）按「老协议」处理：宁可多断一次连接，也不误留半死连接
        const HttpRequest withoutVersion;
        EXPECT_FALSE(HttpSession::shouldKeepAlive(withoutVersion, response));
    }

    TEST(HttpSession, ConnectionTokensMatchCaseInsensitivelyAcrossCommaList)
    {
        // 同名头部的多个值按逗号拆分后逐 token 比对，大小写与两侧空白都不敏感
        HttpRequest mixedCaseKeepAlive;
        mixedCaseKeepAlive.setHttpVersion("HTTP/1.0");
        mixedCaseKeepAlive.addHeader("Connection", "  KEEP-ALIVE  ");
        EXPECT_TRUE(HttpSession::shouldKeepAlive(mixedCaseKeepAlive, HttpResponse{}));

        HttpRequest closeInList;
        closeInList.setHttpVersion("HTTP/1.1");
        closeInList.addHeader("connection", "keep-alive, close");
        EXPECT_FALSE(HttpSession::shouldKeepAlive(closeInList, HttpResponse{}));

        // 前缀相同但不是同一个 token：不能被当成 close 而误断连接
        HttpRequest unrelatedToken;
        unrelatedToken.setHttpVersion("HTTP/1.1");
        unrelatedToken.addHeader("connection", "close-but-not-really");
        EXPECT_TRUE(HttpSession::shouldKeepAlive(unrelatedToken, HttpResponse{}));
    }

    TEST(HttpSession, RespondsToSingleGetRequest)
    {
        HttpSessionFixture fixture;
        ASSERT_TRUE(fixture.isValid()) << "全双工描述符对创建失败";
        addPathEchoingRoute(fixture.router(), "/hello");

        ASSERT_TRUE(fixture.writeRequest(makeRequestText("GET /hello HTTP/1.1", {"host: test"})));
        fixture.start();

        std::string responseText;
        ASSERT_TRUE(awaitResponseLines(fixture, responseText, 1, kWaitTimeout)) << "未在时限内拿到响应：上界 kWaitTimeout";

        EXPECT_TRUE(containsStatusLine(responseText, "HTTP/1.1 200"));
        EXPECT_NE(responseText.find("served-hello"), std::string::npos);
        EXPECT_NE(responseText.find("content-length: 12"), std::string::npos);
        // HTTP/1.1 默认保活：既不该补 close，也不必显式宣告 keep-alive
        EXPECT_EQ(responseText.find("connection:"), std::string::npos);

        EXPECT_TRUE(fixture.closePeerAndAwaitFinished());
        // 注意：这里刻意不断言收口方式。测试端只数到状态行就可能关闭，响应正文尚未读干净时
        // closesocket 会改发 RST，会话于是以传输层错误收口——那是用例读取粒度造成的，不是契约
    }

    TEST(HttpSession, AnswersBothRequestsDeliveredInOnePacket)
    {
        HttpSessionFixture fixture;
        ASSERT_TRUE(fixture.isValid());
        addPathEchoingRoute(fixture.router(), "/first");
        addPathEchoingRoute(fixture.router(), "/second");

        // 回归防护：一个包里粘着两条请求时，第一条应答完剩下的字节必须留在接收缓冲里，
        // 下一轮先喂进解析器；每次都从缓冲区开头读会把第二条请求静默丢掉
        const std::string packet =
                makeRequestText("GET /first HTTP/1.1", {"host: test"}) +
                makeRequestText("GET /second HTTP/1.1", {"host: test"});
        ASSERT_TRUE(fixture.writeRequest(packet));
        fixture.start();

        std::string responseText;
        ASSERT_TRUE(awaitResponseLines(fixture, responseText, 2, kWaitTimeout)) << "流水线里的第二条请求没得到响应：上界 kWaitTimeout";

        EXPECT_EQ(countStatusLines(responseText), 2u);
        EXPECT_TRUE(containsStatusLine(responseText, "HTTP/1.1 200"));
        const std::size_t firstMarkerPosition = responseText.find("served-first");
        const std::size_t secondMarkerPosition = responseText.find("served-second");
        ASSERT_NE(firstMarkerPosition, std::string::npos);
        ASSERT_NE(secondMarkerPosition, std::string::npos);
        // 第二条的应答必须排在第一条之后：顺序不对说明两条报文被喂错了批次
        EXPECT_LT(firstMarkerPosition, secondMarkerPosition);

        EXPECT_TRUE(fixture.closePeerAndAwaitFinished());
    }

    TEST(HttpSession, KeepsReceivingBodyAcrossSeparateWrites)
    {
        HttpSessionFixture fixture;
        ASSERT_TRUE(fixture.isValid());
        addPathEchoingRoute(fixture.router(), "/ping");
        fixture.router().post("/upload", [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
        {
            response.setBody("received-" + std::to_string(request.body().size()));
            co_return;
        });

        // 第一条请求完整、第二条请求只写到头部块中间（连结尾空行都没给）：
        // 会话答完第一条后必然在「继续收第二条」的路上挂起，等下一次 write 把剩余字节补上。
        // 时序说明：第二段字节是在「看到第一条响应」之后才写的，此时挂起确实已发生；
        // 唯一的风险窗口是「会话刚返回 EAGAIN、还没把描述符登记进 epoll」那一瞬，
        // 故给出 kWaitTimeout（2 秒）上界，超时只判失败不挂用例。
        const std::string firstPart =
                makeRequestText("GET /ping HTTP/1.1", {"host: test"}) +
                "POST /upload HTTP/1.1\r\nhost: test\r\ncontent-length: 5\r\n";
        ASSERT_TRUE(fixture.writeRequest(firstPart));
        fixture.start();

        std::string responseText;
        ASSERT_TRUE(awaitResponseLines(fixture, responseText, 1, kWaitTimeout)) << "第一条请求未答：上界 kWaitTimeout";

        // 空行 + 正文补齐第二条请求：这些字节要靠跨次读取存续的接收缓冲继续喂给解析器
        ASSERT_TRUE(fixture.writeRequest("\r\n12345"));
        EXPECT_TRUE(awaitResponseLines(fixture, responseText, 2, kWaitTimeout)) << "分包送达的第二条请求未答：上界 kWaitTimeout";
        EXPECT_NE(responseText.find("received-5"), std::string::npos);

        EXPECT_TRUE(fixture.closePeerAndAwaitFinished());
    }

    TEST(HttpSession, AnswersTwoRoundsOnOneKeptAliveConnection)
    {
        HttpSessionFixture fixture;
        ASSERT_TRUE(fixture.isValid());
        addPathEchoingRoute(fixture.router(), "/round");

        ASSERT_TRUE(fixture.writeRequest(makeRequestText("GET /round HTTP/1.1", {"host: test"})));
        fixture.start();

        std::string responseText;
        ASSERT_TRUE(awaitResponseLines(fixture, responseText, 1, kWaitTimeout));
        EXPECT_FALSE(fixture.isFinished()) << "第一轮答完就把连接收了：HTTP/1.1 默认应当保活";

        // 第二轮：同一条连接继续服务，证明保持活跃循环真的回到了读取状态
        ASSERT_TRUE(fixture.writeRequest(makeRequestText("GET /round HTTP/1.1", {"host: test", "x-round: two"})));
        EXPECT_TRUE(awaitResponseLines(fixture, responseText, 2, kWaitTimeout)) << "第二轮请求未答：上界 kWaitTimeout";
        EXPECT_EQ(countStatusLines(responseText), 2u);

        EXPECT_TRUE(fixture.closePeerAndAwaitFinished());
    }

    TEST(HttpSession, StopsAfterAnsweringRequestThatAskedToClose)
    {
        HttpSessionFixture fixture;
        ASSERT_TRUE(fixture.isValid());
        addPathEchoingRoute(fixture.router(), "/leave");
        addPathEchoingRoute(fixture.router(), "/never");

        // 请求显式 close：答完这条就必须收口，同一包里紧随其后的那条不再处理
        const std::string packet =
                makeRequestText("GET /leave HTTP/1.1", {"host: test", "connection: close"}) +
                makeRequestText("GET /never HTTP/1.1", {"host: test"});
        ASSERT_TRUE(fixture.writeRequest(packet));
        fixture.start();

        std::string responseText;
        ASSERT_TRUE(awaitResponseLines(fixture, responseText, 1, kWaitTimeout));
        EXPECT_TRUE(containsStatusLine(responseText, "HTTP/1.1 200"));
        EXPECT_NE(responseText.find("connection: close"), std::string::npos);
        EXPECT_TRUE(fixture.awaitFinished(kWaitTimeout)) << "close 之后会话未收口：上界 kWaitTimeout";
        EXPECT_EQ(responseText.find("served-never"), std::string::npos) << "已经决定断开的连接上还继续跑了下一条请求";
        EXPECT_EQ(countStatusLines(responseText), 1u);
        // 这条路径是会话自己判定「答完就收口」，既不该再读也不该因写失败而抛
        EXPECT_EQ(fixture.failure(), FailureKind::None);
    }

    TEST(HttpSession, Returns431ForOversizedHeaderFields)
    {
        HttpSessionFixture fixture;
        ASSERT_TRUE(fixture.isValid());
        addPathEchoingRoute(fixture.router(), "/wide");

        // 单个头部值超过解析器的 8 KiB 上限：形态合法但体量越界，必须回 431 而不是 400。
        // 报文大于内核缓冲，必须先让会话跑起来边读边写；等待上界 kLargePayloadWaitTimeout。
        fixture.start();
        const std::string oversizedValue(9000, 'a');
        const std::string request = makeRequestText("GET /wide HTTP/1.1", {"host: test", "x-blob: " + oversizedValue});
        ASSERT_TRUE(fixture.writeRequest(request)) << "超限头部未能全部写入：上界 kLargePayloadWaitTimeout";

        std::string responseText;
        ASSERT_TRUE(awaitResponseLines(fixture, responseText, 1, kLargePayloadWaitTimeout)) << "超限头部未在时限内被判 431：上界 kLargePayloadWaitTimeout";
        EXPECT_TRUE(containsStatusLine(responseText, "HTTP/1.1 431"));
        EXPECT_NE(responseText.find("Request Header Fields Too Large"), std::string::npos);
        EXPECT_NE(responseText.find("connection: close"), std::string::npos);
        EXPECT_TRUE(fixture.awaitFinished(kWaitTimeout)) << "回完 431 没有收口：上界 kWaitTimeout";
    }

    TEST(HttpSession, Returns413ForDeclaredBodyOverLimit)
    {
        HttpSessionFixture fixture;
        ASSERT_TRUE(fixture.isValid());
        addPathEchoingRoute(fixture.router(), "/huge");

        // 声明正文超过 8 MiB 上限：定界器现在就拦，一个正文字节都不必收进内存
        const std::string request = makeRequestText("POST /huge HTTP/1.1",
                                                    {"host: test", "content-length: 9000000000", "content-type: text/plain"});
        ASSERT_TRUE(fixture.writeRequest(request));
        fixture.start();

        std::string responseText;
        ASSERT_TRUE(awaitResponseLines(fixture, responseText, 1, kWaitTimeout)) << "超限声明未在时限内被判 413：上界 kWaitTimeout";
        EXPECT_TRUE(containsStatusLine(responseText, "HTTP/1.1 413"));
        EXPECT_NE(responseText.find("Payload Too Large"), std::string::npos);
        EXPECT_NE(responseText.find("connection: close"), std::string::npos);
        EXPECT_TRUE(fixture.awaitFinished(kWaitTimeout));
    }

    TEST(HttpSession, Returns400ForMalformedRequestLine)
    {
        HttpSessionFixture fixture;
        ASSERT_TRUE(fixture.isValid());
        addPathEchoingRoute(fixture.router(), "/later");

        // 报文根本读不懂（协议版本号非法）：回 400，且收口前不复用这条连接
        const std::string packet =
                makeRequestText("GET /broken HTTP/9.9", {"host: test"}) +
                makeRequestText("GET /later HTTP/1.1", {"host: test"});
        ASSERT_TRUE(fixture.writeRequest(packet));
        fixture.start();

        std::string responseText;
        ASSERT_TRUE(awaitResponseLines(fixture, responseText, 1, kWaitTimeout)) << "畸形报文未在时限内被判 400：上界 kWaitTimeout";
        EXPECT_TRUE(containsStatusLine(responseText, "HTTP/1.1 400"));
        EXPECT_NE(responseText.find("Bad Request"), std::string::npos);
        EXPECT_TRUE(fixture.awaitFinished(kWaitTimeout));
        // 出错即断连：脏请求不能留在同一条连接上继续跑，粘在后面的那条也不该被应答
        EXPECT_EQ(responseText.find("served-later"), std::string::npos);
        EXPECT_EQ(countStatusLines(responseText), 1u);
    }

    TEST(HttpSession, Returns411ForChunkedRequestBody)
    {
        HttpSessionFixture fixture;
        ASSERT_TRUE(fixture.isValid());
        addPathEchoingRoute(fixture.router(), "/stream");

        // 本框架不做分块请求体的帧定界：按 411 Length Required 收口，而不是猜边界
        const std::string request = makeRequestText("POST /stream HTTP/1.1", {"host: test", "transfer-encoding: chunked"});
        ASSERT_TRUE(fixture.writeRequest(request));
        fixture.start();

        std::string responseText;
        ASSERT_TRUE(awaitResponseLines(fixture, responseText, 1, kWaitTimeout)) << "分块请求体未在时限内被判 411：上界 kWaitTimeout";
        EXPECT_TRUE(containsStatusLine(responseText, "HTTP/1.1 411"));
        EXPECT_NE(responseText.find("Length Required"), std::string::npos);

        EXPECT_TRUE(fixture.closePeerAndAwaitFinished());
    }

    TEST(HttpSession, Returns500AfterResettingResponseWhenHandlerThrows)
    {
        HttpSessionFixture fixture;
        ASSERT_TRUE(fixture.isValid());
        fixture.router().get("/boom", [](HttpRequest &, HttpResponse &response) -> Core::Task<>
        {
            // handler 已经写了一半头部与正文：会话必须先整体 reset 再填 500，
            // 否则半成品连同错的 content-length 会一起发出去
            response.setHeader("x-partial", "half-written-marker");
            response.setBody("half-written-body");
            throw Base::Exception("测试用：业务处理函数抛出异常");
            co_return;
        });

        ASSERT_TRUE(fixture.writeRequest(makeRequestText("GET /boom HTTP/1.1", {"host: test"})));
        fixture.start();

        std::string responseText;
        ASSERT_TRUE(awaitResponseLines(fixture, responseText, 1, kWaitTimeout)) << "handler 抛异常后未拿到响应：上界 kWaitTimeout";
        EXPECT_TRUE(containsStatusLine(responseText, "HTTP/1.1 500"));
        EXPECT_NE(responseText.find("Internal Server Error"), std::string::npos);
        EXPECT_NE(responseText.find("content-length: 21"), std::string::npos);
        EXPECT_EQ(responseText.find("half-written-marker"), std::string::npos);
        EXPECT_EQ(responseText.find("half-written-body"), std::string::npos);
        // 500 之后仍按 keep-alive 判定：HTTP/1.1 且没人要求 close，连接可以继续用
        EXPECT_FALSE(fixture.isFinished());

        EXPECT_TRUE(fixture.closePeerAndAwaitFinished());
    }

    TEST(HttpSession, PropagatesConnectionStopToRequestCancelToken)
    {
        HttpSessionFixture fixture;
        ASSERT_TRUE(fixture.isValid());

        // 连接的停止请求要经 detail::ConnectionCancelForwarder 转成本次请求的协作式取消，
        // 业务侧只认 request.cancelToken() 一处出口
        std::atomic<bool> handlerEntered{false};
        std::atomic<bool> cancelObserved{false};
        Core::EventLoop &loop = fixture.loop();
        fixture.router().get("/slow",
                             [&loop, &handlerEntered, &cancelObserved](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                             {
                                 Core::Timer timer(loop);
                                 handlerEntered.store(true, std::memory_order_release);
                                 // 最多轮询 1 秒；用例在 handler 进入循环后立刻发出停止请求，余量足够
                                 for (int pollRound = 0; pollRound < 200 && !request.cancelToken().stop_requested(); ++pollRound)
                                 {
                                     co_await timer.waitFor(std::chrono::milliseconds(5));
                                 }
                                 cancelObserved.store(request.cancelToken().stop_requested(), std::memory_order_release);
                                 response.setBody("slow-finished");
                                 co_return;
                             });

        ASSERT_TRUE(fixture.writeRequest(makeRequestText("GET /slow HTTP/1.1", {"host: test"})));
        fixture.start();

        // 时序说明：两个等待都是 2 秒上界。第一步证明 handler 已在跑（才会命中转发器），
        // 第二步要求它在同一轮事务里观察到取消信号；handler 内部的自轮询上限是 1 秒。
        ASSERT_TRUE(waitForCondition(
                [&handlerEntered]
                {
                    return handlerEntered.load(std::memory_order_acquire);
                },
                kWaitTimeout)) << "handler 未进入：上界 kWaitTimeout";
        EXPECT_TRUE(fixture.session().cancelable().requestStop());
        EXPECT_TRUE(waitForCondition(
                [&cancelObserved]
                {
                    return cancelObserved.load(std::memory_order_acquire);
                },
                kWaitTimeout)) << "连接停止没转成请求取消：上界 kWaitTimeout";

        EXPECT_TRUE(fixture.closePeerAndAwaitFinished());
    }

    TEST(HttpSession, ClosesSocketWhenKeepAliveRoundEnds)
    {
        HttpSessionFixture fixture;
        ASSERT_TRUE(fixture.isValid());
        addPathEchoingRoute(fixture.router(), "/done");

        // HTTP/1.0 且没有显式 keep-alive：答完就必须断连，且会话要自己把 socket 关掉
        ASSERT_TRUE(fixture.writeRequest(makeRequestText("GET /done HTTP/1.0", {"host: test"})));
        fixture.start();

        std::string responseText;
        ASSERT_TRUE(awaitResponseLines(fixture, responseText, 1, kWaitTimeout));
        // 状态行版本跟随请求，不硬编码 1.1
        EXPECT_TRUE(containsStatusLine(responseText, "HTTP/1.0 200"));
        EXPECT_NE(responseText.find("connection: close"), std::string::npos);
        EXPECT_EQ(responseText.find("connection: keep-alive"), std::string::npos) << "1.0 判定为断开时不该同时宣告保活";

        EXPECT_TRUE(fixture.awaitFinished(kWaitTimeout)) << "1.0 事务结束后会话未收口：上界 kWaitTimeout";
        // 会话收口即关闭它那一端：测试端随后必然读到 EOF（返回 0），而不是「暂时没数据」
        EXPECT_TRUE(waitForPeerClosed(fixture.peerDescriptor(), kWaitTimeout)) << "会话退出后描述符没有关掉：上界 kWaitTimeout";
    }

    TEST(HttpSession, EchoesKeepAliveHeaderForHttp10Request)
    {
        HttpSessionFixture fixture;
        ASSERT_TRUE(fixture.isValid());
        addPathEchoingRoute(fixture.router(), "/legacy");

        // HTTP/1.0 显式要求保活时，服务端必须把 keep-alive 显式回给对方，
        // 否则两端各按自己的默认值理解这条连接，出现「一端留着、一端关掉」的错位
        ASSERT_TRUE(fixture.writeRequest(makeRequestText("GET /legacy HTTP/1.0", {"host: test", "connection: keep-alive"})));
        fixture.start();

        std::string responseText;
        ASSERT_TRUE(awaitResponseLines(fixture, responseText, 1, kWaitTimeout));
        EXPECT_TRUE(containsStatusLine(responseText, "HTTP/1.0 200"));
        EXPECT_NE(responseText.find("connection: keep-alive"), std::string::npos);
        EXPECT_FALSE(fixture.isFinished()) << "1.0 显式保活的连接被提前收口";

        EXPECT_TRUE(fixture.closePeerAndAwaitFinished());
    }
} // namespace AsynGyanis::Net
