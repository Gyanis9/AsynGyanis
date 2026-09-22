// Socket 单元测试：Winsock 生命周期与跨平台 accept 语义
#include "Platform/IO/Socket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string_view>
#include <thread>

#include "PlatformTestSupport.h"

namespace AsynGyanis::Platform
{
    namespace
    {
        /// 有界重试统一使用的最长等待毫秒数，避免任何一步无限阻塞
        constexpr int kWaitTimeoutMilliseconds = 2000;

        /**
         * @brief 创建并绑定一个回环监听 socket（已置为非阻塞）
         * @param assignedPort 输出参数，系统分配的监听端口
         * @return int 监听描述符，失败返回 FileDescriptor::kInvalid
         */
        int createLoopbackListener(std::uint16_t &assignedPort)
        {
            Socket::initialize();

            const int listener = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (listener < 0)
            {
                return FileDescriptor::kInvalid;
            }

            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = 0;
            if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
            {
                FileDescriptor::close(listener);
                return FileDescriptor::kInvalid;
            }

            socklen_t addressLength = sizeof(address);
            if (::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &addressLength) < 0)
            {
                FileDescriptor::close(listener);
                return FileDescriptor::kInvalid;
            }
            assignedPort = ntohs(address.sin_port);

            if (::listen(listener, 1) < 0)
            {
                FileDescriptor::close(listener);
                return FileDescriptor::kInvalid;
            }

            // 监听端设为非阻塞，accept 才可安全地在有界循环里重试
            if (!FileDescriptor::setNonBlocking(listener))
            {
                FileDescriptor::close(listener);
                return FileDescriptor::kInvalid;
            }
            return listener;
        }

        /**
         * @brief 向指定回环端口发起连接，成功后转为非阻塞
         * @param port 目标端口
         * @return int 客户端描述符，失败返回 FileDescriptor::kInvalid
         * @note 连接阶段保持阻塞（回环握手即刻完成），随后转非阻塞，
         *       这样轮询读取不会在 recv 中无限挂起。
         */
        int connectToLoopback(const std::uint16_t port)
        {
            const int client = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (client < 0)
            {
                return FileDescriptor::kInvalid;
            }

            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = htons(port);
            if (::connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
            {
                FileDescriptor::close(client);
                return FileDescriptor::kInvalid;
            }

            if (!FileDescriptor::setNonBlocking(client))
            {
                FileDescriptor::close(client);
                return FileDescriptor::kInvalid;
            }
            return client;
        }

        /**
         * @brief 在有界时间内从监听描述符接受一条连接
         * @param listener 非阻塞监听描述符
         * @param address 可选输出参数，对端地址
         * @param addressLength 可选输入输出参数，address 缓冲区容量与实际长度
         * @return int 已连接描述符，超时返回 FileDescriptor::kInvalid
         */
        int acceptWithTimeout(const int listener, sockaddr *address, socklen_t *addressLength)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMilliseconds);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (const int accepted = Socket::accept(listener, address, addressLength); FileDescriptor::isValid(accepted))
                {
                    return accepted;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return FileDescriptor::kInvalid;
        }
    } // namespace

    TEST(Socket, InitializeSucceedsOnBothPlatforms)
    {
        EXPECT_TRUE(Socket::initialize());
        // 内部使用一次性初始化，重复调用同样成功
        EXPECT_TRUE(Socket::initialize());
    }

    TEST(Socket, AcceptOnInvalidDescriptorFails)
    {
        const int accepted = Socket::accept(FileDescriptor::kInvalid, nullptr, nullptr);

        EXPECT_FALSE(FileDescriptor::isValid(accepted));
    }

    TEST(Socket, AcceptReturnsConnectedDescriptorAndPeerAddress)
    {
        std::uint16_t port     = 0;
        const int     listener = createLoopbackListener(port);
        ASSERT_TRUE(FileDescriptor::isValid(listener));

        const int client = connectToLoopback(port);
        ASSERT_TRUE(FileDescriptor::isValid(client));

        sockaddr_in peerAddress{};
        socklen_t   peerAddressLength = sizeof(peerAddress);
        const int   accepted          = acceptWithTimeout(listener, reinterpret_cast<sockaddr *>(&peerAddress),
                                               &peerAddressLength);

        ASSERT_TRUE(FileDescriptor::isValid(accepted));
        EXPECT_EQ(peerAddress.sin_family, AF_INET);
        EXPECT_EQ(peerAddress.sin_addr.s_addr, htonl(INADDR_LOOPBACK));
        EXPECT_GT(peerAddressLength, 0);

        // 两端均已非阻塞：已连接的描述符可双向收发
        constexpr char kmarker = 'S';
        const ssize_t  sent    = FileDescriptor::write(accepted, &kmarker, sizeof(kmarker));
        EXPECT_EQ(sent, 1);

        char       received = '\0';
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitTimeoutMilliseconds);
        while (received == '\0' && std::chrono::steady_clock::now() < deadline)
        {
            if (FileDescriptor::read(client, &received, sizeof(received)) > 0)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_EQ(received, 'S');

        FileDescriptor::close(accepted);
        FileDescriptor::close(client);
        FileDescriptor::close(listener);
    }

    TEST(Socket, AcceptedDescriptorIsNonBlocking)
    {
        std::uint16_t port     = 0;
        const int     listener = createLoopbackListener(port);
        ASSERT_TRUE(FileDescriptor::isValid(listener));

        const int client = connectToLoopback(port);
        ASSERT_TRUE(FileDescriptor::isValid(client));

        const int accepted = acceptWithTimeout(listener, nullptr, nullptr);
        ASSERT_TRUE(FileDescriptor::isValid(accepted));

        // 无数据时读取应立即失败而不是阻塞，说明两侧实现都置入了非阻塞标志
        char buffer[8];
        EXPECT_LT(FileDescriptor::read(accepted, buffer, sizeof(buffer)), 0);

        FileDescriptor::close(accepted);
        FileDescriptor::close(client);
        FileDescriptor::close(listener);
    }

    /**
     * @brief accept 出来的连接不得随进程创建传给子进程
     * @details POSIX 侧由 accept4 的 SOCK_CLOEXEC 在建好时就带上标记；Windows 的 accept 句柄默认
     *          可继承，不补一刀就等于把一条已建立的连接交给随机一个子进程——本框架以导出库的形态
     *          被消费，调用方的 CreateProcess 不一定会带句柄清单把继承收窄。
     */
    TEST(Socket, AcceptedDescriptorIsNotInheritable)
    {
        std::uint16_t port     = 0;
        const int     listener = createLoopbackListener(port);
        ASSERT_TRUE(FileDescriptor::isValid(listener));

        const int client = connectToLoopback(port);
        ASSERT_TRUE(FileDescriptor::isValid(client));

        const int accepted = acceptWithTimeout(listener, nullptr, nullptr);
        ASSERT_TRUE(FileDescriptor::isValid(accepted));

        EXPECT_TRUE(TestSupport::isNotInheritable(accepted)) << "连接句柄可被继承，父进程关掉它之后这一侧仍被子进程持着";

        FileDescriptor::close(accepted);
        FileDescriptor::close(client);
        FileDescriptor::close(listener);
    }

    /**
     * @brief 聚合写把多段拼成一个字节流，并把非法参数当场挡下
     *
     * @details 分段提交的意义在于不必先把数据拼进同一块缓冲（省一次整体拷贝），
     *          因此这里必须验证两件事：线上字节流与「按序拼接」逐字节一致；
     *          段数/空数组这类越界参数当场失败而不是静默拆分或假装发出去。
     */
    TEST(Socket, WriteVectoredConcatenatesSegmentsAndRejectsInvalidArguments)
    {
        int readDescriptor  = -1;
        int writeDescriptor = -1;
        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        const std::string_view first = "alpha";
        const std::string_view empty = first.substr(1, 0); // 长度为 0 但指针非空，两个平台都接受
        const std::string_view third = "beta";
        const Socket::WriteBuffer buffers[3] = {
                {first.data(), first.size()},
                {empty.data(), empty.size()},
                {third.data(), third.size()},
        };

        const ssize_t writtenLength = Socket::writeVectored(writeDescriptor, buffers, 3);
        ASSERT_EQ(writtenLength, static_cast<ssize_t>(first.size() + third.size()));

        char          received[16] = {};
        const ssize_t readLength   = FileDescriptor::read(readDescriptor, received, sizeof(received));
        ASSERT_EQ(readLength, writtenLength);
        EXPECT_EQ(std::string_view(received, static_cast<std::size_t>(readLength)), "alphabeta");

        // 非法参数：返回 -1 并置 kInvalidArgument，绝不静默拆分（拆分会让「一次系统调用」的收益
        // 悄悄消失）也不静默补齐（补齐会让调用方以为数据发出去了）
        EXPECT_EQ(Socket::writeVectored(writeDescriptor, nullptr, 1), -1);
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument);
        EXPECT_EQ(Socket::writeVectored(writeDescriptor, buffers, 0), -1);
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument);
        EXPECT_EQ(Socket::writeVectored(writeDescriptor, buffers, Socket::kMaximumVectorCount + 1), -1);
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument);

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

    namespace
    {
        /**
         * @brief 读回一个 int 尺寸的套接字选项
         * @param descriptor 目标描述符
         * @param optionLevel 选项层级
         * @param optionName 选项名
         * @param[out] value 读回的值
         * @return true 读取成功
         */
        bool readIntegerOption(const int descriptor, const int optionLevel, const int optionName, int &value)
        {
            socklen_t optionLength = static_cast<socklen_t>(sizeof(value));
            return ::getsockopt(descriptor, optionLevel, optionName, reinterpret_cast<char *>(&value), &optionLength) == 0;
        }

        /**
         * @brief 建一个未绑定的 TCP 套接字，供选项读写用例使用
         * @return int 描述符；失败时返回 FileDescriptor::kInvalid
         */
        int createStreamSocket()
        {
            Socket::initialize();
            return static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        }
    } // namespace

    /**
     * @brief 钉住：关延迟确认与地址复用这两个开关真的落到套接字上
     * @details 这两个选项决定首字节延迟与重启时能否立刻绑回同一端口，全仓此前只被别的层间接调用，
     *          一次也没被直测过。判据取内核读回的值而不是 setter 的返回值——setsockopt 传错尺寸
     *          （Windows 的值形参是 const char*，POSIX 是 const void*）时返回值照样可以是 0。
     */
    TEST(Socket, NoDelayAndReuseAddressAreReadableBackOnTheDescriptor)
    {
        const int descriptor = createStreamSocket();
        ASSERT_TRUE(FileDescriptor::isValid(descriptor));

        EXPECT_TRUE(Socket::setNoDelay(descriptor));
        int noDelay = 0;
        ASSERT_TRUE(readIntegerOption(descriptor, IPPROTO_TCP, TCP_NODELAY, noDelay));
        EXPECT_NE(noDelay, 0) << "setNoDelay 报成功却没生效，等于每个小包都要等确认";

        EXPECT_TRUE(Socket::setReuseAddress(descriptor));
        int reuseAddress = 0;
        ASSERT_TRUE(readIntegerOption(descriptor, SOL_SOCKET, SO_REUSEADDR, reuseAddress));
        EXPECT_NE(reuseAddress, 0) << "setReuseAddress 没落到套接字上，重启时会绑不回同一端口";

        FileDescriptor::close(descriptor);
    }

    /**
     * @brief 钉住：缓冲尺寸只接受正数，非正值当场拒绝而不是交给内核
     * @details 「0 字节缓冲」既可能被内核解释成「按上限扩容」也可能被解释成「不缓冲」，两种都不是
     *          调用方写的数；负数更会在无符号内部表示里回绕。拒绝面与放行面都要有用例。
     */
    TEST(Socket, BufferSizeSettersRejectNonPositiveCountsAndAcceptPositiveOnes)
    {
        const int descriptor = createStreamSocket();
        ASSERT_TRUE(FileDescriptor::isValid(descriptor));

        EXPECT_FALSE(Socket::setSendBufferSize(descriptor, 0)) << "0 没有「按上限扩容」的语义";
        EXPECT_FALSE(Socket::setSendBufferSize(descriptor, -1));
        EXPECT_FALSE(Socket::setReceiveBufferSize(descriptor, 0));
        EXPECT_FALSE(Socket::setReceiveBufferSize(descriptor, -4096));

        EXPECT_TRUE(Socket::setSendBufferSize(descriptor, 64 * 1024));
        int sendBufferSize = 0;
        ASSERT_TRUE(readIntegerOption(descriptor, SOL_SOCKET, SO_SNDBUF, sendBufferSize));
        // 内核可以在请求值之上加码（Linux 会翻倍留元数据），只不能更少，也不能是 0
        EXPECT_GE(sendBufferSize, 64 * 1024) << "请求了 64 KiB 却拿到 " << sendBufferSize;

        FileDescriptor::close(descriptor);
    }

    /**
     * @brief 钉住：双栈开关按 int 尺寸传递并读得回来
     * @details 布尔值若按 sizeof(bool)（1 字节）传给 setsockopt，两侧行为都不保证；本层刻意按 int
     *          传。没有 IPv6 的环境建不出套接字，按缺依赖跳过而不是失败。
     */
    TEST(Socket, Ipv6OnlyFlagRoundTripsAsAnInt)
    {
        Socket::initialize();
        const int descriptor = static_cast<int>(::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP));
        if (!FileDescriptor::isValid(descriptor))
        {
            GTEST_SKIP() << "本机没有可用的 IPv6 协议栈，双栈开关无从验证";
        }

        EXPECT_TRUE(Socket::setIpv6Only(descriptor, true));
        int onlyV6 = 0;
        ASSERT_TRUE(readIntegerOption(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, onlyV6));
        EXPECT_EQ(onlyV6, 1);

        EXPECT_TRUE(Socket::setIpv6Only(descriptor, false));
        ASSERT_TRUE(readIntegerOption(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, onlyV6));
        EXPECT_EQ(onlyV6, 0);

        FileDescriptor::close(descriptor);
    }

    /**
     * @brief 钉住：取不到 SO_ERROR 时交回真实错误码，而不是「0 = 连接已建立」
     * @details 异步连接靠这个返回值判成功；把「连描述符都不认」报成 0，调用方会把一次彻底的失败
     *          当成握手完成，之后对着一个不存在的连接收发。
     */
    TEST(Socket, TakePendingErrorDistinguishesUnusableDescriptorFromNoError)
    {
        Socket::initialize();
        EXPECT_NE(Socket::takePendingError(FileDescriptor::kInvalid), 0)
                << "无效描述符必须交出非 0 的错误码，否则与「没有错误」同形";

        std::uint16_t assignedPort = 0;
        const int     listener     = createLoopbackListener(assignedPort);
        ASSERT_TRUE(FileDescriptor::isValid(listener));
        // 刚建好还没接受过任何连接的监听套接字：SO_ERROR 读得到且为 0
        EXPECT_EQ(Socket::takePendingError(listener), 0);

        FileDescriptor::close(listener);
    }

    /**
     * @brief 钉住：段里有长度却没数据时，两平台都按参数非法收口
     * @details 此前只有 Windows 分支查这一形状；POSIX 侧把它交给 sendmsg 只会得到一个随地址取值
     *          变化的 EFAULT，同一份参数在两平台返回不同错误码，调用方的分支就没法写。
     */
    TEST(Socket, WriteVectoredRejectsSegmentWithoutDataOnBothPlatforms)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;
        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        const Socket::WriteBuffer buffers[] = {
                {nullptr, 5},
        };
        EXPECT_EQ(Socket::writeVectored(writeDescriptor, buffers, 1), -1);
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument);

        // 长度为 0 的空段是合法的（调用方常拿它占位），不能被同一判据一起拒掉
        const char placeholder[] = "a";
        const Socket::WriteBuffer acceptableBuffers[] = {
                {placeholder, sizeof(placeholder) - 1},
                {nullptr, 0},
        };
        EXPECT_GT(Socket::writeVectored(writeDescriptor, acceptableBuffers, 2), 0);

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

#if ASYN_PLATFORM_WIN32
    /**
     * @brief 钉住（Windows）：放回一份引用不会把还活着的套接字的 Winsock 支撑抽走
     * @details initialize/finalize 若按「启动过没有」这个布尔量记账，第二个持有者析构时就会
     *          WSACleanup 掉第一个持有者还在用的套接字——症状是之后所有 socket 调用都以
     *          WSANOTINITIALISED 失败，而代码看起来一切正常。这里放回一份引用后必须仍能完成
     *          一次真实的建套接字 + 收发。
     */
    TEST(Socket, WinsockStaysInitializedUntilTheLastReferenceIsReleased)
    {
        ASSERT_TRUE(Socket::initialize());
        ASSERT_TRUE(Socket::initialize());

        Socket::finalize();

        const int descriptor = createStreamSocket();
        ASSERT_TRUE(FileDescriptor::isValid(descriptor))
                << "还有一份引用没放回，Winsock 却被清理了：错误码 " << PlatformError::lastSocketErrorCode();
        EXPECT_TRUE(Socket::setNoDelay(descriptor));

        // 本用例借来的两份引用要如数归还，否则同一进程里后续用例会看到被清理掉的 Winsock
        Socket::finalize();
        ASSERT_TRUE(Socket::initialize());
        FileDescriptor::close(descriptor);
    }
#endif
} // namespace AsynGyanis::Platform
