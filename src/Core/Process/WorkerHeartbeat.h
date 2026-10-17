/**
 * @file WorkerHeartbeat.h
 * @brief worker 进程的心跳：把「本进程每条循环都醒过一轮」这件事写进 master 给的管道，供汇聚喂狗
 * @author Gyanis
 * @date 2026-10-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 多 worker 形态的看门狗：systemd 的 `WATCHDOG_PID` 指的是 master（单元主进程），
 *          worker 自己调 sd_notify 无效也不该——喂狗的统一出口必须在 master。于是 worker 把
 *          「我这边的循环还在推进」交给一条 dedicated 管道，master 收齐所有 worker 的拍之后
 *          才向服务管理器喂一次 `WATCHDOG=1`（汇聚逻辑在 `WorkerSupervisor`）。用的仍是
 *          `RoundTicker` 那条骨架：全部循环都在这一轮醒过才写一拍——任何一条循环卡死，
 *          这个 worker 从此不再有拍，master 的判定随之停喂。
 *
 * @note 管道 write 端描述符由 master 经启动参数传入（fork 继承，见 `WorkerSupervisor`）；
 *       本类不拥有它、也不关闭它——它在子进程生命周期里恒开，关它反而会打断 master 的读端判定。
 * @note 写失败只报首条：master 不在了（EPIPE）时每拍一条 WARN 等于把日志刷满，而这一档的
 *       后果由 master 侧与父死子亡机制处理（killedWithParent），不是这里要救的形态。
 * @note 进程级 SIGPIPE 已在 `Platform::Socket::Initialization` 里忽略（Linux），
 *       EPIPE 因此落在 errno 而不是变成信号。
 * @warning 与 `ServiceWatchdog` 同两条纪律：挂上之后不得重启线程池；本对象必须在线程池
 *          `stop()` 之后再销毁。
 * @warning Windows 侧整条特性不启用（没有 systemd 看门狗）：`arm()` 恒返回 false 并给出原因，
 *          不会挂任何协程。
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Process/RoundTicker.h"
#include "Platform/Platform.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace AsynGyanis::Core
{
    class ThreadPool;

    /**
     * @brief worker 侧心跳节拍器（见文件头）：凑齐一轮就往管道写一个字节
     */
    class ASYN_CORE_API WorkerHeartbeat
    {
    public:
        /**
         * @brief 绑定要挂节拍的线程池、心跳管道的写端与拍间隔
         * @param pool 拍协程挂到它的每一条循环上；必须比本对象活得久，且本对象销毁前它要已经 stop()
         * @param heartbeatWriteDescriptor master 经启动参数传下来的管道写端；非法值在 `arm()` 里被拒
         * @param pingInterval 拍间隔（master 按自己的窗口折好传来）；非正数在 `arm()` 里被拒
         */
        WorkerHeartbeat(ThreadPool &pool, int heartbeatWriteDescriptor, std::chrono::milliseconds pingInterval) noexcept;

        /// @brief 析构时释放拍协程的帧（不关管道、不做任何通知）
        ~WorkerHeartbeat();

        WorkerHeartbeat(const WorkerHeartbeat &)            = delete;
        WorkerHeartbeat &operator=(const WorkerHeartbeat &) = delete;
        WorkerHeartbeat(WorkerHeartbeat &&)                 = delete;
        WorkerHeartbeat &operator=(WorkerHeartbeat &&)      = delete;

        /**
         * @brief 在每条循环上挂一拍拍协程
         * @return true 节拍已挂上；false 表示这一档不拍（原因见 lastError()），且没有挂任何协程
         * @note 重复调用是空操作；线程池须已 start()
         */
        bool arm();

        /// @brief 节拍是否已经挂上；可从任意线程调用（原子量）
        [[nodiscard]] bool isArmed() const noexcept;

        /// @brief 已经写出的心跳条数（含写失败的那些：计数口径是「交出过几条」）；可从任意线程调用
        [[nodiscard]] std::uint64_t pingCount() const noexcept;

        /// @brief 挂着拍协程的循环条数；未挂时为 0
        [[nodiscard]] std::size_t armedLoopCount() const noexcept;

        /**
         * @brief `arm()` 没挂上的原因
         * @return const std::string & 中文原因；`arm()` 成功后被清空；只在 `arm()` 返回后由那条调用线程读
         */
        [[nodiscard]] const std::string &lastError() const noexcept;

    private:
        /// 写一拍并计数；调用者在 `RoundTicker` 的内部锁内调用（多循环凑齐时可能来自任意一条）
        void writePing();

        ThreadPool               &m_pool;                ///< 挂节拍的线程池（不拥有）
        int                       m_writeDescriptor{-1}; ///< 心跳管道写端（不拥有、不关闭）
        std::chrono::milliseconds m_pingInterval{};      ///< 拍间隔（master 折好后传来）
        RoundTicker               m_ticker;              ///< 「每循环一拍、凑齐一轮回调」的骨架（回调 = writePing）

        std::atomic<std::uint64_t> m_pingCount{0};                 ///< 已写出的心跳条数，任意线程可读
        std::atomic<bool>          m_hasLoggedWriteFailure{false}; ///< 写失败只报首条（见文件头）

        std::string m_lastError{}; ///< arm() 没挂上的原因；只在 arm() 的失败路径里写、由那条调用线程读
    };
} // namespace AsynGyanis::Core
