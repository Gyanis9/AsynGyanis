/**
 * @file TcpAcceptor.h
 * @brief TCP 监听套接字：绑定、监听与协程式异步 accept
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/InetAddress.h"

#include <deque>
#include <optional>

namespace AsynGyanis::Net
{
    /**
     * @brief 监听队列的默认长度，单位是「条已完成握手但尚未 accept 的连接」
     *
     * @details 不直接采用系统的 SOMAXCONN：公开头文件里出现 OS 宏会把平台细节泄漏给所有
     *          调用方；而且 Windows 把 SOMAXCONN 解释成「允许内核自行膨胀队列」，等于关掉
     *          背压。128 与 Linux /proc/sys/net/core/somaxconn 的历史默认值一致，
     *          足以吸收瞬时突发，又能让过载在可预期的深度上显式暴露。
     * @note 需要更深或更浅的队列时，调用方直接给 listen(backlog) 传值，本常量只是默认档位。
     */
    inline constexpr int kDefaultListenBacklog = 128;

    /**
     * @brief TCP 监听器，封装非阻塞监听套接字并提供协程式 accept。
     *
     * @details 构造时创建一个与地址协议族匹配的监听套接字，调用方按 bind() → listen() →
     *          accept() 的顺序驱动。accept() 是协程：无连接到达时挂起当前协程，
     *          由事件循环在监听描述符可读时恢复，全程不阻塞线程。
     * @note 所有平台相关调用（accept、socket 选项、错误码）都收敛到 Platform 层，
     *       本类不出现任何操作系统宏形式的错误码。
     * @see TcpServer
     */
    class TcpAcceptor
    {
    public:
        /**
         * @brief 构造监听器：创建监听套接字并预建退避定时器
         * @details 只创建资源，不绑定也不监听；调用方需显式依次调用 bind() 与 listen()。
         *          退避用的定时器在此一次性建好，避免真正发生描述符耗尽时再去做任何分配。
         * @param loop 关联的事件循环，负责 I/O 事件监控与协程唤醒
         * @param address 要监听的本地地址（IP 与端口）
         * @throws Base::SystemException 监听套接字创建失败
         * @note 本对象只占用一个描述符（监听套接字）：定时器是循环级定时器队列的句柄，不额外占描述符
         */
        TcpAcceptor(Core::EventLoop &loop, const Core::InetAddress &address);

        /**
         * @brief 默认析构，随成员生命周期自动关闭监听套接字
         *
         * @details 关闭监听套接字由 Core::AsyncSocket 的析构完成；定时器本身不持有描述符，
         *          未到期的等待随事件循环一起收尾。事件循环仍必须比本对象活得久。
         */
        ~TcpAcceptor() = default;

        // 引用成员 m_loop 一旦初始化就无法重新绑定，且 AsyncSocket 独占描述符所有权，
        // 因此拷贝与移动全部禁止：移动后的对象会留下一个指向旧循环的引用，语义无法自洽。
        TcpAcceptor(const TcpAcceptor &) = delete;
        TcpAcceptor &operator=(const TcpAcceptor &) = delete;
        TcpAcceptor(TcpAcceptor &&) = delete;
        TcpAcceptor &operator=(TcpAcceptor &&) = delete;

        /**
         * @brief 绑定监听地址并开启复用选项
         * @details 绑定前依次尝试 SO_REUSEADDR（允许绑到处于 TIME_WAIT 的地址）、
         *          SO_REUSEPORT（仅 Linux 提供，缺失时按不支持降级）与 IPV6_V6ONLY=0
         *          （仅对 IPv6 套接字设置，使单个监听端口同时接住 IPv4 映射地址）。
         *          这三项失败都不会中止流程，真正决定成败的是最终的 bind()。
         * @return true 绑定成功
         * @return false 地址不可用或已被占用，调用方可修正地址后重试
         */
        [[nodiscard]] bool bind();

        /**
         * @brief 开始接受连接请求
         * @param backlog 监听队列长度，单位是条；新值可用 kDefaultListenBacklog，
         *                实际生效值还会被系统上限（Linux 的 somaxconn）截断
         * @return true 进入监听状态
         * @return false 尚未成功 bind()，或底层 listen 调用失败
         */
 bool listen(int backlog) const;

        /**
         * @brief 异步接受一条新连接
         * @details 先取走上一轮批量 accept 暂存的连接；队列为空时直接在监听描述符上收一条。
         *          由于事件循环采用边沿触发，一次就绪必须把队列抽干，否则残留连接不会再次
         *          产生事件，因此本协程会把多余的连接存入 m_pending 供后续调用直接返回。
         *          可恢复错误全部在协程内部消化、不会抛给调用方：暂无待接受连接时挂起等待监听
         *          描述符可读；被信号中断或对端在队列中被中止时直接重试；描述符与内核缓冲耗尽时
         *          用预先建好的 m_backoffTimer 定时退避（此刻连新协程帧都可能申请不到，
         *          预先建好的定时器只是往循环级队列里插一项，不再需要任何描述符）。
         * @return Core::Task<std::optional<Core::AsyncSocket>> 成功时返回已连接的套接字；
         *         监听套接字已 close() 或描述符失效时返回 std::nullopt，表示应结束接受循环
         * @throws Base::SystemException 出现无法靠重试恢复的终止性错误（如描述符被外部关闭），
         *           异常携带平台 socket 错误码与中文上下文
         * @note 该协程必须在创建本监听器的事件循环线程上恢复，否则引用循环会串错线程
         */
        Core::Task<std::optional<Core::AsyncSocket>> accept();

        /**
         * @brief 关闭监听套接字并丢弃暂存连接
         *
         * @details 清空批量队列（其中未取走的连接由 AsyncSocket 析构关闭），并复位绑定标记，
         *          使 close() 后的对象不会再被误认为「可直接 listen」。
         */
        void close();

        /**
         * @brief 获取构造时传入的监听地址
         * @return Core::InetAddress 本地地址；端口为 0 时返回的仍是请求值，不反映内核分配的端口
         */
        [[nodiscard]] Core::InetAddress localAddress() const;

        /**
         * @brief 获取监听套接字的描述符
         * @return int 描述符编号；未创建或已关闭时为 Platform::FileDescriptor::kInvalid
         */
        [[nodiscard]] int fileDescriptor() const;

    private:
        Core::EventLoop &             m_loop;    ///< 关联的事件循环，用于挂起与唤醒 accept 协程
        Core::AsyncSocket             m_listenSocket; ///< 非阻塞监听套接字，持有描述符所有权
        Core::InetAddress             m_address; ///< 构造时请求的本地地址
        Core::Timer                   m_backoffTimer; ///< 资源紧张时的定时退避器；预先建好是为了不在错误处理路径上做任何分配（定时器只是循环级队列的句柄，不占描述符）
        std::deque<Core::AsyncSocket> m_pending; ///< 批量 accept 抽干监听队列时暂存的连接，下次 accept() 优先从这里取出
        bool                          m_bound{false}; ///< 是否已成功绑定，listen() 的前置条件
    };
} // namespace AsynGyanis::Net
