#include "IOUringContext.h"
#include "Exception.h"

#include <liburing.h>

namespace Base
{
    IOUringContext::IOUringContext(const unsigned entries, const unsigned flags)
    {
        if (const int ret = io_uring_queue_init(entries, &m_ring, flags); ret < 0)
        {
            throw SystemException("io_uring_queue_init failed", std::error_code(-ret, std::system_category()));
        }
        m_initialized = true;
    }

    IOUringContext::~IOUringContext()
    {
        destroy();
    }

    IOUringContext::IOUringContext(IOUringContext &&other) noexcept :
        m_ring(other.m_ring), m_initialized(other.m_initialized)
    {
        other.m_initialized = false;
    }

    IOUringContext &IOUringContext::operator=(IOUringContext &&other) noexcept
    {
        if (this != &other)
        {
            destroy();
            m_ring              = other.m_ring;
            m_initialized       = other.m_initialized;
            other.m_initialized = false;
        }
        return *this;
    }

    io_uring_sqe *IOUringContext::getSqe()
    {
        return io_uring_get_sqe(&m_ring);
    }

    int IOUringContext::submit()
    {
        return io_uring_submit(&m_ring);
    }

    int IOUringContext::submitAndWait(const unsigned waitNr)
    {
        return io_uring_submit_and_wait(&m_ring, waitNr);
    }

    io_uring_cqe *IOUringContext::waitCqe()
    {
        io_uring_cqe *cqe = nullptr;
        io_uring_wait_cqe(&m_ring, &cqe);
        return cqe;
    }

    io_uring_cqe *IOUringContext::peekCqe()
    {
        io_uring_cqe *cqe = nullptr;
        io_uring_peek_cqe(&m_ring, &cqe);
        return cqe;
    }

    void IOUringContext::cqeSeen(io_uring_cqe *cqe)
    {
        io_uring_cqe_seen(&m_ring, cqe);
    }

    int IOUringContext::fd() const noexcept
    {
        return m_ring.ring_fd;
    }

    unsigned IOUringContext::entries() const noexcept
    {
        return m_ring.sq.ring_entries;
    }

    io_uring *IOUringContext::nativeHandle() noexcept
    {
        return &m_ring;
    }

    void IOUringContext::destroy()
    {
        if (m_initialized)
        {
            io_uring_queue_exit(&m_ring);
            m_initialized = false;
        }
    }

}
