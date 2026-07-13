/**
 * @file Platform.h
 * @brief 平台检测宏、类型定义与 errno 兼容层
 * @copyright Copyright (c) 2026
 */

#ifndef PLATFORM_PLATFORM_H
#define PLATFORM_PLATFORM_H

// ============================================================================
// 平台检测
// ============================================================================
#ifdef _WIN32
  #define ASYN_PLATFORM_WIN32 1
  #define ASYN_PLATFORM_LINUX 0
#else
  #define ASYN_PLATFORM_WIN32 0
  #define ASYN_PLATFORM_LINUX 1
#endif

// ============================================================================
// Windows 头文件与类型定义
// ============================================================================
#ifdef _WIN32
  // winsock2.h 必须在 windows.h 之前包含
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

  // Windows SDK 定义了多个与项目枚举值冲突的宏，必须在此 #undef
  // DELETE  — winnt.h 中定义为 (0x00010000L) 访问权限常量
  // ERROR   — winerror.h 中定义为 0，与日志 LogLevel::ERROR 冲突
  #ifdef DELETE
    #undef DELETE
  #endif
  #ifdef ERROR
    #undef ERROR
  #endif

  // wepoll 不提供 EPOLL_CLOEXEC / EPOLLET，补定义
  #ifndef EPOLL_CLOEXEC
    #define EPOLL_CLOEXEC 0
  #endif
  #ifndef EPOLLET
    // wepoll 不支持边缘触发 (EPOLLONESHOT 占用了 1U<<31)，
    // 定义为 0 让 level-triggered 行为保持不变
    #define EPOLLET 0
  #endif

  // ssize_t 在 MSVC 中不存在
  #ifndef _SSIZE_T_DEFINED
    #define _SSIZE_T_DEFINED
    using ssize_t = __int64;
  #endif

  // epoll handle 类型：wepoll 返回 HANDLE
  using epoll_handle_t = HANDLE;

  inline bool epoll_handle_valid(epoll_handle_t h) noexcept
  {
      return h != nullptr;
  }

  inline constexpr epoll_handle_t kInvalidEpollHandle = nullptr;

  // errno 兼容（仅用于 socket 操作）
  #define ASYN_ERRNO        (::WSAGetLastError())
  #define ASYN_EAGAIN       WSAEWOULDBLOCK
  #define ASYN_EWOULDBLOCK  WSAEWOULDBLOCK
  #define ASYN_EINTR        WSAEINTR
  #define ASYN_EINPROGRESS  WSAEWOULDBLOCK
  #define ASYN_ECONNABORTED WSAECONNABORTED
  #define ASYN_EMFILE       WSAEMFILE
  #define ASYN_ENFILE       WSAEMFILE
  #define ASYN_ENOBUFS      WSAENOBUFS
  #define ASYN_ENOMEM       ERROR_NOT_ENOUGH_MEMORY

  // 缺失常量定义
  #ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0
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

// ============================================================================
// Linux 头文件与类型定义
// ============================================================================
#else
  #include <sys/epoll.h>
  #include <sys/socket.h>
  #include <sys/timerfd.h>
  #include <sys/eventfd.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>

  using epoll_handle_t = int;

  inline bool epoll_handle_valid(epoll_handle_t h) noexcept
  {
      return h >= 0;
  }

  inline constexpr epoll_handle_t kInvalidEpollHandle = -1;

  #define ASYN_ERRNO        (errno)
  #define ASYN_EAGAIN       EAGAIN
  #define ASYN_EWOULDBLOCK  EWOULDBLOCK
  #define ASYN_EINTR        EINTR
  #define ASYN_EINPROGRESS  EINPROGRESS
  #define ASYN_ECONNABORTED ECONNABORTED
  #define ASYN_EMFILE       EMFILE
  #define ASYN_ENFILE       ENFILE
  #define ASYN_ENOBUFS      ENOBUFS
  #define ASYN_ENOMEM       ENOMEM
#endif

#endif // PLATFORM_PLATFORM_H
