/**
 * @file EventNotifier.h
 * @brief 跨平台事件通知器，用于从其他线程唤醒事件循环
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <atomic>

namespace AsynGyanis::Platform
{
    /**
     * @brief 跨线程事件通知器
     *
     * @details Linux 基于 eventfd 实现；Windows 没有 eventfd，改用一对 loopback
     *          socket 承载同样的「写一字节即唤醒」语义。
     * @note 用法：把 readDescriptor() 注册进 epoll 监听可读，其它线程调用 notify()，
     *       事件循环被唤醒后调用 drain() 清空累积计数。
     */
    class EventNotifier
    {
    public:
        /**
         * @brief 创建通知器底层句柄
         * @details 创建失败不会抛出异常，可通过 isValid() 判断结果，
         *          以便在初始化日志尚未就绪的阶段安全降级。
         */
        EventNotifier();

        /**
         * @brief 关闭通知器持有的全部句柄
         */
        ~EventNotifier();

        EventNotifier(const EventNotifier &) = delete;

        EventNotifier &operator=(const EventNotifier &) = delete;

        EventNotifier(EventNotifier &&) = delete;

        EventNotifier &operator=(EventNotifier &&) = delete;

        /**
         * @brief 获取供事件循环监听的可读描述符
         * @return int 描述符，创建失败时为 FileDescriptor::kInvalid
         */
        [[nodiscard]] int readDescriptor() const noexcept;

        /**
         * @brief 通知器是否已成功创建
         * @return true 描述符可用
         * @return false 底层句柄创建失败
         */
        [[nodiscard]] bool isValid() const noexcept;

        /**
         * @brief 发送一次唤醒通知
         * @details 线程安全，可在任意线程调用；通知器无效时为空操作。已有「待处理」唤醒时不再写
         *          描述符：唤醒的语义只是「目标循环该醒来看一眼队列」，多写一次不会多处理任何任务，
         *          跨线程投递密集时（每个远程任务一次）这个合并把 N 次 write 压成 1 次。
         */
        void notify() const noexcept;

        /**
         * @brief 读空唤醒数据，使描述符重新回到不可读状态
         * @details 在事件循环回调中调用；通知器无效时为空操作。
         * @note 必须与 notify() 的合并标记配套：本方法**以「清除标记」作为排空完成的判据**，
         *       而不是以「描述符读空」为准——两者交叉时（清除后又有生产者置位却因合并而
         *       未写字节）只有前者能保证不丢唤醒。
         */
        void drain() const noexcept;

    private:
        /**
         * @brief 读空描述符里累积的唤醒字节
         */
        void flushDescriptor() const noexcept;

        int                       m_readDescriptor{-1};   ///< 读端描述符（注册到事件循环）
        int                       m_writeDescriptor{-1};  ///< 写端描述符（用于跨线程唤醒）
        mutable std::atomic<bool> m_wakeupPending{false}; ///< 是否已有一次待处理的唤醒（合并多次 notify 的依据）
    };
} // namespace AsynGyanis::Platform
