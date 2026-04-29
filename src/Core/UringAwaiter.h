/**
 * @file UringAwaiter.h
 * @brief co_await io_uring 操作（填充 SQE → 挂起 → CQE 收割 → 恢复协程）
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_URINGAWAITER_H
#define CORE_URINGAWAITER_H

#include "Uring.h"

#include <coroutine>
#include <liburing.h>
#include <sys/socket.h>
#include <unordered_map>

namespace Core
{
    class UringAwaiter
    {
    public:
        enum class OperationType : uint8_t { Read, Write, Accept, Connect, Timeout };

        UringAwaiter(Uring &uring, const OperationType op, const int fd, void *buf, const unsigned nbytes, const off_t offset) :
            m_uring(uring), m_op(op), m_fd(fd), m_buf(buf), m_nbytes(nbytes), m_offset(offset)
        {
        }

        UringAwaiter(Uring &uring, const OperationType op, const int fd, sockaddr *addr, socklen_t *addrLen) :
            m_uring(uring), m_op(op), m_fd(fd), m_addr(addr), m_addrLen(addrLen)
        {
        }

        UringAwaiter(Uring &uring, const OperationType op, const int fd, const sockaddr *addr, const socklen_t addrLen) :
            m_uring(uring), m_op(op), m_fd(fd), m_addr(const_cast<sockaddr *>(addr)), m_connectAddrLen(addrLen)
        {
        }

        explicit UringAwaiter(Uring &uring, const uint64_t timeoutMs) :
            m_uring(uring), m_op(OperationType::Timeout), m_timeoutMs(timeoutMs)
        {
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle)
        {
            m_handle                    = handle;
            s_pending[handle.address()] = this;
            fillAndSubmit();
        }

        int await_resume()
        {
            s_pending.erase(m_handle.address());
            return m_result;
        }

        static void complete(void *key, const int result)
        {
            if (const auto it = s_pending.find(key); it != s_pending.end())
                it->second->m_result = result;
        }

    private:
        void fillAndSubmit() const
        {
            if (io_uring_sqe *sqe = getSqeForOp())
            {
                io_uring_sqe_set_data(sqe, m_handle.address());
                m_uring.submit();
            }
        }

        io_uring_sqe *getSqeForOp() const
        {
            switch (m_op)
            {
                case OperationType::Read:
                    return m_uring.sqeForRead(m_fd, m_buf, m_nbytes, m_offset);
                case OperationType::Write:
                    return m_uring.sqeForWrite(m_fd, static_cast<const void *>(m_buf), m_nbytes, m_offset);
                case OperationType::Accept:
                    return m_uring.sqeForAccept(m_fd, m_addr, m_addrLen);
                case OperationType::Connect:
                    return m_uring.sqeForConnect(m_fd, m_addr, m_connectAddrLen);
                case OperationType::Timeout:
                {
                    __kernel_timespec ts{};
                    ts.tv_sec  = static_cast<__kernel_time64_t>(m_timeoutMs / 1000);
                    ts.tv_nsec = static_cast<long>((m_timeoutMs % 1000) * 1000000);
                    return m_uring.sqeForTimeout(&ts);
                }
            }
            return nullptr;
        }

        Uring &                 m_uring;
        OperationType           m_op;
        int                     m_fd{-1};
        void *                  m_buf{nullptr};
        unsigned                m_nbytes{0};
        off_t                   m_offset{0};
        sockaddr *              m_addr{nullptr};
        socklen_t *             m_addrLen{nullptr};
        socklen_t               m_connectAddrLen{0};
        uint64_t                m_timeoutMs{0};
        int                     m_result{0};
        std::coroutine_handle<> m_handle;

        static thread_local std::unordered_map<void *, UringAwaiter *> s_pending;
    };

    inline UringAwaiter uringRead(Uring &uring, const int fd, void *buf, const unsigned nbytes, const off_t offset)
    {
        return UringAwaiter(uring, UringAwaiter::OperationType::Read, fd, buf, nbytes, offset);
    }

    inline UringAwaiter uringWrite(Uring &uring, const int fd, const void *buf, const unsigned nbytes, const off_t offset)
    {
        return UringAwaiter(uring, UringAwaiter::OperationType::Write, fd, const_cast<void *>(buf), nbytes, offset);
    }

    inline UringAwaiter uringAccept(Uring &uring, const int fd, sockaddr *addr, socklen_t *addrLen)
    {
        return UringAwaiter(uring, UringAwaiter::OperationType::Accept, fd, addr, addrLen);
    }

    inline UringAwaiter uringConnect(Uring &uring, const int fd, const sockaddr *addr, const socklen_t addrLen)
    {
        return UringAwaiter(uring, UringAwaiter::OperationType::Connect, fd, addr, addrLen);
    }

    inline UringAwaiter uringTimeout(Uring &uring, const uint64_t ms)
    {
        return UringAwaiter(uring, ms);
    }

}

#endif
