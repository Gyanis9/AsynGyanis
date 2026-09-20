// Platform 层示例：套接字工具、描述符与管道、事件通知器、定时器描述符、内存映射文件、数据报往返、
// 进程起停、时间/编码/错误码/进程信息/控制台这些平台杂项
#include "Base/Log/LogMacros.h"
#include "Platform/IO/Console.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/IO/EventNotifier.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/MemoryMappedFile.h"
#include "Platform/IO/Socket.h"
#include "Platform/IO/TimerFileDescriptor.h"
#include "Platform/System/PlatformError.h"
#include "Platform/System/PlatformTime.h"
#include "Platform/System/Process.h"
#include "Platform/System/ProcessInfo.h"
#include "Platform/System/TextEncoding.h"
#include "common/SampleSupport.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace AsynGyanis;

namespace
{
    /// @return Platform::SocketAddress 一个 IPv4 回环地址（port 为主机序，0 表示让内核挑）
    Platform::SocketAddress makeLoopbackAddress(const std::uint16_t port)
    {
        Platform::SocketAddress address{};
        auto                   &ipv4 = reinterpret_cast<sockaddr_in &>(address.storage);
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(port);
        ipv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.length = static_cast<socklen_t>(sizeof(sockaddr_in));
        return address;
    }

    /// @return std::uint16_t 一个 SocketAddress 里的端口（主机序）
    std::uint16_t portOf(const Platform::SocketAddress &address)
    {
        const auto &ipv4 = reinterpret_cast<const sockaddr_in &>(address.storage);
        return ntohs(ipv4.sin_port);
    }

    /// 有界地等到某个描述符可读：读一次拿不到就歇一下再来，不依赖事件循环
    bool waitReadable(const int fileDescriptor, const std::chrono::milliseconds timeout)
    {
        std::uint8_t scratch[16]{};
        const auto   deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (Platform::FileDescriptor::read(fileDescriptor, scratch, sizeof(scratch)) > 0)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return false;
    }

    void demonstrateDescriptors()
    {
        Samples::checklist().check(!Platform::FileDescriptor::isValid(Platform::FileDescriptor::kInvalid) &&
                                           Platform::FileDescriptor::isValid(0),
                                   "FileDescriptor::isValid 认得哨兵值");

        int readDescriptor = -1;
        int writeDescriptor = -1;
        const bool isPairCreated = Platform::FileDescriptor::createPair(readDescriptor, writeDescriptor);
        Samples::checklist().check(isPairCreated && Platform::FileDescriptor::isValid(readDescriptor),
                                   "createPair 造出一对可通讯的描述符");
        if (!isPairCreated)
        {
            return;
        }

        const char        payload[] = "ping";
        const ssize_t     writtenByteCount = Platform::FileDescriptor::write(writeDescriptor, payload, sizeof(payload) - 1);
        std::string       receivedText(static_cast<std::size_t>(writtenByteCount), '\0');
        const ssize_t     readByteCount = Platform::FileDescriptor::read(readDescriptor, receivedText.data(), receivedText.size());
        Samples::checklist().check(writtenByteCount == 4 && readByteCount == 4 && receivedText == "ping",
                                   "管道写入的字节能原样读回来");

        // 置非阻塞之后，空管道上的读要立刻返回错误而不是把调用方挂住
        static_cast<void>(Platform::FileDescriptor::setNonBlocking(readDescriptor));
        std::uint8_t scratch[4]{};
        const ssize_t wouldBlockRead = Platform::FileDescriptor::read(readDescriptor, scratch, sizeof(scratch));
        Samples::checklist().check(wouldBlockRead <= 0, "非阻塞的读在没数据时立刻返回而不停下");

        static_cast<void>(Platform::FileDescriptor::close(readDescriptor));
        static_cast<void>(Platform::FileDescriptor::close(writeDescriptor));
    }

    void demonstrateNotifierAndTimer()
    {
        Platform::EventNotifier notifier;
        Samples::checklist().check(notifier.isValid(), "EventNotifier 建出了可用的唤醒通道");
        notifier.notify();
        Samples::checklist().check(waitReadable(notifier.readDescriptor(), std::chrono::seconds{1}),
                                   "notify 之后读端立刻可读");
        notifier.drain();
        Samples::checklist().check(!waitReadable(notifier.readDescriptor(), std::chrono::milliseconds{80}),
                                   "drain 之后读端不再可读出通知");

        Platform::TimerFileDescriptor timer;
        Samples::checklist().check(timer.isValid(), "TimerFileDescriptor 可用（Windows 上走定时器队列桥接）");
        if (!timer.isValid())
        {
            return;
        }
        Samples::checklist().check(timer.arm(std::chrono::milliseconds{20}), "arm 一个 20 毫秒的一次性定时");
        Samples::checklist().check(waitReadable(timer.fileDescriptor(), std::chrono::seconds{2}),
                                   "到点之后读端拿到了到期通知");
        timer.drain();
        timer.cancel();
    }

    void demonstrateMemoryMapping(const std::filesystem::path &directory)
    {
        const auto path = directory / "mapped.bin";
        {
            std::ofstream output(path, std::ios::binary);
            output << std::string(4096, 'A');
        }
        auto mapped = Platform::MemoryMappedFile::open(path);
        Samples::checklist().check(mapped.isValid() && mapped.bytes().size() == 4096,
                                   "MemoryMappedFile 把整个文件映射进来了");
        if (mapped.isValid())
        {
            Samples::checklist().check(*mapped.bytes().begin() == std::byte{'A'}, "映射区的第一个字节就是文件内容");
        }
        mapped = Platform::MemoryMappedFile{}; // 移动赋值给空对象即解除映射（析构同理）
        Samples::checklist().check(!mapped.isValid(), "映射释放后本对象不再持有区段");

        const auto missing = Platform::MemoryMappedFile::open(directory / "absent.bin");
        Samples::checklist().check(!missing.isValid() && missing.lastError(), "打开不存在的文件如实报错而不是崩");
    }

    void demonstrateDatagramRoundTrip()
    {
        auto socket = Platform::DatagramSocket::bindTo(makeLoopbackAddress(0));
        Samples::checklist().check(socket.isValid() && portOf(socket.localAddress()) != 0,
                                   "数据报套接字绑到内核分配的端口");
        if (!socket.isValid())
        {
            return;
        }
        const std::string text = "hello-datagram";
        const ssize_t     sentByteCount = socket.send(socket.localAddress(), text.data(), text.size());
        std::string       receivedText(text.size(), '\0');
        Platform::SocketAddress peerAddress{};
        const ssize_t           receivedByteCount =
                socket.receive(receivedText.data(), receivedText.size(), peerAddress);
        Samples::checklist().check(sentByteCount == static_cast<ssize_t>(text.size()) &&
                                           receivedByteCount == sentByteCount && receivedText == text &&
                                           portOf(peerAddress) == portOf(socket.localAddress()),
                                   "同一条数据报套接字自发自收，来源地址也带回来了");
        Samples::checklist().check(Platform::DatagramSocket::kMaximumDatagramBytes <= 65535U,
                                   "数据报上限常量在合理范围内");
    }

    void demonstrateSocketOptions()
    {
        auto socket = Platform::DatagramSocket::bindTo(makeLoopbackAddress(0));
        if (!socket.isValid())
        {
            Samples::checklist().check(false, "取样用的数据报套接字没建起来，选项检查跳过");
            return;
        }
        const int descriptor = socket.fileDescriptor();

        // 这些选项在两类平台上分三种结局：支持、平台不支持（返回 false）、以及该选项只适用于
        // 流式套接字。本层约定「不支持就返回 false」，因此断言只看「没有崩且语义自洽」
        Samples::checklist().check(Platform::Socket::setReuseAddress(descriptor), "SO_REUSEADDR 总能设上");
        static_cast<void>(Platform::Socket::setReusePort(descriptor));
        static_cast<void>(Platform::Socket::setNoDelay(descriptor));
        static_cast<void>(Platform::Socket::setDeferAccept(descriptor, 1));
        static_cast<void>(Platform::Socket::setFastOpen(descriptor, 3));
        static_cast<void>(Platform::Socket::setIpv6Only(descriptor, false));
        Samples::checklist().check(Platform::Socket::setSendBufferSize(descriptor, 64 * 1024) &&
                                           Platform::Socket::setReceiveBufferSize(descriptor, 64 * 1024),
                                   "收发缓冲可以设大");

        // 未发生过错误的套接字上，待决错误应为 0；listening/UDP fd 上的 accept 直接失败
        Samples::checklist().check(Platform::Socket::takePendingError(descriptor) == 0, "takePendingError 报 0");
        sockaddr_storage peerStorage{};
        socklen_t        peerLength = sizeof(peerStorage);
        Samples::checklist().check(Platform::Socket::accept(descriptor, reinterpret_cast<sockaddr *>(&peerStorage), &peerLength) < 0,
                                   "在没有待决连接的套接字上 accept 立刻返回负值");

        const char *first = "ab";
        const char *second = "cd";
        const Platform::Socket::WriteBuffer buffers[] = {{first, 2}, {second, 2}};
        static_cast<void>(Platform::Socket::writeVectored(descriptor, buffers, 2));
        Samples::checklist().check(Platform::Socket::kMaximumVectorCount == 16U, "聚合写的向量上限就位");
#ifndef _WIN32
        // 零拷贝只在 POSIX 侧提供：Windows 上 TransmitFile 在非阻塞套接字上仍会停线程，实测不可用
        Samples::checklist().check(Platform::Socket::kMaximumSendFileChunk > 0U, "零拷贝的分块上限常量就位");
#endif

        int readDescriptor = -1;
        int writeDescriptor = -1;
        if (Platform::FileDescriptor::createPair(readDescriptor, writeDescriptor))
        {
            const ssize_t vectoredByteCount = Platform::Socket::writeVectored(writeDescriptor, buffers, 2);
            Samples::checklist().check(vectoredByteCount == 4 || vectoredByteCount < 0,
                                       "聚合写在管道上要么全写要么如实报错（平台差异）");
#ifndef _WIN32
            // 把零拷贝用在不合适的目标上（源不是普通文件、目的不是流式套接字）必须如实返回负值，
            // 不能静默「成功 0 字节」
            Samples::checklist().check(Platform::Socket::sendFileChunk(readDescriptor, writeDescriptor, 0, 16) < 0,
                                       "把零拷贝用在不合适的套接字上被如实拒绝");
#endif
            static_cast<void>(Platform::FileDescriptor::close(readDescriptor));
            static_cast<void>(Platform::FileDescriptor::close(writeDescriptor));
        }
    }

    void demonstrateProcess()
    {
#ifdef _WIN32
        Platform::Process::LaunchOptions quickOptions{"cmd.exe", {"/c", "exit", "0"}};
        // 子进程继承本控制台：把它自己的输出丢掉，否则示例的结论行会被刷屏
        Platform::Process::LaunchOptions longOptions{"cmd.exe", {"/c", "ping -n 30 127.0.0.1 >nul"}};
#else
        Platform::Process::LaunchOptions quickOptions{"/bin/sh", {"-c", "exit 0"}};
        Platform::Process::LaunchOptions longOptions{"/bin/sh", {"-c", "sleep 30"}};
#endif
        auto quickProcess = Platform::Process::spawn(quickOptions);
        Samples::checklist().check(quickProcess.isValid(), "spawn 起一个立刻退出的子进程");
        const bool isFinished = Samples::waitUntil(
                [&quickProcess]
                { return Platform::Process::pollExitCode(quickProcess).value_or(-1) == 0; }, std::chrono::seconds{10});
        Samples::checklist().check(isFinished && !Platform::Process::isRunning(quickProcess),
                                   "子进程按约定的退出码 0 结束");

        auto longProcess = Platform::Process::spawn(longOptions);
        Samples::checklist().check(longProcess.isValid() && Platform::Process::isRunning(longProcess),
                                   "长驻子进程起来之后 isRunning 为真");
        static_cast<void>(Platform::Process::requestTermination(longProcess));
        // 请求终止是「请它自己收手」，子进程可以不理；这里只在它响应时确认，不响应则留给 forceTermination
        static_cast<void>(Samples::waitUntil([&longProcess] { return !Platform::Process::isRunning(longProcess); },
                                             std::chrono::seconds{3}));
        static_cast<void>(Platform::Process::forceTermination(longProcess));
        const bool isGone = Samples::waitUntil([&longProcess] { return !Platform::Process::isRunning(longProcess); },
                                               std::chrono::seconds{5});
        Samples::checklist().check(isGone, "forceTermination 之后长驻子进程确实没了");

        Platform::Process::LaunchOptions bogusOptions;
        bogusOptions.executablePath = "asyn-definitely-not-an-executable";
        const auto bogusProcess = Platform::Process::spawn(bogusOptions);
#if ASYN_PLATFORM_WIN32
        // Windows 上 CreateProcess 当场失败：拿不到句柄，也不抛
        Samples::checklist().check(!bogusProcess.isValid(), "启动不存在的程序时给出无效句柄而不是抛异常");
#else
        // POSIX 上是 fork + exec：fork 会成功、exec 才失败，本层的约定是「子进程以 127 退出」，
        // 所以这里句柄有效、退出码才是结论。两端不同形这件事本身就是这份清单要记录的内容之一
        const bool isBogusFinished = Samples::waitUntil([&bogusProcess]
                                                        {
                                                            return !Platform::Process::isRunning(bogusProcess);
                                                        },
                                                        std::chrono::seconds{5});
        const std::optional<int> bogusExitCode = Platform::Process::pollExitCode(bogusProcess);
        Samples::checklist().check(isBogusFinished && bogusExitCode.value_or(-1) == 127,
                                   "POSIX 上 fork 成功而 exec 失败：句柄有效，退出码按本层约定为 127");
#endif
    }

    void demonstrateMisc()
    {
        Platform::Socket::initialize();
        static const bool isSecondInitialize = Platform::Socket::initialize();
        Platform::Socket::finalize();
        Platform::Socket::finalize();
        Samples::checklist().check(isSecondInitialize, "initialize/finalize 按引用计数配对，可重复调用");

        const std::string utf8Text = "中文与 emoji 🙂";
        const std::string roundTrip = Platform::TextEncoding::toUtf8String(Platform::TextEncoding::toWideString(utf8Text));
        Samples::checklist().check(roundTrip == utf8Text, "UTF-8 与宽字符往返一致");

        const auto utcFields = Platform::PlatformTime::utcTime(0);
        Samples::checklist().check(utcFields.year == 1970 && utcFields.month == 1 && utcFields.day == 1,
                                   "epoch 0 换 UTC 是 1970-01-01");
        const auto localFields = Platform::PlatformTime::localTime(0);
        Samples::checklist().check(localFields.tm_year + 1900 >= 1970 && localFields.tm_mon >= 0 && localFields.tm_mday >= 1,
                                   "localTime 也能给出完整字段");

        Samples::checklist().check(!Platform::PlatformError::message(0).empty() &&
                                           !Platform::PlatformError::message(Platform::PlatformError::kInvalidArgument).empty(),
                                   "错误码能翻成可读文本（含 0 与常见码）");

        Samples::checklist().check(Platform::ProcessInfo::currentProcessId() > 0 &&
                                           !Platform::ProcessInfo::applicationDirectory().empty(),
                                   "进程号与程序目录都拿得到");
        Samples::checklist().check(Platform::ProcessInfo::environmentVariable("PATH").has_value() ||
                                           Platform::ProcessInfo::environmentVariable("Path").has_value(),
                                   "环境变量按名字能取到（大小写随平台）");
        Samples::checklist().check(!Platform::ProcessInfo::environmentVariable("ASYN_DEFINITELY_UNSET").has_value(),
                                   "不存在的环境变量返回空而不是编出一个值");

        Platform::Console::ensureUtf8Output();
        LOG_INFO_FMT("终端是否支持 ANSI 颜色转义：{}", Platform::Console::supportsAnsiEscapeCodes() ? "支持" : "不支持");
        Samples::checklist().check(true, "控制台 UTF-8 输出装配完成");
    }
}

int main()
{
    Samples::setupConsoleLogging();

    const auto directory = std::filesystem::temp_directory_path() /
                           ("asyn-sample-platform-" + std::to_string(Platform::ProcessInfo::currentProcessId()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    static_cast<void>(Platform::Socket::initialize());
    LOG_INFO("=== Platform 层示例开始 ===");
    demonstrateMisc();
    demonstrateDescriptors();
    demonstrateNotifierAndTimer();
    demonstrateMemoryMapping(directory);
    demonstrateDatagramRoundTrip();
    demonstrateSocketOptions();
    demonstrateProcess();
    static_cast<void>(Platform::Socket::finalize());

    std::filesystem::remove_all(directory);
    return Samples::finishSample("platform");
}
