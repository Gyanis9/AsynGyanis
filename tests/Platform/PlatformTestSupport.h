/**
 * @file PlatformTestSupport.h
 * @brief Platform 模块单元测试辅助：转发共享夹具 + 描述符可读性轮询
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 临时目录与等待夹具的实体在 tests/TestSupport/CommonTestSupport.h，这里以 using
 *          转发进 Platform::TestSupport（调用点无需改动）；本文件只保留 Platform 专有的助手。
 */

#pragma once

#include "CommonTestSupport.h"
#include "Platform/IO/FileDescriptor.h"

#include <chrono>
#include <thread>

namespace AsynGyanis::Platform::TestSupport
{
    using AsynGyanis::TestSupport::kWaitTimeout;
    using AsynGyanis::TestSupport::TemporaryDirectory;
    using AsynGyanis::TestSupport::waitForCondition;

    /**
     * @brief 轮询等待描述符变为可读
     *
     * @details 不使用 poll()/select()，因为两者在 Windows 上只面向 socket，
     *          这里以带超时的重复读取实现跨平台等待。
     * @param fileDescriptor 待观察的描述符
     * @param timeoutMilliseconds 最长等待毫秒数
     * @return true 在超时前成功读到数据
     * @return false 超时仍未读到数据
     */
    inline bool waitForReadable(const int fileDescriptor, const int timeoutMilliseconds)
    {
        constexpr int kpollIntervalMilliseconds = 5;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
        char       buffer[64];

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (FileDescriptor::read(fileDescriptor, buffer, sizeof(buffer)) > 0)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kpollIntervalMilliseconds));
        }
        return false;
    }
} // namespace AsynGyanis::Platform::TestSupport
