/**
 * @file Scheduler.h
 * @brief 协程调度器：线程本地就绪队列 + 跨线程投递，不做工作窃取
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <deque>
#include <mutex>
#include <vector>

namespace AsynGyanis::Platform
{
    class EventNotifier;
}

namespace AsynGyanis::Core
{
    /**
     * @brief 协程调度器：线程本地就绪队列 + 跨线程投递，不做工作窃取
     * @details **不做工作窃取**：全局队列里的任务恰恰是「必须回到这个循环上执行」的那些
     *          （跨线程完成回调要 resume 在发起者循环、套接字与 TLS 通道按循环归属），把它们
     *          偷到别的循环执行会破坏亲和性并引入数据竞争；跨循环的负载均衡发生在接受层
     *          （每循环一个监听器 + SO_REUSEPORT），不发生在就绪队列层。
     * @note 本类非线程安全，除 scheduleRemote() 与 postRemote() 这两个投递入口外，其他成员函数
     *       （含 hasWork() 与 runOne()/runAll()）都应由所属 EventLoop 线程调用。
     */
    class Scheduler
    {
    public:
        /**
         * @brief 单趟 runAll() 从跨线程队列里取走的任务上限
         * @details 投递方可以长期不断流（执行器完成回调、别的循环移交的连接），不设上界的一趟
         *          会吃到生产者停手为止，事件循环因此再也回不到 epoll_wait，同循环上的套接字
         *          一个事件都收不到。超出部分的投递不丢：hasWork() 仍为真，循环下一趟接着取
         */
        static constexpr std::size_t kMaximumRemoteItemsPerPass = 256;

        /**
         * @brief 默认构造调度器，内部结构为空
         */
        Scheduler() = default;

        /**
         * @brief 绑定跨线程调度唤醒器
         * @param notifier 唤醒器指针，传入 nullptr 表示禁用唤醒功能
         */
        void setWakeupNotifier(Platform::EventNotifier *notifier) noexcept;

        /**
         * @brief 将协程加入本地就绪队列（本线程调用）
         * @param handle 准备调度的协程句柄（必须非空）
         */
        void schedule(std::coroutine_handle<> handle);

        /**
         * @brief 跨线程调度：将协程推入全局队列（线程安全）
         * @param handle 准备调度的协程句柄（必须非空）
         * @note 本函数解引用调度器自身：调用方（执行器工作线程、解析线程等）必须在整个投递期间
         *       保证目标循环还活着。要么按「先拆执行器再拆循环」的顺序释放资源，要么先判
         *       「等待方还在不在」再投（AsyncResolver 就是这么收口窗口的）
         */
        void scheduleRemote(std::coroutine_handle<> handle);

        /**
         * @brief 在本循环上稍后执行一段代码，不跨线程（与 schedule() 同一线程约束）
         *
         * @details 「决定动作」与「执行动作」要分开一拍、而执行主体不是协程时用它（如定时器到期后
         *          不直接 resume 等待者，而是把恢复动作排进本轮清空）。
         * @param callable 待执行的可调用对象；空对象会被忽略
         * @note 与 schedule() 一样只在所属 EventLoop 线程调用；取出顺序是**先进先出**，投递方排进来的
         *       顺序就是执行顺序，与本地就绪队列的栈式顺序不是一回事
         */
        void postLocal(std::function<void()> callable);

        /**
         * @brief 跨线程投递一段普通代码：在**目标循环**上执行一次（线程安全）
         *
         * @details 用于「动作不属于任何协程帧」的跨循环移交（如把刚接受的连接交给另一个循环接手）：
         *          它必须在目标循环的线程上创建对象并挂进那边的在途表，语义与 scheduleRemote() 一致。
         * @param callable 待执行的可调用对象；空对象（未绑定任何函数）会被忽略
         * @note **异常会向目标循环传播**（与 resume() 抛出相同），投递方应保证自己不抛：需要兜住的错误在可调用对象内部处理
         * @note 目标循环若在轮到它之前就退出，队列里尚未执行的对象会被丢弃——持有系统资源的投递方应包在 RAII 句柄里
         */
        void postRemote(std::function<void()> callable);

        /**
         * @brief 执行一个就绪协程
         * @return true 表示成功执行了一个协程，false 表示无任务可执行
         */
        bool runOne();

        /**
         * @brief 执行所有就绪协程：本地队列清空，跨线程队列每趟最多取 kMaximumRemoteItemsPerPass 件就返回
         * @note 返回时若跨线程队列还有剩余，hasWork() 仍为真，调用方下一趟接着取（不会丢也不会误判空闲）
         */
        void runAll();

        /**
         * @brief 查询是否有待处理的协程
         * @return true 表示至少有一个就绪协程
         */
        [[nodiscard]] bool hasWork() const;

        /**
         * @brief 获取本地就绪队列大小（用于监控/调试）
         * @return 本地队列中的协程数量
         */
        [[nodiscard]] size_t localQueueSize() const;

    private:
        std::vector<std::coroutine_handle<> > m_localQueue;      ///< 本地就绪队列（本线程独享，无锁，使用 vector 模拟栈）
        std::deque<std::function<void()> >    m_localCallables;  ///< 本地待执行代码（同上无锁，先进先出）
        std::deque<std::coroutine_handle<> >  m_globalQueue;     ///< 全局就绪队列（跨线程安全，受 m_globalMutex 保护）
        std::deque<std::function<void()> >    m_remoteCallables; ///< 跨线程投递的普通代码（同上受 m_globalMutex 保护，FIFO）
        std::mutex                            m_globalMutex;     ///< 保护全局队列与跨线程回调队列的互斥锁
        std::atomic<size_t>                   m_globalCount{0};  ///< 全局队列长度（原子变量，用于快速判空）
        std::atomic<size_t>                   m_remoteCallableCount{0}; ///< 跨线程回调条数（同上，用于快速判空）
        Platform::EventNotifier *             m_wakeup{nullptr}; ///< 唤醒器指针，nullptr 表示未启用唤醒
    };
}
