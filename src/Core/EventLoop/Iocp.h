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
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Core
{
    /**
     * @brief Windows 完成端口（IOCP）事件后端，接口与 Epoll 一致
     *
     * @note **监听套接字的连接必须由 takeAcceptedSocket() 取走**：AcceptEx 在完成时已经把连接
     *       从监听队列里摘下并接入它自己的套接字，`::%accept()` 看不到它——TcpAcceptor 走的正是
     *       这条路径（见 AsyncSocket::takeAcceptedConnection()）。
     * @note 线程约束：全部方法只在所属事件循环线程上调用（完成通知也只在 wait() 里取出），
     *       因此注销与回收不需要任何锁。
     * @note 探针以失败完成（对端复位、描述符已关闭）时，连同该方向一起附上 `EPOLLERR | EPOLLHUP`
     *       上报，让上层的 recv/send/accept 自己取回真实错误码。
     * @warning 描述符必须是 Winsock 套接字：套接字、以及 Platform 侧用回环链路对实现的
     *          唤醒与定时器描述符都满足；事件句柄或文件句柄不满足。
     *
     * @details epoll 语义到完成通知的映射（上层 IoWatcher 与 EventLoop 因此无需平台分支）：
     *          `EPOLLONESHOT` 对应「每次武装投递一个探针、完成即上报一次」——读方向是 1 字节
     *          `MSG_PEEK` 的 WSARecv（只探测可读、不消费字节，对端关闭时以 0 字节成功完成），
     *          写方向是零字节 WSASend、监听用 AcceptEx；水平触发由下一次 wait() 重新武装。
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
         * @param fileDescriptor 目标描述符（必须是套接字）
         * @param events 关注的事件位（EPOLLIN / EPOLLOUT，可带 EPOLLONESHOT）
         * @param userData 上报事件时写进 epoll_event.data.ptr 的用户数据（IoWatcher 用自己的地址）
         * @return true 已注册（同一描述符重复注册返回 false）
         * @details 注册即与完成端口建立归属关系，随后按 `events` 里给出的方向投递探针；探针投递失败
         *          **不算注册失败**——已连接套接字的对侧还没就绪、监听描述符暂时拿不到接受套接字
         *          都会走到这里，上层真正等待时会经 modFileDescriptor() 再武装一次。
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
         * @warning 注销之后**同一个还打开着的描述符不能再注册回来**：Windows 没有把句柄从完成端口
         *          解除关联的 API，第二次绑定会被拒（实测三种时序一致：探针已跑完一轮、取消完成尚未
         *          取回、先收掉取消完成再注册）。epoll 与 io_uring 没有这条限制，因此「注销后再注册
         *          同一个活描述符」不能当跨后端契约；本层的注销只发生在套接字关闭路径上。
         */
        bool delFileDescriptor(int fileDescriptor);

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
         * @return true 已投递（含「该方向已有在途探针」）；false 投递失败或描述符当前不支持该操作
         * @note 失败的原因与记账由本方法内部完成（见 noteArmFailure），调用方只看返回值
         */
        bool armProbe(SocketState &state, uint32_t direction);

        /**
         * @brief 投递 AcceptEx 探针（监听描述符的读方向）
         * @param state 监听描述符的状态
         * @return true 已投递，或已有一条接入/在途的连接；false 时本方法已自行记下待重投
         */
        bool armAcceptProbe(SocketState &state);

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
         * @brief 记下「某个方向在投递探针时就撞上了硬错误」，并把它排进待合成表
         * @details 待合成表只由真正撞上硬错误的那两个方向变更过，因此 harvestSyntheticErrorEvents()
         *          不必为找一个方向而遍历整张注册表（遍历代价随连接数线性放大，而事件循环每轮都等）
         * @param state 目标状态
         * @param direction 撞上硬错误的方向
         */
        void noteSyntheticReady(SocketState &state, uint32_t direction);

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
         * @brief 记下「这个方向此刻武装不上、成因属于预期的暂时状态」，排进重投表且不留告警
         * @param state 目标状态
         * @param direction 待重投的方向
         * @details 注册是惰性的（第一次等待才建 IoWatcher），那一刻客户端可能还在 connect 途中、
         *          监听描述符可能还没 listen()：这些状态过一会儿自己就会变好，而出向连接每条都要
         *          经过一次，告警只会把日志冲成噪声
         */
        void noteArmPending(SocketState &state, uint32_t direction);

        /**
         * @brief 记下一个方向投递失败、等下一次 wait() 再试，并在「刚转为失败」时留一条告警
         * @param state 目标状态
         * @param direction 失败的方向
         * @param reason 失败原因的中文短语（错误码由 socketError 单独承载）
         * @param socketError 当时的 WSAGetLastError()，写进告警供排查
         * @details 注册那一刻关注位可能根本武装不上。此后关注位一变（有等待者出现或退场），
         *          `modFileDescriptor()` 会顺手再投一次，因此这张表兜的是「状态已就绪但关注位没变」
         *          那一段——没有它，那一方向再没人投，等待方收不到任何完成通知。
         * @note 告警只在失败位由 0 转 1 的那一次发出：待重投表每轮 wait() 都要重投一次，每轮都记
         *       一条会在长期失败的那条描述符上刷屏——反复重试的是状态，反复告警是噪声
         */
        void noteArmFailure(SocketState &state, uint32_t direction, std::string_view reason, int socketError);

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
        std::vector<SocketState *>     m_pendingSyntheticReady; ///< 探针投递时撞上硬错误、等下一轮合成成事件的状态
    };
} // namespace AsynGyanis::Core
