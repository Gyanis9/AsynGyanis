/**
 * @file Mutex.h
 * @brief 协程互斥量：同一条事件循环上的协程排队进同一段临界区
 * @author Gyanis
 * @date 2026-10-15
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "AsynGyanisExport.h"

#include "Core/Coroutine/Semaphore.h"

#include <cstddef>
#include <optional>

namespace AsynGyanis::Core
{
    class EventLoop;

    /**
     * @brief 协程版互斥量：一段代码同一时刻只让一条协程进，其余按申请先后排队
     *
     * @details 就是一个容量为 1 的 Semaphore（与 asio `async_mutex`、folly `CoMutex` 的实现同一形状），
     *          单独成类的理由只有两条：①类型上写清「这是独占权」，调用方拿不到 `acquire(2)` 这种
     *          对锁无意义的口子；②`Guard` 这个名字在读代码时才对得上「出作用域就放手」的直觉。
     *          排队、放行、撤销唤醒这些机制全部在 Semaphore 那一侧，本类不重复一遍。
     * @note 不可重入：同一条协程连着两次 `co_await lock()` 就是自锁——队伍里排在前面的是自己，
     *       于是永远等自己放手。asio 的 `async_mutex`、folly 的 `CoMutex` 同样不检测重入；本类也不检测
     *       （检测要把持有者记成「哪条协程」，而等待方在两轮之间可以换协程帧，记了也判不准）。
     *       需要可重入的场合请改用自己的状态标志，别在这里加一层。
     * @note 只锁协程，不锁线程：本对象归一条事件循环，跨线程互斥仍归 `std::mutex`。
     *       线程约束与许可账目都见 Semaphore 的类注释。
     * @note 不要用 `std::mutex`/`std::shared_mutex` 顶替本类：那些锁挂起的是线程，而循环线程一挂，
     *       这条循环上所有连接一起停摆——本仓历史上就是这么坏的（锁一律关在闭合作用域里，
     *       不许跨过挂起点）。
     */
    class ASYN_CORE_API Mutex
    {
    public:
        using Guard   = Semaphore::Permit;  ///< 到手的一段独占权：出作用域（含异常展开）即放行下一个等待者
        using Awaiter = Semaphore::Awaiter; ///< lock() 给出的可等待对象

        /**
         * @brief 构造一把未被持有的锁
         * @param loop 所属事件循环：排队与放行都在这条循环上进行
         */
        explicit Mutex(EventLoop &loop) noexcept;

        /**
         * @brief 析构：还挂在队上的等待者按 Semaphore 的停摆口径收尾（不叫醒、不代销毁，只保证不被摸）
         */
        ~Mutex();

        Mutex(const Mutex &)            = delete;
        Mutex &operator=(const Mutex &) = delete;
        Mutex(Mutex &&)                 = delete;
        Mutex &operator=(Mutex &&)      = delete;

        /**
         * @brief 进临界区：锁被别人拿着就挂起排队
         * @details 用法是 `auto guard = co_await mutex.lock();`，guard 出作用域自动放行。
         *          醒来时 `guard` 为空只有一种处置：这一轮没拿到独占权（锁的持有者先死了，或本对象
         *          在等待期间被销毁），调用方必须直接收尾，不许假定自己进了临界区。
         * @return Awaiter 可等待对象；`co_await` 的结果是 `std::optional<Guard>`
         * @throws Base::LogicException 调用者不在所属循环线程上
         */
        [[nodiscard]] Awaiter lock();

        /**
         * @brief 试着进临界区：拿不到就当场返回空，不挂起也不排队
         * @details 与 tokio 的 `try_lock` 同一条口径：不参与公平性，排着队时只要锁空着就抢走。
         *          要顺序请用 lock()。
         * @return std::optional<Guard> 拿到就有值，锁被别人持有则为空
         */
        [[nodiscard]] std::optional<Guard> tryLock();

    private:
        Semaphore m_gate; ///< 容量恒为 1 的闸门：独占权就是那一格许可
    };
} // namespace AsynGyanis::Core
