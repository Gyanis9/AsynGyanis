/**
 * @file TestHttpAcceptDispatch.cpp
 * @brief 接收分发端到端：一个监听器 + 两个工作循环，连接被轮流交给不同循环服务
 * @details 这条用例钉的是「多核扩展不再依赖 SO_REUSEPORT」：接受循环自己不做任何协议工作，
 *          连接落到哪个循环就由哪个循环的服务器应答——响应正文自报家门，因此「分没分过去」
 *          是从客户端看得见的事实，而不是内部计数的自说自话。
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Http/HttpServer.h"

#include "Core/EventLoop/ConnectionDistributor.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        /// 一条请求得在多久内拿到响应
        constexpr auto kRequestTimeout = std::chrono::seconds{5};

        /**
         * @brief 独立线程上跑一个事件循环
         */
        class WorkerLoop
        {
        public:
            WorkerLoop() :
                m_thread(
                        [this]()
                        {
                            m_loop.run();
                        })
            {
            }

            ~WorkerLoop()
            {
                stopAndJoin();
            }

            WorkerLoop(const WorkerLoop &) = delete;

            WorkerLoop &operator=(const WorkerLoop &) = delete;

            [[nodiscard]] Core::EventLoop &loop() noexcept
            {
                return m_loop;
            }

            /**
             * @brief 在循环线程上执行一段动作，并等它做完
             *
             * @details 本文件的服务器与接受侧监听器都归各自的工作循环所有：构造会在该循环里注册描述符，
             *          close() 会关掉监听并顺带清掉活跃连接——两者都只能在循环线程上做，否则就是与循环
             *          抢同一批句柄（TSan 的并发用例集报的正是这一类）。线程约束见 TcpServer::stop()。
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

                const auto deadline = std::chrono::steady_clock::now() + kRequestTimeout;
                while (!isFinished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                EXPECT_TRUE(isFinished.load(std::memory_order_acquire)) << "投递到循环线程的动作没有在时限内完成";
            }

            /// 停循环并等线程退出：销毁服务器之前必须先把循环停掉，否则会跨线程销毁循环内部结构
            void stopAndJoin()
            {
                if (m_thread.joinable())
                {
                    m_loop.stop();
                    m_thread.join();
                }
            }

        private:
            Core::EventLoop m_loop;   ///< 循环本体
            std::thread     m_thread; ///< 承载 run() 的线程
        };

        /**
         * @brief 轮询读，直到累计文本里出现指定标记
         * @param client 回环客户端
         * @param accumulated 输入输出：累计读到的字节
         * @param expectedText 作为「已到达」判据的标记文本
         * @param timeout 等待上限
         * @return true 时限内读到了标记
         */
        bool waitForTextOccurrence(const LoopbackClient &client, std::string &accumulated, const std::string_view expectedText,
                                   const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (accumulated.find(expectedText) == std::string::npos)
            {
                const ReadOutcome outcome = client.readOnce(accumulated);
                if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                {
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return accumulated.find(expectedText) != std::string::npos;
        }

        /**
         * @brief 能读到内核实际分配端口与派发计数的接收服务器
         * @details TcpServer 把监听器放在受保护区，用例通过这个只读口子拿到真实端口——
         *          构造时传的端口是 0，只有内核知道最终分到了哪一个
         */
        class PortObservableHttpServer final : public HttpServer
        {
        public:
            using HttpServer::HttpServer;

            /// 内核实际分配的监听端口
            [[nodiscard]] std::uint16_t boundPort() const
            {
                return queryBoundPort(m_acceptor.fileDescriptor());
            }
        };

        /**
         * @brief 造一台工作服务器：只挂一条自报家门的路由，不接受任何连接
         * @param loop 该服务器所属循环
         * @param identity 响应正文，用于从客户端辨认「谁服务了这条连接」
         * @return std::unique_ptr<HttpServer> 服务器
         */
        std::unique_ptr<HttpServer> makeWorkerServer(Core::EventLoop &loop, std::string identity)
        {
            auto server = std::make_unique<HttpServer>(loop, Core::InetAddress::localhost(0));
            server->router().get("/whoami",
                                 [identity = std::move(identity)](HttpRequest &, HttpResponse &response) -> Core::Task<>
                                 {
                                     response.setBody(identity);
                                     co_return;
                                 });
            return server;
        }
    } // namespace

    /**
     * @brief 四条连接轮转落到两个工作循环：两条由 A 服务、两条由 B 服务
     */
    TEST(HttpAcceptDispatch, HandsConnectionsToWorkerLoopsInTurn)
    {
        WorkerLoop workerLoopA;
        WorkerLoop workerLoopB;

        // 构造必须落在服务器所属的循环线程上：这一步会在循环里注册监听描述符的 IoWatcher
        std::unique_ptr<HttpServer> serverA;
        workerLoopA.runOnLoopAndWait([&workerLoopA, &serverA] { serverA = makeWorkerServer(workerLoopA.loop(), "served-by-A"); });

        std::unique_ptr<HttpServer> serverB;
        workerLoopB.runOnLoopAndWait([&workerLoopB, &serverB] { serverB = makeWorkerServer(workerLoopB.loop(), "served-by-B"); });

        auto distributor = std::make_shared<Core::ConnectionDistributor>();
        distributor->addWorker(workerLoopA.loop(),
                               [server = serverA.get()](const int fileDescriptor)
                               {
                                   server->adoptConnection(fileDescriptor);
                               });
        distributor->addWorker(workerLoopB.loop(),
                               [server = serverB.get()](const int fileDescriptor)
                               {
                                   server->adoptConnection(fileDescriptor);
                               });

        // 接受侧：一台只接受与派发的服务器，自己不建连接（它同样是 HttpServer，只是没人给它派连接）。
        // 它归循环 A，因此同样在 A 的线程上构造、在 A 的线程上收尾
        std::unique_ptr<PortObservableHttpServer> acceptor;
        workerLoopA.runOnLoopAndWait(
                [&workerLoopA, &acceptor]
                {
                    acceptor = std::make_unique<PortObservableHttpServer>(workerLoopA.loop(), Core::InetAddress::localhost(0));
                });
        Core::Task<> acceptTask = acceptor->startAccepting(distributor);

        // 启动必须在服务器所属循环上：投过去之后由本用例持有任务帧直到结束
        workerLoopA.loop().scheduler().postRemote(
                [&workerLoopA, &acceptTask]()
                {
                    workerLoopA.loop().scheduler().schedule(acceptTask.handle());
                });

        // 等到接受循环真正跑起来（isRunning 为 true 说明 bind/listen 已成功）
        const auto readyDeadline = std::chrono::steady_clock::now() + kRequestTimeout;
        while (!acceptor->isRunning() && std::chrono::steady_clock::now() < readyDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        ASSERT_TRUE(acceptor->isRunning()) << "接受循环没有在时限内起来";

        const std::uint16_t listeningPort = acceptor->boundPort();
        ASSERT_NE(listeningPort, 0U);

        // 四条各用一条新连接：轮转顺序就是到达顺序，因此应当是 A、B、A、B
        std::size_t servedByACount = 0;
        std::size_t servedByBCount = 0;
        for (int index = 0; index < 4; ++index)
        {
            LoopbackClient client(listeningPort);
            ASSERT_TRUE(client.isValid()) << "第 " << index << " 条连接没连上";

            std::string responseText;
            ASSERT_TRUE(client.sendText(makeRequestText("GET /whoami HTTP/1.1"), kRequestTimeout)) << "第 " << index << " 条请求没发出去";
            ASSERT_TRUE(waitForTextOccurrence(client, responseText, "\r\n\r\n", kRequestTimeout))
                    << "第 " << index << " 条请求没拿到响应头";
            ASSERT_TRUE(waitForTextOccurrence(client, responseText, "served-by-", kRequestTimeout))
                    << "第 " << index << " 条请求的响应里没有自报家门，实际收到：" << responseText;

            if (responseText.find("served-by-A") != std::string::npos)
            {
                ++servedByACount;
            } else
            {
                ++servedByBCount;
            }
        }

        EXPECT_EQ(servedByACount, 2U) << "轮转没有把一半连接交给工作循环 A";
        EXPECT_EQ(servedByBCount, 2U) << "轮转没有把一半连接交给工作循环 B";
        EXPECT_EQ(distributor->distributedCount(), 4U) << "派发计数与实际连接数不符";

        // 收尾顺序：先在各自循环上关掉服务器（A 上两台、B 上一台），再让循环停手退出。
        // 从测试线程直接 close() 会与循环抢监听描述符与活跃连接的套接字
        workerLoopA.runOnLoopAndWait(
                [&acceptor, &serverA]
                {
                    acceptor->close();
                    serverA->close();
                });
        workerLoopB.runOnLoopAndWait([&serverB] { serverB->close(); });

        workerLoopA.stopAndJoin();
        workerLoopB.stopAndJoin();
    }

    /**
     * @brief 没有登记工作循环时 startAccepting() 直接拒绝，而不是跑起来一条条丢连接
     */
    TEST(HttpAcceptDispatch, RefusesToStartWithoutWorkers)
    {
        WorkerLoop   loop;
        HttpServer   acceptor(loop.loop(), Core::InetAddress::localhost(0));
        auto         emptyDistributor = std::make_shared<Core::ConnectionDistributor>();

        // 启动协程在所属循环上执行，异常也从那里冒出来：这里用 isRunning 观察「没能起来」
        Core::Task<> acceptTask = acceptor.startAccepting(emptyDistributor);
        loop.loop().scheduler().postRemote(
                [&loop, &acceptTask]()
                {
                    loop.loop().scheduler().schedule(acceptTask.handle());
                });

        const auto deadline = std::chrono::steady_clock::now() + kRequestTimeout;
        while (!acceptTask.isReady() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        EXPECT_TRUE(acceptTask.isReady()) << "没有工作循环时启动协程既没完成也没报错";
        EXPECT_FALSE(acceptor.isRunning()) << "没有工作循环却进入了接受循环";

        acceptor.close();
        loop.stopAndJoin();
    }
} // namespace AsynGyanis::Net
