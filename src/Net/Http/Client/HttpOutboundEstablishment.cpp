#include "Net/Http/Client/HttpOutboundEstablishment.h"

#include "Core/Coroutine/Scheduler.h"
#include "Core/EventLoop/EventLoop.h"

#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    bool HttpEstablishmentTable::tryBecomeLeader(const std::string &endpointKey)
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        // 「有这个键」就是在建连：领导者由 insert 成功的一方确认，不需要另记一个标志
        return m_endpoints.try_emplace(endpointKey).second;
    }

    bool HttpEstablishmentTable::attach(const std::string &endpointKey, Waiter &waiter) noexcept
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        const auto                        iterator = m_endpoints.find(endpointKey);
        if (iterator == m_endpoints.end())
        {
            // 领导者在这一刻与挂链之间已经结算完了：不挂一个没人会唤醒的节点，让调用方直接往下走
            return false;
        }

        Endpoint &endpoint = iterator->second;
        Waiter   &sentinel = endpoint.sentinel;
        Waiter   *tail     = sentinel.previous;
        waiter.next        = &sentinel;
        waiter.previous    = tail;
        tail->next         = &waiter;
        sentinel.previous  = &waiter;
        return true;
    }

    void HttpEstablishmentTable::detach(Waiter &waiter) noexcept
    {
        // 摘链改的是别人的指针，因此必须和 attach/settle 互斥：等待者那一侧（协程帧被销毁、
        // 异常展开）走的都是这条带锁的路
        const std::lock_guard<std::mutex> guard(m_mutex);
        unlink(waiter);
    }

    void HttpEstablishmentTable::unlink(Waiter &waiter) noexcept
    {
        // 只动节点自己：前后驱互指，摘掉就是两条赋值。previous 为空即「没挂着」，
        // 于是 settle 已经摘过、或从未挂上的节点在这里都是空操作
        if (waiter.previous == nullptr)
        {
            return;
        }
        waiter.previous->next = waiter.next;
        waiter.next->previous = waiter.previous;
        waiter.previous       = nullptr;
        waiter.next           = nullptr;
    }

    void HttpEstablishmentTable::settle(const std::string &endpointKey)
    {
        std::vector<std::pair<Core::EventLoop *, std::coroutine_handle<>>> toWake;
        {
            const std::lock_guard<std::mutex> guard(m_mutex);
            const auto                        iterator = m_endpoints.find(endpointKey);
            if (iterator == m_endpoints.end())
            {
                return;
            }

            Waiter &sentinel = iterator->second.sentinel;
            while (sentinel.next != &sentinel)
            {
                // 先摘再收：唤醒之后那个节点可能随着协程帧一起消失，摸它就是释放后使用
                Waiter *const waiter = sentinel.next;
                unlink(*waiter);
                toWake.emplace_back(waiter->loop, waiter->handle);
            }
            m_endpoints.erase(iterator);
        }

        // 锁外投：scheduleRemote 要碰那条循环自己的状态，抱着本表的锁进别人的领地不值当
        for (auto &[loop, handle]: toWake)
        {
            loop->scheduler().scheduleRemote(handle);
        }
    }

    HttpEstablishmentAwait::HttpEstablishmentAwait(std::shared_ptr<HttpEstablishmentTable> table, std::string endpointKey, Core::EventLoop &loop) noexcept :
        m_table(std::move(table)), m_endpointKey(std::move(endpointKey)), m_loop(loop)
    {
    }

    HttpEstablishmentAwait::~HttpEstablishmentAwait() noexcept
    {
        if (m_table != nullptr)
        {
            m_table->detach(m_waiter);
        }
    }

    bool HttpEstablishmentAwait::await_ready() const noexcept
    {
        // 挂链之前无法知道端点会不会在下一秒结算，因此这里恒假：判定交给 await_suspend
        return false;
    }

    bool HttpEstablishmentAwait::await_suspend(const std::coroutine_handle<> handle) noexcept
    {
        m_waiter.handle = handle;
        m_waiter.loop   = &m_loop;
        return m_table->attach(m_endpointKey, m_waiter);
    }

    void HttpEstablishmentAwait::await_resume() const noexcept
    {
        // 结论在池里，这里不交东西
    }
} // namespace AsynGyanis::Net
