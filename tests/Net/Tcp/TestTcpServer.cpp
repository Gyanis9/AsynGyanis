// TcpServer 单元测试：纯虚钩子、接受循环、连接丢弃与 ConnectionManager 参与的优雅关闭（stop/close/drain）
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

#include "CoreTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <functional>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

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
            bool listenOnIpv6Any{false};                             ///< 绑 `::` 而非回环：双栈监听器会同时接住 IPv4 客户端
            bool proxyProtocolRequired{false};                       ///< 要求每条连接先送一个 PROXY 协议头
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
         * @brief 在超时上限内逐毫秒轮询等待条件成立（定义见 CoreTestSupport.h，各调用点自带超时）
         */
        using AsynGyanis::Core::TestSupport::waitForCondition;

        /**
         * @brief 在独立线程上驱动 EventLoop 的夹具（定义见 CoreTestSupport.h，借用模式）
         * @note 必须声明在被投协程任务之后：析构顺序保证「先 join 循环线程，再销毁协程帧」，
         *       否则循环线程可能恢复一个已被销毁的句柄
         */
        using AsynGyanis::Core::TestSupport::EventLoopThread;

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
                setProxyProtocolRequired(options.proxyProtocolRequired);
            }

            /**
             * @brief 用「已经在监听中的套接字」构造测试服务器：不 bind、不 listen
             * @details 这条构造入口是零停机换代的落点（新一代直接用交来的描述符接受连接），
             *          所以它与按地址构造共享同一份钩子配置，用例只需换构造方式。
             * @param loop 事件循环
             * @param adoptedListeningDescriptor 已在监听状态的文件描述符，所有权交给基类
             * @param options 钩子行为、连接类型与并发上限
             * @param stopObserved 交给连接对象的停止观察标记
             */
            TestTcpServer(Core::EventLoop &loop, const int adoptedListeningDescriptor, const ServerTestOptions &options,
                          std::atomic<bool> &stopObserved) :
                TcpServer(loop, adoptedListeningDescriptor), m_options(options), m_stopObserved(&stopObserved)
            {
                setMaxConnections(options.maxConnections);
                setPerIpConnectionLimiter(options.perIpLimiter);
                setProxyProtocolRequired(options.proxyProtocolRequired);
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
                // 先记载荷、**最后**才把「调用次数」放出去：次数是等待方判定「到了」的信号，
                // 先加计数再写端口会让等待方在端口还是 0 时就往下走（x86 上先写的计数先可见，
                // Windows CI 实测读到 recordedLocalPort()==0）
                m_recordedLocalPort.store(socket.localAddress().port(), std::memory_order_release);
                m_recordedPeerPort.store(socket.remoteAddress().port(), std::memory_order_release);
                {
                    // 记下来的是「本条连接被当成谁」：PROXY 头改写过的身份要能被断言到
                    const std::lock_guard<std::mutex> peerGuard(m_seenPeersMutex);
                    m_seenPeers.push_back(socket.remoteAddress().ip());
                }
                m_createConnectionCalls.fetch_add(1, std::memory_order_release);

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

            /// 到目前为止被钩子见过的对端 IP（按到达顺序）：PROXY 协议改造过的那个值
            [[nodiscard]] std::vector<std::string> seenPeers() const
            {
                const std::lock_guard<std::mutex> guard(m_seenPeersMutex);
                return m_seenPeers;
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
            mutable std::mutex m_seenPeersMutex;                     ///< 保护下面的到达顺序表
            std::vector<std::string> m_seenPeers;                    ///< 每次钩子调用对应的对端 IP
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
                m_server(m_loop, makeListenAddress(options), options, m_stopObserved),
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
             * @details TcpServer::stop()/close() 必须由运行本服务器事件循环的线程调用：它们关掉的监听
             *          描述符与连接套接字正被该循环读写，从外部线程直接调就是与循环抢同一批句柄。
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
             * @brief 按选项决定监听地址
             * @details 双栈档绑 `::`：TcpAcceptor 会关掉 IPV6_V6ONLY，于是 IPv4 客户端也连得进来，
             *          而对端地址族仍是 AF_INET6——这正是「同一来源有两种地址写法」的那一半前提。
             *          地址直接按 sockaddr_in6 造而不走 InetAddress::resolve()：后者经 getaddrinfo，
             *          「没有全局 IPv6 地址」的机器会解析不出 `::`，而那类机器实测照样能绑上并
             *          收下 IPv4 对端（容器内实测 `::ffff:127.0.0.1`），用例会因此被误判成跳过。
             */
            static Core::InetAddress makeListenAddress(const ServerTestOptions &options)
            {
                if (options.listenOnIpv6Any)
                {
                    sockaddr_in6 any{};
                    any.sin6_family = AF_INET6;
                    any.sin6_addr   = in6addr_any;
                    any.sin6_port   = 0;
                    return Core::InetAddress{any};
                }
                return Core::InetAddress::localhost(0);
            }

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
         *          缓冲区必须按 sockaddr_storage 给：双栈监听器的地址族是 AF_INET6，
         *          只给 sockaddr_in 大小在 Windows 上会让 getsockname 直接失败（不是截断）。
         * @param descriptor 监听描述符
         * @return std::uint16_t 实际端口，失败返回 0
         */
        std::uint16_t queryBoundPort(const int descriptor)
        {
            sockaddr_storage storage{};
            socklen_t        storageLength = static_cast<socklen_t>(sizeof(storage));
            if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&storage), &storageLength) != 0)
            {
                return 0;
            }

            if (storage.ss_family == AF_INET)
            {
                return ntohs(reinterpret_cast<const sockaddr_in *>(&storage)->sin_port);
            }
            if (storage.ss_family == AF_INET6)
            {
                return ntohs(reinterpret_cast<const sockaddr_in6 *>(&storage)->sin6_port);
            }
            return 0;
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

            /**
             * @brief 把一段字节全部写出去（阻塞套接字上 send 会自己写完）
             * @param bytes 要发的字节
             * @return true 全部发出
             */
            bool sendAll(const std::string_view bytes) const
            {
                std::size_t sent = 0U;
                while (sent < bytes.size())
                {
                    const int written = ::send(m_descriptor, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
                    if (written <= 0)
                    {
                        return false;
                    }
                    sent += static_cast<std::size_t>(written);
                }
                return true;
            }

            /// 本端是否已被对端收尾（读到 0 或错误都算）：用于断言「服务器把这条连接关了」
            [[nodiscard]] bool isClosedByPeer() const
            {
                // 非阻塞地问一句：这条判断要放进轮询等待里，而客户端套接字是阻塞的——直接 recv
                // 会一直睡到对端有动作，等待方就再也没有下一次轮询了（实测把用例挂死在这里）
                fd_set readSet{};
                FD_ZERO(&readSet);
                FD_SET(m_descriptor, &readSet);
                timeval timeout{0, 20 * 1000};
                if (::select(m_descriptor + 1, &readSet, nullptr, nullptr, &timeout) <= 0)
                {
                    return false;
                }
                char probe{};
                return ::recv(m_descriptor, &probe, 1, 0) <= 0;
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
        // 端口同理：绑定发生在 start() 里，此前没有任何端口可报
        EXPECT_EQ(server.listeningPort(), 0);
        EXPECT_EQ(server.activeConnectionCount(), 0u);
        EXPECT_EQ(server.createConnectionCalls(), 0u);
    }

    TEST(TcpServer, StartBindsListensAndEntersRunningState)
    {
        RunningServerFixture fixture;

        // start() 依次 bind() 与 listen(kDefaultListenBacklog)，两步都成功才会置位运行标志
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "start() 未在时限内进入接受循环：上界 kWaitTimeout";
        // 端口 0 交给内核挑，因此公开 accessor 必须报出**实际**端口，而不是当初传进去的那个 0。
        // 对照判据仍取 getsockname：那是内核口径，accessor 与它不一致就是回填没做
        const std::uint16_t kernelPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(kernelPort, 0);
        EXPECT_EQ(fixture.server().listeningPort(), kernelPort) << "listeningPort() 与实际绑定端口不一致";
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

    /**
     * @brief 双栈监听器上进来的 IPv4 客户端，按点分地址那一格记账
     * @details 这条用例钉的是**线上真实形态**而不是键的字符串处理：服务器绑 `::`（接受器会关掉
     *          IPV6_V6ONLY），客户端连 127.0.0.1，于是对端地址族是 AF_INET6、文本带 `::ffff:` 前缀。
     *          限额若按这个原文分格，同一来源经纯 IPv4 监听器就会另占一格，上限实际翻倍
     */
    TEST(TcpServer, DualStackListenerAccountsIpv4PeerAsDottedSource)
    {
        ServerTestOptions options;
        options.kind            = ConnectionKind::ObservesStopRequest;
        options.perIpLimiter    = std::make_shared<PerIpConnectionLimiter>(1);
        options.listenOnIpv6Any = true;
        RunningServerFixture fixture(options);
        if (!fixture.awaitRunning(kWaitTimeout))
        {
            GTEST_SKIP() << "本机不能在 :: 上建立双栈监听器（IPv6 不可用），端到端这一半无从构造";
        }

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        const LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "IPv4 客户端连不上双栈监听器：这条监听器没接住另一族";
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().createConnectionCalls() >= 1u;
                },
                kWaitTimeout)) << "双栈监听器上的 IPv4 连接没有走到建连钩子";

        // 不带前缀的写法要能看到这一条：记账格与观测读数是同一格
        EXPECT_EQ(options.perIpLimiter->activeCountFor("127.0.0.1"), 1u) << "映射地址没折成点分本体，按 IP 的上限可被写法绕过";
        EXPECT_EQ(options.perIpLimiter->activeCountFor("::ffff:127.0.0.1"), 1u) << "查询侧折键与记账侧不一致";

        // 同源第二条：名额已被这一族的那个写法占满
        const LoopbackClient secondClient(listeningPort);
        ASSERT_TRUE(secondClient.isValid());
        EXPECT_FALSE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().createConnectionCalls() >= 2u;
                },
                kNegativeCheckTimeout)) << "换一种地址写法就能再占一个名额";

        fixture.runOnLoopAndWait([&fixture] { fixture.server().close(); });
        EXPECT_TRUE(waitForCondition(
                [&options]
                {
                    return options.perIpLimiter->activeCountFor("127.0.0.1") == 0u;
                },
                kWaitTimeout)) << "收尾后点分那一格的名额未归还";
    }

    /**
     * @brief 同一份限额挂在纯 IPv4 与双栈两台监听器上时，一个来源只有一个名额
     * @details 这就是本类设计前提里的那种接法（多监听器共享同一份计数，否则上限会按监听器数量翻倍）。
     *          同一个 IPv4 客户端在两侧的键写法不同——`127.0.0.1` 与 `::ffff:127.0.0.1`——不折键时
     *          两台各占一格，「单个来源」的上限实际是配置值的两倍
     */
    TEST(TcpServer, OneLimiterSharedByIpv4AndDualStackListenersAllowsSingleConnection)
    {
        auto limiter = std::make_shared<PerIpConnectionLimiter>(1);

        ServerTestOptions v4OnlyOptions;
        v4OnlyOptions.kind         = ConnectionKind::ObservesStopRequest;
        v4OnlyOptions.perIpLimiter = limiter;
        RunningServerFixture v4OnlyFixture(v4OnlyOptions);

        ServerTestOptions dualStackOptions;
        dualStackOptions.kind          = ConnectionKind::ObservesStopRequest;
        dualStackOptions.perIpLimiter  = limiter;
        dualStackOptions.listenOnIpv6Any = true;
        RunningServerFixture dualStackFixture(dualStackOptions);

        ASSERT_TRUE(v4OnlyFixture.awaitRunning(kWaitTimeout)) << "IPv4 监听器未进入接受循环";
        if (!dualStackFixture.awaitRunning(kWaitTimeout))
        {
            GTEST_SKIP() << "本机不能在 :: 上建立双栈监听器（IPv6 不可用），两台共享一份限额这一形态无从构造";
        }

        const std::uint16_t v4OnlyPort = queryBoundPort(v4OnlyFixture.listenDescriptor());
        const std::uint16_t dualStackPort = queryBoundPort(dualStackFixture.listenDescriptor());
        ASSERT_NE(v4OnlyPort, 0);
        ASSERT_NE(dualStackPort, 0);

        const LoopbackClient firstClient(v4OnlyPort);
        ASSERT_TRUE(firstClient.isValid()) << "回环连接失败";
        ASSERT_TRUE(waitForCondition(
                [&v4OnlyFixture]
                {
                    return v4OnlyFixture.server().createConnectionCalls() >= 1u;
                },
                kWaitTimeout)) << "第一条连接没挂上 IPv4 监听器";

        // 换一台监听器再连：对端写法不同，但来源是同一个，名额已被占满
        const LoopbackClient secondClient(dualStackPort);
        EXPECT_FALSE(waitForCondition(
                [&dualStackFixture]
                {
                    return dualStackFixture.server().createConnectionCalls() >= 1u;
                },
                kNegativeCheckTimeout))
                << "同一来源换一台监听器就又拿到一个名额：按来源的上限被地址写法稀释";
        EXPECT_EQ(limiter->activeCountFor("127.0.0.1"), 1u) << "两台监听器各记了一格";

        v4OnlyFixture.runOnLoopAndWait([&v4OnlyFixture] { v4OnlyFixture.server().close(); });
        EXPECT_TRUE(waitForCondition(
                [&limiter]
                {
                    return limiter->activeCountFor("127.0.0.1") == 0u;
                },
                kWaitTimeout)) << "第一条连接收尾后共享限额未归还";
    }

    /**
     * @brief 取不到对端地址的描述符只丢掉这一条，不把异常穿出接手路径
     * @details remoteAddress() 在 getpeername 失败时会抛（这里用一条没连上的套接字造出该失败）。
     *          adoptConnection 的契约是「返回真/假、不抛」：接手一条坏描述符不该把整台服务器的接受
     *          路径一起带走。用例同时核对这次失败没占住按 IP 的名额，且服务器此后照常接手新连接
     */
    TEST(TcpServer, AdoptionWithUnusablePeerAddressReturnsFalseWithoutThrowing)
    {
        ServerTestOptions options;
        options.kind = ConnectionKind::ObservesStopRequest;
        options.perIpLimiter = std::make_shared<PerIpConnectionLimiter>(1);
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        // 一条只创建、没连上的套接字：getpeername 对它必然失败
        const int unconnectedDescriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        ASSERT_TRUE(Platform::FileDescriptor::isValid(unconnectedDescriptor));

        bool isAdopted{true};
        bool isThrowing{false};
        fixture.runOnLoopAndWait(
                [&fixture, unconnectedDescriptor, &isAdopted, &isThrowing]
                {
                    try
                    {
                        isAdopted = fixture.server().adoptConnection(unconnectedDescriptor);
                    } catch (...)
                    {
                        isThrowing = true;
                    }
                });
        EXPECT_FALSE(isThrowing) << "接手一条取不到对端地址的描述符把异常抛给了调用方：接受路径会被它一起带走";
        EXPECT_FALSE(isAdopted) << "没接手的这条不该报「已接手」，调用方要靠这个返回值决定下一步";
        EXPECT_EQ(options.perIpLimiter->activeCountFor("127.0.0.1"), 0u) << "失败的那次接手仍占着按 IP 的名额";

        // 接受路径还活着：真连一条进来，钩子就该被调用到
        const LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());
        EXPECT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().createConnectionCalls() >= 1u;
                },
                kWaitTimeout)) << "一次失败的接手之后，服务器不再接受新连接";

        fixture.runOnLoopAndWait([&fixture] { fixture.server().close(); });
        EXPECT_TRUE(fixture.awaitStopObserved(kWaitTimeout));
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
    /**
     * @brief 「接手已在监听的套接字」这条构造入口必须真的能对外服务
     *
     * @details 零停机换代的第二半全靠它：新一代不 bind、不 listen，直接用交来的描述符接受连接。
     *          跨进程的移交本身由 Platform 层的 SocketHandoff 用例覆盖，这条钉的是「引擎拿到那个
     *          描述符之后能不能干活」——该入口此前没有任何直测，而它是整条换代链路上唯一还没
     *          被直接验过的公共面。
     */
    TEST(TcpServerAdoptedListener, ServesConnectionsOnTheHandedOverSocket)
    {
        // 本用例直接用平台套接字，所以自己负责 Winsock 初始化：ctest 是逐用例起进程的，
        // 不能假定「同二进制里别的用例已经把平台初始化好了」
        ASSERT_TRUE(Platform::Socket::initialize());

        // 先自己造一个已在监听状态的套接字：端口交给内核挑，服务器不许再 bind 一次
        const int descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        ASSERT_GE(descriptor, 0);
        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        ASSERT_EQ(::bind(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
        ASSERT_EQ(::listen(descriptor, 16), 0);
        socklen_t addressLength = static_cast<socklen_t>(sizeof(address));
        ASSERT_EQ(::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &addressLength), 0);
        const std::uint16_t listeningPort = ntohs(address.sin_port);
        ASSERT_GT(listeningPort, 0U);

        Core::EventLoop      loop;
        std::atomic<bool>    stopObserved{false};
        std::thread          loopThread([&loop]
        {
            loop.run();
        });

        ServerTestOptions options; ///< 默认 FinishImmediately：连接建好就收口，够证明「确实接受到了」
        TestTcpServer       server(loop, descriptor, options, stopObserved);
        // 描述符的所有权已交给服务器：从这里起不再用 ASSERT，失败也要走完收口路径
        AsynGyanis::Core::Task<void> startTask = server.start();
        loop.scheduler().scheduleRemote(startTask.handle());

        // 接手来的端口应当被原样报出来：偷偷重绑另一个端口，这条先红
        std::uint16_t reportedPort = 0U;
        for (int attempt = 0; attempt < 200 && reportedPort == 0U; ++attempt)
        {
            reportedPort = server.listeningPort();
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        EXPECT_EQ(reportedPort, listeningPort) << "接手入口没有报出交来的端口";

        const int client = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        EXPECT_GE(client, 0);
        if (client >= 0)
        {
            const int connected = ::connect(client, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
            EXPECT_EQ(connected, 0) << "连不上接手来的监听端口：那个套接字没在服务";
            Platform::FileDescriptor::close(client);
        }

        std::size_t createdConnections = 0U;
        for (int attempt = 0; attempt < 200 && createdConnections == 0U; ++attempt)
        {
            createdConnections = server.createConnectionCalls();
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        EXPECT_GE(createdConnections, 1U) << "接受循环没能在交来的描述符上收到连接";
        EXPECT_EQ(server.recordedLocalPort(), listeningPort) << "会话看到的本地端口不是交来的那个";

        // 收口顺序要按线程契约来：close() 只能在跑这条循环的线程上调，所以投递过去，
        // 等接受循环真的退出之后再停循环——否则 startTask 的帧会在协程还挂着时被析构
        loop.scheduler().postRemote([&server]
        {
            server.close();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        loop.stop();
        loopThread.join();
    }

    namespace
    {
        /**
         * @brief 拼一条 v1 PROXY 头
         */
        std::string makeV1Header(const std::string_view source, const int sourcePort,
                                 const std::string_view destination, const int destinationPort)
        {
            return "PROXY TCP4 " + std::string{source} + " " + std::string{destination} + " "
                   + std::to_string(sourcePort) + " " + std::to_string(destinationPort) + "\r\n";
        }

        /**
         * @brief 拼一条 v2 PROXY 头（IPv4 + PROXY 命令）
         */
        std::string makeV2Ipv4Header(const std::array<int, 4> &source, const std::array<int, 4> &destination,
                                     const int sourcePort, const int destinationPort)
        {
            std::string header{"\r\n\r\n\0\r\nQUIT\n", 12};
            header += static_cast<char>(0x21); // 版本 2 + 命令 PROXY
            header += static_cast<char>(0x11); // AF_INET + STREAM
            header += static_cast<char>(0x00); // 地址块长度 12（大端两字节）
            header += static_cast<char>(0x0C);
            for (const int octet: source)
            {
                header += static_cast<char>(octet);
            }
            for (const int octet: destination)
            {
                header += static_cast<char>(octet);
            }
            header += static_cast<char>((sourcePort >> 8) & 0xFF);
            header += static_cast<char>(sourcePort & 0xFF);
            header += static_cast<char>((destinationPort >> 8) & 0xFF);
            header += static_cast<char>(destinationPort & 0xFF);
            return header;
        }
    } // namespace

    /**
     * @brief 要求 PROXY 头时，连接要被记到头上写着的真实来源，而不是代理自己的地址
     * @details 这正是开这个开关的全部理由：回环上所有客户端的对端地址都是 127.0.0.1，不看头就等于
     *          一整个代理的流量共用一个身份
     */
    TEST(TcpServer, AttributesConnectionToTheProxiedSourceFromVersionOneHeader)
    {
        ServerTestOptions options;
        options.kind = ConnectionKind::ObservesStopRequest;
        options.proxyProtocolRequired = true;
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        const LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendAll(makeV1Header("203.0.113.9", 44000, "198.51.100.7", 443)));

        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().seenPeers().size() >= 1U;
                },
                kWaitTimeout))
                << "读完 PROXY 头之后没有建会话：头没被认出来，或者被当场判死";
        const std::vector<std::string> peers = fixture.server().seenPeers();
        EXPECT_EQ(peers[0], "203.0.113.9") << "对端身份没被换成头上写着的来源";
    }

    TEST(TcpServer, AttributesConnectionToTheProxiedSourceFromVersionTwoHeader)
    {
        ServerTestOptions options;
        options.kind = ConnectionKind::ObservesStopRequest;
        options.proxyProtocolRequired = true;
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        const LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendAll(makeV2Ipv4Header({203, 0, 113, 9}, {198, 51, 100, 7}, 44000, 443)));

        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().seenPeers().size() >= 1U;
                },
                kWaitTimeout))
                << "v2 头没被认出来：定长段的长度字段或地址块解析有一条不对";
        EXPECT_EQ(fixture.server().seenPeers()[0], "203.0.113.9");
    }

    /**
     * @brief 头可以分两段到达：读侧要接着读，而不是把第一段当成完整头判死
     */
    TEST(TcpServer, ReadsProxyHeaderSplitAcrossSegments)
    {
        ServerTestOptions options;
        options.kind = ConnectionKind::ObservesStopRequest;
        options.proxyProtocolRequired = true;
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        const LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());
        const std::string header = makeV1Header("203.0.113.21", 1234, "192.0.2.1", 80);
        const std::size_t cut = header.find(" 1234");
        ASSERT_NE(cut, std::string::npos);
        ASSERT_TRUE(client.sendAll(header.substr(0U, cut)));
        ASSERT_TRUE(client.sendAll(header.substr(cut)));

        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().seenPeers().size() >= 1U;
                },
                kWaitTimeout))
                << "分段到达的头没被拼起来：读循环少了「还要再读」那一路";
        EXPECT_EQ(fixture.server().seenPeers()[0], "203.0.113.21");
    }

    /**
     * @brief 按来源限额要用真实来源算：同一个代理后面的不同客户端不该共用一个名额
     * @details 这条正是「忽略 PROXY 头」的判据：不看头时三条连接的对端都是 127.0.0.1，限额 1 会把
     *          第二条一起挡掉；看了头，第二条（另一个来源）就该建起来，而第三条（与首条同源）仍要挡
     */
    TEST(TcpServer, LimitsPerProxiedSourceNotPerProxy)
    {
        ServerTestOptions options;
        options.kind = ConnectionKind::ObservesStopRequest;
        options.proxyProtocolRequired = true;
        options.perIpLimiter = std::make_shared<PerIpConnectionLimiter>(1);
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        const LoopbackClient firstClient(listeningPort);
        ASSERT_TRUE(firstClient.isValid());
        ASSERT_TRUE(firstClient.sendAll(makeV1Header("203.0.113.1", 1, "192.0.2.1", 80)));
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().createConnectionCalls() >= 1U;
                },
                kWaitTimeout))
                << "首条带头的连接没建起来";

        const LoopbackClient secondClient(listeningPort);
        ASSERT_TRUE(secondClient.isValid());
        ASSERT_TRUE(secondClient.sendAll(makeV1Header("203.0.113.2", 2, "192.0.2.1", 80)));
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().createConnectionCalls() >= 2U;
                },
                kWaitTimeout))
                << "另一个来源被代理自己的地址挡住了：限额没用上 PROXY 头里的来源";

        const LoopbackClient thirdClient(listeningPort);
        ASSERT_TRUE(thirdClient.isValid());
        ASSERT_TRUE(thirdClient.sendAll(makeV1Header("203.0.113.1", 3, "192.0.2.1", 80)));
        EXPECT_FALSE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().createConnectionCalls() >= 3U;
                },
                kNegativeCheckTimeout))
                << "与首条同源的第三条被放行了：同一个真实来源拿到了两个名额";
    }

    /**
     * @brief 要求带头却不带头（直接发请求）的连接要被当场收口，且不许建会话
     * @details 两头都要断言：会话数不涨（否则头就成了可选装饰），客户端还要看到收尾（否则是吊死）
     */
    TEST(TcpServer, DropsConnectionThatSendsNoProxyHeader)
    {
        ServerTestOptions options;
        options.kind = ConnectionKind::ObservesStopRequest;
        options.proxyProtocolRequired = true;
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        const LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendAll("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"));

        ASSERT_TRUE(waitForCondition(
                [&client]
                {
                    return client.isClosedByPeer();
                },
                kWaitTimeout))
                << "发了不是头的字节却没被收口：判死那一路没生效";
        EXPECT_EQ(fixture.server().createConnectionCalls(), 0U) << "没带头的连接照样建了会话，等于头是可选的";
    }

    /**
     * @brief 开头确实像 PROXY 头、但字段不合规范的也要收口，且不许建会话
     * @details 与上一条分开的理由：那一路在「一看就不是头」时判死，这一路要读完整条头才发现不对，
     *          走的是解析失败那条出口（关闭动作挂在不同位置，少一处就会吊住对端）
     */
    TEST(TcpServer, DropsConnectionWithMalformedProxyHeader)
    {
        ServerTestOptions options;
        options.kind = ConnectionKind::ObservesStopRequest;
        options.proxyProtocolRequired = true;
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        const LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());
        // 前缀与换行都合规，字段数不对：读侧要一路读到行尾才发现解析不出来
        ASSERT_TRUE(client.sendAll("PROXY TCP4 203.0.113.9\r\n"));

        ASSERT_TRUE(waitForCondition(
                [&client]
                {
                    return client.isClosedByPeer();
                },
                kWaitTimeout))
                << "解析失败那条出口没关套接字：对端在等一个不会来的 FIN";
        EXPECT_EQ(fixture.server().createConnectionCalls(), 0U) << "不合规范的头被当成了合法身份";
    }

    /**
     * @brief 头后面还跟着别的字节时宁可拒绝：本层没有地方安放多出来的那一段
     * @details 钉住这条**取舍**而不是让它悄悄发生：代理把「头 + 请求」挤进一段时连接会被拒掉，
     *          而按实现读掉的字节已无法退回内核缓冲，静默错位比拒绝更糟
     */
    TEST(TcpServer, DropsConnectionCarryingBytesAfterProxyHeader)
    {
        ServerTestOptions options;
        options.kind = ConnectionKind::ObservesStopRequest;
        options.proxyProtocolRequired = true;
        RunningServerFixture fixture(options);
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout));

        const std::uint16_t listeningPort = queryBoundPort(fixture.listenDescriptor());
        ASSERT_NE(listeningPort, 0);

        const LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid());
        ASSERT_TRUE(client.sendAll(makeV1Header("203.0.113.9", 44000, "198.51.100.7", 443) + "GET / HTTP/1.1\r\n"));

        ASSERT_TRUE(waitForCondition(
                [&client]
                {
                    return client.isClosedByPeer();
                },
                kWaitTimeout))
                << "头后多出的字节没被处理却没收口：这条连接被晾在原地";
        EXPECT_EQ(fixture.server().createConnectionCalls(), 0U) << "带尾巴的头被放行了：尾巴里的正文会被当成头的续段丢掉";
    }

} // namespace AsynGyanis::Net
