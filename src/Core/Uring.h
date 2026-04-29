/**
 * @file Uring.h
 * @brief io_uring 增强 RAII 封装 — 注册 buffer/eventfd，SQE 工厂方法
 * @copyright Copyright (c) 2026
 */
#ifndef CORE_URING_H
#define CORE_URING_H

#include "Base/IOUringContext.h"

#include <liburing.h>
#include <sys/socket.h>

#include <vector>

namespace Core
{
    class Uring
    {
    public:
        explicit Uring(Base::IOUringContext &context);

        bool registerEventFd(int eventFd) const;
        bool registerBuffers(const std::vector<iovec> &iovs) const;
        bool unregisterBuffers() const;

        io_uring_sqe *sqeForRead(int fd, void *buf, unsigned nbytes, off_t offset) const;
        io_uring_sqe *sqeForWrite(int fd, const void *buf, unsigned nbytes, off_t offset) const;
        io_uring_sqe *sqeForAccept(int fd, sockaddr *addr, socklen_t *addrLen) const;
        io_uring_sqe *sqeForConnect(int fd, const sockaddr *addr, socklen_t addrLen) const;
        io_uring_sqe *sqeForTimeout(__kernel_timespec *ts) const;
        io_uring_sqe *sqeForCancel(uint64_t userData) const;
        io_uring_sqe *sqeForNop() const;

        int           submit() const;
        int           submitAndWait(unsigned waitNr = 1) const;
        io_uring_cqe *peekCqe() const;
        io_uring_cqe *waitCqe() const;
        void          cqeSeen(io_uring_cqe *cqe) const;

        [[nodiscard]] unsigned              entries() const noexcept;
        [[nodiscard]] int                   fd() const noexcept;
        [[nodiscard]] Base::IOUringContext &context() const noexcept;

    private:
        Base::IOUringContext *m_context;
    };

}

#endif
