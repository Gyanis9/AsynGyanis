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
#include "Platform/System/ServiceNotification.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Core
{
    /// 崩溃重启退避的上限：指数增长到这里为止，再长只会让恢复变慢
    inline constexpr std::chrono::milliseconds kMaximumWorkerRestartBackoff{30'000};

    /// 退避抖动的幅度：在基准值上下各 20% 内随机，避免多个槽位在同一时刻一起重启
    inline constexpr double kWorkerRestartBackoffJitterRatio = 0.2;

    /**
     * @brief 编排器交给 worker 的那个参数名（移交模式下自动追加，值就是交接通道地址）
     * @details worker 侧要认它：拿到地址后用 Core::adoptHandedOverListener(值, 预算) 取回一份
     *          已在监听的套接字，再按各服务器「接管已监听描述符」的构造入口起服务。
     *          调用方把自己那份 listen() 之后就没本类的事了——本层只负责送，不负责收。
     */
    inline constexpr std::string_view kHandedOverListenerArgument = "--handed-over-listener";

    /**
     * @brief 编排器交给 worker 的心跳管道写端参数名（仅 POSIX + 配了看门狗窗口时自动追加，值是 fd 号）
     * @details worker 侧要认它：拿到 fd 后建 `Core::WorkerHeartbeat` 并 `arm()`。描述符由 fork 继承、
     *          参数里只有号码——这条通道只在多 worker + systemd 看门狗同时在场时存在
     */
    inline constexpr std::string_view kWatchdogPipeArgument = "--watchdog-pipe-fd";

    /**
     * @brief 编排器交给 worker 的心跳拍间隔参数名（毫秒，自动追加，值由窗口折半而来）
     * @details worker 不知道窗口，只按这个间隔拍：折半这一步与 `ServiceWatchdog` 同源
     *          （`Platform::ServiceNotification::readWatchdogConfiguration()`），编排器算一次传下去
     */
    inline constexpr std::string_view kWatchdogPingArgument = "--watchdog-ping-ms";

    /**
     * @brief 多进程 worker 的编排器（master 侧）：起进程、盯退出、按需重启、收尾送走；worker 参数全部由调用方给
     * @warning run() 必须在进程还是单线程时调用：worker 由 fork + exec 起（见 Platform::Process），
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
     * @note 进程之间不共享任何状态：按来源 IP 的并发限额、限流桶、指标计数都是各进程一份。
     *       多进程下这意味着「单来源上限 × N、请求速率上限 × N、指标要按进程分别采集」——
     *       要真正的全局口径就得引入进程间共享（或改用单进程多工作循环 + 接受分发）。
     * @note 崩溃循环的收口口径：连续「起来就崩」达到 @c crashLoopLimit 的槽位会被放弃（不再补它，
     *       打一条 ERROR 点名槽位）；若整池都被放弃，run() 返回 false 交给进程管理器接手。只要还有其他
     *       槽位在跑，被放弃的槽位静默满 `10 × crashLoopWindow` 后会重新纳入一次补位尝试（崩计数
     *       清零）——给「配置被改好」「依赖恢复」这类外部修复留一条回去的路，同时不让它每轮都试。
     *       导出的指标 `asyn_worker_slots_given_up` 是累计放弃次数，重新纳入不会把它减回去。
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
            std::string               executablePath;    ///< 要起的可执行文件（通常就是本进程自己的映像）
            std::vector<std::string>  workerArguments;   ///< 每个 worker 的固定参数（不含 argv[0]）
            std::size_t               workerCount{1};    ///< worker 个数；必须大于 1，等于 1 时直接用单进程跑，不需要本类
            std::chrono::milliseconds pollInterval{100}; ///< 观察存活与响应停止请求的轮询间隔
            /**
             * @brief 崩溃重启退避的基准值：第 n 次连续崩溃后等 base × 2^(n-1)，封顶 30 秒，再叠 ±20% 抖动
             * @details 恒定间隔并不能避免崩溃循环里打转（一个必崩的 worker 会以固定频率无限重启）；
             *          指数增长让反复失败的槽位逐步让出 CPU 与日志，抖动避免多个槽位在同一刻一起回来。
             *          首次启动（崩计数为 0）不走退避，冷启动因此不会串成 N × base。
             */
            std::chrono::milliseconds restartBackoff{500};
            std::chrono::milliseconds shutdownTimeout{10000}; ///< 收尾期限：请求退出后等到这个点就强杀
            /**
             * @brief 「起来就崩」的存活门槛，同时是放弃后重新纳入的静默窗口基数
             * @details 存活不足这个时长就退出算一次「起来就崩」；跑满它才退出的 worker 会清零崩计数
             *          （被当作稳定运行，而不是崩溃循环的一环）。槽位被放弃后，静默满
             *          `10 × crashLoopWindow` 会重新纳入一次补位尝试——见类注释里的自愈口径。必须大于 0
             */
            std::chrono::milliseconds crashLoopWindow{3000};
            std::size_t               crashLoopLimit{5}; ///< 连续「起来就崩」达到这个次数就停止补该 worker；至少为 1
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
             *          回收，不说明它已经绑上监听端口——那一段还要等它自己起完循环。所以这一格能给的是
             *          「整池都到位了」，端口上真有人应答要由调用方自己再验一次（`ReferenceServer` 就是
             *          先等这条回调、再对本机端口做一次 TCP 连接试探，然后才向服务管理器报就绪）。
             * @note 回调跑在编排线程上，不得长时间阻塞：那条线程同时负责补崩掉的 worker 与响应停止请求。
             *       每轮重数一遍个数，凑齐那一刻只回调一次；worker 后来崩了不再补起也不会二次回调——
             *       「就绪」是一次性陈述，不是持续状态。
             */
            std::function<void()> onAllWorkersRunning{};

            /**
             * @brief 编排开始收口时回调一次（停止请求到达或整池被放弃）；空 = 不回调
             * @details 给「要告诉监督者我在收尾」那一类调用方用：这一刻 worker 还没被通知退出，
             *          整池仍在跑，正是一次 `STOPPING=1` 该有的位置。放在 `stopAllWorkers()` 之前而不是
             *          `run()` 返回之后——后者已经收尾完了，再报就成了事后说明。
             * @note 同样跑在编排线程上；由信号触发的停止里，本回调是在循环线程而不是信号处理函数里跑的
             *       （处理函数只置原子标记，见 `requestStop()`）。
             */
            std::function<void()> onStopRequested{};

            /**
             * @brief 看门狗通知通路（可空 = 不启用汇聚喂狗）
             * @details 非空且窗口读得出（`readWatchdogConfiguration()`）时，本类为每个槽位建一条心跳管道
             *          并把写端经 `kWatchdogPipeArgument`/`kWatchdogPingArgument` 传给 worker：就绪之后，
             *          只有每个未放弃槽位都在本窗口内有过拍才向该通路喂一条 `WATCHDOG=1`。任何一条
             *          worker 的心跳停摆（事件循环卡死、进程僵住）都会让判定失败而停喂，监督者按超时
             *          重启整个单元——与单进程形态「循环停摆就不再喂」同一语义。
             * @note 本类不拥有通知通路；它须比本对象活得久。Windows 侧整条路径不启用（没有 systemd
             *       看门狗，读窗口那一步自然为空）
             */
            Platform::ServiceNotification *watchdogNotification{nullptr};
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
         * @brief 按连续崩溃次数算下一次补位的退避（指数 + 封顶 + 抖动）
         * @details 第 n 次连续崩溃（n ≥ 1）的基准是 `base × 2^(n-1)`，封顶 @c kMaximumWorkerRestartBackoff；
         *          再乘 `1 ± kWorkerRestartBackoffJitterRatio` 的随机因子。单列成静态函数是为了让
         *          增长、封顶与抖动边界可以被确定性用例钉住（编排层只负责喂一个 [0,1) 的随机数）。
         * @param base 配置里的 restartBackoff（基准值）
         * @param crashCount 连续崩溃次数；0 或 1 都按第一次算（首次启动不调用本函数，调用方按 0 次处理）
         * @param jitterUnit [0,1) 的随机数；0.5 表示不抖动（恰为基准值）
         * @return std::chrono::milliseconds 本次退避时长
         */
        [[nodiscard]] static std::chrono::milliseconds restartBackoffFor(std::chrono::milliseconds base, std::size_t crashCount, double jitterUnit) noexcept;

        /**
         * @brief 当前仍在运行的 worker 数（日志与诊断用），任何线程可调
         * @return std::size_t 个数；这是编排线程最近一轮扫描的快照，最长滞后一个 pollInterval
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

        /**
         * @brief 判断「各槽位的心跳是否全部新鲜」（纯换算，不读状态）
         * @details 单列成纯函数与 `isPoolComplete` 同一个理由：什么时候该喂、什么时候不该喂是这条
         *          汇聚通道的全部判据，而端到端造「某条 worker 的心跳恰好停摆」要靠真进程与真时钟。
         * @param heartbeatFreshBySlot 每槽位本窗口读到过拍没有
         * @param isGivenUpBySlot 每槽位是否已放弃补它（放弃的槽位不再参与——它那一路由「整池放弃」处理）
         * @return true 每个未放弃的槽位都新鲜，可以喂；表不配对或空池时 false（空池没有「都健康」可言）
         */
        [[nodiscard]] static bool allHeartbeatsFresh(const std::vector<bool> &heartbeatFreshBySlot, const std::vector<bool> &isGivenUpBySlot) noexcept;

        /**
         * @brief 判断一个槽位的心跳此刻算不算新鲜（纯换算，不读状态）
         * @details 账本记的是「上一拍什么时候到的」而不是「这一轮读到过没有」：判定与出拍共用同一个周期
         *          （都取窗口折半），只说「读到过没有」的布尔会在检查抢到拍子的那一轮被清掉，而下一轮的
         *          相位关系一模一样，于是永远喂不出去。单列成纯函数是因为这一格的真形只有「真进程 +
         *          真时钟」才造得出来，端到端复跑它只能靠睡。
         * @param lastBeatAt 本槽位上一次读到心跳字节的时刻；空表示成立以来还没读到过拍
         * @param now 判定时刻
         * @param startupGraceUntil 起拍宽限截止，只在「还没读到过拍」时起作用（worker 启动与起拍本身要时间）
         * @param pingInterval 心跳节拍；允许的陈旧时长取 1.5× 节拍——正常到点的拍子最坏也只老一个节拍多一点，
         *        而连着两拍没到的一定越过这条线，于是真停摆仍在一个看门狗窗口内停喂
         * @return true 新鲜，本轮喂
         * @return false 陈旧（停摆，或首拍没赶上宽限），本轮不喂并由监督者按超时接手
         */
        [[nodiscard]] static bool isHeartbeatFresh(const std::optional<std::chrono::steady_clock::time_point> &lastBeatAt, std::chrono::steady_clock::time_point now,
                                                   std::chrono::steady_clock::time_point startupGraceUntil, std::chrono::milliseconds pingInterval) noexcept;

        /**
         * @brief 判断「具备喂狗资格」（纯换算，不读状态）：启用过、就绪已报、未收停机请求
         * @details 就绪门防的是「池缺人还喂健康」——有槽位被放弃时池永远不齐、就绪永不报，
         *          这一格让监督者按超时接手；停机门防的是「收尾流程里假装服务健康」。
         *          单列成纯函数是因为就绪前那段窗口在端到端里短到观察不到（池比一次轮询还齐得快），
         *          这一格只有确定性的判据钉得住。
         * @param enabled 汇聚是否已启用
         * @param readinessAnnounced 整池就绪是否已报过
         * @param stopRequested 是否已收停机请求
         * @return true 可以进入心跳新鲜判定与喂发
         */
        [[nodiscard]] static bool isEligibleToFeed(bool enabled, bool readinessAnnounced, bool stopRequested) noexcept;

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
            std::chrono::steady_clock::time_point givenUpAt{};      ///< 放弃时刻：静默够久后重新纳入一次
            /// 心跳管道的读端（-1 = 该槽位没有心跳通道：没启用看门狗，或是 Windows）；只在编排线程上碰
            int heartbeatReadFd{-1};
            /// 上一次读到心跳字节的时刻；空表示这个槽位成立以来还没读到过拍。
            /// 「本窗口读到过没有」那种布尔表达在这里会输掉相位赛跑（判据见 `isHeartbeatFresh`）
            std::optional<std::chrono::steady_clock::time_point> lastHeartbeatAt{};
            /// 刚（重）起之后的宽限截止：还没读到过拍时，宽限内也算新鲜——worker 启动与起拍本身要时间，
            /// 而这份时间不该由监督者的超时来赔
            std::chrono::steady_clock::time_point heartbeatGraceUntil{};
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

        /**
         * @brief 每轮扫一遍各槽位的心跳管道：有字节就把该槽位标新鲜，宽限过了还没字节就标陈旧
         * @param now 本轮的时刻（编排线程算一次传进来，免得各槽位读数落在不同的钟点上）
         * @note 只在编排线程上调用；未启用看门狗时是空操作
         */
        void pollWorkerHeartbeats(std::chrono::steady_clock::time_point now) noexcept;

        /**
         * @brief 到点（窗口折出的节拍）且资格成立就喂一条 WATCHDOG=1
         * @details 资格三件：就绪已报过（`m_hasAnnouncedReadiness`）、没收到停机请求、
         *          `allHeartbeatsFresh` 为真。任一不成立就留到下一轮再看，并把这一轮的原因按节拍
         *          限频打出来——「为什么没喂」正是排障要的第一句。
         * @param now 本轮的时刻
         * @note 只在编排线程上调用；未启用看门狗时是空操作
         */
        void feedWatchdogIfDue(std::chrono::steady_clock::time_point now);

        /**
         * @brief 读窗口并按需启用看门狗汇聚，返回编排循环的轮询粒度
         * @details 调用方给了通知通路才谈启用；窗口读得出才真正启用（没配 WatchdogSec= 是常态，按信息级
         *          交代一句）。启用后轮询粒度取 `min(pollInterval, 节拍/2)`——喂点判定的分辨率必须细于
         *          节拍的一半，否则「到点」会被睡眠粒度量成「晚点」
         * @return std::chrono::milliseconds 编排循环每轮该睡多久
         * @note 只在 run() 的启动段调用一次
         */
        std::chrono::milliseconds enableWatchdogFeeding();

        /**
         * @brief 整池就绪那一刻的一次性处理：回调观察者、然后把心跳账本翻到「宽限中」
         * @details 看门狗从就绪起计：此前不判定也不喂（与单进程形态「先报就绪再挂节拍」同一顺序）。
         *          宽限让 worker 启动与起拍的时间不算陈旧——那份时间不该由监督者的超时来赔
         * @note 只在编排线程上调用；`m_hasAnnouncedReadiness` 由本方法置真
         */
        void handlePoolReady();

        /**
         * @brief 给一个槽位建心跳管道并把写端参数注入 worker 启动参数（仅 POSIX + 看门狗启用时）
         * @param worker 目标槽位（读端描述符记在它身上）
         * @param workerIndex 槽位序号，仅用于日志
         * @param launchOptions 待派生的启动参数（就地向其 arguments 追加两个参数）
         * @return true 不需要管道或管道已就绪；false 表示开不出来（已按一次「起来就崩」记账）
         * @note Windows 上恒为 true（没有该通道）；调用方在 spawn 之后负责关掉 master 侧的写端副本
         */
        [[nodiscard]] bool attachWatchdogPipe(Worker &worker, std::size_t workerIndex, Platform::Process::LaunchOptions &launchOptions);

        /// 收掉某个槽位的心跳管道读端（重起前、放弃时、收尾时共用；关 -1 是空操作）
        void closeWorkerHeartbeatPipe(Worker &worker) noexcept;

        /// 一轮槽位扫描的读数：在跑的个数与已放弃的个数
        struct SlotScanResult
        {
            std::size_t runningCount{0}; ///< 句柄有效且在跑的槽位数
            std::size_t givenUpCount{0}; ///< 已放弃补位的槽位数
        };

        /**
         * @brief 扫一遍槽位表：数出在跑与已放弃的个数，并把在跑个数发布到观察者读的原子量上
         * @return SlotScanResult 两种个数
         * @note 只在编排线程上调用；不探句柄之外的任何状态
         */
        [[nodiscard]] SlotScanResult scanWorkerSlots() noexcept;

        /// 看门狗启用后，spawn 成功时要关掉的 master 侧写端副本（attachWatchdogPipe 记下；-1 = 没有）
        int m_pendingWatchdogWriteFd{-1};

        /**
         * @brief 组装一个槽位的启动参数：可执行文件、固定参数、槽位序号、进程组与随父终止
         * @param workerIndex 槽位序号（补起的 worker 沿用同一号）
         * @return Platform::Process::LaunchOptions 可直接交给 spawn 的启动参数（心跳/移交参数由调用方追加）
         */
        [[nodiscard]] Platform::Process::LaunchOptions buildLaunchOptions(std::size_t workerIndex) const;

        Configuration       m_configuration; ///< 编排参数（构造时已校验）
        std::vector<Worker> m_workers;       ///< worker 槽位；下标即序号，槽位固定不搬

        /**
         * @brief 看门狗汇聚的启用状态与节拍（仅通知通路非空且窗口可读时启用）
         * @details 喂点判据是「距上次喂已过一个节拍」；失败只报首条，理由与 worker 心跳那条相同
         */
        bool                                  m_isWatchdogFeedingEnabled{false};
        std::chrono::milliseconds             m_watchdogPingInterval{0};
        std::chrono::steady_clock::time_point m_lastWatchdogFeed{};
        bool                                  m_hasLoggedWatchdogFeedFailure{false};

        /// 整池就绪只报一次：那一格是给监督者的陈述，不是持续状态（提升为成员是为了让看门狗资格判定读得到）
        bool m_hasAnnouncedReadiness{false};

        /**
         * @brief 全进程口径的崩溃计数与「已放弃补位」的槽位数
         * @details 本体是原子量而不是去扫槽位：槽位表由编排线程改，抓取发生在另一条线程上，
         *          扫表就要引入一把新锁。两处崩溃记账各加一次，读的人只看到「一共崩了几次」
         */
        std::atomic<std::size_t> m_totalCrashCount{0};    ///< 连续「起来就崩」的累计次数（含补位那几轮）
        std::atomic<std::size_t> m_givenUpWorkerCount{0}; ///< 累计放弃补位的槽位次数（重新纳入后再放弃会继续加）

        /// 退避抖动：只在编排线程上取数，故不需要加锁；种子取 random_device，避免多进程同刻同序重启
        std::mt19937_64                        m_restartJitterGenerator;
        std::uniform_real_distribution<double> m_restartJitterDistribution{0.0, 1.0};

        /**
         * @brief 上面两条挂在进程级指标注册表上的把手
         * @details 刻意不导出 `runningWorkerCount()`：那个读口会顺手回收子进程并改写退出码，
         *          从抓取线程调用等于与编排线程抢回收（见其注释）。进程里有几份容量，看这条自动化
         *          的计数与日志更稳妥
         */
        std::array<Core::ProcessMetricHandle, 2> m_metricHandles{};

        /**
         * @brief 记一次「起来就崩」：槽位计数与进程级累计各加一
         * @details 两处崩溃点（起不来、存活不足窗口就退出）都走这里，避免只有一处记得抬累计。
         *          「连续」由 `reapWorker` 的稳定运行分支清零来界定；起不来那一档没有存活时长，
         *          连续失败自然累积。这里刻意不按两次崩溃的间隔清零：指数退避会把间隔越拉越长，
         *          到某一档必然超过 crashLoopWindow——那样永远凑不满阈值，熔断形同虚设
         */
        void noteWorkerCrash(Worker &worker) noexcept
        {
            ++worker.crashCount;
            m_totalCrashCount.fetch_add(1, std::memory_order_relaxed);
        }

        /// 记下某个槽位已被放弃补位（连续崩到上限），并留下重新纳入的静默起点
        void noteWorkerGivenUp(Worker &worker) noexcept
        {
            worker.isGivenUp = true;
            worker.givenUpAt = std::chrono::steady_clock::now();
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
