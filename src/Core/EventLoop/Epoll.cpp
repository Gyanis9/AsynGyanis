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
        // 缓冲容量固定、不再按负载翻倍：翻倍会把上一次 wait() 交出去的那个视图变成悬垂指针，
        // 而事件循环是**跨着 handleEvents()（它会同步恢复等待中的协程）持有那个视图**的——
        // 一旦恢复出去的协程再碰一次 wait()，回来时就是在已释放的内存上取 data.ptr 并派发。
        // 取满上限也不算丢事件：epoll 是水平触发，没取走的描述符仍在就绪名单里，
        // 本轮循环末尾的又一次 wait() 会立刻把它们再报一遍，因此不需要为「怕丢」而扩容量
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
