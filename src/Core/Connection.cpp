#include "Connection.h"
#include "InetAddress.h"

namespace Core
{
    Connection::Connection(AsyncSocket socket) :
        m_socket(std::move(socket))
    {
    }

    Connection::Connection(Connection &&other) noexcept :
        m_socket(std::move(other.m_socket)),
        m_cancelable(std::move(other.m_cancelable)),
        m_alive(other.m_alive.load(std::memory_order_acquire))
    {
    }

    Connection &Connection::operator=(Connection &&other) noexcept
    {
        if (this != &other)
        {
            m_socket     = std::move(other.m_socket);
            m_cancelable = std::move(other.m_cancelable);
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

}
