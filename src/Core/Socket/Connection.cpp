#include "Core/Socket/Connection.h"
#include "Core/Socket/InetAddress.h"

namespace AsynGyanis::Core
{
    Connection::Connection(AsyncSocket socket) : m_socket(std::move(socket))
    {
    }

    Connection::Connection(Connection &&other) noexcept :
        m_socket(std::move(other.m_socket)), m_cancelable(std::move(other.m_cancelable)), m_alive(other.m_alive.load(std::memory_order_acquire)), m_busy(other.m_busy),
        m_idleDeadline(std::move(other.m_idleDeadline))
    {
    }

    Connection &Connection::operator=(Connection &&other) noexcept
    {
        if (this != &other)
        {
            m_socket       = std::move(other.m_socket);
            m_cancelable   = std::move(other.m_cancelable);
            m_idleDeadline = std::move(other.m_idleDeadline);
            m_busy         = other.m_busy;
            m_alive.store(other.m_alive.load(std::memory_order_acquire), std::memory_order_release);
        }
        return *this;
    }

    Task<> Connection::start()
    {
        co_return;
    }

    void Connection::close()
    {
        // 双重关闭防护：若已关闭则直接返回，防止重复 shutdown/close
        if (!m_alive.exchange(false, std::memory_order_acq_rel))
            return;
        [[maybe_unused]] auto _ = m_cancelable.requestStop();
        m_socket.close();
    }

    bool Connection::isAlive() const noexcept
    {
        return m_alive.load(std::memory_order_acquire);
    }

    AsyncSocket &Connection::socket() noexcept
    {
        return m_socket;
    }

    const AsyncSocket &Connection::socket() const noexcept
    {
        return m_socket;
    }

    Cancelable &Connection::cancelable() noexcept
    {
        return m_cancelable;
    }

    std::string Connection::remoteAddress() const
    {
        return m_socket.remoteAddress().toString();
    }

    std::string Connection::localAddress() const
    {
        return m_socket.localAddress().toString();
    }

    void Connection::refreshIdleDeadline(const std::chrono::milliseconds timeout) noexcept
    {
        // 非正数一律按「关闭本项保护」处理：设一个已经过去的截止时间会让清扫协程立刻关掉连接，
        // 与调用方传 0 想表达的「不限制」正好相反
        if (timeout <= std::chrono::milliseconds::zero())
        {
            m_idleDeadline.reset();
            return;
        }

        m_idleDeadline = std::chrono::steady_clock::now() + timeout;
    }

    void Connection::clearIdleDeadline() noexcept
    {
        m_idleDeadline.reset();
    }

    bool Connection::isIdleExpired(const std::chrono::steady_clock::time_point now) const noexcept
    {
        // 没有截止时间的连接一律不判超期：非 HTTP 会话从未刷过它，不该被空闲清扫误伤
        return m_idleDeadline.has_value() && now >= *m_idleDeadline;
    }

    void Connection::setBusy(const bool busy) noexcept
    {
        // 只被所属事件循环线程写：不引入原子量，也不与 isAlive() 那类跨线程状态耦合
        m_busy = busy;
    }

    bool Connection::isBusy() const noexcept
    {
        return m_busy;
    }

} // namespace AsynGyanis::Core
