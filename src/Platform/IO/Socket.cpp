#include "Platform/IO/Socket.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <csignal>
#include <mutex>
#include <system_error>

namespace AsynGyanis::Platform
{
    namespace
    {
        /// 无效描述符统一在这里拦下：Windows 的 SOCKET 是无符号的，-1 直接传进系统调用会触发
        /// 有符号/无符号转换告警，且回报的错误码不如「不是套接字」准确
        bool rejectInvalidDescriptor(const int descriptor) noexcept
        {
            if (descriptor >= 0)
            {
                return false;
            }
            PlatformError::setLastErrorCode(PlatformError::kNotASocket);
            return true;
        }
    } // namespace

    namespace
    {
#if ASYN_PLATFORM_WIN32
        /**
         * @brief Winsock 初始化引用计数状态
         *
         * @details WSAStartup 与 WSACleanup 必须严格配对，因此不能只记录「是否启动过」。
         *          多个持有网络资源的对象（如逐个析构的 IoContext）各自成对调用时，
         *          只有最后一个引用释放才允许真正清理，否则仍存活的 socket 会失去 Winsock 支撑。
         */
        struct WinsockReferenceCount
        {
            std::mutex mutex;               ///< 保护 initializeCount，跨线程启停时串行化
            int        initializeCount = 0; ///< 当前生效的 WSAStartup 引用数
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
#endif
    } // namespace

    bool Socket::initialize() noexcept
    {
#if ASYN_PLATFORM_WIN32
        auto                             &state = winsockReferenceCount();
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
        // Linux 侧没有需要启动的全局子系统，但有一个进程级设置要在这里落地：忽略 SIGPIPE。
        // 本层的常规发送都带 MSG_NOSIGNAL，但 sendfile 与移交通道的 sendmsg 没有对应的 per-call
        // 标志——向已关闭的对端发送会以默认动作打死整个进程。忽略之后写失败改以 EPIPE 返回，交给既有的
        // 发送失败路径处理（与 TlsContext 对 OpenSSL 内部写入的处理同源，那边是另一条
        // 绕不开的信号来源）。signal() 幂等，但每次 Socket 初始化都会走到这里，
        // call_once 让重复调用只剩一次原子检查
        static std::once_flag sigpipeGuard;
        std::call_once(sigpipeGuard, [] { std::signal(SIGPIPE, SIG_IGN); });
        return true;
#endif
    }

    void Socket::finalize() noexcept
    {
#if ASYN_PLATFORM_WIN32
        auto                             &state = winsockReferenceCount();
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
            // 置非阻塞失败不能静默放过：返回的阻塞套接字会被当作非阻塞套接字注册进事件循环，
            // 之后任何 send/recv 都可能把循环线程停住，而调用方无从察觉。失败即关闭并返回无效值
            if (!FileDescriptor::setNonBlocking(static_cast<int>(fileDescriptor)))
            {
                const int failureCode = PlatformError::lastSocketErrorCode();
                ::closesocket(fileDescriptor);
                PlatformError::setLastErrorCode(failureCode);
                return FileDescriptor::kInvalid;
            }
            // 与 POSIX 侧的 SOCK_CLOEXEC 取平：Windows 的 accept 句柄默认可继承，留着继承位就等于
            // 把一条已建立的连接交给随机一个子进程——父进程关掉它之后这一侧仍被陌生进程持着
            static_cast<void>(FileDescriptor::markNonInheritable(static_cast<int>(fileDescriptor)));
        }
        return static_cast<int>(fileDescriptor);
#else
        return ::accept4(listenDescriptor, address, addressLength, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif
    }

    int Socket::create(const int domain, const int type) noexcept
    {
#if ASYN_PLATFORM_WIN32
        const SOCKET socketHandle   = ::socket(domain, type, 0);
        const int    fileDescriptor = static_cast<int>(socketHandle);
        if (fileDescriptor < 0)
        {
            return FileDescriptor::kInvalid;
        }
        static_cast<void>(FileDescriptor::markNonInheritable(fileDescriptor));
        static_cast<void>(FileDescriptor::setNonBlocking(fileDescriptor));
        return fileDescriptor;
#else
        return ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#endif
    }

    bool Socket::bind(const int descriptor, const sockaddr *const address, const socklen_t addressLength) noexcept
    {
        if (rejectInvalidDescriptor(descriptor))
        {
            return false;
        }
        return ::bind(descriptor, address, addressLength) == 0;
    }

    bool Socket::listen(const int descriptor, const int backlog) noexcept
    {
        if (rejectInvalidDescriptor(descriptor))
        {
            return false;
        }
        return ::listen(descriptor, backlog) == 0;
    }

    bool Socket::connect(const int descriptor, const sockaddr *const address, const socklen_t addressLength) noexcept
    {
        if (rejectInvalidDescriptor(descriptor))
        {
            return false;
        }
        return ::connect(descriptor, address, addressLength) == 0;
    }

    ssize_t Socket::send(const int descriptor, const void *const buffer, const std::size_t length, const int flags) noexcept
    {
        if (rejectInvalidDescriptor(descriptor))
        {
            return -1;
        }
        return static_cast<ssize_t>(::send(descriptor, static_cast<const char *>(buffer), static_cast<int>(length), flags));
    }

    ssize_t Socket::receive(const int descriptor, void *const buffer, const std::size_t length, const int flags) noexcept
    {
        if (rejectInvalidDescriptor(descriptor))
        {
            return -1;
        }
        return static_cast<ssize_t>(::recv(descriptor, static_cast<char *>(buffer), static_cast<int>(length), flags));
    }

    bool Socket::peerAddress(const int descriptor, sockaddr *const address, socklen_t *const addressLength) noexcept
    {
        if (rejectInvalidDescriptor(descriptor))
        {
            return false;
        }
        return ::getpeername(descriptor, address, addressLength) == 0;
    }

    bool Socket::localAddress(const int descriptor, sockaddr *const address, socklen_t *const addressLength) noexcept
    {
        if (rejectInvalidDescriptor(descriptor))
        {
            return false;
        }
        return ::getsockname(descriptor, address, addressLength) == 0;
    }

    bool Socket::shutdown(const int descriptor, const int how) noexcept
    {
        if (rejectInvalidDescriptor(descriptor))
        {
            return false;
        }
        return ::shutdown(descriptor, how) == 0;
    }

    std::error_code Socket::lastError() noexcept
    {
        return std::error_code(PlatformError::lastSocketErrorCode(), std::system_category());
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

    bool Socket::isListeningStream(const int descriptor) noexcept
    {
        int       type       = 0;
        socklen_t typeLength = static_cast<socklen_t>(sizeof(type));
        // 值参数两家不同（Windows 是 char*、POSIX 是 void*）：统一转 char* 即可，不需要平台分支
        if (::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&type), &typeLength) != 0)
        {
            return false;
        }
        if (type != SOCK_STREAM)
        {
            return false;
        }

        int       acceptConnection = 0;
        socklen_t optionLength     = static_cast<socklen_t>(sizeof(acceptConnection));
        return ::getsockopt(descriptor, SOL_SOCKET, SO_ACCEPTCONN, reinterpret_cast<char *>(&acceptConnection), &optionLength) == 0 && acceptConnection != 0;
    }
} // namespace AsynGyanis::Platform
