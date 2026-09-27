// 换代交接通道的用例：同进程内把监听套接字交出去再收回来，收回的那份还接得住连接

#include "Core/Process/UpgradeChannel.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Platform/System/ProcessInfo.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace AsynGyanis::Core
{
    namespace
    {
        using namespace std::chrono_literals;

        /// 连通道与等交接的预算：本机回环上这些都是毫秒级的事，给到秒级只是防 CI 抖
        constexpr std::chrono::milliseconds kAdoptBudget{3000};

        /**
         * @brief 造一个已在监听的回环套接字，并把端口交出来
         * @return int 描述符；负值表示没造出来
         */
        int makeLoopbackListener(std::uint16_t &port)
        {
            port                   = 0U;
            const int   descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = 0;
            if (descriptor < 0 || ::bind(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || ::listen(descriptor, 16) != 0)
            {
                if (descriptor >= 0)
                {
                    static_cast<void>(Platform::FileDescriptor::close(descriptor));
                }
                return -1;
            }
            socklen_t length = static_cast<socklen_t>(sizeof(address));
            if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &length) != 0)
            {
                static_cast<void>(Platform::FileDescriptor::close(descriptor));
                return -1;
            }
            port = ntohs(address.sin_port);
            return descriptor;
        }

        /// 向某个回环端口连一条客户端连接；返回负值表示连不上
        int connectToLoopback(const std::uint16_t port)
        {
            const int   descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port        = htons(port);
            if (descriptor < 0 || ::connect(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
            {
                if (descriptor >= 0)
                {
                    static_cast<void>(Platform::FileDescriptor::close(descriptor));
                }
                return -1;
            }
            return descriptor;
        }
    } // namespace

    /**
     * @brief 钉住换代的地基本身：交出去又收回来的那个描述符，仍在替这个端口accept
     * @details 这条用例不需要第二个进程——SCM_RIGHTS 与 WSADuplicateSocket 都能在同进程内换一份
     *          引用（平台的 write/read 一对函数就是成对入口），因此判据可以在两平台上都跑住。
     * @details 关键是**先关掉原来那份再连**：端口若真跟着老描述符一起没了，连接根本到不了接受队列，
     *          收回来的描述符也就 accept 不出来。「老的不在了、新的仍接得住」才是零停机的实质。
     */
    TEST(UpgradeChannel, HandedOverListenerKeepsAcceptingAfterOriginalIsClosed)
    {
        // 本用例直接用平台套接字调用：Windows 上要先有 Winsock 初始化引用（POSIX 上是空操作）
        const Platform::Socket::Initialization network;
        std::uint16_t                          port          = 0U;
        const int                              oldGeneration = makeLoopbackListener(port);
        ASSERT_GE(oldGeneration, 0) << "夹具没能造出监听套接字";

        auto channel = UpgradeChannel::open();
        ASSERT_TRUE(channel.has_value()) << channel.error();
        ASSERT_FALSE(channel->address().empty());

        // 交棒侧放到另一条线程上：通道两端都是阻塞调用，同一条线程上做两侧会自己等自己
        std::thread       handoffThread;
        std::atomic<bool> isHandedOff{false};
        std::string       handoffError;
        handoffThread = std::thread(
                [&]
                {
                    auto peer = channel->waitForPeer();
                    if (!peer.has_value())
                    {
                        handoffError = peer.error();
                        return;
                    }
                    auto written = channel->handOffListener(*peer, oldGeneration, static_cast<std::uint64_t>(Platform::ProcessInfo::currentProcessId()));
                    if (!written.has_value())
                    {
                        handoffError = written.error();
                        return;
                    }
                    isHandedOff.store(true, std::memory_order_release);
                    static_cast<void>(Platform::FileDescriptor::close(*peer));
                });

        auto adopted = adoptHandedOverListener(channel->address(), kAdoptBudget);
        handoffThread.join();
        ASSERT_TRUE(adopted.has_value()) << "没收回监听套接字：" << adopted.error() << "；交棒侧：" << handoffError;
        ASSERT_TRUE(isHandedOff.load(std::memory_order_acquire)) << "交棒侧没写完：" << handoffError;

        static_cast<void>(Platform::FileDescriptor::close(oldGeneration));

        const int client = connectToLoopback(port);
        ASSERT_GE(client, 0) << "老一代收口之后端口就连不上了：移交并没有真的把监听态带过来";
        const int accepted = Platform::Socket::accept(*adopted, nullptr, nullptr);
        EXPECT_GE(accepted, 0) << "收回来的描述符 accept 不出这条已排队的连接";
        if (accepted >= 0)
        {
            static_cast<void>(Platform::FileDescriptor::close(accepted));
        }
        static_cast<void>(Platform::FileDescriptor::close(client));
        static_cast<void>(Platform::FileDescriptor::close(*adopted));
    }

    /**
     * @brief 拒绝面：连不上通道时在预算内交回失败，而不是无限期挂着
     * @details 新一代可能在老一代还没把通道建好之前就启动了，所以「连不上」是常态而不是结论；
     *          但一直连不上必须有尽头，否则换代脚本会停在那里没人看得见。
     */
    TEST(UpgradeChannel, AdoptGivesUpWithinItsBudgetWhenNobodyListens)
    {
        const Platform::Socket::Initialization network;
        const std::string                      nobodyListens = "127.0.0.1:1";
        const auto                             started       = std::chrono::steady_clock::now();
        auto                                   adopted       = adoptHandedOverListener(nobodyListens, std::chrono::milliseconds{300});
        const auto                             spent         = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

        ASSERT_FALSE(adopted.has_value());
        EXPECT_NE(adopted.error().find("预算"), std::string::npos) << adopted.error();
        EXPECT_GE(spent, 200ms) << "没到预算就放弃了：一次竞态会被当成换代失败";
        EXPECT_LT(spent, 5000ms) << "预算用完还在试：这条路会把换代脚本挂住";
    }

    /**
     * @brief 钉住通道文件的收尾：同一进程内两条通道的地址互不相同，收口之后不留残文件
     * @details 套接字文件不在原地，下一次 bind 就不会以 EADDRINUSE 失败——换过几次代的机器上，
     *          这些残文件是「换代突然开不出通道」最常见的原因。Windows 上通道是回环端口，没有文件，
     *          那条判据不适用，本例在 Windows 上跳过。
     */
    TEST(UpgradeChannel, ChannelAddressesDoNotCollideAndFilesAreRemovedOnClose)
    {
        const Platform::Socket::Initialization network;
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "Windows 上的交接通道是回环端口，没有套接字文件要收尾";
#else
        auto first = UpgradeChannel::open();
        ASSERT_TRUE(first.has_value()) << first.error();
        auto second = UpgradeChannel::open();
        ASSERT_TRUE(second.has_value()) << second.error();
        EXPECT_NE(first->address(), second->address());
        EXPECT_TRUE(std::filesystem::exists(first->address()));

        const std::string firstAddress = first->address();
        first->closeChannel();
        EXPECT_FALSE(std::filesystem::exists(firstAddress)) << "收口没把套接字文件带走：下一次换代会在 bind 上撞 EADDRINUSE";
#endif
    }
} // namespace AsynGyanis::Core
