// Core 多进程示例：WorkerSupervisor 的补位、崩溃上限与停止路径，以及各平台的构造期拒因
#include "Base/Exception/Exception.h"
#include "Base/Log/LogMacros.h"
#include "Core/Process/WorkerSupervisor.h"
#include "Platform/Platform.h"
#include "Platform/System/ProcessInfo.h"
#include "common/SampleSupport.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

#if !ASYN_PLATFORM_WIN32
#include <csignal>
#include <unistd.h>
#endif

using namespace AsynGyanis;

namespace
{
#if !ASYN_PLATFORM_WIN32
    /// 待停的编排器：信号处理函数拿不到实参，只能经这份文件作用域指针转达（与真实部署里 Ctrl+C 的路径同一条）
    Core::WorkerSupervisor *g_supervisorToStop{nullptr};
#endif

    /**
     * @brief 子进程模式：写一份「我活着」的证据文件，活够时长再体面退出
     * @param evidenceDirectory 证据文件目录（master 给的）
     * @param lifetime 存活时长；到期正常退出，让 master 走「稳定运行后退出 → 补一个新的」那条路
     * @return int 证据写成功且活满时长返回 0，写不进去返回非零
     */
    int runHoldChild(const std::string &evidenceDirectory, const std::chrono::milliseconds lifetime)
    {
        std::error_code createError;
        std::filesystem::create_directories(evidenceDirectory, createError);

        const std::filesystem::path evidenceFile =
                std::filesystem::path(evidenceDirectory) / ("worker-" + std::to_string(Platform::ProcessInfo::currentProcessId()) + ".pid");
        std::ofstream stream(evidenceFile);
        stream << Platform::ProcessInfo::currentProcessId() << '\n';
        stream.close();
        if (!stream)
        {
            return 4;
        }

        std::this_thread::sleep_for(lifetime);
        return 0;
    }

    /**
     * @brief 数一数证据目录里出现过多少个不同的 worker 进程号
     * @param evidenceDirectory 证据文件目录
     * @return std::size_t 证据文件个数（文件名含进程号，故个数即「起过的 worker 次数」）
     */
    std::size_t countSpawnedWorkers(const std::string &evidenceDirectory)
    {
        std::error_code listError;
        std::size_t     count{0};
        for (std::filesystem::directory_iterator iterator(evidenceDirectory, listError), end{}; iterator != end; ++iterator)
        {
            ++count;
        }
        return count;
    }

    /**
     * @brief 一段编排的观测结果
     */
    struct SupervisionOutcome
    {
        std::chrono::milliseconds elapsed; ///< 从进入 run() 到它返回用了多久（挂死的段不会回到这里，由外层超时判负）
        std::size_t              spawned;  ///< 证据目录里出现过的 worker 次数
        std::size_t              runningAtExit; ///< run() 返回后编排视图里还在的 worker 数（此时循环已结束，读取无并发）
        bool                     isPoolGivenUp{false}; ///< 这一段是不是以「整池都起来就崩」收场的
    };

    /**
     * @brief 跑一段编排：起 worker、按时刻表收手，并回报这段时间里的观测
     * @param configuration 编排参数（executablePath 与 workerArguments 由调用方给）
     * @param holdFor 编排运行多久后请求停止；0 表示不发停止请求（用于「全部放弃后自己返回」那条路径）
     * @param evidenceDirectory 证据目录，用于统计 worker 起过几次；空表示不统计
     * @return SupervisionOutcome 观测结果
     */
    SupervisionOutcome superviseFor(Core::WorkerSupervisor::Configuration configuration, const std::chrono::milliseconds holdFor,
                                     const std::string &evidenceDirectory)
    {
        Core::WorkerSupervisor supervisor(std::move(configuration));

#if ASYN_PLATFORM_WIN32
        // Windows 的构造函数直接抛，走不到这里
        static_cast<void>(holdFor);
        static_cast<void>(evidenceDirectory);
        return {{}, 0, 0};
#else
        // 停止请求走信号：run() 要求进程此刻只有调用线程（fork 的固有限制），因此不能另起线程去轮询，
        // 只能把「过一会儿停下来」交给定时器信号——这也正是 master 收到 SIGTERM 时的同一条路径
        g_supervisorToStop = &supervisor;
        void (*previousHandler)(int) = std::signal(SIGALRM, [](int)
                                                   {
                                                       if (g_supervisorToStop != nullptr)
                                                       {
                                                           g_supervisorToStop->requestStop();
                                                       }
                                                   });
        if (holdFor > std::chrono::milliseconds::zero())
        {
            // alarm() 只有秒粒度，向上取整保证「至少跑这么久」
            ::alarm(static_cast<unsigned>((holdFor + std::chrono::seconds{1} - std::chrono::milliseconds{1}) / std::chrono::seconds{1}));
        }

        const auto startedAt = std::chrono::steady_clock::now();
        // run() 报的是「这次是不是按请求收的口」，本结构记的是相反的那面（整池是否被放弃）：
        // 两段各自断言一种收场，取反要在这里做一次，别留给读者猜
        const bool isPoolGivenUp = !supervisor.run();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);

        const std::size_t spawnedWorkerCount = evidenceDirectory.empty() ? 0 : countSpawnedWorkers(evidenceDirectory);
        const std::size_t runningAtExit      = supervisor.runningWorkerCount();
        ::alarm(0);
        std::signal(SIGALRM, previousHandler);
        g_supervisorToStop = nullptr;

        return {elapsed, spawnedWorkerCount, runningAtExit, isPoolGivenUp};
#endif
    }

    /**
     * @brief 断言一次构造会抛出，并回报异常文本
     * @param label 用于日志的说明
     * @param configuration 待校验的编排参数
     * @param expectedFragment 异常文本里应出现的关键片段
     * @return bool 抛出了且文本含关键片段
     */
    bool constructionRejects(const std::string &label, const Core::WorkerSupervisor::Configuration &configuration,
                             const std::string_view expectedFragment)
    {
        try
        {
            Core::WorkerSupervisor supervisor(configuration);
            static_cast<void>(supervisor);
        } catch (const Base::Exception &failure)
        {
            const bool isExpected = std::string_view{failure.what()}.find(expectedFragment) != std::string_view::npos;
            LOG_INFO_FMT("{} 被构造期拒掉：{}{}", label, failure.what(), isExpected ? "" : "（与预期关键片段不符）");
            return isExpected;
        } catch (...)
        {
            LOG_ERROR_FMT("{} 抛出了非框架异常", label);
            return false;
        }
        LOG_ERROR_FMT("{} 竟然构造成功了（本平台/本参数下应当被拒）", label);
        return false;
    }
} // namespace

int main(const int argc, char **argv)
{
    using namespace std::chrono_literals;

    // 子进程模式：不装日志、不做检查，只留证据或立刻退出，给编排循环当被管的对象。
    // 判据必须逐个参数看：`--child-crash` 后面没有取值，用「index + 1 < argc」当循环条件就永远
    // 扫不到它，于是子进程会掉回父进程那条路——再去起自己的 worker，一层层往下 fork
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view mode(argv[index]);
        if (mode == "--child-hold" && index + 1 < argc)
        {
            return runHoldChild(argv[index + 1], 400ms);
        }
        if (mode == "--child-crash")
        {
            return 3;
        }
    }

    Samples::setupConsoleLogging();
    LOG_INFO("=== Core 多进程示例开始 ===");

    auto &samples = Samples::checklist();

    // 取本示例映像的绝对路径：worker 由同一份映像起，相对形式在 CreateProcess 下认不稳
    std::error_code selfPathError;
    const auto      selfPath = std::filesystem::weakly_canonical(std::filesystem::absolute(argv[0]), selfPathError);
    if (selfPathError || !std::filesystem::exists(selfPath))
    {
        LOG_ERROR_FMT("拿不到本示例的可执行文件绝对路径，编排器没法起 worker：{}", argv[0]);
        return 1;
    }
    const std::string executablePath = selfPath.string();

    // 配置校验在两个平台上都成立，先把它钉住：空路径、worker 数不足都该在构造期就说清
    samples.check(constructionRejects("可执行文件路径为空", Core::WorkerSupervisor::Configuration{.executablePath = "", .workerCount = 2},
                                      "可执行文件路径为空"),
                  "路径为空在构造期就被拒，不留到运行期");
    samples.check(constructionRejects("worker 数小于 2", Core::WorkerSupervisor::Configuration{.executablePath = executablePath, .workerCount = 1},
                                      "至少为 2"),
                  "worker 数小于 2 在构造期就被拒（单进程不需要编排器）");
    samples.check(constructionRejects("轮询间隔为 0",
                                      Core::WorkerSupervisor::Configuration{
                                              .executablePath = executablePath, .workerCount = 2, .pollInterval = std::chrono::milliseconds{0},
                                      },
                                      "轮询间隔"),
                  "轮询间隔为 0 在构造期就被拒，否则循环会空转");

#if ASYN_PLATFORM_WIN32
    // Windows 上没有 SO_REUSEPORT：多进程共享端口无从谈起，构造当场拒绝而不是留下「只有一个能绑上」的假成功
    samples.check(constructionRejects("Windows 上的多进程编排",
                                      Core::WorkerSupervisor::Configuration{.executablePath = executablePath, .workerCount = 2},
                                      "Windows 不支持多进程 worker 模型"),
                  "Windows 构造多进程编排在当场被拒，改指 workers=1 或 Linux 部署");
    return Samples::finishSample("core_worker");
#else
    // —— 以下只在 POSIX 上跑：worker 真的被起起来、真的被补位、真的按时刻表收手 ——
    const std::string evidenceDirectory = (std::filesystem::temp_directory_path() /
                                           ("asyn-sample-core_worker-" + std::to_string(Platform::ProcessInfo::currentProcessId()))).string();
    std::error_code cleanError;
    std::filesystem::remove_all(evidenceDirectory, cleanError);

    // 第一段：两个槽位、每个 worker 只活 400 毫秒（长过崩溃窗口）——稳态退出要补新的，于是证据文件会多于 2 份
    const SupervisionOutcome holdOutcome = superviseFor(
            Core::WorkerSupervisor::Configuration{
                    .executablePath  = executablePath,
                    .workerArguments = {"--child-hold", evidenceDirectory},
                    .workerCount     = 2,
                    .pollInterval    = 20ms,
                    .restartBackoff  = 20ms,
                    .shutdownTimeout = 2000ms,
                    .crashLoopWindow = 100ms,
                    .crashLoopLimit  = 2U,
            },
            1500ms, evidenceDirectory);
    LOG_INFO_FMT("第一段：{} 毫秒内返回，起过 {} 个 worker", holdOutcome.elapsed.count(), holdOutcome.spawned);

    samples.check(holdOutcome.elapsed < 6s, "收到停止请求后编排循环按时返回（worker 各自收手）");
    samples.check(holdOutcome.spawned > 2, "存活到期的 worker 被补上了新的（证据文件多于 worker 个数）");
    samples.check(holdOutcome.spawned <= 40, "补位节奏受轮询与退避约束，没有变成满速重启");
    samples.check(holdOutcome.runningAtExit == 0, "编排返回时视图里已没有在跑的 worker");
    samples.check(!holdOutcome.isPoolGivenUp, "第一段是按请求收口，不该报成「整池都起来就崩」");

    // 第二段：worker 一起来就退（短于崩溃窗口），连续到上限后编排自己收手——这一段没有任何停止请求
    const SupervisionOutcome crashOutcome = superviseFor(
            Core::WorkerSupervisor::Configuration{
                    .executablePath  = executablePath,
                    .workerArguments = {"--child-crash"},
                    .workerCount     = 2,
                    .pollInterval    = 20ms,
                    .restartBackoff  = 20ms,
                    .shutdownTimeout = 1000ms,
                    .crashLoopWindow = 3000ms,
                    .crashLoopLimit  = 2U,
            },
            std::chrono::milliseconds::zero(), std::string{});
    LOG_INFO_FMT("第二段：{} 毫秒内自行返回", crashOutcome.elapsed.count());

    samples.check(crashOutcome.elapsed < 10s, "「起来就崩」到上限后编排自行返回，没有卡在补位循环里");
    samples.check(crashOutcome.spawned == 0, "立刻退出的 worker 没写出一份存活证据");
    samples.check(crashOutcome.runningAtExit == 0, "自行返回时同样没有留下在跑的 worker");
    samples.check(crashOutcome.isPoolGivenUp, "第二段没有任何停止请求，返回只能是因为整池都被放弃");

    std::filesystem::remove_all(evidenceDirectory, cleanError);

    return Samples::finishSample("core_worker");
#endif
}
