#include "Platform/IO/Socket.h"

#include "Platform/IO/FileDescriptor.h"

namespace AsynGyanis::Platform
{
    bool Socket::initialize() noexcept
    {
#if ASYN_PLATFORM_WIN32
        // Winsock 只需启动一次，static 初始化在 C++11 起保证线程安全
        static const bool kstarted = []
        {
            WSADATA socketData{};
            return ::WSAStartup(MAKEWORD(2, 2), &socketData) == 0;
        }();
        return kstarted;
#else
        return true;
#endif
    }

    void Socket::finalize() noexcept
    {
#if ASYN_PLATFORM_WIN32
        ::WSACleanup();
#endif
    }

    int Socket::accept(const int listenDescriptor, sockaddr *address, socklen_t *addressLength) noexcept
    {
#if ASYN_PLATFORM_WIN32
        const auto fileDescriptor = ::accept(listenDescriptor, address, addressLength);
        if (static_cast<int>(fileDescriptor) >= 0)
        {
            FileDescriptor::setNonBlocking(static_cast<int>(fileDescriptor));
        }
        return static_cast<int>(fileDescriptor);
#else
        return ::accept4(listenDescriptor, address, addressLength, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif
    }
} // namespace AsynGyanis::Platform
