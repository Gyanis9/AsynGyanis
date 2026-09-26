#include "Platform/IO/FileDescriptor.h"

#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

#include <cstdint>
#include <limits>

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief 单次读写的长度上界
         * @details Windows 的 recv/send 长度形参是 int，超过再强转就得到负数或回绕成另一个合法值；
         *          POSIX 的形参虽是 size_t，内核也在这一档之上截短或直接报 EINVAL。把界取成两侧共同的
         *          那个值，同一份超限请求在两平台上得到同一个「当场拒绝」的结果，而不是一侧拒绝、
         *          一侧悄悄少搬 bytes。
         */
        constexpr std::size_t kMaximumTransferLength = static_cast<std::size_t>(std::numeric_limits<int>::max());
    } // namespace

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

    bool FileDescriptor::markNonInheritable(const int fileDescriptor) noexcept
    {
        // 与读写同一条要求：拒掉无效描述符时要把原因留下，否则调用方查到的永远是上一次的错误码
        if (!isValid(fileDescriptor))
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return false;
        }
#if ASYN_PLATFORM_WIN32
        // Winsock 建出来的句柄默认带继承位，只能建好之后再取消（带 WSA_FLAG_NO_HANDLE_INHERIT 的
        // WSASocketW 在老系统上会让整个创建失败，不划算走那条路）
        return ::SetHandleInformation(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(fileDescriptor)), HANDLE_FLAG_INHERIT, 0) != 0;
#else
        const int flags = ::fcntl(fileDescriptor, F_GETFD);
        return flags >= 0 && ::fcntl(fileDescriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
#endif
    }

    ssize_t FileDescriptor::read(const int fileDescriptor, void *buffer, const std::size_t length) noexcept
    {
        // 无效描述符也要把错误码置上：本接口的失败语义是「-1，原因见 PlatformError」，不置就等于
        // 让调用方读到上一次调用留下的残值。与 Socket::writeVectored 同一套判据（无效描述符归参数非法）
        if (!isValid(fileDescriptor))
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }
        // 超限当场拒绝而不是静默少读：回绕后的长度交给底层，读回来的字节数与「对端关闭」同为 0，
        // 调用方无从分辨。错误码走 PlatformError 的通道，两侧的 errno 与平台错误码要一起置上，
        // 否则调用方读到的是上一次调用留下的残值
        if (length > kMaximumTransferLength)
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
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
        // 与读侧同一条要求：失败必须留下本次的错误码，而不是上一次的
        if (!isValid(fileDescriptor))
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }
        // 与读同一条界：这里静默截断更糟——少写了字节却回报成功，是最难排查的那种数据损坏
        if (length > kMaximumTransferLength)
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
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
        // 这一对是 EventNotifier 与 TimerFileDescriptor 的底座：留着继承位，子进程就替父进程持着
        // 唤醒通道的一端，父进程关掉自己的描述符也释放不掉那条通道
        static_cast<void>(markNonInheritable(readDescriptor));
        static_cast<void>(markNonInheritable(writeDescriptor));
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
