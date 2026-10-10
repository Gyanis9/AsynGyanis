/**
 * @file WorkerSupervisor.h
 * @brief 多进程 worker 的编排：起若干 worker 进程、盯住它们的退出、按需重启、收尾时送走
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Metrics/ProcessMetricsRegistry.h"
#include "Platform/System/Process.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Core
{
    /**
     * @brief 编排器交给 worker 的那个参数名（移交模式下自动追加，值就是交接通道地址）
     * @details worker 侧要认它：拿到地址后用 Core::adoptHandedOverListener(值, 预算) 取回一份
     *          已在监听的套接字，再按各服务器「接管已监听描述符」的构造入口起服务。
     *          调用方把自己那份 listen() 之后就没本类的事了——本层只负责送，不负责收。
     */
    inline constexpr std::string_view kHandedOverListenerArgument = "--handed-over-listener";

    /**
     * @brief 多进程 worker 的编排器（master 侧）：起进程、盯退出、按需重启、收尾送走；worker 参数全部由调用方给
     * @warning run() 必须在**进程还是单线程**时调用：worker 由 fork + exec 起（见 Platform::Process），
     *          多线程下 fork 出的子进程只带调用线程，锁与运行库状态都可能不自洽。
     * @note 两个平台的分摊形状不同，配置因此分叉：
     *        @li POSIX 给 @c handoff 留空——每个 worker 自己 bind 同一个端口，靠 SO_REUSEPORT 由内核
     *            分摊连接；
     *        @li Windows 必须给 @c handoff——那边没有 SO_REUSEPORT 的等价物，多个进程各自 bind 同一
     *            端口时内核只会把全部连接交给最后绑上的那一个（其余的一个错都不报，永远收不到连接），
     *            所以改由 master bind 一次、把这份监听套接字逐个复制给 worker。
     * @note 移交模式下每次（重）起 worker 都开一条一次性通道：worker 崩了要重新移交，而通道交完
     *       一个对端就作废（留着只会有第三个进程挤进一次已经谈定的交接）。
     * @warning 移交模式下的补位是有界阻塞：起一个 worker 最多占用 @c handoff.waitBudget 等它连上
     *          通道，期限内没连上就按一次「起来就崩」记数并杀掉那个进程。整池都起不来时编排照常
     *          放弃并返回 false，不会卡在通道上。
     * @note 进程之间**不共享任何状态**：按来源 IP 的并发限额、限流桶、指标计数都是各进程一份。
     *       多进程下这意味着「单来源上限 × N、请求速率上限 × N、指标要按进程分别采集」——
     *       要真正的全局口径就得引入进程间共享（或改用单进程多工作循环 + 接受分发）。
     */
    class ASYN_CORE_API WorkerSupervisor
    {
    public:
        /**
         * @brief Windows 的移交配置：master 已经把监听套接字攥在手里，本类负责逐个复制给 worker
         * @details 存在的理由是「Windows 上多个进程绑同一端口不分摊」这一条平台事实：不换形状就没法
         *          横向扩进程，而换了形状就要有人把监听引用送过去。
         * @note 本结构只在 Windows 上有效：POSIX 上填了它会被构造函数拒掉——那边的 worker 各自 bind，
         *       这份描述符交出去没有使用方，留着一个「填了却不生效」的档位比拒绝更坏。
         */
        struct Handoff
        {
            int                       listeningDescriptor{-1}; ///< master 已 bind + listen 的套接字描述符；所有权仍在调用方，本类不在它上面收连接
            std::chrono::milliseconds waitBudget{5000};        ///< 等一个 worker 连上交接通道的预算；期限内没连上就按一次「起来就崩」处理
        };

        /**
         * @brief 编排参数
         */
        struct Configuration
        {
            std::string               executablePath;         ///< 要起的可执行文件（通常就是本进程自己的映像）
            std::vector<std::string>  workerArguments;        ///< 每个 worker 的固定参数（不含 argv[0]）
            std::size_t               workerCount{1};         ///< worker 个数；必须大于 1，等于 1 时直接用单进程跑，不需要本类
            std::chrono::milliseconds pollInterval{100};      ///< 观察存活与响应停止请求的轮询间隔
            std::chrono::milliseconds restartBackoff{500};    ///< 补 worker 前的等待：避免崩溃循环里打转
            std::chrono::milliseconds shutdownTimeout{10000}; ///< 收尾期限：请求退出后等到这个点就强杀
            std::chrono::milliseconds crashLoopWindow{3000};  ///< 存活不足这个时长就退出，算一次「起来就崩」；必须大于 0
            std::size_t               crashLoopLimit{5};      ///< 连续「起来就崩」达到这个次数就停止补该 worker；至少为 1
            /// 移交配置（Windows 必填、POSIX 必须留空，见类注释与 Handoff 的 @note）：
            /// 有值即启用「master 送监听套接字」那套编排，本类会往 worker 参数尾部追加
            /// kHandedOverListenerArgument 与那条一次性通道的地址
            std::optional<Handoff> handoff;
            /**
             * @brief 给每个 worker 传槽位序号用的参数名；空 = 不传
             * @details 序号是「本进程在池子里排第几」，master 补起一个崩掉的 worker 时沿用同一个槽位号，
             *          因此它每次拿到的号也不变。调用方需要按进程错开什么东西（比如每进程一个指标抓取端口）
             *          才用得着它；本类只负责把 `<参数名> <十进制序号>` 追加到该 worker 的参数尾部，
             *          不认识这个约定的子进程会照常忽略它。
             * @note 名字由调用方定：参数约定属于应用，不属于编排层
             */
            std::string workerIndexArgument{};

            /**
             * @brief 每个槽位都各有一个活着的 worker 时回调一次；空 = 不回调
             * @details 「活着」按进程号判：`Platform::Process::isRunning` 说的是这个子进程还没退出并已被
             *          回收，**不**说明它已经绑上监听端口——那一段还要等它自己起完循环。所以这一格能给的是
             *          「整池都到位了」，端口上真有人应答要由调用方自己再验一次（`ReferenceServer` 就是
             *          先等这条回调、再对本机端口做一次 TCP 连接试探，然后才向服务管理器报就绪）。
             * @note 回调跑在**编排线程**上，不得长时间阻塞：那条线程同时负责补崩掉的 worker 与响应停止请求。
             *       每轮重数一遍个数，凑齐那一刻只回调一次；worker 后来崩了不再补起也不会二次回调——
             *       「就绪」是一次性陈述，不是持续状态。
             */
            std::function<void()> onAllWorkersRunning{};

            /**
             * @brief 编排开始收口时回调一次（停止请求到达或整池被放弃）；空 = 不回调
             * @details 给「要告诉监督者我在收尾」那一类调用方用：这一刻 worker 还没被通知退出，
             *          整池仍在跑，正是一次 `STOPPING=1` 该有的位置。放在 `stopAllWorkers()` 之前而不是
             *          `run()` 返回之后——后者已经收尾完了，再报就成了事后说明。
             * @note 同样跑在编排线程上；由信号触发的停止里，本回调是在**循环线程**而不是信号处理函数里跑的
             *       （处理函数只置原子标记，见 `requestStop()`）。
             */
            std::function<void()> onStopRequested{};
        };

        /**
         * @brief 校验配置并构造
         * @param configuration 编排参数
         * @throws Base::LogicException 配置不成立（可执行文件为空、workerCount 小于 2、崩溃判据
         *         非正），或本平台的移交配置给得不对：Windows 缺 @c handoff（各自 bind 同一端口
         *         不分摊，跑起来只有一个进程收得到连接），POSIX 给了 @c handoff（那边每个 worker
         *         自己 bind，这份描述符没有使用方）
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
         *          停止请求到达后先请求每个 worker 体面退出（POSIX 发 SIGTERM，Windows 向它名下的进程组
         *          发 CTRL_BREAK），等到 shutdownTimeout 仍未退出的强杀，然后返回。
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
         * @brief 当前仍在运行的 worker 数（日志与诊断用），任何线程可调
         * @return std::size_t 个数；这是编排线程**最近一轮扫描**的快照，最长滞后一个 pollInterval
         * @note 本接口不去探句柄：观察线程若直接调存活查询，会与编排线程同时读写同一个进程句柄
         *       （存活查询顺手回收子进程并把退出码与进程号写回句柄）。回收本身不能并发——两个线程
         *       只有一个拿得到退出状态；更要紧的是终止路径读到的进程号可能已被并发的回收改写成
         *       「作废」哨兵，那是一次面向全系统的 kill
         */
        [[nodiscard]] std::size_t runningWorkerCount() const noexcept;

        /**
         * @brief 判断「整池都到位」这一格（纯换算，不读状态）
         * @param runningWorkerCount 本轮数出来的在跑个数
         * @param slotCount 槽位总数
         * @return true 每个槽位此刻都有一个活着的 worker
         * @return false 还缺人，或槽位总数为 0（空池没有「都到位」这回事）
         * @details 单列成纯函数是因为真造一个「永远补不起来」的池子并不确定：`posix_spawn` 对不存在的
         *          可执行文件可能先报成功、子进程随后以 127 退出，那一轮「整池看起来齐了」确实发生过
         *          （CI 上就这么红过一次）。把条件本身钉成可确定复跑的判据，端到端那几条只判自己能稳的形状。
         */
        [[nodiscard]] static bool isPoolComplete(std::size_t runningWorkerCount, std::size_t slotCount) noexcept;

    private:
        /**
         * @brief 一个被跟踪的 worker
         */
        struct Worker
        {
            Platform::Process::Handle             handle;           ///< 进程句柄
            std::chrono::steady_clock::time_point startTime;        ///< 启动时刻：判「起来就崩」用
            std::size_t                           crashCount{0};    ///< 该位连续「起来就崩」的次数
            bool                                  isGivenUp{false}; ///< 是否已放弃补它（连续崩太多次）
        };

        /**
         * @brief 在指定槽位上起一个 worker，失败时按一次「起来就崩」记数
         * @details 移交模式下多三步：开一条一次性通道、把它的地址追加进 worker 参数、等对方连上后
         *          按对方进程号交出监听套接字。这三步里任何一步不成都算该槽位崩一次——起来却拿不到
         *          监听引用的进程不会服务任何连接，留着它只是占一个名额。
         * @param worker 目标槽位
         * @param workerIndex 槽位序号，仅用于日志
         * @return true 起来了（移交模式下还包含「监听套接字已经交到手」）
         */
        [[nodiscard]] bool startWorker(Worker &worker, std::size_t workerIndex);

        /**
         * @brief 给某个槽位记一次「没起来」，连续到上限就放弃补它
         * @details 起不来、开不出通道、等不到对方连上、交付失败四类出口都走这一处：它们的后果相同——
         *          这个槽位上现在没有一个能服务连接的进程。记数与日志只留一份，免得四条出口各自
         *          漂移出不同的「放弃」判据。
         * @param worker 目标槽位
         * @param workerIndex 槽位序号，仅用于日志
         * @param failureReason 一句中文原因（平台错误码，或通道报回的那句）
         */
        void noteFailedStart(Worker &worker, std::size_t workerIndex, const std::string &failureReason);

        /**
         * @brief 收掉一个已退出的 worker，并决定这个槽位接下来怎么办
         * @param worker 目标槽位
         * @param workerIndex 槽位序号，仅用于日志
         * @return true 该槽位已放弃（连续崩太多次）
         */
        [[nodiscard]] bool reapWorker(Worker &worker, std::size_t workerIndex);

        /**
         * @brief 把每个槽位看一遍：缺进程的补上、已退出的收尸
         * @details 单列成一个是因为它做的是一个完整决定（该不该退避、要不要补），而编排循环只关心
         *          「这一轮结束了没有」。只在编排线程上调用，动的是那张只归本线程的槽位表。
         */
        void launchMissingAndReapExited();

        /**
         * @brief 送走全部 worker：先请求体面退出，超期强杀
         */
        void stopAllWorkers();

        /**
         * @brief 强杀之后有界等到子进程真的被收尸，再交还句柄
         * @details 收尾路径与析构兜底共用这一处：丢掉 pid 就等于留下没人收的僵尸
         */
        void waitForForcedTerminationsToLand();

        Configuration       m_configuration; ///< 编排参数（构造时已校验）
        std::vector<Worker> m_workers;       ///< worker 槽位；下标即序号，槽位固定不搬

        /**
         * @brief 全进程口径的崩溃计数与「已放弃补位」的槽位数
         * @details 本体是原子量而不是去扫槽位：槽位表由编排线程改，抓取发生在另一条线程上，
         *          扫表就要引入一把新锁。两处崩溃记账各加一次，读的人只看到「一共崩了几次」
         */
        std::atomic<std::size_t> m_totalCrashCount{0};    ///< 连续「起来就崩」的累计次数（含补位那几轮）
        std::atomic<std::size_t> m_givenUpWorkerCount{0}; ///< 已放弃补位的槽位数：涨一个就等于少一份容量，且不会自愈

        /**
         * @brief 上面两条挂在进程级指标注册表上的把手
         * @details 刻意**不导出** `runningWorkerCount()`：那个读口会顺手回收子进程并改写退出码，
         *          从抓取线程调用等于与编排线程抢回收（见其注释）。进程里有几份容量，看这条自动化
         *          的计数与日志更稳妥
         */
        std::array<Core::ProcessMetricHandle, 2> m_metricHandles{};

        /**
         * @brief 记一次「起来就崩」：槽位计数与进程级累计各加一
         * @details 两处崩溃点（起不来、存活不足窗口就退出）都走这里，避免只有一处记得抬累计
         */
        void noteWorkerCrash(Worker &worker) noexcept
        {
            ++worker.crashCount;
            m_totalCrashCount.fetch_add(1, std::memory_order_relaxed);
        }

        /// 记下某个槽位已被放弃补位（连续崩到上限）
        void noteWorkerGivenUp(Worker &worker) noexcept
        {
            worker.isGivenUp = true;
            m_givenUpWorkerCount.fetch_add(1, std::memory_order_relaxed);
        }

        /// 停止请求：只置一个无锁原子，因此信号处理函数里调用 requestStop() 是安全的
        /// （.cpp 里对 is_always_lock_free 做了断言，平台不满足会在编译期就拦住）
        std::atomic<bool> m_isStopRequested{false};

        /// 在运行的 worker 数：只由编排线程在每轮扫描末尾与收尾末尾发布，观察者线程只读它
        /// 「随父终止」保护缺席只报一次：挂不上作业是宿主性质（每个 worker 都会失败），
        /// 按 worker 报会在补位循环里把日志刷满而信息一句没多
        bool m_killGuardAbsenceReported{false};

        std::atomic<std::size_t> m_runningWorkerCount{0};
    };
} // namespace AsynGyanis::Core
