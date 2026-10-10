/**
 * @file ServiceWatchdog.h
 * @brief 服务管理器的看门狗节拍：喂狗这条心跳由事件循环 own 着发，循环停摆就不再喂
 * @author Gyanis
 * @date 2026-10-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Coroutine/Task.h"
#include "Platform/System/ServiceNotification.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class ThreadPool;

    /**
     * @brief 看门狗节拍的承载者：在线程池的每条循环上挂一拍定时协程，凑齐一轮就喂一次
     *
     * @details 为什么要挂在循环上而不是另起一条线程：监督者要判的是「这个进程还在推进」，而引擎的推进
     *          就是事件循环在转。一条独立线程喂的狗只能证明那条线程没死，循环卡死时它照喂不误——那恰好
     *          是 watchdog 唯一要抓的形态。因此每条工作循环各跑一拍拍协程，**全部**循环都在这一轮里
     *          醒过一次才发一条 `WATCHDOG=1`：任何一条停摆都让这一轮凑不齐，监督者按超时接手。
     * @details 节拍与窗口的换算只有一处真源：`Platform::ServiceNotification::readWatchdogConfiguration()`
     *          按 sd_watchdog_enabled(3) 读变量并折半，本类不再算第二遍，也不调 libsystemd。
     * @note 未配看门狗（没有 `$WATCHDOG_USEC`、`$WATCHDOG_PID` 指的是别的进程、Windows 侧）时
     *       `arm()` 返回 false 并且**不在线程池上挂任何协程**：跑一条永远不发东西的常驻协程，
     *       比不跑更容易让人以为这条通道是通的。原因写在 lastError() 里。
     * @warning 挂上之后不得重启线程池：`ThreadPool::start()` 在 stop() 之后会换新的一批循环，
     *          先前的拍协程连同它的等待器就落到了已销毁的那批循环上。要重启就先停本对象所属的
     *          运行时，重启后重新 `arm()`。
     * @warning 本对象必须在线程池 `stop()`（工作线程已 join）之后再销毁：拍协程的帧由本类持有，
     *          而帧只能在确定没人会再恢复它的时候销毁。
     */
    class ASYN_CORE_API ServiceWatchdog
    {
    public:
        /**
         * @brief 绑定要挂节拍的线程池与要用的通知通路
         * @param pool 节拍协程挂到它的每一条循环上；必须比本对象活得久，且本对象销毁前它要已经 stop()
         * @param notification 已经 `open()` 的通知通路；本类不拥有它，但发节拍的循环可以是任意一条，
         *        因此对它的发送在本类内部串行化
         */
        ServiceWatchdog(ThreadPool &pool, Platform::ServiceNotification &notification) noexcept;

        /**
         * @brief 析构时释放拍协程的帧（不做任何通知）
         */
        ~ServiceWatchdog();

        ServiceWatchdog(const ServiceWatchdog &)            = delete;
        ServiceWatchdog &operator=(const ServiceWatchdog &) = delete;
        ServiceWatchdog(ServiceWatchdog &&)                 = delete;
        ServiceWatchdog &operator=(ServiceWatchdog &&)      = delete;

        /**
         * @brief 读监督者给的窗口，并在每条循环上挂一拍拍协程
         * @return true 节拍已挂上；false 表示这一档不喂狗（原因见 lastError()），且没有挂任何协程
         * @note 可在任意线程调用（排程走 `Scheduler::scheduleRemote()`）；先后两次调用中第二次是
         *       空操作，不会把同一轮挂出两条节拍。并发地同时调用两次不被支持——那是启动路径的
         *       用法错误，不是本方法要兜的场景
         */
        bool arm();

        /**
         * @brief 判断「这一条循环的这一次醒来」是否正好凑齐一轮（纯换算，不读状态）
         * @details 单列成纯函数是为了能被确定性地钉住：什么时候该发、什么时候不该发是这条通道的
         *          全部判据，而按真实时钟去等「某条循环没醒」那一格在测试里只能靠睡。
         * @param progresses 每条循环已完成的拍数，下标即循环编号；调用者那一条已含本拍
         * @param index 刚跑完这一拍的那条循环的编号
         * @return true 每条循环都至少走完了 `progresses[index]` 这一拍，即本轮齐了，该喂一次
         * @return false 还有循环没走到这一拍，本轮不喂（那条循环正停摆，或本拍只是跑得比别人快）
         */
        [[nodiscard]] static bool completesRound(const std::vector<std::uint64_t> &progresses, std::size_t index) noexcept;

        /**
         * @brief 节拍是否已经挂上
         * @return true `arm()` 成功过；false 还没挂或挂不上
         * @note 可从任意线程调用（原子量；置真发生在窗口与拍数表都落完之后）
         */
        [[nodiscard]] bool isArmed() const noexcept;

        /**
         * @brief 已经交出去的节拍条数
         * @return std::uint64_t 累计条数；没挂上时恒为 0
         * @note 可从任意线程调用（原子量）。它只统计「交出」，不统计「送达」——数据报没有回执，
         *       发失败时本计数同样加一，失败原因看发送那一步的日志
         */
        [[nodiscard]] std::uint64_t pingCount() const noexcept;

        /**
         * @brief 当前节拍间隔
         * @return std::chrono::milliseconds 已挂上时是窗口的二分之一（向下取整：宁可喂早也不喂晚）；
         *         没挂上时为 0
         * @note 可从任意线程调用（原子量）
         */
        [[nodiscard]] std::chrono::milliseconds pingInterval() const noexcept;

        /**
         * @brief 挂着拍协程的循环条数
         * @return std::size_t 未挂时为 0，挂上后等于 `arm()` 那一刻线程池的循环条数
         * @note 这一格是「一轮要凑齐几条」的读数：重复 `arm()` 不会把它加到两倍，
         *       而喂狗要的恰恰是「一条循环一拍」这件事能被外部核对
         */
        [[nodiscard]] std::size_t armedLoopCount() const noexcept;

        /**
         * @brief `arm()` 没挂上的原因
         * @return const std::string & 中文原因；`arm()` 成功后被清空
         * @note 只在 `arm()` 返回后由那条调用线程读：本串不在锁里，别的线程读它没有意义
         */
        [[nodiscard]] const std::string &lastError() const noexcept;

    private:
        /**
         * @brief 一条循环的拍协程：到点就把本循环的拍数记上，凑齐一轮就喂
         * @param index 本协程所属循环的编号
         * @return Core::Task<> 常驻协程，帧由本对象持有到销毁
         */
        Task<> tickLoop(std::size_t index);

        /// 交出一条 WATCHDOG=1 并计数；调用者必须已持有 m_stateMutex
        void sendPingLocked();

        ThreadPool                    &m_pool;         ///< 挂节拍的线程池（不拥有）
        Platform::ServiceNotification &m_notification; ///< 发节拍用的通知通路（不拥有）

        /// 保护 m_progresses 与对 m_notification 的发送：凑齐一轮的那条循环可以是任意一条，
        /// 而 ServiceNotification 本身不是线程安全的
        mutable std::mutex         m_stateMutex;
        std::vector<std::uint64_t> m_progresses{};                ///< 每条循环已完成的拍数，下标即循环编号（在 m_stateMutex 下读写）
        std::atomic<std::int64_t>  m_pingIntervalMicroseconds{0}; ///< 窗口折出的节拍（微秒），0 表示还没挂上
        std::atomic<bool>          m_isArmed{false};              ///< 节拍是否已挂上（载荷全落之后才置真）
        std::atomic<std::uint64_t> m_pingCount{0};                ///< 已交出的节拍条数，任意线程可读

        std::string         m_lastError{}; ///< arm() 没挂上的原因；只在 arm() 的失败路径里写
        std::vector<Task<>> m_tickTasks{}; ///< 拍协程的帧：先入表后排度，销毁前线程池必须已停
    };
} // namespace AsynGyanis::Core
