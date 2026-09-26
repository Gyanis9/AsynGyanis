/**
 * @file AsyncUdpSocket.h
 * @brief 事件循环上的数据报套接字：整条收、整条发，收发都带对端地址
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoWatcher.h"
#include "Platform/IO/DatagramSocket.h"

#include <cstddef>
#include <memory>

namespace AsynGyanis::Core
{
    /**
     * @brief 事件循环上的数据报套接字
     * @note 报文是**整条**收发：内核不会把一条报文切开，因此发不下时等可写后整条重发，
     *       接收也要连同来源地址一起交回
     * @warning 与其它循环对象同一条线程契约：只在**所属事件循环线程**上创建、使用与关闭
     *          （内部状态没有原子保护）
     * @note 一条报文的最大长度见 Platform::DatagramSocket::kMaximumDatagramBytes
     */
    class AsyncUdpSocket
    {
    public:
        /**
         * @brief 接手一个已绑定的平台套接字
         * @param loop 所属事件循环
         * @param socket 已绑定且已置非阻塞的套接字，所有权随之转移
         */
        AsyncUdpSocket(EventLoop &loop, Platform::DatagramSocket socket);

        ~AsyncUdpSocket() = default;

        AsyncUdpSocket(AsyncUdpSocket &&other) noexcept;

        AsyncUdpSocket &operator=(AsyncUdpSocket &&other) noexcept;

        AsyncUdpSocket(const AsyncUdpSocket &) = delete;

        AsyncUdpSocket &operator=(const AsyncUdpSocket &) = delete;

        /**
         * @brief 套接字是否可用
         * @return true 描述符有效（未被移动走、未关闭）
         */
        [[nodiscard]] bool isValid() const noexcept;

        /**
         * @brief 取底层描述符
         * @return int 描述符；无效时为负数
         */
        [[nodiscard]] int fileDescriptor() const noexcept;

        /**
         * @brief 取本端绑定地址
         * @return Platform::SocketAddress 本端地址；无效套接字返回未设置的地址
         */
        [[nodiscard]] Platform::SocketAddress localAddress() const noexcept;

        /**
         * @brief 一次数据报接收的结果
         * @note 按值返回而不是写进调用方给的引用：本方法是惰性协程，调用方可能先拿到 Task、
         *       稍后才 await，那时那个实参（临时量或已离开作用域的局部对象）已经亡故
         */
        struct DatagramReceiveResult
        {
            ssize_t                 receivedByteCount{-1}; ///< 收到的字节数；负值表示没收到（原因看 socketErrorCode）
            Platform::SocketAddress peerAddress;           ///< 来源地址（失败时无意义）
            /**
             * @brief 没收到字节时的平台错误码；0 表示「只是没数据、套接字已不可用」这一类无码收场
             * @details 单靠 receivedByteCount 分不开两种「-1」：套接字被关（该收手）与对端不可达
             *          （ICMP 带回来的错误，套接字本身还好好的，该继续读）。混为一谈的代价见 @note
             */
            int socketErrorCode{0};
        };

        /**
         * @brief 接收一个数据报并带回来源地址（内部吸收「暂时没有数据」）
         * @param buffer 目标缓冲
         * @param capacity 缓冲容量，至少 1 字节（空报文也要占一位）
         * @return 字节数与来源地址（见结构体说明：按值返回）
         * @note 等待可读期间套接字被关闭时 receivedByteCount 为 -1 且 socketErrorCode 为 0；
         *       **0 是合法的空报文**
         * @note 缓冲放不下整条报文时多出的字节被丢弃（UDP 语义），返回值即 capacity
         * @note 平台报错同样按 -1 + socketErrorCode 交出，**不抛**：无连接套接字上这些码
         *       （WSAECONNRESET / EHOSTUNREACH / ECONNREFUSED …）都是 ICMP 替某个已消失的对端
         *       捎来的回声，套接字本身还能用。之前这里按硬失败抛，而抛出的异常落不进正在 await 的
         *       协程——本框架里被调度器恢复的协程抛异常只会被记一行「没人接住」然后丢弃，
         *       于是**监听循环当场消失**：一个消失的对端就让整台 QUIC 服务器不再接受任何来源
         * @throws Base::InvalidArgumentException 缓冲为空或容量为 0（调用方写错了，不必重试）
         * @throws Base::SystemException 套接字无效（已被移动走或关闭）
         */
        [[nodiscard]] Task<DatagramReceiveResult> asyncReceiveFrom(void *buffer, std::size_t capacity);

        /**
         * @brief 发一条报文，内部吸收「发送缓冲暂时放不下」
         * @param peerAddress 目标地址（按值收：本方法是惰性协程，到首次恢复才读参数，
         *        按引用接临时量会让它在那之前就已亡故——ASan 实测为 stack-use-after-scope）
         * @param buffer 待发数据；调用方必须让它活到 await 结束
         * @param length 数据长度；0 表示合法的空报文
         * @return 实际发出的字节数（与 length 相等即成功）；等待可写期间套接字被关闭时返回 -1
         * @note 数据报不会部分写出，因此等待可写后是**整条重发**
         * @throws Base::InvalidArgumentException 缓冲为空，或单条报文超过
         *         Platform::DatagramSocket::kMaximumDatagramBytes（不会被内核切开，须自行分片）
         * @throws Base::SystemException 套接字无效（已被移动走或关闭），或平台层报错
         */
        [[nodiscard]] Task<ssize_t> asyncSendTo(Platform::SocketAddress peerAddress, const void *buffer, std::size_t length);

        /**
         * @brief 关闭套接字
         * @details 顺序与 `AsyncSocket::close()` 同理：**先**销毁注册对象——它会唤醒仍挂在可读/可写上的
         *          等待协程（关描述符本身不唤醒 epoll 的等待者），**再**关描述符。少了前一步，
         *          正在等的协程就永远醒不过来，出站侧的看门狗于是形同虚设。
         * @note 幂等：已关闭或描述符已被移动走时什么都不做
         * @warning 只能在所属事件循环线程上调用（与等待同一线程的约定）
         */
        void close() noexcept;

    private:
        /**
         * @brief 首次等待时才建立 IoWatcher
         * @details 与 AsyncSocket 同一理由：注册要写进 epoll，而很多套接字一辈子不会被等待
         * @return IoWatcher* 注册对象；套接字无效时为空
         */
        [[nodiscard]] IoWatcher *ensureWatcher() const;

        /// 等可读；co_await 结果为 false 表示注册已失效（描述符已关闭），调用方应停止重试
        [[nodiscard]] IoWatcher::Awaiter waitReadable() const;

        /// 等可写；co_await 结果为 false 表示注册已失效（描述符已关闭），调用方应停止重试
        [[nodiscard]] IoWatcher::Awaiter waitWritable() const;

        EventLoop                *m_loop{nullptr}; ///< 所属事件循环（非拥有）
        Platform::DatagramSocket  m_socket;        ///< 平台套接字（拥有描述符）
        mutable std::unique_ptr<IoWatcher> m_watcher; ///< 常驻注册对象；首次等待时建立
    };
} // namespace AsynGyanis::Core
