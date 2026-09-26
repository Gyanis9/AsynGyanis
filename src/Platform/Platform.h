/**
 * @file Platform.h
 * @brief 平台检测宏与操作系统底层头文件聚合，全项目跨平台代码的统一入口
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

// ============================================================================
// 平台检测
// ----------------------------------------------------------------------------
// 下列标识符必须是宏（供各模块 #if 分支与头文件裁剪使用），故按规范使用
// UPPER_SNAKE_CASE，不适用 constexpr 常量规则。
// ============================================================================
#if defined(_WIN32)
#define ASYN_PLATFORM_WIN32 1
#define ASYN_PLATFORM_LINUX 0
#elif defined(__linux__)
#define ASYN_PLATFORM_WIN32 0
#define ASYN_PLATFORM_LINUX 1
#else
#error "AsynGyanis 仅支持 Windows 与 Linux 平台"
#endif

// ============================================================================
// 操作系统头文件与缺失符号替身
// ============================================================================
#include <cerrno>
#include <cstddef>

#if ASYN_PLATFORM_WIN32
// 这四行的顺序是有约束的，不能按字母排：mswsock.h 与 ws2tcpip.h 都不自带所需的基础类型，
// 它们要靠先包含进来的 winsock2.h 与 windows.h 提供；一旦排到前面，SDK 头自身就解析失败
// （症状是 mswsockdef.h 里成片「未知重写说明符」「缺少类型说明符」，不看错源码的人会去查 SDK）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

// Windows SDK 宏与项目标识符冲突，必须在此清除：
//   DELETE —— winnt.h 中的访问权限常量 (0x00010000L)
//   ERROR  —— winerror.h 中定义为 0，与日志等级 LogLevel::ERROR 冲突
#ifdef DELETE
#undef DELETE
#endif
#ifdef ERROR
#undef ERROR
#endif

// wepoll 兼容层未提供的 epoll 标志。EPOLLET 置 0 使边缘触发退化为水平触发，
// 因为 wepoll 用 1U<<31 实现了 EPOLLONESHOT。
#ifndef EPOLL_CLOEXEC
#define EPOLL_CLOEXEC 0
#endif
#ifndef EPOLLET
#define EPOLLET 0
#endif

// MSVC 运行库不提供 ssize_t
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
using ssize_t = __int64; ///< 与 POSIX 对齐的带符号尺寸类型
#endif

// POSIX 送/关闭标志在 Windows 上的替身，同样必须是宏以便在系统调用实参处使用
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
// winsock 没有 per-call 的「不等待」标志：本层套接字一律非阻塞，取 0 即等价
#define MSG_DONTWAIT 0
#endif
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>
#endif

namespace AsynGyanis::Platform
{
    /// epoll 实例句柄类型：Windows 上 wepoll 返回 HANDLE，Linux 上为文件描述符
    using EpollHandle =
#if ASYN_PLATFORM_WIN32
            HANDLE
#else
            int
#endif
            ;

    inline constexpr EpollHandle kInvalidEpollHandle =
#if ASYN_PLATFORM_WIN32
            nullptr
#else
            -1
#endif
            ;

    /**
     * @brief 判断 epoll 句柄是否有效
     * @param handle epoll 实例句柄
     * @return true 句柄有效
     * @return false 句柄创建失败
     */
    constexpr bool isEpollHandleValid(EpollHandle handle) noexcept
    {
#if ASYN_PLATFORM_WIN32
        return handle != nullptr;
#else
        return handle >= 0;
#endif
    }
} // namespace AsynGyanis::Platform
