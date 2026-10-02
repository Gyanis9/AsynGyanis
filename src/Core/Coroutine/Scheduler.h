/**
 * @file Scheduler.h
 * @brief 协程调度器：线程本地就绪队列 + 跨线程投递，不做工作窃取
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "AsynGyanisExport.h"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <functional>
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
     * @note 本类非线程安全，除 scheduleRemote() 与 postRemote() 这两个投递入口、以及只读原子计数的
     *       remotePendingCount() 与 failedDispatchCount() 之外，其他成员函数（含 hasWork() 与 runOne()/runAll()）
     *       都应由所属 EventLoop 线程调用。
     * @note **协程帧的归属：调度器从不拥有、也从不销毁任何帧。** 队列里存的是裸
     *       `std::coroutine_handle<>`，它只表示「这一拍要 resume 谁」，不带来任何所有权。因此一条
     *       句柄在**被派发之前**必须一直有人持有它所属的帧，否则派发时 resume 的就是已经被销毁的内存。
     *       持有者只能是两类：还在 `co_await` 它的父协程（帧由父侧的 Task 拿着），或者一个活到派发之后的
     *       `Task` 对象（本仓库的写法是把 Task 存进服务器的在途表，见 TcpServer::m_connectionTasks）。
     * @warning 由此有两条容易写错的地方：①**不要把 owning Task 放在会先于派发析构的作用域里**——
     *          `Task` 的析构与移动赋值都无条件 `destroy()`（见 Task.h 那条「帧不会自行释放」的注释），
     *          局部 Task 出作用域就把还在队列里的帧一起销毁了；②**先入表、后排度**，反过来的话入表那一步
     *          抛 bad_alloc 就留下一个没人持有、却已经能被打发的句柄。这两条都不是推演：本仓库曾在
     *          「隧道随帧销毁」上真出过一次业务帧丢失（收口改成先唤醒再销毁的那一轮）。
     * @see schedule(), scheduleRemote()
     */
    class ASYN_CORE_API Scheduler
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
         * @param handle 准备调度的协程句柄；**空句柄被就地忽略**（不丢任何东西：本来就没有要恢复的帧）
         * @note 帧的归属见类注释那条 @note：本方法只借这个句柄用一拍，既不拥有也不销毁它
         */
        void schedule(std::coroutine_handle<> handle);

        /**
         * @brief 跨线程调度：将协程推入全局队列（线程安全）
         * @param handle 准备调度的协程句柄；空句柄同样被就地忽略
         * @note 帧的归属与 schedule() 同一条：跨线程排队的这一拍里，帧必须仍由它的持有者管着
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
         * @note **可调用对象抛出的异常不会传到投递方，也不会穿出目标循环**：它在所属循环的派发级
         *       守卫里被就地收下（计入 failedDispatchCount()，首条告警）。此前这里是「异常向目标循环
         *       传播」，而那条传播链的终点是线程入口——一条坏投递会带走整条循环和它上面的全部连接。
         * @note 目标循环若在轮到它之前就退出，队列里尚未执行的对象会被丢弃——持有系统资源的投递方应包在 RAII 句柄里
         */
        void postRemote(std::function<void()> callable);

        /**
         * @brief 执行一个就绪协程
         * @return true 表示成功执行了一个协程，false 表示无任务可执行
         * @note 被执行的协程或可调用对象抛出时就地收下并计数，不向调用方传播（与 runAll() 同一口径）
         */
        bool runOne();

        /**
         * @brief 执行所有就绪协程：本地队列清空，跨线程队列每趟最多取 kMaximumRemoteItemsPerPass 件就返回
         * @note 返回时若跨线程队列还有剩余，hasWork() 仍为真，调用方下一趟接着取（不会丢也不会误判空闲）
         * @note 每一批**全部跑完**，单条抛出既不丢掉同批其余、也不向调用方传播：计入
         *       failedDispatchCount() 并在首条告警。早先是「跑完整批再把首个异常重抛」，
         *       而 runAll() 的调用点是事件循环的泵——那等于让一条坏投递停掉整条循环
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

        /**
         * @brief 跨线程投递里还没被取走的件数（协程 + 可调用体）
         * @details 与 hasWork() 不同，本函数**只读原子计数**，不碰本地队列，因此任意线程可调——
         *          事件循环的自观测快照要靠它回答「活儿已经堆在门口而没人进来取」，而那正是循环
         *          可能已经停住的时候。本地就绪队列不在口径里：那份账只有循环线程自己数得清。
         * @return std::size_t 两条跨线程队列的长度之和
         */
        [[nodiscard]] std::size_t remotePendingCount() const noexcept;

        /**
         * @brief 被派发级守卫就地收下的抛出条数（协程恢复与投递的可调用对象合并计数）
         * @details 这是一条「循环还活着、但有人在里面抛」的判据：此前抛出会沿传播链停掉整条循环，
         *          于是「一条坏投递」与「整个 worker 不再服务」之间没有任何可观测的中间态。
         *          与 remotePendingCount() 同理，本函数只读原子计数，因此任意线程可调。
         * @return std::size_t 累计条数
         */
        [[nodiscard]] std::size_t failedDispatchCount() const noexcept;

    private:
        /**
         * @brief 记下一条被派发级守卫收下的抛出：先计数，再在首条时告警
         * @note 必须在 catch 块内调用（要取 std::current_exception() 的原文）。告警自身失败时
         *       只吞掉告警：计数已经落定，不能让日志通路反过来把异常重新放出去
         */
        void noteDispatchFailure();

        /**
         * @brief 执行一条派发体，把它抛出的异常就地收下并计数
         * @tparam Work 无参可调用体（std::function 或捕获式 lambda）
         *
         * @details 派发点的兜底要逐处写就会漂移：本类有 12 处「执行别人投进来的东西」，
         *          其中几处原先漏了守卫，同一条循环因此对本地投递和跨线程投递给出两种失败语义。
         *          收成一处之后，新增派发点只需包一层。
         * @param work 待执行的派发体
         */
        template<typename Work>
        void runGuarded(Work &&work)
        {
            try
            {
                work();
            } catch (...)
            {
                noteDispatchFailure();
            }
        }

        std::vector<std::coroutine_handle<>> m_localQueue;             ///< 本地就绪队列（本线程独享，无锁，使用 vector 模拟栈）
        std::deque<std::function<void()>>    m_localCallables;         ///< 本地待执行代码（同上无锁，先进先出）
        std::deque<std::coroutine_handle<>>  m_globalQueue;            ///< 全局就绪队列（跨线程安全，受 m_globalMutex 保护）
        std::deque<std::function<void()>>    m_remoteCallables;        ///< 跨线程投递的普通代码（同上受 m_globalMutex 保护，FIFO）
        std::mutex                           m_globalMutex;            ///< 保护全局队列与跨线程回调队列的互斥锁
        std::atomic<size_t>                  m_globalCount{0};         ///< 全局队列长度（原子变量，用于快速判空）
        std::atomic<size_t>                  m_remoteCallableCount{0}; ///< 跨线程回调条数（同上，用于快速判空）
        std::atomic<std::size_t>             m_failedDispatchCount{0}; ///< 被派发级守卫就地收下的抛出条数（任意线程可读，见 failedDispatchCount()）
        Platform::EventNotifier             *m_wakeup{nullptr};        ///< 唤醒器指针，nullptr 表示未启用唤醒
    };
} // namespace AsynGyanis::Core
