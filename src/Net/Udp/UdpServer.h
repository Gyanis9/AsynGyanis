/**
 * @file UdpServer.h
 * @brief UDP 服务端：一条端口面对任意多个来源，收到的每条报文连同来源交给处理器，处理器决定答不答
 * @author Gyanis
 * @date 2026-09-28
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncUdpSocket.h"
#include "Core/Socket/InetAddress.h"
#include "Platform/IO/DatagramSocket.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief UDP 服务端：绑定一条数据报端口，按到达次序逐条交付报文
     *
     * @details 与 `QuicServer` 的分工：那里也是「一条端口、多个来源」，但它还要按报文头里的连接标识
     *          把报文路由给各条连接；本类没有连接这一层，一条报文就是全部输入，回包发往来路。
     *          适合发现广播、遥测上报、单请求单响应这类本就无连接的数据报协议。
     *
     * @warning **报文串行处理**：上一条的处理器没跑完，下一条就留在内核队列里。这是刻意的取舍——
     *          并发派发要把每条报文复制一份（每条一次堆分配），而慢处理器配上无限扇出等于把内存
     *          交给对端。处理器里放耗时工作时会堵住整个监听，耗时项请自行投递到别处再回来。
     *
     * @warning 线程契约与其它循环对象一致：本类只在**所属事件循环线程**上创建、使用与关闭。
     *          外部线程要停它，把 `stop()` 投递过去（`scheduler().postRemote()`），不要直接调。
     */
    class UdpServer
    {
    public:
        /**
         * @brief 报文处理器：收到一条报文时调用，交回要发给来源的字节
         * @param sourceAddress 报文来源地址（回包就发往这里）
         * @param payload 报文净字节；指向本服务端的收包缓冲，**只在本协程跑完之前有效**，
         *        不得留到之后使用（串行派发保证这期间不会有下一条报文覆盖它）
         * @return 要发回的字节；空表示不作答（零长报文与不作答是两件事，见 listen() 的说明）
         */
        using MessageHandler = std::function<Core::Task<std::vector<std::uint8_t>>(Core::InetAddress sourceAddress, std::span<const std::uint8_t> payload)>;

        /**
         * @brief 服务端配置
         */
        struct Configuration
        {
            /**
             * @brief 收包缓冲的容量，也就是本端一次能交付的整条报文上限
             * @details 取 UDP 净载荷上限时（默认值）除 IPv6 巨帧外不可能截断；调小就是把截断交给
             *          调用方——内核放不下整条报文时多余字节直接丢弃，而这一层分辨不出「恰好这么长」
             *          与「被截断」（`AsyncUdpSocket::asyncReceiveFrom()` 的 @note），所以宁可默认给满。
             * @note 必须在构造时给：缓冲在 `listen()` 里一次分配，跑起来之后改它就是把正在使用的
             *       缓冲换个尺寸
             */
            std::size_t    maximumDatagramByteCount{Platform::DatagramSocket::kMaximumDatagramBytes};
            MessageHandler onMessage; ///< 报文处理器；没有它这台监听器只会把每条报文丢掉，故 listen() 被驱动时拒绝
        };

        /**
         * @brief 运行计数的一份快照
         * @details 各项本体是原子量，因此本方法可以从任意线程调用；它读的是「取这一份的这一刻」的
         *          近似值，几项之间不保证是同一个瞬间的成套读数
         */
        struct Stats
        {
            std::uint64_t receivedDatagramCount{0}; ///< 交付给处理器的报文条数（读数失败的空转不计）
            std::uint64_t sentDatagramCount{0};     ///< 本端发出的报文条数（应答与主动下发都算）
            std::uint64_t unsentDatagramCount{0};   ///< 该发却没发出去的条数：超限、发送中套接字被关、平台报错
            std::uint64_t failedHandlerCount{0};    ///< 处理器抛出异常而被本类接住的条数
        };

        /**
         * @brief 建一台尚未监听的服务端
         * @param eventLoop 所属事件循环
         * @param configuration 配置（按值收：此后不再读调用方那份）
         * @throws Base::InvalidArgumentException 收包缓冲容量为 0 或超过单条报文上限：
         *         前者连一条空报文都放不下，后者收得到却答不出去
         */
        UdpServer(Core::EventLoop &eventLoop, Configuration configuration);

        /**
         * @brief 建一台**接手别人已经绑好的数据报套接字**的服务端（跨进程共享一条 UDP 端口那一步）
         * @param eventLoop 所属事件循环
         * @param configuration 配置（按值收：此后不再读调用方那份）
         * @param adoptedBoundSocket 已经 bind 过的数据报套接字，所有权随之转移；本对象析构或 stop()
         *        会关掉它——交出去的那一方此后不该再碰它
         * @throws Base::InvalidArgumentException 交来的套接字无效（`bindTo` 失败或已被移走的那份空对象）
         * @details 存在的理由：Windows 没有 `SO_REUSEPORT` 的等价物，多个 worker 各自 bind 同一端口时内核
         *          把全部报文交给最后绑上的那一个，其余进程一个错都不报却永远收不到报文。于是「一条端口、
         *          多个进程」只能由一方 bind、把套接字交给别人（`Platform::Socket::writeListeningSocketHandoff`
         *          的机制本身不限套接字类型），worker 这一侧的接手动作就是这个构造 + 不带地址的 listen()。
         * @note 套接字从哪来由调用方决定：`Platform::DatagramSocket::bindTo`（自己绑）或
         *       `Platform::DatagramSocket::adopt`（把别的进程交过来的描述符包成本层的持有者语义，
         *       它会区分「描述符无效」「不是套接字」「不是 SOCK_DGRAM」「还没 bind」四种不合格）
         * @note 与按地址构造的差别只在端口从哪来：本构造**不会去 bind**，端口从交来的套接字问，
         *       `listeningPort()` 报的是真值
         */
        UdpServer(Core::EventLoop &eventLoop, Configuration configuration, Platform::DatagramSocket adoptedBoundSocket);

        ~UdpServer();

        UdpServer(const UdpServer &) = delete;

        UdpServer &operator=(const UdpServer &) = delete;

        /**
         * @brief 绑定端口并逐条交付报文，直到 stop()
         * @param localAddress 本地地址；端口给 0 表示由内核分配，实际端口读 listeningPort()
         * @return Core::Task<> 收循环退出时完成
         * @throws Base::InvalidArgumentException 没有设置处理器、本服务端是接手构造出来的
         *         （端口已经定在交过来的套接字上，那种情形请调不带地址的 listen()）、或这台服务端
         *         已经在某个端口上监听（一台只能启动一次）
         * @throws Base::SystemException 绑定失败
         * @note 本方法是惰性协程：抛出发生在**首次恢复**（await 或投递给循环）那一刻，不在调用处
         * @note 零长报文**照样交付**（payload 为空）：无连接协议里「一条不带内容的报文」常常就是
         *       全部输入（唤醒信号、探测），把它当「没收到」等于把这类协议判死
         * @note 读数报回的平台错误（ICMP 替一个已消失的对端捎回来的那类）只跳过这一次读数，
         *       循环继续——退出就等于让一个消失的对端把整台服务变成不再接受任何来源
         * @warning 本协程的帧必须活到 `stop()` 之后（与其它循环对象同一条销毁纪律）
         */
        [[nodiscard]] Core::Task<> listen(Core::InetAddress localAddress);

        /**
         * @brief 在接手来的套接字上逐条交付报文，直到 stop()
         * @return Core::Task<> 收循环退出时完成
         * @throws Base::InvalidArgumentException 本对象不是接手构造出来的：那种情形要调
         *         listen(地址)，由本端自己 bind；或这台服务端已经在某个端口上监听（一台只能启动一次）
         * @note 本方法是惰性协程：抛出发生在**首次恢复**（await 或投递给循环）那一刻，不在调用处
         * @note 交付口径与 listen(地址) 完全一致（零长报文照交付、读数报错不带走循环），差别只在
         *       端口与套接字都不是本端建的，因此这里也不会有「绑定失败」那一类出口
         * @warning 本协程的帧必须活到 `stop()` 之后（与其它循环对象同一条销毁纪律）
         */
        [[nodiscard]] Core::Task<> listen();

        /**
         * @brief 收口：置停止标记并关掉套接字，让挂在读数上的协程醒过来退出循环
         * @details 只置标记不够：数据报的等待者是挂在「可读」上的，关掉描述符本身不会叫醒它，
         *          必须销毁那份注册对象（`AsyncUdpSocket::close()` 的顺序正是为此）。
         * @note 幂等；未监听过时只翻标记
         * @warning 必须在所属事件循环线程上调用；外部线程请投递（见类注释的线程契约）
         */
        void stop() noexcept;

        /**
         * @brief 主动发一条报文给某个地址（不等对方先来）
         * @param targetAddress 目标地址（按值收：本方法是惰性协程，收引用会把临时量留到悬空）
         * @param payload 报文净字节；空区间发的是零长报文（合法）；长度由调用方保证活到 await 结束
         * @return Core::Task<bool> true 整条已交给内核；false 本端没在监听、发送期间套接字被关，
         *         或平台报错（原因逐条落日志，计数进 Stats::unsentDatagramCount）
         * @note 与应答共用一条出口、同一套判据：单条报文超限不会截断发出，而是整条拒发并告警——
         *       数据报不会被内核切开，交出一半比不交更坏
         */
        [[nodiscard]] Core::Task<bool> sendTo(Core::InetAddress targetAddress, std::span<const std::uint8_t> payload);

        /**
         * @brief 本端实际绑定的端口
         * @return std::uint16_t 端口；尚未绑定成功时为 0
         * @note 可从别的线程读：绑定成功才写入非 0 值，读它就等于问「监听起来了吗」。
         *       stop() 之后保留最后一次的端口（端口还在、人已经收手，这是诊断要看的信息）
         */
        [[nodiscard]] std::uint16_t listeningPort() const noexcept;

        /// 运行计数快照
        [[nodiscard]] Stats stats() const noexcept;

    protected:
        /**
         * @brief 一次读数失败后，收循环该继续还是收手（纯换算，不读状态）
         * @details 单列成纯函数是为了把这条判据确定性地钉住：真正制造一个 ICMP 错误要靠对端消失
         *          的时刻与读数竞态，那条用例只会赌时序。而这一支走错退化成的正是历史上那个缺陷——
         *          一个消失的对端让整台服务不再接受任何来源。
         * @param socketErrorCode 平台错误码；0 表示「只是没数据且套接字已不可用」
         * @param isSocketValid 本端套接字是否还有效
         * @param isStopped 是否已请求停止
         * @return true 继续读下一条；false 退出收循环
         */
        [[nodiscard]] static bool continuesAfterReceiveFailure(int socketErrorCode, bool isSocketValid, bool isStopped) noexcept;

    private:
        /**
         * @brief 没给处理器就拒绝开始服务（两条 listen 入口共用同一条判据）
         * @throws Base::InvalidArgumentException 处理器为空
         */
        void requireMessageHandler() const;

        /**
         * @brief 已经在监听就拒绝再一次启动（两条 listen 入口共用同一条判据）
         * @throws Base::InvalidArgumentException 本服务端已经在某个端口上服务
         */
        void requireFreshStart() const;

        /**
         * @brief 两条 listen 的共用主体：在已就绪的套接字上逐条交付报文
         * @details 端口在这里从套接字问回来（接手来的口没有别的来源），发布之后两条路共用同一段
         *          收循环——把「端口从哪来」与「收到报文后做什么」分开，才不会出现两种形状各有一份
         *          交付逻辑、日后只改一处的分叉
         * @return Core::Task<> 收循环退出时完成
         */
        [[nodiscard]] Core::Task<> serveOnBoundSocket();

        /**
         * @brief 交付一条报文：处理器算完就按来源把应答发出去
         * @param peerAddress 来源地址（原始平台地址：回包直接用它，省一次换算）
         * @param payload 报文净字节，指向收包缓冲
         */
        [[nodiscard]] Core::Task<> serveOne(const Platform::SocketAddress peerAddress, std::span<const std::uint8_t> payload);

        /**
         * @brief 发一条报文的唯一出口（应答与主动下发都走这里）
         * @param targetAddress 目标地址
         * @param payload 报文净字节
         * @return Core::Task<bool> 见 sendTo()
         */
        [[nodiscard]] Core::Task<bool> sendDatagram(const Platform::SocketAddress targetAddress, std::span<const std::uint8_t> payload);

        Core::EventLoop                      &m_eventLoop;     ///< 所属事件循环
        Configuration                         m_configuration; ///< 配置（构造时定，此后不再改）
        std::unique_ptr<Core::AsyncUdpSocket> m_socket;        ///< 套接字的事件循环封装；未监听时为空
        /// 接手来的平台套接字：有值即「接手模式」，无参的 listen() 认它，带地址的 listen() 拒绝。
        /// 被 listen() 移交给 m_socket 之后本项转为空
        std::optional<Platform::DatagramSocket> m_adoptedSocket;

        /// 实际绑定的端口：绑定成功才写入非 0 值，因此外部线程读它就等于问「监听起来了吗」
        /// （release 与读侧 acquire 配对；它不代表允许跨线程碰本类的其它成员）
        std::atomic<std::uint16_t> m_listeningPort{0};
        std::atomic<bool>          m_isStopped{false}; ///< 是否已请求停止

        std::atomic<std::uint64_t> m_receivedDatagramCount{0}; ///< 见 Stats
        std::atomic<std::uint64_t> m_sentDatagramCount{0};     ///< 见 Stats
        std::atomic<std::uint64_t> m_unsentDatagramCount{0};   ///< 见 Stats
        std::atomic<std::uint64_t> m_failedHandlerCount{0};    ///< 见 Stats
    };
} // namespace AsynGyanis::Net
