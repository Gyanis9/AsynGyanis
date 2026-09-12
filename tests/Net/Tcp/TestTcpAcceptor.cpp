/**
 * @file TestTcpAcceptor.cpp
 * @brief TcpAcceptor 单元测试：绑定与监听前置条件、复用选项装配、批量 accept 与关闭后的状态复位
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Tcp/TcpAcceptor.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/SystemException.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/InetAddress.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 一般等待上限：回环上的握手与 accept 都在毫秒级完成；超时即判失败，不允许无界等待
        constexpr std::chrono::milliseconds kWaitTimeout{2000};

        /// 「断言某事不发生」时用的观察窗口（例如证明 accept 正在等待而不是立刻抛异常）
        constexpr std::chrono::milliseconds kNegativeCheckTimeout{200};

        /// 一次 accept 驱动的结果类别
        enum class FailureKind
        {
            None,             ///< 正常跑完
            SystemException,  ///< 抛出 Base::SystemException（终止性错误）
            BaseException,    ///< 抛出其它 Base::Exception
            UnknownException  ///< 抛出框架外的异常
        };

        /**
         * @brief accept 协程的结果槽
         * @details completed 以 release 语义发布，其余字段仅在 completed 为 true 后可读。
         *          已接受的套接字所有权留在这里，用例断完即由本对象的析构统一关闭，
         *          因此本对象必须声明在事件循环之后、在循环线程 join 之后才析构。
         */
        struct AcceptOutcome
        {
            std::atomic<bool> completed{false};                ///< 驱动协程是否已结束
            FailureKind failure{FailureKind::None};            ///< 结束原因
            bool sawNullOpt{false};                            ///< accept() 是否返回了 nullopt（表示应结束接受循环）
            std::vector<Core::AsyncSocket> acceptedSockets;    ///< 已接受的连接，按返回顺序保存

            /// 结果槽是否已在时限内完成且没有抛异常
            [[nodiscard]] bool isCompletedCleanly() const
            {
                return completed.load(std::memory_order_acquire) && failure == FailureKind::None;
            }
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
         * @details 必须声明在被投协程任务与结果槽之后：析构顺序保证「先 join 循环线程，
         *          再销毁协程帧与已接受的套接字」，否则循环线程可能恢复已销毁的句柄。
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
         * @brief 连续驱动若干次 accept()，把结果与异常类别写回结果槽
         * @param acceptor 被测监听器
         * @param outcome 结果槽
         * @param acceptRoundCount 计划驱动的 accept() 次数
         * @return Core::Task<> 协程，结束时 completed 已置位
         */
        Core::Task<> driveAccept(TcpAcceptor &acceptor, AcceptOutcome &outcome, const std::size_t acceptRoundCount)
        {
            try
            {
                for (std::size_t round = 0; round < acceptRoundCount; ++round)
                {
                    std::optional<Core::AsyncSocket> acceptedSocket = co_await acceptor.accept();
                    if (!acceptedSocket.has_value())
                    {
                        outcome.sawNullOpt = true;
                        break;
                    }
                    outcome.acceptedSockets.push_back(std::move(acceptedSocket.value()));
                }
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
            outcome.completed.store(true, std::memory_order_release);
            co_return;
        }

        /**
         * @brief 查询监听描述符上由内核实际分配的本地地址
         * @details TcpAcceptor::localAddress() 有意只回构造时请求的地址（端口 0 不会被回填），
         *          因此「系统分配的实际端口」必须自己从套接字上问，用 InetAddress 承载结果。
         * @param descriptor 监听描述符
         * @return Core::InetAddress 成功时为实际地址，失败时为空地址
         */
        Core::InetAddress queryBoundAddress(const int descriptor)
        {
            sockaddr_storage storage{};
            socklen_t        storageLength = static_cast<socklen_t>(sizeof(storage));
            if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&storage), &storageLength) != 0)
            {
                return Core::InetAddress{};
            }
            return Core::InetAddress(storage, storageLength);
        }

        /**
         * @brief 查询 SO_REUSEADDR 是否已生效
         * @param descriptor 目标描述符
         * @return true 选项值为非零
         */
        bool isReuseAddressEnabled(const int descriptor)
        {
            int       optionValue  = 0;
            socklen_t optionLength = sizeof(optionValue);
            if (::getsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char *>(&optionValue), &optionLength) != 0)
            {
                return false;
            }
            return optionValue != 0;
        }

#if ASYN_PLATFORM_LINUX
        /**
         * @brief 查询 SO_REUSEPORT 是否已生效（Linux 3.9+ 才有的选项）
         * @param descriptor 目标描述符
         * @return true 选项值为非零
         */
        bool isReusePortEnabled(const int descriptor)
        {
            int       optionValue  = 0;
            socklen_t optionLength = sizeof(optionValue);
            if (::getsockopt(descriptor, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<char *>(&optionValue), &optionLength) != 0)
            {
                return false;
            }
            return optionValue != 0;
        }
#endif // ASYN_PLATFORM_LINUX

        /**
         * @brief 查询 TCP_NODELAY 是否已生效
         * @details 用于钉住「accept() 给服务端连接一律关闭 Nagle」这条契约。
         * @param descriptor 已连接描述符
         * @return true 选项值为非零
         */
        bool isNoDelayEnabled(const int descriptor)
        {
            int       optionValue  = 0;
            socklen_t optionLength = sizeof(optionValue);
            if (::getsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&optionValue), &optionLength) != 0)
            {
                return false;
            }
            return optionValue != 0;
        }

        /**
         * @brief 一条到 127.0.0.1 指定端口的普通阻塞客户端连接
         * @details 客户端刻意不用框架的 AsyncSocket：测试只想把「有连接到达」这件事喂给监听器，
         *          不希望再引入事件循环与协程。析构即关闭，不留悬空监听或对端连接。
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
                m_localPort = localPortOf(m_descriptor);
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

            /// 本端随机分配的源端口，用于和 accept 出来的对端端口对账
            [[nodiscard]] std::uint16_t localPort() const noexcept
            {
                return m_localPort;
            }

        private:
            /// 取描述符的本地端口，失败返回 0
            static std::uint16_t localPortOf(const int descriptor)
            {
                sockaddr_in address{};
                socklen_t   addressLength = static_cast<socklen_t>(sizeof(address));
                if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &addressLength) != 0)
                {
                    return 0;
                }
                return ntohs(address.sin_port);
            }

            Platform::Socket::Initialization m_socketInitialization; ///< 保证 Winsock 在本对象存活期间保持初始化
            int                             m_descriptor{Platform::FileDescriptor::kInvalid}; ///< 客户端描述符
            std::uint16_t                   m_localPort{0};                                   ///< 本端源端口
        };

        /// 把若干端口排序后返回，用于「按集合而非顺序」比对
        std::vector<std::uint16_t> sortedPorts(const std::vector<std::uint16_t> &ports)
        {
            std::vector<std::uint16_t> sorted = ports;
            std::sort(sorted.begin(), sorted.end());
            return sorted;
        }
    } // namespace

    TEST(TcpAcceptor, ClassForbidsCopyAndMove)
    {
        // 持有 Core::EventLoop& 与独占描述符所有权：移动会留下指向旧循环的引用，语义无法自洽
        static_assert(!std::is_copy_constructible_v<TcpAcceptor>, "TcpAcceptor 禁止拷贝构造");
        static_assert(!std::is_copy_assignable_v<TcpAcceptor>, "TcpAcceptor 禁止拷贝赋值");
        static_assert(!std::is_move_constructible_v<TcpAcceptor>, "TcpAcceptor 禁止移动构造");
        static_assert(!std::is_move_assignable_v<TcpAcceptor>, "TcpAcceptor 禁止移动赋值");
        SUCCEED() << "以上均为编译期断言";
    }

    TEST(TcpAcceptor, DefaultListenBacklogIsPortableQueueDepth)
    {
        // 默认档位是命名空间常量而不是 SOMAXCONN：公开头文件里不该出现 OS 宏，
        // 而且 Windows 把 SOMAXCONN 解释成「由内核自行膨胀队列」，等于关掉背压
        static_assert(kDefaultListenBacklog == 128, "默认监听队列深度改动必须同步文档与用例");
        EXPECT_EQ(kDefaultListenBacklog, 128);
    }

    TEST(TcpAcceptor, ConstructorCreatesSocketButListenRequiresBind)
    {
        Core::EventLoop loop;
        TcpAcceptor     acceptor(loop, Core::InetAddress::localhost(0));

        // 构造阶段只创建资源（监听套接字 + 预建退避定时器），并未绑定：
        // 因此描述符有效，而「未绑定就监听」必须直接判 false，
        // 而不是把语义含糊的内核 EINVAL 抛给调用方
        EXPECT_TRUE(Platform::FileDescriptor::isValid(acceptor.fileDescriptor()));
        EXPECT_FALSE(acceptor.listen(kDefaultListenBacklog));
        EXPECT_FALSE(acceptor.listen(1));
    }

    TEST(TcpAcceptor, LocalAddressReportsRequestedAddressOnly)
    {
        Core::EventLoop loop;
        const Core::InetAddress requestedAddress = Core::InetAddress::localhost(0);
        TcpAcceptor             acceptor(loop, requestedAddress);

        // 端口 0 时返回的仍是请求值：本类的 localAddress() 不反映内核分配的端口，
        // 想要实际端口必须自己问套接字（见 queryBoundAddress 的用例）
        ASSERT_TRUE(acceptor.bind());
        EXPECT_EQ(acceptor.localAddress().port(), 0);
        EXPECT_EQ(acceptor.localAddress().ip(), "127.0.0.1");
        EXPECT_EQ(acceptor.localAddress(), requestedAddress);
    }

    TEST(TcpAcceptor, BindThenListenEntersListeningState)
    {
        Core::EventLoop loop;
        TcpAcceptor     acceptor(loop, Core::InetAddress::localhost(0));

        ASSERT_TRUE(acceptor.bind());
        EXPECT_TRUE(acceptor.listen(kDefaultListenBacklog));

        // 回环 + 临时端口：内核分配的端口非零，且协议族与地址仍是回环 IPv4
        const Core::InetAddress boundAddress = queryBoundAddress(acceptor.fileDescriptor());
        EXPECT_NE(boundAddress.port(), 0);
        EXPECT_EQ(boundAddress.ip(), "127.0.0.1");
    }

    TEST(TcpAcceptor, ListenHonoursExplicitBacklogArgument)
    {
        Core::EventLoop loop;
        TcpAcceptor     acceptor(loop, Core::InetAddress::localhost(0));

        // listen 的队列深度只能由调用方显式给值：这里给一个比默认档浅得多的值，
        // 底层 listen() 对 backlog 只做上限截断、不做拒绝，因此仍应成功
        ASSERT_TRUE(acceptor.bind());
        EXPECT_TRUE(acceptor.listen(1));
    }

    TEST(TcpAcceptor, BindEnablesAddressReuseOnListenSocket)
    {
        Core::EventLoop loop;
        TcpAcceptor     acceptor(loop, Core::InetAddress::localhost(0));

        // 重启服务时上一代连接留下的 TIME_WAIT 会占住端口，绑定前必须装配 SO_REUSEADDR
        ASSERT_TRUE(acceptor.bind());
        EXPECT_TRUE(isReuseAddressEnabled(acceptor.fileDescriptor()));
    }

    TEST(TcpAcceptor, BindAttemptsPortReuseWithoutBlockingBind)
    {
        Core::EventLoop loop;
        TcpAcceptor     acceptor(loop, Core::InetAddress::localhost(0));

        // SO_REUSEPORT 只在 Linux 3.9+ 存在：Windows 上设置失败也必须照常完成绑定，
        // 即「平台不支持就降级」，不作为绑定失败上抛
        ASSERT_TRUE(acceptor.bind());
#if ASYN_PLATFORM_LINUX
        EXPECT_TRUE(isReusePortEnabled(acceptor.fileDescriptor()));
#else
        // 非 Linux 平台没有 SO_REUSEPORT：Platform 层必须明确回答「不支持」，
        // 而 bind() 依然成功——降级不失败，正是本用例要钉的那半边
        EXPECT_FALSE(Platform::Socket::setReusePort(acceptor.fileDescriptor()));
#endif
    }

    TEST(TcpAcceptor, CloseResetsDescriptorAndBoundState)
    {
        Core::EventLoop loop;
        TcpAcceptor     acceptor(loop, Core::InetAddress::localhost(0));

        ASSERT_TRUE(acceptor.bind());
        ASSERT_TRUE(acceptor.listen(kDefaultListenBacklog));

        acceptor.close();

        // 关闭后描述符失效，且 m_bound 一并复位：此后 listen() 必须被拒绝而不是拿旧状态蒙混过关
        EXPECT_FALSE(Platform::FileDescriptor::isValid(acceptor.fileDescriptor()));
        EXPECT_FALSE(acceptor.listen(kDefaultListenBacklog));
        EXPECT_NO_THROW(acceptor.close());
    }

    TEST(TcpAcceptor, AcceptReturnsNulloptAfterClose)
    {
        Core::EventLoop loop;
        TcpAcceptor     acceptor(loop, Core::InetAddress::localhost(0));
        ASSERT_TRUE(acceptor.bind());
        ASSERT_TRUE(acceptor.listen(kDefaultListenBacklog));
        acceptor.close();

        // 监听套接字已关：按契约返回空值让服务器循环正常收尾，不抛异常
        AcceptOutcome   outcome;
        Core::Task<>    driverTask = driveAccept(acceptor, outcome, 1);
        EventLoopThread loopThread(loop);
        loopThread.schedule(driverTask);

        ASSERT_TRUE(waitForCondition(
                [&outcome]
                {
                    return outcome.completed.load(std::memory_order_acquire);
                },
                kWaitTimeout));
        EXPECT_TRUE(outcome.isCompletedCleanly());
        EXPECT_TRUE(outcome.sawNullOpt);
        EXPECT_TRUE(outcome.acceptedSockets.empty());
    }

    TEST(TcpAcceptor, AcceptReturnsPendingConnectionWithoutThrowing)
    {
        Core::EventLoop loop;
        TcpAcceptor     acceptor(loop, Core::InetAddress::localhost(0));
        ASSERT_TRUE(acceptor.bind());
        ASSERT_TRUE(acceptor.listen(kDefaultListenBacklog));

        const std::uint16_t listeningPort = queryBoundAddress(acceptor.fileDescriptor()).port();
        ASSERT_NE(listeningPort, 0);

        // 连接在 accept() 之前就已进入监听队列：本轮必须直接把套接字交出来
        const LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环客户端连接失败";

        AcceptOutcome   outcome;
        Core::Task<>    driverTask = driveAccept(acceptor, outcome, 1);
        EventLoopThread loopThread(loop);
        loopThread.schedule(driverTask);

        ASSERT_TRUE(waitForCondition(
                [&outcome]
                {
                    return outcome.completed.load(std::memory_order_acquire);
                },
                kWaitTimeout)) << "accept 未在时限内返回已排队连接：等待上界 kWaitTimeout";
        ASSERT_TRUE(outcome.isCompletedCleanly());
        ASSERT_EQ(outcome.acceptedSockets.size(), 1u);
        EXPECT_FALSE(outcome.sawNullOpt);

        const Core::AsyncSocket &acceptedSocket = outcome.acceptedSockets.front();
        ASSERT_TRUE(Platform::FileDescriptor::isValid(acceptedSocket.fileDescriptor()));
        EXPECT_EQ(acceptedSocket.remoteAddress().ip(), "127.0.0.1");
        EXPECT_EQ(acceptedSocket.remoteAddress().port(), client.localPort());
        // 服务端连接一律关闭 Nagle，否则 HTTP 小包会被攒到 ACK 才发
        EXPECT_TRUE(isNoDelayEnabled(acceptedSocket.fileDescriptor()));
    }

    TEST(TcpAcceptor, AcceptDrainsWholeListenQueueIntoPendingList)
    {
        Core::EventLoop loop;
        TcpAcceptor     acceptor(loop, Core::InetAddress::localhost(0));
        ASSERT_TRUE(acceptor.bind());
        ASSERT_TRUE(acceptor.listen(kDefaultListenBacklog));

        const std::uint16_t listeningPort = queryBoundAddress(acceptor.fileDescriptor()).port();
        ASSERT_NE(listeningPort, 0);

        constexpr std::size_t kClientCount = 3;
        std::array<std::unique_ptr<LoopbackClient>, kClientCount> clients;
        std::vector<std::uint16_t>                                clientPorts;
        for (std::size_t clientIndex = 0; clientIndex < kClientCount; ++clientIndex)
        {
            clients[clientIndex] = std::make_unique<LoopbackClient>(listeningPort);
            if (!clients[clientIndex]->isValid())
            {
                clientPorts.clear();
                break;
            }
            clientPorts.push_back(clients[clientIndex]->localPort());
        }
        ASSERT_EQ(clientPorts.size(), kClientCount) << "三条回环连接未全部建立，批量 accept 无从验证";

        // 事件循环是边沿触发：一次就绪必须把队列抽干，否则残留连接不会再次产生事件。
        // 三条连接在本轮 accept 之前就已握手完成并全部入队，因此三次 accept() 都应直接返回。
        // 时序说明：若某条连接尚未及入队，第 3 次 accept 会挂起等待可读事件，
        // 由 kWaitTimeout 兜住并判失败——上界 2 秒。
        AcceptOutcome   outcome;
        Core::Task<>    driverTask = driveAccept(acceptor, outcome, kClientCount);
        EventLoopThread loopThread(loop);
        loopThread.schedule(driverTask);

        ASSERT_TRUE(waitForCondition(
                [&outcome]
                {
                    return outcome.completed.load(std::memory_order_acquire);
                },
                kWaitTimeout)) << "批量 accept 未在时限内收干三条连接：等待上界 kWaitTimeout";
        ASSERT_TRUE(outcome.isCompletedCleanly());
        ASSERT_EQ(outcome.acceptedSockets.size(), kClientCount);

        std::vector<std::uint16_t> acceptedPorts;
        for (const Core::AsyncSocket &acceptedSocket: outcome.acceptedSockets)
        {
            acceptedPorts.push_back(acceptedSocket.remoteAddress().port());
        }
        // 只比对集合：入队顺序由内核决定，用例不去钉它
        EXPECT_EQ(sortedPorts(acceptedPorts), sortedPorts(clientPorts));
    }

    TEST(TcpAcceptor, AcceptWaitsForIncomingConnectionInsteadOfThrowing)
    {
        Core::EventLoop loop;
        TcpAcceptor     acceptor(loop, Core::InetAddress::localhost(0));
        ASSERT_TRUE(acceptor.bind());
        ASSERT_TRUE(acceptor.listen(kDefaultListenBacklog));

        const std::uint16_t listeningPort = queryBoundAddress(acceptor.fileDescriptor()).port();
        ASSERT_NE(listeningPort, 0);

        // 队列为空时 EAGAIN 必须在协程内部消化：挂起等待监听描述符可读，绝不抛给调用方。
        // 时序说明：先用 kNegativeCheckTimeout（200ms）证明它「还没回来也没炸」，
        // 这一步是观察窗口而非固定 sleep；随后再给 kWaitTimeout 等真正完成。
        AcceptOutcome   outcome;
        Core::Task<>    driverTask = driveAccept(acceptor, outcome, 1);
        EventLoopThread loopThread(loop);
        loopThread.schedule(driverTask);

        const bool completedTooEarly = waitForCondition(
                [&outcome]
                {
                    return outcome.completed.load(std::memory_order_acquire);
                },
                kNegativeCheckTimeout);
        EXPECT_FALSE(completedTooEarly) << "无连接可接受时 accept 立刻返回：说明它没有挂起等待";

        if (!completedTooEarly)
        {
            const LoopbackClient client(listeningPort);
            ASSERT_TRUE(client.isValid());
            EXPECT_TRUE(waitForCondition(
                    [&outcome]
                    {
                        return outcome.completed.load(std::memory_order_acquire);
                    },
                    kWaitTimeout)) << "连接到达后 accept 未被唤醒：等待上界 kWaitTimeout";
            EXPECT_TRUE(outcome.isCompletedCleanly()) << "可恢复错误被当成终止性错误抛出";
            EXPECT_EQ(outcome.acceptedSockets.size(), 1u);
        }
    }

    TEST(TcpAcceptor, RebindAfterCloseSucceedsOnFreshInstance)
    {
        Core::EventLoop loop;
        TcpAcceptor     firstAcceptor(loop, Core::InetAddress::localhost(0));
        ASSERT_TRUE(firstAcceptor.bind());
        ASSERT_TRUE(firstAcceptor.listen(kDefaultListenBacklog));

        // close() 之后再 listen 必须失败；换一个全新实例仍应能正常绑定并监听。
        // 两条合起来证明「已绑定」是每实例状态而非全局残留。
        // 注意：这里不比对两次分配到的端口，回环临时端口可能被内核重新派发。
        firstAcceptor.close();
        EXPECT_FALSE(firstAcceptor.listen(kDefaultListenBacklog));

        TcpAcceptor secondAcceptor(loop, Core::InetAddress::localhost(0));
        ASSERT_TRUE(secondAcceptor.bind());
        EXPECT_TRUE(secondAcceptor.listen(kDefaultListenBacklog));
        EXPECT_NE(queryBoundAddress(secondAcceptor.fileDescriptor()).port(), 0);
    }
} // namespace AsynGyanis::Net
