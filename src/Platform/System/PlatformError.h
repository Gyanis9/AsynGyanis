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
     * @details Linux 下系统调用与 socket 统一通过 errno 报告错误；Windows 下 socket 走
     *          WSAGetLastError()，而 CRT/文件类调用依旧设置 errno，
     *          因此两个访问器分别对应这两条路径（lastSocketErrorCode / lastErrorCode）。
     *          本类把差异收敛成两组语义常量：
     *          - 除下面显式标注者外，常量取自 socket 空间，必须与 lastSocketErrorCode() 配对比较
     *            （这正是全部现有调用方的用法：AsyncSocket 与 TcpAcceptor 的资源紧张判定）；
     *          - kInterrupted 在两套空间里都能用：POSIX 只认 errno 的 EINTR，
     *            Windows 下 WSAEINTR 属于 socket 空间，系统空间的中断是另一个码，
     *            现有系统空间调用点（inotify）只在 Linux 编译，因此不受影响。
     * @note Windows 的 socket 空间没有「内存不足」与「系统文件表满」的独立取值，
     *       因此 kOutOfMemory 与 kNoBufferSpace 同值、kSystemFileTableFull 与
     *       kTooManyOpenFiles 同值；判定时把它们整组写成 OR 即可，
     *       不要假设两两不同。
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
                ; ///< 调用被中断，可安全重试（POSIX 与 Windows socket 空间均适用）

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
                // Windows 的 socket 空间没有独立的「系统文件表满」，与 kTooManyOpenFiles 同值；
                // Linux 上 ENFILE 与 EMFILE 是两回事，因此这个常量必须保留
                WSAEMFILE
#else
                ENFILE
#endif
                ; ///< 系统级文件表已满（Windows 上与 kTooManyOpenFiles 同值）

        static constexpr int kNoBufferSpace =
#if ASYN_PLATFORM_WIN32
                WSAENOBUFS
#else
                ENOBUFS
#endif
                ; ///< 缓冲区/内存不足

        static constexpr int kOutOfMemory =
#if ASYN_PLATFORM_WIN32
                // 原实现取 ERROR_NOT_ENOUGH_MEMORY（Win32 系统空间），而调用方一律拿
                // WSAGetLastError() 的结果来比，永远匹配不上。socket 空间里内存不足就是
                // WSAENOBUFS，与 kNoBufferSpace 同值——这是平台事实，不是笔误
                WSAENOBUFS
#else
                ENOMEM
#endif
                ; ///< 内存分配失败（Windows 上与 kNoBufferSpace 同值）

        static constexpr int kInvalidArgument =
#if ASYN_PLATFORM_WIN32
                WSAEINVAL
#else
                EINVAL
#endif
                ; ///< 参数非法：调用方传了平台不接受的值（如超出平台上限的段数/长度）

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
