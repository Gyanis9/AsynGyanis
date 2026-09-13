/**
 * @file TestTcpServer.cpp
 * @brief TcpServer 单元测试：纯虚钩子、接受循环、连接丢弃与 ConnectionManager 参与的优雅关闭（stop/close/drain）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Tcp/TcpServer.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/SystemException.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/Connection.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 一般等待上限：回环上的握手与协程唤醒都在毫秒级完成；超时即判失败，不允许无界等待
        constexpr std::chrono::milliseconds kWaitTimeout{2000};

        /// 「断言某事不发生」时用的观察窗口，只用于证明协程确实还在等待
        constexpr std::chrono::milliseconds kNegativeCheckTimeout{200};

        /// 「立刻完成」的判定上界：drain 的强关只涉及同线程内的几次唤醒，200ms 已是宽裕的上界
        constexpr std::chrono::milliseconds kImmediateCompletionTimeout{200};

        /// 测试连接轮询停止请求的间隔：足够短，让优雅关闭的断言不必久等
        constexpr std::chrono::milliseconds kStopPollInterval{5};

        /// createConnection 钩子的测试行为
        enum class CreateConnectionMode
        {
            Normal,             ///< 正常返回一个连接对象
            ReturnsNullPointer, ///< 故意返回空指针，模拟子类缺陷
            ThrowsOnce          ///< 首次调用抛异常，之后的调用恢复正常
        };

        /// 交给服务器的连接对象类型
        enum class ConnectionKind
        {
            FinishImmediately,  ///< start() 立刻完成：模拟一条转瞬即逝的连接
            ObservesStopRequest ///< 只等停止请求、完全不读套接字：模拟一条长期活跃的连接
        };

        /// 服务器夹具的构造参数
        struct ServerTestOptions
        {
            CreateConnectionMode mode{CreateConnectionMode::Normal}; ///< 钩子行为
            ConnectionKind kind{ConnectionKind::FinishImmediately};  ///< 连接类型
            std::size_t maxConnections{0};                           ///< 并发上限，0 表示不限制
            std::shared_ptr<PerIpConnectionLimiter> perIpLimiter{};  ///< 按来源 IP 的限额；空表示不作该限制
            bool markBusy{false};                                    ///< 连接是否自报「有在途工作」（用于分辨 drain 的等待与强关）
        };

        /// start() 协程的结束原因
        enum class FailureKind
        {
            None,             ///< 正常退出接受循环
            SystemException,  ///< 抛出 Base::SystemException
            BaseException,    ///< 抛出其它 Base::Exception
            UnknownException  ///< 抛出框架外的异常
        };

        /**
         * @brief 服务器主协程的结果槽
         * @details stopped 以 release 语义发布，failure 只在 stopped 为 true 后可读。
         */
        struct ServerOutcome
        {
            std::atomic<bool> stopped{false};  ///< start() 协程是否已结束
            FailureKind failure{FailureKind::None}; ///< 退出方式
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
         * @brief 在独立线程上驱动 EventLoop 的夹具
         * @details 必须声明在被投协程任务之后：析构顺序保证「先 join 循环线程，再销毁协程帧」，
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

            /// 停止循环并等待承载线程退出
            /// @note 析构会自动调用；需要在销毁服务器之前先让循环停手时显式调用它
            void join()
            {
                m_loop.stop();
                if (m_worker.joinable())
                {
                    m_worker.join();
                }
            }

            ~EventLoopThread()
            {
                join();
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
         * @brief 观察停止请求的测试连接
         *
         * @details start() 只按固定节奏轮询自己的 Cancelable，一次都不读套接字。
         *          这样「服务器 close() → ConnectionManager::shutdown() → 本连接收尾」这条路径
         *          不依赖「描述符被关掉时协程正挂在 epoll 上」这一平台相关行为，用例才稳定。
         */
        class ObservingStopConnection final : public Core::Connection
        {
        public:
            /**
             * @brief 构造连接
             * @param loop 所属事件循环，用于建退避用的定时器
             * @param socket 已接受的套接字，所有权转交基类
             * @param stopObserved 观察到停止请求后置位的标记
             * @param markBusy 是否自报「有在途工作」：置位后 drain 必须为它让路，只有强关路径能收掉它
             */
            ObservingStopConnection(Core::EventLoop &loop, Core::AsyncSocket socket, std::atomic<bool> &stopObserved,
                                    const bool markBusy) :
                Core::Connection(std::move(socket)), m_stopObserved(&stopObserved), m_timer(loop)
            {
                // 协议层才会维护这个标记，测试连接直接置位，用来把「等」与「强关」两条路径区分开
                setBusy(markBusy);
            }

            /**
             * @brief 会话协程：等到连接被请求停止为止
             * @details 重写 Core::Connection::start()。基类默认实现是立即完成的空协程，
             *          这里用「长期在线」的形态，用来观察服务器的优雅关闭是否真的通知到了连接。
             * @return Core::Task<> 协程，收到停止请求后完成
             */
            Core::Task<> start() override
            {
                while (!cancelable().isStopRequested())
                {
                    co_await m_timer.waitFor(kStopPollInterval);
                }
                m_stopObserved->store(true, std::memory_order_release);
                co_return;
            }

        private:
            std::atomic<bool> *m_stopObserved{nullptr}; ///< 观察到的停止请求写回这里
            Core::Timer       m_timer;                  ///< 轮询节拍器
        };

        /**
         * @brief 最小可运行的 TcpServer 测试子类
         * @details createConnection 是纯虚钩子，不重写就无法实例化服务器；本子类把钩子的
         *          三种行为（正常/返回空指针/抛异常）做成可配置开关，并把基类 protected 成员
         *          透出成只读访问器，供用例观测「实际监听端口 / 活跃连接数 / 钩子调用次数」。
         */
        class TestTcpServer final : public TcpServer
        {
        public:
            /**
             * @brief 构造测试服务器
             * @param loop 事件循环
             * @param address 监听地址（端口 0 表示让内核分配）
             * @param options 钩子行为、连接类型与并发上限
             * @param stopObserved 交给连接对象的停止观察标记
             */
            TestTcpServer(Core::EventLoop &loop, const Core::InetAddress &address, const ServerTestOptions &options,
                          std::atomic<bool> &stopObserved) :
                TcpServer(loop, address), m_options(options), m_stopObserved(&stopObserved)
            {
                // 并发上限必须在 start() 之前定下，与基类的调用契约一致
                setMaxConnections(options.maxConnections);
                setPerIpConnectionLimiter(options.perIpLimiter);
            }

            /**
             * @brief 为重 accepted 的连接创建会话对象
             * @details 重写 TcpServer::createConnection()（基类为纯虚）。这里按配置开关决定
             *          返回正常连接、返回空指针还是抛异常，并顺手记录套接字两端的端口，
             *          用来证明基类确实把这条连接原样交给了子类钩子。
             * @param socket 已 accept 的套接字，所有权按配置转移或随形参析构关闭
             * @return std::shared_ptr<Core::Connection> 按 m_options 决定的连接对象，或空指针
             */
            [[nodiscard]] std::shared_ptr<Core::Connection> createConnection(Core::AsyncSocket socket) override
            {
                m_createConnectionCalls.fetch_add(1, std::memory_order_release);
                m_recordedLocalPort.store(socket.localAddress().port(), std::memory_order_release);
                m_recordedPeerPort.store(socket.remoteAddress().port(), std::memory_order_release);

                if (m_options.mode == CreateConnectionMode::ReturnsNullPointer)
                {
                    // 形参在此析构：这条连接被基类丢弃并关闭
                    return nullptr;
                }

                if (m_options.mode == CreateConnectionMode::ThrowsOnce && m_throwRemaining.load(std::memory_order_acquire) > 0)
                {
                    m_throwRemaining.fetch_sub(1, std::memory_order_acq_rel);
                    throw Base::Exception("测试用：会话对象创建失败");
                }

                if (m_options.kind == ConnectionKind::ObservesStopRequest)
                {
                    return std::make_shared<ObservingStopConnection>(m_loop, std::move(socket), *m_stopObserved, m_options.markBusy);
                }
                return std::make_shared<Core::Connection>(std::move(socket));
            }

            /// 监听描述符（供用例查询内核实际分配的端口）
            [[nodiscard]] int listenDescriptor() const
            {
                return m_acceptor.fileDescriptor();
            }

            /// createConnection 被调用的次数
            [[nodiscard]] std::size_t createConnectionCalls() const
            {
                return m_createConnectionCalls.load(std::memory_order_acquire);
            }

            /// 最近一次交给钩子的套接字所在的服务端端口
            [[nodiscard]] std::uint16_t recordedLocalPort() const
            {
                return m_recordedLocalPort.load(std::memory_order_acquire);
            }

            /// 最近一次交给钩子的套接字的对端端口
            [[nodiscard]] std::uint16_t recordedPeerPort() const
            {
                return m_recordedPeerPort.load(std::memory_order_acquire);
            }

            /// 当前挂在连接管理器上的活跃连接数
            [[nodiscard]] std::size_t activeConnectionCount() const
            {
                return m_connectionManager.activeCount();
            }

            /// 让「首次调用抛异常」开关重新武装
            void armSingleThrow()
            {
                m_throwRemaining.store(1, std::memory_order_release);
            }

        private:
            ServerTestOptions m_options;                             ///< 钩子配置
            std::atomic<bool> *m_stopObserved{nullptr};              ///< 交给连接的观察标记
            std::atomic<std::size_t> m_createConnectionCalls{0};     ///< 钩子调用次数
            std::atomic<std::uint16_t> m_recordedLocalPort{0};       ///< 钩子收到的服务端端口
            std::atomic<std::uint16_t> m_recordedPeerPort{0};        ///< 钩子收到的对端端口
            std::atomic<int> m_throwRemaining{0};                    ///< 剩余需要抛异常的次数
        };

        /**
         * @brief 跑起 TcpServer 的夹具
         * @details 成员顺序即生命周期顺序：循环 → 停止观察标记 → 结果槽 → 服务器 →
         *          主协程任务 → 循环线程（观察标记与结果槽必须先于服务器和任务构造，
         *          因为它们的引用要传进去）。析构时先由本类析构体请求停止接受循环，
         *          再按逆序 join 线程、销毁协程帧与服务器。
         */
        class RunningServerFixture
        {
        public:
            explicit RunningServerFixture(const ServerTestOptions &options = {}) :
                m_loop(),
                m_server(m_loop, Core::InetAddress::localhost(0), options, m_stopObserved),
                m_serverTask(driveStart(m_server, m_outcome)),
                m_loopThread(m_loop)
            {
                m_loopThread.schedule(m_serverTask);
            }

            ~RunningServerFixture()
            {
                // 顺序要紧：先让循环线程停手并退出，再收尾服务器。挂起的等待器（accept 的
                // 事件注册、清扫协程的定时器登记）都活在循环内部的结构里，而收尾会销毁这些
                // 协程帧；在循环仍在跑的时候从本线程销毁它们，等于跨线程改动那些无锁结构
                m_loopThread.join();
                m_server.close();
            }

            RunningServerFixture(const RunningServerFixture &) = delete;
            RunningServerFixture &operator=(const RunningServerFixture &) = delete;

            /// 主协程是否已按预期结束
            [[nodiscard]] bool awaitServerStopped(const std::chrono::milliseconds timeout)
            {
                return waitForCondition(
                        [this]
                        {
                            return m_outcome.stopped.load(std::memory_order_acquire);
                        },
                        timeout);
            }

            /// 服务器是否已进入接受循环
            [[nodiscard]] bool awaitRunning(const std::chrono::milliseconds timeout)
            {
                return waitForCondition(
                        [this]
                        {
                            return m_server.isRunning();
                        },
                        timeout);
            }

            /// 连接是否已观察到停止请求
            [[nodiscard]] bool awaitStopObserved(const std::chrono::milliseconds timeout)
            {
                return waitForCondition(
                        [this]
                        {
                            return m_stopObserved.load(std::memory_order_acquire);
                        },
                        timeout);
            }

            /**
             * @brief 把 drain() 投到循环线程并等它跑完
             * @details drain 只能在所属循环线程上运行，因此按 scheduleRemote 投递；任务对象由成员持有到
             *          结束，协程帧才不会被提前销毁。完成标记每次调用先清空，可重复调用。
             * @param drainTimeout 交给 drain 的最长等待时长
             * @param waitTimeout 本方法自身的等待上限
             * @return true drain 在时限内完成
             */
            [[nodiscard]] bool drainServer(const std::chrono::milliseconds drainTimeout, const std::chrono::milliseconds waitTimeout)
            {
                m_drainFinished.store(false, std::memory_order_release);
                m_drainTask = driveDrain(m_server, m_drainFinished, drainTimeout);
                m_loopThread.schedule(m_drainTask);
                return waitForCondition(
                        [this]
                        {
                            return m_drainFinished.load(std::memory_order_acquire);
                        },
                        waitTimeout);
            }

            [[nodiscard]] TestTcpServer &server() noexcept { return m_server; }

            /**
             * @brief 在循环线程上执行一段动作，并等它做完
             *
             * @details TcpServer::stop()/close() 的线程约束是「必须由运行本服务器事件循环的那个线程调用」：
             *          它们关掉的是监听描述符与每条活跃连接的套接字，而那些 IoWatcher 正被该循环读写，
             *          从外部线程直接调就是与循环抢同一批句柄（TSan 在并发用例集里报的正是这一处）。
             *          文档给的正路是投递（scheduler().postRemote()）——用例从测试线程发起停止时走这条，
             *          顺带把这条正路本身也测到了。
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
            [[nodiscard]] ServerOutcome &outcome() noexcept { return m_outcome; }
            [[nodiscard]] int listenDescriptor() const { return m_server.listenDescriptor(); }

        private:
            /**
             * @brief 把 start() 包一层，记录它的退出方式
             * @param server 被测服务器
             * @param outcome 结果槽
             * @return Core::Task<> 协程，start() 返回后置位 stopped
             */
            static Core::Task<> driveStart(TestTcpServer &server, ServerOutcome &outcome)
            {
                try
                {
                    co_await server.start();
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
                outcome.stopped.store(true, std::memory_order_release);
                co_return;
            }

            /**
             * @brief 把 drain() 包一层，跑完即置位完成标记
             * @param server 被测服务器
             * @param drainFinished 输出：drain 是否已返回
             * @param drainTimeout 交给 drain 的最长等待时长
             * @return Core::Task<> 协程，drain 返回后完成
             */
            static Core::Task<> driveDrain(TestTcpServer &server, std::atomic<bool> &drainFinished, const std::chrono::milliseconds drainTimeout)
            {
                co_await server.drain(drainTimeout);
                drainFinished.store(true, std::memory_order_release);
                co_return;
            }

            Core::EventLoop m_loop;        ///< 事件循环本体
            std::atomic<bool> m_stopObserved{false}; ///< 连接观察到停止请求的标记，必须先于服务器构造
            ServerOutcome   m_outcome;     ///< 主协程结果槽，必须先于任务构造
            TestTcpServer   m_server;      ///< 被测服务器
            Core::Task<>    m_serverTask;  ///< 由 driveStart 产生的主协程任务
            std::atomic<bool> m_drainFinished{false}; ///< drain 是否已返回，必须先于 drain 任务构造
            Core::Task<>    m_drainTask{nullptr};   ///< 由 driveDrain 产生的 drain 协程任务
            EventLoopThread m_loopThread;  ///< 承载 run() 的线程，最后构造、最先析构
        };

        /**
         * @brief 查询描述符上由内核实际分配的本地端口
         * @details TcpServer 不对外暴露监听器，端口只能从监听描述符问出来；
         *          TcpAcceptor::localAddress() 有意只回请求值，不适用于此。
         * @param descriptor 监听描述符
         * @return std::uint16_t 实际端口，失败返回 0
         */
        std::uint16_t queryBoundPort(const int descriptor)
        {
            sockaddr_in  address{};
            socklen_t    addressLength = static_cast<socklen_t>(sizeof(address));
            if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &addressLength) != 0)
            {
                return 0;
            }
            return ntohs(address.sin_port);
        }

        /**
         * @brief 一条到 127.0.0.1 指定端口的普通阻塞客户端连接
         * @details 客户端刻意不用框架的 AsyncSocket：测试只需要「有连接到达」这件事，
         *          不希望再引入第二套事件循环。析构即关闭，不给用例留残余连接。
         */
        class LoopbackClient
        {
        public:
            explicit LoopbackClient(const std::uint16_t port)
            {
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
                    Platform::FileDescriptor::close(m_descriptor);
                    m_descriptor = Platform::FileDescriptor::kInvalid;
                    return;
                }

                sockaddr_in  localAddress{};
                socklen_t    localLength = static_cast<socklen_t>(sizeof(localAddress));
                if (::getsockname(m_descriptor, reinterpret_cast<sockaddr *>(&localAddress), &localLength) == 0)
                {
                    m_localPort = ntohs(localAddress.sin_port);
                }
            }

            ~LoopbackClient()
            {
                Platform::FileDescriptor::close(m_descriptor);
            }

            LoopbackClient(const LoopbackClient &) = delete;
            LoopbackClient &operator=(const LoopbackClient &) = delete;

            [[nodiscard]] bool isValid() const noexcept
            {
                return Platform::FileDescriptor::isValid(m_descriptor);
            }

            /// 本端随机分配的源端口
            [[nodiscard]] std::uint16_t localPort() const noexcept
            {
                return m_localPort;
            }

            /// 关闭本端，模拟「客户端先断开」
            void closeNow() noexcept
            {
                Platform::FileDescriptor::close(m_descriptor);
                m_descriptor = Platform::FileDescriptor::kInvalid;
            }

        private:
            Platform::Socket::Initialization m_socketInitialization; ///< 保证 Winsock 在本对象存活期间保持初始化
            int                             m_descriptor{Platform::FileDescriptor::kInvalid}; ///< 客户端描述符
            std::uint16_t                   m_localPort{0};                                   ///< 本端源端口
        };
    } // namespace

    TEST(TcpServer, CreateConnectionIsPureVirtual)
    {
        // createConnection 现在是纯虚钩子：基类既不能被构造、也不能被复制搬移
        static_assert(std::is_abstract_v<TcpServer>, "TcpServer 必须是抽象基类，靠 createConnection 装载协议");
        static_assert(!std::is_default_constructible_v<TcpServer>, "TcpServer 需要事件循环与监听地址，不能默认构造");
        static_assert(!std::is_constructible_v<TcpServer, Core::EventLoop &, const Core::InetAddress &>,
                      "仅重写别的钩子不足以实例化 TcpServer：createConnection 仍是纯虚");
        static_assert(!std::is_copy_constructible_v<TcpServer>, "TcpServer 禁止拷贝");
        static_assert(!std::is_move_constructible_v<TcpServer>, "TcpServer 禁止移动");
        static_assert(std::is_abstract_v<TestTcpServer> == false, "重写 createConnection 后即可实例化");
        SUCCEED() << "以上均为编译期断言";
    }

    TEST(TcpServer, ServerReportsNotRunningBeforeStart)
    {
        Core::EventLoop loop;
        std::atomic<bool> stopObserved{false};
        TestTcpServer     server(loop, Core::InetAddress::localhost(0), ServerTestOptions{}, stopObserved);

        // 构造不绑定也不监听：start() 才是唯一入口
        EXPECT_FALSE(server.isRunning());
        EXPECT_EQ(server.activeConnectionCount(), 0u);
        EXPECT_EQ(server.createConnectionCalls(), 0u);
    }

    TEST(TcpServer, StartBindsListensAndEntersRunningState)
    {
        RunningServerFixture fixture;

        // start() 依次 bind() 与 listen(kDefaultListenBacklog)，两步都成功才会置位运行标志
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "start() 未在时限内进入接受循环：上界 kWaitTimeout";
        EXPECT_NE(queryBoundPort(fixture.listenDescriptor()), 0);
        EXPECT_EQ(fixture.server().createConnectionCalls(), 0u);
    }

    TEST(TcpServer, AcceptedSocketIsHandedToCreateConnection)
    {
        RunningServerFixture fixture;
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        const LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环客户端连接失败，钩子无从被触发";

        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().createConnectionCalls() >= 1u;
                },
                kWaitTimeout)) << "新连接未在时限内到达 createConnection：上界 kWaitTimeout";
        // 钩子拿到的必须就是这条连接：两端端口都对得上
        EXPECT_EQ(fixture.server().recordedLocalPort(), listeningPort);
        EXPECT_EQ(fixture.server().recordedPeerPort(), client.localPort());
    }

    TEST(TcpServer, NullPointerConnectionIsDiscardedWithoutStoppingAcceptLoop)
    {
        ServerTestOptions options;
        options.mode = CreateConnectionMode::ReturnsNullPointer;
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        // 返回 nullptr 属于子类缺陷：基类记一条中文错误并丢弃这条连接，
        // 但绝不让空连接进连接管理器，也不终止接受循环
        const LoopbackClient firstClient(listeningPort);
        ASSERT_TRUE(firstClient.isValid());
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().createConnectionCalls() >= 1u;
                },
                kWaitTimeout));

        const LoopbackClient secondClient(listeningPort);
        ASSERT_TRUE(secondClient.isValid());
        EXPECT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().createConnectionCalls() >= 2u;
                },
                kWaitTimeout)) << "空连接把接受循环带停了：上界 kWaitTimeout";
        EXPECT_EQ(fixture.server().activeConnectionCount(), 0u);
        EXPECT_TRUE(fixture.server().isRunning());
    }

    TEST(TcpServer, ThrowingCreateConnectionOnlyDropsThatConnection)
    {
        ServerTestOptions options;
        options.mode = CreateConnectionMode::ThrowsOnce;
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));
        fixture.server().armSingleThrow();

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        // 子类钩子抛异常只是这一条连接建会话失败：基类记录中文错误后继续接受下一条
        const LoopbackClient failingClient(listeningPort);
        ASSERT_TRUE(failingClient.isValid());
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().createConnectionCalls() >= 1u;
                },
                kWaitTimeout));

        const LoopbackClient secondClient(listeningPort);
        ASSERT_TRUE(secondClient.isValid());
        EXPECT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().createConnectionCalls() >= 2u;
                },
                kWaitTimeout)) << "一次钩子异常就让接受循环停摆：上界 kWaitTimeout";
        EXPECT_TRUE(fixture.server().isRunning());
    }

    TEST(TcpServer, MaxConnectionsDropsExtraConnectionBeforeCallingHook)
    {
        ServerTestOptions options;
        options.kind = ConnectionKind::ObservesStopRequest;
        options.maxConnections = 1;
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        const LoopbackClient firstClient(listeningPort);
        ASSERT_TRUE(firstClient.isValid());
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().activeConnectionCount() >= 1u;
                },
                kWaitTimeout)) << "首条连接未在时限内挂上管理器：上界 kWaitTimeout";

        // 第二条连接在达到上限后到达：过载保护在钩子之前生效，直接丢弃新连接，
        // 连 createConnection 都不必调用。
        const LoopbackClient secondClient(listeningPort);
        ASSERT_TRUE(secondClient.isValid());
        // 时序说明：这里用 kNegativeCheckTimeout（200ms）的观察窗口证明「第二次钩子没被调用」，
        // 上界之内没发生就当作不发生，不会把用例挂住。
        EXPECT_FALSE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().createConnectionCalls() >= 2u;
                },
                kNegativeCheckTimeout));
        EXPECT_EQ(fixture.server().createConnectionCalls(), 1u);
        EXPECT_EQ(fixture.server().activeConnectionCount(), 1u);

        fixture.runOnLoopAndWait([&fixture] { fixture.server().close(); });
        EXPECT_TRUE(fixture.awaitStopObserved(kWaitTimeout));
        EXPECT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().activeConnectionCount() == 0u;
                },
                kWaitTimeout)) << "连接协程结束后未从管理器摘除：上界 kWaitTimeout";
    }

    TEST(TcpServer, PerIpLimitDropsExtraConnectionFromSameSource)
    {
        ServerTestOptions options;
        options.kind = ConnectionKind::ObservesStopRequest;
        options.perIpLimiter = std::make_shared<PerIpConnectionLimiter>(1);
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        // 两条连接都来自回环：对端地址相同、端口不同。限额的键取地址本身，所以它们算同一个来源
        const LoopbackClient firstClient(listeningPort);
        ASSERT_TRUE(firstClient.isValid());
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().activeConnectionCount() >= 1u;
                },
                kWaitTimeout)) << "首条连接未在时限内挂上管理器：上界 kWaitTimeout";

        const LoopbackClient secondClient(listeningPort);
        ASSERT_TRUE(secondClient.isValid());
        // 同源的第二条：名额已满，应当连 createConnection 都不调用（上界之内没发生就当作不发生）
        EXPECT_FALSE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().createConnectionCalls() >= 2u;
                },
                kNegativeCheckTimeout));
        EXPECT_EQ(fixture.server().createConnectionCalls(), 1u);
        EXPECT_EQ(options.perIpLimiter->activeCountFor("127.0.0.1"), 1u);

        fixture.runOnLoopAndWait([&fixture] { fixture.server().close(); });
        EXPECT_TRUE(fixture.awaitStopObserved(kWaitTimeout));
        // 连接结束后名额必须还回去：否则这个来源被永久锁在限额上，它再也连不进来
        EXPECT_TRUE(waitForCondition(
                [&options]
                {
                    return options.perIpLimiter->activeCountFor("127.0.0.1") == 0u;
                },
                kWaitTimeout)) << "连接结束后按 IP 的名额未归还：上界 kWaitTimeout";
    }

    TEST(TcpServer, CloseShutsDownActiveConnectionThroughConnectionManager)
    {
        ServerTestOptions options;
        options.kind = ConnectionKind::ObservesStopRequest;
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        const LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().activeConnectionCount() >= 1u;
                },
                kWaitTimeout)) << "连接未在时限内挂上管理器：上界 kWaitTimeout";

        // close() = stop() + ConnectionManager::shutdown()：正在存活的连接会被请求停止并关掉描述符
        fixture.runOnLoopAndWait([&fixture] { fixture.server().close(); });
        EXPECT_TRUE(fixture.awaitStopObserved(kWaitTimeout)) << "shutdown 没通知到活跃连接：上界 kWaitTimeout";
        EXPECT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().activeConnectionCount() == 0u;
                },
                kWaitTimeout)) << "连接协程收尾后没被摘除：上界 kWaitTimeout";
    }

    TEST(TcpServer, StopLetsAcceptLoopFinishWithoutEscapingException)
    {
        RunningServerFixture fixture;
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        // stop() 关掉监听器后，挂起的 accept 会以 nullopt（或被捕获的终止性错误）收场，
        // start() 随即走完「等待全部连接任务」的收尾并返回，且不把异常抛给调度器。
        // 时序说明：这一步依赖「关闭监听描述符能把挂在 epoll 上的协程唤醒」，
        // 在 Windows/wepoll 上最可能不稳，故留 2 秒上界，超时只判失败不挂用例。
        fixture.runOnLoopAndWait([&fixture] { fixture.server().stop(); });
        EXPECT_TRUE(fixture.awaitServerStopped(kWaitTimeout)) << "stop() 后 start() 未在时限内结束：上界 kWaitTimeout";
        EXPECT_EQ(fixture.outcome().failure, FailureKind::None) << "start() 把接受错误抛到了调度器之外";
        EXPECT_FALSE(fixture.server().isRunning());
    }

    TEST(TcpServer, DrainClosesIdleConnectionWithoutWaitingForDeadline)
    {
        // 钉住：没有在途工作的连接在 drain 的第一轮就被收掉，因此 drain 远早于 drainTimeout 返回。
        // 连接对象只轮询停止请求、从不读套接字，所以这里的「立刻」不依赖任何平台相关的关闭唤醒行为
        ServerTestOptions options;
        options.kind = ConnectionKind::ObservesStopRequest;
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        const LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环客户端连接失败";
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().activeConnectionCount() >= 1u;
                },
                kWaitTimeout)) << "连接未在时限内挂上管理器：上界 kWaitTimeout";

        // 期限给得远大于用例的等待上界：一旦 drain 真的按期限等，下面两条断言必然失败
        constexpr std::chrono::milliseconds drainTimeout{8000};
        const auto                               drainStartTime = std::chrono::steady_clock::now();
        ASSERT_TRUE(fixture.drainServer(drainTimeout, kWaitTimeout)) << "drain 未在时限内完成：上界 kWaitTimeout";
        const std::chrono::milliseconds drainElapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - drainStartTime);

        // 空闲连接被请求停止并关闭（而不是被留在那里等自己结束）
        EXPECT_TRUE(fixture.awaitStopObserved(kWaitTimeout)) << "drain 没有通知到空闲连接：上界 kWaitTimeout";
        EXPECT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().activeConnectionCount() == 0u;
                },
                kWaitTimeout)) << "空闲连接收尾后未从管理器摘除：上界 kWaitTimeout";
        EXPECT_LT(drainElapsed, drainTimeout) << "drain 等满了期限：空闲连接没有被立刻收掉，耗时 " << drainElapsed.count() << "ms";
        EXPECT_FALSE(fixture.server().isRunning());
    }

    TEST(TcpServer, NonPositiveDrainTimeoutForceClosesBusyConnectionImmediately)
    {
        // 钉住：drainTimeout 非正数时 drain 不等任何连接——连自报「有在途工作」的连接也被立刻强关，
        // 因此它等价于 close()。连接自报忙碌是关键前提：否则「没等」与「本来就空闲」分辨不开
        ServerTestOptions options;
        options.kind     = ConnectionKind::ObservesStopRequest;
        options.markBusy = true;
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        const LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环客户端连接失败";
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().activeConnectionCount() >= 1u;
                },
                kWaitTimeout)) << "连接未在时限内挂上管理器：上界 kWaitTimeout";

        // 期限为 0：不等待，直接强关。上界取 kImmediateCompletionTimeout，超出即说明 drain 在等
        ASSERT_TRUE(fixture.drainServer(std::chrono::milliseconds::zero(), kWaitTimeout)) << "drain 未在时限内完成：上界 kWaitTimeout";
        EXPECT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().activeConnectionCount() == 0u;
                },
                kImmediateCompletionTimeout)) << "非正期限下忙碌连接没有被立刻强关：上界 kImmediateCompletionTimeout";
        EXPECT_TRUE(fixture.awaitStopObserved(kWaitTimeout)) << "强关没有通知到连接：上界 kWaitTimeout";
        EXPECT_FALSE(fixture.server().isRunning());
    }
} // namespace AsynGyanis::Net
