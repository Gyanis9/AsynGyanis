/**
 * @file Uring.h
 * @brief Linux io_uring 事件后端：用一次性 POLL_ADD 复刻 epoll 的就绪通知语义
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "Platform/Platform.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

struct io_uring_sqe;
struct io_uring_cqe;
struct __kernel_timespec;

namespace AsynGyanis::Core
{
    /**
     * @brief io_uring 事件后端，成员集合与 Epoll 完全一致
     *
     * @note 仅在 Linux 上可用，且需要内核 5.6+（`IORING_OP_POLL_REMOVE` / 超时操作齐备）；
     *       由构建开关 ASYN_WITH_IO_URING 选用，Windows 与未开启时的 Linux 仍走各自后端。
     * @details 把 `IORING_OP_POLL_ADD` 当作一次性就绪通知：每次武装提交一条 poll 请求，内核在关注
     *          事件满足时投递完成通知，wait() 翻译回 epoll_event 交给上层（上层因此无平台分支）。
     *          两处适配：`EPOLLONESHOT` 与一次性天然对应，完成送达后不自动重投、由上层重新武装；
     *          水平触发（IoWatcher 的注册全是这一类）在上报后要补投，补投只按登记下来的描述符做，
     *          不遍历整张注册表——每轮全表扫描会把单事件循环的开销做成连接数的线性函数。
     * @warning 本后端只在 `ASYN_WITH_IO_URING=ON` 时参与构建，而 CI 那条作业是**只编译不运行**的：
     *          语义漂移（就绪事件迟一拍交付、空闲注册对象被反复唤醒这类）不会由默认门禁发现。
     *          要实跑得在放行 io_uring 的容器里执行同一批用例：io_uring 系统调用会被 Docker 默认
     *          seccomp 挡掉（`io_uring_setup` 返回 EPERM），需 `--security-opt seccomp=unconfined`。
     */
    class Uring
    {
    public:
        /**
         * @brief 创建 io_uring 实例（io_uring_setup + 环形映射）
         * @throws Base::SystemException 内核不支持 io_uring 或映射失败
         */
        Uring();

        /**
         * @brief 析构：解除映射并关闭 ring 描述符
         */
        ~Uring();

        Uring(Uring &&other) noexcept;

        Uring &operator=(Uring &&other) noexcept;

        Uring(const Uring &) = delete;

        Uring &operator=(const Uring &) = delete;

        /**
         * @brief 注册描述符并立即按 events 武装一次轮询
         * @param fileDescriptor 目标描述符
         * @param events         关注的事件位（可带 EPOLLONESHOT）
         * @param userData       完成通知里原样带回的用户数据（通常是 IoWatcher 地址）
         * @return true 注册成功；false 已注册过、描述符非法或提交失败
         */
        bool addFileDescriptor(int fileDescriptor, std::uint32_t events, void *userData = nullptr);

        /**
         * @brief 修改关注的事件位（在途轮询会被取消后按新掩码重投）
         * @param fileDescriptor 目标描述符
         * @param events         新的事件位
         * @param userData       新的用户数据
         * @return true 已更新（含「本来就是这个掩码」）；false 未注册或提交失败
         */
        bool modFileDescriptor(int fileDescriptor, std::uint32_t events, void *userData = nullptr);

        /**
         * @brief 注销描述符（在途轮询先取消，完成通知到齐后释放记录）
         * @param fileDescriptor 目标描述符
         * @return true 已注销；false 未注册
         */
        bool delFileDescriptor(int fileDescriptor);

        /**
         * @brief 等待就绪事件（把完成通知翻译成 epoll_event）
         * @param timeoutMs 超时毫秒数，-1 表示无限等待，0 表示立即返回
         * @return 就绪事件视图；空表示超时/无事件
         * @throws Base::SystemException 提交或等待的系统调用失败
         */
        [[nodiscard]] std::span<epoll_event> wait(int timeoutMs = 0);

        /**
         * @brief 取 ring 描述符（io_uring_setup 返回的 fd）
         */
        [[nodiscard]] Platform::EpollHandle fileDescriptor() const noexcept;

    private:
        /**
         * @brief 单个描述符的注册记录
         */
        struct Registration
        {
            int           fileDescriptor{0};
            void         *userData{nullptr};
            std::uint32_t events{0};           ///< 关注的事件位（不含 EPOLLONESHOT / EPOLLET）
            std::uint32_t inFlightEvents{0};   ///< 在途轮询提交时用的掩码
            bool          isOneShot{false};    ///< 一次上报后不自动重投，由上层重新武装
            std::uint64_t inFlightTicket{0};   ///< 在途 POLL_ADD 的票据；0 表示没有
            bool          pendingRearm{false}; ///< 取消在途轮询后要按新掩码重投
            bool          pendingDelete{false};///< 等取消完成通知到齐后销毁记录
            bool          pendingRemove{false};///< 取消请求已提交、等待其完成通知
        };

        /**
         * @brief 释放全部映射与 ring 描述符（幂等）
         */
        void destroy();

        /**
         * @brief 取一条空闲的提交项（队列满时先提交一批腾位）
         * @return io_uring_sqe* 可用提交项；nullptr 表示腾位失败
         */
        io_uring_sqe *acquireSubmission();

        /**
         * @brief 把已排队但未提交的条目交给内核
         * @return true 全部提交；false 提交失败
         */
        bool flushSubmissions();

        /**
         * @brief 把「发布 → 收单」推到不再产生新提交项为止
         * @return true 已推到安静且队列里没有悬着的提交项；false 提交失败
         * @details 收单会就地产生新提交项（取消完成后按新掩码重投），只看一次发布不够：
         *          停在队列里的提交项到不了内核，也就永远不会产出完成通知。
         */
        bool publishUntilQuiet();

        /**
         * @brief 收掉当前所有完成通知并翻译成就绪事件
         */
        void reapCompletions();

        /**
         * @brief 提交一次 POLL_ADD（一次性）
         */
        bool submitPoll(Registration &registration);

        /**
         * @brief 提交一次 POLL_REMOVE 取消指定的在途轮询
         */
        bool submitPollRemove(std::uint64_t targetTicket);

        /**
         * @brief 提交一次超时操作（只能有一个在途，旧的先撤）
         */
        bool submitTimeout(int timeoutMs);

        /**
         * @brief 提交一次超时撤销（撤不掉说明它已触发，忽略即可）
         */
        bool submitTimeoutRemove(std::uint64_t targetTicket);

        /**
         * @brief 处理一条完成通知
         * @param ticket 提交时写入的 user_data
         * @param result 完成结果（轮询掩码或负错误码）
         */
        void handleCompletion(std::uint64_t ticket, std::int32_t result);

        /**
         * @brief 维护需要动作的注册项：水平触发补投、待重投补投、待删除补撤
         * @details 只走 m_attentionDescriptors 记下的那几条，不遍历整张注册表：在途轮询没完成
         *          的描述符本来就无事可做，扫它们要把单轮开销做成 O(在册描述符数)。
         */
        void maintainRegistrations();

        /**
         * @brief 按描述符查注册记录
         */
        [[nodiscard]] Registration *findRegistration(int fileDescriptor) const;

        /**
         * @brief 销毁一条注册记录（仅当它已无在途操作）
         */
        void eraseRegistration(Registration *registration);

        /**
         * @brief 把一条在途轮询的注册记录移入「僵尸表」：描述符键立刻释放，记录留到取消完成
         * @param registration 待删记录（其 inFlightTicket 必须非 0）
         * @note 描述符键必须当场释放：内核虽然还持有记录的地址，但那个 fd 号可能马上被下一条连接
         *       复用——键留着的话新连接注册同一个 fd 号会直接失败（IoWatcher 构造随之抛异常）
         */
        void zombifyRegistration(Registration *registration);

        // ---- 环形映射（内核共享内存；跨线程可见性由 __atomic 内建保证） ----
        unsigned      *m_submissionHead{nullptr};   ///< SQ 头
        unsigned      *m_submissionTail{nullptr};   ///< SQ 尾
        unsigned      *m_submissionRingMask{nullptr}; ///< SQ 下标掩码
        unsigned      *m_submissionEntriesCount{nullptr}; ///< SQ 条目数（只读）
        unsigned      *m_submissionArray{nullptr};  ///< SQ 下标数组
        unsigned      *m_completionHead{nullptr};   ///< CQ 头
        unsigned      *m_completionTail{nullptr};   ///< CQ 尾
        unsigned      *m_completionRingMask{nullptr}; ///< CQ 下标掩码
        io_uring_sqe  *m_submissionEntries{nullptr};  ///< 提交项数组
        io_uring_cqe  *m_completionEntries{nullptr};  ///< 完成项数组
        void          *m_submissionRingMapping{nullptr}; ///< SQ 环那段映射的基址（munmap 与算偏移都用它）
        std::size_t    m_submissionRingMappingSize{0};   ///< 上面那段映射的长度
        void          *m_completionRingMapping{nullptr}; ///< CQ 环映射基址；内核把两段并成一份时与 SQ 相同
        std::size_t    m_completionRingMappingSize{0};   ///< 上面那段映射的长度
        void          *m_submissionEntriesMapping{nullptr};   ///< SQ 条目数组的映射基址（内核侧那块）
        std::size_t    m_submissionEntriesMappingSize{0};     ///< 上面那段映射的长度
        unsigned       m_submissionCapacity{0};      ///< SQ 条目数（本地副本）
        /// 已取走、尚未发布给内核的槽位数：取槽只推进它，尾指针等 flush 时才发布——
        /// 内核因此不会读到半写的 SQE（「先填内容、再发布尾指针」这条顺序由它保证）
        unsigned       m_reservedSubmissionCount{0};

        int  m_ringFileDescriptor{-1};  ///< ring 描述符
        bool m_isValid{false};          ///< 是否已完成初始化

        /// 超时操作的时值：内核持有它的地址直到完成，因此必须是成员、不能是局部
        __kernel_timespec *m_timeoutValue{nullptr};

        std::map<int, std::unique_ptr<Registration>> m_registrations; ///< 按描述符的注册表
        /// 僵尸登记项（键是在途票据）：描述符键已释放、但内核还持有记录地址的那些。取消完成通知
        /// 到达时在这里释放；在那之前同一个 fd 号必须能被新连接重新注册
        std::map<std::uint64_t, std::unique_ptr<Registration>> m_zombiePolls;
        std::map<std::uint64_t, Registration *>      m_inFlightPolls; ///< 票据到在途轮询的映射
        /// 需要维护动作的描述符（按号存，不存指针：记录可能在这之前就被销毁）
        /// 投递一次电平事件、或某次提交没成功时登记，维护只走这几条。
        /// 残留的号被新连接复用也无害：那条登记最多让新注册提前补投一次它本来就要补的轮询，
        /// 而不会把动作错派给别的对象——维护读的是注册记录此刻的状态，不是登记时的状态
        std::vector<int>                             m_attentionDescriptors;
        std::uint64_t                                m_nextTicket{1};  ///< 票据分配器（单调递增）
        std::uint64_t                                m_timeoutTicket{0}; ///< 在途超时操作的票据

        std::vector<epoll_event> m_readyEvents; ///< wait() 返回的事件缓冲
    };
} // namespace AsynGyanis::Core
