/**
 * @file Iocp.h
 * @brief Windows 完成端口后端：与 Epoll 同接口，替代 wepoll 的用户态 AFD 轮询
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/Platform.h"

// 只借 wepoll 的 epoll_event 与 epoll 事件位定义：本后端不经过它的 AFD 轮询，
// 报给上层的仍是同一套 epoll 语义（上层的 IoWatcher / EventLoop 因此无需平台分支）
#include "wepoll.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Core
{
    /**
     * @brief Windows 完成端口（IOCP）事件后端，接口与 Epoll 一致
     *
     * @details epoll 语义到完成通知的映射（上层 IoWatcher 与 EventLoop 对此无感）：
     *          - **一次性关注（`EPOLLONESHOT`）**：每次武装投递一个探针操作，完成即上报一次，
     *            上层收到后按需再武装——这正是 epoll 里「一次上报消耗掉一次关注」的行为。
     *            连接套接字的读方向是 1 字节 `MSG_PEEK` 的 WSARecv（只探测可读、不消费字节；
     *            对端关闭时以 0 字节成功完成，与 recv 返回 0 同义），写方向是零字节 WSASend，
     *            监听套接字用 AcceptEx。
     *          - **水平触发（不带 `EPOLLONESHOT`）**：上报之后由下一次 wait() 重新武装，
     *            与 epoll_wait 每次重新取一次就绪状态等价。只有 EventLoop 的唤醒描述符用它。
     *          - 探针以失败完成（对端复位、描述符已关闭）时，连同该方向一起附上
     *            `EPOLLERR | EPOLLHUP` 上报，让上层的 recv/send/accept 自己取回真实错误码。
     *
     * @note **监听套接字的连接必须由 takeAcceptedSocket() 取走**：AcceptEx 在完成时已经把连接
     *       从监听队列里摘下并接入它自己的套接字，`::accept()` 看不到它。TcpAcceptor 走的正是
     *       这条路径（见 AsyncSocket::takeAcceptedConnection()）。
     * @note 线程约束：全部方法只在所属事件循环线程上调用（完成通知也只在 wait() 里取出），
     *       因此注销与回收不需要任何锁。
     * @warning 描述符必须是 Winsock 套接字：套接字、以及 Platform 侧用回环链路对实现的
     *          唤醒与定时器描述符都满足；事件句柄或文件句柄不满足。
     *
     * @see Epoll, IoWatcher, EventLoop
     */
    class Iocp
    {
    public:
        /**
         * @brief 创建完成端口
         * @throws Base::SystemException CreateIoCompletionPort 失败
         */
        Iocp();

        /**
         * @brief 析构：取消全部在途探针、关闭完成端口并释放套接字状态
         */
        ~Iocp();

        Iocp(Iocp &&) noexcept;

        Iocp &operator=(Iocp &&) noexcept;

        Iocp(const Iocp &) = delete;

        Iocp &operator=(const Iocp &) = delete;

        /**
         * @brief 注册描述符并武装它的事件位
         *
         * @details 注册即与完成端口建立归属关系，随后按 `events` 里给出的方向投递探针。
         *          探针投递失败**不算注册失败**：已连接套接字的对侧还没就绪、监听描述符暂时
         *          拿不到接受套接字都会走到这里，上层真正等待时会经 modFileDescriptor() 再武装一次。
         * @param fileDescriptor 目标描述符（必须是套接字）
         * @param events 关注的事件位（EPOLLIN / EPOLLOUT，可带 EPOLLONESHOT）
         * @param userData 上报事件时写进 epoll_event.data.ptr 的用户数据（IoWatcher 用自己的地址）
         * @return true 已注册（同一描述符重复注册返回 false）
         */
        bool addFileDescriptor(int fileDescriptor, uint32_t events, void *userData);

        /**
         * @brief 更新用户数据并按需补武装关注位
         * @param fileDescriptor 目标描述符
         * @param events 本次关注的事件位（EPOLLIN / EPOLLOUT，可带 EPOLLONESHOT）
         * @param userData 新的用户数据
         * @return true 该描述符已注册；false 表示它不在注册表里（EPOLL_CTL_MOD 的 ENOENT 同义）
         * @note 已在途的探针不会被重复投递；不再关注的方向的在途探针保持不动，
         *       其完成通知最多让上层多读一次（空转一次即回到等待），不会丢事件
         */
        bool modFileDescriptor(int fileDescriptor, uint32_t events, void *userData);

        /**
         * @brief 注销描述符并取消它的在途探针
         *
         * @details 取消是异步的：完成通知可能已经在队列里，因此状态不能立刻释放，
         *          要等最后一条在途探针的完成通知取出来之后再回收（见 m_graveyard）。
         *          上层的调用顺序是「先注销、后关闭描述符」，因此这里不负责关闭套接字。
         * @param fileDescriptor 目标描述符
         * @return true 已注销；false 表示它不在注册表里
         */
        bool delFileDescriptor(int fileDescriptor);

        /**
         * @brief 语义同 modFileDescriptor()：完成端口没有「重新装配」这一步，探针天然一次性
         * @param fileDescriptor 目标描述符
         * @param events 本次关注的事件位
         * @param userData 新的用户数据
         * @return true 该描述符已注册
         */
        bool rearmFileDescriptor(int fileDescriptor, uint32_t events, void *userData);

        /**
         * @brief 取出完成通知并翻译成 epoll_event 列表
         * @param timeoutMs 超时毫秒数，-1 表示无限等待
         * @return std::span<epoll_event> 本次就绪的事件（可能为空）
         * @throws Base::SystemException GetQueuedCompletionStatusEx 失败（超时与告警等待除外）
         */
        [[nodiscard]] std::span<epoll_event> wait(int timeoutMs = 0);

        /**
         * @brief 取走监听描述符上由 AcceptEx 接入的连接
         * @param listenerFileDescriptor 监听描述符
         * @param acceptedFileDescriptor 输出参数：已接入连接的描述符（成功时写入）
         * @return true 已取走一条连接（调用方随后拥有该描述符）
         * @return false 当前没有待取的连接，或该描述符不是本后端注册的监听套接字
         */
        bool takeAcceptedSocket(int listenerFileDescriptor, int *acceptedFileDescriptor);

        /**
         * @brief 取完成端口句柄
         * @return Platform::EpollHandle 完成端口句柄（HANDLE）
         */
        [[nodiscard]] Platform::EpollHandle fileDescriptor() const noexcept;

    private:
        /// 一个探针自带的完成信息：完成通知给出的是 OVERLAPPED 的地址，靠它反查所属状态与方向
        struct ProbeContext;

        /// 一个注册描述符的全部状态
        struct SocketState;

        /// 关闭完成端口之前的统一收尾：取消在途探针并释放全部状态
        void destroy();

        /**
         * @brief 为一个方向投递探针
         * @param state 目标状态
         * @param direction EPOLLIN 或 EPOLLOUT
         * @param errorText 可选输出参数：失败原因（进入调用时不会被清空，只在失败时写入）
         * @return true 已投递（含「该方向已有在途探针」）；false 投递失败或描述符当前不支持该操作
         */
        bool armProbe(SocketState &state, uint32_t direction, std::string *errorText);

        /**
         * @brief 投递 AcceptEx 探针（监听描述符的读方向）
         * @param state 监听描述符的状态
         * @param errorText 可选输出参数：失败原因
         * @return true 已投递，或已有一条接入/在途的连接
         */
        bool armAcceptProbe(SocketState &state, std::string *errorText);

        /**
         * @brief 把一条完成通知翻译成 epoll_event（必要时先完成状态的回收）
         * @param entry 完成通知
         */
        void translateCompletion(const OVERLAPPED_ENTRY &entry);

        /**
         * @brief 把「探针投递时就撞上硬错误」的方向合成成错误事件交给等待方
         * @details 这类方向没有完成通知可等：只记重投的话等待方永远收不到任何事件（epoll 此时
         *          会报 EPOLLERR|EPOLLHUP，Windows 没有等价物，只能自己造）
         */
        void harvestSyntheticErrorEvents();

        /**
         * @brief 取消一个状态上的全部在途探针
         * @param state 目标状态
         * @note 取消是异步的：完成通知仍会入队，由它的回收路径收尾
         */
        static void cancelProbes(SocketState &state) noexcept;

        /**
         * @brief 把在途探针的完成通知收完（仅用于析构：关端口与释放状态之前）
         * @details 内核在操作完成时仍会写 OVERLAPPED 里的状态码；通知不排空就
         *          释放状态等于让它写已释放内存。收齐或等待超时即返回。
         */
        void drainCompletions() noexcept;

        /**
         * @brief 关闭一个状态持有的接受套接字（在途的与已接入的）
         * @param state 目标状态
         */
        static void closeAcceptedSockets(SocketState &state) noexcept;

        /**
         * @brief 注销后的状态在最后一条完成通知取出来之后回收
         * @param state 目标状态
         * @note 在途探针尚未清空时什么都不做，等它的完成通知再进来
         */
        void releaseIfDrained(SocketState &state);

        /// 水平触发关注的重武装：上一次 wait() 上报过的描述符在下一次 wait() 前重新投递探针
        void rearmLevelTriggered();

        /**
         * @brief 记下一个方向投递失败，等下一次 wait() 再试
         * @param state 目标状态
         * @param direction 失败的方向
         * @details 注册与武装是两件事：IoWatcher 在 AsyncSocket 构造时就注册（那时 listen() 还没调用、
         *          连接也还没建立），而关注位此刻根本武装不上。上层只看到「注册成功」，
         *          于是不会再要求武装一次——失败的方向必须由后端自己记住并重试，
         *          否则那天就不会有任何完成通知到达（监听描述符因此一条连接都接不进来）。
         */
        void noteArmFailure(SocketState &state, uint32_t direction);

        /// 重试上一次投递失败的方向（在每次 wait() 阻塞之前）
        void retryFailedArms();

        /// 合并索引：按 key 在扁平表里查找，返回 m_results 下标；未命中返回 kEmptyResultSlot
        [[nodiscard]] std::size_t findResultSlot(void *userData) const noexcept;

        /// 合并索引：登记一条新结果（装载因子过半先扩容，排空路径会累积多批）
        void noteResultSlot(void *userData, std::size_t resultIndex);

        /// 合并索引：把一条结果散进当前表（不判扩容，供 noteResultSlot 与扩容重散使用）
        void insertResultSlot(void *userData, std::size_t resultIndex);

        /// 合并索引：表容量翻倍后把已有条目重新散一遍
        void growResultMergeTable();

        /// 合并索引：开始新一轮收集（只清本轮用过的槽位，槽位与容量沿用）
        void resetResultMergeTable() noexcept;

        static constexpr int kMaximumEventCount = 1024; ///< 单次 wait() 最多取出的完成通知数

        static constexpr std::uint32_t kDrainTimeoutMilliseconds = 1000; ///< 排空完成通知的单次等待上限（毫秒）

        /// 合并索引的初始槽位数：事件上限的两倍且是 2 的幂，装到一半就扩容，探测链因此不会长
        static constexpr std::size_t kInitialResultMergeSlotCount = 2048;

        /// 空槽标记（下标表里的哨兵值）。删除只发生在整表重置时，因此探测到空槽即可判定「没有」
        static constexpr std::size_t kEmptyResultSlot = static_cast<std::size_t>(-1);

        Platform::EpollHandle          m_iocp{nullptr};     ///< 完成端口句柄
        std::vector<OVERLAPPED_ENTRY>  m_entries;           ///< 单次取出的完成通知
        std::vector<epoll_event>       m_results;           ///< 翻译结果（wait() 的返回值指向它）
        /// 同一批完成通知的合并索引：data.ptr → m_results 下标，与 m_results 同生命周期。
        /// 用开放寻址的扁平表而不是 unordered_map：后者每插一个键分配一个节点，
        /// 而合并是**每条完成通知**都要走的一步（把同一注册对象两个方向的完成并入一条 epoll_event）
        std::vector<void *>            m_resultSlotUserData; ///< 槽位的键（内容仅在下标有效时有意义）
        std::vector<std::size_t>       m_resultSlotIndex;    ///< 槽位的值：m_results 下标，kEmptyResultSlot 为空槽
        /// 本轮真正用过的槽位：一轮通常只有几条完成通知，重置时按这张表逐个清即可——
        /// 整表填充是 16 KB 级别的工作量，比清几条还贵
        std::vector<std::size_t>       m_usedResultSlots;
        std::size_t                    m_resultSlotMask{0};  ///< 槽位数 - 1（槽位数恒为 2 的幂）
        std::unordered_map<int, SocketState *> m_sockets;      ///< 活动注册表：描述符 → 状态
        std::vector<SocketState *>     m_graveyard;         ///< 已注销但仍有完成通知在队的状态
        std::vector<SocketState *>     m_pendingRearm;      ///< 需要重新武装的水平触发状态
        std::vector<SocketState *>     m_pendingArmRetry;   ///< 上一次投递失败、需要重试的状态
    };
} // namespace AsynGyanis::Core
