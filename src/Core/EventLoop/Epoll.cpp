#include "Core/EventLoop/Epoll.h"

#include "Base/Exception/SystemException.h"

#include <cerrno>

namespace AsynGyanis::Core
{
    Epoll::Epoll()
    {
        m_fileDescriptor = epoll_create1(EPOLL_CLOEXEC);
        if (!Platform::isEpollHandleValid(m_fileDescriptor))
        {
            // 系统调用失败交由 SystemException 承载：它会自己读 errno 并附上可读描述，
            // 不必在这里手工 strerror，也避免把平台差异（strerror_s / strerror_r）写进 Core
            throw Base::SystemException("epoll_create1 失败");
        }
        m_events.resize(kMaximumEventCount);
    }

    Epoll::~Epoll()
    {
        destroy();
    }

    Epoll::Epoll(Epoll &&other) noexcept :
        m_fileDescriptor(other.m_fileDescriptor), m_events(std::move(other.m_events))
    {
        other.m_fileDescriptor = Platform::kInvalidEpollHandle;
    }

    Epoll &Epoll::operator=(Epoll &&other) noexcept
    {
        if (this != &other)
        {
            destroy();
            m_fileDescriptor       = other.m_fileDescriptor;
            m_events               = std::move(other.m_events);
            other.m_fileDescriptor = Platform::kInvalidEpollHandle;
        }
        return *this;
    }

    bool Epoll::addFileDescriptor(const int fileDescriptor, const uint32_t events, void *const userData) const
    {
        epoll_event ev{};
        ev.events   = events;
        ev.data.ptr = userData;
        return epoll_ctl(m_fileDescriptor, EPOLL_CTL_ADD,
#ifdef _WIN32
                         static_cast<SOCKET>(fileDescriptor),
#else
                         fileDescriptor,
#endif
                         &ev) == 0;
    }

    bool Epoll::modFileDescriptor(const int fileDescriptor, const uint32_t events, void *const userData) const
    {
        epoll_event ev{};
        ev.events   = events;
        ev.data.ptr = userData;
        return epoll_ctl(m_fileDescriptor, EPOLL_CTL_MOD,
#ifdef _WIN32
                         static_cast<SOCKET>(fileDescriptor),
#else
                         fileDescriptor,
#endif
                         &ev) == 0;
    }

    bool Epoll::delFileDescriptor(const int fileDescriptor) const
    {
        return epoll_ctl(m_fileDescriptor, EPOLL_CTL_DEL,
#ifdef _WIN32
                         static_cast<SOCKET>(fileDescriptor),
#else
                         fileDescriptor,
#endif
                         nullptr) == 0;
    }

    bool Epoll::rearmFileDescriptor(const int fileDescriptor, const uint32_t events, void *const userData) const
    {
        epoll_event ev{};
        ev.events   = events | EPOLLONESHOT;
        ev.data.ptr = userData;
        if (epoll_ctl(m_fileDescriptor, EPOLL_CTL_MOD,
#ifdef _WIN32
                      static_cast<SOCKET>(fileDescriptor),
#else
                      fileDescriptor,
#endif
                      &ev) == 0)
            return true;
        // 仅在文件描述符尚未注册时才回退到 ADD
        if (errno == ENOENT)
            return epoll_ctl(m_fileDescriptor, EPOLL_CTL_ADD,
#ifdef _WIN32
                             static_cast<SOCKET>(fileDescriptor),
#else
                             fileDescriptor,
#endif
                             &ev) == 0;
        return false;
    }

    std::span<epoll_event> Epoll::wait(const int timeoutMs)
    {
        const int n = epoll_wait(m_fileDescriptor, m_events.data(), static_cast<int>(m_events.size()), timeoutMs);
        if (n < 0)
        {
            if (errno == EINTR)
                return {};
            // 交由 SystemException 读取 errno 并附上可读描述：既省掉手工 strerror，
            // 也把 strerror_s / strerror_r 这类平台差异挡在 Core 之外
            throw Base::SystemException("epoll_wait 失败");
        }
        // 防止高负载下丢失事件
        if (static_cast<size_t>(n) >= m_events.size() / 2)
        {
            m_events.resize(m_events.size() * 2);
        }
        return {m_events.data(), static_cast<size_t>(n)};
    }

    Platform::EpollHandle Epoll::fileDescriptor() const noexcept
    {
        return m_fileDescriptor;
    }

    void Epoll::destroy()
    {
        if (Platform::isEpollHandleValid(m_fileDescriptor))
        {
#ifdef _WIN32
            epoll_close(m_fileDescriptor);
#else
            close(m_fileDescriptor);
#endif
            m_fileDescriptor = Platform::kInvalidEpollHandle;
        }
    }

}
