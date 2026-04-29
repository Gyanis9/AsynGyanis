/**
 * @file Connection.h
 * @brief TCP connection base class with cooperative cancellation support
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_CONNECTION_H
#define CORE_CONNECTION_H

#include "AsyncSocket.h"
#include "Cancelable.h"
#include "Task.h"

#include <atomic>
#include <string>

namespace Core
{
    class EventLoop;

    class Connection
    {
    public:
        Connection(EventLoop &loop, AsyncSocket socket);

        virtual ~Connection() = default;

        Connection(const Connection &)            = delete;
        Connection &operator=(const Connection &) = delete;
        Connection(Connection &&) noexcept;
        Connection &operator=(Connection &&) noexcept;

        virtual Task<>     start();
        void               close();
        [[nodiscard]] bool isAlive() const noexcept;

        [[nodiscard]] AsyncSocket &socket() noexcept;
        [[nodiscard]] Cancelable & cancelable() noexcept;
        [[nodiscard]] std::string  remoteAddress() const;
        [[nodiscard]] std::string  localAddress() const;

    private:
        AsyncSocket       m_socket;
        Cancelable        m_cancelable;
        std::atomic<bool> m_alive{true};
    };

}

#endif