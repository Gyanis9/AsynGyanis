/**
 * @file ReloadSignal.h
 * @brief 重载信号观察者：把 SIGHUP（运维意义上的「换一份配置继续跑」）接到注册的重载动作上
 * @author Gyanis
 * @date 2026-10-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;

    /**
     * @brief 重载信号观察者：注册重载动作，SIGHUP 到达时执行
     *
     * @details 为什么要有它：本引擎的重载入口一直是**文件监听**那一侧（`Base::ConfigManager::enableHotReload()`，
     *          inotify / ReadDirectoryChangesW），它回答的是「盘上的文件变了」。运维要的是另一件事——
     *          「我知道文件变了，现在请立刻重读」，标准做法是给主进程发一个 SIGHUP（systemd 的
     *          `ExecReload=` 默认就是 `/bin/kill -HUP $MAINPID`，nginx 那一族的 reload 也走这条路）。
     *          没有这一路时，操作者只能等防抖窗口自己到，或者干脆重启进程——后者把在途请求一起丢掉。
     * @details 与停机观察者（`GracefulShutdown`）是两条独立的通道：动作不同、时机不同，屏蔽的信号集也互不相交
     *          （那边管 SIGINT/SIGTERM，这边只管 SIGHUP），所以两个观察者可以在同一进程里并存。
     * @details 动作一律被投回构造时给出的那个事件循环去执行（与停机观察者同一条契约）：要重读的配置通常
     *          正被循环线程上的服务器与处理器读着，在别的线程上改它就是数据竞态。
     * @note **Linux/POSIX 专有**。Windows 没有 SIGHUP 这套约定，控制台也没有「重载」这一类事件
     *       （`CTRL_*` 五种全是停机向的），Windows 侧的 `isInstalled()` 恒为 false 并记一条说明——
     *       那不是待办，是平台事实；Windows 上的重载入口仍是文件监听那一条。
     * @note 一个进程只允许一个观察者：信号屏蔽字与待取队列都是进程级的，两个实例会互相抢同一条 SIGHUP，
     *       表现是一条运维信号只让一半动作跑。第二个实例不装接管，`isInstalled()` 回 false 并记 ERROR 日志。
     * @note 装上之后 SIGHUP 不再按缺省动作终止进程（缺省动作正是「终止」，多数人不清楚）。这正是本类要的
     *       效果，但请把它当成一次契约变更看待：原本靠 `kill -HUP` 杀进程的脚本会改成触发一次重载。
     * @note POSIX 侧要求**尽早构造**（在其它工作线程创建之前）：构造时把 SIGHUP 挡进本线程的屏蔽字，
     *       之后派生的线程继承它，信号因此只会由本类那个等待线程取走。已经跑起来的线程不会被打上屏蔽。
     * @note 只收**进程定向**的信号：要在进程内模拟一次，请用 `kill(getpid(), SIGHUP)`，
     *       或直接调 requestReload()；`raise()`/`pthread_kill()` 那种线程定向信号本类的等待线程取不到。
     */
    class ASYN_CORE_API ReloadSignal
    {
    public:
        /**
         * @brief 绑定要承接收重载动作的事件循环，并接管 SIGHUP
         * @param loop 重载动作将在该循环线程上执行；不拥有它，但必须比本对象先活着
         */
        explicit ReloadSignal(EventLoop &loop);

        /**
         * @brief 不绑定事件循环，接管 SIGHUP
         * @details 给「动作本来就与循环无关」的宿主用（例如只置一个原子量、主循环自己轮到了再重读）。
         *          动作因此**在收到信号的那条线程上就地执行**；要碰循环上的状态请用带事件循环的构造。
         */
        ReloadSignal();

        /**
         * @brief 摘掉信号接管并停掉等待线程
         * @note 已投出但还没执行的动作不受影响：它们在那个循环自己的队列里
         */
        ~ReloadSignal();

        ReloadSignal(const ReloadSignal &)            = delete;
        ReloadSignal &operator=(const ReloadSignal &) = delete;
        ReloadSignal(ReloadSignal &&)                 = delete;
        ReloadSignal &operator=(ReloadSignal &&)      = delete;

        /**
         * @brief 注册一个重载动作
         * @details 触发后按注册顺序依次执行；单个动作抛出的异常被就地接住并记 ERROR 日志，后面的动作照跑
         *          ——与停机观察者同一条理由：一次重读失败不该把「把新配置接上」整段带走。
         * @details 注册表**跨轮次保留**：每一次 SIGHUP 都会把当前登记的动作为重跑一遍（与
         *          `GracefulShutdown` 的单向一次性正好相反）。要只跑一次就自己 `cancel()`，
         *          或在动作里带一个「只装一次」的闩锁。
         * @param action 要在事件循环线程上执行的动作
         * @return std::size_t 句柄，可交给 cancel() 摘掉
         */
        std::size_t onReload(std::function<void()> action);

        /**
         * @brief 摘掉一个已注册的动作
         * @param token onReload() 给出的句柄
         * @return true 摘掉了一条；false 表示句柄不存在
         */
        bool cancel(std::size_t token) noexcept;

        /**
         * @brief 程序侧触发一次重载，路径与真信号完全相同
         * @details 给「管理端点要重读配置」「自检脚本自己触发」这类场景用：与其自己抄一遍顺序，
         *          不如让信号与程序触发共用同一条路。
         * @note 与停机不同，重载**可以重复触发**：每一次都是一轮新的重读，不做单向闩锁。
         */
        void requestReload() noexcept;

        /**
         * @brief 信号接管是否装上了
         * @return true 已装；false 表示本平台没有这条路，或本进程里已有别的观察者
         */
        [[nodiscard]] bool isInstalled() const noexcept;

        /**
         * @brief 累计触发过几次重载（信号与程序侧都算一次）
         * @return std::uint64_t 触发次数；运维面上「发了三次 HUP 只重读一次」这类问题看这一格
         */
        [[nodiscard]] std::uint64_t reloadCount() const noexcept;

        /**
         * @brief 提前把 SIGHUP 挡进本线程的信号屏蔽字（POSIX；Windows 上空转返回 true）
         * @details 与 `GracefulShutdown::blockStopSignals()` 同一用法：需要在起工作线程之前就挡住时调这一句。
         * @return true 屏蔽字已设好（或本平台没有这套机制）；false 系统调用失败
         */
        [[nodiscard]] static bool blockReloadSignal() noexcept;

    private:
        /// 装信号接管：登记进程级唯一观察者、设屏蔽字并起等待线程；失败时逐条出声并保持未装上
        void install() noexcept;

        /// 摘掉接管：停等待线程并让出进程级唯一登记位（析构调它）
        void uninstall() noexcept;

        /**
         * @brief 把注册表里的动作整份取走并交出去（触发只在此处发生一次一轮）
         * @details 「取走」与「执行」分开：等待线程只负责取，执行落在循环线程或就地，注册表因此不会
         *          在两个线程之间被半读半写
         */
        void fireReload() noexcept;

        /// 把动作交出去：绑了循环就投回循环线程，没绑就地执行
        void releaseActions(std::vector<std::function<void()>> &&actions) noexcept;

        /// 执行单个动作并把异常就地接住
        static void runAction(const std::function<void()> &action) noexcept;

        EventLoop *m_loop{nullptr}; ///< 承接收重载动作的循环（不拥有）

        mutable std::mutex                                         m_mutex;        ///< 保护 m_actions 与 m_nextToken
        std::vector<std::pair<std::size_t, std::function<void()>>> m_actions;      ///< 已登记的重载动作，按注册顺序
        std::size_t                                                m_nextToken{1}; ///< 下一个可分配句柄

        std::atomic<bool>          m_isInstalled{false}; ///< 信号接管是否装上（每进程仅一个观察者装得上）
        std::atomic<std::uint64_t> m_reloadCount{0};     ///< 累计触发次数

        /// POSIX 侧取信号的等待线程。**必须声明在最后**：成员按声明逆序销毁，最后声明的最先销毁，
        /// 于是 jthread 的 join 发生在上面那批原子量与容器之前——否则等待线程可能在它们已经没了
        /// 的时候还在读它们（sigtimedwait 带超时，所以停得下来）
        std::jthread m_waiter;
    };
} // namespace AsynGyanis::Core
