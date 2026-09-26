/**
 * @file wepoll.h
 * @brief Windows 上 epoll 兼容层的类型与常量定义（ABI 对齐 Linux 的 wepoll 接口）
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 *
 * @details 本头只留 epoll_data、epoll_event、EPOLL_ 系列常量与 SOCKET、HANDLE 这些与 Linux
 *          逐位对应的类型与常量，供 Iocp.h 借用。epoll_create、epoll_create1、epoll_close、
 *          epoll_ctl、epoll_wait 这五个入口点**不再声明**：vendored 实现早已从本仓库摘除
 *          （Windows 侧改走 IOCP），声明留着就是给装了这份头文件的消费者留一个「编得过、
 *          链不上」的坑。类型名、常量名与位值一律不动；EPOLL_ 的 UPPER_SNAKE 命名与全局
 *          作用域属于这份对齐 ABI 的接口，不适用本工程的命名约定。
 */
#pragma once

#include <cstdint>

/**
 * @brief epoll 事件掩码
 * @details 与 Linux 的 EPOLL* 逐位对应（bit 0/1/2/3/4/6/7/8/9/10/13/31）。刻意保留不带
 *          enum class 的作用域枚举：调用方按 EPOLLIN 这类裸名使用，改成 enum class 即改接口。
 *          底层类型固定为 std::uint32_t，与 epoll_event::events 运算时不再掺入符号位。
 */
enum EPOLL_EVENTS : std::uint32_t
{
    EPOLLIN      = 1U << 0,  ///< 可读：fd 上有数据可无阻塞读取
    EPOLLPRI     = 1U << 1,  ///< 高优先级/带外数据可读，如 TCP 紧急数据
    EPOLLOUT     = 1U << 2,  ///< 可写：fd 可无阻塞写入
    EPOLLERR     = 1U << 3,  ///< 错误条件：fd 发生错误；epoll_wait 通常会报告，无需显式注册
    EPOLLHUP     = 1U << 4,  ///< 挂断：对端关闭或 fd 被挂起；epoll_wait 通常会报告，无需显式注册
    EPOLLRDNORM  = 1U << 6,  ///< 普通数据可读；通常与 EPOLLIN 语义相关
    EPOLLRDBAND  = 1U << 7,  ///< 优先带（带外）数据可读
    EPOLLWRNORM  = 1U << 8,  ///< 普通数据可写；通常与 EPOLLOUT 语义相关
    EPOLLWRBAND  = 1U << 9,  ///< 优先带（带外）数据可写
    EPOLLMSG     = 1U << 10, ///< 永不上报；保留取值只为与 Linux 的位序对齐
    EPOLLRDHUP   = 1U << 13, ///< 对端关闭连接或半关闭写方向（流套接字收到 FIN）
    EPOLLONESHOT = 1U << 31  ///< 一次性触发：报过事件即停用，需重新装配才会再报
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
    void         *ptr;  ///< 用户数据（wepoll 侧挂内部状态）
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
