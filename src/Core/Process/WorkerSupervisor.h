/**
 * @file WorkerSupervisor.h
 * @brief 多进程 worker 的编排：起若干 worker 进程、盯住它们的退出、按需重启、收尾时送走
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/System/Process.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    /**
     * @brief 多进程 worker 的编排器（master 侧）：起进程、盯退出、按需重启、收尾送走；worker 参数全部由调用方给
     * @warning run() 必须在**进程还是单线程**时调用：worker 由 fork + exec 起（见 Platform::Process），
     *          多线程下 fork 出的子进程只带调用线程，锁与运行库状态都可能不自洽。
     * @warning Windows 上多进程共享端口没有等价物（缺 SO_REUSEPORT），因此 workerCount > 1 在构造时
     *          直接拒绝，而不是跑起来才发现只有一个进程能绑上端口。
     * @note 进程之间**不共享任何状态**：按来源 IP 的并发限额、限流桶、指标计数都是各进程一份。
     *       多进程下这意味着「单来源上限 × N、请求速率上限 × N、指标要按进程分别采集」——
     *       要真正的全局口径就得引入进程间共享（或改用单进程多工作循环 + 接受分发）。
     */
    class WorkerSupervisor
    {
    public:
        /**
         * @brief 编排参数
         */
        struct Configuration
        {
            std::string              executablePath;                ///< 要起的可执行文件（通常就是本进程自己的映像）
            std::vector<std::string> workerArguments;               ///< 每个 worker 的固定参数（不含 argv[0]）
            std::size_t              workerCount{1};                ///< worker 个数；必须大于 1，等于 1 时直接用单进程跑，不需要本类
            std::chrono::milliseconds pollInterval{100};             ///< 观察存活与响应停止请求的轮询间隔
            std::chrono::milliseconds restartBackoff{500};           ///< 补 worker 前的等待：避免崩溃循环里打转
            std::chrono::milliseconds shutdownTimeout{10000};        ///< 收尾期限：请求退出后等到这个点就强杀
            std::chrono::milliseconds crashLoopWindow{3000};         ///< 存活不足这个时长就退出，算一次「起来就崩」
            std::size_t              crashLoopLimit{5};              ///< 连续「起来就崩」达到这个次数就停止补该 worker
        };

        /**
         * @brief 校验配置并构造
         * @param configuration 编排参数
         * @throws Base::LogicException 配置不成立（可执行文件为空、workerCount 小于 2）或本平台不支持
         *         （Windows 上没有 SO_REUSEPORT，多进程无法共享端口；提示改用 workers=1）
         */
        explicit WorkerSupervisor(Configuration configuration);

        /**
         * @brief 析构：把仍在跟踪的 worker 强杀掉
         * @details run() 之外抛出异常时走这条路，避免留下孤儿进程继续占着端口。正常返回的 run()
         *          已经把 worker 都送走了，这里无事可做。
         */
        ~WorkerSupervisor();

        WorkerSupervisor(const WorkerSupervisor &) = delete;

        WorkerSupervisor &operator=(const WorkerSupervisor &) = delete;

        /**
         * @brief 起 worker 并进入编排循环（阻塞）
         * @details 循环里做三件事：把该在的 worker 补齐、收掉已退出的并决定是否补、检查停止请求。
         *          停止请求到达后先对全部 worker 发 SIGTERM，等到 shutdownTimeout 仍未退出的强杀，
         *          然后返回。
         * @return bool true 表示是按请求收口；false 表示全部 worker 都因「起来就崩」被放弃而提前退出
         * @note 返回值就是「这次编排算不算成了」：调用方要据此决定退出码，否则进程管理器与脚本
         *       看到的是「服务退出码 0」，分不清是被停掉的还是整池子都起不来
         */
        [[nodiscard]] bool run();

        /**
         * @brief 请求停止编排（可从信号处理函数调用）
         * @details 只置一个原子标记，不做任何分配、不做系统调用，因此满足异步信号安全：
         *          SIGTERM/SIGINT 的处理函数可以直接调它，退出流程全部留给 run() 的循环。
         */
        void requestStop() noexcept;

        /**
         * @brief 当前仍在运行的 worker 数（日志与诊断用）
         * @return std::size_t 个数
         */
        [[nodiscard]] std::size_t runningWorkerCount() const noexcept;

    private:
        /**
         * @brief 一个被跟踪的 worker
         */
        struct Worker
        {
            Platform::Process::Handle            handle;      ///< 进程句柄
            std::chrono::steady_clock::time_point startTime;  ///< 启动时刻：判「起来就崩」用
            std::size_t                          crashCount{0};///< 该位连续「起来就崩」的次数
            bool                                 isGivenUp{false}; ///< 是否已放弃补它（连续崩太多次）
        };

        /**
         * @brief 在指定槽位上起一个 worker，失败时按一次「起来就崩」记数
         * @param worker 目标槽位
         * @param workerIndex 槽位序号，仅用于日志
         * @return true 起来了
         */
        [[nodiscard]] bool startWorker(Worker &worker, std::size_t workerIndex);

        /**
         * @brief 收掉一个已退出的 worker，并决定这个槽位接下来怎么办
         * @param worker 目标槽位
         * @param workerIndex 槽位序号，仅用于日志
         * @return true 该槽位已放弃（连续崩太多次）
         */
        [[nodiscard]] bool reapWorker(Worker &worker, std::size_t workerIndex);

        /**
         * @brief 送走全部 worker：先请求体面退出，超期强杀
         */
        void stopAllWorkers();

        /**
         * @brief 强杀之后有界等到子进程真的被收尸，再交还句柄
         * @details 收尾路径与析构兜底共用这一处：丢掉 pid 就等于留下没人收的僵尸
         */
        void waitForForcedTerminationsToLand();

        Configuration             m_configuration;   ///< 编排参数（构造时已校验）
        std::vector<Worker>       m_workers;         ///< worker 槽位；下标即序号，槽位固定不搬

        /// 停止请求：只置一个无锁原子，因此信号处理函数里调用 requestStop() 是安全的
        /// （.cpp 里对 is_always_lock_free 做了断言，平台不满足会在编译期就拦住）
        std::atomic<bool> m_isStopRequested{false};
    };
} // namespace AsynGyanis::Core
