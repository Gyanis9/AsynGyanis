/**
 * @file TestSocketTuning.cpp
 * @brief 套接字调参面：缓冲区上限（SO_SNDBUF/SO_RCVBUF）的读写、监听器下发与连接继承、
 *        延迟接受（TCP_DEFER_ACCEPT）的平台支持约定
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

// 覆盖场景：
//   一. Platform::Socket 的缓冲区 setter：正值下发成功且 getsockopt 可读回不小于请求值；
//       非正值被拒绝（不把含糊取值交给内核）
//   二. 延迟接受的平台约定：Windows 无该选项（返回 false，按不支持降级），Linux 应成功
//   三. TcpAcceptor 调参下发：监听套接字与每条接受到的连接都带上配置的缓冲区上限

#include "Net/Tcp/TcpAcceptor.h"

#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/InetAddress.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 等待上限：回环上的接受动作远快于此
        constexpr std::chrono::milliseconds kWaitTimeout{5000};

        /// 本用例使用的缓冲区上限（64 KiB：明显高于系统默认，且各平台都允许）
        constexpr int kBufferSizeBytes = 64 * 1024;

        /**
         * @brief 轮询等待条件成立
         * @param predicate 条件
         * @param timeout 等待上限
         * @return true 在时限内成立
         */
        template<typename Predicate>
        bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (predicate())
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return predicate();
        }

        /**
         * @brief 读取套接字整数选项
         * @param descriptor 目标描述符
         * @param level 选项层级
         * @param optionName 选项名
         * @return int 选项值；读取失败返回 -1
         */
        int queryIntegerOption(const int descriptor, const int level, const int optionName)
        {
            int       optionValue  = -1;
            socklen_t optionLength = static_cast<socklen_t>(sizeof(optionValue));
            if (::getsockopt(descriptor, level, optionName, reinterpret_cast<char *>(&optionValue), &optionLength) != 0)
            {
                return -1;
            }
            return optionValue;
        }

        /**
         * @brief 查询监听描述符上由内核实际分配的本地地址（端口 0 场景拿真实端口）
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
         * @brief 在独立线程上驱动事件循环的夹具
         * @details 必须声明在协程任务与结果槽之后：析构顺序保证「先 join 循环线程，
         *          再销毁协程帧与已接受的套接字」，否则循环线程可能恢复已销毁的句柄
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

            void schedule(Core::Task<> &task)
            {
                m_loop.scheduler().scheduleRemote(task.handle());
            }

        private:
            Core::EventLoop &m_loop;  ///< 被执行的事件循环
            std::thread     m_worker; ///< 承载 run() 的线程
        };

        /// 一次接受尝试的结果槽
        struct AcceptOutcome
        {
            std::vector<Core::AsyncSocket> acceptedSockets; ///< 已接受的连接
            std::atomic<bool>              completed{false}; ///< 驱动协程是否已结束
        };

        /**
         * @brief 驱动一次 accept()，把结果写回结果槽
         * @param acceptor 被测监听器
         * @param outcome 结果槽
         * @return Core::Task<> 协程，结束时 completed 已置位
         */
        Core::Task<> driveAcceptOnce(TcpAcceptor &acceptor, AcceptOutcome &outcome)
        {
            std::optional<Core::AsyncSocket> acceptedSocket = co_await acceptor.accept();
            if (acceptedSocket.has_value())
            {
                outcome.acceptedSockets.push_back(std::move(acceptedSocket.value()));
            }
            outcome.completed.store(true, std::memory_order_release);
            co_return;
        }

        /**
         * @brief 回环客户端：连上监听端口即可（不发送数据）
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
                }
            }

            ~LoopbackClient()
            {
                if (Platform::FileDescriptor::isValid(m_descriptor))
                {
                    Platform::FileDescriptor::close(m_descriptor);
                }
            }

            LoopbackClient(const LoopbackClient &) = delete;

            LoopbackClient &operator=(const LoopbackClient &) = delete;

            [[nodiscard]] bool isValid() const noexcept
            {
                return Platform::FileDescriptor::isValid(m_descriptor);
            }

        private:
            int m_descriptor{Platform::FileDescriptor::kInvalid}; ///< 客户端描述符
        };
    } // namespace

    /**
     * @brief 钉住：缓冲区上限的正值被下发且可读回，非正值一律拒绝
     */
    TEST(SocketTuning, BufferSettersAcceptPositiveSizesAndRejectNonPositive)
    {
        int localDescriptor  = Platform::FileDescriptor::kInvalid;
        int remoteDescriptor = Platform::FileDescriptor::kInvalid;
        ASSERT_TRUE(Platform::FileDescriptor::createPair(localDescriptor, remoteDescriptor));

        EXPECT_TRUE(Platform::Socket::setReceiveBufferSize(localDescriptor, kBufferSizeBytes));
        EXPECT_TRUE(Platform::Socket::setSendBufferSize(localDescriptor, kBufferSizeBytes));

        // 内核会按自身策略取整（Linux 约为请求值的两倍），断言方向只保证「不小于请求值」
        EXPECT_GE(queryIntegerOption(localDescriptor, SOL_SOCKET, SO_RCVBUF), kBufferSizeBytes);
        EXPECT_GE(queryIntegerOption(localDescriptor, SOL_SOCKET, SO_SNDBUF), kBufferSizeBytes);

        // 非正值没有「按上限扩容」的语义：直接拒绝，不把含糊取值交给内核
        EXPECT_FALSE(Platform::Socket::setReceiveBufferSize(localDescriptor, 0));
        EXPECT_FALSE(Platform::Socket::setReceiveBufferSize(localDescriptor, -1));
        EXPECT_FALSE(Platform::Socket::setSendBufferSize(localDescriptor, 0));
        EXPECT_FALSE(Platform::Socket::setSendBufferSize(localDescriptor, -1));

        Platform::FileDescriptor::close(localDescriptor);
        Platform::FileDescriptor::close(remoteDescriptor);
    }

    /**
     * @brief 钉住：延迟接受的平台约定——Windows 无该选项（返回 false），Linux 支持
     * @note 探针必须是真实的 TCP 套接字：createPair() 在 Linux 上给的是 AF_UNIX 的 socketpair，
     *       它没有 TCP 层选项，用它探测会得到「平台不支持」的假阴性
     */
    TEST(SocketTuning, DeferAcceptFollowsPlatformSupport)
    {
        Core::EventLoop     loop;
        Core::AsyncSocket   probeSocket = Core::AsyncSocket::create(loop);
        const int           probeDescriptor = probeSocket.fileDescriptor();
        ASSERT_GE(probeDescriptor, 0) << "TCP 探针套接字没有创建成功";

#if ASYN_PLATFORM_WIN32
        // Windows 没有 TCP_DEFER_ACCEPT：按「平台不支持」返回 false，调用方据此处降级
        EXPECT_FALSE(Platform::Socket::setDeferAccept(probeDescriptor, 1));
#else
        EXPECT_TRUE(Platform::Socket::setDeferAccept(probeDescriptor, 1));
#endif
    }

    /**
     * @brief 钉住：监听器把缓冲区上限下发到监听套接字，并作用到每条接受到的连接
     */
    TEST(SocketTuning, AcceptorAppliesBufferSizesToListeningAndAcceptedSockets)
    {
        Core::EventLoop    loop;
        TcpAcceptor        acceptor(loop, Core::InetAddress::localhost(0));
        TcpAcceptor::SocketTuning tuning;
        tuning.receiveBufferBytes = kBufferSizeBytes;
        tuning.sendBufferBytes    = kBufferSizeBytes;
        // 延迟接受一并配置：Windows 上被降级、Linux 上生效，都不应影响监听与接受
        tuning.deferAcceptSeconds = 1;
        acceptor.setSocketTuning(tuning);
        EXPECT_EQ(acceptor.socketTuning().receiveBufferBytes, kBufferSizeBytes);

        ASSERT_TRUE(acceptor.bind());
        ASSERT_TRUE(acceptor.listen(kDefaultListenBacklog));

        // 监听套接字：调参在 listen() 时已下发
        EXPECT_GE(queryIntegerOption(acceptor.fileDescriptor(), SOL_SOCKET, SO_RCVBUF), kBufferSizeBytes);
        EXPECT_GE(queryIntegerOption(acceptor.fileDescriptor(), SOL_SOCKET, SO_SNDBUF), kBufferSizeBytes);

        const Core::InetAddress boundAddress = queryBoundAddress(acceptor.fileDescriptor());
        ASSERT_NE(boundAddress.port(), 0);

        LoopbackClient client(boundAddress.port());
        ASSERT_TRUE(client.isValid());

        AcceptOutcome  outcome;
        Core::Task<>   acceptTask   = driveAcceptOnce(acceptor, outcome);
        EventLoopThread loopThread(loop);
        loopThread.schedule(acceptTask);

        ASSERT_TRUE(waitUntil(
                [&outcome]
                {
                    return outcome.completed.load(std::memory_order_acquire);
                },
                kWaitTimeout)) << "accept() 未在时限内完成";
        ASSERT_EQ(outcome.acceptedSockets.size(), 1U);

        // 接受到的连接：显式下发过同一组取值（不依赖「监听套接字继承」这一平台差异）
        const int acceptedDescriptor = outcome.acceptedSockets.front().fileDescriptor();
        EXPECT_GE(queryIntegerOption(acceptedDescriptor, SOL_SOCKET, SO_RCVBUF), kBufferSizeBytes);
        EXPECT_GE(queryIntegerOption(acceptedDescriptor, SOL_SOCKET, SO_SNDBUF), kBufferSizeBytes);
    }
} // namespace AsynGyanis::Net
