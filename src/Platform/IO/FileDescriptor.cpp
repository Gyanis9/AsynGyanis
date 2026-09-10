#include "Platform/IO/FileDescriptor.h"

#include "Platform/IO/Socket.h"

namespace AsynGyanis::Platform
{
    bool FileDescriptor::setNonBlocking(const int fileDescriptor) noexcept
    {
#if ASYN_PLATFORM_WIN32
        u_long mode = 1;
        return ::ioctlsocket(fileDescriptor, FIONBIO, &mode) == 0;
#else
        const int flags = ::fcntl(fileDescriptor, F_GETFL, 0);
        if (flags < 0)
        {
            return false;
        }
        return ::fcntl(fileDescriptor, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    ssize_t FileDescriptor::read(const int fileDescriptor, void *buffer, const std::size_t length) noexcept
    {
        if (!isValid(fileDescriptor))
        {
            return -1;
        }
#if ASYN_PLATFORM_WIN32
        return ::recv(fileDescriptor, static_cast<char *>(buffer), static_cast<int>(length), 0);
#else
        return ::read(fileDescriptor, buffer, length);
#endif
    }

    ssize_t FileDescriptor::write(const int fileDescriptor, const void *buffer, const std::size_t length) noexcept
    {
        if (!isValid(fileDescriptor))
        {
            return -1;
        }
#if ASYN_PLATFORM_WIN32
        return ::send(fileDescriptor, static_cast<const char *>(buffer), static_cast<int>(length), 0);
#else
        return ::write(fileDescriptor, buffer, length);
#endif
    }

    int FileDescriptor::close(const int fileDescriptor) noexcept
    {
        if (!isValid(fileDescriptor))
        {
            return 0;
        }
#if ASYN_PLATFORM_WIN32
        return ::closesocket(fileDescriptor);
#else
        return ::close(fileDescriptor);
#endif
    }

    bool FileDescriptor::createPair(int &readDescriptor, int &writeDescriptor) noexcept
    {
        readDescriptor  = kInvalid;
        writeDescriptor = kInvalid;

        // Windows 需要完成 Winsock 初始化才能创建 socket
        Socket::initialize();

#if ASYN_PLATFORM_WIN32
        // Windows 无 socketpair，用 loopback TCP 的 监听-连接-接受 构造等价描述符对
        const int listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener < 0)
        {
            return false;
        }

        BOOL reuseAddress = TRUE;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuseAddress), sizeof(reuseAddress));

        sockaddr_in loopbackAddress{};
        loopbackAddress.sin_family      = AF_INET;
        loopbackAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        loopbackAddress.sin_port        = 0;
        if (::bind(listener, reinterpret_cast<sockaddr *>(&loopbackAddress), sizeof(loopbackAddress)) < 0)
        {
            close(listener);
            return false;
        }

        socklen_t addressLength = sizeof(loopbackAddress);
        if (::getsockname(listener, reinterpret_cast<sockaddr *>(&loopbackAddress), &addressLength) < 0)
        {
            close(listener);
            return false;
        }

        if (::listen(listener, 1) < 0)
        {
            close(listener);
            return false;
        }

        const int client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client < 0)
        {
            close(listener);
            return false;
        }

        if (::connect(client, reinterpret_cast<sockaddr *>(&loopbackAddress), sizeof(loopbackAddress)) < 0)
        {
            close(listener);
            close(client);
            return false;
        }

        const int server = ::accept(listener, nullptr, nullptr);
        close(listener);
        if (server < 0)
        {
            close(client);
            return false;
        }

        readDescriptor  = server;
        writeDescriptor = client;
        if (!setNonBlocking(readDescriptor) || !setNonBlocking(writeDescriptor))
        {
            close(readDescriptor);
            close(writeDescriptor);
            readDescriptor  = kInvalid;
            writeDescriptor = kInvalid;
            return false;
        }
        return true;
#else
        const int descriptors[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors) != 0)
        {
            return false;
        }
        readDescriptor  = descriptors[0];
        writeDescriptor = descriptors[1];
        if (!setNonBlocking(readDescriptor) || !setNonBlocking(writeDescriptor))
        {
            close(readDescriptor);
            close(writeDescriptor);
            readDescriptor  = kInvalid;
            writeDescriptor = kInvalid;
            return false;
        }
        return true;
#endif
    }
} // namespace AsynGyanis::Platform
