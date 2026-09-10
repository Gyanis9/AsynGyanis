/**
 * @file PlatformError.h
 * @brief 跨平台错误码常量与最近一次系统错误的读取
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

#include <string>

namespace AsynGyanis::Platform
{
    /**
     * @brief 平台错误码工具
     *
     * @details Linux 下系统调用统一通过 errno 报告错误；Windows 下 socket 使用
     *          WSAGetLastError()，其取值与 POSIX errno 不同（例如 WSAEWOULDBLOCK
     *          为 10035）。本类把两侧的差异收敛为一组 POSIX 语义的常量与访问器，
     *          调用方只需与本平台无关的语义值比较即可。
     * @note 常量值在 Windows 上对应 socket 错误码，因此比较前请确认错误码来源
     *       与访问器成对使用（lastSocketErrorCode 对 kWouldBlock 等）。
     */
    class PlatformError
    {
    public:
        static constexpr int kInterrupted =
#if ASYN_PLATFORM_WIN32
                WSAEINTR
#else
                EINTR
#endif
                ; ///< 调用被信号中断，可安全重试

        static constexpr int kWouldBlock =
#if ASYN_PLATFORM_WIN32
                WSAEWOULDBLOCK
#else
                EAGAIN
#endif
                ; ///< 非阻塞描述符暂无数据

        static constexpr int kInProgress =
#if ASYN_PLATFORM_WIN32
                WSAEWOULDBLOCK
#else
                EINPROGRESS
#endif
                ; ///< 非阻塞连接正在进行中

        static constexpr int kConnectionAborted =
#if ASYN_PLATFORM_WIN32
                WSAECONNABORTED
#else
                ECONNABORTED
#endif
                ; ///< 连接被本地中止

        static constexpr int kTooManyOpenFiles =
#if ASYN_PLATFORM_WIN32
                WSAEMFILE
#else
                EMFILE
#endif
                ; ///< 进程描述符已达上限

        static constexpr int kSystemFileTableFull =
#if ASYN_PLATFORM_WIN32
                WSAEMFILE
#else
                ENFILE
#endif
                ; ///< 系统级文件表已满

        static constexpr int kNoBufferSpace =
#if ASYN_PLATFORM_WIN32
                WSAENOBUFS
#else
                ENOBUFS
#endif
                ; ///< 缓冲区内存不足

        static constexpr int kOutOfMemory =
#if ASYN_PLATFORM_WIN32
                ERROR_NOT_ENOUGH_MEMORY
#else
                ENOMEM
#endif
                ; ///< 内存分配失败

        /**
         * @brief 读取最近一次 CRT/文件类系统调用的错误码
         * @return int 当前线程 errno 值
         */
        static int lastErrorCode() noexcept;

        /**
         * @brief 读取最近一次 socket 操作错误码
         * @details Windows 返回 WSAGetLastError()，Linux 与 errno 同源故直接返回 errno。
         * @return int 平台 socket 错误码
         */
        static int lastSocketErrorCode() noexcept;

        /**
         * @brief 设置当前线程的最近错误码
         * @param errorCode 需要写入的错误码
         */
        static void setLastErrorCode(int errorCode) noexcept;

        /**
         * @brief 将平台错误码转换为可读描述
         * @param errorCode 由 lastErrorCode()/lastSocketErrorCode() 取得的错误码
         * @return std::string 本地化程度取决于运行库实现
         */
        static std::string message(int errorCode);
    };
} // namespace AsynGyanis::Platform
