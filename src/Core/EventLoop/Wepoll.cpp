#include "wepoll.h"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
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

namespace AsynGyanis::Core
{

    /** @brief 从 ntdll 解析本实现用到的 NT 函数指针 */
    WEPOLL_INTERNAL int ntGlobalInit();

    using NTSTATUS  = LONG;
    using PNTSTATUS = NTSTATUS *;

    /**
     * @brief 判断 NTSTATUS 是否表示成功
     * @param status 待判定的状态码
     * @return true 成功（NT 约定：非负即成功）
     */
    inline constexpr bool ntSuccess(const NTSTATUS status) noexcept
    {
        return status >= 0;
    }

    // 下面这些与 SDK 头（ntdef.h / winnt.h）同名的常量只能用宏 + #ifndef 守卫：
    // 有则用系统定义、没有才补上；换成 constexpr 变量会在 SDK 也定义的那些宏面前直接编译错。
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

    typedef struct _IO_STATUS_BLOCK
    {
        NTSTATUS  Status;
        ULONG_PTR Information;
    } IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

    typedef VOID (NTAPI *PIO_APC_ROUTINE)(PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, ULONG Reserved);

    typedef struct _UNICODE_STRING
    {
        USHORT Length;
        USHORT MaximumLength;
        PWSTR  Buffer;
    } UNICODE_STRING, *PUNICODE_STRING;

#define RTL_CONSTANT_STRING(s) {sizeof(s) - sizeof((s)[0]), sizeof(s), const_cast<PWSTR>(s)}

    typedef struct _OBJECT_ATTRIBUTES
    {
        ULONG           Length;
        HANDLE          RootDirectory;
        PUNICODE_STRING ObjectName;
        ULONG           Attributes;
        PVOID           SecurityDescriptor;
        PVOID           SecurityQualityOfService;
    } OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

#define RTL_CONSTANT_OBJECT_ATTRIBUTES(ObjectName, Attributes) {sizeof(OBJECT_ATTRIBUTES), nullptr, ObjectName, Attributes, nullptr, nullptr}

#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001UL
#endif

#define NT_NTDLL_IMPORT_LIST(X)                                                                                                                                                    \
    X(NTSTATUS, NTAPI, NtCancelIoFileEx, (HANDLE FileHandle, PIO_STATUS_BLOCK IoRequestToCancel, PIO_STATUS_BLOCK IoStatusBlock))                                                  \
                                                                                                                                                                                   \
    X(NTSTATUS, NTAPI, NtCreateFile,                                                                                                                                               \
      (PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,    \
       ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength))                                                                           \
                                                                                                                                                                                   \
    X(NTSTATUS, NTAPI, NtDeviceIoControlFile,                                                                                                                                      \
      (HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode, PVOID InputBuffer,                      \
       ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength))                                                                                                     \
                                                                                                                                                                                   \
    X(ULONG, WINAPI, RtlNtStatusToDosError, (NTSTATUS Status))

#define X(returnType, attributes, name, parameters) WEPOLL_INTERNAL_VAR returnType(attributes *name) parameters = nullptr;
    NT_NTDLL_IMPORT_LIST(X)
#undef X

    // AFD 驱动的事件位：ntddafd.h 是驱动侧私有接口、不在 SDK 里，取值必须逐位对齐，
    // 名字因此照抄那边的常量而不是本工程的常量命名
    static constexpr std::uint32_t AFD_POLL_RECEIVE           = 0x0001;
    static constexpr std::uint32_t AFD_POLL_RECEIVE_EXPEDITED = 0x0002;
    static constexpr std::uint32_t AFD_POLL_SEND              = 0x0004;
    static constexpr std::uint32_t AFD_POLL_DISCONNECT        = 0x0008;
    static constexpr std::uint32_t AFD_POLL_ABORT             = 0x0010;
    static constexpr std::uint32_t AFD_POLL_LOCAL_CLOSE       = 0x0020;
    static constexpr std::uint32_t AFD_POLL_ACCEPT            = 0x0080;
    static constexpr std::uint32_t AFD_POLL_CONNECT_FAIL      = 0x0100;

    typedef struct _AFD_POLL_HANDLE_INFO
    {
        HANDLE   Handle;
        ULONG    Events;
        NTSTATUS Status;
    } AFD_POLL_HANDLE_INFO, *PAFD_POLL_HANDLE_INFO;

    typedef struct _AFD_POLL_INFO
    {
        LARGE_INTEGER        Timeout;
        ULONG                NumberOfHandles;
        ULONG                Exclusive;
        AFD_POLL_HANDLE_INFO Handles[1];
    } AFD_POLL_INFO, *PAFD_POLL_INFO;

    /** @brief 打开 AFD 设备的辅助句柄，并把它与 IOCP 端口关联起来 */
    WEPOLL_INTERNAL int afdCreateHelperHandle(HANDLE iocpHandle, HANDLE *afdHelperHandleOut);

    /** @brief 向 AFD 下发一次异步轮询请求 */
    WEPOLL_INTERNAL int afdPoll(HANDLE afdHelperHandle, AFD_POLL_INFO *pollInfo, IO_STATUS_BLOCK *ioStatusBlock);

    /** @brief 撤销尚未完成的 AFD 轮询请求 */
    WEPOLL_INTERNAL int afdCancelPoll(HANDLE afdHelperHandle, IO_STATUS_BLOCK *ioStatusBlock);

    /** @brief 读取当前 LastError，按映射表设置 errno */
    WEPOLL_INTERNAL void errorMapWindowsError();

    /** @brief 按给定错误码同时设置 LastError 与 errno */
    WEPOLL_INTERNAL void errorSetWindowsError(DWORD error);

    /**
     * @brief 记下当前 LastError 的映射结果，再把失败值原样交回，供调用点直接 return
     * @tparam ValueType 失败值类型，与调用方的返回类型一致（也包括指针）
     * @param failureValue 要交回的失败值
     * @return ValueType 原样交回 failureValue
     */
    template<typename ValueType>
    [[nodiscard]] ValueType failWithLastWindowsError(const ValueType failureValue)
    {
        errorMapWindowsError();
        return failureValue;
    }

    /**
     * @brief 按给定错误码设置 errno 与 LastError，再把失败值原样交回，供调用点直接 return
     * @tparam ValueType 失败值类型，与调用方的返回类型一致（也包括指针）
     * @param failureValue 要交回的失败值
     * @param error 要设置的 Win32 错误码
     * @return ValueType 原样交回 failureValue
     */
    template<typename ValueType>
    [[nodiscard]] ValueType failWithWindowsError(const ValueType failureValue, const DWORD error)
    {
        errorSetWindowsError(error);
        return failureValue;
    }

    /** @brief 校验句柄是否有效，无效则按 ERROR_INVALID_HANDLE 收口 */
    WEPOLL_INTERNAL int errorCheckHandle(HANDLE handle);

    /// AFD 轮询的 ioctl 码，同样取自驱动侧私有接口
    static constexpr std::uint32_t IOCTL_AFD_POLL = 0x00012024;

    static UNICODE_STRING afdHelperName = RTL_CONSTANT_STRING(L"\\Device\\Afd\\Wepoll");

    static OBJECT_ATTRIBUTES afdHelperAttributes = RTL_CONSTANT_OBJECT_ATTRIBUTES(&afdHelperName, 0);

    int afdCreateHelperHandle(HANDLE iocpHandle, HANDLE *afdHelperHandleOut)
    {
        HANDLE          afdHelperHandle;
        IO_STATUS_BLOCK iosb;
        NTSTATUS        status;

        // 打开 \Device\Afd 时不带任何扩展属性：拿到的句柄只能与 AFD 驱动对话，
        // 它没有绑定的端点，因此不是一个套接字。
        status = NtCreateFile(&afdHelperHandle, SYNCHRONIZE, &afdHelperAttributes, &iosb, nullptr, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, nullptr, 0);
        if (status != STATUS_SUCCESS)
            return failWithWindowsError(-1, RtlNtStatusToDosError(status));

        // 句柄已经拿到：这两步任一步失败都要先关掉它再报错。短路求值保证第二步只在第一步
        // 成功时才执行，报出来的仍是真正失败那一步的错误码
        if (CreateIoCompletionPort(afdHelperHandle, iocpHandle, 0, 0) == nullptr || !SetFileCompletionNotificationModes(afdHelperHandle, FILE_SKIP_SET_EVENT_ON_HANDLE))
        {
            CloseHandle(afdHelperHandle);
            return failWithLastWindowsError(-1);
        }

        *afdHelperHandleOut = afdHelperHandle;
        return 0;
    }

    int afdPoll(HANDLE afdHelperHandle, AFD_POLL_INFO *pollInfo, IO_STATUS_BLOCK *ioStatusBlock)
    {
        NTSTATUS status;

        // 不支持无限等待：Timeout 为负值一律按非法参数拒绝。
        assert(ioStatusBlock != nullptr);

        ioStatusBlock->Status = STATUS_PENDING;
        status = NtDeviceIoControlFile(afdHelperHandle, nullptr, nullptr, ioStatusBlock, ioStatusBlock, IOCTL_AFD_POLL, pollInfo, sizeof *pollInfo, pollInfo, sizeof *pollInfo);

        if (status == STATUS_SUCCESS)
            return 0;
        else if (status == STATUS_PENDING)
            return failWithWindowsError(-1, ERROR_IO_PENDING);
        else
            return failWithWindowsError(-1, RtlNtStatusToDosError(status));
    }

    int afdCancelPoll(HANDLE afdHelperHandle, IO_STATUS_BLOCK *ioStatusBlock)
    {
        NTSTATUS        cancelStatus;
        IO_STATUS_BLOCK cancelIoStatusBlock;

        // 这次轮询若已完成或此前已被撤销，就没有什么要做的了。
        if (ioStatusBlock->Status != STATUS_PENDING)
            return 0;

        cancelStatus = NtCancelIoFileEx(afdHelperHandle, ioStatusBlock, &cancelIoStatusBlock);

        // 操作恰好在 NtCancelIoFileEx() 之前完成时，它会返回 STATUS_NOT_FOUND，
        // 这不算错误。
        if (cancelStatus == STATUS_SUCCESS || cancelStatus == STATUS_NOT_FOUND)
            return 0;
        else
            return failWithWindowsError(-1, RtlNtStatusToDosError(cancelStatus));
    }

    /** @brief 初始化 epoll 层的全局句柄表 */
    WEPOLL_INTERNAL int epollGlobalInit();

    /** @brief 保证全局初始化已完成，重复调用只做一次 */
    WEPOLL_INTERNAL int ensureInitialized();

    struct PortState;
    struct Queue;
    struct SockState;
    struct TsTreeNode;

    /** @brief 新建端口状态：IOCP 端口、套接字表与队列、句柄树节点一次就位 */
    WEPOLL_INTERNAL PortState *portNew(HANDLE *iocpHandleOut);

    /** @brief 关闭端口：注销全部套接字并释放资源 */
    WEPOLL_INTERNAL int portClose(PortState *portState);

    /** @brief 关闭并释放端口，同时从全局句柄树摘除 */
    WEPOLL_INTERNAL int portDelete(PortState *portState);

    /** @brief epoll_wait 的实现：取事件并算出等待超时 */
    WEPOLL_INTERNAL int portWait(PortState *portState, struct epoll_event *events, int maxevents, int timeout);

    /** @brief epoll_ctl 的实现：校验参数后按操作码分派 */
    WEPOLL_INTERNAL int portCtl(PortState *portState, int op, SOCKET sock, struct epoll_event *ev);

    /** @brief 把套接字插进端口的套接字表 */
    WEPOLL_INTERNAL int portRegisterSocket(PortState *portState, SockState *sockState, SOCKET socket);

    /** @brief 把套接字从端口的套接字表里摘掉 */
    WEPOLL_INTERNAL void portUnregisterSocket(PortState *portState, SockState *sockState);

    /** @brief 按套接字句柄查出对应的套接字状态 */
    WEPOLL_INTERNAL SockState *portFindSocket(PortState *portState, SOCKET socket);

    /** @brief 把套接字挂进待更新队列 */
    WEPOLL_INTERNAL void portRequestSocketUpdate(PortState *portState, SockState *sockState);

    /** @brief 把套接字从待更新队列移到待删除队列 */
    WEPOLL_INTERNAL void portCancelSocketUpdate(PortState *portState, SockState *sockState);

    /** @brief 把套接字挂进待删除队列（延后释放） */
    WEPOLL_INTERNAL void portAddDeletedSocket(PortState *portState, SockState *sockState);

    /** @brief 把套接字从待删除队列摘掉 */
    WEPOLL_INTERNAL void portRemoveDeletedSocket(PortState *portState, SockState *sockState);

    /** @brief 取端口的 IOCP 句柄 */
    WEPOLL_INTERNAL HANDLE portGetIocpHandle(PortState *portState);

    /** @brief 取端口的轮询组队列 */
    WEPOLL_INTERNAL Queue *portGetPollGroupQueue(PortState *portState);

    /** @brief 从句柄树节点取回所属的端口状态 */
    WEPOLL_INTERNAL PortState *portStateFromHandleTreeNode(TsTreeNode *treeNode);

    /** @brief 取端口状态里用于句柄表的树节点 */
    WEPOLL_INTERNAL TsTreeNode *portStateToHandleTreeNode(PortState *portState);

    /**
     * @brief 引用锁：把「这块内存不许被提前释放」与「销毁时等所有引用放完」合进一个原子字
     * @details 低 28 位是引用计数、高 4 位是销毁标记，等待与唤醒走 C++20 的 atomic::wait/notify （标准库在 Windows 上用 WaitOnAddress 实现），原实现自备的 keyed event 因此整块删掉
     * 协议： - ref()：只在计数上加一，无等待；对已销毁的锁调用是用法错误，由断言兜住； - unref()：减一；恰好减到「只剩销毁标记」时唤醒正在等的销毁者； -
     * unrefAndDestroy()：在同一个原子操作里放下自己那份引用并置上销毁标记，然后等其它引用放完 ——这一步是本类比 shared_ptr 多出来的：销毁者要确认没人还在用这块内存； -
     * 销毁完成后写入毒值，此后任何 ref/unref 都会被断言抓住
     */
    class RefLock
    {
    public:
        /// 初始化为「零引用、未销毁」
        void reset() noexcept
        {
            m_state.store(0, std::memory_order_relaxed);
        }

        /// 增加一份引用
        void ref() noexcept
        {
            const std::uint32_t state = m_state.fetch_add(kReferenceUnit, std::memory_order_acq_rel) + kReferenceUnit;

            // 计数不得溢出，也不得在销毁之后再加引用（NDEBUG 下断言消失，故显式丢弃该值）
            static_cast<void>(state);
            assert((state & kDestroyMask) == 0);
        }

        /// 释放一份引用；恰好是最后一份时唤醒等在那里的销毁者
        void unref() noexcept
        {
            const std::uint32_t state = m_state.fetch_sub(kReferenceUnit, std::memory_order_acq_rel) - kReferenceUnit;

            // 计数不得下溢，也不得对已销毁的锁再释放
            assert((state & kDestroyMask & ~kDestroyFlag) == 0);

            if (state == kDestroyFlag)
                m_state.notify_one();
        }

        /// 放下自己那份引用并置上销毁标记，然后等其它引用全部放完
        void unrefAndDestroy() noexcept
        {
            // fetch_add 返回的是加之前的值，而这里要判断加之后的状态，故自己补上增量
            const std::uint32_t delta = kDestroyFlag - kReferenceUnit;
            const std::uint32_t state = m_state.fetch_add(delta, std::memory_order_acq_rel) + delta;

            // 销毁只能发生一次，且必须由持有引用的那一方发起
            assert((state & kDestroyMask) == kDestroyFlag);

            // 还有别的引用在外面就一直睡；醒来重读，虚假唤醒因此无害
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

    // 注意：树函数失败时不设置 errno 与 LastError。每个对外接口最多只有一种失败
    // 模式，需要时由调用方自己设置恰当的错误码。

    struct TreeNode;

    /** @brief 树节点：只留键，形状信息交给标准容器保管 */
    struct TreeNode
    {
        uintptr_t m_key; ///< 节点只留键：树的形状信息改由标准容器保管
    };

    // 键 → 节点。原先是一棵手写的红黑树（插入、删除、旋转、重平衡近 200 行），而调用方只用到
    // 「按键插入、按键查找、按键删除」三件事，因此交给标准库的有序表：std::map 同样按键有序，
    // 且节点地址稳定（再平衡不会让节点搬家），下面用 CONTAINER_OF 从节点取回宿主结构照旧成立。
    using Tree = std::map<uintptr_t, TreeNode *>;

    /** @brief 初始化「键 → 节点」表（只用于生命周期已经开始的对象） */
    WEPOLL_INTERNAL void treeInit(Tree *tree);

    /** @brief 初始化树的节点 */
    WEPOLL_INTERNAL void treeNodeInit(TreeNode *node);

    /** @brief 按键插入节点，键重复返回 -1 */
    WEPOLL_INTERNAL int treeAdd(Tree *tree, TreeNode *node, uintptr_t key);

    /** @brief 按键删除节点 */
    WEPOLL_INTERNAL void treeDel(Tree *tree, TreeNode *node);

    /** @brief 按键查找节点 */
    WEPOLL_INTERNAL TreeNode *treeFind(const Tree *tree, uintptr_t key);

    /** @brief 取最小键的节点，供「逐个摘除直到空」使用 */
    WEPOLL_INTERNAL TreeNode *treeRoot(const Tree *tree);

    /** @brief 带锁的句柄表：键是 epoll 实例句柄 */
    struct TsTree
    {
        Tree    m_tree;
        SRWLOCK m_lock;
    };

    /** @brief 带引用锁的树节点 */
    struct TsTreeNode
    {
        TreeNode m_treeNode;
        RefLock  m_reflock;
    };

    /** @brief 初始化带锁的句柄树 */
    WEPOLL_INTERNAL void tsTreeInit(TsTree *rtl);

    /** @brief 初始化带引用锁的树节点 */
    WEPOLL_INTERNAL void tsTreeNodeInit(TsTreeNode *node);

    /** @brief 加锁插入节点，键重复按已存在处理 */
    WEPOLL_INTERNAL int tsTreeAdd(TsTree *tsTree, TsTreeNode *node, uintptr_t key);

    /** @brief 摘除节点并对它持一份引用 */
    WEPOLL_INTERNAL TsTreeNode *tsTreeDelAndRef(TsTree *tsTree, uintptr_t key);

    /** @brief 查节点并对它持一份引用，找不到返回空 */
    WEPOLL_INTERNAL TsTreeNode *tsTreeFindAndRef(TsTree *tsTree, uintptr_t key);

    /** @brief 放下一份节点引用 */
    WEPOLL_INTERNAL void tsTreeNodeUnref(TsTreeNode *node);

    /** @brief 放下最后一份引用时销毁节点 */
    WEPOLL_INTERNAL void tsTreeNodeUnrefAndDestroy(TsTreeNode *node);

    static TsTree epollHandleTree;

    int epollGlobalInit()
    {
        tsTreeInit(&epollHandleTree);
        return 0;
    }

    /** @brief 创建一个 epoll 实例并登记进全局句柄表 */
    static HANDLE epollCreate()
    {
        PortState * portState;
        HANDLE      ephnd;
        TsTreeNode *treeNode;

        if (ensureInitialized() < 0)
            return nullptr;

        portState = portNew(&ephnd);
        if (portState == nullptr)
            return nullptr;

        treeNode = portStateToHandleTreeNode(portState);
        if (tsTreeAdd(&epollHandleTree, treeNode, reinterpret_cast<uintptr_t>(ephnd)) < 0)
        {
            // 同一句柄不可能已经在表里：真到了这里说明句柄复用或表被破坏。
            portDelete(portState);
            return failWithWindowsError(nullptr, ERROR_ALREADY_EXISTS);
        }

        return ephnd;
    }

    extern "C"
    {
    /** @brief 创建 epoll 实例 */
    HANDLE epoll_create(int size)
    {
        if (size <= 0)
            return failWithWindowsError(nullptr, ERROR_INVALID_PARAMETER);

        return epollCreate();
    }

    /** @brief 创建 epoll 实例（只接受 flags 为 0） */
    HANDLE epoll_create1(int flags)
    {
        if (flags != 0)
            return failWithWindowsError(nullptr, ERROR_INVALID_PARAMETER);

        return epollCreate();
    }

    /** @brief 关闭 epoll 实例并注销它上面登记的套接字 */
    int epoll_close(HANDLE ephnd)
    {
        TsTreeNode *treeNode;
        PortState * portState;

        if (ensureInitialized() < 0)
            return -1;

        treeNode = tsTreeDelAndRef(&epollHandleTree, reinterpret_cast<uintptr_t>(ephnd));
        if (treeNode == nullptr)
        {
            errorSetWindowsError(ERROR_INVALID_PARAMETER);

            // 与其它入口同一口径：句柄自身的错误优先于「表里找不到」
            errorCheckHandle(ephnd);
            return -1;
        }

        portState = portStateFromHandleTreeNode(treeNode);
        portClose(portState);

        tsTreeNodeUnrefAndDestroy(treeNode);

        return portDelete(portState);
    }

    /** @brief 在实例上注册、修改或注销一个套接字 */
    int epoll_ctl(HANDLE ephnd, int op, SOCKET sock, struct epoll_event *ev)
    {
        TsTreeNode *treeNode;
        PortState * portState;
        int         r;

        if (ensureInitialized() < 0)
            return -1;

        treeNode = tsTreeFindAndRef(&epollHandleTree, reinterpret_cast<uintptr_t>(ephnd));
        if (treeNode == nullptr)
        {
            errorSetWindowsError(ERROR_INVALID_PARAMETER);
            // Linux 的 epoll_ctl() 里 EBADF 优先于其它错误，这里照此行为实现。
            errorCheckHandle(ephnd);
            errorCheckHandle(reinterpret_cast<HANDLE>(sock));
            return -1;
        }

        portState = portStateFromHandleTreeNode(treeNode);
        r         = portCtl(portState, op, sock, ev);

        tsTreeNodeUnref(treeNode);

        if (r < 0)
        {
            // 与上面同一口径：两行重复换来一个没有跳转的单一出口
            errorCheckHandle(ephnd);
            errorCheckHandle(reinterpret_cast<HANDLE>(sock));
            return -1;
        }

        return 0;
    }

    /** @brief 等一批就绪事件 */
    int epoll_wait(HANDLE ephnd, struct epoll_event *events, int maxevents, int timeout)
    {
        TsTreeNode *treeNode;
        PortState * portState;
        int         eventCount;

        if (maxevents <= 0)
            return failWithWindowsError(-1, ERROR_INVALID_PARAMETER);

        if (ensureInitialized() < 0)
            return -1;

        treeNode = tsTreeFindAndRef(&epollHandleTree, reinterpret_cast<uintptr_t>(ephnd));
        if (treeNode == nullptr)
        {
            errorSetWindowsError(ERROR_INVALID_PARAMETER);
            errorCheckHandle(ephnd);
            return -1;
        }

        portState  = portStateFromHandleTreeNode(treeNode);
        eventCount = portWait(portState, events, maxevents, timeout);

        tsTreeNodeUnref(treeNode);

        if (eventCount < 0)
        {
            errorCheckHandle(ephnd);
            return -1;
        }

        return eventCount;
    }
    }

#define ERR__ERRNO_MAPPINGS(X)                                                                                                                                                     \
    X(ERROR_ACCESS_DENIED, EACCES)                                                                                                                                                 \
    X(ERROR_ALREADY_EXISTS, EEXIST)                                                                                                                                                \
    X(ERROR_BAD_COMMAND, EACCES)                                                                                                                                                   \
    X(ERROR_BAD_EXE_FORMAT, ENOEXEC)                                                                                                                                               \
    X(ERROR_BAD_LENGTH, EACCES)                                                                                                                                                    \
    X(ERROR_BAD_NETPATH, ENOENT)                                                                                                                                                   \
    X(ERROR_BAD_NET_NAME, ENOENT)                                                                                                                                                  \
    X(ERROR_BAD_NET_RESP, ENETDOWN)                                                                                                                                                \
    X(ERROR_BAD_PATHNAME, ENOENT)                                                                                                                                                  \
    X(ERROR_BROKEN_PIPE, EPIPE)                                                                                                                                                    \
    X(ERROR_CANNOT_MAKE, EACCES)                                                                                                                                                   \
    X(ERROR_COMMITMENT_LIMIT, ENOMEM)                                                                                                                                              \
    X(ERROR_CONNECTION_ABORTED, ECONNABORTED)                                                                                                                                      \
    X(ERROR_CONNECTION_ACTIVE, EISCONN)                                                                                                                                            \
    X(ERROR_CONNECTION_REFUSED, ECONNREFUSED)                                                                                                                                      \
    X(ERROR_CRC, EACCES)                                                                                                                                                           \
    X(ERROR_DIR_NOT_EMPTY, ENOTEMPTY)                                                                                                                                              \
    X(ERROR_DISK_FULL, ENOSPC)                                                                                                                                                     \
    X(ERROR_DUP_NAME, EADDRINUSE)                                                                                                                                                  \
    X(ERROR_FILENAME_EXCED_RANGE, ENOENT)                                                                                                                                          \
    X(ERROR_FILE_NOT_FOUND, ENOENT)                                                                                                                                                \
    X(ERROR_GEN_FAILURE, EACCES)                                                                                                                                                   \
    X(ERROR_GRACEFUL_DISCONNECT, EPIPE)                                                                                                                                            \
    X(ERROR_HOST_DOWN, EHOSTUNREACH)                                                                                                                                               \
    X(ERROR_HOST_UNREACHABLE, EHOSTUNREACH)                                                                                                                                        \
    X(ERROR_INSUFFICIENT_BUFFER, EFAULT)                                                                                                                                           \
    X(ERROR_INVALID_ADDRESS, EADDRNOTAVAIL)                                                                                                                                        \
    X(ERROR_INVALID_FUNCTION, EINVAL)                                                                                                                                              \
    X(ERROR_INVALID_HANDLE, EBADF)                                                                                                                                                 \
    X(ERROR_INVALID_NETNAME, EADDRNOTAVAIL)                                                                                                                                        \
    X(ERROR_INVALID_PARAMETER, EINVAL)                                                                                                                                             \
    X(ERROR_INVALID_USER_BUFFER, EMSGSIZE)                                                                                                                                         \
    X(ERROR_IO_PENDING, EINPROGRESS)                                                                                                                                               \
    X(ERROR_LOCK_VIOLATION, EACCES)                                                                                                                                                \
    X(ERROR_MORE_DATA, EMSGSIZE)                                                                                                                                                   \
    X(ERROR_NETNAME_DELETED, ECONNABORTED)                                                                                                                                         \
    X(ERROR_NETWORK_ACCESS_DENIED, EACCES)                                                                                                                                         \
    X(ERROR_NETWORK_BUSY, ENETDOWN)                                                                                                                                                \
    X(ERROR_NETWORK_UNREACHABLE, ENETUNREACH)                                                                                                                                      \
    X(ERROR_NOACCESS, EFAULT)                                                                                                                                                      \
    X(ERROR_NONPAGED_SYSTEM_RESOURCES, ENOMEM)                                                                                                                                     \
    X(ERROR_NOT_ENOUGH_MEMORY, ENOMEM)                                                                                                                                             \
    X(ERROR_NOT_ENOUGH_QUOTA, ENOMEM)                                                                                                                                              \
    X(ERROR_NOT_FOUND, ENOENT)                                                                                                                                                     \
    X(ERROR_NOT_LOCKED, EACCES)                                                                                                                                                    \
    X(ERROR_NOT_READY, EACCES)                                                                                                                                                     \
    X(ERROR_NOT_SAME_DEVICE, EXDEV)                                                                                                                                                \
    X(ERROR_NOT_SUPPORTED, ENOTSUP)                                                                                                                                                \
    X(ERROR_NO_MORE_FILES, ENOENT)                                                                                                                                                 \
    X(ERROR_NO_SYSTEM_RESOURCES, ENOMEM)                                                                                                                                           \
    X(ERROR_OPERATION_ABORTED, EINTR)                                                                                                                                              \
    X(ERROR_OUT_OF_PAPER, EACCES)                                                                                                                                                  \
    X(ERROR_PAGED_SYSTEM_RESOURCES, ENOMEM)                                                                                                                                        \
    X(ERROR_PAGEFILE_QUOTA, ENOMEM)                                                                                                                                                \
    X(ERROR_PATH_NOT_FOUND, ENOENT)                                                                                                                                                \
    X(ERROR_PIPE_NOT_CONNECTED, EPIPE)                                                                                                                                             \
    X(ERROR_PORT_UNREACHABLE, ECONNRESET)                                                                                                                                          \
    X(ERROR_PROTOCOL_UNREACHABLE, ENETUNREACH)                                                                                                                                     \
    X(ERROR_REM_NOT_LIST, ECONNREFUSED)                                                                                                                                            \
    X(ERROR_REQUEST_ABORTED, EINTR)                                                                                                                                                \
    X(ERROR_REQ_NOT_ACCEP, EWOULDBLOCK)                                                                                                                                            \
    X(ERROR_SECTOR_NOT_FOUND, EACCES)                                                                                                                                              \
    X(ERROR_SEM_TIMEOUT, ETIMEDOUT)                                                                                                                                                \
    X(ERROR_SHARING_VIOLATION, EACCES)                                                                                                                                             \
    X(ERROR_TOO_MANY_NAMES, ENOMEM)                                                                                                                                                \
    X(ERROR_TOO_MANY_OPEN_FILES, EMFILE)                                                                                                                                           \
    X(ERROR_UNEXP_NET_ERR, ECONNABORTED)                                                                                                                                           \
    X(ERROR_WAIT_NO_CHILDREN, ECHILD)                                                                                                                                              \
    X(ERROR_WORKING_SET_QUOTA, ENOMEM)                                                                                                                                             \
    X(ERROR_WRITE_PROTECT, EACCES)                                                                                                                                                 \
    X(ERROR_WRONG_DISK, EACCES)                                                                                                                                                    \
    X(WSAEACCES, EACCES)                                                                                                                                                           \
    X(WSAEADDRINUSE, EADDRINUSE)                                                                                                                                                   \
    X(WSAEADDRNOTAVAIL, EADDRNOTAVAIL)                                                                                                                                             \
    X(WSAEAFNOSUPPORT, EAFNOSUPPORT)                                                                                                                                               \
    X(WSAECONNABORTED, ECONNABORTED)                                                                                                                                               \
    X(WSAECONNREFUSED, ECONNREFUSED)                                                                                                                                               \
    X(WSAECONNRESET, ECONNRESET)                                                                                                                                                   \
    X(WSAEDISCON, EPIPE)                                                                                                                                                           \
    X(WSAEFAULT, EFAULT)                                                                                                                                                           \
    X(WSAEHOSTDOWN, EHOSTUNREACH)                                                                                                                                                  \
    X(WSAEHOSTUNREACH, EHOSTUNREACH)                                                                                                                                               \
    X(WSAEINPROGRESS, EBUSY)                                                                                                                                                       \
    X(WSAEINTR, EINTR)                                                                                                                                                             \
    X(WSAEINVAL, EINVAL)                                                                                                                                                           \
    X(WSAEISCONN, EISCONN)                                                                                                                                                         \
    X(WSAEMSGSIZE, EMSGSIZE)                                                                                                                                                       \
    X(WSAENETDOWN, ENETDOWN)                                                                                                                                                       \
    X(WSAENETRESET, EHOSTUNREACH)                                                                                                                                                  \
    X(WSAENETUNREACH, ENETUNREACH)                                                                                                                                                 \
    X(WSAENOBUFS, ENOMEM)                                                                                                                                                          \
    X(WSAENOTCONN, ENOTCONN)                                                                                                                                                       \
    X(WSAENOTSOCK, ENOTSOCK)                                                                                                                                                       \
    X(WSAEOPNOTSUPP, EOPNOTSUPP)                                                                                                                                                   \
    X(WSAEPROCLIM, ENOMEM)                                                                                                                                                         \
    X(WSAESHUTDOWN, EPIPE)                                                                                                                                                         \
    X(WSAETIMEDOUT, ETIMEDOUT)                                                                                                                                                     \
    X(WSAEWOULDBLOCK, EWOULDBLOCK)                                                                                                                                                 \
    X(WSANOTINITIALISED, ENETDOWN)                                                                                                                                                 \
    X(WSASYSNOTREADY, ENETDOWN)                                                                                                                                                    \
    X(WSAVERNOTSUPPORTED, ENOSYS)

    /** @brief 把 Win32 错误码映射成 errno */
    static errno_t errorMapWindowsErrorToErrno(DWORD error)
    {
        switch (error)
        {
#define X(errorSymbol, errnoSymbol)                                                                                                                                                \
    case errorSymbol:                                                                                                                                                              \
        return errnoSymbol;
            ERR__ERRNO_MAPPINGS(X)
#undef X
        }
        return EINVAL;
    }

    void errorMapWindowsError()
    {
        errno = errorMapWindowsErrorToErrno(GetLastError());
    }

    void errorSetWindowsError(DWORD error)
    {
        SetLastError(error);
        errno = errorMapWindowsErrorToErrno(error);
    }

    int errorCheckHandle(HANDLE handle)
    {
        DWORD flags;

        // GetHandleInformation() 对 INVALID_HANDLE_VALUE 也会返回成功，因此要单独
        // 判掉这种情况。
        if (handle == INVALID_HANDLE_VALUE)
            return failWithWindowsError(-1, ERROR_INVALID_HANDLE);

        if (!GetHandleInformation(handle, &flags))
            return failWithLastWindowsError(-1);

        return 0;
    }

    // 从成员地址反推宿主对象地址。保留成宏是因为 offsetof 要的是成员名这个 token，函数拿不到；
    // 改成成员指针的写法得借「拿空对象取成员地址」那个形式上有 UB 的惯用法，为一行宏换来
    // UBSan 风险不划算。参数依次是：成员地址、宿主类型、成员名。
#define CONTAINER_OF(pointer, hostType, memberName) ((hostType *) ((uintptr_t) (pointer) - offsetof(hostType, memberName)))

    /** @brief 初始化 WinSock（只做一次） */
    WEPOLL_INTERNAL int wsGlobalInit();

    /** @brief 取套接字最底层的句柄，供 AFD 轮询使用 */
    WEPOLL_INTERNAL SOCKET wsGetBaseSocket(SOCKET socket);

    static bool      initializationDone = false;
    static INIT_ONCE onceControl        = INIT_ONCE_STATIC_INIT;

    /** @brief INIT_ONCE 的一次性回调：按顺序完成三块全局初始化 */
    static BOOL CALLBACK onceCallback([[maybe_unused]] INIT_ONCE *once, [[maybe_unused]] void *parameter, [[maybe_unused]] void **context)
    {

        // 注意：这里的初始化顺序不能颠倒。
        if (wsGlobalInit() < 0 || ntGlobalInit() < 0 || epollGlobalInit() < 0)
            return FALSE;

        initializationDone = true;
        return TRUE;
    }

    int ensureInitialized()
    {
        if (!initializationDone && !InitOnceExecuteOnce(&onceControl, onceCallback, nullptr, nullptr))
            // `InitOnceExecuteOnce()` 本身不会失败，回调返回 FALSE 时它也不设置错误码；
            // 这里返回 -1 表示全局初始化失败，errno 与 SetLastError() 由那个失败的初始化
            // 函数负责设置。
            return -1;

        return 0;
    }

    // 绕开一个两难：FARPROC 直接转成函数指针，GCC 8 会报警告甚至报错；先转 void*
    // 再转，MSVC 又不接受。要让两种编译器都干净编过，就用下面的「桥」类型中转：
    // MY_FUNC func = (MY_FUNC) (NtFunctionPointerCast) addr;
#ifdef __GNUC__
    using NtFunctionPointerCast = void *;
#else
    using NtFunctionPointerCast = FARPROC;
#endif

    int ntGlobalInit()
    {
        HMODULE ntdll;
        FARPROC functionPointer;

        ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
            return -1;

#define X(returnType, attributes, name, parameters)                                                                                                                                \
    functionPointer = GetProcAddress(ntdll, #name);                                                                                                                                \
    if (functionPointer == nullptr)                                                                                                                                                \
        return -1;                                                                                                                                                                 \
    name = (returnType(attributes *) parameters)(NtFunctionPointerCast) functionPointer;
        NT_NTDLL_IMPORT_LIST(X)
#undef X

        return 0;
    }

    struct PollGroup;

    struct QueueNode;

    /** @brief 组还有容量就复用并取一份引用，否则新建一组 */
    WEPOLL_INTERNAL PollGroup *pollGroupAcquire(PortState *port);

    /** @brief 归还一份引用，用量归零后把组移到队尾 */
    WEPOLL_INTERNAL void pollGroupRelease(PollGroup *pollGroup);

    /** @brief 删除轮询组（只在端口关闭时调用） */
    WEPOLL_INTERNAL void pollGroupDelete(PollGroup *pollGroup);

    /** @brief 从队列节点取回所属的轮询组 */
    WEPOLL_INTERNAL PollGroup *pollGroupFromQueueNode(QueueNode *queueNode);

    WEPOLL_INTERNAL HANDLE
    /** @brief 取轮询组复用的 AFD 辅助句柄 */
    pollGroupGetAfdHelperHandle(PollGroup *pollGroup);

    /** @brief 侵入式双向链表节点：内联在持有者里，不进堆 */
    struct QueueNode
    {
        QueueNode *m_previous;
        QueueNode *m_next;
    };

    /**
     * @brief 带哨兵头的侵入式双向链表：节点内联在持有者里（不进堆），因此「从节点摘除」是 O(1)、
     * @details 队列本身一次分配都不用做。本文件的两处用法都吃这个代价——每轮 epoll_wait 之后有成百上千 个 socket 要在这几张表之间挪动。改用 std::list
     * 会让每个节点多一次分配，还得把迭代器塞回 节点才能保住同样的摘除代价，得不偿失
     */
    struct Queue
    {
        QueueNode m_head; ///< 哨兵：m_head.m_next 是首元素、m_head.m_previous 是尾元素；空表时两者都指回 m_head
    };

    /** @brief 初始化带哨兵头的空队列 */
    WEPOLL_INTERNAL void queueInit(Queue *queue);

    /** @brief 把节点初始化成「不在任何表里」 */
    WEPOLL_INTERNAL void queueNodeInit(QueueNode *node);

    /** @brief 取队首节点，空表返回空指针 */
    WEPOLL_INTERNAL QueueNode *queueFirst(const Queue *queue);

    /** @brief 取队尾节点 */
    WEPOLL_INTERNAL QueueNode *queueLast(const Queue *queue);

    /** @brief 把节点追加到队尾 */
    WEPOLL_INTERNAL void queueAppend(Queue *queue, QueueNode *node);

    /** @brief 把节点移到队首 */
    WEPOLL_INTERNAL void queueMoveToStart(Queue *queue, QueueNode *node);

    /** @brief 把节点移到队尾 */
    WEPOLL_INTERNAL void queueMoveToEnd(Queue *queue, QueueNode *node);

    /** @brief 把节点从它所在的表里摘掉 */
    WEPOLL_INTERNAL void queueRemove(QueueNode *node);

    /** @brief 队列是否为空 */
    WEPOLL_INTERNAL bool queueIsEmpty(const Queue *queue);

    /** @brief 节点是否在某个表里（自指即不在） */
    WEPOLL_INTERNAL bool queueIsEnqueued(const QueueNode *node);

    static constexpr std::size_t kMaximumPollGroupSize = 32;

    /** @brief 共享同一个 AFD 辅助句柄的一组套接字 */
    struct PollGroup
    {
        PortState *m_portState;
        QueueNode  m_queueNode;
        HANDLE     m_afdHelperHandle;
        size_t     m_groupSize;
    };

    /** @brief 新建轮询组并挂到端口的轮询组队列尾 */
    static PollGroup *pollGroupNew(PortState *portState)
    {
        HANDLE iocpHandle     = portGetIocpHandle(portState);
        Queue *pollGroupQueue = portGetPollGroupQueue(portState);

        // nothrow 版本：失败返回空指针，与「失败即返回 NULL + 设 LastError」的约定一致。
        // 值初始化把各成员清零，队列节点仍要显式初始化成自指的哨兵
        PollGroup *pollGroup = new(std::nothrow) PollGroup{};
        if (pollGroup == nullptr)
            return failWithWindowsError(nullptr, ERROR_NOT_ENOUGH_MEMORY);

        queueNodeInit(&pollGroup->m_queueNode);
        pollGroup->m_portState = portState;

        if (afdCreateHelperHandle(iocpHandle, &pollGroup->m_afdHelperHandle) < 0)
        {
            delete pollGroup;
            return nullptr;
        }

        queueAppend(pollGroupQueue, &pollGroup->m_queueNode);

        return pollGroup;
    }

    void pollGroupDelete(PollGroup *pollGroup)
    {
        assert(pollGroup->m_groupSize == 0);
        CloseHandle(pollGroup->m_afdHelperHandle);
        queueRemove(&pollGroup->m_queueNode);
        delete pollGroup;
    }

    PollGroup *pollGroupFromQueueNode(QueueNode *queueNode)
    {
        return CONTAINER_OF(queueNode, PollGroup, m_queueNode);
    }

    HANDLE pollGroupGetAfdHelperHandle(PollGroup *pollGroup)
    {
        return pollGroup->m_afdHelperHandle;
    }

    PollGroup *pollGroupAcquire(PortState *portState)
    {
        Queue *    pollGroupQueue = portGetPollGroupQueue(portState);
        PollGroup *pollGroup      = !queueIsEmpty(pollGroupQueue) ? CONTAINER_OF(queueLast(pollGroupQueue), PollGroup, m_queueNode) : nullptr;

        if (pollGroup == nullptr || pollGroup->m_groupSize >= kMaximumPollGroupSize)
            pollGroup = pollGroupNew(portState);
        if (pollGroup == nullptr)
            return nullptr;

        if (++pollGroup->m_groupSize == kMaximumPollGroupSize)
            queueMoveToStart(pollGroupQueue, &pollGroup->m_queueNode);

        return pollGroup;
    }

    void pollGroupRelease(PollGroup *pollGroup)
    {
        PortState *portState      = pollGroup->m_portState;
        Queue *    pollGroupQueue = portGetPollGroupQueue(portState);

        pollGroup->m_groupSize--;
        assert(pollGroup->m_groupSize < kMaximumPollGroupSize);

        queueMoveToEnd(pollGroupQueue, &pollGroup->m_queueNode);

        // 轮询组目前只在 epoll 端口关闭时才释放。
    }

    /** @brief 新建套接字状态并登记进端口的套接字表 */
    WEPOLL_INTERNAL SockState *sockNew(PortState *portState, SOCKET socket);

    /** @brief 删除套接字状态，等在途轮询收尾后再释放 */
    WEPOLL_INTERNAL void sockDelete(PortState *portState, SockState *sockState);

    /** @brief 强制删除套接字状态，不等在途轮询 */
    WEPOLL_INTERNAL void sockForceDelete(PortState *portState, SockState *sockState);

    /** @brief 记下用户关注的事件，必要时请求重新下发 */
    WEPOLL_INTERNAL int sockSetEvent(PortState *portState, SockState *sockState, const struct epoll_event *ev);

    /** @brief 把一个套接字最新的关注事件下发给 AFD */
    WEPOLL_INTERNAL int sockUpdate(PortState *portState, SockState *sockState);

    /** @brief 处理一条套接字完成包：翻译成 epoll 事件并回填用户数据 */
    WEPOLL_INTERNAL int sockFeedEvent(PortState *portState, IO_STATUS_BLOCK *ioStatusBlock, struct epoll_event *ev);

    /** @brief 从队列节点取回所属的套接字状态 */
    WEPOLL_INTERNAL SockState *sockStateFromQueueNode(QueueNode *queueNode);

    /** @brief 取套接字状态里的队列节点 */
    WEPOLL_INTERNAL QueueNode *sockStateToQueueNode(SockState *sockState);

    /** @brief 从树节点取回所属的套接字状态 */
    WEPOLL_INTERNAL SockState *sockStateFromTreeNode(TreeNode *treeNode);

    /** @brief 取套接字状态里的树节点 */
    WEPOLL_INTERNAL TreeNode *sockStateToTreeNode(SockState *sockState);

    /// 完成包列表放在栈上时的上限：超过它才值得上堆
    static constexpr std::size_t kMaximumOnStackCompletions = 256;

    /** @brief 一个 epoll 实例的全部状态 */
    struct PortState
    {
        HANDLE           m_iocpHandle;
        Tree             m_sockTree;
        Queue            m_sockUpdateQueue;
        Queue            m_sockDeletedQueue;
        Queue            m_pollGroupQueue;
        TsTreeNode       m_handleTreeNode;
        CRITICAL_SECTION m_lock;
        size_t           m_activePollCount;
    };

    /** @brief 分配并值初始化端口状态 */
    static PortState *portAlloc()
    {
        // 值初始化：平凡成员清零，sockTree 与引用锁各自构造好——不再需要 memset 之后再补构造
        auto *portState = new(std::nothrow) PortState{};
        if (portState == nullptr)
            return failWithWindowsError(nullptr, ERROR_NOT_ENOUGH_MEMORY);

        return portState;
    }

    /** @brief 释放端口状态 */
    static void portFree(PortState *port)
    {
        assert(port != nullptr);
        // 与 new 配对：容器成员由析构自动收尾
        delete port;
    }

    /** @brief 创建 IOCP 完成端口 */
    static HANDLE portCreateIocp()
    {
        HANDLE iocpHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (iocpHandle == nullptr)
            return failWithLastWindowsError(nullptr);

        return iocpHandle;
    }

    PortState *portNew(HANDLE *iocpHandleOut)
    {
        PortState *portState;
        HANDLE     iocpHandle;

        portState = portAlloc();
        if (portState == nullptr)
            return nullptr;

        iocpHandle = portCreateIocp();
        if (iocpHandle == nullptr)
        {
            portFree(portState);
            return nullptr;
        }

        portState->m_iocpHandle = iocpHandle;
        queueInit(&portState->m_sockUpdateQueue);
        queueInit(&portState->m_sockDeletedQueue);
        queueInit(&portState->m_pollGroupQueue);
        tsTreeNodeInit(&portState->m_handleTreeNode);
        InitializeCriticalSection(&portState->m_lock);

        *iocpHandleOut = iocpHandle;
        return portState;
    }

    /** @brief 关掉底层的 IOCP 句柄 */
    static int portCloseIocp(PortState *portState)
    {
        HANDLE iocpHandle       = portState->m_iocpHandle;
        portState->m_iocpHandle = nullptr;

        if (!CloseHandle(iocpHandle))
            return failWithLastWindowsError(-1);

        return 0;
    }

    int portClose(PortState *portState)
    {
        int result;

        EnterCriticalSection(&portState->m_lock);
        result = portCloseIocp(portState);
        LeaveCriticalSection(&portState->m_lock);

        return result;
    }

    int portDelete(PortState *portState)
    {
        TreeNode * treeNode;
        QueueNode *queueNode;

        // 走到这里时 IOCP 端口应当已经关闭。
        assert(portState->m_iocpHandle == nullptr);

        while ((treeNode = treeRoot(&portState->m_sockTree)) != nullptr)
        {
            SockState *sockState = sockStateFromTreeNode(treeNode);
            sockForceDelete(portState, sockState);
        }

        while ((queueNode = queueFirst(&portState->m_sockDeletedQueue)) != nullptr)
        {
            SockState *sockState = sockStateFromQueueNode(queueNode);
            sockForceDelete(portState, sockState);
        }

        while ((queueNode = queueFirst(&portState->m_pollGroupQueue)) != nullptr)
        {
            PollGroup *pollGroup = pollGroupFromQueueNode(queueNode);
            pollGroupDelete(pollGroup);
        }

        assert(queueIsEmpty(&portState->m_sockUpdateQueue));

        DeleteCriticalSection(&portState->m_lock);

        portFree(portState);

        return 0;
    }

    /** @brief 把所有待更新的套接字事件下发给 AFD */
    static int portUpdateEvents(PortState *portState)
    {
        Queue *sockUpdateQueue = &portState->m_sockUpdateQueue;

        // 遍历队列，为每个需要它的套接字下新的轮询请求。
        while (!queueIsEmpty(sockUpdateQueue))
        {
            QueueNode *queueNode = queueFirst(sockUpdateQueue);
            SockState *sockState = sockStateFromQueueNode(queueNode);

            if (sockUpdate(portState, sockState) < 0)
                return -1;

            // sockUpdate() 会把套接字从更新队列里摘掉。
        }

        return 0;
    }

    /** @brief 仅在端口处于轮询状态时下发待更新事件 */
    static void portUpdateEventsIfPolling(PortState *portState)
    {
        if (portState->m_activePollCount > 0)
            portUpdateEvents(portState);
    }

    /** @brief 把一批 IOCP 完成包翻译成 epoll 事件 */
    static int portFeedEvents(PortState *portState, struct epoll_event *epollEvents, OVERLAPPED_ENTRY *iocpEvents, DWORD iocpEventCount)
    {
        int epollEventCount = 0;

        for (DWORD i = 0; i < iocpEventCount; i++)
        {
            const auto          ioStatusBlock = (IO_STATUS_BLOCK *) iocpEvents[i].lpOverlapped;
            struct epoll_event *ev            = &epollEvents[epollEventCount];

            epollEventCount += sockFeedEvent(portState, ioStatusBlock, ev);
        }

        return epollEventCount;
    }

    /** @brief 从 IOCP 取一批完成包并逐个翻译成事件 */
    static int portPoll(PortState *portState, struct epoll_event *epollEvents, OVERLAPPED_ENTRY *iocpEvents, DWORD maxevents, DWORD timeout)
    {
        DWORD completionCount;

        if (portUpdateEvents(portState) < 0)
            return -1;

        portState->m_activePollCount++;

        LeaveCriticalSection(&portState->m_lock);

        BOOL r = GetQueuedCompletionStatusEx(portState->m_iocpHandle, iocpEvents, maxevents, &completionCount, timeout, FALSE);

        EnterCriticalSection(&portState->m_lock);

        portState->m_activePollCount--;

        if (!r)
            return failWithLastWindowsError(-1);

        return portFeedEvents(portState, epollEvents, iocpEvents, completionCount);
    }

    int portWait(PortState *portState, struct epoll_event *events, int maxevents, int timeout)
    {
        OVERLAPPED_ENTRY                    stackIocpEvents[kMaximumOnStackCompletions];
        OVERLAPPED_ENTRY *                  iocpEvents = stackIocpEvents;
        // 只有 maxevents 很大时才上堆：改用 unique_ptr 后所有返回路径都自动归还，不必再手工 free
        std::unique_ptr<OVERLAPPED_ENTRY[]> heapIocpEvents;
        uint64_t                            due = 0;
        DWORD                               completionTimeout;
        int                                 result;

        if (maxevents <= 0)
            return failWithWindowsError(-1, ERROR_INVALID_PARAMETER);

        // 决定完成包列表放栈上还是堆上。堆分配故意用 nothrow：失败就退回栈数组，
        // 而不是让整个 wait 失败。
        if (static_cast<std::size_t>(maxevents) > std::size(stackIocpEvents))
        {
            heapIocpEvents.reset(new(std::nothrow) OVERLAPPED_ENTRY[static_cast<std::size_t>(maxevents)]);

            if (heapIocpEvents != nullptr)
                iocpEvents = heapIocpEvents.get();
            else
                maxevents = static_cast<int>(std::size(stackIocpEvents));
        }

        // 用户给出的超时既不是 0 也不是无限时，算出本次 GetQueuedCompletionStatus
        // 的超时值与等待截止时刻。
        if (timeout > 0)
        {
            due               = GetTickCount64() + static_cast<std::uint64_t>(timeout);
            completionTimeout = static_cast<DWORD>(timeout);
        } else if (timeout == 0)
        {
            completionTimeout = 0;
        } else
        {
            completionTimeout = INFINITE;
        }

        EnterCriticalSection(&portState->m_lock);

        // 反复取出完成包，直到发现至少一个用户关心的事件或超时为止。
        for (;;)
        {
            uint64_t now;

            result = portPoll(portState, events, iocpEvents, static_cast<DWORD>(maxevents), completionTimeout);
            if (result < 0 || result > 0)
                break; // 有结果、出错或超时

            if (timeout < 0)
                continue; // 超时为负表示无限等待，不检查截止时刻

            now = GetTickCount64();

            // 截止时刻不允许落在过去。
            if (now >= due)
            {
                SetLastError(WAIT_TIMEOUT);
                break;
            }

            // 重算传给 GetQueuedCompletionStatus 的剩余超时。
            completionTimeout = static_cast<DWORD>(due - now);
        }

        portUpdateEventsIfPolling(portState);

        LeaveCriticalSection(&portState->m_lock);

        if (result >= 0)
            return result;
        else if (GetLastError() == WAIT_TIMEOUT)
            return 0;
        else
            return -1;
    }

    /** @brief 处理 EPOLL_CTL_ADD：登记套接字并装配首次轮询 */
    static int portCtlAdd(PortState *portState, SOCKET sock, struct epoll_event *ev)
    {
        SockState *sockState = sockNew(portState, sock);
        if (sockState == nullptr)
            return -1;

        if (sockSetEvent(portState, sockState, ev) < 0)
        {
            sockDelete(portState, sockState);
            return -1;
        }

        portUpdateEventsIfPolling(portState);

        return 0;
    }

    /** @brief 处理 EPOLL_CTL_MOD：更新关注事件并按需重新装配 */
    static int portCtlMod(PortState *portState, SOCKET sock, struct epoll_event *ev)
    {
        SockState *sockState = portFindSocket(portState, sock);
        if (sockState == nullptr)
            return -1;

        if (sockSetEvent(portState, sockState, ev) < 0)
            return -1;

        portUpdateEventsIfPolling(portState);

        return 0;
    }

    /** @brief 处理 EPOLL_CTL_DEL：注销套接字 */
    static int portCtlDel(PortState *portState, SOCKET sock)
    {
        SockState *sockState = portFindSocket(portState, sock);
        if (sockState == nullptr)
            return -1;

        sockDelete(portState, sockState);

        return 0;
    }

    /** @brief 按操作码把请求分派到增删改三个实现 */
    static int portCtlOp(PortState *portState, int op, SOCKET sock, struct epoll_event *ev)
    {
        switch (op)
        {
            case EPOLL_CTL_ADD:
                return portCtlAdd(portState, sock, ev);
            case EPOLL_CTL_MOD:
                return portCtlMod(portState, sock, ev);
            case EPOLL_CTL_DEL:
                return portCtlDel(portState, sock);
            default:
                return failWithWindowsError(-1, ERROR_INVALID_PARAMETER);
        }
    }

    int portCtl(PortState *portState, int op, SOCKET sock, struct epoll_event *ev)
    {
        int result;

        EnterCriticalSection(&portState->m_lock);
        result = portCtlOp(portState, op, sock, ev);
        LeaveCriticalSection(&portState->m_lock);

        return result;
    }

    int portRegisterSocket(PortState *portState, SockState *sockState, SOCKET socket)
    {
        if (treeAdd(&portState->m_sockTree, sockStateToTreeNode(sockState), socket) < 0)
            return failWithWindowsError(-1, ERROR_ALREADY_EXISTS);
        return 0;
    }

    void portUnregisterSocket(PortState *portState, SockState *sockState)
    {
        treeDel(&portState->m_sockTree, sockStateToTreeNode(sockState));
    }

    SockState *portFindSocket(PortState *portState, SOCKET socket)
    {
        TreeNode *treeNode = treeFind(&portState->m_sockTree, socket);
        if (treeNode == nullptr)
            return failWithWindowsError(nullptr, ERROR_NOT_FOUND);
        return sockStateFromTreeNode(treeNode);
    }

    void portRequestSocketUpdate(PortState *portState, SockState *sockState)
    {
        if (queueIsEnqueued(sockStateToQueueNode(sockState)))
            return;
        queueAppend(&portState->m_sockUpdateQueue, sockStateToQueueNode(sockState));
    }

    void portCancelSocketUpdate(PortState *portState, SockState *sockState)
    {
        if (!queueIsEnqueued(sockStateToQueueNode(sockState)))
            return;
        queueRemove(sockStateToQueueNode(sockState));
    }

    void portAddDeletedSocket(PortState *portState, SockState *sockState)
    {
        if (queueIsEnqueued(sockStateToQueueNode(sockState)))
            return;
        queueAppend(&portState->m_sockDeletedQueue, sockStateToQueueNode(sockState));
    }

    void portRemoveDeletedSocket([[maybe_unused]] PortState *portState, SockState *sockState)
    {
        if (!queueIsEnqueued(sockStateToQueueNode(sockState)))
            return;
        queueRemove(sockStateToQueueNode(sockState));
    }

    HANDLE portGetIocpHandle(PortState *portState)
    {
        assert(portState->m_iocpHandle != nullptr);
        return portState->m_iocpHandle;
    }

    Queue *portGetPollGroupQueue(PortState *portState)
    {
        return &portState->m_pollGroupQueue;
    }

    PortState *portStateFromHandleTreeNode(TsTreeNode *treeNode)
    {
        return CONTAINER_OF(treeNode, PortState, m_handleTreeNode);
    }

    TsTreeNode *portStateToHandleTreeNode(PortState *portState)
    {
        return &portState->m_handleTreeNode;
    }

    void queueInit(Queue *queue)
    {
        queueNodeInit(&queue->m_head);
    }

    void queueNodeInit(QueueNode *node)
    {
        // 自指即「不在任何表里」：入队与摘除都靠这条不变式判断，因此不需要额外的标记位
        node->m_previous = node;
        node->m_next     = node;
    }

    /**
     * @brief 把节点从它当前所在的表里摘下来，不动它自己的 previous/next 指向
     * @details queueRemove 与两个 queueMove 共用，改动这里要同时顾到「被摘的可能是首元素或尾元素」
     */
    static inline void queueDetachNode(QueueNode *node)
    {
        node->m_previous->m_next = node->m_next;
        node->m_next->m_previous = node->m_previous;
    }

    QueueNode *queueFirst(const Queue *queue)
    {
        return !queueIsEmpty(queue) ? queue->m_head.m_next : nullptr;
    }

    QueueNode *queueLast(const Queue *queue)
    {
        return !queueIsEmpty(queue) ? queue->m_head.m_previous : nullptr;
    }

    void queueAppend(Queue *queue, QueueNode *node)
    {
        node->m_next             = &queue->m_head;
        node->m_previous         = queue->m_head.m_previous;
        node->m_previous->m_next = node;
        queue->m_head.m_previous = node;
    }

    void queueMoveToStart(Queue *queue, QueueNode *node)
    {
        queueDetachNode(node);

        // 插到哨兵之后即队首（这里不再单列一个 prepend：只有这一处用它）
        node->m_next             = queue->m_head.m_next;
        node->m_previous         = &queue->m_head;
        node->m_next->m_previous = node;
        queue->m_head.m_next     = node;
    }

    void queueMoveToEnd(Queue *queue, QueueNode *node)
    {
        queueDetachNode(node);
        queueAppend(queue, node);
    }

    void queueRemove(QueueNode *node)
    {
        queueDetachNode(node);
        queueNodeInit(node);
    }

    bool queueIsEmpty(const Queue *queue)
    {
        return !queueIsEnqueued(&queue->m_head);
    }

    bool queueIsEnqueued(const QueueNode *node)
    {
        return node->m_previous != node;
    }


    static constexpr std::uint32_t kKnownEpollEvents =
            EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDNORM | EPOLLRDBAND | EPOLLWRNORM | EPOLLWRBAND | EPOLLMSG | EPOLLRDHUP;

    enum class SockPollStatus
    {
        Idle = 0, ///< 尚未下发轮询请求
        Pending,  ///< 已下发 AFD 轮询，等待完成
        Cancelled ///< 已请求撤销，等待回收
    };

    /** @brief 一个已注册套接字的状态 */
    struct SockState
    {
        IO_STATUS_BLOCK m_ioStatusBlock;
        AFD_POLL_INFO   m_pollInfo;
        QueueNode       m_queueNode;
        TreeNode        m_treeNode;
        PollGroup *     m_pollGroup;
        SOCKET          m_baseSocket;
        epoll_data_t    m_userData;
        uint32_t        m_userEvents;
        uint32_t        m_pendingEvents;
        SockPollStatus  m_pollStatus;
        bool            m_deletePending;
    };

    /** @brief 分配并值初始化套接字状态 */
    static inline SockState *sockAlloc()
    {
        SockState *sockState = new(std::nothrow) SockState{};
        if (sockState == nullptr)
            return failWithWindowsError(nullptr, ERROR_NOT_ENOUGH_MEMORY);
        return sockState;
    }

    /** @brief 释放套接字状态 */
    static inline void sockFree(SockState *sockState)
    {
        delete sockState;
    }

    /** @brief 撤销套接字上尚未完成的 AFD 轮询 */
    static int sockCancelPoll(SockState *sockState)
    {
        assert(sockState->m_pollStatus == SockPollStatus::Pending);

        if (afdCancelPoll(pollGroupGetAfdHelperHandle(sockState->m_pollGroup), &sockState->m_ioStatusBlock) < 0)
            return -1;

        sockState->m_pollStatus    = SockPollStatus::Cancelled;
        sockState->m_pendingEvents = 0;
        return 0;
    }

    SockState *sockNew(PortState *portState, SOCKET socket)
    {
        SOCKET     baseSocket;
        PollGroup *pollGroup;
        SockState *sockState;

        if (socket == 0 || socket == INVALID_SOCKET)
            return failWithWindowsError(nullptr, ERROR_INVALID_HANDLE);

        baseSocket = wsGetBaseSocket(socket);
        if (baseSocket == INVALID_SOCKET)
            return nullptr;

        pollGroup = pollGroupAcquire(portState);
        if (pollGroup == nullptr)
            return nullptr;

        sockState = sockAlloc();
        if (sockState == nullptr)
        {
            pollGroupRelease(pollGroup);
            return nullptr;
        }

        sockState->m_baseSocket = baseSocket;
        sockState->m_pollGroup  = pollGroup;

        treeNodeInit(&sockState->m_treeNode);
        queueNodeInit(&sockState->m_queueNode);

        if (portRegisterSocket(portState, sockState, socket) < 0)
        {
            sockFree(sockState);
            pollGroupRelease(pollGroup);
            return nullptr;
        }

        return sockState;
    }

    /** @brief 删除套接字状态的公共实现：按轮询状态决定立即还是延后释放 */
    static int sockDeleteInternal(PortState *portState, SockState *sockState, bool force)
    {
        if (!sockState->m_deletePending)
        {
            if (sockState->m_pollStatus == SockPollStatus::Pending)
                sockCancelPoll(sockState);

            portCancelSocketUpdate(portState, sockState);
            portUnregisterSocket(portState, sockState);

            sockState->m_deletePending = true;
        }

        // 轮询请求还没回来时，sockState 不能就地 free：交给 sockFeedEvent() 或
        // portClose() 稍后收尾。
        if (force || sockState->m_pollStatus == SockPollStatus::Idle)
        {
            portRemoveDeletedSocket(portState, sockState);
            pollGroupRelease(sockState->m_pollGroup);
            sockFree(sockState);
        } else
        {
            portAddDeletedSocket(portState, sockState);
        }

        return 0;
    }

    void sockDelete(PortState *portState, SockState *sockState)
    {
        sockDeleteInternal(portState, sockState, false);
    }

    void sockForceDelete(PortState *portState, SockState *sockState)
    {
        sockDeleteInternal(portState, sockState, true);
    }

    int sockSetEvent(PortState *portState, SockState *sockState, const struct epoll_event *ev)
    {
        // EPOLLERR 与 EPOLLHUP 始终上报，即使用户没有订阅；但带 EPOLLONESHOT 的
        // 套接字报过一次事件后，这两个也会被停掉。
        uint32_t events = ev->events | EPOLLERR | EPOLLHUP;

        sockState->m_userEvents = events;
        sockState->m_userData   = ev->data;

        if ((events & kKnownEpollEvents & ~sockState->m_pendingEvents) != 0)
            portRequestSocketUpdate(portState, sockState);

        return 0;
    }

    /** @brief 把 epoll 事件掩码翻译成 AFD 轮询位 */
    static inline DWORD sockEpollEventsToAfdEvents(uint32_t epollEvents)
    {
        // 始终监听 AFD_POLL_LOCAL_CLOSE：套接字被 closesocket() 或 CloseHandle()
        // 关掉时触发它。
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

    /** @brief 把 AFD 回报的事件位翻译回 epoll 事件 */
    static inline uint32_t sockAfdEventsToEpollEvents(DWORD afdEvents)
    {
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
            // connect() 失败后，Linux 会把这几个事件一起报上来。
            epollEvents |= EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLRDNORM | EPOLLWRNORM | EPOLLRDHUP;

        return epollEvents;
    }

    int sockUpdate(PortState *portState, SockState *sockState)
    {
        assert(!sockState->m_deletePending);

        if ((sockState->m_pollStatus == SockPollStatus::Pending) && (sockState->m_userEvents & kKnownEpollEvents & ~sockState->m_pendingEvents) == 0)
        {
            // 用户关心的事件都已由挂起的那次轮询覆盖。它可能因为一个已不再关心的事件
            // 提前完成，届时会按更新后的掩码重新下发。

        } else if (sockState->m_pollStatus == SockPollStatus::Pending)
        {
            // 已有轮询在挂起，但它没覆盖用户关心的全部事件：先撤销这次轮询，等它的
            // 完成包回来再按正确的掩码重新下发。
            if (sockCancelPoll(sockState) < 0)
                return -1;

        } else if (sockState->m_pollStatus == SockPollStatus::Cancelled)
        {
            // 轮询已被撤销，还在等它返回，此时无需做什么。

        } else if (sockState->m_pollStatus == SockPollStatus::Idle)
        {
            // 没有挂起的轮询，发起一次。
            sockState->m_pollInfo.Exclusive         = FALSE;
            sockState->m_pollInfo.NumberOfHandles   = 1;
            sockState->m_pollInfo.Timeout.QuadPart  = INT64_MAX;
            sockState->m_pollInfo.Handles[0].Handle = reinterpret_cast<HANDLE>(sockState->m_baseSocket);
            sockState->m_pollInfo.Handles[0].Status = 0;
            sockState->m_pollInfo.Handles[0].Events = sockEpollEventsToAfdEvents(sockState->m_userEvents);

            if (afdPoll(pollGroupGetAfdHelperHandle(sockState->m_pollGroup), &sockState->m_pollInfo, &sockState->m_ioStatusBlock) < 0)
            {
                switch (GetLastError())
                {
                    case ERROR_IO_PENDING:
                        // 重叠轮询正在进行，属预期情况。
                        break;
                    case ERROR_INVALID_HANDLE:
                        // 套接字已关闭，会从 epoll 集合里移除。
                        return sockDeleteInternal(portState, sockState, false);
                    default:
                        // 其它错误原样交给调用方。
                        return failWithLastWindowsError(-1);
                }
            }

            // 轮询请求已成功下发。
            sockState->m_pollStatus    = SockPollStatus::Pending;
            sockState->m_pendingEvents = sockState->m_userEvents;

        } else
        {
            // 不可达。
            assert(false);
        }

        portCancelSocketUpdate(portState, sockState);
        return 0;
    }

    int sockFeedEvent(PortState *portState, IO_STATUS_BLOCK *ioStatusBlock, struct epoll_event *ev)
    {
        SockState *    sockState   = CONTAINER_OF(ioStatusBlock, SockState, m_ioStatusBlock);
        AFD_POLL_INFO *pollInfo    = &sockState->m_pollInfo;
        uint32_t       epollEvents = 0;

        sockState->m_pollStatus    = SockPollStatus::Idle;
        sockState->m_pendingEvents = 0;

        if (sockState->m_deletePending)
        {
            // 套接字此前已被删除，现在可以释放了。
            return sockDeleteInternal(portState, sockState, false);

        } else if (ioStatusBlock->Status == STATUS_CANCELLED)
        {
            // 轮询请求被 CancelIoEx 撤销。

        } else if (!ntSuccess(ioStatusBlock->Status))
        {
            // 重叠请求本身以意外方式失败。
            epollEvents = EPOLLERR;

        } else if (pollInfo->NumberOfHandles < 1)
        {
            // 这次轮询成功返回，但没有报告任何套接字事件。

        } else if (pollInfo->Handles[0].Events & AFD_POLL_LOCAL_CLOSE)
        {
            // 轮询报告套接字已关闭。
            return sockDeleteInternal(portState, sockState, false);

        } else
        {
            // 上报了与本套接字相关的事件。
            epollEvents = sockAfdEventsToEpollEvents(pollInfo->Handles[0].Events);
        }

        // 重新入队，下一轮会为它下发新的轮询请求。
        portRequestSocketUpdate(portState, sockState);

        // 滤掉用户没有订阅的事件。
        epollEvents &= sockState->m_userEvents;

        // 没有可上报的 epoll 事件就直接返回。
        if (epollEvents == 0)
            return 0;

        // 带 EPOLLONESHOT 的套接字要把所有事件都停掉监听，EPOLLERR 与 EPOLLHUP
        // 也不例外；但「套接字已关闭」始终继续监听。
        if (sockState->m_userEvents & EPOLLONESHOT)
            sockState->m_userEvents = 0;

        ev->data   = sockState->m_userData;
        ev->events = epollEvents;
        return 1;
    }

    SockState *sockStateFromQueueNode(QueueNode *queueNode)
    {
        return CONTAINER_OF(queueNode, SockState, m_queueNode);
    }

    QueueNode *sockStateToQueueNode(SockState *sockState)
    {
        return &sockState->m_queueNode;
    }

    SockState *sockStateFromTreeNode(TreeNode *treeNode)
    {
        return CONTAINER_OF(treeNode, SockState, m_treeNode);
    }

    TreeNode *sockStateToTreeNode(SockState *sockState)
    {
        return &sockState->m_treeNode;
    }

    void tsTreeInit(TsTree *tsTree)
    {
        treeInit(&tsTree->m_tree);
        InitializeSRWLock(&tsTree->m_lock);
    }

    void tsTreeNodeInit(TsTreeNode *node)
    {
        treeNodeInit(&node->m_treeNode);
        node->m_reflock.reset();
    }

    int tsTreeAdd(TsTree *tsTree, TsTreeNode *node, uintptr_t key)
    {
        int r;

        AcquireSRWLockExclusive(&tsTree->m_lock);
        r = treeAdd(&tsTree->m_tree, &node->m_treeNode, key);
        ReleaseSRWLockExclusive(&tsTree->m_lock);

        return r;
    }

    /** @brief 加锁按键查节点 */
    static inline TsTreeNode *tsTreeFindNode(TsTree *tsTree, uintptr_t key)
    {
        TreeNode *treeNode = treeFind(&tsTree->m_tree, key);
        if (treeNode == nullptr)
            return nullptr;

        return CONTAINER_OF(treeNode, TsTreeNode, m_treeNode);
    }

    TsTreeNode *tsTreeDelAndRef(TsTree *tsTree, uintptr_t key)
    {
        TsTreeNode *tsTreeNode;

        AcquireSRWLockExclusive(&tsTree->m_lock);

        tsTreeNode = tsTreeFindNode(tsTree, key);
        if (tsTreeNode != nullptr)
        {
            treeDel(&tsTree->m_tree, &tsTreeNode->m_treeNode);
            tsTreeNode->m_reflock.ref();
        }

        ReleaseSRWLockExclusive(&tsTree->m_lock);

        return tsTreeNode;
    }

    TsTreeNode *tsTreeFindAndRef(TsTree *tsTree, uintptr_t key)
    {
        TsTreeNode *tsTreeNode;

        AcquireSRWLockShared(&tsTree->m_lock);

        tsTreeNode = tsTreeFindNode(tsTree, key);
        if (tsTreeNode != nullptr)
            tsTreeNode->m_reflock.ref();

        ReleaseSRWLockShared(&tsTree->m_lock);

        return tsTreeNode;
    }

    void tsTreeNodeUnref(TsTreeNode *node)
    {
        node->m_reflock.unref();
    }

    void tsTreeNodeUnrefAndDestroy(TsTreeNode *node)
    {
        node->m_reflock.unrefAndDestroy();
    }

    void treeInit(Tree *tree)
    {
        // 只用于生命周期已经开始的对象（静态的 epollHandleTree）；malloc 存储上的那份由
        // portNew 直接 construct_at，不能用这里的赋值。
        *tree = Tree{};
    }

    void treeNodeInit(TreeNode *node)
    {
        node->m_key = 0;
    }

    int treeAdd(Tree *tree, TreeNode *node, uintptr_t key)
    {
        node->m_key = key;

        // 同一个键重复插入是调用方的错误：返回 -1 让它按 EEXIST 报出去（与手写树同样的约定）
        return tree->emplace(key, node).second ? 0 : -1;
    }

    void treeDel(Tree *tree, TreeNode *node)
    {
        tree->erase(node->m_key);
    }

    TreeNode *treeFind(const Tree *tree, uintptr_t key)
    {
        const auto iterator = tree->find(key);
        return iterator == tree->end() ? nullptr : iterator->second;
    }

    TreeNode *treeRoot(const Tree *tree)
    {
        // 只给 portDelete 的「逐个摘除直到空」用：给最小键的那个节点即可，
        // 原实现给的是红黑树的根（同样是「随便一个」，摘除顺序本就不可依赖）
        return tree->empty() ? nullptr : tree->begin()->second;
    }

#ifndef SIO_BSP_HANDLE_POLL
#define SIO_BSP_HANDLE_POLL 0x4800001D
#endif

#ifndef SIO_BASE_HANDLE
#define SIO_BASE_HANDLE 0x48000022
#endif

    int wsGlobalInit()
    {
        int     r;
        WSADATA wsaData;

        r = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (r != 0)
            return failWithWindowsError(-1, static_cast<DWORD>(r));

        return 0;
    }

    /** @brief 沿协议链取一层 BSP 套接字，绕过破坏 SIO_BASE_HANDLE 的 LSP */
    static inline SOCKET wsIoctlGetBspSocket(SOCKET socket, DWORD ioctl)
    {
        SOCKET bspSocket;
        DWORD  bytes;

        if (WSAIoctl(socket, ioctl, nullptr, 0, &bspSocket, sizeof bspSocket, &bytes, nullptr, nullptr) != SOCKET_ERROR)
            return bspSocket;
        else
            return INVALID_SOCKET;
    }

    SOCKET wsGetBaseSocket(SOCKET socket)
    {
        SOCKET baseSocket;
        DWORD  error;

        for (;;)
        {
            baseSocket = wsIoctlGetBspSocket(socket, SIO_BASE_HANDLE);
            if (baseSocket != INVALID_SOCKET)
                return baseSocket;

            error = GetLastError();
            if (error == WSAENOTSOCK)
                return failWithWindowsError(INVALID_SOCKET, error);

            // 微软文档明确要求 LSP 不得拦截 `SIO_BASE_HANDLE` ioctl [1]，但基于
            // Komodia 的 LSP 照拦不误并把它弄坏，目的显然是防止绕过 LSP [2]。所幸它们
            // 没处理 `SIO_BSP_HANDLE_POLL`，我们借此拿到协议链下一层对应的套接字；拿到就
            // 回到循环再问一次 `SIO_BASE_HANDLE`，直到确认已经走到最底层。
            // [1] https://docs.microsoft.com/en-us/windows/win32/winsock/winsock-ioctls
            // [2] https://www.komodia.com/newwiki/index.php?title=Komodia%27s_Redirector_bug_fixes#Version_2.2.2.6

            baseSocket = wsIoctlGetBspSocket(socket, SIO_BSP_HANDLE_POLL);
            if (baseSocket != INVALID_SOCKET && baseSocket != socket)
                socket = baseSocket;
            else
                return failWithWindowsError(INVALID_SOCKET, error);
        }
    }

} // namespace AsynGyanis::Core
