#include "Core/Coroutine/AsyncExecutor.h"

#include "Platform/System/CpuAffinity.h"

namespace AsynGyanis::Core
{
    AsyncExecutor::AsyncExecutor(const std::size_t workerCount)
    {
        // 0 表示自动：按「本进程实际可用的核数」取（容器配额与 cpuset 会把它收窄）。
        // 无论如何都不允许 0 个线程：没有工作线程时提交的任务永远不会被执行，
        // 调用方会看到一个永远不完成的协程，这种错误几乎无法从现象上定位。
        // 这里更要收紧：工作线程干的是压缩这类纯 CPU 活，按宿主核数起会在配额内直接抢走
        // 事件循环本就不多的 CPU 时间——正是外派压缩想要保护的那一方
        const std::size_t resolvedWorkerCount =
                workerCount == 0 ? Platform::CpuAffinity::recommendedWorkerCount() : workerCount;

        m_workers.reserve(resolvedWorkerCount);
        for (std::size_t index = 0; index < resolvedWorkerCount; ++index)
        {
            // 交给 jthread 一个带停止令牌的入口：停止请求由 jthread 在析构时发出，
            // 线程函数无需自己管理「何时退出」以外的任何同步
            m_workers.emplace_back(
                    [this](const std::stop_token &stopToken)
                    {
                        workerLoop(stopToken);
                    });
        }
    }

    AsyncExecutor::~AsyncExecutor()
    {
        // 先立停止标志再请求停止：此后提交的任务一律被拒绝（显式失败），
        // 队列里已接收的任务仍由下面的流程跑完
        m_isStopping.store(true, std::memory_order_release);

        {
            // 停止请求必须与 worker 的等待谓词在**同一把锁**下发布：谓词（读 stop_requested 与队列）
            // 在锁内求值，锁外通知时唤醒可能落在「worker 已判定谓词为假、尚未入睡」的窗口里被丢弃——
            // worker 会永远睡着，jthread 析构时的 join 随之永久阻塞
            const std::lock_guard lock(m_mutex);
            for (std::jthread &worker: m_workers)
            {
                // 请求停止（幂等）：已请求过或无活动任务时都无副作用
                static_cast<void>(worker.request_stop());
            }
            // 唤醒所有可能阻塞在条件变量上的工作线程，让它们看到停止请求后退出
            m_condition.notify_all();
        }

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

    AsyncExecutor::SubmissionResult AsyncExecutor::enqueue(std::function<void()> task)
    {
        {
            std::lock_guard lock(m_mutex);
            // 已进入停止流程：工作线程即将（或已经）退出，没人会取这张队列，
            // 收下任务等于让提交方永久挂起——宁可当场拒绝，由调用方显式失败
            if (m_isStopping.load(std::memory_order_acquire))
            {
                return SubmissionResult::RejectedByShutdown;
            }

            // 排队上限按「每线程」算，判定与入队同在一把锁里：既不超卖名额，也不会出现
            // 「报了 Accepted 却没排上」。超出的一律如实拒绝，让提交方去降并发——
            // 队列无界时一次下游变慢就能把进程撑死，而那本来只是几个请求的延迟问题
            if (m_tasks.size() >= workerCount() * kMaximumPendingTasksPerWorker)
            {
                return SubmissionResult::RejectedBySaturatedQueue;
            }

            m_tasks.push_back(std::move(task));
            m_pendingCount.fetch_add(1, std::memory_order_relaxed);
        }

        // 入队后立刻唤醒一个等待线程。不必持锁调用 notify：唤醒与入队之间没有需要原子化的关系，
        // 被唤醒的线程会自己重新取锁并检查队列
        m_condition.notify_one();
        return SubmissionResult::Accepted;
    }

    void AsyncExecutor::workerLoop(const std::stop_token &stopToken)
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

            // 用户代码在锁外执行：一次阻塞的数据库调用可以耗到数百毫秒，一次整块压缩也要数毫秒，
            // 若持着队列锁执行，其它工作线程会全部堵在 wait/push 上，等于退化成单线程
            task();
        }
    }

} // namespace AsynGyanis::Core
