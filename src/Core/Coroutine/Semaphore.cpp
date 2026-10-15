#include "Core/Coroutine/Semaphore.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/LogicException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/EventLoop/EventLoop.h"

#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Core
{
    void CoroutineResumeTicket::attach(const std::coroutine_handle<> handle) noexcept
    {
        m_handle.store(handle);
    }

    void CoroutineResumeTicket::revoke() noexcept
    {
        // 取走而不是写空：留着旧句柄，投递方会以为自己还没恢复过这个帧
        m_handle.exchange(nullptr);
    }

    void CoroutineResumeTicket::resumeOnce() noexcept
    {
        // exchange 取走再恢复：同一个句柄只允许被恢复一次，先判后取会让两次投递都以为自己拿到了
        const std::coroutine_handle<> handle = m_handle.exchange(nullptr);
        if (handle == nullptr)
        {
            return; // 等待器已经析构（帧被销毁、异常展开、或这一份已被别处取走）：本次唤醒落空
        }
        try
        {
            handle.resume();
        } catch (const std::exception &failure)
        {
            LOG_ERROR_FMT("Semaphore: 恢复等待中的协程时抛穿了 noexcept 边界，本次唤醒已丢弃（原因：{}）", failure.what());
        } catch (...)
        {
            LOG_ERROR("Semaphore: 恢复等待中的协程时抛穿了 noexcept 边界，本次唤醒已丢弃（非标准异常）");
        }
    }

    Semaphore::Semaphore(EventLoop &loop, const std::size_t initialPermitCount) noexcept : m_loop(loop), m_capacity(initialPermitCount), m_availablePermitCount(initialPermitCount)
    {
    }

    Semaphore::~Semaphore()
    {
        const std::size_t abandonedWaiterCount   = m_waitingCount;
        const std::size_t outstandingPermitCount = m_capacity - m_availablePermitCount;

        // 队上还挂着人：本对象一死就再没人放行，逐个标记成「已脱离队列」——它们的析构只认这个标记，
        // 不会再摸本对象。已经记到账的等待者醒来拿到空结果，协程帧交给各自的持有者销毁
        // （收尾口径与 TimerQueue 停摆时一致：不叫醒、不代销毁，只保证不被摸）
        for (Awaiter *waiter = m_waiterHead; waiter != nullptr;)
        {
            Awaiter *const next    = waiter->m_next;
            waiter->m_semaphore    = nullptr;
            waiter->m_isLinked     = false;
            waiter->m_grantedCount = 0;
            waiter                 = next;
        }
        m_waiterHead   = nullptr;
        m_waiterTail   = nullptr;
        m_waitingCount = 0;

        // 许可没全回来只有两种成因：调用方把 Permit 放得比本对象久（悬垂），或同一份许可被还了两次。
        // 两种都要出声——静默的话，下一个读代码的人只能从「队伍莫名堵死」倒推
        if (abandonedWaiterCount != 0 || outstandingPermitCount != 0)
        {
            LOG_WARN_FMT("Semaphore: 析构时仍有 {} 个等待者挂在队上、{} 个许可在外面。原因：信号量比它的等待者或许可先死。"
                         "替代做法：让持有本信号量的对象先收拢在途协程（与连接池、HTTP/2 会话的「先唤醒再销毁」同一处理），"
                         "再销毁本对象",
                         abandonedWaiterCount, outstandingPermitCount);
        }
    }

    Semaphore::Awaiter Semaphore::acquire(const std::size_t permitCount)
    {
        if (permitCount == 0)
        {
            throw Base::InvalidArgumentException("取许可失败：许可个数为 0。原因：0 个许可既不占用并发额度也拦不住任何人，本类不猜调用方想表达什么。"
                                                 "替代做法：不需要许可就别等，需要就按至少 1 个取");
        }
        assertOwnerThread("Semaphore::acquire");
        return Awaiter(*this, permitCount);
    }

    std::optional<Semaphore::Permit> Semaphore::tryAcquire(const std::size_t permitCount)
    {
        if (permitCount == 0)
        {
            throw Base::InvalidArgumentException("取许可失败：许可个数为 0。原因：0 个许可既不占用并发额度也拦不住任何人，本类不猜调用方想表达什么。"
                                                 "替代做法：不需要许可就直接往下做，需要就按至少 1 个取");
        }
        assertOwnerThread("Semaphore::tryAcquire");
        // 本口不看队伍：手上有额度就给（口径与 tokio 的 try_acquire 一致，见类注释）
        if (permitCount > m_availablePermitCount)
        {
            return std::nullopt;
        }
        m_availablePermitCount -= permitCount;
        return Permit{*this, permitCount};
    }

    void Semaphore::release(const std::size_t permitCount)
    {
        if (permitCount == 0)
        {
            throw Base::InvalidArgumentException("放回许可失败：许可个数为 0。原因：0 个放行不会叫醒任何人，是一次空操作。"
                                                 "替代做法：按实际多出来的个数放回，通常应该是让 Permit 出作用域自动归还");
        }
        assertOwnerThread("Semaphore::release");
        returnPermits(permitCount, ReturnSource::PublicRelease);
    }

    std::size_t Semaphore::availablePermitCount() const noexcept
    {
        return m_availablePermitCount;
    }

    std::size_t Semaphore::waitingCount() const noexcept
    {
        return m_waitingCount;
    }

    void Semaphore::linkAwaiter(Awaiter &awaiter) noexcept
    {
        awaiter.m_previous = m_waiterTail;
        awaiter.m_next     = nullptr;
        if (m_waiterTail != nullptr)
        {
            m_waiterTail->m_next = &awaiter;
        } else
        {
            m_waiterHead = &awaiter;
        }
        m_waiterTail       = &awaiter;
        awaiter.m_isLinked = true;
        ++m_waitingCount;
    }

    void Semaphore::unlinkAwaiter(Awaiter &awaiter) noexcept
    {
        if (!awaiter.m_isLinked)
        {
            return;
        }
        awaiter.m_isLinked = false;
        --m_waitingCount;
        if (awaiter.m_previous != nullptr)
        {
            awaiter.m_previous->m_next = awaiter.m_next;
        } else
        {
            m_waiterHead = awaiter.m_next;
        }
        if (awaiter.m_next != nullptr)
        {
            awaiter.m_next->m_previous = awaiter.m_previous;
        } else
        {
            m_waiterTail = awaiter.m_previous;
        }
        awaiter.m_previous = nullptr;
        awaiter.m_next     = nullptr;
    }

    void Semaphore::grantFromHead()
    {
        std::vector<std::shared_ptr<CoroutineResumeTicket>> readyTickets;
        for (Awaiter *waiter = m_waiterHead; waiter != nullptr; waiter = waiter->m_next)
        {
            // 已经记过账的队头不再占额度，直接跳过去看下一个
            if (waiter->m_grantedCount != 0)
            {
                continue;
            }
            // 严格顺序：队头要不到就停，后面要得少的也不许越过它（类注释里那条取舍）
            if (waiter->m_permitCount > m_availablePermitCount)
            {
                break;
            }
            m_availablePermitCount -= waiter->m_permitCount;
            waiter->m_grantedCount = waiter->m_permitCount;
            readyTickets.push_back(waiter->m_ticket);
        }
        if (readyTickets.empty())
        {
            return;
        }
        // 恢复排到下一拍，而不是就地 resume：放行常常发生在 Permit 的析构里，就地恢复等于让别人
        // 在我的析构途中跑起来，它若顺手销毁了本对象的持有者，返回时摸到的就是已释放的 this。
        // 投的也不是裸句柄而是凭证：句柄摘不回来，凭证能在等待器析构时被撤销
        m_loop.scheduler().postLocal(
                [tickets = std::move(readyTickets)]
                {
                    for (const std::shared_ptr<CoroutineResumeTicket> &ticket: tickets)
                    {
                        ticket->resumeOnce();
                    }
                });
    }

    void Semaphore::returnPermits(const std::size_t permitCount, const ReturnSource source)
    {
        // 不变式：可发放 + 还在外面的 == 容量。走到越界只可能是同一份许可被还了两次
        if (permitCount > m_capacity - m_availablePermitCount)
        {
            if (source == ReturnSource::PublicRelease)
            {
                throw Base::LogicException("Semaphore: 放回 " + std::to_string(permitCount) + " 个许可会超出容量 " + std::to_string(m_capacity) +
                                           "。原因：同一份许可被放了两次，或 release() 与手上的 Permit 重复归还。"
                                           "替代做法：只在没有 RAII 载体时才直接调 release()，其余归还交给 Permit 析构");
            }
            // Permit 的归还点在 noexcept 边界上（析构），抛不出去：夹到容量并记错误，账目仍然闭合
            const std::size_t clampCount = m_capacity - m_availablePermitCount;
            LOG_ERROR_FMT("Semaphore: 归还 {} 个许可会超出容量 {}，已按 {} 个夹住。原因：同一份许可被还了两次", permitCount, m_capacity, clampCount);
            m_availablePermitCount = m_capacity;
        } else
        {
            m_availablePermitCount += permitCount;
        }

        if (source == ReturnSource::PermitReturn)
        {
            // 归还点可能在析构上：放行要往调度器投一件东西，投不动（内存吃紧、线程不合）时
            // 就地收下并出声，不让一次归还把进程 terminate
            try
            {
                grantFromHead();
            } catch (const std::exception &failure)
            {
                LOG_ERROR_FMT("Semaphore: 归还许可后放行等待者失败，额度已入账，等待者由下一次放行叫醒（原因：{}）", failure.what());
            } catch (...)
            {
                LOG_ERROR("Semaphore: 归还许可后放行等待者失败（非标准异常），额度已入账，等待者由下一次放行叫醒");
            }
            return;
        }
        grantFromHead();
    }

    void Semaphore::assertOwnerThread(const char *operation) const
    {
        // 判据与 Scheduler::schedule()、TimerQueue::insert() 同一条：循环没在跑时不设限
        // （构造期在主线程建对象、再把循环交给另一条线程是既有良性形态）
        if (!m_loop.isOnOwnerThread())
        {
            throw Base::LogicException(std::string(operation) + " 被外来线程使用：本信号量只归一条事件循环，而那条循环正在别处跑。"
                                                                "原因：队伍与额度计数都没有锁，外来线程排队会把链表的簿记打乱。"
                                                                "替代做法：外来线程先 postRemote() 投进所属循环，再在那条循环里 co_await");
        }
    }

    Semaphore::Permit::Permit(Semaphore &semaphore, const std::size_t permitCount) noexcept : m_semaphore(&semaphore), m_permitCount(permitCount)
    {
    }

    Semaphore::Permit::Permit(Permit &&other) noexcept : m_semaphore(other.m_semaphore), m_permitCount(other.m_permitCount)
    {
        // 交出所有权的一侧清零：同一份许可只能还一次，两个持有者都还就是把门多开一格
        other.m_semaphore   = nullptr;
        other.m_permitCount = 0;
    }

    Semaphore::Permit &Semaphore::Permit::operator=(Permit &&other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        // 先还自己手上的，再接管对方的（顺序反了会让本方那份没人还）
        if (m_semaphore != nullptr && m_permitCount != 0)
        {
            m_semaphore->returnPermits(m_permitCount, Semaphore::ReturnSource::PermitReturn);
        }
        m_semaphore         = other.m_semaphore;
        m_permitCount       = other.m_permitCount;
        other.m_semaphore   = nullptr;
        other.m_permitCount = 0;
        return *this;
    }

    Semaphore::Permit::~Permit()
    {
        if (m_semaphore != nullptr && m_permitCount != 0)
        {
            m_semaphore->returnPermits(m_permitCount, Semaphore::ReturnSource::PermitReturn);
        }
    }

    std::size_t Semaphore::Permit::permitCount() const noexcept
    {
        return m_permitCount;
    }

    Semaphore::Awaiter::Awaiter(Semaphore &semaphore, const std::size_t permitCount) noexcept : m_semaphore(&semaphore), m_permitCount(permitCount)
    {
    }

    Semaphore::Awaiter::~Awaiter()
    {
        // 凭证先撤销：本帧都要没了，投出去的那次唤醒必须落空（裸句柄一旦投出去就摘不回来）
        if (m_ticket)
        {
            m_ticket->revoke();
        }
        // 信号量已经析构并把自己标成脱离：此时既不摸它的链表，也不还许可（额度随对象一起没了）
        if (m_semaphore == nullptr)
        {
            return;
        }
        unlinkIfLinked();
        // 已记账却没被取走（醒来前帧被销毁、或恢复被取消）：许可必须回到池子里，
        // 不还就是许可泄漏——队伍会永久堵死，而且现场只表现为「没人再进得去」
        if (m_grantedCount != 0)
        {
            const std::size_t grantedCount = std::exchange(m_grantedCount, 0);
            m_semaphore->returnPermits(grantedCount, Semaphore::ReturnSource::PermitReturn);
        }
    }

    void Semaphore::Awaiter::unlinkIfLinked() noexcept
    {
        if (m_isLinked)
        {
            m_semaphore->unlinkAwaiter(*this);
        }
    }

    bool Semaphore::Awaiter::await_ready() noexcept
    {
        Semaphore &semaphore = *m_semaphore; // acquire() 刚构造完本对象，信号量必然还活着
        // 队伍非空就不许当场拿走：许可只按申请顺序给（这一格是「不越队抢」的唯一落点）
        if (semaphore.m_waiterHead != nullptr || semaphore.m_availablePermitCount < m_permitCount)
        {
            return false;
        }
        semaphore.m_availablePermitCount -= m_permitCount;
        m_grantedCount = m_permitCount;
        return true;
    }

    bool Semaphore::Awaiter::await_suspend(const std::coroutine_handle<> handle)
    {
        try
        {
            m_ticket = std::make_shared<CoroutineResumeTicket>();
        } catch (...)
        {
            // 协程语言规定 await_suspend 抛出后本协程留在挂起点上，而挂上去没人叫醒就是永久堵死。
            // 处置与连接池的等待体同形：不挂起、回 false 交调用方按「这一轮没拿到许可」结账
            // （await_resume 看到账上是空的，自然给出空结果）
            LOG_ERROR("Semaphore: 分配唤醒凭证失败（内存吃紧），本次不挂起，按未拿到许可收尾");
            return false;
        }
        m_ticket->attach(handle);
        m_semaphore->linkAwaiter(*this);
        return true;
    }

    std::optional<Semaphore::Permit> Semaphore::Awaiter::await_resume()
    {
        // 账上是空的只有两种成因：等待期间信号量被析构（许可随之作废），或凭证分配失败没挂上队。
        // 两种都交回空结果，调用方按「没拿到许可」收尾——不猜、也不给一个指向已死对象的 Permit
        if (m_semaphore == nullptr || m_grantedCount == 0)
        {
            unlinkIfLinked();
            return std::nullopt;
        }
        const std::size_t grantedCount = std::exchange(m_grantedCount, 0);
        // 取走许可即离队：队列里只留在途请求，于是「还有额度可发」与「队里还排着人」这两件事互斥，
        // 后来的等待者不会因一个已经拿完许可、却还挂在队上的节点而永远排不到（许可留在队上时的
        // await_ready 判据只看队头存不存在，不看它是否已经消费过）
        unlinkIfLinked();
        return Permit{*m_semaphore, grantedCount};
    }
} // namespace AsynGyanis::Core
