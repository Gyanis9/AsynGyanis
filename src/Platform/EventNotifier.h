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
     * Linux: 基于 eventfd 实现，readFileDescriptor() 返回 eventfd。
     * Windows: 基于 loopback socket pair 实现，readFileDescriptor() 返回读端 socket。
     *
     * 用法：
     * 1. 将 readFileDescriptor() 注册到 epoll
     * 2. 跨线程调用 notify() 唤醒事件循环
     * 3. 事件循环中被唤醒后调用 drain() 清空数据
     */
    class EventNotifier
    {
    public:
        EventNotifier()
        {
#ifdef _WIN32
            int readFileDescriptor = -1, writeFileDescriptor = -1;
            if (createSocketPair(readFileDescriptor, writeFileDescriptor))
            {
                m_readFileDescriptor  = readFileDescriptor;
                m_writeFileDescriptor = writeFileDescriptor;
                setNonBlocking(m_readFileDescriptor);
            }
#else
            m_fileDescriptor = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#endif
        }

        ~EventNotifier()
        {
#ifdef _WIN32
            if (m_readFileDescriptor >= 0)
                closeFileDescriptor(m_readFileDescriptor);
            if (m_writeFileDescriptor >= 0)
                closeFileDescriptor(m_writeFileDescriptor);
#else
            if (m_fileDescriptor >= 0)
                ::close(m_fileDescriptor);
#endif
        }

        EventNotifier(const EventNotifier &)            = delete;
        EventNotifier &operator=(const EventNotifier &) = delete;
        EventNotifier(EventNotifier &&)                 = delete;
        EventNotifier &operator=(EventNotifier &&)      = delete;

        /**
         * @brief 获取供 epoll 监听的文件描述符
         */
        int readFileDescriptor() const noexcept
        {
#ifdef _WIN32
            return m_readFileDescriptor;
#else
            return m_fileDescriptor;
#endif
        }

        /**
         * @brief 唤醒事件循环（线程安全）
         */
        void notify() const
        {
#ifdef _WIN32
            if (m_writeFileDescriptor >= 0)
            {
                char byte = 1;
                ::send(m_writeFileDescriptor, &byte, 1, 0);
            }
#else
            if (m_fileDescriptor >= 0)
            {
                uint64_t value = 1;
                ::write(m_fileDescriptor, &value, sizeof(value));
            }
#endif
        }

        /**
         * @brief 读取并清空通知数据（在事件循环中被唤醒后调用）
         */
        void drain() const
        {
#ifdef _WIN32
            if (m_readFileDescriptor >= 0)
            {
                char buffer[64];
                while (::recv(m_readFileDescriptor, buffer, sizeof(buffer), 0) > 0)
                {
                }
            }
#else
            if (m_fileDescriptor >= 0)
            {
                uint64_t value;
                ::read(m_fileDescriptor, &value, sizeof(value));
            }
#endif
        }

    private:
#ifdef _WIN32
        int m_readFileDescriptor{-1};  ///< 读端 socket（注册到 epoll）
        int m_writeFileDescriptor{-1}; ///< 写端 socket（用于唤醒）
#else
        int m_fileDescriptor{-1}; ///< eventfd 文件描述符
#endif
    };

} // namespace Platform

#endif // PLATFORM_EVENTNOTIFIER_H
