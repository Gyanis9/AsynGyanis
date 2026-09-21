#include "Platform/IO/FileDescriptor.h"

#include "Platform/IO/Socket.h"

#include <limits>

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
        // recv/send 的长度形参是 int：超过上界时强转会得到负数或回绕成另一个合法值，
        // 而回绕后的结果与「对端关闭/暂无数据」同为 0，调用方无从分辨。与 Socket 的向量发送同一口径——
        // 宁可明确失败，也不静默少读。同时把错误码置上，否则调用方读到的是上一次调用留下的残值
        if (length > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            ::WSASetLastError(WSAEMSGSIZE);
            return -1;
        }
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
        // 同上：这里静默截断会更糟——少写了字节却回报成功，是最难排查的那种数据损坏
        if (length > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            ::WSASetLastError(WSAEMSGSIZE);
            return -1;
        }
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
        // 这里只申请引用而不配对 finalize()：返回的描述符在使用期间必须依赖 Winsock 保持初始化
        Socket::initialize();

#if ASYN_PLATFORM_WIN32
        // Windows 无 socketpair，用 loopback TCP 的 监听-连接-接受 构造等价描述符对
        const auto listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener < 0)
        {
            return false;
        }

        constexpr BOOL kreuseAddress = TRUE;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&kreuseAddress), sizeof(kreuseAddress));

        sockaddr_in loopbackAddress{};
        loopbackAddress.sin_family      = AF_INET;
        loopbackAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        loopbackAddress.sin_port        = 0;
        if (::bind(listener, reinterpret_cast<sockaddr *>(&loopbackAddress), sizeof(loopbackAddress)) < 0)
        {
            close(static_cast<int>(listener));
            return false;
        }

        socklen_t addressLength = sizeof(loopbackAddress);
        if (::getsockname(listener, reinterpret_cast<sockaddr *>(&loopbackAddress), &addressLength) < 0)
        {
            close(static_cast<int>(listener));
            return false;
        }

        if (::listen(listener, 1) < 0)
        {
            close(static_cast<int>(listener));
            return false;
        }

        const auto client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client < 0)
        {
            close(static_cast<int>(listener));
            return false;
        }

        if (::connect(client, reinterpret_cast<sockaddr *>(&loopbackAddress), sizeof(loopbackAddress)) < 0)
        {
            close(static_cast<int>(listener));
            close(static_cast<int>(client));
            return false;
        }

        const auto server = ::accept(listener, nullptr, nullptr);
        close(static_cast<int>(listener));
        if (server < 0)
        {
            close(static_cast<int>(client));
            return false;
        }

        readDescriptor  = static_cast<int>(server);
        writeDescriptor = static_cast<int>(client);
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
        // socketpair 会向该数组写入两个描述符
        int descriptors[2] = {-1, -1};
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
