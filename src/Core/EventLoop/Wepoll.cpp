/*
 * wepoll - epoll for Windows
 * https://github.com/piscisaureus/wepoll
 *
 * Copyright 2012-2020, Bert Belder <bertbelder@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* 接口只有一处出处：wepoll.h 是冻结的第三方接口（含 BSD-2-Clause 许可），
 * 本文件实现它。头里的声明在 extern "C" 内，因此下面四个入口点的定义自动保持 C 链接。 */
#include "wepoll.h"

#include <assert.h>

#include <stdlib.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <new>

#define WEPOLL_INTERNAL static
#define WEPOLL_INTERNAL_VAR static

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreserved-id-macro"
#endif

#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif

#define _WIN32_WINNT 0x0600

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#ifndef __GNUC__
#pragma warning(push, 1)
#endif

#include <WS2tcpip.h>
#include <WinSock2.h>
#include <Windows.h>

#ifndef __GNUC__
#pragma warning(pop)
#endif

WEPOLL_INTERNAL int nt_global_init(void);

typedef LONG NTSTATUS;
typedef NTSTATUS* PNTSTATUS;

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS) 0x00000000L)
#endif

#ifndef STATUS_PENDING
#define STATUS_PENDING ((NTSTATUS) 0x00000103L)
#endif

#ifndef STATUS_CANCELLED
#define STATUS_CANCELLED ((NTSTATUS) 0xC0000120L)
#endif

#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND ((NTSTATUS) 0xC0000225L)
#endif

typedef struct _IO_STATUS_BLOCK {
  NTSTATUS Status;
  ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef VOID(NTAPI* PIO_APC_ROUTINE)(PVOID ApcContext,
                                     PIO_STATUS_BLOCK IoStatusBlock,
                                     ULONG Reserved);

typedef struct _UNICODE_STRING {
  USHORT Length;
  USHORT MaximumLength;
  PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

#define RTL_CONSTANT_STRING(s) \
  { sizeof(s) - sizeof((s)[0]), sizeof(s), const_cast<PWSTR>(s) }

typedef struct _OBJECT_ATTRIBUTES {
  ULONG Length;
  HANDLE RootDirectory;
  PUNICODE_STRING ObjectName;
  ULONG Attributes;
  PVOID SecurityDescriptor;
  PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

#define RTL_CONSTANT_OBJECT_ATTRIBUTES(ObjectName, Attributes) \
  { sizeof(OBJECT_ATTRIBUTES), NULL, ObjectName, Attributes, NULL, NULL }

#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001UL
#endif

#define NT_NTDLL_IMPORT_LIST(X)           \
  X(NTSTATUS,                             \
    NTAPI,                                \
    NtCancelIoFileEx,                     \
    (HANDLE FileHandle,                   \
     PIO_STATUS_BLOCK IoRequestToCancel,  \
     PIO_STATUS_BLOCK IoStatusBlock))     \
                                          \
  X(NTSTATUS,                             \
    NTAPI,                                \
    NtCreateFile,                         \
    (PHANDLE FileHandle,                  \
     ACCESS_MASK DesiredAccess,           \
     POBJECT_ATTRIBUTES ObjectAttributes, \
     PIO_STATUS_BLOCK IoStatusBlock,      \
     PLARGE_INTEGER AllocationSize,       \
     ULONG FileAttributes,                \
     ULONG ShareAccess,                   \
     ULONG CreateDisposition,             \
     ULONG CreateOptions,                 \
     PVOID EaBuffer,                      \
     ULONG EaLength))                     \
                                          \
  X(NTSTATUS,                             \
    NTAPI,                                \
    NtDeviceIoControlFile,                \
    (HANDLE FileHandle,                   \
     HANDLE Event,                        \
     PIO_APC_ROUTINE ApcRoutine,          \
     PVOID ApcContext,                    \
     PIO_STATUS_BLOCK IoStatusBlock,      \
     ULONG IoControlCode,                 \
     PVOID InputBuffer,                   \
     ULONG InputBufferLength,             \
     PVOID OutputBuffer,                  \
     ULONG OutputBufferLength))           \
                                          \
  X(ULONG, WINAPI, RtlNtStatusToDosError, (NTSTATUS Status))

#define X(return_type, attributes, name, parameters) \
  WEPOLL_INTERNAL_VAR return_type(attributes* name) parameters = NULL;
NT_NTDLL_IMPORT_LIST(X)
#undef X

#define AFD_POLL_RECEIVE           0x0001
#define AFD_POLL_RECEIVE_EXPEDITED 0x0002
#define AFD_POLL_SEND              0x0004
#define AFD_POLL_DISCONNECT        0x0008
#define AFD_POLL_ABORT             0x0010
#define AFD_POLL_LOCAL_CLOSE       0x0020
#define AFD_POLL_ACCEPT            0x0080
#define AFD_POLL_CONNECT_FAIL      0x0100

typedef struct _AFD_POLL_HANDLE_INFO {
  HANDLE Handle;
  ULONG Events;
  NTSTATUS Status;
} AFD_POLL_HANDLE_INFO, *PAFD_POLL_HANDLE_INFO;

typedef struct _AFD_POLL_INFO {
  LARGE_INTEGER Timeout;
  ULONG NumberOfHandles;
  ULONG Exclusive;
  AFD_POLL_HANDLE_INFO Handles[1];
} AFD_POLL_INFO, *PAFD_POLL_INFO;

WEPOLL_INTERNAL int afd_create_helper_handle(HANDLE iocp_handle,
                                             HANDLE* afd_helper_handle_out);

WEPOLL_INTERNAL int afd_poll(HANDLE afd_helper_handle,
                             AFD_POLL_INFO* poll_info,
                             IO_STATUS_BLOCK* io_status_block);
WEPOLL_INTERNAL int afd_cancel_poll(HANDLE afd_helper_handle,
                                    IO_STATUS_BLOCK* io_status_block);

#define return_map_error(value) \
  do {                          \
    err_map_win_error();        \
    return (value);             \
  } while (0)

#define return_set_error(value, error) \
  do {                                 \
    err_set_win_error(error);          \
    return (value);                    \
  } while (0)

WEPOLL_INTERNAL void err_map_win_error(void);
WEPOLL_INTERNAL void err_set_win_error(DWORD error);
WEPOLL_INTERNAL int err_check_handle(HANDLE handle);

#define IOCTL_AFD_POLL 0x00012024

static UNICODE_STRING afd__helper_name =
    RTL_CONSTANT_STRING(L"\\Device\\Afd\\Wepoll");

static OBJECT_ATTRIBUTES afd__helper_attributes =
    RTL_CONSTANT_OBJECT_ATTRIBUTES(&afd__helper_name, 0);

int afd_create_helper_handle(HANDLE iocp_handle,
                             HANDLE* afd_helper_handle_out) {
  HANDLE afd_helper_handle;
  IO_STATUS_BLOCK iosb;
  NTSTATUS status;

  /* By opening \Device\Afd without specifying any extended attributes, we'll
   * get a handle that lets us talk to the AFD driver, but that doesn't have an
   * associated endpoint (so it's not a socket). */
  status = NtCreateFile(&afd_helper_handle,
                        SYNCHRONIZE,
                        &afd__helper_attributes,
                        &iosb,
                        NULL,
                        0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_OPEN,
                        0,
                        NULL,
                        0);
  if (status != STATUS_SUCCESS)
    return_set_error(-1, RtlNtStatusToDosError(status));

  /* 句柄已经拿到：这两步任一步失败都要先关掉它再报错。短路求值保证第二步只在第一步
     成功时才执行，报出来的仍是真正失败那一步的错误码 */
  if (CreateIoCompletionPort(afd_helper_handle, iocp_handle, 0, 0) == NULL ||
      !SetFileCompletionNotificationModes(afd_helper_handle,
                                          FILE_SKIP_SET_EVENT_ON_HANDLE)) {
    CloseHandle(afd_helper_handle);
    return_map_error(-1);
  }

  *afd_helper_handle_out = afd_helper_handle;
  return 0;
}

int afd_poll(HANDLE afd_helper_handle,
             AFD_POLL_INFO* poll_info,
             IO_STATUS_BLOCK* io_status_block) {
  NTSTATUS status;

  /* Blocking operation is not supported. */
  assert(io_status_block != NULL);

  io_status_block->Status = STATUS_PENDING;
  status = NtDeviceIoControlFile(afd_helper_handle,
                                 NULL,
                                 NULL,
                                 io_status_block,
                                 io_status_block,
                                 IOCTL_AFD_POLL,
                                 poll_info,
                                 sizeof *poll_info,
                                 poll_info,
                                 sizeof *poll_info);

  if (status == STATUS_SUCCESS)
    return 0;
  else if (status == STATUS_PENDING)
    return_set_error(-1, ERROR_IO_PENDING);
  else
    return_set_error(-1, RtlNtStatusToDosError(status));
}

int afd_cancel_poll(HANDLE afd_helper_handle,
                    IO_STATUS_BLOCK* io_status_block) {
  NTSTATUS cancel_status;
  IO_STATUS_BLOCK cancel_iosb;

  /* If the poll operation has already completed or has been cancelled earlier,
   * there's nothing left for us to do. */
  if (io_status_block->Status != STATUS_PENDING)
    return 0;

  cancel_status =
      NtCancelIoFileEx(afd_helper_handle, io_status_block, &cancel_iosb);

  /* NtCancelIoFileEx() may return STATUS_NOT_FOUND if the operation completed
   * just before calling NtCancelIoFileEx(). This is not an error. */
  if (cancel_status == STATUS_SUCCESS || cancel_status == STATUS_NOT_FOUND)
    return 0;
  else
    return_set_error(-1, RtlNtStatusToDosError(cancel_status));
}

WEPOLL_INTERNAL int epoll_global_init(void);

WEPOLL_INTERNAL int init(void);

typedef struct port_state port_state_t;
typedef struct queue queue_t;
typedef struct sock_state sock_state_t;
typedef struct ts_tree_node ts_tree_node_t;

WEPOLL_INTERNAL port_state_t* port_new(HANDLE* iocp_handle_out);
WEPOLL_INTERNAL int port_close(port_state_t* port_state);
WEPOLL_INTERNAL int port_delete(port_state_t* port_state);

WEPOLL_INTERNAL int port_wait(port_state_t* port_state,
                              struct epoll_event* events,
                              int maxevents,
                              int timeout);

WEPOLL_INTERNAL int port_ctl(port_state_t* port_state,
                             int op,
                             SOCKET sock,
                             struct epoll_event* ev);

WEPOLL_INTERNAL int port_register_socket(port_state_t* port_state,
                                         sock_state_t* sock_state,
                                         SOCKET socket);
WEPOLL_INTERNAL void port_unregister_socket(port_state_t* port_state,
                                            sock_state_t* sock_state);
WEPOLL_INTERNAL sock_state_t* port_find_socket(port_state_t* port_state,
                                               SOCKET socket);

WEPOLL_INTERNAL void port_request_socket_update(port_state_t* port_state,
                                                sock_state_t* sock_state);
WEPOLL_INTERNAL void port_cancel_socket_update(port_state_t* port_state,
                                               sock_state_t* sock_state);

WEPOLL_INTERNAL void port_add_deleted_socket(port_state_t* port_state,
                                             sock_state_t* sock_state);
WEPOLL_INTERNAL void port_remove_deleted_socket(port_state_t* port_state,
                                                sock_state_t* sock_state);

WEPOLL_INTERNAL HANDLE port_get_iocp_handle(port_state_t* port_state);
WEPOLL_INTERNAL queue_t* port_get_poll_group_queue(port_state_t* port_state);

WEPOLL_INTERNAL port_state_t* port_state_from_handle_tree_node(
    ts_tree_node_t* tree_node);
WEPOLL_INTERNAL ts_tree_node_t* port_state_to_handle_tree_node(
    port_state_t* port_state);

/* 引用锁：把「这块内存不许被提前释放」与「销毁时等所有引用放完」合进一个原子字。
   低 28 位是引用计数、高 4 位是销毁标记，等待与唤醒走 C++20 的 atomic::wait/notify
   （标准库在 Windows 上用 WaitOnAddress 实现），原实现自备的 keyed event 因此整块删掉。

   协议：
     - ref()：只在计数上加一，无等待；对已销毁的锁调用是用法错误，由断言兜住；
     - unref()：减一；恰好减到「只剩销毁标记」时唤醒正在等的销毁者；
     - unrefAndDestroy()：在同一个原子操作里放下自己那份引用并置上销毁标记，然后等其它引用放完
       ——这一步是本类比 shared_ptr 多出来的：销毁者要确认没人还在用这块内存；
     - 销毁完成后写入毒值，此后任何 ref/unref 都会被断言抓住。 */
class RefLock
{
public:
  /// 初始化为「零引用、未销毁」
  void init() noexcept
  {
    m_state.store(0, std::memory_order_relaxed);
  }

  /// 增加一份引用
  void ref() noexcept
  {
    const std::uint32_t state = m_state.fetch_add(kReferenceUnit, std::memory_order_acq_rel) + kReferenceUnit;

    /* 计数不得溢出，也不得在销毁之后再加引用（NDEBUG 下断言消失，故显式丢弃该值） */
    static_cast<void>(state);
    assert((state & kDestroyMask) == 0);
  }

  /// 释放一份引用；恰好是最后一份时唤醒等在那里的销毁者
  void unref() noexcept
  {
    const std::uint32_t state = m_state.fetch_sub(kReferenceUnit, std::memory_order_acq_rel) - kReferenceUnit;

    /* 计数不得下溢，也不得对已销毁的锁再释放 */
    assert((state & kDestroyMask & ~kDestroyFlag) == 0);

    if (state == kDestroyFlag)
      m_state.notify_one();
  }

  /// 放下自己那份引用并置上销毁标记，然后等其它引用全部放完
  void unrefAndDestroy() noexcept
  {
    /* fetch_add 返回的是加之前的值，而这里要判断加之后的状态，故自己补上增量 */
    const std::uint32_t delta = kDestroyFlag - kReferenceUnit;
    const std::uint32_t state = m_state.fetch_add(delta, std::memory_order_acq_rel) + delta;

    /* 销毁只能发生一次，且必须由持有引用的那一方发起 */
    assert((state & kDestroyMask) == kDestroyFlag);

    /* 还有别的引用在外面就一直睡；醒来重读，虚假唤醒因此无害 */
    std::uint32_t current = state;
    while ((current & kReferenceMask) != 0)
    {
      m_state.wait(current, std::memory_order_relaxed);
      current = m_state.load(std::memory_order_relaxed);
    }

    const std::uint32_t previous = m_state.exchange(kPoisonValue, std::memory_order_acq_rel);
    assert(previous == kDestroyFlag);
  }

private:
  static constexpr std::uint32_t kReferenceUnit = 0x00000001U; ///< 引用计数的单位增量
  static constexpr std::uint32_t kReferenceMask = 0x0fffffffU; ///< 低 28 位：引用计数
  static constexpr std::uint32_t kDestroyFlag   = 0x10000000U; ///< 第 28 位：已请求销毁
  static constexpr std::uint32_t kDestroyMask   = 0xf0000000U; ///< 高 4 位：销毁标记区
  static constexpr std::uint32_t kPoisonValue   = 0x300dead0U; ///< 销毁完成后写入的毒值

  std::atomic<std::uint32_t> m_state{0}; ///< 计数与销毁标记打包在一个字里：等待与唤醒按它的地址工作
};

#include <stdbool.h>

/* N.b.: the tree functions do not set errno or LastError when they fail. Each
 * of the API functions has at most one failure mode. It is up to the caller to
 * set an appropriate error code when necessary. */

typedef struct tree_node tree_node_t;

typedef struct tree_node {
  uintptr_t key; /* 节点只留键：树的形状信息改由标准容器保管 */
} tree_node_t;

/* 键 → 节点。原先是一棵手写的红黑树（插入、删除、旋转、重平衡近 200 行），而调用方只用到
 * 「按键插入、按键查找、按键删除」三件事，因此交给标准库的有序表：std::map 同样按键有序，
 * 且节点地址稳定（再平衡不会让节点搬家），下面用 container_of 从节点取回宿主结构照旧成立。 */
typedef std::map<uintptr_t, tree_node_t*> tree_t;

WEPOLL_INTERNAL void tree_init(tree_t* tree);
WEPOLL_INTERNAL void tree_node_init(tree_node_t* node);

WEPOLL_INTERNAL int tree_add(tree_t* tree, tree_node_t* node, uintptr_t key);
WEPOLL_INTERNAL void tree_del(tree_t* tree, tree_node_t* node);

WEPOLL_INTERNAL tree_node_t* tree_find(const tree_t* tree, uintptr_t key);
WEPOLL_INTERNAL tree_node_t* tree_root(const tree_t* tree);

typedef struct ts_tree {
  tree_t tree;
  SRWLOCK lock;
} ts_tree_t;

typedef struct ts_tree_node {
  tree_node_t tree_node;
  RefLock reflock;
} ts_tree_node_t;

WEPOLL_INTERNAL void ts_tree_init(ts_tree_t* rtl);
WEPOLL_INTERNAL void ts_tree_node_init(ts_tree_node_t* node);

WEPOLL_INTERNAL int ts_tree_add(ts_tree_t* ts_tree,
                                ts_tree_node_t* node,
                                uintptr_t key);

WEPOLL_INTERNAL ts_tree_node_t* ts_tree_del_and_ref(ts_tree_t* ts_tree,
                                                    uintptr_t key);
WEPOLL_INTERNAL ts_tree_node_t* ts_tree_find_and_ref(ts_tree_t* ts_tree,
                                                     uintptr_t key);

WEPOLL_INTERNAL void ts_tree_node_unref(ts_tree_node_t* node);
WEPOLL_INTERNAL void ts_tree_node_unref_and_destroy(ts_tree_node_t* node);

static ts_tree_t epoll__handle_tree;

int epoll_global_init(void) {
  ts_tree_init(&epoll__handle_tree);
  return 0;
}

static HANDLE epoll__create(void) {
  port_state_t* port_state;
  HANDLE ephnd;
  ts_tree_node_t* tree_node;

  if (init() < 0)
    return NULL;

  port_state = port_new(&ephnd);
  if (port_state == NULL)
    return NULL;

  tree_node = port_state_to_handle_tree_node(port_state);
  if (ts_tree_add(&epoll__handle_tree, tree_node, (uintptr_t) ephnd) < 0) {
    /* This should never happen. */
    port_delete(port_state);
    return_set_error(NULL, ERROR_ALREADY_EXISTS);
  }

  return ephnd;
}

HANDLE epoll_create(int size) {
  if (size <= 0)
    return_set_error(NULL, ERROR_INVALID_PARAMETER);

  return epoll__create();
}

HANDLE epoll_create1(int flags) {
  if (flags != 0)
    return_set_error(NULL, ERROR_INVALID_PARAMETER);

  return epoll__create();
}

int epoll_close(HANDLE ephnd) {
  ts_tree_node_t* tree_node;
  port_state_t* port_state;

  if (init() < 0)
    return -1;

  tree_node = ts_tree_del_and_ref(&epoll__handle_tree, (uintptr_t) ephnd);
  if (tree_node == NULL) {
    err_set_win_error(ERROR_INVALID_PARAMETER);

    /* 与其它入口同一口径：句柄自身的错误优先于「表里找不到」 */
    err_check_handle(ephnd);
    return -1;
  }

  port_state = port_state_from_handle_tree_node(tree_node);
  port_close(port_state);

  ts_tree_node_unref_and_destroy(tree_node);

  return port_delete(port_state);
}

int epoll_ctl(HANDLE ephnd, int op, SOCKET sock, struct epoll_event* ev) {
  ts_tree_node_t* tree_node;
  port_state_t* port_state;
  int r;

  if (init() < 0)
    return -1;

  tree_node = ts_tree_find_and_ref(&epoll__handle_tree, (uintptr_t) ephnd);
  if (tree_node == NULL) {
    err_set_win_error(ERROR_INVALID_PARAMETER);
    /* On Linux, in the case of epoll_ctl(), EBADF takes priority over other
     * errors. Wepoll mimics this behavior. */
    err_check_handle(ephnd);
    err_check_handle((HANDLE) sock);
    return -1;
  }

  port_state = port_state_from_handle_tree_node(tree_node);
  r = port_ctl(port_state, op, sock, ev);

  ts_tree_node_unref(tree_node);

  if (r < 0) {
    /* 与上面同一口径：两行重复换来一个没有跳转的单一出口 */
    err_check_handle(ephnd);
    err_check_handle((HANDLE) sock);
    return -1;
  }

  return 0;
}

int epoll_wait(HANDLE ephnd,
               struct epoll_event* events,
               int maxevents,
               int timeout) {
  ts_tree_node_t* tree_node;
  port_state_t* port_state;
  int num_events;

  if (maxevents <= 0)
    return_set_error(-1, ERROR_INVALID_PARAMETER);

  if (init() < 0)
    return -1;

  tree_node = ts_tree_find_and_ref(&epoll__handle_tree, (uintptr_t) ephnd);
  if (tree_node == NULL) {
    err_set_win_error(ERROR_INVALID_PARAMETER);
    err_check_handle(ephnd);
    return -1;
  }

  port_state = port_state_from_handle_tree_node(tree_node);
  num_events = port_wait(port_state, events, maxevents, timeout);

  ts_tree_node_unref(tree_node);

  if (num_events < 0) {
    err_check_handle(ephnd);
    return -1;
  }

  return num_events;
}

#include <errno.h>

#define ERR__ERRNO_MAPPINGS(X)               \
  X(ERROR_ACCESS_DENIED, EACCES)             \
  X(ERROR_ALREADY_EXISTS, EEXIST)            \
  X(ERROR_BAD_COMMAND, EACCES)               \
  X(ERROR_BAD_EXE_FORMAT, ENOEXEC)           \
  X(ERROR_BAD_LENGTH, EACCES)                \
  X(ERROR_BAD_NETPATH, ENOENT)               \
  X(ERROR_BAD_NET_NAME, ENOENT)              \
  X(ERROR_BAD_NET_RESP, ENETDOWN)            \
  X(ERROR_BAD_PATHNAME, ENOENT)              \
  X(ERROR_BROKEN_PIPE, EPIPE)                \
  X(ERROR_CANNOT_MAKE, EACCES)               \
  X(ERROR_COMMITMENT_LIMIT, ENOMEM)          \
  X(ERROR_CONNECTION_ABORTED, ECONNABORTED)  \
  X(ERROR_CONNECTION_ACTIVE, EISCONN)        \
  X(ERROR_CONNECTION_REFUSED, ECONNREFUSED)  \
  X(ERROR_CRC, EACCES)                       \
  X(ERROR_DIR_NOT_EMPTY, ENOTEMPTY)          \
  X(ERROR_DISK_FULL, ENOSPC)                 \
  X(ERROR_DUP_NAME, EADDRINUSE)              \
  X(ERROR_FILENAME_EXCED_RANGE, ENOENT)      \
  X(ERROR_FILE_NOT_FOUND, ENOENT)            \
  X(ERROR_GEN_FAILURE, EACCES)               \
  X(ERROR_GRACEFUL_DISCONNECT, EPIPE)        \
  X(ERROR_HOST_DOWN, EHOSTUNREACH)           \
  X(ERROR_HOST_UNREACHABLE, EHOSTUNREACH)    \
  X(ERROR_INSUFFICIENT_BUFFER, EFAULT)       \
  X(ERROR_INVALID_ADDRESS, EADDRNOTAVAIL)    \
  X(ERROR_INVALID_FUNCTION, EINVAL)          \
  X(ERROR_INVALID_HANDLE, EBADF)             \
  X(ERROR_INVALID_NETNAME, EADDRNOTAVAIL)    \
  X(ERROR_INVALID_PARAMETER, EINVAL)         \
  X(ERROR_INVALID_USER_BUFFER, EMSGSIZE)     \
  X(ERROR_IO_PENDING, EINPROGRESS)           \
  X(ERROR_LOCK_VIOLATION, EACCES)            \
  X(ERROR_MORE_DATA, EMSGSIZE)               \
  X(ERROR_NETNAME_DELETED, ECONNABORTED)     \
  X(ERROR_NETWORK_ACCESS_DENIED, EACCES)     \
  X(ERROR_NETWORK_BUSY, ENETDOWN)            \
  X(ERROR_NETWORK_UNREACHABLE, ENETUNREACH)  \
  X(ERROR_NOACCESS, EFAULT)                  \
  X(ERROR_NONPAGED_SYSTEM_RESOURCES, ENOMEM) \
  X(ERROR_NOT_ENOUGH_MEMORY, ENOMEM)         \
  X(ERROR_NOT_ENOUGH_QUOTA, ENOMEM)          \
  X(ERROR_NOT_FOUND, ENOENT)                 \
  X(ERROR_NOT_LOCKED, EACCES)                \
  X(ERROR_NOT_READY, EACCES)                 \
  X(ERROR_NOT_SAME_DEVICE, EXDEV)            \
  X(ERROR_NOT_SUPPORTED, ENOTSUP)            \
  X(ERROR_NO_MORE_FILES, ENOENT)             \
  X(ERROR_NO_SYSTEM_RESOURCES, ENOMEM)       \
  X(ERROR_OPERATION_ABORTED, EINTR)          \
  X(ERROR_OUT_OF_PAPER, EACCES)              \
  X(ERROR_PAGED_SYSTEM_RESOURCES, ENOMEM)    \
  X(ERROR_PAGEFILE_QUOTA, ENOMEM)            \
  X(ERROR_PATH_NOT_FOUND, ENOENT)            \
  X(ERROR_PIPE_NOT_CONNECTED, EPIPE)         \
  X(ERROR_PORT_UNREACHABLE, ECONNRESET)      \
  X(ERROR_PROTOCOL_UNREACHABLE, ENETUNREACH) \
  X(ERROR_REM_NOT_LIST, ECONNREFUSED)        \
  X(ERROR_REQUEST_ABORTED, EINTR)            \
  X(ERROR_REQ_NOT_ACCEP, EWOULDBLOCK)        \
  X(ERROR_SECTOR_NOT_FOUND, EACCES)          \
  X(ERROR_SEM_TIMEOUT, ETIMEDOUT)            \
  X(ERROR_SHARING_VIOLATION, EACCES)         \
  X(ERROR_TOO_MANY_NAMES, ENOMEM)            \
  X(ERROR_TOO_MANY_OPEN_FILES, EMFILE)       \
  X(ERROR_UNEXP_NET_ERR, ECONNABORTED)       \
  X(ERROR_WAIT_NO_CHILDREN, ECHILD)          \
  X(ERROR_WORKING_SET_QUOTA, ENOMEM)         \
  X(ERROR_WRITE_PROTECT, EACCES)             \
  X(ERROR_WRONG_DISK, EACCES)                \
  X(WSAEACCES, EACCES)                       \
  X(WSAEADDRINUSE, EADDRINUSE)               \
  X(WSAEADDRNOTAVAIL, EADDRNOTAVAIL)         \
  X(WSAEAFNOSUPPORT, EAFNOSUPPORT)           \
  X(WSAECONNABORTED, ECONNABORTED)           \
  X(WSAECONNREFUSED, ECONNREFUSED)           \
  X(WSAECONNRESET, ECONNRESET)               \
  X(WSAEDISCON, EPIPE)                       \
  X(WSAEFAULT, EFAULT)                       \
  X(WSAEHOSTDOWN, EHOSTUNREACH)              \
  X(WSAEHOSTUNREACH, EHOSTUNREACH)           \
  X(WSAEINPROGRESS, EBUSY)                   \
  X(WSAEINTR, EINTR)                         \
  X(WSAEINVAL, EINVAL)                       \
  X(WSAEISCONN, EISCONN)                     \
  X(WSAEMSGSIZE, EMSGSIZE)                   \
  X(WSAENETDOWN, ENETDOWN)                   \
  X(WSAENETRESET, EHOSTUNREACH)              \
  X(WSAENETUNREACH, ENETUNREACH)             \
  X(WSAENOBUFS, ENOMEM)                      \
  X(WSAENOTCONN, ENOTCONN)                   \
  X(WSAENOTSOCK, ENOTSOCK)                   \
  X(WSAEOPNOTSUPP, EOPNOTSUPP)               \
  X(WSAEPROCLIM, ENOMEM)                     \
  X(WSAESHUTDOWN, EPIPE)                     \
  X(WSAETIMEDOUT, ETIMEDOUT)                 \
  X(WSAEWOULDBLOCK, EWOULDBLOCK)             \
  X(WSANOTINITIALISED, ENETDOWN)             \
  X(WSASYSNOTREADY, ENETDOWN)                \
  X(WSAVERNOTSUPPORTED, ENOSYS)

static errno_t err__map_win_error_to_errno(DWORD error) {
  switch (error) {
#define X(error_sym, errno_sym) \
  case error_sym:               \
    return errno_sym;
    ERR__ERRNO_MAPPINGS(X)
#undef X
  }
  return EINVAL;
}

void err_map_win_error(void) {
  errno = err__map_win_error_to_errno(GetLastError());
}

void err_set_win_error(DWORD error) {
  SetLastError(error);
  errno = err__map_win_error_to_errno(error);
}

int err_check_handle(HANDLE handle) {
  DWORD flags;

  /* GetHandleInformation() succeeds when passed INVALID_HANDLE_VALUE, so check
   * for this condition explicitly. */
  if (handle == INVALID_HANDLE_VALUE)
    return_set_error(-1, ERROR_INVALID_HANDLE);

  if (!GetHandleInformation(handle, &flags))
    return_map_error(-1);

  return 0;
}

#include <stddef.h>

#define array_count(a) (sizeof(a) / (sizeof((a)[0])))

#define container_of(ptr, type, member) \
  ((type*) ((uintptr_t) (ptr) - offsetof(type, member)))

#define unused_var(v) ((void) (v))

/* Polyfill `inline` for older versions of msvc (up to Visual Studio 2013) */
#if defined(_MSC_VER) && _MSC_VER < 1900
#define inline __inline
#endif

WEPOLL_INTERNAL int ws_global_init(void);
WEPOLL_INTERNAL SOCKET ws_get_base_socket(SOCKET socket);

static bool init__done = false;
static INIT_ONCE init__once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init__once_callback(INIT_ONCE* once,
                                         void* parameter,
                                         void** context) {
  unused_var(once);
  unused_var(parameter);
  unused_var(context);

  /* N.b. that initialization order matters here. */
  if (ws_global_init() < 0 || nt_global_init() < 0 ||
      epoll_global_init() < 0)
    return FALSE;

  init__done = true;
  return TRUE;
}

int init(void) {
  if (!init__done &&
      !InitOnceExecuteOnce(&init__once, init__once_callback, NULL, NULL))
    /* `InitOnceExecuteOnce()` itself is infallible, and it doesn't set any
     * error code when the once-callback returns FALSE. We return -1 here to
     * indicate that global initialization failed; the failing init function is
     * resposible for setting `errno` and calling `SetLastError()`. */
    return -1;

  return 0;
}

/* Set up a workaround for the following problem:
 *   FARPROC addr = GetProcAddress(...);
 *   MY_FUNC func = (MY_FUNC) addr;          <-- GCC 8 warning/error.
 *   MY_FUNC func = (MY_FUNC) (void*) addr;  <-- MSVC  warning/error.
 * To compile cleanly with either compiler, do casts with this "bridge" type:
 *   MY_FUNC func = (MY_FUNC) (nt__fn_ptr_cast_t) addr; */
#ifdef __GNUC__
typedef void* nt__fn_ptr_cast_t;
#else
typedef FARPROC nt__fn_ptr_cast_t;
#endif

int nt_global_init(void) {
  HMODULE ntdll;
  FARPROC fn_ptr;

  ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == NULL)
    return -1;

#define X(return_type, attributes, name, parameters) \
  fn_ptr = GetProcAddress(ntdll, #name);             \
  if (fn_ptr == NULL)                                \
    return -1;                                       \
  name = (return_type(attributes*) parameters)(nt__fn_ptr_cast_t) fn_ptr;
  NT_NTDLL_IMPORT_LIST(X)
#undef X

  return 0;
}

#include <string.h>

typedef struct poll_group poll_group_t;

typedef struct queue_node queue_node_t;

WEPOLL_INTERNAL poll_group_t* poll_group_acquire(port_state_t* port);
WEPOLL_INTERNAL void poll_group_release(poll_group_t* poll_group);

WEPOLL_INTERNAL void poll_group_delete(poll_group_t* poll_group);

WEPOLL_INTERNAL poll_group_t* poll_group_from_queue_node(
    queue_node_t* queue_node);
WEPOLL_INTERNAL HANDLE
    poll_group_get_afd_helper_handle(poll_group_t* poll_group);

typedef struct queue_node {
  queue_node_t* previous;
  queue_node_t* next;
} queue_node_t;

/* 带哨兵头的侵入式双向链表：节点内联在持有者里（不进堆），因此「从节点摘除」是 O(1)、
   队列本身一次分配都不用做。本文件的两处用法都吃这个代价——每轮 epoll_wait 之后有成百上千
   个 socket 要在这几张表之间挪动。改用 std::list 会让每个节点多一次分配，还得把迭代器塞回
   节点才能保住同样的摘除代价，得不偿失。 */
typedef struct queue {
  queue_node_t head; /* 哨兵：head.next 是首元素、head.previous 是尾元素；空表时两者都指回 head */
} queue_t;

WEPOLL_INTERNAL void queueInit(queue_t* queue);
WEPOLL_INTERNAL void queueNodeInit(queue_node_t* node);

WEPOLL_INTERNAL queue_node_t* queueFirst(const queue_t* queue);
WEPOLL_INTERNAL queue_node_t* queueLast(const queue_t* queue);

WEPOLL_INTERNAL void queueAppend(queue_t* queue, queue_node_t* node);
WEPOLL_INTERNAL void queueMoveToStart(queue_t* queue, queue_node_t* node);
WEPOLL_INTERNAL void queueMoveToEnd(queue_t* queue, queue_node_t* node);
WEPOLL_INTERNAL void queueRemove(queue_node_t* node);

WEPOLL_INTERNAL bool queueIsEmpty(const queue_t* queue);
WEPOLL_INTERNAL bool queueIsEnqueued(const queue_node_t* node);

static const size_t POLL_GROUP__MAX_GROUP_SIZE = 32;

typedef struct poll_group {
  port_state_t* port_state;
  queue_node_t queue_node;
  HANDLE afd_helper_handle;
  size_t group_size;
} poll_group_t;

static poll_group_t* poll_group__new(port_state_t* port_state) {
  HANDLE iocp_handle = port_get_iocp_handle(port_state);
  queue_t* poll_group_queue = port_get_poll_group_queue(port_state);

  /* nothrow 版本：失败返回空指针，与「失败即返回 NULL + 设 LastError」的约定一致。
     值初始化把各成员清零，队列节点仍要显式初始化成自指的哨兵 */
  poll_group_t* poll_group = new (std::nothrow) poll_group_t{};
  if (poll_group == NULL)
    return_set_error(NULL, ERROR_NOT_ENOUGH_MEMORY);

  queueNodeInit(&poll_group->queue_node);
  poll_group->port_state = port_state;

  if (afd_create_helper_handle(iocp_handle, &poll_group->afd_helper_handle) <
      0) {
    delete poll_group;
    return NULL;
  }

  queueAppend(poll_group_queue, &poll_group->queue_node);

  return poll_group;
}

void poll_group_delete(poll_group_t* poll_group) {
  assert(poll_group->group_size == 0);
  CloseHandle(poll_group->afd_helper_handle);
  queueRemove(&poll_group->queue_node);
  delete poll_group;
}

poll_group_t* poll_group_from_queue_node(queue_node_t* queue_node) {
  return container_of(queue_node, poll_group_t, queue_node);
}

HANDLE poll_group_get_afd_helper_handle(poll_group_t* poll_group) {
  return poll_group->afd_helper_handle;
}

poll_group_t* poll_group_acquire(port_state_t* port_state) {
  queue_t* poll_group_queue = port_get_poll_group_queue(port_state);
  poll_group_t* poll_group =
      !queueIsEmpty(poll_group_queue)
          ? container_of(
                queueLast(poll_group_queue), poll_group_t, queue_node)
          : NULL;

  if (poll_group == NULL ||
      poll_group->group_size >= POLL_GROUP__MAX_GROUP_SIZE)
    poll_group = poll_group__new(port_state);
  if (poll_group == NULL)
    return NULL;

  if (++poll_group->group_size == POLL_GROUP__MAX_GROUP_SIZE)
    queueMoveToStart(poll_group_queue, &poll_group->queue_node);

  return poll_group;
}

void poll_group_release(poll_group_t* poll_group) {
  port_state_t* port_state = poll_group->port_state;
  queue_t* poll_group_queue = port_get_poll_group_queue(port_state);

  poll_group->group_size--;
  assert(poll_group->group_size < POLL_GROUP__MAX_GROUP_SIZE);

  queueMoveToEnd(poll_group_queue, &poll_group->queue_node);

  /* Poll groups are currently only freed when the epoll port is closed. */
}

WEPOLL_INTERNAL sock_state_t* sock_new(port_state_t* port_state,
                                       SOCKET socket);
WEPOLL_INTERNAL void sock_delete(port_state_t* port_state,
                                 sock_state_t* sock_state);
WEPOLL_INTERNAL void sock_force_delete(port_state_t* port_state,
                                       sock_state_t* sock_state);

WEPOLL_INTERNAL int sock_set_event(port_state_t* port_state,
                                   sock_state_t* sock_state,
                                   const struct epoll_event* ev);

WEPOLL_INTERNAL int sock_update(port_state_t* port_state,
                                sock_state_t* sock_state);
WEPOLL_INTERNAL int sock_feed_event(port_state_t* port_state,
                                    IO_STATUS_BLOCK* io_status_block,
                                    struct epoll_event* ev);

WEPOLL_INTERNAL sock_state_t* sock_state_from_queue_node(
    queue_node_t* queue_node);
WEPOLL_INTERNAL queue_node_t* sock_state_to_queue_node(
    sock_state_t* sock_state);
WEPOLL_INTERNAL sock_state_t* sock_state_from_tree_node(
    tree_node_t* tree_node);
WEPOLL_INTERNAL tree_node_t* sock_state_to_tree_node(sock_state_t* sock_state);

#define PORT__MAX_ON_STACK_COMPLETIONS 256

typedef struct port_state {
  HANDLE iocp_handle;
  tree_t sock_tree;
  queue_t sock_update_queue;
  queue_t sock_deleted_queue;
  queue_t poll_group_queue;
  ts_tree_node_t handle_tree_node;
  CRITICAL_SECTION lock;
  size_t active_poll_count;
} port_state_t;

static port_state_t* port__alloc(void) {
  /* 值初始化：平凡成员清零，sock_tree 与引用锁各自构造好——不再需要 memset 之后再补构造 */
  port_state_t* port_state = new (std::nothrow) port_state_t{};
  if (port_state == NULL)
    return_set_error(NULL, ERROR_NOT_ENOUGH_MEMORY);

  return port_state;
}

static void port__free(port_state_t* port) {
  assert(port != NULL);
  /* 与 new 配对：容器成员由析构自动收尾 */
  delete port;
}

static HANDLE port__create_iocp(void) {
  HANDLE iocp_handle =
      CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
  if (iocp_handle == NULL)
    return_map_error(NULL);

  return iocp_handle;
}

port_state_t* port_new(HANDLE* iocp_handle_out) {
  port_state_t* port_state;
  HANDLE iocp_handle;

  port_state = port__alloc();
  if (port_state == NULL)
    return NULL;

  iocp_handle = port__create_iocp();
  if (iocp_handle == NULL) {
    port__free(port_state);
    return NULL;
  }

  port_state->iocp_handle = iocp_handle;
  queueInit(&port_state->sock_update_queue);
  queueInit(&port_state->sock_deleted_queue);
  queueInit(&port_state->poll_group_queue);
  ts_tree_node_init(&port_state->handle_tree_node);
  InitializeCriticalSection(&port_state->lock);

  *iocp_handle_out = iocp_handle;
  return port_state;
}

static int port__close_iocp(port_state_t* port_state) {
  HANDLE iocp_handle = port_state->iocp_handle;
  port_state->iocp_handle = NULL;

  if (!CloseHandle(iocp_handle))
    return_map_error(-1);

  return 0;
}

int port_close(port_state_t* port_state) {
  int result;

  EnterCriticalSection(&port_state->lock);
  result = port__close_iocp(port_state);
  LeaveCriticalSection(&port_state->lock);

  return result;
}

int port_delete(port_state_t* port_state) {
  tree_node_t* tree_node;
  queue_node_t* queue_node;

  /* At this point the IOCP port should have been closed. */
  assert(port_state->iocp_handle == NULL);

  while ((tree_node = tree_root(&port_state->sock_tree)) != NULL) {
    sock_state_t* sock_state = sock_state_from_tree_node(tree_node);
    sock_force_delete(port_state, sock_state);
  }

  while ((queue_node = queueFirst(&port_state->sock_deleted_queue)) != NULL) {
    sock_state_t* sock_state = sock_state_from_queue_node(queue_node);
    sock_force_delete(port_state, sock_state);
  }

  while ((queue_node = queueFirst(&port_state->poll_group_queue)) != NULL) {
    poll_group_t* poll_group = poll_group_from_queue_node(queue_node);
    poll_group_delete(poll_group);
  }

  assert(queueIsEmpty(&port_state->sock_update_queue));

  DeleteCriticalSection(&port_state->lock);

  port__free(port_state);

  return 0;
}

static int port__update_events(port_state_t* port_state) {
  queue_t* sock_update_queue = &port_state->sock_update_queue;

  /* Walk the queue, submitting new poll requests for every socket that needs
   * it. */
  while (!queueIsEmpty(sock_update_queue)) {
    queue_node_t* queue_node = queueFirst(sock_update_queue);
    sock_state_t* sock_state = sock_state_from_queue_node(queue_node);

    if (sock_update(port_state, sock_state) < 0)
      return -1;

    /* sock_update() removes the socket from the update queue. */
  }

  return 0;
}

static void port__update_events_if_polling(port_state_t* port_state) {
  if (port_state->active_poll_count > 0)
    port__update_events(port_state);
}

static int port__feed_events(port_state_t* port_state,
                             struct epoll_event* epoll_events,
                             OVERLAPPED_ENTRY* iocp_events,
                             DWORD iocp_event_count) {
  int epoll_event_count = 0;
  DWORD i;

  for (i = 0; i < iocp_event_count; i++) {
    IO_STATUS_BLOCK* io_status_block =
        (IO_STATUS_BLOCK*) iocp_events[i].lpOverlapped;
    struct epoll_event* ev = &epoll_events[epoll_event_count];

    epoll_event_count += sock_feed_event(port_state, io_status_block, ev);
  }

  return epoll_event_count;
}

static int port__poll(port_state_t* port_state,
                      struct epoll_event* epoll_events,
                      OVERLAPPED_ENTRY* iocp_events,
                      DWORD maxevents,
                      DWORD timeout) {
  DWORD completion_count;

  if (port__update_events(port_state) < 0)
    return -1;

  port_state->active_poll_count++;

  LeaveCriticalSection(&port_state->lock);

  BOOL r = GetQueuedCompletionStatusEx(port_state->iocp_handle,
                                       iocp_events,
                                       maxevents,
                                       &completion_count,
                                       timeout,
                                       FALSE);

  EnterCriticalSection(&port_state->lock);

  port_state->active_poll_count--;

  if (!r)
    return_map_error(-1);

  return port__feed_events(
      port_state, epoll_events, iocp_events, completion_count);
}

int port_wait(port_state_t* port_state,
              struct epoll_event* events,
              int maxevents,
              int timeout) {
  OVERLAPPED_ENTRY stack_iocp_events[PORT__MAX_ON_STACK_COMPLETIONS];
  OVERLAPPED_ENTRY* iocp_events = stack_iocp_events;
  /* 只有 maxevents 很大时才上堆：改用 unique_ptr 后所有返回路径都自动归还，不必再手工 free */
  std::unique_ptr<OVERLAPPED_ENTRY[]> heap_iocp_events;
  uint64_t due = 0;
  DWORD gqcs_timeout;
  int result;

  /* Check whether `maxevents` is in range. */
  if (maxevents <= 0)
    return_set_error(-1, ERROR_INVALID_PARAMETER);

  /* Decide whether the IOCP completion list can live on the stack, or allocate
   * memory for it on the heap. The heap allocation is nothrow on purpose: if it
   * fails, fall back to the stack array instead of failing the whole wait. */
  if (static_cast<std::size_t>(maxevents) > array_count(stack_iocp_events)) {
    heap_iocp_events.reset(
        new (std::nothrow) OVERLAPPED_ENTRY[static_cast<std::size_t>(maxevents)]);

    if (heap_iocp_events != nullptr)
      iocp_events = heap_iocp_events.get();
    else
      maxevents = static_cast<int>(array_count(stack_iocp_events));
  }

  /* Compute the timeout for GetQueuedCompletionStatus, and the wait end
   * time, if the user specified a timeout other than zero or infinite. */
  if (timeout > 0) {
    due = GetTickCount64() + (uint64_t) timeout;
    gqcs_timeout = (DWORD) timeout;
  } else if (timeout == 0) {
    gqcs_timeout = 0;
  } else {
    gqcs_timeout = INFINITE;
  }

  EnterCriticalSection(&port_state->lock);

  /* Dequeue completion packets until either at least one interesting event
   * has been discovered, or the timeout is reached. */
  for (;;) {
    uint64_t now;

    result = port__poll(
        port_state, events, iocp_events, (DWORD) maxevents, gqcs_timeout);
    if (result < 0 || result > 0)
      break; /* Result, error, or time-out. */

    if (timeout < 0)
      continue; /* When timeout is negative, never time out. */

    /* Update time. */
    now = GetTickCount64();

    /* Do not allow the due time to be in the past. */
    if (now >= due) {
      SetLastError(WAIT_TIMEOUT);
      break;
    }

    /* Recompute time-out argument for GetQueuedCompletionStatus. */
    gqcs_timeout = (DWORD)(due - now);
  }

  port__update_events_if_polling(port_state);

  LeaveCriticalSection(&port_state->lock);

  if (result >= 0)
    return result;
  else if (GetLastError() == WAIT_TIMEOUT)
    return 0;
  else
    return -1;
}

static int port__ctl_add(port_state_t* port_state,
                         SOCKET sock,
                         struct epoll_event* ev) {
  sock_state_t* sock_state = sock_new(port_state, sock);
  if (sock_state == NULL)
    return -1;

  if (sock_set_event(port_state, sock_state, ev) < 0) {
    sock_delete(port_state, sock_state);
    return -1;
  }

  port__update_events_if_polling(port_state);

  return 0;
}

static int port__ctl_mod(port_state_t* port_state,
                         SOCKET sock,
                         struct epoll_event* ev) {
  sock_state_t* sock_state = port_find_socket(port_state, sock);
  if (sock_state == NULL)
    return -1;

  if (sock_set_event(port_state, sock_state, ev) < 0)
    return -1;

  port__update_events_if_polling(port_state);

  return 0;
}

static int port__ctl_del(port_state_t* port_state, SOCKET sock) {
  sock_state_t* sock_state = port_find_socket(port_state, sock);
  if (sock_state == NULL)
    return -1;

  sock_delete(port_state, sock_state);

  return 0;
}

static int port__ctl_op(port_state_t* port_state,
                        int op,
                        SOCKET sock,
                        struct epoll_event* ev) {
  switch (op) {
    case EPOLL_CTL_ADD:
      return port__ctl_add(port_state, sock, ev);
    case EPOLL_CTL_MOD:
      return port__ctl_mod(port_state, sock, ev);
    case EPOLL_CTL_DEL:
      return port__ctl_del(port_state, sock);
    default:
      return_set_error(-1, ERROR_INVALID_PARAMETER);
  }
}

int port_ctl(port_state_t* port_state,
             int op,
             SOCKET sock,
             struct epoll_event* ev) {
  int result;

  EnterCriticalSection(&port_state->lock);
  result = port__ctl_op(port_state, op, sock, ev);
  LeaveCriticalSection(&port_state->lock);

  return result;
}

int port_register_socket(port_state_t* port_state,
                         sock_state_t* sock_state,
                         SOCKET socket) {
  if (tree_add(&port_state->sock_tree,
               sock_state_to_tree_node(sock_state),
               socket) < 0)
    return_set_error(-1, ERROR_ALREADY_EXISTS);
  return 0;
}

void port_unregister_socket(port_state_t* port_state,
                            sock_state_t* sock_state) {
  tree_del(&port_state->sock_tree, sock_state_to_tree_node(sock_state));
}

sock_state_t* port_find_socket(port_state_t* port_state, SOCKET socket) {
  tree_node_t* tree_node = tree_find(&port_state->sock_tree, socket);
  if (tree_node == NULL)
    return_set_error(NULL, ERROR_NOT_FOUND);
  return sock_state_from_tree_node(tree_node);
}

void port_request_socket_update(port_state_t* port_state,
                                sock_state_t* sock_state) {
  if (queueIsEnqueued(sock_state_to_queue_node(sock_state)))
    return;
  queueAppend(&port_state->sock_update_queue,
               sock_state_to_queue_node(sock_state));
}

void port_cancel_socket_update(port_state_t* port_state,
                               sock_state_t* sock_state) {
  unused_var(port_state);
  if (!queueIsEnqueued(sock_state_to_queue_node(sock_state)))
    return;
  queueRemove(sock_state_to_queue_node(sock_state));
}

void port_add_deleted_socket(port_state_t* port_state,
                             sock_state_t* sock_state) {
  if (queueIsEnqueued(sock_state_to_queue_node(sock_state)))
    return;
  queueAppend(&port_state->sock_deleted_queue,
               sock_state_to_queue_node(sock_state));
}

void port_remove_deleted_socket(port_state_t* port_state,
                                sock_state_t* sock_state) {
  unused_var(port_state);
  if (!queueIsEnqueued(sock_state_to_queue_node(sock_state)))
    return;
  queueRemove(sock_state_to_queue_node(sock_state));
}

HANDLE port_get_iocp_handle(port_state_t* port_state) {
  assert(port_state->iocp_handle != NULL);
  return port_state->iocp_handle;
}

queue_t* port_get_poll_group_queue(port_state_t* port_state) {
  return &port_state->poll_group_queue;
}

port_state_t* port_state_from_handle_tree_node(ts_tree_node_t* tree_node) {
  return container_of(tree_node, port_state_t, handle_tree_node);
}

ts_tree_node_t* port_state_to_handle_tree_node(port_state_t* port_state) {
  return &port_state->handle_tree_node;
}

void queueInit(queue_t* queue) {
  queueNodeInit(&queue->head);
}

void queueNodeInit(queue_node_t* node) {
  /* 自指即「不在任何表里」：入队与摘除都靠这条不变式判断，因此不需要额外的标记位 */
  node->previous = node;
  node->next     = node;
}

/* 把节点从它当前所在的表里摘下来，不动它自己的 previous/next 指向。
   remove 与两个 move_* 共用，改动这里要同时顾到「被摘的可能是首元素或尾元素」。 */
static inline void queueDetachNode(queue_node_t* node) {
  node->previous->next = node->next;
  node->next->previous = node->previous;
}

queue_node_t* queueFirst(const queue_t* queue) {
  return !queueIsEmpty(queue) ? queue->head.next : nullptr;
}

queue_node_t* queueLast(const queue_t* queue) {
  return !queueIsEmpty(queue) ? queue->head.previous : nullptr;
}

void queueAppend(queue_t* queue, queue_node_t* node) {
  node->next             = &queue->head;
  node->previous         = queue->head.previous;
  node->previous->next   = node;
  queue->head.previous   = node;
}

void queueMoveToStart(queue_t* queue, queue_node_t* node) {
  queueDetachNode(node);

  /* 插到哨兵之后即队首（这里不再单列一个 prepend：只有这一处用它） */
  node->next           = queue->head.next;
  node->previous       = &queue->head;
  node->next->previous = node;
  queue->head.next     = node;
}

void queueMoveToEnd(queue_t* queue, queue_node_t* node) {
  queueDetachNode(node);
  queueAppend(queue, node);
}

void queueRemove(queue_node_t* node) {
  queueDetachNode(node);
  queueNodeInit(node);
}

bool queueIsEmpty(const queue_t* queue) {
  return !queueIsEnqueued(&queue->head);
}

bool queueIsEnqueued(const queue_node_t* node) {
  return node->previous != node;
}


static const uint32_t SOCK__KNOWN_EPOLL_EVENTS =
    EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDNORM |
    EPOLLRDBAND | EPOLLWRNORM | EPOLLWRBAND | EPOLLMSG | EPOLLRDHUP;

typedef enum sock__poll_status {
  SOCK__POLL_IDLE = 0,
  SOCK__POLL_PENDING,
  SOCK__POLL_CANCELLED
} sock__poll_status_t;

typedef struct sock_state {
  IO_STATUS_BLOCK io_status_block;
  AFD_POLL_INFO poll_info;
  queue_node_t queue_node;
  tree_node_t tree_node;
  poll_group_t* poll_group;
  SOCKET base_socket;
  epoll_data_t user_data;
  uint32_t user_events;
  uint32_t pending_events;
  sock__poll_status_t poll_status;
  bool delete_pending;
} sock_state_t;

static inline sock_state_t* sock__alloc(void) {
  sock_state_t* sock_state = new (std::nothrow) sock_state_t{};
  if (sock_state == NULL)
    return_set_error(NULL, ERROR_NOT_ENOUGH_MEMORY);
  return sock_state;
}

static inline void sock__free(sock_state_t* sock_state) {
  delete sock_state;
}

static int sock__cancel_poll(sock_state_t* sock_state) {
  assert(sock_state->poll_status == SOCK__POLL_PENDING);

  if (afd_cancel_poll(poll_group_get_afd_helper_handle(sock_state->poll_group),
                      &sock_state->io_status_block) < 0)
    return -1;

  sock_state->poll_status = SOCK__POLL_CANCELLED;
  sock_state->pending_events = 0;
  return 0;
}

sock_state_t* sock_new(port_state_t* port_state, SOCKET socket) {
  SOCKET base_socket;
  poll_group_t* poll_group;
  sock_state_t* sock_state;

  if (socket == 0 || socket == INVALID_SOCKET)
    return_set_error(NULL, ERROR_INVALID_HANDLE);

  base_socket = ws_get_base_socket(socket);
  if (base_socket == INVALID_SOCKET)
    return NULL;

  poll_group = poll_group_acquire(port_state);
  if (poll_group == NULL)
    return NULL;

  sock_state = sock__alloc();
  if (sock_state == NULL) {
    poll_group_release(poll_group);
    return NULL;
  }

  sock_state->base_socket = base_socket;
  sock_state->poll_group = poll_group;

  tree_node_init(&sock_state->tree_node);
  queueNodeInit(&sock_state->queue_node);

  if (port_register_socket(port_state, sock_state, socket) < 0) {
    sock__free(sock_state);
    poll_group_release(poll_group);
    return NULL;
  }

  return sock_state;
}

static int sock__delete(port_state_t* port_state,
                        sock_state_t* sock_state,
                        bool force) {
  if (!sock_state->delete_pending) {
    if (sock_state->poll_status == SOCK__POLL_PENDING)
      sock__cancel_poll(sock_state);

    port_cancel_socket_update(port_state, sock_state);
    port_unregister_socket(port_state, sock_state);

    sock_state->delete_pending = true;
  }

  /* If the poll request still needs to complete, the sock_state object can't
   * be free()d yet. `sock_feed_event()` or `port_close()` will take care
   * of this later. */
  if (force || sock_state->poll_status == SOCK__POLL_IDLE) {
    /* Free the sock_state now. */
    port_remove_deleted_socket(port_state, sock_state);
    poll_group_release(sock_state->poll_group);
    sock__free(sock_state);
  } else {
    /* Free the socket later. */
    port_add_deleted_socket(port_state, sock_state);
  }

  return 0;
}

void sock_delete(port_state_t* port_state, sock_state_t* sock_state) {
  sock__delete(port_state, sock_state, false);
}

void sock_force_delete(port_state_t* port_state, sock_state_t* sock_state) {
  sock__delete(port_state, sock_state, true);
}

int sock_set_event(port_state_t* port_state,
                   sock_state_t* sock_state,
                   const struct epoll_event* ev) {
  /* EPOLLERR and EPOLLHUP are always reported, even when not requested by the
   * caller. However they are disabled after a event has been reported for a
   * socket for which the EPOLLONESHOT flag as set. */
  uint32_t events = ev->events | EPOLLERR | EPOLLHUP;

  sock_state->user_events = events;
  sock_state->user_data = ev->data;

  if ((events & SOCK__KNOWN_EPOLL_EVENTS & ~sock_state->pending_events) != 0)
    port_request_socket_update(port_state, sock_state);

  return 0;
}

static inline DWORD sock__epoll_events_to_afd_events(uint32_t epoll_events) {
  /* Always monitor for AFD_POLL_LOCAL_CLOSE, which is triggered when the
   * socket is closed with closesocket() or CloseHandle(). */
  DWORD afd_events = AFD_POLL_LOCAL_CLOSE;

  if (epoll_events & (EPOLLIN | EPOLLRDNORM))
    afd_events |= AFD_POLL_RECEIVE | AFD_POLL_ACCEPT;
  if (epoll_events & (EPOLLPRI | EPOLLRDBAND))
    afd_events |= AFD_POLL_RECEIVE_EXPEDITED;
  if (epoll_events & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND))
    afd_events |= AFD_POLL_SEND;
  if (epoll_events & (EPOLLIN | EPOLLRDNORM | EPOLLRDHUP))
    afd_events |= AFD_POLL_DISCONNECT;
  if (epoll_events & EPOLLHUP)
    afd_events |= AFD_POLL_ABORT;
  if (epoll_events & EPOLLERR)
    afd_events |= AFD_POLL_CONNECT_FAIL;

  return afd_events;
}

static inline uint32_t sock__afd_events_to_epoll_events(DWORD afd_events) {
  uint32_t epoll_events = 0;

  if (afd_events & (AFD_POLL_RECEIVE | AFD_POLL_ACCEPT))
    epoll_events |= EPOLLIN | EPOLLRDNORM;
  if (afd_events & AFD_POLL_RECEIVE_EXPEDITED)
    epoll_events |= EPOLLPRI | EPOLLRDBAND;
  if (afd_events & AFD_POLL_SEND)
    epoll_events |= EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;
  if (afd_events & AFD_POLL_DISCONNECT)
    epoll_events |= EPOLLIN | EPOLLRDNORM | EPOLLRDHUP;
  if (afd_events & AFD_POLL_ABORT)
    epoll_events |= EPOLLHUP;
  if (afd_events & AFD_POLL_CONNECT_FAIL)
    /* Linux reports all these events after connect() has failed. */
    epoll_events |=
        EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLRDNORM | EPOLLWRNORM | EPOLLRDHUP;

  return epoll_events;
}

int sock_update(port_state_t* port_state, sock_state_t* sock_state) {
  assert(!sock_state->delete_pending);

  if ((sock_state->poll_status == SOCK__POLL_PENDING) &&
      (sock_state->user_events & SOCK__KNOWN_EPOLL_EVENTS &
       ~sock_state->pending_events) == 0) {
    /* All the events the user is interested in are already being monitored by
     * the pending poll operation. It might spuriously complete because of an
     * event that we're no longer interested in; when that happens we'll submit
     * a new poll operation with the updated event mask. */

  } else if (sock_state->poll_status == SOCK__POLL_PENDING) {
    /* A poll operation is already pending, but it's not monitoring for all the
     * events that the user is interested in. Therefore, cancel the pending
     * poll operation; when we receive it's completion package, a new poll
     * operation will be submitted with the correct event mask. */
    if (sock__cancel_poll(sock_state) < 0)
      return -1;

  } else if (sock_state->poll_status == SOCK__POLL_CANCELLED) {
    /* The poll operation has already been cancelled, we're still waiting for
     * it to return. For now, there's nothing that needs to be done. */

  } else if (sock_state->poll_status == SOCK__POLL_IDLE) {
    /* No poll operation is pending; start one. */
    sock_state->poll_info.Exclusive = FALSE;
    sock_state->poll_info.NumberOfHandles = 1;
    sock_state->poll_info.Timeout.QuadPart = INT64_MAX;
    sock_state->poll_info.Handles[0].Handle = (HANDLE) sock_state->base_socket;
    sock_state->poll_info.Handles[0].Status = 0;
    sock_state->poll_info.Handles[0].Events =
        sock__epoll_events_to_afd_events(sock_state->user_events);

    if (afd_poll(poll_group_get_afd_helper_handle(sock_state->poll_group),
                 &sock_state->poll_info,
                 &sock_state->io_status_block) < 0) {
      switch (GetLastError()) {
        case ERROR_IO_PENDING:
          /* Overlapped poll operation in progress; this is expected. */
          break;
        case ERROR_INVALID_HANDLE:
          /* Socket closed; it'll be dropped from the epoll set. */
          return sock__delete(port_state, sock_state, false);
        default:
          /* Other errors are propagated to the caller. */
          return_map_error(-1);
      }
    }

    /* The poll request was successfully submitted. */
    sock_state->poll_status = SOCK__POLL_PENDING;
    sock_state->pending_events = sock_state->user_events;

  } else {
    /* Unreachable. */
    assert(false);
  }

  port_cancel_socket_update(port_state, sock_state);
  return 0;
}

int sock_feed_event(port_state_t* port_state,
                    IO_STATUS_BLOCK* io_status_block,
                    struct epoll_event* ev) {
  sock_state_t* sock_state =
      container_of(io_status_block, sock_state_t, io_status_block);
  AFD_POLL_INFO* poll_info = &sock_state->poll_info;
  uint32_t epoll_events = 0;

  sock_state->poll_status = SOCK__POLL_IDLE;
  sock_state->pending_events = 0;

  if (sock_state->delete_pending) {
    /* Socket has been deleted earlier and can now be freed. */
    return sock__delete(port_state, sock_state, false);

  } else if (io_status_block->Status == STATUS_CANCELLED) {
    /* The poll request was cancelled by CancelIoEx. */

  } else if (!NT_SUCCESS(io_status_block->Status)) {
    /* The overlapped request itself failed in an unexpected way. */
    epoll_events = EPOLLERR;

  } else if (poll_info->NumberOfHandles < 1) {
    /* This poll operation succeeded but didn't report any socket events. */

  } else if (poll_info->Handles[0].Events & AFD_POLL_LOCAL_CLOSE) {
    /* The poll operation reported that the socket was closed. */
    return sock__delete(port_state, sock_state, false);

  } else {
    /* Events related to our socket were reported. */
    epoll_events =
        sock__afd_events_to_epoll_events(poll_info->Handles[0].Events);
  }

  /* Requeue the socket so a new poll request will be submitted. */
  port_request_socket_update(port_state, sock_state);

  /* Filter out events that the user didn't ask for. */
  epoll_events &= sock_state->user_events;

  /* Return if there are no epoll events to report. */
  if (epoll_events == 0)
    return 0;

  /* If the the socket has the EPOLLONESHOT flag set, unmonitor all events,
   * even EPOLLERR and EPOLLHUP. But always keep looking for closed sockets. */
  if (sock_state->user_events & EPOLLONESHOT)
    sock_state->user_events = 0;

  ev->data = sock_state->user_data;
  ev->events = epoll_events;
  return 1;
}

sock_state_t* sock_state_from_queue_node(queue_node_t* queue_node) {
  return container_of(queue_node, sock_state_t, queue_node);
}

queue_node_t* sock_state_to_queue_node(sock_state_t* sock_state) {
  return &sock_state->queue_node;
}

sock_state_t* sock_state_from_tree_node(tree_node_t* tree_node) {
  return container_of(tree_node, sock_state_t, tree_node);
}

tree_node_t* sock_state_to_tree_node(sock_state_t* sock_state) {
  return &sock_state->tree_node;
}

void ts_tree_init(ts_tree_t* ts_tree) {
  tree_init(&ts_tree->tree);
  InitializeSRWLock(&ts_tree->lock);
}

void ts_tree_node_init(ts_tree_node_t* node) {
  tree_node_init(&node->tree_node);
  node->reflock.init();
}

int ts_tree_add(ts_tree_t* ts_tree, ts_tree_node_t* node, uintptr_t key) {
  int r;

  AcquireSRWLockExclusive(&ts_tree->lock);
  r = tree_add(&ts_tree->tree, &node->tree_node, key);
  ReleaseSRWLockExclusive(&ts_tree->lock);

  return r;
}

static inline ts_tree_node_t* ts_tree__find_node(ts_tree_t* ts_tree,
                                                 uintptr_t key) {
  tree_node_t* tree_node = tree_find(&ts_tree->tree, key);
  if (tree_node == NULL)
    return NULL;

  return container_of(tree_node, ts_tree_node_t, tree_node);
}

ts_tree_node_t* ts_tree_del_and_ref(ts_tree_t* ts_tree, uintptr_t key) {
  ts_tree_node_t* ts_tree_node;

  AcquireSRWLockExclusive(&ts_tree->lock);

  ts_tree_node = ts_tree__find_node(ts_tree, key);
  if (ts_tree_node != NULL) {
    tree_del(&ts_tree->tree, &ts_tree_node->tree_node);
    ts_tree_node->reflock.ref();
  }

  ReleaseSRWLockExclusive(&ts_tree->lock);

  return ts_tree_node;
}

ts_tree_node_t* ts_tree_find_and_ref(ts_tree_t* ts_tree, uintptr_t key) {
  ts_tree_node_t* ts_tree_node;

  AcquireSRWLockShared(&ts_tree->lock);

  ts_tree_node = ts_tree__find_node(ts_tree, key);
  if (ts_tree_node != NULL)
    ts_tree_node->reflock.ref();

  ReleaseSRWLockShared(&ts_tree->lock);

  return ts_tree_node;
}

void ts_tree_node_unref(ts_tree_node_t* node) {
  node->reflock.unref();
}

void ts_tree_node_unref_and_destroy(ts_tree_node_t* node) {
  node->reflock.unrefAndDestroy();
}

void tree_init(tree_t* tree) {
  /* 只用于生命周期已经开始的对象（静态的 epoll__handle_tree）；malloc 存储上的那份由
     port__new 直接 construct_at，不能用这里的赋值。 */
  *tree = tree_t{};
}

void tree_node_init(tree_node_t* node) {
  node->key = 0;
}

int tree_add(tree_t* tree, tree_node_t* node, uintptr_t key) {
  node->key = key;

  /* 同一个键重复插入是调用方的错误：返回 -1 让它按 EEXIST 报出去（与手写树同样的约定） */
  return tree->emplace(key, node).second ? 0 : -1;
}

void tree_del(tree_t* tree, tree_node_t* node) {
  tree->erase(node->key);
}

tree_node_t* tree_find(const tree_t* tree, uintptr_t key) {
  const auto iterator = tree->find(key);
  return iterator == tree->end() ? NULL : iterator->second;
}

tree_node_t* tree_root(const tree_t* tree) {
  /* 只给 port_delete 的「逐个摘除直到空」用：给最小键的那个节点即可，
     原实现给的是红黑树的根（同样是「随便一个」，摘除顺序本就不可依赖） */
  return tree->empty() ? NULL : tree->begin()->second;
}

#ifndef SIO_BSP_HANDLE_POLL
#define SIO_BSP_HANDLE_POLL 0x4800001D
#endif

#ifndef SIO_BASE_HANDLE
#define SIO_BASE_HANDLE 0x48000022
#endif

int ws_global_init(void) {
  int r;
  WSADATA wsa_data;

  r = WSAStartup(MAKEWORD(2, 2), &wsa_data);
  if (r != 0)
    return_set_error(-1, (DWORD) r);

  return 0;
}

static inline SOCKET ws__ioctl_get_bsp_socket(SOCKET socket, DWORD ioctl) {
  SOCKET bsp_socket;
  DWORD bytes;

  if (WSAIoctl(socket,
               ioctl,
               NULL,
               0,
               &bsp_socket,
               sizeof bsp_socket,
               &bytes,
               NULL,
               NULL) != SOCKET_ERROR)
    return bsp_socket;
  else
    return INVALID_SOCKET;
}

SOCKET ws_get_base_socket(SOCKET socket) {
  SOCKET base_socket;
  DWORD error;

  for (;;) {
    base_socket = ws__ioctl_get_bsp_socket(socket, SIO_BASE_HANDLE);
    if (base_socket != INVALID_SOCKET)
      return base_socket;

    error = GetLastError();
    if (error == WSAENOTSOCK)
      return_set_error(INVALID_SOCKET, error);

    /* Even though Microsoft documentation clearly states that LSPs should
     * never intercept the `SIO_BASE_HANDLE` ioctl [1], Komodia based LSPs do
     * so anyway, breaking it, with the apparent intention of preventing LSP
     * bypass [2]. Fortunately they don't handle `SIO_BSP_HANDLE_POLL`, which
     * we can use to obtain the socket associated with the next protocol chain
     * entry. If this succeeds, loop around and call `SIO_BASE_HANDLE` again
     * with the retrieved BSP socket to be sure that we actually got all the
     * way to the base.
     *  [1] https://docs.microsoft.com/en-us/windows/win32/winsock/winsock-ioctls
     *  [2] https://www.komodia.com/newwiki/index.php?title=Komodia%27s_Redirector_bug_fixes#Version_2.2.2.6
     */

    base_socket = ws__ioctl_get_bsp_socket(socket, SIO_BSP_HANDLE_POLL);
    if (base_socket != INVALID_SOCKET && base_socket != socket)
      socket = base_socket;
    else
      return_set_error(INVALID_SOCKET, error);
  }
}
