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
     * @details 不直接采用系统的 SOMAXCONN：公开头文件里出现 OS 宏会把平台细节泄漏给所有调用方；
     *          而且 Windows 把它解释成「允许内核自行膨胀队列」，等于关掉背压。128 与 Linux
     *          /proc/sys/net/core/somaxconn 的默认值一致，足以吸收瞬时突发又让过载显式暴露。
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
         * @brief 接手一个**已经在监听中**的套接字：不新建、不 bind、不 listen
         *
         * @details 零停机重启靠这条路径：监听套接字由 supervisor 持有（Linux socket activation、Windows 服务
         *          管理器传入），本进程只负责接手并开始接受连接。接手后 bind()/listen() 直接返回成功——后置
         *          条件已成立，绝不能重新绑定，否则上一代仍在接受的连接会被丢掉。本地地址从内核取（getsockname），
         *          因此 listeningPort() 报的是真值。
         * @param loop 关联的事件循环，要求与按地址构造时相同（必须比本对象活得久）
         * @param adoptedListeningDescriptor 已经在监听状态的套接字描述符；描述符**所有权随之转移**，
         *        本对象析构或 close() 会关掉它——对旧进程来说这正是它该做的事（关闭自己那一份、
         *        不再接受新连接，等在途请求 drain 完再退出）
         * @throws Base::InvalidArgumentException 描述符无效（用法错误，不交给底层报含糊的系统错误）
         * @note 描述符会被置为非阻塞：继承来的监听套接字通常是阻塞的，不改会把事件循环卡在 accept 上
         */
        TcpAcceptor(Core::EventLoop &loop, int adoptedListeningDescriptor);

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
         * @brief 监听与接受套接字的调参项
         * @details 各项 0 表示保持系统默认、不下发对应的 setsockopt。缓冲区上限同时作用于
         *          监听套接字与每条接受到的连接；延迟接受与 TFO 只对监听套接字有意义。
         */
        struct SocketTuning
        {
            int receiveBufferBytes{0};    ///< SO_RCVBUF 上限（字节），0 = 系统默认
            int sendBufferBytes{0};       ///< SO_SNDBUF 上限（字节），0 = 系统默认
            int deferAcceptSeconds{0};    ///< TCP_DEFER_ACCEPT 等待秒数（仅 Linux），0 = 关闭
            int fastOpenQueueLength{0};   ///< TFO 队列长度（Windows/Linux），0 = 关闭
        };

        /**
         * @brief 设置套接字调参，须在 bind()/listen() 之前调用
         * @details 监听套接字的缓冲区在 listen() 时统一下发（接受到的连接可继承），每条接受到的
         *          连接另按同一取值显式设置一遍以保证跨平台一致；延迟接受在 Windows 上被 Platform
         *          层按「不支持」降级（返回 false），不影响监听本身。
         * @param tuning 调参项，见 SocketTuning
         */
        void setSocketTuning(const SocketTuning &tuning) noexcept;

        /**
         * @brief 查询当前的套接字调参
         * @return SocketTuning 当前取值
         */
        [[nodiscard]] const SocketTuning &socketTuning() const noexcept;

        /**
         * @brief 异步接受一条新连接
         * @details 先取走上一轮批量 accept 暂存的连接，队列为空时直接在监听描述符上收一条。事件循环边沿触发，
         *          一次就绪必须把队列抽干，多余的连接存入 m_pending。可恢复错误全部在协程内消化：暂无连接挂起
         *          等待、被信号中断直接重试、描述符或内核缓冲耗尽用 m_backoffTimer 退避（此时连新协程帧都可能
         *          申请不到）。
         * @return Core::Task<std::optional<Core::AsyncSocket>> 成功时返回已连接的套接字；
         *         监听套接字已 close() 或描述符失效时返回 std::nullopt，表示应结束接受循环
         * @throws Base::SystemException 出现无法靠重试恢复的终止性错误（如描述符被外部关闭），
         *           异常携带平台 socket 错误码与中文上下文
         * @note 该协程必须在创建本监听器的事件循环线程上恢复，否则引用循环会串错线程
         */
        Core::Task<std::optional<Core::AsyncSocket> > accept();

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
        /**
         * @brief 把调参项里「按连接生效」的部分应用到一条新接受的连接
         * @details 缓冲区上限与监听套接字同值；设置失败只影响性能，不丢连接
         * @param descriptor 新接受的连接描述符
         */
        void applyAcceptedSocketTuning(int descriptor) const noexcept;

        Core::EventLoop &             m_loop;         ///< 关联的事件循环，用于挂起与唤醒 accept 协程
        Core::AsyncSocket             m_listenSocket; ///< 非阻塞监听套接字，持有描述符所有权
        Core::InetAddress             m_address;      ///< 构造时请求的本地地址
        Core::Timer                   m_backoffTimer; ///< 资源紧张时的定时退避器；预先建好是为了不在错误处理路径上做任何分配（定时器只是循环级队列的句柄，不占描述符）
        std::deque<Core::AsyncSocket> m_pending;      ///< 批量 accept 抽干监听队列时暂存的连接，下次 accept() 优先从这里取出
        bool                          m_bound{false}; ///< 是否已成功绑定，listen() 的前置条件
        bool                          m_isAdopted{false}; ///< 是否由「接手已在监听的套接字」构造而来
        SocketTuning                  m_tuning{};    ///< 套接字调参，listen() 与 accept() 时下发；0 项不下发
    };
} // namespace AsynGyanis::Net
