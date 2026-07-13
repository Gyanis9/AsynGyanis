/**
 * @file EventNotifier.h
 * @brief 跨平台事件通知器 — Linux 用 eventfd，Windows 用 socket pair
 * @copyright Copyright (c) 2026
 */

#ifndef PLATFORM_EVENTNOTIFIER_H
#define PLATFORM_EVENTNOTIFIER_H

#include "Platform.h"
#include "SocketCompat.h"

#include <cstdint>

namespace Platform
{
    /**
     * @brief 跨线程事件通知器
     *
     * Linux: 基于 eventfd 实现，readFd() 返回 eventfd。
     * Windows: 基于 loopback socket pair 实现，readFd() 返回读端 socket。
     *
     * 用法：
     * 1. 将 readFd() 注册到 epoll
     * 2. 跨线程调用 notify() 唤醒事件循环
     * 3. 事件循环中被唤醒后调用 drain() 清空数据
     */
    class EventNotifier
    {
    public:
        EventNotifier()
        {
#ifdef _WIN32
            int rfd = -1, wfd = -1;
            if (createSocketPair(rfd, wfd))
            {
                m_readFd  = rfd;
                m_writeFd = wfd;
                setNonBlocking(m_readFd);
            }
#else
            m_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#endif
        }

        ~EventNotifier()
        {
#ifdef _WIN32
            if (m_readFd >= 0)
                closeFd(m_readFd);
            if (m_writeFd >= 0)
                closeFd(m_writeFd);
#else
            if (m_fd >= 0)
                ::close(m_fd);
#endif
        }

        EventNotifier(const EventNotifier &)            = delete;
        EventNotifier &operator=(const EventNotifier &) = delete;
        EventNotifier(EventNotifier &&)                 = delete;
        EventNotifier &operator=(EventNotifier &&)      = delete;

        /**
         * @brief 获取供 epoll 监听的文件描述符
         */
        int readFd() const noexcept
        {
#ifdef _WIN32
            return m_readFd;
#else
            return m_fd;
#endif
        }

        /**
         * @brief 唤醒事件循环（线程安全）
         */
        void notify() const
        {
#ifdef _WIN32
            if (m_writeFd >= 0)
            {
                char byte = 1;
                ::send(m_writeFd, &byte, 1, 0);
            }
#else
            if (m_fd >= 0)
            {
                uint64_t val = 1;
                ::write(m_fd, &val, sizeof(val));
            }
#endif
        }

        /**
         * @brief 读取并清空通知数据（在事件循环中被唤醒后调用）
         */
        void drain() const
        {
#ifdef _WIN32
            if (m_readFd >= 0)
            {
                char buf[64];
                while (::recv(m_readFd, buf, sizeof(buf), 0) > 0)
                {
                }
            }
#else
            if (m_fd >= 0)
            {
                uint64_t val;
                ::read(m_fd, &val, sizeof(val));
            }
#endif
        }

    private:
#ifdef _WIN32
        int m_readFd{-1};  ///< 读端 socket（注册到 epoll）
        int m_writeFd{-1}; ///< 写端 socket（用于唤醒）
#else
        int m_fd{-1}; ///< eventfd 文件描述符
#endif
    };

} // namespace Platform

#endif // PLATFORM_EVENTNOTIFIER_H
