#include "Base/Log/Sinks/AsyncSink.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>

namespace AsynGyanis::Base
{
    namespace
    {
        /// Block 策略单次等待队列空间的上限：下游卡死时按丢弃处置，不无限期阻塞调用线程
        constexpr std::chrono::milliseconds kMaximumBlockWaitMilliseconds{200};
    } // namespace
    AsyncSink::AsyncSink(std::unique_ptr<LogSink> wrappedSink, const size_t queueSize, const OverflowPolicy policy) :
        m_wrappedSink(std::move(wrappedSink))
        // 容量兜底钳制：0 容量不是「不限量」而是三种策略各自的错误语义——Drop 全丢、
        // DropOldest 对空队列 pop（未定义行为）且待落地计数回绕、Block 永久阻塞。
        // 配置边界已有一次钳制，这里再做一次是为了让 AsyncSink 自身不依赖「调用方传了合法容量」。
        , m_maximumQueueSize(queueSize < kMinimumQueueSize ? kMinimumQueueSize : queueSize)
        , m_overflowPolicy(policy)
    {
        // jthread 在析构时会 request_stop 并 join；本类的 stop() 已负责唤醒条件变量后再 join，
        // 因此把停止状态统一收敛到 stop_token 上，不再另设 m_running 布尔量
        m_workerThread = std::jthread([this](const std::stop_token &stopToken)
        {
            workerLoop(stopToken);
        });
        m_stopToken = m_workerThread.get_stop_token();
    }

    AsyncSink::~AsyncSink()
    {
        stop();
    }

    void AsyncSink::write(const LogEvent &event)
    {
        std::unique_lock lock(m_queueMutex);

        // 已请求停止：事件既不会落地，也不应留在队列里冒充「待落地」而让 flush() 永远等不到 0。
        // 这条路径同样计入丢弃数，否则停止窗口内的日志会静默消失
        if (m_stopToken.stop_requested())
        {
            m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (m_overflowPolicy == OverflowPolicy::Drop)
        {
            if (m_queue.size() >= m_maximumQueueSize)
            {
                // 丢弃新到事件并计数，供运维监控日志丢失规模
                // 事件从未入队，不计入待落地计数，否则 flush() 会等到永远无法满足的条件
                m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            m_queue.push(event);
            ++m_pendingCount;
        } else if (m_overflowPolicy == OverflowPolicy::DropOldest)
        {
            // 队列非空时才会淘汰：容量至少为 1，size() >= 容量 蕴含队列非空
            if (m_queue.size() >= m_maximumQueueSize)
            {
                // 淘汰队首最旧事件，为最新日志腾出空间
                // 被淘汰的事件不会再被 worker 处理，需同步核销它的待落地计数
                m_queue.pop();
                --m_pendingCount;
                m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
            }
            m_queue.push(event);
            ++m_pendingCount;
        } else
        {
            // Block：等队列腾出空间。**等待有上界**——下游 sink 卡住（慢盘、网络盘失联）时
            // 队列再也不会腾位，而调用方可能就是事件循环线程本身，无限期等它等于把整个循环停摆。
            // 超时与「因停止而结束」同一条处置：计入丢弃数，让运维能从 droppedEventCount() 看到代价
            const bool hasSpace = m_queueCondition.wait_for(lock, kMaximumBlockWaitMilliseconds, [this]
            {
                return m_queue.size() < m_maximumQueueSize || m_stopToken.stop_requested();
            });
            if (!hasSpace || m_stopToken.stop_requested())
            {
                // 事件不会入队，与其它策略一样计入丢弃数
                m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            m_queue.push(event);
            ++m_pendingCount;
        }
        lock.unlock();
        m_queueCondition.notify_one();
    }

    uint64_t AsyncSink::droppedEventCount() const noexcept
    {
        return m_droppedEventCount.load(std::memory_order_relaxed);
    }

    void AsyncSink::flush()
    {
        std::unique_lock lock(m_queueMutex);
        // 等待条件用「待落地数为 0」而非「队列为空」：worker 取出事件后队列即空，
        // 但下游 write 尚未返回，此时放行会让 flush() 在日志仍在途时提前返回
        m_flushCondition.wait(lock, [this]
        {
            return m_pendingCount == 0 || m_stopToken.stop_requested();
        });
        if (m_wrappedSink)
        {
            m_wrappedSink->flush();
        }
    }

    void AsyncSink::stop()
    {
        // std::call_once：首次调用执行 request_stop + notify + join，重复调用直接返回；
        // 并发的多个调用者都会等到 join 完成才返回，避免「停止未完成就被调用方当作已完成」。
        // 这里不额外设「已停止」标志位：停止状态由 stop_token 单一表达，worker 循环也读同一来源
        std::call_once(m_stopOnce, [this]
        {
            {
                // 停止标记必须与 worker 的等待谓词在**同一把锁**下发布：谓词在锁内读 stop_requested()，
                // 若在锁外通知，唤醒可能落在「worker 已判定谓词为假、尚未入睡」的窗口里被丢弃，
                // worker 会永远睡在条件变量上、随后的 join() 永久阻塞
                const std::lock_guard lock(m_queueMutex);
                m_workerThread.request_stop();
                m_queueCondition.notify_all();
            }
            m_workerThread.join();
            if (m_wrappedSink)
            {
                m_wrappedSink->flush();
            }
        });
    }

    void AsyncSink::workerLoop(const std::stop_token &stopToken)
    {
        // 取出队首事件并在锁外落地：下游 write 可能长时间阻塞（如磁盘 IO），
        // 持锁写出会卡住所有生产者。落地返回后才核销待落地计数
        const auto drainOneEvent = [this](std::unique_lock<std::mutex> &lock)
        {
            const LogEvent event = std::move(m_queue.front());
            m_queue.pop();
            lock.unlock();
            try
            {
                if (m_wrappedSink)
                {
                    // 被包装 sink 自己的等级过滤要在这里补上：它永远不是 Logger 的直接子节点，
                    // 而 Logger 只按挂在它下面的 sink 预筛（LogSink 的契约里写明过滤由调用方问）。
                    // 少了这一步，`wrapped: {type: file, level: ERROR}` 会收到 DEBUG/INFO 全量内容
                    if (m_wrappedSink->shouldLog(event.level))
                    {
                        m_wrappedSink->write(event);
                    } else
                    {
                        m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (...)
            {
                // 防止单个 Sink 异常导致整个 worker 线程崩溃
                // 注意：这里刻意不在日志库内部再打日志，否则异常递归/自死锁风险大于收益，
                // 当前代价是该条事件静默丢失（由 AsyncSink 之外的调用方决定是否需要补偿）
            }
            lock.lock();
            // 队列出现空间，唤醒因队列满而阻塞的写入者
            m_queueCondition.notify_one();
            // 事件已交给下游，待落地计数归零时唤醒全部 flush 等待者
            if (--m_pendingCount == 0)
            {
                m_flushCondition.notify_all();
            }
        };

        while (!stopToken.stop_requested())
        {
            std::unique_lock lock(m_queueMutex);
            m_queueCondition.wait(lock, [this, &stopToken]
            {
                return !m_queue.empty() || stopToken.stop_requested();
            });
            while (!m_queue.empty())
            {
                drainOneEvent(lock);
            }
            m_flushCondition.notify_all();
        }
        // 停止后尽力排空队列中残留的事件
        {
            std::unique_lock lock(m_queueMutex);
            while (!m_queue.empty())
            {
                drainOneEvent(lock);
            }
            m_flushCondition.notify_all();
        }
    }
} // namespace AsynGyanis::Base
