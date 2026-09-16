#include "Core/EventLoop/IoWatcher.h"

#include "Base/Exception/LogicException.h"
#include "Base/Exception/SystemException.h"
#include "Core/EventLoop/EventLoop.h"

namespace AsynGyanis::Core
{
    IoWatcher::Awaiter::Awaiter(IoWatcher &watcher, const std::uint32_t event) noexcept :
        m_watcher(&watcher), m_event(event)
    {
    }

    IoWatcher::Awaiter::~Awaiter()
    {
        // 协程帧可能在等待期间被销毁（取消、异常展开）：此时必须顺手摘除登记，
        // 否则注册对象里会留下指向本等待器的悬空指针，事件到达时就会写它
        if (m_isAttached)
        {
            m_watcher->detachWaiter(m_event);
        }
    }

    bool IoWatcher::Awaiter::await_ready() noexcept
    {
        // 上一次武装上报时就绪、却没人领走的那一份：直接完成，不挂起也不武装。
        // 这一步同时把标记取走，避免它残留下来让后续等待空转
        if (!m_watcher->consumeReady(m_event))
        {
            return false;
        }
        m_isReady = true;
        return true;
    }

    bool IoWatcher::Awaiter::await_suspend(const std::coroutine_handle<> handle)
    {
        if (!m_watcher->attachWaiter(m_event, handle, *this))
        {
            // 注册已失效或关注位武装不上：不挂起，直接以「未就绪」结束等待
            return false;
        }
        m_isAttached = true;
        return true;
    }

    bool IoWatcher::Awaiter::await_resume() const noexcept
    {
        return m_isReady;
    }

    void IoWatcher::Awaiter::markReady() noexcept
    {
        m_isReady = true;
    }

    void IoWatcher::Awaiter::markDetached() noexcept
    {
        m_isAttached = false;
    }

    IoWatcher::IoWatcher(EventLoop &loop, const int fileDescriptor) :
        m_loop(&loop), m_fileDescriptor(fileDescriptor)
    {
        // 描述符无效时保持「未注册」状态：持有空描述符的对象（占位、已关闭）因此可以统一处理，
        // 其等待会立刻以「未就绪」结束，而不是抛异常或永久挂起
        if (m_fileDescriptor < 0)
        {
            return;
        }

        // 注册只建立归属关系，顺手带上可读方向的关注：可读要等真有数据才可能就绪，
        // 因此最多产生一次无害的探测事件；而可写几乎长期为真，提前武装它只会在没人等待时
        // 交付一份陈旧的可写就绪（例如套接字刚创建、还没 connect 就被判成「可写」）。
        // 水平触发下这次关注会一直有效，直到本类显式改掩码（见 armEvents）
        if (!m_loop->epoll().addFileDescriptor(m_fileDescriptor, EPOLLIN, this))
        {
            throw Base::SystemException("把文件描述符注册到 epoll 失败（该描述符可能已被另一个 "
                                        "IoWatcher 注册，或不是有效的描述符）");
        }
        m_isRegistered = true;
        m_armedEvents  = EPOLLIN;
    }

    IoWatcher::~IoWatcher()
    {
        // 先把等待者摘下来再反注册：本对象正在析构，不能让它们继续以为自己还挂在 epoll 上
        const std::coroutine_handle<> readWaiter  = std::exchange(m_readWaiter.handle, nullptr);
        const std::coroutine_handle<> writeWaiter = std::exchange(m_writeWaiter.handle, nullptr);
        // 关键：必须告诉等待器「你已不在我的登记槽里」。否则它被恢复、析构时会反过来调用
        // 本对象去摘除登记——而本对象此刻正在析构，那就是释放后使用（ASan 实测抓到过）
        if (m_readWaiter.awaiter != nullptr)
        {
            m_readWaiter.awaiter->markDetached();
            m_readWaiter.awaiter = nullptr;
        }
        if (m_writeWaiter.awaiter != nullptr)
        {
            m_writeWaiter.awaiter->markDetached();
            m_writeWaiter.awaiter = nullptr;
        }
        m_isRegistered = false;
        m_armedEvents  = 0;

        if (m_fileDescriptor >= 0)
        {
            [[maybe_unused]] auto _ = m_loop->epoll().delFileDescriptor(m_fileDescriptor);
        }

        // 唤醒仍挂在等待器上的协程：不能让它永远等一个再也不会到来的事件
        //（关闭描述符不会让 epoll 唤醒挂在它上面的等待者，这类等待只能由本类自己收尾）。
        // 用 scheduleRemote 而不是 schedule：销毁本对象的可能是**另一个线程**
        //（外部调用 stop()/close() 关闭监听套接字或连接就是这条路径），
        // 而 schedule 既不是线程安全的，也不会唤醒正睡在 epoll_wait 上的事件循环；
        // scheduleRemote 同时解决这两点。这里也不是就地恢复——本对象还在析构中，
        // 就地恢复会让协程在析构未完成时回来访问成员
        if (readWaiter)
        {
            m_loop->scheduler().scheduleRemote(readWaiter);
        }
        if (writeWaiter)
        {
            m_loop->scheduler().scheduleRemote(writeWaiter);
        }
    }

    bool IoWatcher::isValid() const noexcept
    {
        return m_isRegistered;
    }

    int IoWatcher::fileDescriptor() const noexcept
    {
        return m_fileDescriptor;
    }

    IoWatcher::Awaiter IoWatcher::waitReadable() noexcept
    {
        return Awaiter(*this, EPOLLIN);
    }

    IoWatcher::Awaiter IoWatcher::waitWritable() noexcept
    {
        return Awaiter(*this, EPOLLOUT);
    }

    void IoWatcher::handleEvents(const std::uint32_t events) noexcept
    {
        // 水平触发：这次上报不消耗关注位（不再有 ONESHOT 的一发即消），账本因此保持不动。
        // 只有「没有人等的方向」必须收掉——内核会一轮一轮重复上报同一个就绪，
        // 收不掉就是 epoll_wait 全速空转
        // 错误与挂断同时算作可读与可写：让上层的 recv/send 自己去拿真实错误。
        // 在这里吞掉它们会把「对端已关闭」变成一次静默的无事发生，而读侧正需要靠
        // 那一次可读把 recv 走到 0（EOF）
        const bool isReadable = (events & (EPOLLIN | EPOLLERR | EPOLLHUP)) != 0;
        const bool isWritable = (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0;

        // 就绪要么当场交给等待者、要么留给下一次等待，二者只能其一：
        // 两边都给会让「交出去的那次等待」之后还残留一个标记，后续等待遂空转重试。
        // 先记下「本次上报时谁在等」：掩码收敛要用它区分「刚被唤醒」与「本来就没人等」
        const bool hadReadWaiter  = m_readWaiter.handle != nullptr;
        const bool hadWriteWaiter = m_writeWaiter.handle != nullptr;
        std::coroutine_handle<> resumableRead =
            isReadable ? takeWaiter(EPOLLIN, true) : std::coroutine_handle<>();
        std::coroutine_handle<> resumableWrite =
            isWritable ? takeWaiter(EPOLLOUT, true) : std::coroutine_handle<>();

        if (isReadable && !resumableRead)
        {
            m_readyEvents |= EPOLLIN;
        }
        if (isWritable && !resumableWrite)
        {
            m_readyEvents |= EPOLLOUT;
        }

        // 掩码收敛到「谁在等」的形状，但刚被唤醒的方向保持武装：它多半马上会再次等待
        //（keep-alive 的读写循环），保持武装就省下了「先收掉、再武装」两次 epoll_ctl。
        // 而「本次上报时本来就没有等待者」的方向必须收掉——它没有任何人会领走后续上报，
        // 尤其可写几乎长期为真，留着就是 epoll_wait 全速空转。
        // 必须在恢复协程之前做：恢复之后本对象可能已被销毁
        std::uint32_t wanted =
                (m_readWaiter.handle ? EPOLLIN : 0U) | (m_writeWaiter.handle ? EPOLLOUT : 0U);
        if (hadReadWaiter && isReadable)
        {
            wanted |= EPOLLIN;
        }
        if (hadWriteWaiter && isWritable)
        {
            wanted |= EPOLLOUT;
        }
        if (wanted != m_armedEvents && !armEvents(wanted))
        {
            if (!resumableRead)
            {
                resumableRead = takeWaiter(EPOLLIN, false);
            }
            if (!resumableWrite)
            {
                resumableWrite = takeWaiter(EPOLLOUT, false);
            }
        }

        // 恢复放在最后：被恢复的代码可能立刻销毁本对象（读到对端关闭后关闭连接），
        // 那之后对成员的每一次访问都是释放后使用。
        //
        // 写侧必须先于读侧：两侧的等待者可能属于不同的协程帧，而读侧的协议收口会把自己那条
        // 协程链（含仍挂在写等待上的协程帧）一并销毁——先走读侧，写侧那个刚被取出的句柄就指向
        // 已析构的帧，再 resume 是释放后使用
        if (resumableWrite)
        {
            resumableWrite.resume();
        }
        if (resumableRead)
        {
            resumableRead.resume();
        }
    }

    bool IoWatcher::attachWaiter(const std::uint32_t event, const std::coroutine_handle<> handle, Awaiter &awaiter)
    {
        if (!m_isRegistered)
        {
            // 已关闭或从未注册成功：不挂起，让等待方立刻收尾
            return false;
        }

        WaiterSlot &slot = slotFor(event);
        if (slot.handle)
        {
            // 同一方向并发等待几乎总是用法错误（例如同一连接起了两个读协程）。
            // 静默让其中一个永远等不到，比当场报错危险得多
            throw Base::LogicException("同一文件描述符的同一方向上已有协程在等待："
                                       "请确保一个方向同时只有一个等待者");
        }

        slot.handle  = handle;
        slot.awaiter = &awaiter;

        // 关注位只在有人等的时候武装：这才是「内核在盯着什么」与「现在谁在等」一致的含义
        const std::uint32_t needed = (m_readWaiter.handle ? EPOLLIN : 0U) | (m_writeWaiter.handle ? EPOLLOUT : 0U);
        if (!armEvents(needed))
        {
            slot.handle  = nullptr;
            slot.awaiter = nullptr;
            return false;
        }
        return true;
    }

    std::coroutine_handle<> IoWatcher::takeWaiter(const std::uint32_t event, const bool isReady) noexcept
    {
        WaiterSlot &                 slot   = slotFor(event);
        const std::coroutine_handle<> handle = std::exchange(slot.handle, nullptr);
        if (handle && slot.awaiter != nullptr)
        {
            // 先写回结果与「已摘除」，再恢复协程；此后再不访问等待器指针
            //（它随协程帧，可能已被销毁）。摘除标记是必需的：否则等待器析构时会回来调本对象
            if (isReady)
            {
                slot.awaiter->markReady();
            }
            slot.awaiter->markDetached();
            slot.awaiter = nullptr;
        }
        return handle;
    }

    void IoWatcher::detachWaiter(const std::uint32_t event) noexcept
    {
        WaiterSlot &slot = slotFor(event);
        slot.handle      = nullptr;
        slot.awaiter     = nullptr;
    }

    bool IoWatcher::armEvents(const std::uint32_t events) noexcept
    {
        if (events == m_armedEvents)
        {
            // 内核状态已经就是要的形状：不动它。水平触发下这一步就是省下来的那次 epoll_ctl——
            // 一次就绪上报不再需要重新武装，只有「谁在等」发生变化时才改掩码
            return true;
        }

        // 掩码为 0 同样要落一次 mod：水平触发下「不关注」必须显式写进内核，
        // 否则长期为真的关注位（如可写）会在没人等待时被反复上报
        if (!m_loop->epoll().modFileDescriptor(m_fileDescriptor, events, this))
        {
            // 失败时内核掩码停在原样：账本必须记成「未知」，不能记成「已武装 0 位」——
            // 后者会让下一次 armEvents(0) 命中「已经是要的形状」的短路，从此再也不下发清零，
            // 而水平触发下那套陈旧关注位会一轮一轮上报（epoll_wait 空转）
            m_armedEvents = kUnknownArmedEvents;
            return false;
        }
        m_armedEvents = events;
        return true;
    }

    bool IoWatcher::consumeReady(const std::uint32_t event) noexcept
    {
        if ((m_readyEvents & event) == 0)
        {
            return false;
        }
        m_readyEvents &= ~event;
        return true;
    }

    IoWatcher::WaiterSlot &IoWatcher::slotFor(const std::uint32_t event) noexcept
    {
        return (event & EPOLLIN) != 0 ? m_readWaiter : m_writeWaiter;
    }

} // namespace AsynGyanis::Core
