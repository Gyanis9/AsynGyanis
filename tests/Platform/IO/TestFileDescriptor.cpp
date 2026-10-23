// FileDescriptor 单元测试：有效性判定、非阻塞、描述符对与读写关闭
#include "Platform/IO/FileDescriptor.h"

#include "Platform/IO/HandleFaultInjection.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <string>
#include <thread>

#if !ASYN_PLATFORM_WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#endif

#include "PlatformTestSupport.h"

namespace AsynGyanis::Platform
{
    TEST(FileDescriptor, IsValidRejectsNegativeDescriptors)
    {
        EXPECT_FALSE(FileDescriptor::isValid(FileDescriptor::kInvalid));
        EXPECT_FALSE(FileDescriptor::isValid(-42));
    }

    TEST(FileDescriptor, IsValidAcceptsZeroAndPositiveDescriptors)
    {
        EXPECT_TRUE(FileDescriptor::isValid(0));
        EXPECT_TRUE(FileDescriptor::isValid(7));
    }

    TEST(FileDescriptor, InvalidConstantIsNegativeOne)
    {
        EXPECT_EQ(FileDescriptor::kInvalid, -1);
    }

    TEST(FileDescriptor, ReadOnInvalidDescriptorFailsWithoutTouchingBuffer)
    {
        char buffer[8] = {0};

        EXPECT_EQ(FileDescriptor::read(FileDescriptor::kInvalid, buffer, sizeof(buffer)), -1);
        EXPECT_EQ(FileDescriptor::write(FileDescriptor::kInvalid, buffer, sizeof(buffer)), -1);
    }

    /**
     * @brief 钉住：无效描述符这三条拒掉的入口，要把「为什么拒」留在 PlatformError 里
     * @details 本接口的失败语义写成「-1，原因见 PlatformError」，而原先只有长度超限那一条置了码：
     *          描述符无效时直接返回，调用方查到的码属于上一次别的调用——报出来的原因是假的。
     *          判据刻意取「先把码清成 0、再问它」：不这么写的用例在实现不置码时也会蒙对。
     *          取值与 Socket::writeVectored 对齐（无效描述符与超限长度同归 kInvalidArgument），
     *          PlatformError 没有 EBADF 一档，两平台要给出同一个数就只能用这一个。
     */
    TEST(FileDescriptor, InvalidDescriptorFailuresLeaveTheirOwnReason)
    {
        char buffer[8] = {0};

        PlatformError::setLastErrorCode(0);
        EXPECT_EQ(FileDescriptor::read(FileDescriptor::kInvalid, buffer, sizeof(buffer)), -1);
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument);
        EXPECT_EQ(PlatformError::lastErrorCode(), PlatformError::kInvalidArgument) << "只置了 socket 那一侧，按文件类通道读错误码的调用方拿到的还是残值";

        PlatformError::setLastErrorCode(0);
        EXPECT_EQ(FileDescriptor::write(FileDescriptor::kInvalid, buffer, sizeof(buffer)), -1);
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument);
        EXPECT_EQ(PlatformError::lastErrorCode(), PlatformError::kInvalidArgument);

        PlatformError::setLastErrorCode(0);
        EXPECT_FALSE(FileDescriptor::markNonInheritable(FileDescriptor::kInvalid));
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument);
        EXPECT_EQ(PlatformError::lastErrorCode(), PlatformError::kInvalidArgument);
    }

    /**
     * @brief 标记「不随 spawn 传给子进程」要落到查得回的位置上，而不只是返回 true
     * @details 判据是先把继承位打回去、再调这一次、再查一遍：只看返回值会放过「调用成功但标志写歪」
     *          那一类实现（比如把 F_SETFD 写成 F_SETFL，fcntl 两趟都返回 0）。createPair 出来的两端
     *          本来就是不可继承的，不先打回去这条用例在坏实现下也会绿。
     */
    TEST(FileDescriptor, MarkNonInheritableActuallyClearsTheInheritFlag)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;
        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

#if ASYN_PLATFORM_WIN32
        const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(readDescriptor));
        ASSERT_NE(::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT), 0);
#else
        const int initialFlags = ::fcntl(readDescriptor, F_GETFD);
        ASSERT_GE(initialFlags, 0);
        ASSERT_NE(::fcntl(readDescriptor, F_SETFD, initialFlags & ~FD_CLOEXEC), -1);
#endif
        ASSERT_FALSE(TestSupport::isNotInheritable(readDescriptor)) << "继承位没打回去，这条用例就没有被测的那一半";

        EXPECT_TRUE(FileDescriptor::markNonInheritable(readDescriptor));
        EXPECT_TRUE(TestSupport::isNotInheritable(readDescriptor)) << "返回 true 而继承位没清：子进程仍能拿到这一端";

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

    TEST(FileDescriptor, CloseOnInvalidDescriptorIsNoOpSuccess)
    {
        EXPECT_EQ(FileDescriptor::close(FileDescriptor::kInvalid), 0);
    }

    /**
     * @brief 钉住：单次读写的长度上界两平台共用一个界，越界给出同一个「参数非法」错误码
     * @details 界此前只存在于 Windows 分支，且置的是 WSAEMSGSIZE：与 `Socket::writeVectored` 用的
     *          kInvalidArgument 不是一套（PlatformError 的文档把「超出平台上限的长度」明确划给
     *          kInvalidArgument），也没走 PlatformError 通道因此漏置 errno——调用方按
     *          lastErrorCode() 读到的只是上一次留下的残值。POSIX 分支原先根本不判：两 GiB 的读取
     *          长度会原样交给内核，而调用方给的缓冲区只有 8 字节，所以这条用例同时也是那道护栏。
     */
    TEST(FileDescriptor, RefusesTransferLengthsAboveTheSharedBound)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;
        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        const std::size_t oversizedLength = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1U;
        char              buffer[8]       = {0};
        // 先把错误码清成 0：要证的是「本次调用置的码」，不能靠上一次留下的值蒙对
        PlatformError::setLastErrorCode(0);

        EXPECT_EQ(FileDescriptor::read(readDescriptor, buffer, oversizedLength), -1) << "超限的读取长度必须在交给内核之前拒掉";
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument);
        EXPECT_EQ(PlatformError::lastErrorCode(), PlatformError::kInvalidArgument) << "错误码只置在平台侧、没置 errno，调用方按文件类通道读就是残值";

        PlatformError::setLastErrorCode(0);
        EXPECT_EQ(FileDescriptor::write(writeDescriptor, buffer, oversizedLength), -1) << "超限的写入长度宁可失败，也不能少写字节却回报成功";
        EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kInvalidArgument);
        EXPECT_EQ(PlatformError::lastErrorCode(), PlatformError::kInvalidArgument);

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

    TEST(FileDescriptor, SetNonBlockingFailsForInvalidDescriptor)
    {
        EXPECT_FALSE(FileDescriptor::setNonBlocking(FileDescriptor::kInvalid));
    }

    TEST(FileDescriptor, CreatePairReturnsTwoValidDescriptors)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;

        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        EXPECT_TRUE(FileDescriptor::isValid(readDescriptor));
        EXPECT_TRUE(FileDescriptor::isValid(writeDescriptor));
        EXPECT_NE(readDescriptor, writeDescriptor);

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

    TEST(FileDescriptor, CreatePairOutputIsNonBlocking)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;

        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        // 非阻塞描述符在无数据时读取应立即返回 -1 而不是阻塞
        char buffer[16];
        EXPECT_LT(FileDescriptor::read(readDescriptor, buffer, sizeof(buffer)), 0);

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

    TEST(FileDescriptor, WriteFromOneEndIsReadableFromTheOther)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;

        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        const std::string payload = "hello descriptor pair";
        const ssize_t     written = FileDescriptor::write(writeDescriptor, payload.data(), payload.size());
        EXPECT_EQ(written, static_cast<ssize_t>(payload.size()));

        char        buffer[64] = {0};
        std::size_t totalRead  = 0;
        const auto  deadline   = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while (totalRead < payload.size() && std::chrono::steady_clock::now() < deadline)
        {
            const ssize_t bytesRead = FileDescriptor::read(readDescriptor, buffer + totalRead, payload.size() - totalRead);
            if (bytesRead > 0)
            {
                totalRead += static_cast<std::size_t>(bytesRead);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        EXPECT_EQ(std::string(buffer, totalRead), payload);

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

    TEST(FileDescriptor, PairIsUsableInBothDirections)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;

        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        constexpr char kmarker = 'A';
        EXPECT_EQ(FileDescriptor::write(readDescriptor, &kmarker, sizeof(kmarker)), 1);
        EXPECT_TRUE(TestSupport::waitForReadable(writeDescriptor, 1000));

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

    TEST(FileDescriptor, CreatePairResetsOutputsOnFailure)
    {
        // 成功路径下输出参数一定被写入有效值；失败路径下保持 kInvalid
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;

        if (!FileDescriptor::createPair(readDescriptor, writeDescriptor))
        {
            EXPECT_EQ(readDescriptor, FileDescriptor::kInvalid);
            EXPECT_EQ(writeDescriptor, FileDescriptor::kInvalid);
        } else
        {
            FileDescriptor::close(readDescriptor);
            FileDescriptor::close(writeDescriptor);
        }
        SUCCEED();
    }

    /**
     * @brief 成对的描述符不得随 spawn 传下去：两端都要与父进程再无关系
     * @details 这一对是 EventNotifier 与 TimerFileDescriptor 的底座。可继承的一端一旦被子进程拿到，
     *          父进程关掉自己那端也关不掉这条唤醒通道（子进程还持着一份引用），而子进程往对端写一个
     *          字节就会叫醒父进程的事件循环。POSIX 侧由 SOCK_CLOEXEC 在建好时就带上标记；Windows 的
     *          Winsock 句柄默认就是可继承的，必须建完显式取消继承位。仓库自带的 spawn 已用句柄清单把
     *          继承收窄到三个标准句柄，这条判据是给库外消费者的（本框架以 find_package 与 Conan 包
     *          对外导出，别人的 CreateProcess 未必带清单）。
     */
    TEST(FileDescriptor, CreatePairDescriptorsAreNotInheritable)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;
        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        EXPECT_TRUE(TestSupport::isNotInheritable(readDescriptor)) << "读端可被继承：父进程关闭后唤醒通道仍被子进程持着";
        EXPECT_TRUE(TestSupport::isNotInheritable(writeDescriptor)) << "写端可被继承：子进程写一个字节就会叫醒父进程的循环";

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

#if ASYN_PLATFORM_WIN32
    /**
     * @brief 钉住（Windows）：长度大到会回绕成小正数时，读写按失败收口而不是「搬了 0 字节」
     * @details recv/send 的长度形参是 int。刻意取 4 GiB 这个形状：它回绕之后是正数 0，
     *          于是系统调用正常返回 0 —— 读侧与「对端关闭/暂无数据」同形，写侧看起来是「一个字节都没
     *          发出去但没报错」，两种都不会被调用方察觉。取 INT_MAX+1 那种回绕成负数的形状反而测不出
     *          问题（系统调用自己就会报错），所以这里要量的正是「回绕后仍合法」那一类。
     *          POSIX 侧长度形参本就是 size_t、不存在这次回绕，因此用例只在 Windows 编译。
     */
    TEST(FileDescriptor, WrappingLengthFailsInsteadOfQuietlyTransferringNothing)
    {
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;
        ASSERT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor));

        constexpr std::size_t wrappingLength = static_cast<std::size_t>(1) << 32;
        char                  buffer[1]      = {};

        EXPECT_EQ(FileDescriptor::read(readDescriptor, buffer, wrappingLength), -1) << "长度回绕成 0 会安静地什么也没读，而 0 在本接口里另有「对端关闭」的含义";
        EXPECT_EQ(FileDescriptor::write(writeDescriptor, buffer, wrappingLength), -1) << "写侧同样的回绕会报成「发送了 0 字节」，调用方以为已经发完";

        FileDescriptor::close(readDescriptor);
        FileDescriptor::close(writeDescriptor);
    }

    /**
     * @brief 钉住（Windows）：句柄建不出来按当次调用分类，清理不覆盖原因，注入清除后能再建
     * @details Windows 没有可用的 per-process 句柄上限，真耗尽会伤及整台机器；经测试注入点
     *          分别让监听 socket、客户端 socket、accept 失败，三条出口都要给出 WSAEMFILE，
     *          输出保持无效。旧实现的 `listener < 0` 判不出 INVALID_SOCKET（SOCKET 是无符号的），
     *          第一条会在下一步 bind 处把原因换成 WSAENOTSOCK。
     */
    TEST(FileDescriptor, InjectedHandleExhaustionIsClassifiedAndRecovers)
    {
        const auto expectInjectedFailureAndRecovery = [](const int allowedSuccesses)
        {
            Platform::Testing::failHandleCreationAfter(allowedSuccesses, PlatformError::kTooManyOpenFiles);
            PlatformError::setLastErrorCode(0);

            int readDescriptor  = FileDescriptor::kInvalid;
            int writeDescriptor = FileDescriptor::kInvalid;
            EXPECT_FALSE(FileDescriptor::createPair(readDescriptor, writeDescriptor)) << "第 " << (allowedSuccesses + 1) << " 次句柄创建失败后 createPair 仍报成功";
            EXPECT_EQ(readDescriptor, FileDescriptor::kInvalid);
            EXPECT_EQ(writeDescriptor, FileDescriptor::kInvalid);
            EXPECT_EQ(PlatformError::lastErrorCode(), PlatformError::kTooManyOpenFiles) << "失败原因被清理路径覆盖了：读到的是关闭或下一步调用的错误";
            EXPECT_EQ(PlatformError::lastSocketErrorCode(), PlatformError::kTooManyOpenFiles);

            Platform::Testing::clearHandleCreationFailure();
            EXPECT_TRUE(FileDescriptor::createPair(readDescriptor, writeDescriptor)) << "清除注入后仍建不出描述符对";
            FileDescriptor::close(readDescriptor);
            FileDescriptor::close(writeDescriptor);
        };

        expectInjectedFailureAndRecovery(0);
        expectInjectedFailureAndRecovery(1);
        expectInjectedFailureAndRecovery(2);
    }
#endif

    /**
     * @brief 真实 fd 耗尽后 accept/createPair 按 EMFILE/ENFILE 分类，关掉一条即恢复
     * @details 监听队列里有待接受连接时 fd 限额被占满，内核会让 accept 以 EMFILE 失败；
     *          释放一条后再 accept 必须成功——这正是上层接受循环「退避后重试」依赖的前提。
     *          子进程里改限额，父进程的其余用例不受影响；Windows 没有可用的 per-process
     *          句柄限额，且句柄工厂 seam 尚未实现，本档显式 SKIP。
     */
    TEST(FileDescriptor, ReportsExhaustionAndRecoversAfterOneClose)
    {
#if ASYN_PLATFORM_WIN32
        GTEST_SKIP() << "Windows 没有可用的 per-process 句柄上限；句柄工厂 seam 尚未实现";
#elif defined(__linux__)
        const pid_t child = ::fork();
        ASSERT_NE(child, -1) << "fork 失败，无法造出独立 fd 限额";
        if (child == 0)
        {
            // 子进程不跑 gtest 断言：所有出口都先收掉自己开的东西，再用退出码把阶段带回父进程
            int              listener = FileDescriptor::kInvalid;
            int              client   = FileDescriptor::kInvalid;
            std::vector<int> heldDescriptors;
            const auto       finish = [&](const int exitCode)
            {
                for (const int descriptor: heldDescriptors)
                {
                    ::close(descriptor);
                }
                if (client >= 0)
                {
                    ::close(client);
                }
                if (listener >= 0)
                {
                    FileDescriptor::close(listener);
                }
                ::_exit(exitCode);
            };

            // 先备好一条「已在监听、且队列里有待接受连接」的套接字：限额压下去之后就没法再建了
            listener = Socket::create(AF_INET, SOCK_STREAM);
            if (!FileDescriptor::isValid(listener))
            {
                finish(10);
            }
            constexpr int kreuseAddress = 1;
            if (::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &kreuseAddress, sizeof(kreuseAddress)) != 0)
            {
                finish(11);
            }
            sockaddr_in listenAddress{};
            listenAddress.sin_family      = AF_INET;
            listenAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            listenAddress.sin_port        = 0;
            if (!Socket::bind(listener, reinterpret_cast<const sockaddr *>(&listenAddress), sizeof(listenAddress)))
            {
                finish(12);
            }
            socklen_t addressLength = sizeof(listenAddress);
            if (::getsockname(listener, reinterpret_cast<sockaddr *>(&listenAddress), &addressLength) != 0)
            {
                finish(13);
            }
            if (!Socket::listen(listener, 4))
            {
                finish(14);
            }
            // 客户端用阻塞套接字：未 accept 的连接也能完成三次握手并排在监听队列里
            client = ::socket(AF_INET, SOCK_STREAM, 0);
            if (client < 0)
            {
                finish(15);
            }
            if (::connect(client, reinterpret_cast<const sockaddr *>(&listenAddress), sizeof(listenAddress)) != 0)
            {
                finish(16);
            }

            rlimit limit{};
            if (::getrlimit(RLIMIT_NOFILE, &limit) != 0)
            {
                finish(17);
            }
            std::size_t     currentOpenCount = 0;
            std::error_code error;
            for ([[maybe_unused]] const auto &entry: std::filesystem::directory_iterator("/proc/self/fd", error))
            {
                ++currentOpenCount;
            }
            if (error)
            {
                finish(18);
            }
            limit.rlim_cur = static_cast<rlim_t>(currentOpenCount + 8U);
            if (limit.rlim_max < limit.rlim_cur)
            {
                limit.rlim_max = limit.rlim_cur;
            }
            if (::setrlimit(RLIMIT_NOFILE, &limit) != 0)
            {
                finish(19);
            }

            bool sawLimit = false;
            for (int attempt = 0; attempt < 64; ++attempt)
            {
                const int descriptor = ::open("/dev/null", O_RDONLY);
                if (descriptor < 0)
                {
                    sawLimit = errno == EMFILE || errno == ENFILE;
                    break;
                }
                heldDescriptors.push_back(descriptor);
            }
            if (!sawLimit)
            {
                finish(20);
            }

            // 一：accept 必须失败并把「名额不够」如实分类，而不是返回一个看起来有效的描述符
            const int refusedDescriptor = Socket::accept(listener, nullptr, nullptr);
            if (FileDescriptor::isValid(refusedDescriptor))
            {
                FileDescriptor::close(refusedDescriptor);
                finish(21);
            }
            const int acceptErrorCode = PlatformError::lastSocketErrorCode();
            if (acceptErrorCode != EMFILE && acceptErrorCode != ENFILE)
            {
                finish(22);
            }

            // 二：成对描述符要两条名额，同样必须失败且分类一致（不能退回 pipe 之类只占一条的兜底）
            int readDescriptor  = FileDescriptor::kInvalid;
            int writeDescriptor = FileDescriptor::kInvalid;
            if (FileDescriptor::createPair(readDescriptor, writeDescriptor))
            {
                FileDescriptor::close(readDescriptor);
                FileDescriptor::close(writeDescriptor);
                finish(23);
            }
            const int pairErrorCode = PlatformError::lastErrorCode();
            if (pairErrorCode != EMFILE && pairErrorCode != ENFILE)
            {
                finish(24);
            }

            // 三：只释放一条，accept 需要的正是这一条；不恢复就说明失败被记成了「监听器已坏」
            ::close(heldDescriptors.back());
            heldDescriptors.pop_back();
            const int acceptedDescriptor = Socket::accept(listener, nullptr, nullptr);
            if (!FileDescriptor::isValid(acceptedDescriptor))
            {
                finish(25);
            }
            FileDescriptor::close(acceptedDescriptor);
            finish(0);
        }

        int status = 0;
        ASSERT_EQ(::waitpid(child, &status, 0), child);
        ASSERT_TRUE(WIFEXITED(status)) << "子进程没有正常退出";
        EXPECT_EQ(WEXITSTATUS(status), 0) << "退出码 10=建监听套接字失败，11=setsockopt 失败，12=bind 失败，13=getsockname 失败，"
                                             "14=listen 失败，15=建客户端失败，16=connect 失败，17=getrlimit 失败，"
                                             "18=/proc/self/fd 读不出来，19=setrlimit 失败，20=没开满，"
                                             "21=耗尽后 accept 仍成功，22=accept 错误码不是 EMFILE/ENFILE，"
                                             "23=耗尽后 createPair 仍成功，24=createPair 错误码不是 EMFILE/ENFILE，"
                                             "25=关掉一条后 accept 仍未恢复";
#else
        GTEST_SKIP() << "本平台的 fd 限额制造方式未覆盖";
#endif
    }
} // namespace AsynGyanis::Platform
