/**
 * @file HandleFaultInjection.h
 * @brief 句柄创建失败的测试注入点（不随安装包发布）
 * @author Gyanis
 * @date 2026-10-23
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Platform/Platform.h"

namespace AsynGyanis::Platform::Testing
{
#if ASYN_PLATFORM_WIN32
    /**
     * @brief 让第 allowedSuccesses + 1 次句柄创建失败，并置入给定的平台错误码
     * @details Windows 没有可用的 per-process 句柄上限：真实耗尽系统句柄会伤及整台机器。
     *          测试经本注入点造出「socket/accept 建不出来」的形状，确定性地钉住失败分类与
     *          清理路径——关闭已建出的句柄、输出保持无效、失败原因不被清理调用覆盖。
     * @param allowedSuccesses 先放行的成功创建次数；0 表示下一次创建就失败
     * @param errorCode WSA 错误码（如 WSAEMFILE）
     * @note 只给测试用：命中一次后自动恢复真实调用
     */
    ASYN_PLATFORM_API void failHandleCreationAfter(int allowedSuccesses, int errorCode) noexcept;

    /**
     * @brief 取消尚未命中的注入
     */
    ASYN_PLATFORM_API void clearHandleCreationFailure() noexcept;
#endif
} // namespace AsynGyanis::Platform::Testing
