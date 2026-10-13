// worker 侧心跳的跨进程夹具：认编排器追加的 --watchdog-pipe-fd <n> 与 --watchdog-ping-ms <n>，
// 起两条循环的池、把 WorkerHeartbeat 挂上，然后活着直到被终止。它不做任何判定——判据在
// tests/Core/Process/TestWorkerSupervisor.cpp 那一侧：假服务管理器收到 WATCHDOG=1 = 本进程的拍
// 真的经「参数传递 + fork 继承」到了 master。
//
// 为什么要有这份工具：master 的心跳汇聚要求「另一个进程真的在拍」，同进程内自造一条管道测不到
// 「编排器建管道 → 继承给子进程 → 子进程起拍」这条完整路径。
//
// 输出协议：READY（心跳已挂上）后不再吐行
// 退出码：
//   0  正常收手（本工具在被终止前不会自己退）
//   2  命令行缺参数，或本平台没有该通道
//   3  心跳 arm 失败（原因写进 stdout）
#include "Core/Coroutine/ThreadPool.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Process/WorkerHeartbeat.h"
#include "Core/Process/WorkerSupervisor.h"
#include "Platform/Platform.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{
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
     * @brief 从参数表里取某个选项的值
     * @param arguments 命令行参数（含 argv[0]）
     * @param option 选项名（用编排器的常量，避免两处字符串漂移）
     * @return std::string_view 值；没给时为空
     */
    [[nodiscard]] std::string_view findOptionValue(const std::vector<std::string> &arguments, const std::string_view option)
    {
        for (std::size_t index = 0; index + 1U < arguments.size(); ++index)
        {
            if (arguments[index] == option)
            {
                return std::string_view(arguments[index + 1U]);
            }
        }
        return {};
    }
} // namespace

int main(const int argc, char **argv)
{
#if ASYN_PLATFORM_WIN32
    static_cast<void>(argc);
    static_cast<void>(argv);
    std::printf("本平台没有 worker 心跳通道（systemd 看门狗专属）\n");
    return 2;
#else
    const std::vector<std::string> arguments(argv, argv + argc);
    const std::string_view         pipeFdText = findOptionValue(arguments, AsynGyanis::Core::kWatchdogPipeArgument);
    const std::string_view         pingText   = findOptionValue(arguments, AsynGyanis::Core::kWatchdogPingArgument);
    if (pipeFdText.empty() || pingText.empty())
    {
        std::printf("缺 %s 或 %s：本夹具只被编排器起，按约定就该有这两个参数\n", std::string(AsynGyanis::Core::kWatchdogPipeArgument).c_str(),
                    std::string(AsynGyanis::Core::kWatchdogPingArgument).c_str());
        return 2;
    }

    const int                    pipeFd       = std::atoi(std::string(pipeFdText).c_str());
    const auto                   pingInterval = std::chrono::milliseconds(std::atoll(std::string(pingText).c_str()));
    AsynGyanis::Core::ThreadPool pool{2};
    pool.start();
    for (std::size_t index = 0; index < 2; ++index)
    {
        // 循环起跑没有回调可等，这里按「isRunning 为真再挂」做一次有界轮询：arm 走的 scheduleRemote
        // 要求循环已经在转
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!pool.eventLoop(index).isRunning() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    AsynGyanis::Core::WorkerHeartbeat heartbeat(pool, pipeFd, pingInterval);
    if (!heartbeat.arm())
    {
        std::printf("心跳没有挂上：%s\n", heartbeat.lastError().c_str());
        return 3;
    }
    report("READY");

    // 一直活着直到被终止（默认 SIGTERM 动作直接收掉本进程，收尾路径不用跑）
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#endif
}
