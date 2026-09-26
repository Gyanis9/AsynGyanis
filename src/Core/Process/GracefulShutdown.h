/**
 * @file GracefulShutdown.h
 * @brief 停机信号的接管：把 SIGINT/SIGTERM（Windows 上是控制台关闭事件）接到循环线程上的收尾动作
 * @author Gyanis
 * @date 2026-09-27
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;

    /// 收尾动作是否跑完的握手对象（定义在实现侧）：需要等动作跑完的调用方与投回循环的动作共用一份
    class ShutdownHandshake;

    /**
     * @brief 停机信号观察者：注册收尾动作，信号到达时在事件循环线程上依次执行
     *
     * @details 接管哪些事件：POSIX 是 SIGINT 与 SIGTERM，Windows 是 CTRL_C_EVENT、CTRL_BREAK_EVENT、
     *          CTRL_CLOSE_EVENT、CTRL_LOGOFF_EVENT 与 CTRL_SHUTDOWN_EVENT。动作一律被投回构造时给出的
     *          那个事件循环去执行——收尾通常要碰服务器与会话的状态，而那些状态按本库的线程契约只允许
     *          在所属循环上读写。
     * @details 为什么放在库里而不是示例里：此前每个使用方都要自己抄一遍 `std::signal` + 一个全局标志
     *          （`samples/main.cpp` 就是那份抄本），抄错一次的后果是「Ctrl+C 之后连接被硬切、
     *          在途请求随进程一起丢」，而且没人会知道自己抄错了。
     * @note 一个进程只允许一个观察者：信号处理器与 SIGTERM 的屏蔽都是进程级的，两个实例会互相抢。
     *       第二个实例不会装处理器，`isInstalled()` 返回 false 并记一条 ERROR 日志——不静默降级。
     * @note POSIX 侧要求**尽早构造**（在其它工作线程创建之前）：构造时把 SIGINT/SIGTERM 加进本线程的
     *       信号屏蔽字，之后派生的线程继承它，信号因此只会由本类那个等待线程（带超时的 sigtimedwait
     *       轮询）取走。
     *       已经跑起来的线程不会被打上屏蔽，它们仍可能自己收到信号并按缺省动作终止进程。
     * @note 只收**进程定向**的信号（`kill(pid, SIGTERM)`、终端的 Ctrl+C 都是这一类）：用 `raise()` 或
     *       `pthread_kill()` 送给某条线程的线程定向信号，只有那条线程自己能收，本类的等待线程取不到它。
     *       要在进程内模拟一次真信号，请用 `kill(getpid(), SIGTERM)`，或者直接调 requestShutdown()。
     * @note 目标循环如果已经退出，投回去的动作会被丢弃（`Scheduler::postRemote` 的既有语义）：
     *       收尾动作不该依赖一个已经不在跑的循环。
     */
    class GracefulShutdown
    {
    public:
        /// 这次停机是被什么引发的
        enum class Reason : std::uint8_t
        {
            Interrupt,    ///< SIGINT / Ctrl+C
            Terminate,    ///< SIGTERM
            ConsoleEvent, ///< Windows 特有的关闭、注销或关机事件
            Programmatic, ///< requestShutdown()：与信号走同一条路的程序侧触发
        };

        /**
         * @brief 绑定要承接收尾动作的事件循环，并接管停机信号
         * @param loop 收尾动作将在该循环线程上执行；不拥有它，但必须比本对象先活着
         */
        explicit GracefulShutdown(EventLoop &loop);

        /**
         * @brief 摘掉信号处理器并停掉等待线程
         * @note 已投出但还没执行的动作不受影响：它们在那个循环自己的队列里
         */
        ~GracefulShutdown();

        GracefulShutdown(const GracefulShutdown &)            = delete;
        GracefulShutdown &operator=(const GracefulShutdown &) = delete;
        GracefulShutdown(GracefulShutdown &&)                 = delete;
        GracefulShutdown &operator=(GracefulShutdown &&)      = delete;

        /**
         * @brief 注册一个收尾动作
         * @details 触发后按注册顺序依次执行；单个动作抛出的异常被就地接住并记 ERROR 日志，
         *          后面的动作照跑——否则一条日志器写不进盘就能把「把连接体面关掉」这件事一起带走。
         * @param action 要在事件循环线程上执行的动作
         * @return std::size_t 句柄，可交给 cancel() 摘掉
         */
        std::size_t onShutdown(std::function<void()> action);

        /**
         * @brief 摘掉一个已注册的动作
         * @param token onShutdown() 给出的句柄
         * @return true 摘掉了一条；false 表示句柄不存在（含已经触发过收尾的情况）
         */
        bool cancel(std::size_t token) noexcept;

        /**
         * @brief 程序侧触发一次停机，路径与真信号完全相同
         * @details 给「管理端点要停服」「自检脚本跑完就走」这类场景用：与其自己调 drain()，
         *          不如让信号与程序触发共用一份顺序，省掉两套收尾代码走岔的可能。
         *          已经触发过则什么都不做（停机是单向的）。
         * @param reason 记入 triggeredBy() 的成因
         */
        void requestShutdown(Reason reason = Reason::Programmatic) noexcept;

        /// 是否已经触发过收尾（触发后再来的信号一律放行给系统缺省处理）
        [[nodiscard]] bool isTriggered() const noexcept
        {
            return m_isTriggered.load(std::memory_order_acquire);
        }

        /// 触发成因；尚未触发时为空
        [[nodiscard]] std::optional<Reason> triggeredBy() const noexcept;

        /**
         * @brief 信号接管是否装上了
         * @return true 已装；false 表示这个进程里已有别的观察者（见类说明的那条 @note）
         */
        [[nodiscard]] bool isInstalled() const noexcept;

    private:
        /**
         * @brief 触发收尾，并视情况等它跑完
         * @param reason 触发成因
         * @param waitsForCompletion 是否阻塞等收尾动作执行完毕。Windows 的关闭/注销/关机事件必须等：
         *        处理器一返回系统就终止进程，投回循环的收尾动作根本没有机会跑
         */
        void triggerShutdown(Reason reason, bool waitsForCompletion) noexcept;

        /// 把已登记的动作投回事件循环执行（触发只发生一次，由调用方用原子量拦住）
        /// handshake 非空表示调用方要等动作跑完（Windows 的关闭事件），可为空
        void dispatchActions(std::shared_ptr<ShutdownHandshake> handshake) noexcept;

        /**
         * @brief 把一份可调用体投回构造时给的循环，跑完后放掉握手
         * @param runnable 要在循环线程上执行的东西
         * @param handshake 需要被叫醒的等待方；为空表示没人等
         */
        void postRunnable(std::function<void()> runnable, std::shared_ptr<ShutdownHandshake> handshake) noexcept;

        /**
         * @brief 执行单个收尾动作并把异常就地接住
         * @param action 待执行的动作
         */
        static void runAction(const std::function<void()> &action) noexcept;

        EventLoop *m_loop{nullptr}; ///< 承接收尾动作的循环（不拥有）

        mutable std::mutex                                         m_mutex;        ///< 保护 m_actions 与 m_nextToken
        std::vector<std::pair<std::size_t, std::function<void()>>> m_actions;      ///< 已登记的收尾动作，按注册顺序
        std::size_t                                                m_nextToken{1}; ///< 下一个可分配句柄

        std::atomic<bool>         m_isTriggered{false}; ///< 是否已触发（触发即单向，重复触发被丢掉）
        std::atomic<std::uint8_t> m_reason{0};          ///< 触发成因，取值是 Reason 的底层值；0 表示尚未触发
        std::atomic<bool>         m_isInstalled{false}; ///< 信号接管是否装上（每进程仅一个观察者装得上）

        /// POSIX 侧取信号的等待线程。**必须声明在最后**：成员按声明逆序销毁，最后声明的最先销毁，
        /// 于是 jthread 的 join 发生在上面那批原子量与容器之前——否则等待线程可能在它们已经没了
        /// 的时候还在读它们（不带超时的 sigwait 是叫不醒的，所以等待用的是固定短周期轮询）
        std::jthread m_waiter;
    };
} // namespace AsynGyanis::Core
