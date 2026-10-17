/**
 * @file RoundTicker.h
 * @brief 「每条循环各一拍、凑齐一轮才回调」的节拍骨架：看门狗的两家承载者（进程内喂狗与 worker 心跳）共用
 * @author Gyanis
 * @date 2026-10-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 为什么要挂在循环上而不是另起一条线程：这类心跳要判的是「这个进程还在推进」，而引擎的推进
 *          就是事件循环在转。一条独立线程发的心跳只能证明那条线程没死，循环卡死时它照发不误——那恰好
 *          是要抓的形态。因此每条工作循环各跑一拍拍协程，全部循环都在这一轮里醒过一次才回调：
 *          任何一条停摆都让这一轮凑不齐，接收方按超时接手。本类只做这条骨架；回调里发什么
 *          （`WATCHDOG=1` 还是往管道写心跳字节）由持有者决定。
 *
 * @note 拍间隔由 `arm()` 给出，本类不读任何环境变量：窗口的折半只有一处真源
 *       （`Platform::ServiceNotification::readWatchdogConfiguration()`），折完再交给这里。
 * @warning 挂上之后不得重启线程池：`ThreadPool::start()` 在 stop() 之后会换新的一批循环，
 *          先前的拍协程连同它的等待器就落到了已销毁的那批循环上。要重启就先停本对象所属的
 *          运行时，重启后重新 `arm()`。
 * @warning 本对象必须在线程池 `stop()`（工作线程已 join）之后再销毁：拍协程的帧由本类持有，
 *          而帧只能在确定没人会再恢复它的时候销毁。
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Coroutine/Task.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace AsynGyanis::Core
{
    class ThreadPool;

    /**
     * @brief 「每循环一拍、凑齐一轮回调」的节拍器
     *
     * @details 回调在持有内部锁的状态下调用：凑齐一轮的那条循环可以是任意一条，而回调里的接收方
     *          （通知通路、管道写）往往要在多条循环之间串行化，锁的存在让回调无需自己再带一把。
     *          回调因而不得反手调本类的任何方法（会自死锁），也不得阻塞：它跑在某条循环线程上。
     */
    class ASYN_CORE_API RoundTicker
    {
    public:
        /// 一轮凑齐时回调；调用时机见类说明（持内部锁、循环线程上下文）
        using RoundCallback = std::function<void()>;

        /**
         * @brief 绑定要挂节拍的线程池与凑齐一轮时的回调
         * @param pool 拍协程挂到它的每一条循环上；必须比本对象活得久，且本对象销毁前它要已经 stop()
         * @param onRoundComplete 凑齐一轮的回调；空回调合法（只数轮次不做事，给纯判据类用例留的形状）
         */
        RoundTicker(ThreadPool &pool, RoundCallback onRoundComplete) noexcept;

        /// @brief 析构时释放拍协程的帧（不做任何通知）
        ~RoundTicker();

        RoundTicker(const RoundTicker &)            = delete;
        RoundTicker &operator=(const RoundTicker &) = delete;
        RoundTicker(RoundTicker &&)                 = delete;
        RoundTicker &operator=(RoundTicker &&)      = delete;

        /**
         * @brief 在每条循环上挂一拍拍协程
         * @param interval 拍间隔；必须为正（窗口折不出至少 1 毫秒的节拍在 Platform 那一层就被拒）
         * @return true 节拍已挂上；先后两次调用中第二次是空操作，不会把同一轮挂出两条节拍
         * @note 可在线程池已经 start() 之后从任意线程调用（排度走 `Scheduler::scheduleRemote()`）。
         *       并发地同时调用两次不被支持——那是启动路径的用法错误，不是本方法要兜的场景
         */
        bool arm(std::chrono::milliseconds interval);

        /**
         * @brief 判断「这一条循环的这一次醒来」是否正好凑齐一轮（纯换算，不读状态）
         * @details 单列成纯函数是为了能被确定性地钉住：什么时候该回调、什么时候不该回调是这条
         *          通道的全部判据，而按真实时钟去等「某条循环没醒」那一格在测试里只能靠睡。
         * @param progresses 每条循环已完成的拍数，下标即循环编号；调用者那一条已含本拍
         * @param index 刚跑完这一拍的那条循环的编号
         * @return true 每条循环都至少走完了 `progresses[index]` 这一拍，即本轮齐了，该回调一次
         * @return false 还有循环没走到这一拍，本轮不回调（那条循环正停摆，或本拍只是跑得比别人快）
         */
        [[nodiscard]] static bool completesRound(const std::vector<std::uint64_t> &progresses, std::size_t index) noexcept;

        /// @brief 节拍是否已经挂上；可从任意线程调用（原子量，置真发生在拍数表与那批帧都落完之后）
        [[nodiscard]] bool isArmed() const noexcept;

        /**
         * @brief 已凑齐的轮数（即回调触发次数）
         * @return std::uint64_t 累计轮数；没挂上时恒为 0
         * @note 可从任意线程调用（原子量）。「交出几条/发出几条」那类更细的口径由持有者在回调里数
         */
        [[nodiscard]] std::uint64_t completedRoundCount() const noexcept;

        /**
         * @brief 挂着拍协程的循环条数
         * @return std::size_t 未挂时为 0，挂上后等于 `arm()` 那一刻线程池的循环条数
         * @note 这一格是「一轮要凑齐几条」的读数：重复 `arm()` 不会把它加到两倍
         */
        [[nodiscard]] std::size_t armedLoopCount() const noexcept;

    private:
        /**
         * @brief 一条循环的拍协程：到点就把本循环的拍数记上，凑齐一轮就回调
         * @param index 本协程所属循环的编号
         * @return Core::Task<> 常驻协程，帧由本对象持有到销毁
         */
        Task<> tickLoop(std::size_t index);

        ThreadPool   &m_pool;            ///< 挂节拍的线程池（不拥有）
        RoundCallback m_onRoundComplete; ///< 凑齐一轮的回调；空回调合法

        /// 保护 m_progresses 与回调：凑齐一轮的那条循环可以是任意一条，回调里的接收方往往要在
        /// 多条循环之间串行化
        mutable std::mutex         m_stateMutex;
        std::vector<std::uint64_t> m_progresses{};            ///< 每条循环已完成的拍数，下标即循环编号（在 m_stateMutex 下读写）
        std::atomic<std::int64_t>  m_intervalMicroseconds{0}; ///< 拍间隔（微秒），0 表示还没挂上
        std::atomic<bool>          m_isArmed{false};          ///< 节拍是否已挂上（载荷全落之后才置真）
        std::atomic<std::uint64_t> m_completedRoundCount{0};  ///< 已凑齐的轮数，任意线程可读

        std::vector<Task<>> m_tickTasks{}; ///< 拍协程的帧：先入表后排度，销毁前线程池必须已停
    };
} // namespace AsynGyanis::Core
