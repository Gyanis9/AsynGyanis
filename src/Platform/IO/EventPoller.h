/**
 * @file EventPoller.h
 * @brief 事件后端的编译期选型：epoll / IOCP / io_uring 三选一
 * @author Gyanis
 * @date 2026-10-21
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Platform/IO/EventBackendContract.h"
#include "Platform/IO/Epoll.h"

namespace AsynGyanis::Platform
{
    /**
     * @brief 当前平台/构建开关选中的事件后端
     * @details Epoll.h 在 Windows 上以 using Epoll = Iocp 指过去，在开启 ASYN_WITH_IO_URING
     *          的 Linux 上以 using Epoll = Uring 指过去；本别名只给 Core 一个与平台无关的名字。
     */
    using EventPoller = Epoll;

    static_assert(EventBackend<EventPoller>);
} // namespace AsynGyanis::Platform
