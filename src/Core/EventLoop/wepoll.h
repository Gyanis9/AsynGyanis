/**
 * @file wepoll.h
 * @brief Windows 上 epoll 兼容层的对外接口（vendored wepoll，实现见同目录 Wepoll.cpp）
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 *
 * @details 接口按 ABI 对齐 Linux：类型名、常量名与入口点签名一律不动（含参数名），Linux 与
 *          Windows 因此共用同一份调用代码（见 Epoll.cpp）。声明刻意留在全局作用域——塞进命名空间
 *          就等于改 C 链接；EPOLL_* 这类常量的 UPPER_SNAKE 命名同理，属于这份对齐 ABI 的接口，
 *          而不适用本工程的命名约定。
 */

#pragma once

#include <cstdint>

/**
 * @brief 入口点的链接标记
 * @details 当前恒为空：实现编进 Core 静态库，不存在跨 DLL 导出。将来若做成动态库，
 *          只需在这里把它定义成导出宏，声明处一行都不用改。
 */
#define WEPOLL_EXPORT

/**
 * @brief epoll 事件掩码
 * @details 与 Linux 的 EPOLL* 逐位对应（bit 0/1/2/3/4/6/7/8/9/10/13/31）。刻意保留不带
 *          enum class 的作用域枚举：调用方按 EPOLLIN 这类裸名使用，改成 enum class 即改接口。
 *          底层类型固定为 std::uint32_t，与 epoll_event::events 运算时不再掺入符号位。
 */
enum EPOLL_EVENTS : std::uint32_t
{
    EPOLLIN = 1U << 0,      ///< 可读：fd 上有数据可无阻塞读取
    EPOLLPRI = 1U << 1,     ///< 高优先级/带外数据可读，如 TCP 紧急数据
    EPOLLOUT = 1U << 2,     ///< 可写：fd 可无阻塞写入
    EPOLLERR = 1U << 3,     ///< 错误条件：fd 发生错误；epoll_wait 通常会报告，无需显式注册
    EPOLLHUP = 1U << 4,     ///< 挂断：对端关闭或 fd 被挂起；epoll_wait 通常会报告，无需显式注册
    EPOLLRDNORM = 1U << 6,  ///< 普通数据可读；通常与 EPOLLIN 语义相关
    EPOLLRDBAND = 1U << 7,  ///< 优先带（带外）数据可读
    EPOLLWRNORM = 1U << 8,  ///< 普通数据可写；通常与 EPOLLOUT 语义相关
    EPOLLWRBAND = 1U << 9,  ///< 优先带（带外）数据可写
    EPOLLMSG = 1U << 10,    ///< 永不上报；保留取值只为与 Linux 的位序对齐
    EPOLLRDHUP = 1U << 13,  ///< 对端关闭连接或半关闭写方向（流套接字收到 FIN）
    EPOLLONESHOT = 1U << 31 ///< 一次性触发：报过事件即停用，需重新装配才会再报
};

/**
 * @brief epoll_ctl 的操作码
 * @details 与 Linux 同值同义；取值名保持 UPPER_SNAKE，与上面的 EPOLL* 同理（调用方按裸名使用），
 *          枚举自身是我们新起的名字，故按本工程约定用 PascalCase。
 */
enum EpollCtlOperation : int
{
    EPOLL_CTL_ADD = 1, ///< 注册描述符
    EPOLL_CTL_MOD = 2, ///< 修改已注册描述符的关注事件
    EPOLL_CTL_DEL = 3  ///< 注销描述符
};

/**
 * @brief Windows 侧的句柄与套接字类型
 * @details 本头要在尚未包含 windows.h 时也能独立编译，故自带这两个别名；它们与 windows.h
 *          的定义是同一类型（`void*` 与 `UINT_PTR`，即 `std::uintptr_t`），先后包含不会冲突。
 */
using HANDLE = void *;
using SOCKET = std::uintptr_t;

/**
 * @brief epoll_event 的负载联合体
 * @details 六个成员共用同一段存储。wepoll 自身只写 ptr，其余成员为对齐 Linux 的 epoll_data
 *          而保留，方便调用方沿用习惯写法。
 */
union epoll_data
{
    void *        ptr;  ///< 用户数据（wepoll 侧挂内部状态）
    int           fd;   ///< 文件描述符
    std::uint32_t u32;  ///< 32 位值
    std::uint64_t u64;  ///< 64 位值
    SOCKET        sock; ///< 套接字（Windows 专用）
    HANDLE        hnd;  ///< 句柄（Windows 专用）
};

/// epoll_data 的别名：调用方与 Linux 侧共用同一组类型名
using epoll_data_t = epoll_data;

/**
 * @brief 就绪事件与用户数据（与 Linux 的 struct epoll_event 逐字段对应）
 */
struct epoll_event
{
    std::uint32_t events; ///< 事件掩码，EPOLLIN 等按位取或
    epoll_data_t  data;   ///< 该描述符在 epoll_ctl 时登记的用户数据
};

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief 创建 epoll 实例
 * @param size 必须为正（非正按非法参数拒绝），但不用于预分配容量：wepoll 的规模随实际注册增长
 * @return 成功返回实例句柄，失败返回空句柄并把原因写进 LastError
 */
WEPOLL_EXPORT HANDLE epoll_create(int size);

/**
 * @brief 创建 epoll 实例（带创建标志的变体）
 * @param flags 只接受 0：Windows 句柄本就不被子进程继承，Platform.h 因此把 EPOLL_CLOEXEC 垫成 0
 * @return 成功返回实例句柄，失败返回空句柄并把原因写进 LastError
 */
WEPOLL_EXPORT HANDLE epoll_create1(int flags);

/**
 * @brief 关闭 epoll 实例
 * @param ephnd 目标实例句柄
 * @return 0 成功；-1 失败（句柄未知或已关闭），原因同步写进 errno 与 LastError
 * @note 实例上登记的描述符随之全部注销；挂在等待上的调用方会被唤醒并按「实例已关闭」收尾
 */
WEPOLL_EXPORT int epoll_close(HANDLE ephnd);

/**
 * @brief 在实例上增删改一个注册项
 * @param ephnd 目标实例句柄
 * @param op EPOLL_CTL_ADD 注册、EPOLL_CTL_MOD 改关注事件、EPOLL_CTL_DEL 注销
 * @param sock 目标套接字
 * @param event ADD/MOD 时给出事件掩码与用户数据；DEL 时不读它，可传空指针
 * @return 0 成功；-1 失败（errno 与 LastError 同步设置，与 Linux 的口径一致）
 */
WEPOLL_EXPORT int epoll_ctl(HANDLE ephnd, int op, SOCKET sock, struct epoll_event *event);

/**
 * @brief 等待实例上就绪的事件
 * @param ephnd 目标实例句柄
 * @param events 输出缓冲区，就绪事件被写进这里
 * @param maxevents events 的容量，也是本次最多返回的事件数
 * @param timeout 超时毫秒数：-1 无限等待、0 立即返回、正值最多等这么久
 * @return 就绪事件条数；0 表示超时；-1 表示失败（errno 与 LastError 同步设置）
 */
WEPOLL_EXPORT int epoll_wait(HANDLE ephnd, struct epoll_event *events, int maxevents, int timeout);

#ifdef __cplusplus
}
#endif
