#include "Platform/IO/Socket.h"

#include "Platform/System/PlatformError.h"

#if ASYN_PLATFORM_WIN32
#include <mstcpip.h>
#endif

namespace AsynGyanis::Platform
{
    namespace
    {
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
    } // namespace

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

    bool Socket::requiresListenerHandoff() noexcept
    {
#if ASYN_PLATFORM_WIN32
        return true;
#else
        return false;
#endif
    }

    bool Socket::setNoDelay(const int descriptor) noexcept
    {
        return setIntegerOption(descriptor, IPPROTO_TCP, TCP_NODELAY, 1);
    }

    bool Socket::setKeepAlive(const int descriptor, const int idleSeconds, const int intervalSeconds, const int probeCount) noexcept
    {
        if (idleSeconds <= 0)
        {
            // 非正的 idle 不构成「开启保活」：时刻表会退回系统默认的空闲两小时，半开会话的兜底
            // 等于没做。与其下发一份语义含糊的取值，不如拒绝并让调用方按「未开启」处理
            return false;
        }
        if (!setIntegerOption(descriptor, SOL_SOCKET, SO_KEEPALIVE, 1))
        {
            return false;
        }

        // 以下每一块都按「平台有此入口才下发」判定（用特性宏而不是操作系统宏，理由同 setReusePort()）：
        // 调用方没给的那一格留系统默认，本函数不替它编一个默认值
#ifdef TCP_KEEPIDLE
        if (!setIntegerOption(descriptor, IPPROTO_TCP, TCP_KEEPIDLE, idleSeconds))
        {
            return false;
        }
#endif
#ifdef TCP_KEEPINTVL
        if (intervalSeconds > 0 && !setIntegerOption(descriptor, IPPROTO_TCP, TCP_KEEPINTVL, intervalSeconds))
        {
            return false;
        }
#endif
#ifdef TCP_KEEPCNT
        if (probeCount > 0 && !setIntegerOption(descriptor, IPPROTO_TCP, TCP_KEEPCNT, probeCount))
        {
            return false;
        }
#endif
#ifdef SIO_KEEPALIVE_VALS
        // Windows 没有 TCP_KEEPIDLE 这一组选项名，整张时刻表只能由 SIO_KEEPALIVE_VALS 一次给出（毫秒），
        // 且结构体里没有探测次数那一格：调用方只给了 idle 就没法凑出时刻表，此时只保留上面已置上的开关
        if (intervalSeconds > 0)
        {
            tcp_keepalive request{};
            request.onoff             = 1;
            request.keepalivetime     = static_cast<u_long>(idleSeconds) * 1000U;
            request.keepaliveinterval = static_cast<u_long>(intervalSeconds) * 1000U;

            unsigned long returnedByteCount = 0;
            return ::WSAIoctl(descriptor, SIO_KEEPALIVE_VALS, &request, static_cast<DWORD>(sizeof(request)), nullptr, 0, &returnedByteCount, nullptr, nullptr) == 0;
        }
#endif
        return true;
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

    bool Socket::setFastOpen(const int descriptor, const int queueLength) noexcept
    {
#ifdef TCP_FASTOPEN
        // 负值没有「允许 TFO 的等待队列长度」这一语义：直接拒绝，不把含糊取值交给内核
        if (queueLength < 0)
        {
            return false;
        }
        return setIntegerOption(descriptor, IPPROTO_TCP, TCP_FASTOPEN, queueLength);
#else
        // 头文件里没有该选项（老内核头 / 老 SDK）：按「平台不支持」返回 false 由调用方降级
        (void) descriptor;
        (void) queueLength;
        return false;
#endif
    }

    bool Socket::setIpv6Only(const int descriptor, const bool isOnlyV6) noexcept
    {
        // 布尔值在两个平台上都按 int 尺寸传递，写成 0/1 避免 sizeof(bool) 歧义
        return setIntegerOption(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, isOnlyV6 ? 1 : 0);
    }

    bool Socket::setUserTimeout(const int descriptor, const int milliseconds) noexcept
    {
        if (milliseconds <= 0)
        {
            // 非正的毫秒数没有「在途多久算死」的语义：0 在内核里是关掉这一项，负值无定义。
            // 与 setKeepAlive 同一条口径：不下发含糊取值，交回 false 让调用方按「未开启」处理
            return false;
        }
#ifdef TCP_USER_TIMEOUT
        return setIntegerOption(descriptor, IPPROTO_TCP, TCP_USER_TIMEOUT, milliseconds);
#else
        // Windows 没有该选项（TCP_MAXRT 语义不同，见头文件说明）：返回 false 让调用方按
        // 「本平台没有这一格」处理——配合 hasUserTimeout() 可以把它与「设置失败」分开报
        (void) descriptor;
        return false;
#endif
    }

    bool Socket::setLinger(const int descriptor, const bool isEnabled, const int seconds) noexcept
    {
        // SO_LINGER 在两侧都是 struct linger：开关是 u_short、超时是 u_short（秒）。
        // 负的秒数按 0 处理（都是「不等待」），因此不需要拒绝这一个入参形状
        linger option{};
        option.l_onoff  = isEnabled ? 1 : 0;
        option.l_linger = static_cast<unsigned short>(seconds < 0 ? 0 : seconds);
        return ::setsockopt(descriptor, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&option), static_cast<socklen_t>(sizeof(option))) == 0;
    }
} // namespace AsynGyanis::Platform
