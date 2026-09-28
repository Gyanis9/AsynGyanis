// 监听套接字跨进程移交的 worker 侧夹具：认编排器追加的 --handed-over-listener <地址>，取回移交来的
// 监听引用，接受连接并回一行文本。它不做任何判定——判据在 tests/Core/Process/TestWorkerSupervisor.cpp
// 那一侧，由「连得上、拿得到回话、回话来自一个接手过移交的进程」三件事给出。
//
// 为什么要有这份工具：本进程自己试不出「另一个进程接手监听套接字」这条路径——平台的 write/read
// 一对函数能在同一进程内换一份引用（既有的交接用例就是这么跑的），而 WSADuplicateSocketW 按
// **目标进程号**发凭证，跨进程那条路只有真的另起一个进程才走得到。
//
// 输出协议（每行一条，立即 flush）：
//   READY                  已取回监听套接字并开始接受连接
//   ACCEPTED <序号>         第 n 条连接已回过话
// 退出码：
//   0  收到终止（accept 报错）后正常收手
//   2  命令行缺 --handed-over-listener
//   3  收下移交来的监听套接字失败（原因写进 stdout）
//   4  监听套接字上 accept 不出来（原因写进 stdout）
#include "Core/Process/UpgradeChannel.h"
#include "Core/Process/WorkerSupervisor.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/Platform.h"
#include "Platform/System/PlatformError.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    /// 取回监听引用的重试预算：编排器是先派生再等对方连上通道，这边慢一点不算问题
    constexpr std::chrono::milliseconds kAdoptBudget{15000};

    /// 回话文本：用例只判「拿到的是这一句」，内容本身没有业务含义
    constexpr std::string_view kAnswer = "handoff-ok\n";

    /**
     * @brief 打一行事件并立即 flush（父进程靠这些行判断 worker 到底走到哪一步）
     * @param text 行内容（不含换行）
     */
    void report(const std::string_view text)
    {
        std::printf("%s\n", std::string(text).c_str());
        std::fflush(stdout);
    }

    /**
     * @brief 从参数表里取 --handed-over-listener 的值
     * @param arguments 命令行参数（含 argv[0]）
     * @return std::string_view 通道地址；没给时为空
     */
    [[nodiscard]] std::string_view findChannelAddress(const std::vector<std::string> &arguments)
    {
        for (std::size_t index = 0; index + 1U < arguments.size(); ++index)
        {
            if (arguments[index] == AsynGyanis::Core::kHandedOverListenerArgument)
            {
                return std::string_view(arguments[index + 1U]);
            }
        }
        return {};
    }
} // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index)
    {
        arguments.emplace_back(argv[index]);
    }

    const std::string_view channelAddress = findChannelAddress(arguments);
    if (channelAddress.empty())
    {
        report("缺少 --handed-over-listener，没有交接通道可连");
        return 2;
    }

    const auto adopted = AsynGyanis::Core::adoptHandedOverListener(channelAddress, kAdoptBudget);
    if (!adopted)
    {
        report("收下监听套接字失败：" + adopted.error());
        return 3;
    }
    const int listener = *adopted;
    report("READY");

    // 一条一条地接：编排器要的是「这个进程能替这个端口接活」，接完一条继续等下一条，
    // 直到父进程把它收掉（那时 accept 报错，本程序按正常收手退出）
    unsigned acceptedCount = 0U;
    while (true)
    {
        const int client = static_cast<int>(::accept(listener, nullptr, nullptr));
        if (client < 0)
        {
            report("accept 失败，错误码 " + std::to_string(AsynGyanis::Platform::PlatformError::lastSocketErrorCode()));
            return 4;
        }
        static_cast<void>(::send(client, kAnswer.data(), static_cast<int>(kAnswer.size()), 0));
        // 关掉会话用平台的出口：Windows 上套接字要走 closesocket，CRT 的 close 只管文件描述符
        static_cast<void>(AsynGyanis::Platform::FileDescriptor::close(client));
        ++acceptedCount;
        report("ACCEPTED " + std::to_string(acceptedCount));
    }
}
