#include "Platform/IO/Socket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <limits>
#include <mutex>

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief Winsock 初始化引用计数状态
         *
         * @details WSAStartup 与 WSACleanup 必须严格配对，因此不能只记录「是否启动过」。
         *          多个持有网络资源的对象（如逐个析构的 IoContext）各自成对调用时，
         *          只有最后一个引用释放才允许真正清理，否则仍存活的 socket 会失去 Winsock 支撑。
         */
        struct WinsockReferenceCount
        {
            std::mutex mutex;                    ///< 保护 initializeCount，跨线程启停时串行化
            int        initializeCount = 0;      ///< 当前生效的 WSAStartup 引用数
        };

        /**
         * @brief 取得进程级 Winsock 引用计数状态
         * @return WinsockReferenceCount& 全局唯一的状态对象
         */
        WinsockReferenceCount &winsockReferenceCount()
        {
            // 函数内 static 由 C++11 起保证线程安全的惰性初始化
            static WinsockReferenceCount state;
            return state;
        }

        /**
         * @brief 设置一个 int 尺寸的套接字选项
         * @param descriptor 目标套接字描述符
         * @param optionLevel 选项层级，如 SOL_SOCKET / IPPROTO_TCP / IPPROTO_IPV6
         * @param optionName 选项名
         * @param optionValue 选项值
         * @return true 设置成功
         */
        bool setIntegerOption(const int descriptor, const int optionLevel, const int optionName, const int optionValue) noexcept
        {
            // Windows 的值参数是 const char*，POSIX 是 const void*：
            // 统一转成 const char* 对两边都成立，业务层因此不必再写平台分支
            return ::setsockopt(descriptor, optionLevel, optionName, reinterpret_cast<const char *>(&optionValue), static_cast<socklen_t>(sizeof(optionValue))) == 0;
        }
    }

    bool Socket::initialize() noexcept
    {
#if ASYN_PLATFORM_WIN32
        auto                            &state = winsockReferenceCount();
        const std::lock_guard<std::mutex> lock(state.mutex);
        // 仅首个引用真正启动 Winsock，后续调用累加计数即可
        if (state.initializeCount == 0)
        {
            WSADATA socketData{};
            if (::WSAStartup(MAKEWORD(2, 2), &socketData) != 0)
            {
                return false;
            }
        }
        ++state.initializeCount;
        return true;
#else
        return true;
#endif
    }

    void Socket::finalize() noexcept
    {
#if ASYN_PLATFORM_WIN32
        auto                            &state = winsockReferenceCount();
        const std::lock_guard<std::mutex> lock(state.mutex);
        // 计数归零才清理；多余的 finalize 调用直接忽略，避免把计数减成负数
        if (state.initializeCount > 0 && --state.initializeCount == 0)
        {
            ::WSACleanup();
        }
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

    bool Socket::setReuseAddress(const int descriptor) noexcept
    {
        return setIntegerOption(descriptor, SOL_SOCKET, SO_REUSEADDR, 1);
    }

    bool Socket::setReusePort(const int descriptor) noexcept
    {
#ifdef SO_REUSEPORT
        // 用特性宏而不是操作系统宏判定：可用性取决于内核版本，不是取决于厂商
        return setIntegerOption(descriptor, SOL_SOCKET, SO_REUSEPORT, 1);
#else
        // Windows 没有该选项，返回 false 让调用方按「不支持」降级而不是当作失败
        (void) descriptor;
        return false;
#endif
    }

    bool Socket::setNoDelay(const int descriptor) noexcept
    {
        return setIntegerOption(descriptor, IPPROTO_TCP, TCP_NODELAY, 1);
    }

    bool Socket::setSendBufferSize(const int descriptor, const int byteCount) noexcept
    {
        if (byteCount <= 0)
        {
            // 非正值没有「按上限扩容」的语义，直接拒绝而不是把含糊的取值交给内核
            return false;
        }
        return setIntegerOption(descriptor, SOL_SOCKET, SO_SNDBUF, byteCount);
    }

    bool Socket::setReceiveBufferSize(const int descriptor, const int byteCount) noexcept
    {
        if (byteCount <= 0)
        {
            return false;
        }
        return setIntegerOption(descriptor, SOL_SOCKET, SO_RCVBUF, byteCount);
    }

    bool Socket::setDeferAccept(const int descriptor, const int seconds) noexcept
    {
#ifdef TCP_DEFER_ACCEPT
        if (seconds < 0)
        {
            return false;
        }
        return setIntegerOption(descriptor, IPPROTO_TCP, TCP_DEFER_ACCEPT, seconds);
#else
        // Windows 没有该选项，返回 false 让调用方按「不支持」降级而不是当作失败
        (void) descriptor;
        (void) seconds;
        return false;
#endif
    }

    bool Socket::setIpv6Only(const int descriptor, const bool isOnlyV6) noexcept
    {
        // 布尔值在两个平台上都按 int 尺寸传递，写成 0/1 避免 sizeof(bool) 歧义
        return setIntegerOption(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, isOnlyV6 ? 1 : 0);
    }

    int Socket::takePendingError(const int descriptor) noexcept
    {
        int       pendingError = 0;
        socklen_t optionLength = static_cast<socklen_t>(sizeof(pendingError));

        // Windows 的值参数是 char*、POSIX 是 void*，统一转 char* 两边都能接受，
        // 因此这里同样不需要平台分支
        if (::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&pendingError), &optionLength) != 0)
        {
            // 连 SO_ERROR 都读不到说明描述符已经失效：如实交回该错误，
            // 而不是返回 0 让调用方误判为「连接已建立」
            return PlatformError::lastSocketErrorCode();
        }
        return pendingError;
    }

    ssize_t Socket::writeVectored(const int descriptor, const WriteBuffer *const buffers, const std::size_t bufferCount) noexcept
    {
        // 段数与长度先按平台上限校验：这两处超限都必须当场失败。
        // 段数超限静默拆分会让「一次系统调用」的收益悄悄消失；单段长度被截断更糟——
        // 线上字节流会缺一截，而返回值看起来一切正常
        if (!FileDescriptor::isValid(descriptor) || buffers == nullptr || bufferCount == 0 || bufferCount > kMaximumVectorCount)
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return -1;
        }

#if ASYN_PLATFORM_WIN32
        WSABUF windowsBuffers[kMaximumVectorCount]{};
        for (std::size_t index = 0; index < bufferCount; ++index)
        {
            if (!buffers[index].data && buffers[index].length != 0)
            {
                PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
                return -1;
            }
            if (buffers[index].length > static_cast<std::size_t>(std::numeric_limits<ULONG>::max()))
            {
                PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
                return -1;
            }
            windowsBuffers[index].buf = static_cast<char *>(const_cast<void *>(buffers[index].data));
            windowsBuffers[index].len = static_cast<ULONG>(buffers[index].length);
        }

        DWORD sentLength = 0;
        if (::WSASend(static_cast<SOCKET>(descriptor), windowsBuffers, static_cast<DWORD>(bufferCount), &sentLength, 0, nullptr, nullptr) == SOCKET_ERROR)
        {
            return -1;
        }
        return static_cast<ssize_t>(sentLength);
#else
        iovec vectors[kMaximumVectorCount]{};
        for (std::size_t index = 0; index < bufferCount; ++index)
        {
            vectors[index].iov_base = const_cast<void *>(buffers[index].data);
            vectors[index].iov_len  = buffers[index].length;
        }

        // 用 sendmsg 而不是 writev：只有它带 MSG_NOSIGNAL，对端已关闭时不会把进程打死
        msghdr message{};
        message.msg_iov    = vectors;
        message.msg_iovlen = bufferCount;
        return ::sendmsg(descriptor, &message, MSG_NOSIGNAL);
#endif
    }
} // namespace AsynGyanis::Platform
