/**
 * @file AsyncExecutor.cpp
 * @brief 阻塞任务执行器实现 —— 工作线程循环与任务入队
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Database/Pool/AsyncExecutor.h"

#include <algorithm>

namespace AsynGyanis::Database
{
    AsyncExecutor::AsyncExecutor(const std::size_t workerCount)
    {
        // 0 表示自动：按硬件并发度取，取不到（返回 0）时按 1 处理。
        // 无论如何都不允许 0 个线程：没有工作线程时提交的任务永远不会被执行，
        // 调用方会看到一个永远不完成的协程，这种错误几乎无法从现象上定位
        const std::size_t resolvedWorkerCount =
            workerCount == 0 ? std::max<std::size_t>(1, std::thread::hardware_concurrency()) : workerCount;

        m_workers.reserve(resolvedWorkerCount);
        for (std::size_t index = 0; index < resolvedWorkerCount; ++index)
        {
            // 交给 jthread 一个带停止令牌的入口：停止请求由 jthread 在析构时发出，
            // 线程函数无需自己管理「何时退出」以外的任何同步
            m_workers.emplace_back(
                [this](const std::stop_token stopToken)
                {
                    workerLoop(stopToken);
                });
        }
    }

    AsyncExecutor::~AsyncExecutor()
    {
        for (std::jthread &worker: m_workers)
        {
            // 请求停止（幂等）：已请求过或无活动任务时都无副作用
            static_cast<void>(worker.request_stop());
        }

        // 唤醒所有可能阻塞在条件变量上的工作线程，让它们看到停止请求后退出。
        // 若只改标志而不唤醒，线程会一直睡到下一次有新任务（可能永远不来）
        m_condition.notify_all();

        // 这里不显式 join：worker 是 jthread，析构时会自动 join，
        // 而它们在成员声明顺序上是最后销毁的，因此 join 一定发生在队列与锁销毁之前
    }

    AsyncExecutor &AsyncExecutor::shared()
    {
        // 函数内静态对象：C++11 起初始化线程安全（magic static），
        // 且随进程退出自动销毁，届时所有工作线程都被 join，不会出现「进程退出时线程还在跑」
        static AsyncExecutor sharedExecutor;
        return sharedExecutor;
    }

    void AsyncExecutor::enqueue(std::function<void()> task)
    {
        {
            std::lock_guard lock(m_mutex);
            m_tasks.push_back(std::move(task));
            m_pendingCount.fetch_add(1, std::memory_order_relaxed);
        }

        // 入队后立刻唤醒一个等待线程。不必持锁调用 notify：唤醒与入队之间没有需要原子化的关系，
        // 被唤醒的线程会自己重新取锁并检查队列
        m_condition.notify_one();
    }

    void AsyncExecutor::workerLoop(const std::stop_token stopToken)
    {
        while (true)
        {
            std::function<void()> task;
            {
                std::unique_lock lock(m_mutex);
                m_condition.wait(lock,
                                 [this, &stopToken]()
                                 {
                                     // 两种唤醒条件：有新任务可做，或收到停止请求
                                     return stopToken.stop_requested() || !m_tasks.empty();
                                 });

                // 能走到这里且队列为空，只可能是「已请求停止且队列已空」——此时线程该退出了。
                // 停止后不再接受新任务，但队列里已接收的任务必须先跑完：直接丢弃会让等待它们的
                // 协程永远挂起（任务里保存着恢复句柄，丢了就没人再叫醒那些协程）
                if (m_tasks.empty())
                {
                    return;
                }

                task = std::move(m_tasks.front());
                m_tasks.pop_front();
                m_pendingCount.fetch_sub(1, std::memory_order_relaxed);
            }

            // 用户代码在锁外执行：一个阻塞的数据库调用可能耗时数百毫秒甚至更久，
            // 若持着队列锁执行，其它工作线程会全部堵在 wait/push 上，等于退化成单线程
            task();
        }
    }

} // namespace AsynGyanis::Database
