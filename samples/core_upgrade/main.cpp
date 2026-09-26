// 零停机换代示例：把正在监听的套接字交给新一代进程，端口全程不关，答话者由本代换到新代

#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/Router.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/Platform.h"
#include "Platform/System/PlatformError.h"
#include "Platform/System/Process.h"
#include "Platform/System/ProcessInfo.h"
#include "common/SampleSupport.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

// 零停机换代的端到端自检：一次调用把整条链跑完，退出码与结论行就是证据。
// 本进程自己 bind/listen，用「接手已在监听的套接字」那条构造入口起服务器；先探一轮确认本代在服务，
// 再派生新一代（同一个可执行体带 --takeover），经一条 unix 域通道用 SCM_RIGHTS 把监听套接字交给它；
// 等新一代答过话之后本代才 drain 收口，最后再探一轮——此时答话的只能是新一代。
//
// 只在 POSIX 上构建（门在 samples/CMakeLists.txt）：Windows 上新一代的事件循环接不住那份移交句柄——
// 句柄与完成端口的关联是内核对象级且不可解除的，交棒方的循环早就把它挂上了自己的端口。那条拒绝现在
// 带得出原因，用例是 IoWatcher.DescriptorOnAnotherCompletionPortThrowsWithWin32Reason。
//
// 通道必须是 AF_UNIX：内核只在 unix 域里随 SCM_RIGHTS 送描述符。拿 loopback TCP 当通道时 sendmsg 与
// recvmsg 两边都返回成功、8 字节数据一字不差，只有描述符被静默丢掉（实测 recvmsg 后 msg_controllen
// 归 0），接收侧只能报「一个描述符也没有」——这条对照实测写在 Socket.h 的 @note 里。
//
// 排序本身就是结论的一部分：交棒方在「新代被验证过」之前绝不能收口，先关就成了端口上没人接。
// 另一半在 Core：监听套接字收口时不能对端点做 shutdown（见 AsyncSocket::markAsListening()），
// 那会把同一端点上新一代那份引用一起停掉——实测正是「父代收口后子代 10/10 连不上」。
//
// 两条实测换来的硬约束，改动时别踩回去：
// · 循环跑在别的线程上时，投它的常驻协程必须用 scheduler().scheduleRemote()——schedule() 是
//   「同线程稍后执行」，从主线程投会永远排不进去（症状是本代一次都不答话）；
// · Core::Task<> 的帧在堆上，投出去之后要让 Task 活到协程跑完，否则 resume 打在已释放帧上。

using namespace AsynGyanis;

namespace
{
    /// 每个阶段的探测次数与间隔
    constexpr int kPhaseProbeCount = 10;
    constexpr int kProbeIntervalMs = 40;

    /// 等新一代答话的总上限：要给够「起进程 + 连通道 + 接手 + 起服务」的时间
    constexpr int kHandoffWaitAttempts = 150;

    /// 本代 drain 的等待上限，以及等它跑完的时间（Task 必须活到这段时间）
    constexpr int kDrainTimeoutMs    = 500;
    constexpr int kDrainSettleWaitMs = 900;

    /// 新一代连通道的重试次数（每次之间歇 50 毫秒）：本代可能还在准备通道，别把一次竞态判成失败
    constexpr int kChannelConnectAttempts = 100;

    /// 强杀新一代之后等它真正消失的上限（句柄释放前必须确认，否则留下僵尸）
    constexpr int kChildExitWaitMs = 2000;

    /// 一个回环端点
    sockaddr_in loopbackEndpoint(const std::uint16_t port)
    {
        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = htons(port);
        return address;
    }

    /**
     * @brief 造一条已在监听的 unix 域通道（描述符只能随 AF_UNIX 过去，理由见文件头）
     * @param path 套接字文件路径
     * @return int 通道监听描述符；失败为 -1
     * @note bind 前先 unlink：上一次运行被强杀时这个文件会留在原地，bind 于是以 EADDRINUSE 失败，
     *       看起来像「端口被占」而其实只是个残文件
     * @note 用 umask 而不是 bind 之后 chmod：套接字文件一建出来就允许别人连，而它交出去的是监听
     *       套接字——那个窗口里任何本机进程连上来都能拿走这份引用
     */
    int createUnixChannelListener(const std::filesystem::path &path)
    {
        const std::string text = path.string();
        if (text.empty() || text.size() > sizeof(sockaddr_un::sun_path) - 1U)
        {
            Samples::printStartupError(std::format("交接通道路径长度 {} 超出 unix 套接字路径上限 {}", text.size(), sizeof(sockaddr_un::sun_path) - 1U));
            return -1;
        }
        const int descriptor = static_cast<int>(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (descriptor < 0)
        {
            return -1;
        }
        const mode_t previousMask = ::umask(S_IRWXG | S_IRWXO);
        sockaddr_un  address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, text.c_str(), sizeof(address.sun_path) - 1U);
        const bool isBound = ::unlink(text.c_str()) == 0 || errno == ENOENT;
        if (isBound && (::bind(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || ::listen(descriptor, 4) != 0))
        {
            Platform::FileDescriptor::close(descriptor);
            ::umask(previousMask);
            return -1;
        }
        ::umask(previousMask);
        if (!isBound)
        {
            Platform::FileDescriptor::close(descriptor);
            return -1;
        }
        return descriptor;
    }

    /// 本代这一轮用的交接通道路径（放在临时目录里，按进程号错开，退出时删掉）
    std::filesystem::path channelPathForCurrentProcess()
    {
        return std::filesystem::temp_directory_path() / ("asyn-core-upgrade-" + std::to_string(Platform::ProcessInfo::currentProcessId()) + ".sock");
    }

    /**
     * @brief 作用域结束时删掉套接字文件的守卫
     * @details bind 之前已经 unlink 过一次（残文件不挡路），这里收尾只是不让临时目录攒下一堆
     *          本示例留下的空壳。删除失败不报：文件不在就是已达目的，路径也不可恢复什么结论。
     */
    class ChannelPathGuard
    {
    public:
        /**
         * @brief 记下要在作用域结束时删掉的路径
         * @param path 套接字文件路径
         */
        explicit ChannelPathGuard(std::filesystem::path path) : m_path(std::move(path))
        {
        }

        ChannelPathGuard(const ChannelPathGuard &) = delete;

        ChannelPathGuard &operator=(const ChannelPathGuard &) = delete;

        /// @brief 删掉那个套接字文件；文件已不在或删不掉都不报（既成事实，也不影响任何结论）
        ~ChannelPathGuard()
        {
            std::error_code ignored;
            std::filesystem::remove(m_path, ignored);
        }

    private:
        std::filesystem::path m_path; ///< 要删掉的套接字文件路径
    };

    /**
     * @brief 造一个已在监听的套接字（端口交给内核挑）
     * @param[out] port 实际端口
     * @return int 描述符；失败为 -1
     */
    int createListeningSocket(std::uint16_t &port)
    {
        port                 = 0U;
        const int descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (descriptor < 0)
        {
            return -1;
        }
        sockaddr_in address = loopbackEndpoint(0U);
        if (::bind(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || ::listen(descriptor, 128) != 0)
        {
            Platform::FileDescriptor::close(descriptor);
            return -1;
        }
        socklen_t length = static_cast<socklen_t>(sizeof(address));
        if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &length) != 0)
        {
            Platform::FileDescriptor::close(descriptor);
            return -1;
        }
        port = ntohs(address.sin_port);
        return descriptor;
    }

    /**
     * @brief 造一条已在监听的交接通道，并给出新一代连它要用的地址文本
     * @param[out] addressText 交给 --takeover 的取值：unix 套接字的路径
     * @return int 通道监听描述符；失败为 -1
     */
    int createChannelListener(std::string &addressText)
    {
        const std::filesystem::path path       = channelPathForCurrentProcess();
        const int                   descriptor = createUnixChannelListener(path);
        addressText                            = path.string();
        return descriptor;
    }

    /**
     * @brief 连上交接通道
     * @param addressText createChannelListener 给出的通道地址
     * @return int 已连通的通道描述符；-1 表示重试到上限仍没连上
     * @note 每次重试都换一个新描述符：connect 失败后的套接字不再保证可用，同一个 fd 上重连不算重试
     */
    int connectChannel(const std::string &addressText)
    {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, addressText.c_str(), sizeof(address.sun_path) - 1U);

        for (int attempt = 0; attempt < kChannelConnectAttempts; ++attempt)
        {
            const int descriptor = static_cast<int>(::socket(AF_UNIX, SOCK_STREAM, 0));
            if (descriptor < 0)
            {
                return -1;
            }
            if (::connect(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0)
            {
                return descriptor;
            }
            Platform::FileDescriptor::close(descriptor);
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        return -1;
    }

    /// 两代注册同一份路由：把本进程 pid 报出去，答话者因此可辨认
    void registerRoutes(Net::Router &router)
    {
        router.get("/pid",
                   [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
                   {
                       response.setStatus(200);
                       response.setHeader("Content-Type", "application/json");
                       response.setBody(R"({"pid":)" + std::to_string(Platform::ProcessInfo::currentProcessId()) + R"(,"generation":"alive"})");
                       co_return;
                   });
    }

    /**
     * @brief 发一次真实 HTTP 请求，答话者的 pid 从正文里取
     * @param port 目标端口
     * @param[out] answeringPid 答话进程号
     * @return true 连接、请求与解析都成功
     */
    bool probeOnce(const std::uint16_t port, long &answeringPid)
    {
        answeringPid     = 0L;
        const int client = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (client < 0)
        {
            return false;
        }
        // 读写都要有界：哪一端卡住就记一次失败，不能让整个示例自己挂在这里
        timeval timeout{};
        timeout.tv_sec  = 2;
        timeout.tv_usec = 0;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));

        sockaddr_in address = loopbackEndpoint(port);
        if (::connect(client, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
        {
            Platform::FileDescriptor::close(client);
            return false;
        }

        static constexpr std::string_view kRequest = "GET /pid HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        if (::send(client, kRequest.data(), static_cast<int>(kRequest.size()), 0) != static_cast<int>(kRequest.size()))
        {
            Platform::FileDescriptor::close(client);
            return false;
        }

        std::string body;
        char        chunk[512];
        while (true)
        {
            const int readLength = ::recv(client, chunk, static_cast<int>(sizeof(chunk)), 0);
            if (readLength <= 0)
            {
                break;
            }
            body.append(chunk, static_cast<std::size_t>(readLength));
        }
        Platform::FileDescriptor::close(client);

        const std::size_t marker = body.find(R"("pid":)");
        if (marker == std::string::npos)
        {
            return false;
        }
        answeringPid = std::strtol(body.c_str() + marker + std::strlen(R"("pid":)"), nullptr, 10);
        return answeringPid > 0L;
    }

    /// 数一数这一轮的答话者里有多少就是目标进程
    int countAnswers(const std::vector<long> &answers, const long targetPid)
    {
        int count = 0;
        for (const long pid: answers)
        {
            count += pid == targetPid ? 1 : 0;
        }
        return count;
    }

    /**
     * @brief 探若干轮，把答话者记下来
     * @param port 目标端口
     * @param attempts 探测次数上限
     * @param[out] answers 答话者 pid 列表
     * @param isStoppingAtTargetPid true 时一旦看到目标 pid 答话就收手（等新一代接手时用）
     * @param targetPid 与 isStoppingAtTargetPid 搭配的目标进程号
     * @return int 这一轮里的失败次数
     */
    int probePhase(const std::uint16_t port, const int attempts, std::vector<long> &answers, const bool isStoppingAtTargetPid, const long targetPid)
    {
        int failures = 0;
        for (int attempt = 0; attempt < attempts; ++attempt)
        {
            long answeringPid = 0L;
            if (probeOnce(port, answeringPid))
            {
                answers.push_back(answeringPid);
                if (isStoppingAtTargetPid && answeringPid == targetPid)
                {
                    return failures;
                }
            } else
            {
                ++failures;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{kProbeIntervalMs});
        }
        return failures;
    }

    /**
     * @brief 新一代：连上交接通道、收下监听套接字，并在它上面起服务器
     * @param channelAddress 本代交出的通道地址（见 createChannelListener 的两种取值）
     * @return int 退出码：0 表示接手并服务过
     */
    int runTakeoverChild(const std::string &channelAddress)
    {
        const int channel = connectChannel(channelAddress);
        if (channel < 0)
        {
            Samples::printStartupError("新一代连不上交接通道");
            return 3;
        }

        const int adopted = Platform::Socket::readListeningSocketHandoff(channel);
        Platform::FileDescriptor::close(channel);
        if (adopted < 0)
        {
            Samples::printStartupError(std::format("新一代没能收下监听套接字，错误码 {}", Platform::PlatformError::lastErrorCode()));
            return 4;
        }

        Core::EventLoop loop;
        Net::HttpServer server(loop, adopted);
        registerRoutes(server.router());
        // 循环就在本线程上跑，投递用 schedule() 即可（同线程）
        Core::Task<void> listenTask = server.start();
        loop.scheduler().schedule(listenTask.handle());
        std::cout << "新一代 pid=" << Platform::ProcessInfo::currentProcessId() << " 已在接手来的监听套接字上服务" << std::endl;
        loop.run();
        return 0;
    }
} // namespace

int main(int argc, char **argv)
{
    // 探测端写向一条已被对端收掉的连接会拿到 SIGPIPE：默认动作是打死进程，那会让「一次失败」
    // 变成「整个示例没跑完」，结论行也就丢了
    std::signal(SIGPIPE, SIG_IGN);
    Samples::setupConsoleLogging();
    Samples::SampleChecklist &samples = Samples::checklist();

    if (!Platform::Socket::initialize())
    {
        Samples::printStartupError("套接字子系统初始化失败");
        return 1;
    }

    for (int index = 1; index + 1 < argc; ++index)
    {
        if (std::strcmp(argv[index], "--takeover") == 0)
        {
            return runTakeoverChild(Samples::readOptionValue(argc, argv, index, "--takeover", "交接通道地址"));
        }
    }

    // ---- 本代：先把两个套接字备好，趁进程还是单线程时派生新一代 -------------------
    std::uint16_t servicePort   = 0U;
    const int     serviceSocket = createListeningSocket(servicePort);
    if (serviceSocket < 0)
    {
        Samples::printStartupError("本代没能建立监听套接字");
        return 1;
    }

    std::string channelAddress;
    const int   channelListener = createChannelListener(channelAddress);
    if (channelListener < 0)
    {
        Samples::printStartupError("本代没能建立交接通道");
        return 1;
    }
    // unix 套接字是个文件：本代退出时把它带走，不让临时目录攒下空壳
    const ChannelPathGuard channelGuard(channelAddress);

    // 派生必须在起循环线程之前：spawn 在 POSIX 上是 fork + exec，多线程下 fork 出来的子进程
    // 只带着调用线程（见 Process::spawn 的警告）。新一代此时只会阻塞在通道读取上，等本代交棒。
    const Platform::Process::Handle childHandle = Platform::Process::spawn(Platform::Process::LaunchOptions{
            .executablePath = argv[0],
            .arguments      = {"--takeover", channelAddress},
    });
    if (!childHandle.isValid())
    {
        Samples::printStartupError("新一代没起来");
        return 1;
    }
    const long childPid = childHandle.processId();

    Core::EventLoop loop;
    Net::HttpServer server(loop, serviceSocket);
    registerRoutes(server.router());
    std::thread loopThread([&loop] { loop.run(); });
    // 循环已经在另一条线程上跑着：投递必须走 scheduleRemote（跨线程）
    Core::Task<void> listenTask = server.start();
    loop.scheduler().scheduleRemote(listenTask.handle());
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    const long parentPid = Platform::ProcessInfo::currentProcessId();

    std::vector<long> beforeHandoff;
    const int         beforeHandoffFailures = probePhase(servicePort, kPhaseProbeCount, beforeHandoff, false, 0L);
    const int         servedByParent        = countAnswers(beforeHandoff, parentPid);
    samples.check(servedByParent == kPhaseProbeCount && beforeHandoffFailures == 0,
                  std::format("交棒前 {} 次请求全部由本代答话（实际 {} 次，失败 {} 次）", kPhaseProbeCount, servedByParent, beforeHandoffFailures));

    const int channel = Platform::Socket::accept(channelListener, nullptr, nullptr);
    Platform::FileDescriptor::close(channelListener);
    if (channel < 0)
    {
        samples.check(false, "新一代没连上交接通道");
        loop.stop();
        loopThread.join();
        static_cast<void>(Platform::Process::forceTermination(childHandle));
        return Samples::finishSample("core_upgrade");
    }

    // 交出去。本代不关：SCM_RIGHTS 让两代各持同一个开放文件描述的一份引用，收口的顺序决定端口有空窗没空窗
    const bool isHandoffWritten = Platform::Socket::writeListeningSocketHandoff(channel, serviceSocket, static_cast<std::uint64_t>(childPid));
    Platform::FileDescriptor::close(channel);
    if (!isHandoffWritten)
    {
        samples.check(false, std::format("交出监听套接字失败，错误码 {}", Platform::PlatformError::lastErrorCode()));
        loop.stop();
        loopThread.join();
        static_cast<void>(Platform::Process::forceTermination(childHandle));
        return Samples::finishSample("core_upgrade");
    }

    // 等新一代答话——这一步成立之前本代绝不收口
    std::vector<long> afterHandoff;
    const int         handoffPhaseFailures     = probePhase(servicePort, kHandoffWaitAttempts, afterHandoff, true, childPid);
    const int         servedByChildBeforeDrain = countAnswers(afterHandoff, childPid);
    samples.check(servedByChildBeforeDrain >= 1 && handoffPhaseFailures == 0,
                  std::format("交棒后、本代收口前新一代答过话（答话 {} 次，本轮探测 {} 次，失败 {} 次）", servedByChildBeforeDrain, afterHandoff.size() + handoffPhaseFailures,
                              handoffPhaseFailures));

    // 本代收口：drain 是堆帧协程，Task 要活到它跑完
    {
        Core::Task<void> drainTask = server.drain(std::chrono::milliseconds{kDrainTimeoutMs});
        loop.scheduler().scheduleRemote(drainTask.handle());
        std::this_thread::sleep_for(std::chrono::milliseconds{kDrainSettleWaitMs});
    }

    // 老一代退出之后，端口应当仍由新一代服务
    std::vector<long> afterDrain;
    const int         afterDrainFailures      = probePhase(servicePort, kPhaseProbeCount, afterDrain, false, 0L);
    const int         servedByChildAfterDrain = countAnswers(afterDrain, childPid);
    samples.check(servedByChildAfterDrain == kPhaseProbeCount && afterDrainFailures == 0,
                  std::format("本代收口后 {} 次请求全部由新一代答话（实际 {} 次，失败 {} 次）", kPhaseProbeCount, servedByChildAfterDrain, afterDrainFailures));

    const int totalProbeCount = static_cast<int>(beforeHandoff.size()) + beforeHandoffFailures + static_cast<int>(afterHandoff.size()) + handoffPhaseFailures +
                                static_cast<int>(afterDrain.size()) + afterDrainFailures;
    const int totalFailures   = beforeHandoffFailures + handoffPhaseFailures + afterDrainFailures;
    samples.check(totalFailures == 0, std::format("全程 {} 次探测零失败：换代期间端口没有一次没人接（失败 {} 次）", totalProbeCount, totalFailures));

    loop.stop();
    loopThread.join();
    // 新一代此时仍在服务，端口挂在它身上：不收掉就成了占着端口的野进程
    samples.check(Platform::Process::forceTermination(childHandle), "收口时向新一代发出了终止");
    // 句柄一释放就没人回收那个 pid，刚强杀过必须先等它结束（POSIX 上否则留下僵尸）
    const bool isChildGone = Samples::waitUntil([&childHandle] { return !Platform::Process::isRunning(childHandle); }, std::chrono::milliseconds{kChildExitWaitMs});
    samples.check(isChildGone, "新一代已随本示例收口，没留下占着端口的进程");

    LOG_INFO_FMT("换代结论：本代答话 {} 次；收口前新一代答话 {} 次；收口后新一代答话 {} 次；服务端口 {}", servedByParent, servedByChildBeforeDrain, servedByChildAfterDrain,
                 servicePort);

    return Samples::finishSample("core_upgrade");
}
