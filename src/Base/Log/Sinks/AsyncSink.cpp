#include "Base/Log/Sinks/AsyncSink.h"

#include <algorithm>
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

        /// 回收已消费前缀的最小槽位数：低于这个规模，搬一次内存的代价不如再攒一会儿
        constexpr std::size_t kMinimumReclaimableSlotCount = 32U;

        /// 槽位数组的起始规模：一次配置成百上千条日志的 Sink 不少，起步太小会让头几次入队各扩一次
        constexpr std::size_t kInitialSlotCapacity = 8U;
    } // namespace
    AsyncSink::AsyncSink(std::unique_ptr<LogSink> wrappedSink, const size_t queueSize, const OverflowPolicy policy) :
        m_wrappedSink(std::move(wrappedSink))
        // 容量两端都要钳。下界：0 容量不是「不限量」而是三种策略各自的错误语义——Drop 全丢、
        // DropOldest 对空队列 pop（未定义行为）且待落地计数回绕、Block 永久阻塞。
        // 上界：一条事件在队列里要占 sizeof(LogEvent) 字节，容量乘以它就是下游卡住时最多占住的
        // 内存；配置里 queue_size 多打几个 0，本该「按策略丢弃或阻塞」的背压就变成了 OOM。
        // 配置边界已有一次钳制，这里再做一次是为了让 AsyncSink 自身不依赖「调用方传了合法容量」
        , m_maximumQueueSize(std::clamp(queueSize, kMinimumQueueSize, kMaximumQueueSize))
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
        // 左值入口只做一件事：复制一份交出所有权，之后与接管那条走同一段代码，
        // 于是三种溢出策略的判定与计数不必各写一遍
        write(LogEvent{event});
    }

    void AsyncSink::write(LogEvent &&event)
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
            if (queuedEventCount() >= m_maximumQueueSize)
            {
                // 丢弃新到事件并计数，供运维监控日志丢失规模
                // 事件从未入队，不计入待落地计数，否则 flush() 会等到永远无法满足的条件
                m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            appendSlot(std::move(event));
            ++m_acceptedCount;
        } else if (m_overflowPolicy == OverflowPolicy::DropOldest)
        {
            // 队列非空时才会淘汰：容量至少为 1，在队数 >= 容量 蕴含队列非空
            if (queuedEventCount() >= m_maximumQueueSize)
            {
                // 淘汰队首最旧事件，为最新日志腾出空间
                // 被淘汰的事件不会再被 worker 处理，需同步核销它的待落地计数
                discardFrontSlot();
                // 被淘汰的事件不再有人等它，当场了结；新进来的才是本次受理的那条
                settleAcceptedEvent();
                m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
            }
            appendSlot(std::move(event));
            ++m_acceptedCount;
        } else
        {
            // Block：等队列腾出空间。**等待有上界**——下游 sink 卡住（慢盘、网络盘失联）时
            // 队列再也不会腾位，而调用方可能就是事件循环线程本身，无限期等它等于把整个循环停摆。
            // 超时与「因停止而结束」同一条处置：计入丢弃数，让运维能从 droppedEventCount() 看到代价
            const bool hasSpace = m_spaceCondition.wait_for(lock, kMaximumBlockWaitMilliseconds, [this]
            {
                return queuedEventCount() < m_maximumQueueSize || m_stopToken.stop_requested();
            });
            if (!hasSpace || m_stopToken.stop_requested())
            {
                // 事件不会入队，与其它策略一样计入丢弃数
                m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            appendSlot(std::move(event));
            ++m_acceptedCount;
        }
        lock.unlock();
        // 只叫消费者：**入队是占走一个空位，不是腾出一个空位**，等着腾位的写入者本来就不该被这一步
        // 叫醒。生产者与消费者共用一条条件变量时，notify_one 有概率落在某个正在等空位的写入者身上——
        // 它复检「还有空位」为假于是再睡下，这次唤醒就此吞掉，而事件已经在队列里、消费者还在睡。
        // 之后每条写入都要付满 Block 超时并计入丢弃，flush() 更是等不到 pending 归零
        m_workCondition.notify_one();
    }

    void AsyncSink::appendSlot(LogEvent &&event)
    {
        // 回收条件：已消费的槽位攒到一定规模，且不少于在队事件数。回收一次要把整段在队事件往前搬，
        // 太频繁就成了「每取一条搬一次」，白扣掉核销槽位省下的那笔
        if (m_headIndex >= kMinimumReclaimableSlotCount && m_headIndex >= queuedEventCount())
        {
            m_slots.erase(m_slots.begin(), m_slots.begin() + static_cast<std::ptrdiff_t>(m_headIndex));
            m_headIndex = 0;
        }
        if (m_slots.size() == m_slots.capacity())
        {
            // 倍增至配置容量为止：入队因此只在扩容那一次取堆，且峰值内存不超过调用方要的队列规模。
            // 保底留出一格是因为 size() 里可能还压着一段待回收的前缀，它比在队事件数更大
            const std::size_t doubledCapacity = m_slots.capacity() * 2U < kInitialSlotCapacity
                                                    ? kInitialSlotCapacity
                                                    : m_slots.capacity() * 2U;
            m_slots.reserve(std::max(std::min(doubledCapacity, m_maximumQueueSize), m_slots.size() + 1U));
        }
        m_slots.push_back(std::move(event));
    }

    LogEvent AsyncSink::takeFrontSlot()
    {
        LogEvent event = std::move(m_slots[m_headIndex]);
        discardFrontSlot();
        return event;
    }

    void AsyncSink::discardFrontSlot()
    {
        ++m_headIndex;
        // 队列刚好排空时下标与槽位一起归零：留着已消费的前缀会让后面的入队不断向尾部扩张
        if (m_headIndex == m_slots.size())
        {
            m_slots.clear();
            m_headIndex = 0;
        }
    }

    std::size_t AsyncSink::queuedEventCount() const noexcept
    {
        return m_slots.size() - m_headIndex;
    }

    void AsyncSink::settleAcceptedEvent()
    {
        // 调用方持有队列锁：这里与 flush() 读的两个计数因此天然同步
        ++m_settledCount;
        // 只有确实有人在等时才碰条件变量：每条事件都 notify_all 会让不相关的写路径白付唤醒钱
        if (m_flushWaiterCount != 0)
        {
            m_flushCondition.notify_all();
        }
    }

    uint64_t AsyncSink::droppedEventCount() const noexcept
    {
        return m_droppedEventCount.load(std::memory_order_relaxed);
    }

    void AsyncSink::flush()
    {
        {
            std::unique_lock lock(m_queueMutex);
            // 水位取进场那一刻的受理数：worker 取出事件后队列即空但下游 write 还没返回，
            // 所以不能等「队列空」；而等「清零」在有持续生产者的进程里永远不会成立——
            // 后来者的账不是本次要等的账
            const std::size_t targetAcceptedCount = m_acceptedCount;
            ++m_flushWaiterCount;
            m_flushCondition.wait(lock, [this, targetAcceptedCount]
            {
                return m_settledCount >= targetAcceptedCount || m_stopToken.stop_requested();
            });
            --m_flushWaiterCount;
        }
        // 转发刷新必须在锁外：这一句可能是 FlushFileBuffers 或一次标准输出刷新，握着队列锁
        // 做它就等于让全进程所有写日志的线程排在一次慢盘刷新后面。m_wrappedSink 自构造起
        // 不再换指，下游各 Sink 自己的写与刷新也各自持锁，stop() 早就是这么排的
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
                // 停止标记必须与等待谓词在**同一把锁**下发布：谓词在锁内读 stop_requested()，
                // 若在锁外通知，唤醒可能落在「等待方已判定谓词为假、尚未入睡」的窗口里被丢弃，
                // worker 会永远睡在条件变量上、随后的 join() 永久阻塞
                const std::lock_guard lock(m_queueMutex);
                m_workerThread.request_stop();
                // 两类等待者各有各的条件变量，两条都要叫：只叫一条会把另一类留在睡梦里
                m_workCondition.notify_all();
                m_spaceCondition.notify_all();
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
            const LogEvent event = takeFrontSlot();
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
            // 队列腾出空间，只叫因队列满而阻塞的写入者（消费者此刻不需要被叫）
            m_spaceCondition.notify_one();
            // 事件已交给下游（或被过滤丢掉），本次受理的账到这里才算清
            settleAcceptedEvent();
        };

        while (!stopToken.stop_requested())
        {
            std::unique_lock lock(m_queueMutex);
            m_workCondition.wait(lock, [this, &stopToken]
            {
                return queuedEventCount() > 0 || stopToken.stop_requested();
            });
            while (queuedEventCount() > 0)
            {
                drainOneEvent(lock);
            }
            m_flushCondition.notify_all();
        }
        // 停止后尽力排空队列中残留的事件
        {
            std::unique_lock lock(m_queueMutex);
            while (queuedEventCount() > 0)
            {
                drainOneEvent(lock);
            }
            m_flushCondition.notify_all();
        }
    }
} // namespace AsynGyanis::Base
