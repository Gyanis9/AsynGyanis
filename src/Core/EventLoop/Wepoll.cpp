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

WEPOLL_INTERNAL int ntGlobalInit(void);

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
  { sizeof(OBJECT_ATTRIBUTES), nullptr, ObjectName, Attributes, nullptr, nullptr }

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

#define X(returnType, attributes, name, parameters) \
  WEPOLL_INTERNAL_VAR returnType(attributes* name) parameters = nullptr;
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

WEPOLL_INTERNAL int afdCreateHelperHandle(HANDLE iocpHandle,
                                             HANDLE* afdHelperHandleOut);

WEPOLL_INTERNAL int afdPoll(HANDLE afdHelperHandle,
                             AFD_POLL_INFO* pollInfo,
                             IO_STATUS_BLOCK* ioStatusBlock);
WEPOLL_INTERNAL int afdCancelPoll(HANDLE afdHelperHandle,
                                    IO_STATUS_BLOCK* ioStatusBlock);

#define RETURN_MAP_ERROR(value) \
  do {                          \
    errorMapWindowsError();        \
    return (value);             \
  } while (0)

#define RETURN_SET_ERROR(value, error) \
  do {                                 \
    errorSetWindowsError(error);          \
    return (value);                    \
  } while (0)

WEPOLL_INTERNAL void errorMapWindowsError(void);
WEPOLL_INTERNAL void errorSetWindowsError(DWORD error);
WEPOLL_INTERNAL int errorCheckHandle(HANDLE handle);

#define IOCTL_AFD_POLL 0x00012024

static UNICODE_STRING afdHelperName =
    RTL_CONSTANT_STRING(L"\\Device\\Afd\\Wepoll");

static OBJECT_ATTRIBUTES afdHelperAttributes =
    RTL_CONSTANT_OBJECT_ATTRIBUTES(&afdHelperName, 0);

int afdCreateHelperHandle(HANDLE iocpHandle,
                             HANDLE* afdHelperHandleOut) {
  HANDLE afdHelperHandle;
  IO_STATUS_BLOCK iosb;
  NTSTATUS status;

  /* By opening \Device\Afd without specifying any extended attributes, we'll
   * get a handle that lets us talk to the AFD driver, but that doesn't have an
   * associated endpoint (so it's not a socket). */
  status = NtCreateFile(&afdHelperHandle,
                        SYNCHRONIZE,
                        &afdHelperAttributes,
                        &iosb,
                        nullptr,
                        0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_OPEN,
                        0,
                        nullptr,
                        0);
  if (status != STATUS_SUCCESS)
    RETURN_SET_ERROR(-1, RtlNtStatusToDosError(status));

  /* 句柄已经拿到：这两步任一步失败都要先关掉它再报错。短路求值保证第二步只在第一步
     成功时才执行，报出来的仍是真正失败那一步的错误码 */
  if (CreateIoCompletionPort(afdHelperHandle, iocpHandle, 0, 0) == nullptr ||
      !SetFileCompletionNotificationModes(afdHelperHandle,
                                          FILE_SKIP_SET_EVENT_ON_HANDLE)) {
    CloseHandle(afdHelperHandle);
    RETURN_MAP_ERROR(-1);
  }

  *afdHelperHandleOut = afdHelperHandle;
  return 0;
}

int afdPoll(HANDLE afdHelperHandle,
             AFD_POLL_INFO* pollInfo,
             IO_STATUS_BLOCK* ioStatusBlock) {
  NTSTATUS status;

  /* Blocking operation is not supported. */
  assert(ioStatusBlock != nullptr);

  ioStatusBlock->Status = STATUS_PENDING;
  status = NtDeviceIoControlFile(afdHelperHandle,
                                 nullptr,
                                 nullptr,
                                 ioStatusBlock,
                                 ioStatusBlock,
                                 IOCTL_AFD_POLL,
                                 pollInfo,
                                 sizeof *pollInfo,
                                 pollInfo,
                                 sizeof *pollInfo);

  if (status == STATUS_SUCCESS)
    return 0;
  else if (status == STATUS_PENDING)
    RETURN_SET_ERROR(-1, ERROR_IO_PENDING);
  else
    RETURN_SET_ERROR(-1, RtlNtStatusToDosError(status));
}

int afdCancelPoll(HANDLE afdHelperHandle,
                    IO_STATUS_BLOCK* ioStatusBlock) {
  NTSTATUS cancelStatus;
  IO_STATUS_BLOCK cancelIoStatusBlock;

  /* If the poll operation has already completed or has been cancelled earlier,
   * there's nothing left for us to do. */
  if (ioStatusBlock->Status != STATUS_PENDING)
    return 0;

  cancelStatus =
      NtCancelIoFileEx(afdHelperHandle, ioStatusBlock, &cancelIoStatusBlock);

  /* NtCancelIoFileEx() may return STATUS_NOT_FOUND if the operation completed
   * just before calling NtCancelIoFileEx(). This is not an error. */
  if (cancelStatus == STATUS_SUCCESS || cancelStatus == STATUS_NOT_FOUND)
    return 0;
  else
    RETURN_SET_ERROR(-1, RtlNtStatusToDosError(cancelStatus));
}

WEPOLL_INTERNAL int epollGlobalInit(void);

WEPOLL_INTERNAL int ensureInitialized(void);

struct PortState;
struct Queue;
struct SockState;
struct TsTreeNode;

WEPOLL_INTERNAL PortState* portNew(HANDLE* iocpHandleOut);
WEPOLL_INTERNAL int portClose(PortState* portState);
WEPOLL_INTERNAL int portDelete(PortState* portState);

WEPOLL_INTERNAL int portWait(PortState* portState,
                              struct epoll_event* events,
                              int maxevents,
                              int timeout);

WEPOLL_INTERNAL int portCtl(PortState* portState,
                             int op,
                             SOCKET sock,
                             struct epoll_event* ev);

WEPOLL_INTERNAL int portRegisterSocket(PortState* portState,
                                         SockState* sockState,
                                         SOCKET socket);
WEPOLL_INTERNAL void portUnregisterSocket(PortState* portState,
                                            SockState* sockState);
WEPOLL_INTERNAL SockState* portFindSocket(PortState* portState,
                                               SOCKET socket);

WEPOLL_INTERNAL void portRequestSocketUpdate(PortState* portState,
                                                SockState* sockState);
WEPOLL_INTERNAL void portCancelSocketUpdate(PortState* portState,
                                               SockState* sockState);

WEPOLL_INTERNAL void portAddDeletedSocket(PortState* portState,
                                             SockState* sockState);
WEPOLL_INTERNAL void portRemoveDeletedSocket(PortState* portState,
                                                SockState* sockState);

WEPOLL_INTERNAL HANDLE portGetIocpHandle(PortState* portState);
WEPOLL_INTERNAL Queue* portGetPollGroupQueue(PortState* portState);

WEPOLL_INTERNAL PortState* portStateFromHandleTreeNode(
    TsTreeNode* treeNode);
WEPOLL_INTERNAL TsTreeNode* portStateToHandleTreeNode(
    PortState* portState);

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
  void ensureInitialized() noexcept
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

struct TreeNode;

struct TreeNode {
  uintptr_t key; /* 节点只留键：树的形状信息改由标准容器保管 */
};

/* 键 → 节点。原先是一棵手写的红黑树（插入、删除、旋转、重平衡近 200 行），而调用方只用到
 * 「按键插入、按键查找、按键删除」三件事，因此交给标准库的有序表：std::map 同样按键有序，
 * 且节点地址稳定（再平衡不会让节点搬家），下面用 CONTAINER_OF 从节点取回宿主结构照旧成立。 */
typedef std::map<uintptr_t, TreeNode*> Tree;

WEPOLL_INTERNAL void treeInit(Tree* tree);
WEPOLL_INTERNAL void treeNodeInit(TreeNode* node);

WEPOLL_INTERNAL int treeAdd(Tree* tree, TreeNode* node, uintptr_t key);
WEPOLL_INTERNAL void treeDel(Tree* tree, TreeNode* node);

WEPOLL_INTERNAL TreeNode* treeFind(const Tree* tree, uintptr_t key);
WEPOLL_INTERNAL TreeNode* treeRoot(const Tree* tree);

struct TsTree {
  Tree tree;
  SRWLOCK lock;
};

struct TsTreeNode {
  TreeNode treeNode;
  RefLock reflock;
};

WEPOLL_INTERNAL void tsTreeInit(TsTree* rtl);
WEPOLL_INTERNAL void tsTreeNodeInit(TsTreeNode* node);

WEPOLL_INTERNAL int tsTreeAdd(TsTree* tsTree,
                                TsTreeNode* node,
                                uintptr_t key);

WEPOLL_INTERNAL TsTreeNode* tsTreeDelAndRef(TsTree* tsTree,
                                                    uintptr_t key);
WEPOLL_INTERNAL TsTreeNode* tsTreeFindAndRef(TsTree* tsTree,
                                                     uintptr_t key);

WEPOLL_INTERNAL void tsTreeNodeUnref(TsTreeNode* node);
WEPOLL_INTERNAL void tsTreeNodeUnrefAndDestroy(TsTreeNode* node);

static TsTree epollHandleTree;

int epollGlobalInit(void) {
  tsTreeInit(&epollHandleTree);
  return 0;
}

static HANDLE epollCreate(void) {
  PortState* portState;
  HANDLE ephnd;
  TsTreeNode* treeNode;

  if (ensureInitialized() < 0)
    return nullptr;

  portState = portNew(&ephnd);
  if (portState == nullptr)
    return nullptr;

  treeNode = portStateToHandleTreeNode(portState);
  if (tsTreeAdd(&epollHandleTree, treeNode, reinterpret_cast<uintptr_t>(ephnd)) < 0) {
    /* This should never happen. */
    portDelete(portState);
    RETURN_SET_ERROR(nullptr, ERROR_ALREADY_EXISTS);
  }

  return ephnd;
}

HANDLE epoll_create(int size) {
  if (size <= 0)
    RETURN_SET_ERROR(nullptr, ERROR_INVALID_PARAMETER);

  return epollCreate();
}

HANDLE epoll_create1(int flags) {
  if (flags != 0)
    RETURN_SET_ERROR(nullptr, ERROR_INVALID_PARAMETER);

  return epollCreate();
}

int epoll_close(HANDLE ephnd) {
  TsTreeNode* treeNode;
  PortState* portState;

  if (ensureInitialized() < 0)
    return -1;

  treeNode = tsTreeDelAndRef(&epollHandleTree, reinterpret_cast<uintptr_t>(ephnd));
  if (treeNode == nullptr) {
    errorSetWindowsError(ERROR_INVALID_PARAMETER);

    /* 与其它入口同一口径：句柄自身的错误优先于「表里找不到」 */
    errorCheckHandle(ephnd);
    return -1;
  }

  portState = portStateFromHandleTreeNode(treeNode);
  portClose(portState);

  tsTreeNodeUnrefAndDestroy(treeNode);

  return portDelete(portState);
}

int epoll_ctl(HANDLE ephnd, int op, SOCKET sock, struct epoll_event* ev) {
  TsTreeNode* treeNode;
  PortState* portState;
  int r;

  if (ensureInitialized() < 0)
    return -1;

  treeNode = tsTreeFindAndRef(&epollHandleTree, reinterpret_cast<uintptr_t>(ephnd));
  if (treeNode == nullptr) {
    errorSetWindowsError(ERROR_INVALID_PARAMETER);
    /* On Linux, in the case of epoll_ctl(), EBADF takes priority over other
     * errors. Wepoll mimics this behavior. */
    errorCheckHandle(ephnd);
    errorCheckHandle(reinterpret_cast<HANDLE>(sock));
    return -1;
  }

  portState = portStateFromHandleTreeNode(treeNode);
  r = portCtl(portState, op, sock, ev);

  tsTreeNodeUnref(treeNode);

  if (r < 0) {
    /* 与上面同一口径：两行重复换来一个没有跳转的单一出口 */
    errorCheckHandle(ephnd);
    errorCheckHandle(reinterpret_cast<HANDLE>(sock));
    return -1;
  }

  return 0;
}

int epoll_wait(HANDLE ephnd,
               struct epoll_event* events,
               int maxevents,
               int timeout) {
  TsTreeNode* treeNode;
  PortState* portState;
  int eventCount;

  if (maxevents <= 0)
    RETURN_SET_ERROR(-1, ERROR_INVALID_PARAMETER);

  if (ensureInitialized() < 0)
    return -1;

  treeNode = tsTreeFindAndRef(&epollHandleTree, reinterpret_cast<uintptr_t>(ephnd));
  if (treeNode == nullptr) {
    errorSetWindowsError(ERROR_INVALID_PARAMETER);
    errorCheckHandle(ephnd);
    return -1;
  }

  portState = portStateFromHandleTreeNode(treeNode);
  eventCount = portWait(portState, events, maxevents, timeout);

  tsTreeNodeUnref(treeNode);

  if (eventCount < 0) {
    errorCheckHandle(ephnd);
    return -1;
  }

  return eventCount;
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

static errno_t errorMapWindowsErrorToErrno(DWORD error) {
  switch (error) {
#define X(errorSymbol, errnoSymbol) \
  case errorSymbol:               \
    return errnoSymbol;
    ERR__ERRNO_MAPPINGS(X)
#undef X
  }
  return EINVAL;
}

void errorMapWindowsError(void) {
  errno = errorMapWindowsErrorToErrno(GetLastError());
}

void errorSetWindowsError(DWORD error) {
  SetLastError(error);
  errno = errorMapWindowsErrorToErrno(error);
}

int errorCheckHandle(HANDLE handle) {
  DWORD flags;

  /* GetHandleInformation() succeeds when passed INVALID_HANDLE_VALUE, so check
   * for this condition explicitly. */
  if (handle == INVALID_HANDLE_VALUE)
    RETURN_SET_ERROR(-1, ERROR_INVALID_HANDLE);

  if (!GetHandleInformation(handle, &flags))
    RETURN_MAP_ERROR(-1);

  return 0;
}

#include <stddef.h>

#define ARRAY_COUNT(a) (sizeof(a) / (sizeof((a)[0])))

#define CONTAINER_OF(ptr, type, member) \
  ((type*) ((uintptr_t) (ptr) - offsetof(type, member)))

#define UNUSED_VAR(v) ((void) (v))

/* Polyfill `inline` for older versions of msvc (up to Visual Studio 2013) */
#if defined(_MSC_VER) && _MSC_VER < 1900
#define inline __inline
#endif

WEPOLL_INTERNAL int wsGlobalInit(void);
WEPOLL_INTERNAL SOCKET wsGetBaseSocket(SOCKET socket);

static bool initializationDone = false;
static INIT_ONCE onceControl = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK onceCallback(INIT_ONCE* once,
                                         void* parameter,
                                         void** context) {
  UNUSED_VAR(once);
  UNUSED_VAR(parameter);
  UNUSED_VAR(context);

  /* N.b. that initialization order matters here. */
  if (wsGlobalInit() < 0 || ntGlobalInit() < 0 ||
      epollGlobalInit() < 0)
    return FALSE;

  initializationDone = true;
  return TRUE;
}

int ensureInitialized(void) {
  if (!initializationDone &&
      !InitOnceExecuteOnce(&onceControl, onceCallback, nullptr, nullptr))
    /* `InitOnceExecuteOnce()` itself is infallible, and it doesn't set any
     * error code when the once-callback returns FALSE. We return -1 here to
     * indicate that global initialization failed; the failing ensureInitialized function is
     * resposible for setting `errno` and calling `SetLastError()`. */
    return -1;

  return 0;
}

/* Set up a workaround for the following problem:
 *   FARPROC addr = GetProcAddress(...);
 *   MY_FUNC func = (MY_FUNC) addr;          <-- GCC 8 warning/error.
 *   MY_FUNC func = (MY_FUNC) (void*) addr;  <-- MSVC  warning/error.
 * To compile cleanly with either compiler, do casts with this "bridge" type:
 *   MY_FUNC func = (MY_FUNC) (NtFunctionPointerCast) addr; */
#ifdef __GNUC__
typedef void* NtFunctionPointerCast;
#else
typedef FARPROC NtFunctionPointerCast;
#endif

int ntGlobalInit(void) {
  HMODULE ntdll;
  FARPROC functionPointer;

  ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr)
    return -1;

#define X(returnType, attributes, name, parameters) \
  functionPointer = GetProcAddress(ntdll, #name);             \
  if (functionPointer == nullptr)                                \
    return -1;                                       \
  name = (returnType(attributes*) parameters)(NtFunctionPointerCast) functionPointer;
  NT_NTDLL_IMPORT_LIST(X)
#undef X

  return 0;
}

#include <string.h>

struct PollGroup;

struct QueueNode;

WEPOLL_INTERNAL PollGroup* pollGroupAcquire(PortState* port);
WEPOLL_INTERNAL void pollGroupRelease(PollGroup* pollGroup);

WEPOLL_INTERNAL void pollGroupDelete(PollGroup* pollGroup);

WEPOLL_INTERNAL PollGroup* pollGroupFromQueueNode(
    QueueNode* queueNode);
WEPOLL_INTERNAL HANDLE
    pollGroupGetAfdHelperHandle(PollGroup* pollGroup);

struct QueueNode {
  QueueNode* previous;
  QueueNode* next;
};

/* 带哨兵头的侵入式双向链表：节点内联在持有者里（不进堆），因此「从节点摘除」是 O(1)、
   队列本身一次分配都不用做。本文件的两处用法都吃这个代价——每轮 epoll_wait 之后有成百上千
   个 socket 要在这几张表之间挪动。改用 std::list 会让每个节点多一次分配，还得把迭代器塞回
   节点才能保住同样的摘除代价，得不偿失。 */
struct Queue {
  QueueNode head; /* 哨兵：head.next 是首元素、head.previous 是尾元素；空表时两者都指回 head */
};

WEPOLL_INTERNAL void queueInit(Queue* queue);
WEPOLL_INTERNAL void queueNodeInit(QueueNode* node);

WEPOLL_INTERNAL QueueNode* queueFirst(const Queue* queue);
WEPOLL_INTERNAL QueueNode* queueLast(const Queue* queue);

WEPOLL_INTERNAL void queueAppend(Queue* queue, QueueNode* node);
WEPOLL_INTERNAL void queueMoveToStart(Queue* queue, QueueNode* node);
WEPOLL_INTERNAL void queueMoveToEnd(Queue* queue, QueueNode* node);
WEPOLL_INTERNAL void queueRemove(QueueNode* node);

WEPOLL_INTERNAL bool queueIsEmpty(const Queue* queue);
WEPOLL_INTERNAL bool queueIsEnqueued(const QueueNode* node);

static const size_t POLL_GROUP__MAX_GROUP_SIZE = 32;

struct PollGroup {
  PortState* portState;
  QueueNode queueNode;
  HANDLE afdHelperHandle;
  size_t groupSize;
};

static PollGroup* pollGroupNew(PortState* portState) {
  HANDLE iocpHandle = portGetIocpHandle(portState);
  Queue* pollGroupQueue = portGetPollGroupQueue(portState);

  /* nothrow 版本：失败返回空指针，与「失败即返回 NULL + 设 LastError」的约定一致。
     值初始化把各成员清零，队列节点仍要显式初始化成自指的哨兵 */
  PollGroup* pollGroup = new (std::nothrow) PollGroup{};
  if (pollGroup == nullptr)
    RETURN_SET_ERROR(nullptr, ERROR_NOT_ENOUGH_MEMORY);

  queueNodeInit(&pollGroup->queueNode);
  pollGroup->portState = portState;

  if (afdCreateHelperHandle(iocpHandle, &pollGroup->afdHelperHandle) <
      0) {
    delete pollGroup;
    return nullptr;
  }

  queueAppend(pollGroupQueue, &pollGroup->queueNode);

  return pollGroup;
}

void pollGroupDelete(PollGroup* pollGroup) {
  assert(pollGroup->groupSize == 0);
  CloseHandle(pollGroup->afdHelperHandle);
  queueRemove(&pollGroup->queueNode);
  delete pollGroup;
}

PollGroup* pollGroupFromQueueNode(QueueNode* queueNode) {
  return CONTAINER_OF(queueNode, PollGroup, queueNode);
}

HANDLE pollGroupGetAfdHelperHandle(PollGroup* pollGroup) {
  return pollGroup->afdHelperHandle;
}

PollGroup* pollGroupAcquire(PortState* portState) {
  Queue* pollGroupQueue = portGetPollGroupQueue(portState);
  PollGroup* pollGroup =
      !queueIsEmpty(pollGroupQueue)
          ? CONTAINER_OF(
                queueLast(pollGroupQueue), PollGroup, queueNode)
          : nullptr;

  if (pollGroup == nullptr ||
      pollGroup->groupSize >= POLL_GROUP__MAX_GROUP_SIZE)
    pollGroup = pollGroupNew(portState);
  if (pollGroup == nullptr)
    return nullptr;

  if (++pollGroup->groupSize == POLL_GROUP__MAX_GROUP_SIZE)
    queueMoveToStart(pollGroupQueue, &pollGroup->queueNode);

  return pollGroup;
}

void pollGroupRelease(PollGroup* pollGroup) {
  PortState* portState = pollGroup->portState;
  Queue* pollGroupQueue = portGetPollGroupQueue(portState);

  pollGroup->groupSize--;
  assert(pollGroup->groupSize < POLL_GROUP__MAX_GROUP_SIZE);

  queueMoveToEnd(pollGroupQueue, &pollGroup->queueNode);

  /* Poll groups are currently only freed when the epoll port is closed. */
}

WEPOLL_INTERNAL SockState* sockNew(PortState* portState,
                                       SOCKET socket);
WEPOLL_INTERNAL void sockDelete(PortState* portState,
                                 SockState* sockState);
WEPOLL_INTERNAL void sockForceDelete(PortState* portState,
                                       SockState* sockState);

WEPOLL_INTERNAL int sockSetEvent(PortState* portState,
                                   SockState* sockState,
                                   const struct epoll_event* ev);

WEPOLL_INTERNAL int sockUpdate(PortState* portState,
                                SockState* sockState);
WEPOLL_INTERNAL int sockFeedEvent(PortState* portState,
                                    IO_STATUS_BLOCK* ioStatusBlock,
                                    struct epoll_event* ev);

WEPOLL_INTERNAL SockState* sockStateFromQueueNode(
    QueueNode* queueNode);
WEPOLL_INTERNAL QueueNode* sockStateToQueueNode(
    SockState* sockState);
WEPOLL_INTERNAL SockState* sockStateFromTreeNode(
    TreeNode* treeNode);
WEPOLL_INTERNAL TreeNode* sockStateToTreeNode(SockState* sockState);

#define PORT__MAX_ON_STACK_COMPLETIONS 256

struct PortState {
  HANDLE iocpHandle;
  Tree sockTree;
  Queue sockUpdateQueue;
  Queue sockDeletedQueue;
  Queue pollGroupQueue;
  TsTreeNode handleTreeNode;
  CRITICAL_SECTION lock;
  size_t activePollCount;
};

static PortState* portAlloc(void) {
  /* 值初始化：平凡成员清零，sockTree 与引用锁各自构造好——不再需要 memset 之后再补构造 */
  PortState* portState = new (std::nothrow) PortState{};
  if (portState == nullptr)
    RETURN_SET_ERROR(nullptr, ERROR_NOT_ENOUGH_MEMORY);

  return portState;
}

static void portFree(PortState* port) {
  assert(port != nullptr);
  /* 与 new 配对：容器成员由析构自动收尾 */
  delete port;
}

static HANDLE portCreateIocp(void) {
  HANDLE iocpHandle =
      CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
  if (iocpHandle == nullptr)
    RETURN_MAP_ERROR(nullptr);

  return iocpHandle;
}

PortState* portNew(HANDLE* iocpHandleOut) {
  PortState* portState;
  HANDLE iocpHandle;

  portState = portAlloc();
  if (portState == nullptr)
    return nullptr;

  iocpHandle = portCreateIocp();
  if (iocpHandle == nullptr) {
    portFree(portState);
    return nullptr;
  }

  portState->iocpHandle = iocpHandle;
  queueInit(&portState->sockUpdateQueue);
  queueInit(&portState->sockDeletedQueue);
  queueInit(&portState->pollGroupQueue);
  tsTreeNodeInit(&portState->handleTreeNode);
  InitializeCriticalSection(&portState->lock);

  *iocpHandleOut = iocpHandle;
  return portState;
}

static int portCloseIocp(PortState* portState) {
  HANDLE iocpHandle = portState->iocpHandle;
  portState->iocpHandle = nullptr;

  if (!CloseHandle(iocpHandle))
    RETURN_MAP_ERROR(-1);

  return 0;
}

int portClose(PortState* portState) {
  int result;

  EnterCriticalSection(&portState->lock);
  result = portCloseIocp(portState);
  LeaveCriticalSection(&portState->lock);

  return result;
}

int portDelete(PortState* portState) {
  TreeNode* treeNode;
  QueueNode* queueNode;

  /* At this point the IOCP port should have been closed. */
  assert(portState->iocpHandle == nullptr);

  while ((treeNode = treeRoot(&portState->sockTree)) != nullptr) {
    SockState* sockState = sockStateFromTreeNode(treeNode);
    sockForceDelete(portState, sockState);
  }

  while ((queueNode = queueFirst(&portState->sockDeletedQueue)) != nullptr) {
    SockState* sockState = sockStateFromQueueNode(queueNode);
    sockForceDelete(portState, sockState);
  }

  while ((queueNode = queueFirst(&portState->pollGroupQueue)) != nullptr) {
    PollGroup* pollGroup = pollGroupFromQueueNode(queueNode);
    pollGroupDelete(pollGroup);
  }

  assert(queueIsEmpty(&portState->sockUpdateQueue));

  DeleteCriticalSection(&portState->lock);

  portFree(portState);

  return 0;
}

static int portUpdateEvents(PortState* portState) {
  Queue* sockUpdateQueue = &portState->sockUpdateQueue;

  /* Walk the queue, submitting new poll requests for every socket that needs
   * it. */
  while (!queueIsEmpty(sockUpdateQueue)) {
    QueueNode* queueNode = queueFirst(sockUpdateQueue);
    SockState* sockState = sockStateFromQueueNode(queueNode);

    if (sockUpdate(portState, sockState) < 0)
      return -1;

    /* sockUpdate() removes the socket from the update queue. */
  }

  return 0;
}

static void portUpdateEventsIfPolling(PortState* portState) {
  if (portState->activePollCount > 0)
    portUpdateEvents(portState);
}

static int portFeedEvents(PortState* portState,
                             struct epoll_event* epollEvents,
                             OVERLAPPED_ENTRY* iocpEvents,
                             DWORD iocpEventCount) {
  int epollEventCount = 0;
  DWORD i;

  for (i = 0; i < iocpEventCount; i++) {
    IO_STATUS_BLOCK* ioStatusBlock =
        (IO_STATUS_BLOCK*) iocpEvents[i].lpOverlapped;
    struct epoll_event* ev = &epollEvents[epollEventCount];

    epollEventCount += sockFeedEvent(portState, ioStatusBlock, ev);
  }

  return epollEventCount;
}

static int portPoll(PortState* portState,
                      struct epoll_event* epollEvents,
                      OVERLAPPED_ENTRY* iocpEvents,
                      DWORD maxevents,
                      DWORD timeout) {
  DWORD completionCount;

  if (portUpdateEvents(portState) < 0)
    return -1;

  portState->activePollCount++;

  LeaveCriticalSection(&portState->lock);

  BOOL r = GetQueuedCompletionStatusEx(portState->iocpHandle,
                                       iocpEvents,
                                       maxevents,
                                       &completionCount,
                                       timeout,
                                       FALSE);

  EnterCriticalSection(&portState->lock);

  portState->activePollCount--;

  if (!r)
    RETURN_MAP_ERROR(-1);

  return portFeedEvents(
      portState, epollEvents, iocpEvents, completionCount);
}

int portWait(PortState* portState,
              struct epoll_event* events,
              int maxevents,
              int timeout) {
  OVERLAPPED_ENTRY stackIocpEvents[PORT__MAX_ON_STACK_COMPLETIONS];
  OVERLAPPED_ENTRY* iocpEvents = stackIocpEvents;
  /* 只有 maxevents 很大时才上堆：改用 unique_ptr 后所有返回路径都自动归还，不必再手工 free */
  std::unique_ptr<OVERLAPPED_ENTRY[]> heapIocpEvents;
  uint64_t due = 0;
  DWORD completionTimeout;
  int result;

  /* Check whether `maxevents` is in range. */
  if (maxevents <= 0)
    RETURN_SET_ERROR(-1, ERROR_INVALID_PARAMETER);

  /* Decide whether the IOCP completion list can live on the stack, or allocate
   * memory for it on the heap. The heap allocation is nothrow on purpose: if it
   * fails, fall back to the stack array instead of failing the whole wait. */
  if (static_cast<std::size_t>(maxevents) > ARRAY_COUNT(stackIocpEvents)) {
    heapIocpEvents.reset(
        new (std::nothrow) OVERLAPPED_ENTRY[static_cast<std::size_t>(maxevents)]);

    if (heapIocpEvents != nullptr)
      iocpEvents = heapIocpEvents.get();
    else
      maxevents = static_cast<int>(ARRAY_COUNT(stackIocpEvents));
  }

  /* Compute the timeout for GetQueuedCompletionStatus, and the wait end
   * time, if the user specified a timeout other than zero or infinite. */
  if (timeout > 0) {
    due = GetTickCount64() + static_cast<std::uint64_t>(timeout);
    completionTimeout = static_cast<DWORD>(timeout);
  } else if (timeout == 0) {
    completionTimeout = 0;
  } else {
    completionTimeout = INFINITE;
  }

  EnterCriticalSection(&portState->lock);

  /* Dequeue completion packets until either at least one interesting event
   * has been discovered, or the timeout is reached. */
  for (;;) {
    uint64_t now;

    result = portPoll(
        portState, events, iocpEvents, static_cast<DWORD>(maxevents), completionTimeout);
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
    completionTimeout = static_cast<DWORD>(due - now);
  }

  portUpdateEventsIfPolling(portState);

  LeaveCriticalSection(&portState->lock);

  if (result >= 0)
    return result;
  else if (GetLastError() == WAIT_TIMEOUT)
    return 0;
  else
    return -1;
}

static int portCtlAdd(PortState* portState,
                         SOCKET sock,
                         struct epoll_event* ev) {
  SockState* sockState = sockNew(portState, sock);
  if (sockState == nullptr)
    return -1;

  if (sockSetEvent(portState, sockState, ev) < 0) {
    sockDelete(portState, sockState);
    return -1;
  }

  portUpdateEventsIfPolling(portState);

  return 0;
}

static int portCtlMod(PortState* portState,
                         SOCKET sock,
                         struct epoll_event* ev) {
  SockState* sockState = portFindSocket(portState, sock);
  if (sockState == nullptr)
    return -1;

  if (sockSetEvent(portState, sockState, ev) < 0)
    return -1;

  portUpdateEventsIfPolling(portState);

  return 0;
}

static int portCtlDel(PortState* portState, SOCKET sock) {
  SockState* sockState = portFindSocket(portState, sock);
  if (sockState == nullptr)
    return -1;

  sockDelete(portState, sockState);

  return 0;
}

static int portCtlOp(PortState* portState,
                        int op,
                        SOCKET sock,
                        struct epoll_event* ev) {
  switch (op) {
    case EPOLL_CTL_ADD:
      return portCtlAdd(portState, sock, ev);
    case EPOLL_CTL_MOD:
      return portCtlMod(portState, sock, ev);
    case EPOLL_CTL_DEL:
      return portCtlDel(portState, sock);
    default:
      RETURN_SET_ERROR(-1, ERROR_INVALID_PARAMETER);
  }
}

int portCtl(PortState* portState,
             int op,
             SOCKET sock,
             struct epoll_event* ev) {
  int result;

  EnterCriticalSection(&portState->lock);
  result = portCtlOp(portState, op, sock, ev);
  LeaveCriticalSection(&portState->lock);

  return result;
}

int portRegisterSocket(PortState* portState,
                         SockState* sockState,
                         SOCKET socket) {
  if (treeAdd(&portState->sockTree,
               sockStateToTreeNode(sockState),
               socket) < 0)
    RETURN_SET_ERROR(-1, ERROR_ALREADY_EXISTS);
  return 0;
}

void portUnregisterSocket(PortState* portState,
                            SockState* sockState) {
  treeDel(&portState->sockTree, sockStateToTreeNode(sockState));
}

SockState* portFindSocket(PortState* portState, SOCKET socket) {
  TreeNode* treeNode = treeFind(&portState->sockTree, socket);
  if (treeNode == nullptr)
    RETURN_SET_ERROR(nullptr, ERROR_NOT_FOUND);
  return sockStateFromTreeNode(treeNode);
}

void portRequestSocketUpdate(PortState* portState,
                                SockState* sockState) {
  if (queueIsEnqueued(sockStateToQueueNode(sockState)))
    return;
  queueAppend(&portState->sockUpdateQueue,
               sockStateToQueueNode(sockState));
}

void portCancelSocketUpdate(PortState* portState,
                               SockState* sockState) {
  UNUSED_VAR(portState);
  if (!queueIsEnqueued(sockStateToQueueNode(sockState)))
    return;
  queueRemove(sockStateToQueueNode(sockState));
}

void portAddDeletedSocket(PortState* portState,
                             SockState* sockState) {
  if (queueIsEnqueued(sockStateToQueueNode(sockState)))
    return;
  queueAppend(&portState->sockDeletedQueue,
               sockStateToQueueNode(sockState));
}

void portRemoveDeletedSocket(PortState* portState,
                                SockState* sockState) {
  UNUSED_VAR(portState);
  if (!queueIsEnqueued(sockStateToQueueNode(sockState)))
    return;
  queueRemove(sockStateToQueueNode(sockState));
}

HANDLE portGetIocpHandle(PortState* portState) {
  assert(portState->iocpHandle != nullptr);
  return portState->iocpHandle;
}

Queue* portGetPollGroupQueue(PortState* portState) {
  return &portState->pollGroupQueue;
}

PortState* portStateFromHandleTreeNode(TsTreeNode* treeNode) {
  return CONTAINER_OF(treeNode, PortState, handleTreeNode);
}

TsTreeNode* portStateToHandleTreeNode(PortState* portState) {
  return &portState->handleTreeNode;
}

void queueInit(Queue* queue) {
  queueNodeInit(&queue->head);
}

void queueNodeInit(QueueNode* node) {
  /* 自指即「不在任何表里」：入队与摘除都靠这条不变式判断，因此不需要额外的标记位 */
  node->previous = node;
  node->next     = node;
}

/* 把节点从它当前所在的表里摘下来，不动它自己的 previous/next 指向。
   queueRemove 与两个 queueMove 共用，改动这里要同时顾到「被摘的可能是首元素或尾元素」。 */
static inline void queueDetachNode(QueueNode* node) {
  node->previous->next = node->next;
  node->next->previous = node->previous;
}

QueueNode* queueFirst(const Queue* queue) {
  return !queueIsEmpty(queue) ? queue->head.next : nullptr;
}

QueueNode* queueLast(const Queue* queue) {
  return !queueIsEmpty(queue) ? queue->head.previous : nullptr;
}

void queueAppend(Queue* queue, QueueNode* node) {
  node->next             = &queue->head;
  node->previous         = queue->head.previous;
  node->previous->next   = node;
  queue->head.previous   = node;
}

void queueMoveToStart(Queue* queue, QueueNode* node) {
  queueDetachNode(node);

  /* 插到哨兵之后即队首（这里不再单列一个 prepend：只有这一处用它） */
  node->next           = queue->head.next;
  node->previous       = &queue->head;
  node->next->previous = node;
  queue->head.next     = node;
}

void queueMoveToEnd(Queue* queue, QueueNode* node) {
  queueDetachNode(node);
  queueAppend(queue, node);
}

void queueRemove(QueueNode* node) {
  queueDetachNode(node);
  queueNodeInit(node);
}

bool queueIsEmpty(const Queue* queue) {
  return !queueIsEnqueued(&queue->head);
}

bool queueIsEnqueued(const QueueNode* node) {
  return node->previous != node;
}


static const uint32_t SOCK__KNOWN_EPOLL_EVENTS =
    EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDNORM |
    EPOLLRDBAND | EPOLLWRNORM | EPOLLWRBAND | EPOLLMSG | EPOLLRDHUP;

enum class SockPollStatus {
  Idle = 0,   ///< 尚未下发轮询请求
  Pending,    ///< 已下发 AFD 轮询，等待完成
  Cancelled   ///< 已请求撤销，等待回收
};

struct SockState {
  IO_STATUS_BLOCK ioStatusBlock;
  AFD_POLL_INFO pollInfo;
  QueueNode queueNode;
  TreeNode treeNode;
  PollGroup* pollGroup;
  SOCKET baseSocket;
  epoll_data_t userData;
  uint32_t userEvents;
  uint32_t pendingEvents;
  SockPollStatus pollStatus;
  bool deletePending;
};

static inline SockState* sockAlloc(void) {
  SockState* sockState = new (std::nothrow) SockState{};
  if (sockState == nullptr)
    RETURN_SET_ERROR(nullptr, ERROR_NOT_ENOUGH_MEMORY);
  return sockState;
}

static inline void sockFree(SockState* sockState) {
  delete sockState;
}

static int sockCancelPoll(SockState* sockState) {
  assert(sockState->pollStatus == SockPollStatus::Pending);

  if (afdCancelPoll(pollGroupGetAfdHelperHandle(sockState->pollGroup),
                      &sockState->ioStatusBlock) < 0)
    return -1;

  sockState->pollStatus = SockPollStatus::Cancelled;
  sockState->pendingEvents = 0;
  return 0;
}

SockState* sockNew(PortState* portState, SOCKET socket) {
  SOCKET baseSocket;
  PollGroup* pollGroup;
  SockState* sockState;

  if (socket == 0 || socket == INVALID_SOCKET)
    RETURN_SET_ERROR(nullptr, ERROR_INVALID_HANDLE);

  baseSocket = wsGetBaseSocket(socket);
  if (baseSocket == INVALID_SOCKET)
    return nullptr;

  pollGroup = pollGroupAcquire(portState);
  if (pollGroup == nullptr)
    return nullptr;

  sockState = sockAlloc();
  if (sockState == nullptr) {
    pollGroupRelease(pollGroup);
    return nullptr;
  }

  sockState->baseSocket = baseSocket;
  sockState->pollGroup = pollGroup;

  treeNodeInit(&sockState->treeNode);
  queueNodeInit(&sockState->queueNode);

  if (portRegisterSocket(portState, sockState, socket) < 0) {
    sockFree(sockState);
    pollGroupRelease(pollGroup);
    return nullptr;
  }

  return sockState;
}

static int sockDeleteInternal(PortState* portState,
                        SockState* sockState,
                        bool force) {
  if (!sockState->deletePending) {
    if (sockState->pollStatus == SockPollStatus::Pending)
      sockCancelPoll(sockState);

    portCancelSocketUpdate(portState, sockState);
    portUnregisterSocket(portState, sockState);

    sockState->deletePending = true;
  }

  /* If the poll request still needs to complete, the sockState object can't
   * be free()d yet. `sockFeedEvent()` or `portClose()` will take care
   * of this later. */
  if (force || sockState->pollStatus == SockPollStatus::Idle) {
    /* Free the sockState now. */
    portRemoveDeletedSocket(portState, sockState);
    pollGroupRelease(sockState->pollGroup);
    sockFree(sockState);
  } else {
    /* Free the socket later. */
    portAddDeletedSocket(portState, sockState);
  }

  return 0;
}

void sockDelete(PortState* portState, SockState* sockState) {
  sockDeleteInternal(portState, sockState, false);
}

void sockForceDelete(PortState* portState, SockState* sockState) {
  sockDeleteInternal(portState, sockState, true);
}

int sockSetEvent(PortState* portState,
                   SockState* sockState,
                   const struct epoll_event* ev) {
  /* EPOLLERR and EPOLLHUP are always reported, even when not requested by the
   * caller. However they are disabled after a event has been reported for a
   * socket for which the EPOLLONESHOT flag as set. */
  uint32_t events = ev->events | EPOLLERR | EPOLLHUP;

  sockState->userEvents = events;
  sockState->userData = ev->data;

  if ((events & SOCK__KNOWN_EPOLL_EVENTS & ~sockState->pendingEvents) != 0)
    portRequestSocketUpdate(portState, sockState);

  return 0;
}

static inline DWORD sockEpollEventsToAfdEvents(uint32_t epollEvents) {
  /* Always monitor for AFD_POLL_LOCAL_CLOSE, which is triggered when the
   * socket is closed with closesocket() or CloseHandle(). */
  DWORD afdEvents = AFD_POLL_LOCAL_CLOSE;

  if (epollEvents & (EPOLLIN | EPOLLRDNORM))
    afdEvents |= AFD_POLL_RECEIVE | AFD_POLL_ACCEPT;
  if (epollEvents & (EPOLLPRI | EPOLLRDBAND))
    afdEvents |= AFD_POLL_RECEIVE_EXPEDITED;
  if (epollEvents & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND))
    afdEvents |= AFD_POLL_SEND;
  if (epollEvents & (EPOLLIN | EPOLLRDNORM | EPOLLRDHUP))
    afdEvents |= AFD_POLL_DISCONNECT;
  if (epollEvents & EPOLLHUP)
    afdEvents |= AFD_POLL_ABORT;
  if (epollEvents & EPOLLERR)
    afdEvents |= AFD_POLL_CONNECT_FAIL;

  return afdEvents;
}

static inline uint32_t sockAfdEventsToEpollEvents(DWORD afdEvents) {
  uint32_t epollEvents = 0;

  if (afdEvents & (AFD_POLL_RECEIVE | AFD_POLL_ACCEPT))
    epollEvents |= EPOLLIN | EPOLLRDNORM;
  if (afdEvents & AFD_POLL_RECEIVE_EXPEDITED)
    epollEvents |= EPOLLPRI | EPOLLRDBAND;
  if (afdEvents & AFD_POLL_SEND)
    epollEvents |= EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;
  if (afdEvents & AFD_POLL_DISCONNECT)
    epollEvents |= EPOLLIN | EPOLLRDNORM | EPOLLRDHUP;
  if (afdEvents & AFD_POLL_ABORT)
    epollEvents |= EPOLLHUP;
  if (afdEvents & AFD_POLL_CONNECT_FAIL)
    /* Linux reports all these events after connect() has failed. */
    epollEvents |=
        EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLRDNORM | EPOLLWRNORM | EPOLLRDHUP;

  return epollEvents;
}

int sockUpdate(PortState* portState, SockState* sockState) {
  assert(!sockState->deletePending);

  if ((sockState->pollStatus == SockPollStatus::Pending) &&
      (sockState->userEvents & SOCK__KNOWN_EPOLL_EVENTS &
       ~sockState->pendingEvents) == 0) {
    /* All the events the user is interested in are already being monitored by
     * the pending poll operation. It might spuriously complete because of an
     * event that we're no longer interested in; when that happens we'll submit
     * a new poll operation with the updated event mask. */

  } else if (sockState->pollStatus == SockPollStatus::Pending) {
    /* A poll operation is already pending, but it's not monitoring for all the
     * events that the user is interested in. Therefore, cancel the pending
     * poll operation; when we receive it's completion package, a new poll
     * operation will be submitted with the correct event mask. */
    if (sockCancelPoll(sockState) < 0)
      return -1;

  } else if (sockState->pollStatus == SockPollStatus::Cancelled) {
    /* The poll operation has already been cancelled, we're still waiting for
     * it to return. For now, there's nothing that needs to be done. */

  } else if (sockState->pollStatus == SockPollStatus::Idle) {
    /* No poll operation is pending; start one. */
    sockState->pollInfo.Exclusive = FALSE;
    sockState->pollInfo.NumberOfHandles = 1;
    sockState->pollInfo.Timeout.QuadPart = INT64_MAX;
    sockState->pollInfo.Handles[0].Handle = reinterpret_cast<HANDLE>(sockState->baseSocket);
    sockState->pollInfo.Handles[0].Status = 0;
    sockState->pollInfo.Handles[0].Events =
        sockEpollEventsToAfdEvents(sockState->userEvents);

    if (afdPoll(pollGroupGetAfdHelperHandle(sockState->pollGroup),
                 &sockState->pollInfo,
                 &sockState->ioStatusBlock) < 0) {
      switch (GetLastError()) {
        case ERROR_IO_PENDING:
          /* Overlapped poll operation in progress; this is expected. */
          break;
        case ERROR_INVALID_HANDLE:
          /* Socket closed; it'll be dropped from the epoll set. */
          return sockDeleteInternal(portState, sockState, false);
        default:
          /* Other errors are propagated to the caller. */
          RETURN_MAP_ERROR(-1);
      }
    }

    /* The poll request was successfully submitted. */
    sockState->pollStatus = SockPollStatus::Pending;
    sockState->pendingEvents = sockState->userEvents;

  } else {
    /* Unreachable. */
    assert(false);
  }

  portCancelSocketUpdate(portState, sockState);
  return 0;
}

int sockFeedEvent(PortState* portState,
                    IO_STATUS_BLOCK* ioStatusBlock,
                    struct epoll_event* ev) {
  SockState* sockState =
      CONTAINER_OF(ioStatusBlock, SockState, ioStatusBlock);
  AFD_POLL_INFO* pollInfo = &sockState->pollInfo;
  uint32_t epollEvents = 0;

  sockState->pollStatus = SockPollStatus::Idle;
  sockState->pendingEvents = 0;

  if (sockState->deletePending) {
    /* Socket has been deleted earlier and can now be freed. */
    return sockDeleteInternal(portState, sockState, false);

  } else if (ioStatusBlock->Status == STATUS_CANCELLED) {
    /* The poll request was cancelled by CancelIoEx. */

  } else if (!NT_SUCCESS(ioStatusBlock->Status)) {
    /* The overlapped request itself failed in an unexpected way. */
    epollEvents = EPOLLERR;

  } else if (pollInfo->NumberOfHandles < 1) {
    /* This poll operation succeeded but didn't report any socket events. */

  } else if (pollInfo->Handles[0].Events & AFD_POLL_LOCAL_CLOSE) {
    /* The poll operation reported that the socket was closed. */
    return sockDeleteInternal(portState, sockState, false);

  } else {
    /* Events related to our socket were reported. */
    epollEvents =
        sockAfdEventsToEpollEvents(pollInfo->Handles[0].Events);
  }

  /* Requeue the socket so a new poll request will be submitted. */
  portRequestSocketUpdate(portState, sockState);

  /* Filter out events that the user didn't ask for. */
  epollEvents &= sockState->userEvents;

  /* Return if there are no epoll events to report. */
  if (epollEvents == 0)
    return 0;

  /* If the the socket has the EPOLLONESHOT flag set, unmonitor all events,
   * even EPOLLERR and EPOLLHUP. But always keep looking for closed sockets. */
  if (sockState->userEvents & EPOLLONESHOT)
    sockState->userEvents = 0;

  ev->data = sockState->userData;
  ev->events = epollEvents;
  return 1;
}

SockState* sockStateFromQueueNode(QueueNode* queueNode) {
  return CONTAINER_OF(queueNode, SockState, queueNode);
}

QueueNode* sockStateToQueueNode(SockState* sockState) {
  return &sockState->queueNode;
}

SockState* sockStateFromTreeNode(TreeNode* treeNode) {
  return CONTAINER_OF(treeNode, SockState, treeNode);
}

TreeNode* sockStateToTreeNode(SockState* sockState) {
  return &sockState->treeNode;
}

void tsTreeInit(TsTree* tsTree) {
  treeInit(&tsTree->tree);
  InitializeSRWLock(&tsTree->lock);
}

void tsTreeNodeInit(TsTreeNode* node) {
  treeNodeInit(&node->treeNode);
  node->reflock.ensureInitialized();
}

int tsTreeAdd(TsTree* tsTree, TsTreeNode* node, uintptr_t key) {
  int r;

  AcquireSRWLockExclusive(&tsTree->lock);
  r = treeAdd(&tsTree->tree, &node->treeNode, key);
  ReleaseSRWLockExclusive(&tsTree->lock);

  return r;
}

static inline TsTreeNode* tsTreeFindNode(TsTree* tsTree,
                                                 uintptr_t key) {
  TreeNode* treeNode = treeFind(&tsTree->tree, key);
  if (treeNode == nullptr)
    return nullptr;

  return CONTAINER_OF(treeNode, TsTreeNode, treeNode);
}

TsTreeNode* tsTreeDelAndRef(TsTree* tsTree, uintptr_t key) {
  TsTreeNode* tsTreeNode;

  AcquireSRWLockExclusive(&tsTree->lock);

  tsTreeNode = tsTreeFindNode(tsTree, key);
  if (tsTreeNode != nullptr) {
    treeDel(&tsTree->tree, &tsTreeNode->treeNode);
    tsTreeNode->reflock.ref();
  }

  ReleaseSRWLockExclusive(&tsTree->lock);

  return tsTreeNode;
}

TsTreeNode* tsTreeFindAndRef(TsTree* tsTree, uintptr_t key) {
  TsTreeNode* tsTreeNode;

  AcquireSRWLockShared(&tsTree->lock);

  tsTreeNode = tsTreeFindNode(tsTree, key);
  if (tsTreeNode != nullptr)
    tsTreeNode->reflock.ref();

  ReleaseSRWLockShared(&tsTree->lock);

  return tsTreeNode;
}

void tsTreeNodeUnref(TsTreeNode* node) {
  node->reflock.unref();
}

void tsTreeNodeUnrefAndDestroy(TsTreeNode* node) {
  node->reflock.unrefAndDestroy();
}

void treeInit(Tree* tree) {
  /* 只用于生命周期已经开始的对象（静态的 epollHandleTree）；malloc 存储上的那份由
     portNew 直接 construct_at，不能用这里的赋值。 */
  *tree = Tree{};
}

void treeNodeInit(TreeNode* node) {
  node->key = 0;
}

int treeAdd(Tree* tree, TreeNode* node, uintptr_t key) {
  node->key = key;

  /* 同一个键重复插入是调用方的错误：返回 -1 让它按 EEXIST 报出去（与手写树同样的约定） */
  return tree->emplace(key, node).second ? 0 : -1;
}

void treeDel(Tree* tree, TreeNode* node) {
  tree->erase(node->key);
}

TreeNode* treeFind(const Tree* tree, uintptr_t key) {
  const auto iterator = tree->find(key);
  return iterator == tree->end() ? nullptr : iterator->second;
}

TreeNode* treeRoot(const Tree* tree) {
  /* 只给 portDelete 的「逐个摘除直到空」用：给最小键的那个节点即可，
     原实现给的是红黑树的根（同样是「随便一个」，摘除顺序本就不可依赖） */
  return tree->empty() ? nullptr : tree->begin()->second;
}

#ifndef SIO_BSP_HANDLE_POLL
#define SIO_BSP_HANDLE_POLL 0x4800001D
#endif

#ifndef SIO_BASE_HANDLE
#define SIO_BASE_HANDLE 0x48000022
#endif

int wsGlobalInit(void) {
  int r;
  WSADATA wsaData;

  r = WSAStartup(MAKEWORD(2, 2), &wsaData);
  if (r != 0)
    RETURN_SET_ERROR(-1, static_cast<DWORD>(r));

  return 0;
}

static inline SOCKET wsIoctlGetBspSocket(SOCKET socket, DWORD ioctl) {
  SOCKET bspSocket;
  DWORD bytes;

  if (WSAIoctl(socket,
               ioctl,
               nullptr,
               0,
               &bspSocket,
               sizeof bspSocket,
               &bytes,
               nullptr,
               nullptr) != SOCKET_ERROR)
    return bspSocket;
  else
    return INVALID_SOCKET;
}

SOCKET wsGetBaseSocket(SOCKET socket) {
  SOCKET baseSocket;
  DWORD error;

  for (;;) {
    baseSocket = wsIoctlGetBspSocket(socket, SIO_BASE_HANDLE);
    if (baseSocket != INVALID_SOCKET)
      return baseSocket;

    error = GetLastError();
    if (error == WSAENOTSOCK)
      RETURN_SET_ERROR(INVALID_SOCKET, error);

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

    baseSocket = wsIoctlGetBspSocket(socket, SIO_BSP_HANDLE_POLL);
    if (baseSocket != INVALID_SOCKET && baseSocket != socket)
      socket = baseSocket;
    else
      RETURN_SET_ERROR(INVALID_SOCKET, error);
  }
}
