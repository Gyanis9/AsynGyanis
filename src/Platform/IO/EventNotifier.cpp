/**
 * @file EventNotifier.cpp
 * @brief 跨平台事件通知器，用于从其他线程唤醒事件循环
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Platform/IO/EventNotifier.h"

#include "Platform/IO/FileDescriptor.h"

#include <cstdint>

namespace AsynGyanis::Platform
{
    EventNotifier::EventNotifier()
    {
#if ASYN_PLATFORM_LINUX
        // eventfd 的读写共用同一个描述符
        const int fileDescriptor = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        m_readDescriptor         = fileDescriptor;
        m_writeDescriptor        = fileDescriptor;
#else
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;
        if (FileDescriptor::createPair(readDescriptor, writeDescriptor))
        {
            m_readDescriptor  = readDescriptor;
            m_writeDescriptor = writeDescriptor;
        }
#endif
    }

    EventNotifier::~EventNotifier()
    {
        FileDescriptor::close(m_readDescriptor);
        if (m_writeDescriptor != m_readDescriptor)
        {
            // Linux 下 eventfd 两端同源，只关闭一次
            FileDescriptor::close(m_writeDescriptor);
        }
    }

    int EventNotifier::readDescriptor() const noexcept
    {
        return m_readDescriptor;
    }

    bool EventNotifier::isValid() const noexcept
    {
        return FileDescriptor::isValid(m_readDescriptor) && FileDescriptor::isValid(m_writeDescriptor);
    }

    void EventNotifier::notify() const noexcept
    {
        if (!isValid())
        {
            return;
        }
#if ASYN_PLATFORM_LINUX
        // eventfd 要求写入 sizeof(std::uint64_t) 字节，短写会返回 EINVAL
        constexpr std::uint64_t increment = 1;
        FileDescriptor::write(m_writeDescriptor, &increment, sizeof(increment));
#else
        constexpr char kmarker = 1;
        FileDescriptor::write(m_writeDescriptor, &kmarker, sizeof(kmarker));
#endif
    }

    void EventNotifier::drain() const noexcept
    {
        if (!isValid())
        {
            return;
        }
        // 非阻塞描述符读到 0 或 -1 即表示已排空，循环覆盖多次 notify 合并的情况
        char buffer[64];
        while (FileDescriptor::read(m_readDescriptor, buffer, sizeof(buffer)) > 0)
        {
        }
    }
} // namespace AsynGyanis::Platform
