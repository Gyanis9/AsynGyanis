#include "Uring.h"
#include "Base/Exception.h"

namespace Core
{
    Uring::Uring(Base::IOUringContext &context) :
        m_context(&context)
    {
    }

    bool Uring::registerEventFd(const int eventFd) const
    {
        return io_uring_register_eventfd(m_context->nativeHandle(), eventFd) == 0;
    }

    bool Uring::registerBuffers(const std::vector<iovec> &iovs) const
    {
        return io_uring_register_buffers(m_context->nativeHandle(), iovs.data(), static_cast<unsigned>(iovs.size())) == 0;
    }

    bool Uring::unregisterBuffers() const
    {
        return io_uring_unregister_buffers(m_context->nativeHandle()) == 0;
    }

    io_uring_sqe *Uring::sqeForRead(const int fd, void *buf, const unsigned nbytes, const off_t offset) const
    {
        io_uring_sqe *sqe = m_context->getSqe();
        if (!sqe)
            return nullptr;
        io_uring_prep_read(sqe, fd, buf, static_cast<__u32>(nbytes), static_cast<__u64>(offset));
        return sqe;
    }

    io_uring_sqe *Uring::sqeForWrite(const int fd, const void *buf, const unsigned nbytes, const off_t offset) const
    {
        io_uring_sqe *sqe = m_context->getSqe();
        if (!sqe)
            return nullptr;
        io_uring_prep_send(sqe, fd, buf, static_cast<__u32>(nbytes), MSG_NOSIGNAL);
        return sqe;
    }

    io_uring_sqe *Uring::sqeForAccept(const int fd, sockaddr *addr, socklen_t *addrLen) const
    {
        io_uring_sqe *sqe = m_context->getSqe();
        if (!sqe)
            return nullptr;
        io_uring_prep_accept(sqe, fd, addr, addrLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        return sqe;
    }

    io_uring_sqe *Uring::sqeForConnect(const int fd, const sockaddr *addr, const socklen_t addrLen) const
    {
        io_uring_sqe *sqe = m_context->getSqe();
        if (!sqe)
            return nullptr;
        io_uring_prep_connect(sqe, fd, addr, addrLen);
        return sqe;
    }

    io_uring_sqe *Uring::sqeForTimeout(__kernel_timespec *ts) const
    {
        io_uring_sqe *sqe = m_context->getSqe();
        if (!sqe)
            return nullptr;
        io_uring_prep_timeout(sqe, ts, 1, 0);
        return sqe;
    }

    io_uring_sqe *Uring::sqeForCancel(const uint64_t userData) const
    {
        io_uring_sqe *sqe = m_context->getSqe();
        if (!sqe)
            return nullptr;
        io_uring_prep_cancel(sqe, reinterpret_cast<void *>(static_cast<uintptr_t>(userData)), 0);
        return sqe;
    }

    io_uring_sqe *Uring::sqeForNop() const
    {
        io_uring_sqe *sqe = m_context->getSqe();
        if (!sqe)
            return nullptr;
        io_uring_prep_nop(sqe);
        return sqe;
    }

    int Uring::submit() const
    {
        return m_context->submit();
    }

    int Uring::submitAndWait(const unsigned waitNr) const
    {
        return m_context->submitAndWait(waitNr);
    }

    io_uring_cqe *Uring::peekCqe() const
    {
        return m_context->peekCqe();
    }

    io_uring_cqe *Uring::waitCqe() const
    {
        return m_context->waitCqe();
    }

    void Uring::cqeSeen(io_uring_cqe *cqe) const
    {
        m_context->cqeSeen(cqe);
    }

    unsigned Uring::entries() const noexcept
    {
        return m_context->entries();
    }

    int Uring::fd() const noexcept
    {
        return m_context->fd();
    }

    Base::IOUringContext &Uring::context() const noexcept
    {
        return *m_context;
    }

}
